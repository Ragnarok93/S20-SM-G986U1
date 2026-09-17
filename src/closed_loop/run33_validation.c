#define _GNU_SOURCE
#include "bucket_answer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reuse the proven pile, timing, and resource infrastructure from v10.5.
 * This translation unit intentionally does not call run_bucket_collect()
 * and never touches the main-chain code paths. */
#include "bucket_collect_v10_5.c"

#define RUN33_PAGE_COUNT 8192UL
#define RUN33_MMAP_BYTES (RUN33_PAGE_COUNT * KS_PAGE_SIZE)
#define RUN33_POS_OFFSET_A 8
#define RUN33_POS_OFFSET_B 0x400
#define RUN33_VOTING_ROUNDS 3

static uint32_t run33_rol32(uint32_t x, uint32_t n)
{
    return (x << (n & 31U)) | (x >> ((32U - (n & 31U)) & 31U));
}

static void run33_mix(uint32_t *a, uint32_t *b, uint32_t *c)
{
    *a -= *c; *a ^= run33_rol32(*c, 4);  *c += *b;
    *b -= *a; *b ^= run33_rol32(*a, 6);  *a += *c;
    *c -= *b; *c ^= run33_rol32(*b, 8);  *b += *a;
    *a -= *c; *a ^= run33_rol32(*c, 16); *c += *b;
    *b -= *a; *b ^= run33_rol32(*a, 19); *a += *c;
    *c -= *b; *c ^= run33_rol32(*b, 4);  *b += *a;
}

static void run33_final(uint32_t *a, uint32_t *b, uint32_t *c)
{
    *c ^= *b; *c -= run33_rol32(*b, 14);
    *a ^= *c; *a -= run33_rol32(*c, 11);
    *b ^= *a; *b -= run33_rol32(*a, 25);
    *c ^= *b; *c -= run33_rol32(*b, 16);
    *a ^= *c; *a -= run33_rol32(*c, 4);
    *b ^= *a; *b -= run33_rol32(*a, 14);
    *c ^= *b; *c -= run33_rol32(*b, 24);
}

static uint32_t run33_jhash2(const uint32_t *k, uint32_t length,
                             uint32_t initval)
{
    uint32_t a = 0xdeadbeefU + (length << 2) + initval;
    uint32_t b = a;
    uint32_t c = a;

    while (length > 3) {
        a += k[0];
        b += k[1];
        c += k[2];
        run33_mix(&a, &b, &c);
        length -= 3;
        k += 3;
    }
    switch (length) {
    case 3:
        c += k[2]; /* fallthrough */
    case 2:
        b += k[1]; /* fallthrough */
    case 1:
        a += k[0];
        run33_final(&a, &b, &c);
        break;
    case 0:
        break;
    }
    return c;
}

static uint32_t run33_bucket(uintptr_t mm, uintptr_t base, size_t page_index,
                             size_t offset)
{
    uint64_t addr = (uint64_t)base + page_index * KS_PAGE_SIZE + offset;
    uint64_t page = addr & KS_PAGE_MASK;
    uint32_t words[4] = {
        (uint32_t)mm,
        (uint32_t)(mm >> 32),
        (uint32_t)page,
        (uint32_t)(page >> 32),
    };

    return run33_jhash2(words, 4, (uint32_t)(addr & 0xfffU)) &
           (uint32_t)(RUN32_HASHSIZE - 1U);
}

static int run33_load_average_below_2(void)
{
    FILE *f = fopen("/proc/loadavg", "re");
    char line[128];
    double one = 0.0;
    int ok = 0;

    if (!f) {
        return 0;
    }
    if (fgets(line, sizeof(line), f) && sscanf(line, "%lf", &one) == 1 &&
        one < 2.0) {
        ok = 1;
    }
    fclose(f);
    return ok;
}

