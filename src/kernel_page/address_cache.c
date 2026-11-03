/* address_cache.c — see address_cache.h.  Behaviour is copied from the call
 * sites it came from: same line buffer, same accept/reject rules, same order
 * of checks.  Nothing here prints; the caller keeps its messages.
 */
#include "address_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- path selection ---------------------------------------------------- */

static char kp_cache_path_buf[512];

static int path_usable(const char *path, int write) {
  char parent[512];
  char *slash;
  size_t n;
  if (!path || !*path || strlen(path) >= sizeof(parent)) return 0;
  if (access(path, F_OK) == 0)
    return access(path, write ? W_OK : R_OK) == 0;
  n = strlen(path);
  memcpy(parent, path, n + 1);
  slash = strrchr(parent, '/');
  if (!slash) return 0;
  if (slash == parent) parent[1] = 0;
  else *slash = 0;
  return access(parent, write ? W_OK : R_OK) == 0;
}

static const char *try_dir(const char *dir, int write) {
  char candidate[512];
  if (!dir || !*dir) return NULL;
  snprintf(candidate, sizeof(candidate), "%s/ks_cache.txt", dir);
  if (!path_usable(candidate, write)) return NULL;
  snprintf(kp_cache_path_buf, sizeof(kp_cache_path_buf), "%s", candidate);
  return kp_cache_path_buf;
}

const char *kp_cache_select(int write) {
  const char *env;
  const char *p;

  env = getenv("KS_CACHE_PATH");
  if (env && *env && path_usable(env, write)) {
    snprintf(kp_cache_path_buf, sizeof(kp_cache_path_buf), "%s", env);
    return kp_cache_path_buf;
  }
  p = try_dir(getenv("GHOSTLOCK_HOME"), write);
  if (p) return p;
  p = try_dir(getenv("TMPDIR"), write);
  if (p) return p;
  p = try_dir(getenv("HOME"), write);
  if (p) return p;
  if (path_usable("/sdcard/Download/ghostlock_ks_cache.txt", write)) {
    snprintf(kp_cache_path_buf, sizeof(kp_cache_path_buf), "%s",
             "/sdcard/Download/ghostlock_ks_cache.txt");
    return kp_cache_path_buf;
  }
  if (path_usable("/data/local/tmp/ks_cache.txt", write)) {
    snprintf(kp_cache_path_buf, sizeof(kp_cache_path_buf), "%s",
             "/data/local/tmp/ks_cache.txt");
    return kp_cache_path_buf;
  }
  return NULL;
}

size_t kp_cache_radius_bytes(void) {
  const char *env = getenv("KS_CACHE_RADIUS_GB");
  unsigned long long gb = env ? strtoull(env, NULL, 0) : 8;
  if (!gb) gb = 8;
  if (gb > 128) gb = 128;
  return (size_t)gb << 30;
}

int kp_boot_id_read(char *buf, size_t len) {
  FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
  if (!f) return 0;
  if (!fgets(buf, len, f)) {
    fclose(f);
    return 0;
  }
  fclose(f);
  buf[strcspn(buf, "\r\n")] = 0;
  return strlen(buf) >= 8;
}

/* --- saved anchor ------------------------------------------------------ */

enum kp_cache_status kp_cache_parse(FILE *f, const char *boot_id,
                                    uintptr_t lo, uintptr_t hi,
                                    uintptr_t *anchor) {
  char line[128];
  char file_boot[128] = "";
  unsigned long long value = 0;
  int have_anchor = 0;

  if (!f || !boot_id || !anchor) return KP_CACHE_NO_ANCHOR;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strncmp(line, "boot_id=", 8)) {
      snprintf(file_boot, sizeof(file_boot), "%s", line + 8);
      continue;
    }
    if (sscanf(line, "anchor_mm=0x%llx", &value) == 1 ||
        sscanf(line, "anchor_mm=%llx", &value) == 1) {
      have_anchor = 1;
    }
  }
  if (!have_anchor) return KP_CACHE_NO_ANCHOR;
  if ((uintptr_t)value < lo || (uintptr_t)value >= hi)
    return KP_CACHE_OUT_OF_RANGE;
  if (strcmp(file_boot, boot_id) != 0) return KP_CACHE_BOOT_MISMATCH;
  *anchor = (uintptr_t)value;
  return KP_CACHE_OK;
}

int kp_cache_format(char *buf, size_t len, const char *boot_id,
                    uintptr_t anchor, uintptr_t lo, uintptr_t hi) {
  int n;
  if (!buf || !boot_id) return 0;
  if (anchor < lo || anchor >= hi) return 0;
  n = snprintf(buf, len, "boot_id=%s\nanchor_mm=0x%016zx\n", boot_id, anchor);
  if (n <= 0 || (size_t)n >= len) return 0;
  return n;
}

/* --- one-shot answer file ---------------------------------------------- */

static int answer_plausible(uintptr_t mm, uintptr_t lo, uintptr_t hi,
                            uintptr_t page_offset, uint64_t ram_bytes) {
  if (mm < lo || mm >= hi) return 0;
  /* A leaked mm whose linear offset exceeds physical RAM cannot be real. */
  if ((uint64_t)(mm - page_offset) >= ram_bytes) return 0;
  return 1;
}

enum kp_answer_status kp_answer_parse(const char *buf, size_t n, uintptr_t lo,
                                      uintptr_t hi, uintptr_t page_offset,
                                      uint64_t ram_bytes, uintptr_t *out) {
  size_t i;
  uintptr_t mm = 0;
  int eol_ok;

  if (!buf || !out) return KP_ANSWER_LEN;
  if (n != 22 && n != 23) return KP_ANSWER_LEN;
  eol_ok = n == 22 ? buf[21] == '\n'
                   : (buf[21] == '\r' && buf[22] == '\n');
  if (memcmp(buf, "mm=0x", 5) != 0 || !eol_ok) return KP_ANSWER_FORMAT;
  for (i = 5; i < n - 1; ++i) {
    unsigned char c = (unsigned char)buf[i];
    unsigned v;
    if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a' + 10);
    else return KP_ANSWER_HEX;
    mm = (mm << 4) | v;
  }
  /* Hand the parsed value back before judging it: the caller's rejection
   * message prints the number it parsed, as it always did. */
  *out = mm;
  if ((mm & 0x3f) != 0) return KP_ANSWER_IMPLAUSIBLE;
  if (!answer_plausible(mm, lo, hi, page_offset, ram_bytes))
    return KP_ANSWER_IMPLAUSIBLE;
  return KP_ANSWER_OK;
}
