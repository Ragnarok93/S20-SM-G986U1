/* Host test for src/kernel_page/address_cache.c — the two files that decide
 * whether a recovered address may be reused.  No device.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel_page/address_cache.h"

static int failures;

static void check(const char *name, int got, int want) {
  if (got == want) {
    printf("ok   %s\n", name);
    return;
  }
  failures++;
  printf("FAIL %s: got %d, want %d\n", name, got, want);
}

static FILE *as_file(const char *text) {
  FILE *f = tmpfile();
  if (!f) {
    perror("tmpfile");
    exit(2);
  }
  fputs(text, f);
  rewind(f);
  return f;
}

int main(void) {
  const uintptr_t lo = (uintptr_t)0xffffff8000000000ULL;
  const uintptr_t hi = (uintptr_t)0xffffffffffffffffULL;
  const uintptr_t page_offset = (uintptr_t)0xffffff8000000000ULL;
  const uint64_t ram = 8ULL << 30;
  const uintptr_t good = (uintptr_t)0xffffff8012345000ULL;
  uintptr_t a = 0;
  FILE *f;
  char buf[64];
  char ans[32];
  int n;

  /* --- saved anchor --------------------------------------------------- */
  f = as_file("boot_id=boot-a\nanchor_mm=0xffffff8012345000\n");
  check("cache: matching boot accepted",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OK, 1);
  fclose(f);
  check("cache: anchor value", a == good, 1);

  f = as_file("boot_id=boot-b\nanchor_mm=0xffffff8012345000\n");
  check("cache: other boot rejected",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_BOOT_MISMATCH, 1);
  fclose(f);

  f = as_file("boot_id=boot-a\n");
  check("cache: missing anchor rejected",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_NO_ANCHOR, 1);
  fclose(f);

  f = as_file("boot_id=boot-a\nanchor_mm=0x0000000000000000\n");
  check("cache: zero anchor rejected",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OUT_OF_RANGE, 1);
  fclose(f);

  f = as_file("boot_id=boot-a\nanchor_mm=0x0000000000001000\n");
  check("cache: anchor outside the direct map rejected",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OUT_OF_RANGE, 1);
  fclose(f);

  f = as_file("boot_id=boot-a\nanchor_mm=ffffff8012345000\n");
  check("cache: bare hex accepted, as %llx did",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OK, 1);
  fclose(f);

  f = as_file("boot_id=boot-a\r\nanchor_mm=0xffffff8012345000\r\n");
  check("cache: CRLF accepted",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OK, 1);
  fclose(f);

  f = as_file("boot_id=boot-a\nanchor_mm=0xffffff8012345000\n"
              "anchor_mm=0xffffff8023456000\n");
  a = 0;
  check("cache: last anchor wins",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OK, 1);
  fclose(f);
  check("cache: last anchor value", a == (uintptr_t)0xffffff8023456000ULL, 1);

  /* --- writer --------------------------------------------------------- */
  n = kp_cache_format(buf, sizeof(buf), "boot-a", good, lo, hi);
  check("format: returns a length", n > 0, 1);
  check("format: exact text",
        n > 0 && strcmp(buf, "boot_id=boot-a\nanchor_mm=0xffffff8012345000\n")
                     == 0,
        1);
  f = as_file(buf);
  a = 0;
  check("format: round-trips through the parser",
        kp_cache_parse(f, "boot-a", lo, hi, &a) == KP_CACHE_OK && a == good, 1);
  fclose(f);
  check("format: refuses an out-of-range anchor",
        kp_cache_format(buf, sizeof(buf), "boot-a", 0, lo, hi), 0);
  check("format: refuses a short buffer",
        kp_cache_format(buf, 8, "boot-a", good, lo, hi), 0);

  /* --- answer file ---------------------------------------------------- */
  n = snprintf(ans, sizeof(ans), "mm=0x%016zx\n", (size_t)good);
  check("answer: LF form accepted",
        kp_answer_parse(ans, (size_t)n, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_OK,
        1);
  check("answer: parsed value", a == good, 1);

  /* Latent defect, preserved for behaviour parity: the hex loop runs to
   * n-1, so on the 23-byte CRLF form it consumes '\r' as a hex digit and the
   * answer is rejected.  The CRLF branch has therefore never accepted a file;
   * changing that would change which addresses a run may reuse. */
  n = snprintf(ans, sizeof(ans), "mm=0x%016zx\r\n", (size_t)good);
  check("answer: CRLF form rejected as hex (latent defect, unchanged)",
        kp_answer_parse(ans, (size_t)n, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_HEX,
        1);

  n = snprintf(ans, sizeof(ans), "mm=0x%016zx\n", (size_t)good);
  check("answer: truncated rejected",
        kp_answer_parse(ans, (size_t)n - 1, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_LEN,
        1);
  check("answer: extra byte rejected",
        kp_answer_parse(ans, (size_t)n + 1, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_FORMAT,
        1);

  memcpy(ans, "MM=0x", 5);
  n = snprintf(ans + 5, sizeof(ans) - 5, "%016zx\n", (size_t)good);
  check("answer: wrong prefix rejected",
        kp_answer_parse(ans, (size_t)n + 5, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_FORMAT,
        1);

  n = snprintf(ans, sizeof(ans), "mm=0x%016zx\n", (size_t)good);
  ans[5] = 'A';
  check("answer: uppercase hex rejected",
        kp_answer_parse(ans, (size_t)n, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_HEX,
        1);

  n = snprintf(ans, sizeof(ans), "mm=0x%016zx\n", (size_t)(good + 0x8));
  check("answer: misaligned rejected",
        kp_answer_parse(ans, (size_t)n, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_IMPLAUSIBLE,
        1);

  n = snprintf(ans, sizeof(ans), "mm=0x%016zx\n", (size_t)0xffffff8012345040ULL +
                                                       (ram + 0x1000));
  check("answer: beyond RAM rejected",
        kp_answer_parse(ans, (size_t)n, lo, hi, page_offset, ram, &a) ==
            KP_ANSWER_IMPLAUSIBLE,
        1);

  /* --- radius --------------------------------------------------------- */
  unsetenv("KS_CACHE_RADIUS_GB");
  check("radius: default 8 GiB", (int)(kp_cache_radius_bytes() >> 30), 8);
  setenv("KS_CACHE_RADIUS_GB", "256", 1);
  check("radius: clamped to 128 GiB", (int)(kp_cache_radius_bytes() >> 30), 128);
  setenv("KS_CACHE_RADIUS_GB", "0", 1);
  check("radius: zero falls back to 8 GiB",
        (int)(kp_cache_radius_bytes() >> 30), 8);
  unsetenv("KS_CACHE_RADIUS_GB");

  if (failures) {
    printf("%d case(s) failed\n", failures);
    return 1;
  }
  printf("all cache cases passed\n");
  return 0;
}