/* Private-pile counterpart to the v10.5 shared-oracle round. */
static int run33_round(unsigned int *pile_addr, unsigned int *probe_addr,
                       size_t idle_time, size_t threshold, int *out_reason,
                       size_t *out_before, size_t *out_after,
                       size_t *out_confirm)
{
    size_t before = 0;
    size_t after = 0;
    size_t confirm = 0;
    int verdict = 0;

    *out_reason = ORACLE_OK;
    *out_before = 0;
    *out_after = 0;
    *out_confirm = 0;
    if (!wait_bucket_idle((size_t)probe_addr, idle_time, threshold)) {
        *out_reason = ORACLE_BUSY;
        return 0;
    }
    before = measure((size_t)probe_addr);
    *out_before = before;
    if (!before) {
        *out_reason = ORACLE_NO_BASELINE;
        return 0;
    }
    if (pile_create(pile_addr, PILE_WAITERS, 0) != 0) {
        *out_reason = ORACLE_PILE_CREATE;
        return 0;
    }
    usleep(SELFTEST_SETTLE_USEC);
    {
        size_t begin = rdtsc_begin();

        (void)futex(pile_addr, FUTEX_WAKE_PRIVATE, 1);
        confirm = rdtsc_end() - begin;
    }
    *out_confirm = confirm;
    if (confirm <= idle_time * 3) {
        *out_reason = ORACLE_CONFIRM_WEAK;
        pile_wake_full();
        (void)wait_bucket_idle((size_t)probe_addr, idle_time, threshold);
        (void)pile_drain();
        return 0;
    }
    after = measure((size_t)probe_addr);
    *out_after = after;
    if (after > threshold && after > before * 3) {
        verdict = 1;
    } else if (after <= threshold && after <= before * 3) {
        verdict = 2;
    } else {
        *out_reason = ORACLE_INCONCLUSIVE;
    }
    pile_wake_full();
    (void)wait_bucket_idle((size_t)probe_addr, idle_time, threshold);
    if (pile_drain() != 0) {
        *out_reason = ORACLE_DRAIN;
        return 0;
    }
    return verdict;
}

static int run33_vote(const char *stage, unsigned int *pile_addr,
                      unsigned int *probe_addr, int want_slow,
                      size_t idle_time, size_t threshold, int *out_reason)
{
    for (size_t round = 0; round < RUN33_VOTING_ROUNDS; ++round) {
        int reason = ORACLE_OK;
        size_t before = 0;
        size_t after = 0;
        size_t confirm = 0;
        int verdict = run33_round(pile_addr, probe_addr, idle_time,
                                  threshold, &reason, &before, &after,
                                  &confirm);

        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=%s round=%zu want_slow=%d "
                "verdict=%d before=%zu after=%zu confirm=%zu reason=%s\n",
                stage, round, want_slow, verdict, before, after, confirm,
                oracle_reason_name(reason));
        if (verdict != (want_slow ? 1 : 2)) {
            *out_reason = want_slow ? ORACLE_POSITIVE_MISS :
                                      ORACLE_NEGATIVE_HIT;
            return 0;
        }
    }
    return 1;
}

static int run33_measure_baseline(unsigned char *arena, size_t *out_idle,
                                  size_t *out_threshold)
{
    size_t idle_time = SIZE_MAX;

    for (size_t page = 0; page < RUN33_PAGE_COUNT; ++page) {
        static const size_t offsets[] = {
            RUN33_POS_OFFSET_A, RUN33_POS_OFFSET_B, 0x800
        };

        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
            size_t value = measure((size_t)(arena + page * KS_PAGE_SIZE +
                                            offsets[i]));

            if (value != 0 && (idle_time == SIZE_MAX || value < idle_time)) {
                idle_time = value;
            }
        }
    }
    if (idle_time == SIZE_MAX) {
        return 0;
    }
    *out_idle = idle_time;
    *out_threshold = idle_time * 5;
    return 1;
}

