#include "../src/closed_loop/binding_renderer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CURRENT_BOOT "11111111-2222-3333-4444-555555555555"
#define OLD_BOOT "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
#define TEST_MM 0xffffffc51bc66cc0ULL

static int g_checked;
static int g_failed;

static void check(int id, int pass, const char *reason) {
  ++g_checked;
  if (!pass) {
    ++g_failed;
    printf("FAIL C%02d reason=%s\n", id, reason);
  } else {
    printf("PASS C%02d\n", id);
  }
}

static void render_companion(char *buf, size_t cap, size_t *len) {
  if (binding_render_companion_buffer(CURRENT_BOOT, buf, cap, len) != 0) {
    memset(buf, 0, cap);
    *len = 0;
  }
}

static int parse_companion(const void *data, size_t len, uint64_t btime,
                           int64_t mm_mtime, int64_t companion_mtime,
                           char *reason) {
  return binding_parse_companion_buffer(data, len, CURRENT_BOOT, btime,
                                        mm_mtime, companion_mtime, reason,
                                        128) == 0;
}

static int parse_gate(const void *data, size_t len, uintptr_t mm,
                      uint64_t btime, int64_t mm_mtime, int64_t gate_mtime,
                      char *reason) {
  return binding_parse_run22_gate_buffer(data, len, CURRENT_BOOT, mm, btime,
                                         mm_mtime, gate_mtime, reason,
                                         128) == 0;
}

static void test_companion(void) {
  char buf[96];
  size_t len = 0;
  char reason[128];
  int64_t fresh = 1000;
  uint64_t btime = 1000;

  render_companion(buf, sizeof(buf), &len);
  check(1, len == 45 && parse_companion(buf, len, btime, fresh, fresh,
                                        reason), reason);
  check(2, !parse_companion(buf, 44, btime, fresh, fresh, reason), reason);
  check(3, !parse_companion(buf, 46, btime, fresh, fresh, reason), reason);
  buf[0] = 'b'; buf[1] = 'o'; buf[2] = 'o'; buf[3] = 't'; buf[4] = 'i';
  buf[5] = 'd'; buf[6] = '='; buf[7] = '\0';
  check(4, !parse_companion(buf, 45, btime, fresh, fresh, reason), reason);

  render_companion(buf, sizeof(buf), &len);
  memcpy(buf + 8, OLD_BOOT, 36);
  check(5, !parse_companion(buf, 45, btime, fresh, fresh, reason), reason);
  buf[8] = 'A';
  check(6, !parse_companion(buf, 45, btime, fresh, fresh, reason), reason);
  buf[44] = '\r';
  buf[45] = '\n';
  check(7, !parse_companion(buf, 46, btime, fresh, fresh, reason), reason);
  check(8, !parse_companion(buf, 44, btime, fresh, fresh, reason), reason);
  render_companion(buf, sizeof(buf), &len);
  check(9, parse_companion(buf, 45, btime, btime, btime, reason), reason);
  check(10, !parse_companion(buf, 45, btime, btime - 1, btime, reason),
        reason);
  render_companion(buf, sizeof(buf), &len);
  memcpy(buf + 8, OLD_BOOT, 36);
  check(11, !parse_companion(buf, 45, btime, btime, btime, reason), reason);
}

static void render_gate(char *buf, size_t cap, size_t *len) {
  if (binding_render_run22_gate_buffer(CURRENT_BOOT, TEST_MM, 1, 1, 1,
                                       buf, cap, len) != 0) {
    memset(buf, 0, cap);
    *len = 0;
  }
}

static void test_gate(void) {
  char buf[160];
  size_t len = 0;
  char reason[128];
  int64_t fresh = 1000;
  uint64_t btime = 1000;

  render_gate(buf, sizeof(buf), &len);
  check(1, len == 94 && parse_gate(buf, len, TEST_MM, btime, fresh, fresh,
                                   reason), reason);
  check(2, !parse_gate(buf, 93, TEST_MM, btime, fresh, fresh, reason),
        reason);
  check(3, !parse_gate(buf, 95, TEST_MM, btime, fresh, fresh, reason),
        reason);
  memcpy(buf + 8, OLD_BOOT, 36);
  check(4, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);

  render_gate(buf, sizeof(buf), &len);
  buf[50] ^= 1;
  check(5, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);
  render_gate(buf, sizeof(buf), &len);
  buf[50] = 'A';
  check(6, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);
  render_gate(buf, sizeof(buf), &len);
  memcpy(buf + 67, "t2c=FAIL\n", 9);
  check(7, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);
  render_gate(buf, sizeof(buf), &len);
  memcpy(buf + 76, "c2t=FAIL\n", 9);
  check(8, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);
  render_gate(buf, sizeof(buf), &len);
  memcpy(buf + 85, "c2c=FAIL\n", 9);
  check(9, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);
  check(10, !parse_gate(buf, 85, TEST_MM, btime, fresh, fresh, reason),
        reason);
  check(11, !parse_gate(buf, 96, TEST_MM, btime, fresh, fresh, reason),
        reason);

  {
    char reordered[94];
    memcpy(reordered, "mm=0xffffffc51bc66cc0\n", 22);
    memcpy(reordered + 22, buf, 45);
    memcpy(reordered + 67, "t2c=PASS\n", 9);
    memcpy(reordered + 76, "c2t=PASS\n", 9);
    memcpy(reordered + 85, "c2c=PASS\n", 9);
    check(12, !parse_gate(reordered, 94, TEST_MM, btime, fresh, fresh,
                          reason), reason);
  }
  check(13, !parse_gate(buf, 94, 0xffffffd5b5c69380ULL, btime, fresh, fresh,
                        reason), reason);
  check(14, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh - 1, reason),
        reason);
  check(15, !parse_gate(buf, 94, TEST_MM, btime, fresh - 1, fresh, reason),
        reason);
  memcpy(buf + 8, OLD_BOOT, 36);
  check(16, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);

  render_gate(buf, sizeof(buf), &len);
  memcpy(buf + 67, "t2c=pass\n", 9);
  check(17, !parse_gate(buf, 94, TEST_MM, btime, fresh, fresh, reason),
        reason);
  {
    char spliced[94];
    memcpy(spliced, buf, 67);
    spliced[67] = '\n';
    memcpy(spliced + 68, "t2c=PASS\n", 9);
    memcpy(spliced + 77, "c2t=PASS\n", 9);
    memcpy(spliced + 86, "c2c=PASS", 8);
    check(18, !parse_gate(spliced, 94, TEST_MM, btime, fresh, fresh,
                          reason), reason);
  }
}

int main(void) {
  test_companion();
  test_gate();
  printf("TOTAL=%d PASS=%d FAIL=%d\n", g_checked,
         g_checked - g_failed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
