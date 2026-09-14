#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t addr; uint32_t before; uint32_t after; } row_t;

static uint32_t rol32(uint32_t x, unsigned n) {
  return (x << n) | (x >> (32 - n));
}

static void jhash_mix(uint32_t *a, uint32_t *b, uint32_t *c) {
  *a -= *c; *a ^= rol32(*c, 4); *c += *b;
  *b -= *a; *b ^= rol32(*a, 6); *a += *c;
  *c -= *b; *c ^= rol32(*a, 8); *b += *a;
  *a -= *c; *a ^= rol32(*c, 16); *c += *b;
  *b -= *a; *b ^= rol32(*a, 19); *a += *c;
  *c -= *b; *c ^= rol32(*b, 4); *b += *a;
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

static uint32_t jhash_n4(const uint32_t *k, uint32_t initval) {
  uint32_t a = 0xdeadbeef + (4u << 2) + initval;
  uint32_t b = a, c = a;
  a += k[0]; b += k[1]; c += k[2];
  jhash_mix(&a, &b, &c);
  a += k[3];
  jhash_final(&a, &b, &c);
  return c;
}

static uint32_t futex_hash(uint64_t addr, uint64_t mm, uint32_t mask) {
  uint32_t w[4];
  uint32_t page = (uint32_t)(addr & ~0xfffULL);
  w[0] = page;
  w[1] = (uint32_t)((addr & ~0xfffULL) >> 32);
  w[2] = (uint32_t)mm;
  w[3] = (uint32_t)(mm >> 32);
  return jhash_n4(w, (uint32_t)(addr & 0xfffULL)) & mask;
}

static int cmp_after_desc(const void *pa, const void *pb) {
  const row_t *a = pa, *b = pb;
  if (a->after != b->after) return a->after > b->after ? -1 : 1;
  if (a->before != b->before) return a->before < b->before ? -1 : 1;
  if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: %s timings.txt start end step top_n [mask] [threshold]\n", argv[0]);
    return 2;
  }
  const char *path = argv[1];
  uint64_t start = strtoull(argv[2], NULL, 0);
  uint64_t end = strtoull(argv[3], NULL, 0);
  uint64_t step = strtoull(argv[4], NULL, 0);
  size_t top_n = strtoul(argv[5], NULL, 0);
  uint32_t mask = argc > 6 ? strtoul(argv[6], NULL, 0) : 2047;
  uint32_t threshold = argc > 7 ? strtoul(argv[7], NULL, 0) : 0;
  if (!step || end <= start || top_n < 2 || top_n > 4096) return 2;

  FILE *f = fopen(path, "rb");
  if (!f) { perror("open timings"); return 1; }
  row_t *rows = malloc(sizeof(row_t) * 1 << 18);
  size_t count = 0, cap = 1 << 18;
  if (!rows) return 1;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] < '0' || line[0] > '9') continue;
    char *p = strchr(line, ' ');
    if (!p) continue;
    row_t row;
    row.addr = strtoull(p + 1, NULL, 16);
    p = strchr(p + 1, ' ');
    if (!p) continue;
    row.before = strtoul(p + 1, NULL, 10);
    p = strchr(p + 1, ' ');
    if (!p) continue;
    row.after = strtoul(p + 1, NULL, 10);
    if (threshold && row.after <= threshold) continue;
    if (count == cap) {
      cap *= 2;
      row_t *tmp = realloc(rows, sizeof(row_t) * cap);
      if (!tmp) return 1;
      rows = tmp;
    }
    rows[count++] = row;
  }
  fclose(f);
  qsort(rows, count, sizeof(*rows), cmp_after_desc);
  if (count < top_n) top_n = count;
  fprintf(stderr, "rows=%zu top=%zu mask=%u range=%016llx-%016llx step=%llx\n",
          count, top_n, mask, start, end, step);

  uint64_t best_mm = 0;
  size_t best_agree = 0;
  uint32_t best_bucket = 0;
  uint64_t checked = 0;
#pragma omp parallel for schedule(static) reduction(max:best_agree)
  for (int64_t mi = 0; mi < (int64_t)((end - start) / step); ++mi) {
    uint64_t mm = start + (uint64_t)mi * step;
    uint32_t counts[2048] = {0};
    for (size_t i = 0; i < top_n; ++i) counts[futex_hash(rows[i].addr, mm, mask)]++;
    for (uint32_t b = 0; b < 2048; ++b) {
      if (counts[b] > best_agree) {
        best_agree = counts[b];
        best_mm = mm;
        best_bucket = b;
      }
    }
#pragma omp atomic
    checked++;
  }
  printf("MAX=MM=0x%016llx BUCKET=%u AGREE=%zu/%zu CHECKED=%llu\n",
         best_mm, best_bucket, best_agree, top_n, checked);
  if (best_agree) {
    for (size_t i = 0; i < top_n; ++i)
      if (futex_hash(rows[i].addr, best_mm, mask) == best_bucket)
        printf("HIT addr=0x%016llx before=%u after=%u\n", rows[i].addr, rows[i].before, rows[i].after);
  }
  return 0;
}