int run33_validation(void)
{
    uintptr_t mm = 0;
    char reason[128] = "invalid";
    char boot_id[64];
    unsigned long hashsize = 0;
    const char *hashsize_source = "invalid";
    unsigned char *arena = MAP_FAILED;
    uintptr_t base = 0;
    size_t idle_time = 0;
    size_t threshold = 0;
    int oracle_reason = ORACLE_OK;
    int fail = 0;
    int rc = 2;

    if (!bucket_answer_load(&mm, reason, sizeof(reason))) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=answer-load verdict=FAIL "
                "reason=%s\n", reason);
        return 2;
    }
    fprintf(stderr,
            "RUN33_MM_VALIDATION gate=answer-load verdict=PASS "
            "mm=0x%016zx\n", mm);
    if (!read_boot_id(boot_id, sizeof(boot_id))) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=boot-id verdict=FAIL "
                "reason=read\n");
        return 2;
    }
    if (!resolve_hashsize(&hashsize, &hashsize_source) ||
        hashsize != RUN32_HASHSIZE) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=hashsize verdict=FAIL "
                "expected=0x800 actual=0x%lx source=%s\n",
                hashsize, hashsize_source);
        return 2;
    }
    if (!run33_load_average_below_2()) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=loadavg verdict=FAIL "
                "reason=load>=2\n");
        return 2;
    }
    if (!pin_main_cpu0()) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=affinity verdict=FAIL "
                "reason=sched-setaffinity\n");
        return 2;
    }
    arena = mmap(NULL, RUN33_MMAP_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=alloc verdict=FAIL reason=mmap "
                "errno=%d\n", errno);
        return 2;
    }
    base = (uintptr_t)arena;
    memset(arena, 0, RUN33_MMAP_BYTES);
    if (!run33_measure_baseline(arena, &idle_time, &threshold)) {
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=baseline verdict=FAIL "
                "reason=no-baseline\n");
        fail = 1;
        goto out;
    }
    fprintf(stderr,
            "RUN33_MM_VALIDATION gate=baseline verdict=PASS idle=%zu "
            "threshold=%zu\n", idle_time, threshold);
    {
        static uint16_t first_page_for_bucket[RUN32_HASHSIZE];

        memset(first_page_for_bucket, 0xff, sizeof(first_page_for_bucket));
        size_t positive_page_a = RUN33_PAGE_COUNT;
        size_t positive_page_b = RUN33_PAGE_COUNT;
        size_t negative_page_b = RUN33_PAGE_COUNT;
        uint32_t positive_bucket = 0;
        uint32_t negative_a_bucket = run33_bucket(mm, base, 1,
                                                  RUN33_POS_OFFSET_A);
        uint32_t negative_b_bucket = 0;

        for (size_t page = 0; page < RUN33_PAGE_COUNT; ++page) {
            uint32_t bucket = run33_bucket(mm, base, page,
                                           RUN33_POS_OFFSET_A);

            if (first_page_for_bucket[bucket] != 0xffff) {
                positive_page_a = first_page_for_bucket[bucket];
                positive_page_b = page;
                positive_bucket = bucket;
                break;
            }
            first_page_for_bucket[bucket] = (uint16_t)page;
        }
        if (positive_page_b == RUN33_PAGE_COUNT) {
            fprintf(stderr,
                    "RUN33_MM_VALIDATION gate=partition verdict=FAIL "
                    "reason=no-positive-collision pages=%zu\n",
                    RUN33_PAGE_COUNT);
            fail = 1;
            goto out;
        }
        for (size_t page = 2; page < RUN33_PAGE_COUNT; ++page) {
            uint32_t bucket = run33_bucket(mm, base, page,
                                           RUN33_POS_OFFSET_A);

            if (bucket != negative_a_bucket) {
                negative_page_b = page;
                negative_b_bucket = bucket;
                break;
            }
        }
        if (negative_page_b == RUN33_PAGE_COUNT) {
            fprintf(stderr,
                    "RUN33_MM_VALIDATION gate=partition verdict=FAIL "
                    "reason=no-negative-pair pages=%zu\n",
                    RUN33_PAGE_COUNT);
            fail = 1;
            goto out;
        }
        unsigned int *positive_pile =
            (unsigned int *)(void *)(arena + positive_page_a *
                                     KS_PAGE_SIZE + RUN33_POS_OFFSET_A);
        unsigned int *positive_probe =
            (unsigned int *)(void *)(arena + positive_page_b *
                                     KS_PAGE_SIZE + RUN33_POS_OFFSET_A);
        unsigned int *negative_pile =
            (unsigned int *)(void *)(arena + KS_PAGE_SIZE +
                                     RUN33_POS_OFFSET_A);
        unsigned int *negative_probe =
            (unsigned int *)(void *)(arena + negative_page_b *
                                     KS_PAGE_SIZE +
                                     RUN33_POS_OFFSET_A);
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=partition verdict=PASS "
                "stage=RUN33_POSITIVE bucket=0x%03x pages=%zu-%zu\n",
                positive_bucket, positive_page_a, positive_page_b);
        fprintf(stderr,
                "RUN33_MM_VALIDATION gate=partition verdict=PASS "
                "stage=RUN33_NEGATIVE bucket_a=0x%03x bucket_b=0x%03x "
                "pages=1-%zu\n", negative_a_bucket, negative_b_bucket,
                negative_page_b);
        if (!run33_vote("RUN33_POSITIVE", positive_pile, positive_probe, 1,
                        idle_time, threshold, &oracle_reason) ||
            !run33_vote("RUN33_POSITIVE", positive_probe, positive_pile, 1,
                        idle_time, threshold, &oracle_reason) ||
            !run33_vote("RUN33_NEGATIVE", negative_pile, negative_probe, 0,
                        idle_time, threshold, &oracle_reason) ||
            !run33_vote("RUN33_NEGATIVE", negative_probe, negative_pile, 0,
                        idle_time, threshold, &oracle_reason)) {
            fail = 1;
        }
    }
out:
    if (arena != MAP_FAILED) {
        munmap(arena, RUN33_MMAP_BYTES);
    }
    rc = fail ? 1 : 0;
    fprintf(stderr, "RUN33_MM_VALIDATION VERDICT=%s\n",
            fail ? "FAIL" : "PASS");
    return rc;
}

int main(void)
{
    const char *gate = getenv("KS_RUN33_VALIDATION");

    if (!gate || strcmp(gate, "1") != 0) {
        return 3;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "RUN33 start pid=%d\n", (int)getpid());
    return run33_validation();
}
