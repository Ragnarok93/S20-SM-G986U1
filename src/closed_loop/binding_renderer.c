#include "binding_renderer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_reason(char *reason, size_t size, const char *text) {
  if (reason && size) {
    snprintf(reason, size, "%s", text);
  }
}

static int lower_hex(uintptr_t value, char *out, size_t out_size) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    out[i] = digits[(value >> ((15 - i) * 4)) & 0xf];
  }
  out[16] = '\0';
  return out_size >= 17 ? 0 : -1;
}

static int canonical_uuid(const char *uuid) {
  static const unsigned char hyphens[] = {8, 13, 18, 23};
  if (!uuid || strlen(uuid) != 36) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(hyphens); ++i) {
    if (uuid[hyphens[i]] != '-') {
      return 0;
    }
  }
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      continue;
    }
    if (!((uuid[i] >= '0' && uuid[i] <= '9') ||
          (uuid[i] >= 'a' && uuid[i] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

int binding_render_mm_answer_buffer(uintptr_t mm, char *buf, size_t cap,
                                    size_t *len) {
  char hex[17];
  int n;
  if (!buf || !len) {
    return -1;
  }
  if (lower_hex(mm, hex, sizeof(hex)) != 0) {
    return -1;
  }
  n = snprintf(buf, cap, "mm=0x%s\n", hex);
  if (n < 0 || (size_t)n >= cap || (size_t)n != 22) {
    return -1;
  }
  *len = (size_t)n;
  return 0;
}

int binding_render_companion_buffer(const char *boot_id, char *buf,
                                    size_t cap, size_t *len) {
  int n;
  if (!canonical_uuid(boot_id) || !buf || !len) {
    return -1;
  }
  n = snprintf(buf, cap, "boot_id=%s\n", boot_id);
  if (n < 0 || (size_t)n >= cap || (size_t)n != 45) {
    return -1;
  }
  *len = (size_t)n;
  return 0;
}

int binding_parse_companion_buffer(const void *data, size_t len,
                                   const char *current_boot, uint64_t btime,
                                   int64_t mm_mtime, int64_t companion_mtime,
                                   char *reason, size_t reason_size) {
  const unsigned char *bytes = (const unsigned char *)data;
  int format_ok;

  if (reason && reason_size) {
    snprintf(reason, reason_size, "invalid");
  }
  if (!data || !current_boot || strlen(current_boot) != 36) {
    set_reason(reason, reason_size, "input");
    return -1;
  }
  if (len != 45) {
    set_reason(reason, reason_size, "len");
    return -1;
  }
  format_ok = canonical_uuid(current_boot) &&
              memcmp(bytes, "boot_id=", 8) == 0 &&
              memcmp(bytes + 8, current_boot, 36) == 0 && bytes[44] == '\n';
  if (!format_ok) {
    set_reason(reason, reason_size, !format_ok ? "boot_scope_format" :
                "boot_scope_canonical");
    return -1;
  }
  if (btime == 0 || mm_mtime < 0 || companion_mtime < 0 ||
      (uint64_t)mm_mtime < btime || (uint64_t)companion_mtime < btime) {
    set_reason(reason, reason_size, "mtime");
    return -1;
  }
  set_reason(reason, reason_size, "ok");
  return 0;
}

int binding_render_run22_gate_buffer(const char *boot_id, uintptr_t mm,
                                     int t2c_pass, int c2t_pass,
                                     int c2c_pass, char *buf, size_t cap,
                                     size_t *len) {
  char hex[17];
  int n;
  if (!canonical_uuid(boot_id) || !buf || !len) {
    return -1;
  }
  if (lower_hex(mm, hex, sizeof(hex)) != 0) {
    return -1;
  }
  n = snprintf(buf, cap,
               "boot_id=%s\n"
               "mm=0x%s\n"
               "t2c=%s\n"
               "c2t=%s\n"
               "c2c=%s\n",
               boot_id, hex, t2c_pass ? "PASS" : "FAIL",
               c2t_pass ? "PASS" : "FAIL",
               c2c_pass ? "PASS" : "FAIL");
  if (n < 0 || (size_t)n >= cap || (size_t)n != 94) {
    return -1;
  }
  *len = (size_t)n;
  return 0;
}

static int parse_gate_segment(const unsigned char *bytes, size_t offset,
                              const char *expected, const char *segment,
                              char *reason, size_t reason_size) {
  if (memcmp(bytes + offset, expected, strlen(expected)) != 0) {
    set_reason(reason, reason_size, segment);
    return 0;
  }
  return 1;
}

int binding_parse_run22_gate_buffer(const void *data, size_t len,
                                    const char *current_boot, uintptr_t mm,
                                    uint64_t btime, int64_t mm_mtime,
                                    int64_t gate_mtime, char *reason,
                                    size_t reason_size) {
  const unsigned char *bytes = (const unsigned char *)data;
  char mm_hex[24];
  int boot_ok;
  int mm_ok;

  if (reason && reason_size) {
    snprintf(reason, reason_size, "invalid");
  }
  if (!data || !current_boot || strlen(current_boot) != 36) {
    set_reason(reason, reason_size, "input");
    return -1;
  }
  if (len != 94) {
    set_reason(reason, reason_size, "len");
    return -1;
  }
  snprintf(mm_hex, sizeof(mm_hex), "0x%016llx",
           (unsigned long long)mm);
  boot_ok = canonical_uuid(current_boot) &&
            memcmp(bytes, "boot_id=", 8) == 0 &&
            memcmp(bytes + 8, current_boot, 36) == 0 && bytes[44] == '\n' &&
            bytes[44] == '\n';
  mm_ok = memcmp(bytes + 45, "mm=0x", 5) == 0 &&
          memcmp(bytes + 50, mm_hex + 2, 16) == 0 && bytes[66] == '\n';
  if (!boot_ok || !mm_ok) {
    set_reason(reason, reason_size, "format");
    return -1;
  }
  if (!parse_gate_segment(bytes, 67, "t2c=PASS\n", "t2c", reason,
                          reason_size) ||
      !parse_gate_segment(bytes, 76, "c2t=PASS\n", "c2t", reason,
                          reason_size) ||
      !parse_gate_segment(bytes, 85, "c2c=PASS\n", "c2c", reason,
                          reason_size)) {
    return -1;
  }
  if (btime == 0 || mm_mtime < 0 || gate_mtime < 0 ||
      (uint64_t)mm_mtime < btime || (uint64_t)gate_mtime < btime) {
    set_reason(reason, reason_size, "mtime");
    return -1;
  }
  set_reason(reason, reason_size, "ok");
  return 0;
}

#ifndef CLOSED_LOOP_HOST_TEST
static int fsync_dir_of(const char *path) {
  char dir[512];
  char *slash;
  int fd;
  if (!path || strlen(path) >= sizeof(dir)) {
    return -1;
  }
  snprintf(dir, sizeof(dir), "%s", path);
  slash = strrchr(dir, '/');
  if (!slash || slash == dir) {
    return -1;
  }
  *slash = '\0';
  fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (fsync(fd) != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

int binding_write_atomic(const char *path, const void *data, size_t len) {
  char tmp_path[768];
  int fd;
  if (!path || !data || strlen(path) + 4 >= sizeof(tmp_path)) {
    return -1;
  }
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
  fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return -1;
  }
  if (write(fd, data, len) != (ssize_t)len || fsync(fd) != 0) {
    int saved_errno = errno;
    close(fd);
    unlink(tmp_path);
    errno = saved_errno;
    return -1;
  }
  if (close(fd) != 0 || rename(tmp_path, path) != 0 ||
      fsync_dir_of(path) != 0) {
    unlink(tmp_path);
    return -1;
  }
  return 0;
}

int binding_render_mm_answer(uintptr_t mm, const char *path) {
  char buf[64];
  size_t len = 0;
  if (binding_render_mm_answer_buffer(mm, buf, sizeof(buf), &len) != 0) {
    return -1;
  }
  return binding_write_atomic(path, buf, len);
}

int binding_render_companion(const char *boot_id, const char *path) {
  char buf[64];
  size_t len = 0;
  if (binding_render_companion_buffer(boot_id, buf, sizeof(buf), &len) != 0) {
    return -1;
  }
  return binding_write_atomic(path, buf, len);
}

int binding_render_run22_gate(const char *boot_id, uintptr_t mm,
                              int t2c_pass, int c2t_pass, int c2c_pass,
                              const char *path) {
  char buf[128];
  size_t len = 0;
  if (binding_render_run22_gate_buffer(boot_id, mm, t2c_pass, c2t_pass,
                                       c2c_pass, buf, sizeof(buf),
                                       &len) != 0) {
    return -1;
  }
  return binding_write_atomic(path, buf, len);
}
#endif /* !CLOSED_LOOP_HOST_TEST */
