/* Host test for src/kernel_page/identity.c.
 *
 * The geometry used here is the one this device reported:
 *   mm_struct 808 992 1024 32 8 : ... : slabdata 31 31 0
 * so objsize 1024, objperslab 32, pagesperslab 8, span 32768.  The address is
 * the leaked mm of a delivered cycle, ffffffffc0a3af8400.
 *
 * The point of the wrong-span case is that the resolution can come out false:
 * a span that is not the measured one puts the object on a different page and
 * a different slot, so a passing first case is evidence about the arithmetic,
 * not a tautology.  The last cases pin the rule that matters for the table:
 * a page is only derived when every measured geometry field agrees, and an
 * incomplete table never reports a page grouping or a co-residency verdict.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kernel_page/identity.h"

int g_exlog_fd = -1;
char g_exlog_path[256] = "";

/* identity.c logs through the payload's log function; on the host it just
 * prints so a failure can be read. */
void exlog_writef(const char *level, const char *fmt, ...) {
  va_list ap;
  (void)level;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

static int failures;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);            \
      failures++;                                                      \
    }                                                                  \
  } while (0)

static struct kp_geometry measured(void) {
  struct kp_geometry g;
  g.objsize = 1024;
  g.objs_per_slab = 32;
  g.pages_per_slab = 8;
  g.span = 8 * 4096;
  g.assumed_objs_per_slab = 32;
  g.assumed_span = 8 * 4096;
  g.measured = 1;
  g.span_agrees = 1;
  g.assumes_agree = 1;
  return g;
}

static void fill_table(const struct kp_geometry *g) {
  struct kp_identity id;
  kp_identity_window(32);
  for (size_t i = 0; i < 32; i++) {
    kp_identity_freeze(i, 'S');
  }
  (void)kp_identity_anchor(g, 0xffffffc0a3af8400ULL, 1, &id);
  for (size_t i = 0; i < 32; i++) {
    const char *state = "yes";
    int seen = 1;
    if (i == 2) {
      state = "zombie";
      seen = 0;
    } else if (i == 3) {
      state = "gone";
      seen = 0;
    } else if (i == 31) {
      state = "unavailable";
    }
    kp_identity_slot(i, state, seen, 1, (long)i == 1 ? -1 : 0, 0, 0, &id);
  }
}

int main(void) {
  struct kp_geometry g = measured();
  struct kp_identity id;

  /* Measured address of the leaked mm from a delivered cycle. */
  CHECK(kp_identity_anchor(&g, 0xffffffc0a3af8400ULL, 1, &id) == 1);
  CHECK(id.page == 0xffffffc0a3af8000ULL);
  CHECK(id.offset == 0x400);
  CHECK(id.slot == 1);
  CHECK(id.slot_match == 1);
  CHECK(id.page_exact == 1);
  CHECK(id.span == 8 * 4096);
  CHECK(id.page_reason == NULL);
  /* The mask is only exact for a span-aligned page: the derived base is a
   * multiple of the measured span, the offset stays inside it, and the
   * derived slot stays inside the object count. */
  CHECK((id.page & (uintptr_t)(8 * 4096 - 1)) == 0);
  CHECK(id.offset < (size_t)(8 * 4096));
  CHECK(id.slot >= 0 && id.slot < 32);

  /* The object one stride lower is slot 0 and does not match slot 1. */
  CHECK(kp_identity_anchor(&g, 0xffffffc0a3af8000ULL, 1, &id) == 0);
  CHECK(id.slot == 0);
  CHECK(id.page == 0xffffffc0a3af8000ULL);

  /* A span the kernel did not report moves the page and the slot: the first
   * case is falsifiable rather than arithmetic that always agrees. */
  {
    struct kp_geometry wrong = measured();
    wrong.span = 16 * 4096;
    CHECK(kp_identity_anchor(&wrong, 0xffffffc0a3af8400ULL, 1, &id) == 0);
    CHECK(id.page == 0xffffffc0a3af0000ULL);
    CHECK(id.offset == 0x8400);
    CHECK(id.slot == 33);
  }

  /* No geometry: no page, and the reason names what is missing. */
  {
    struct kp_geometry none = measured();
    none.measured = 0;
    none.span = -1;
    CHECK(kp_identity_anchor(&none, 0xffffffc0a3af8400ULL, 1, &id) == 0);
    CHECK(id.page == 0);
    CHECK(id.page_exact == 0);
    CHECK(id.page_reason != NULL &&
          strcmp(id.page_reason, "no-slab-geometry") == 0);
  }

  /* A geometry whose own fields disagree produces no page at all. */
  {
    struct kp_geometry odd = measured();
    odd.span_agrees = 0;
    CHECK(kp_identity_anchor(&odd, 0xffffffc0a3af8400ULL, 1, &id) == 0);
    CHECK(id.page == 0);
    CHECK(id.page_exact == 0);
    CHECK(id.page_reason != NULL &&
          strcmp(id.page_reason, "geometry-self-inconsistent") == 0);
  }

  /* A span that is not a power of two cannot be masked at all, even when its
   * own fields agree; the table must refuse a page rather than mask with
   * 12287. */
  {
    struct kp_geometry np = measured();
    np.objsize = 1024;
    np.objs_per_slab = 12;
    np.pages_per_slab = 3;
    np.span = 3 * 4096;
    np.assumed_objs_per_slab = 12;
    np.assumed_span = 3 * 4096;
    np.span_agrees = 1;
    np.assumes_agree = 1;
    CHECK(kp_identity_anchor(&np, 0xffffffc0a3af8400ULL, 1, &id) == 0);
    CHECK(id.page == 0);
    CHECK(id.page_exact == 0);
    CHECK(id.page_reason != NULL &&
          strcmp(id.page_reason, "non-power-of-two-span") == 0);
  }

  /* Measured geometry that disagrees with the burst's constants is a failed
   * check too: the page would be arithmetically valid but about a page the
   * construction did not build. */
  {
    struct kp_geometry other = measured();
    other.assumes_agree = 0;
    CHECK(kp_identity_anchor(&other, 0xffffffc0a3af8400ULL, 1, &id) == 0);
    CHECK(id.page == 0);
    CHECK(id.page_exact == 0);
    CHECK(id.page_reason != NULL &&
          strcmp(id.page_reason, "construction-geometry-mismatch") == 0);
  }

  /* The table: all 32 rows, one address.  Incomplete tables must report the
   * partial view and no grouping verdict. */
  {
    struct kp_identity_stats st;
    fill_table(&g);
    kp_identity_stats(&st);
    CHECK(st.slots == 32);
    CHECK(st.rows == 32);
    CHECK(st.addressed == 1);
    CHECK(st.page_known == 1);
    CHECK(st.unresolved == 31);
    CHECK(st.invalid_geometry == 0);
    CHECK(st.duplicate_addresses == 0);
    CHECK(st.pages_observed == 1);
    CHECK(st.complete == 0);
    CHECK(st.distinct_pages == 0);
    CHECK(st.co_residency == KP_CO_UNAVAILABLE);
    CHECK(st.co_reason != NULL &&
          strcmp(st.co_reason, "addressed-too-few") == 0);
    kp_identity_summary();
  }

  /* An interrupted scan: rows stop early, and no grouping number may appear.
   * `unresolved` is slots minus known pages, so it stays 31 even though only
   * seven rows were recorded; the missing rows are visible as rows<slots. */
  {
    struct kp_identity_stats st;
    kp_identity_window(32);
    for (size_t i = 0; i < 3; i++) {
      kp_identity_freeze(i, 'S');
    }
    (void)kp_identity_anchor(&g, 0xffffffc0a3af8400ULL, 1, &id);
    for (size_t i = 0; i < 7; i++) {
      kp_identity_slot(i, "yes", 1, 1, 0, 0, 0, &id);
    }
    kp_identity_stats(&st);
    CHECK(st.rows == 7);
    CHECK(st.slots == 32);
    CHECK(st.page_known == 1);
    CHECK(st.unresolved == 31);
    CHECK(st.complete == 0);
    CHECK(st.distinct_pages == 0);
    CHECK(st.objects_per_page == 0);
    CHECK(st.co_residency == KP_CO_UNAVAILABLE);
    CHECK(st.co_reason != NULL && strcmp(st.co_reason, "rows-missing") == 0);
    kp_identity_summary();
  }

  /* A row whose address exists but whose geometry failed is counted as
   * invalid, not as a known page. */
  {
    struct kp_geometry odd = measured();
    struct kp_identity_stats st;
    odd.span_agrees = 0;
    kp_identity_window(32);
    for (size_t i = 0; i < 32; i++) {
      kp_identity_freeze(i, 'S');
    }
    (void)kp_identity_anchor(&odd, 0xffffffc0a3af8400ULL, 1, &id);
    for (size_t i = 0; i < 32; i++) {
      kp_identity_slot(i, "yes", 1, 1, 0, 0, 0, &id);
    }
    kp_identity_stats(&st);
    CHECK(st.rows == 32);
    CHECK(st.addressed == 1);
    CHECK(st.page_known == 0);
    CHECK(st.invalid_geometry == 1);
    CHECK(st.pages_observed == 0);
    CHECK(st.complete == 0);
    CHECK(st.co_residency == KP_CO_UNAVAILABLE);
    kp_identity_summary();
  }

  /* Variadic rows without a freeze sample are allowed and must not crash or
   * invent a state. */
  {
    kp_identity_window(32);
    kp_identity_slot(0, NULL, -1, 0, 0, 0, 0, NULL);
    kp_identity_slot(31, "unavailable", 1, 0, 0, 0, 0, NULL);
    kp_identity_summary();
  }

  /* The Cycle B route: one measured address per slot.  With all 32 on one
   * page the table is complete and may state co-residency; the grouping
   * branch is exercised here because a single-address run can never reach
   * it. */
  {
    struct kp_identity_stats st;
    kp_identity_window(32);
    for (size_t i = 0; i < 32; i++) {
      CHECK(kp_identity_object(&g, i, 0xffffffc0a3af8000ULL + i * 1024) == 1);
      kp_identity_freeze(i, 'S');
    }
    for (size_t i = 0; i < 32; i++) {
      kp_identity_slot(i, "yes", 1, 1, 0, 0, 0, NULL);
    }
    kp_identity_stats(&st);
    CHECK(st.rows == 32);
    CHECK(st.addressed == 32);
    CHECK(st.page_known == 32);
    CHECK(st.complete == 1);
    CHECK(st.distinct_pages == 1);
    CHECK(st.objects_per_page == 32);
    CHECK(st.co_residency == KP_CO_RESIDENT);
    CHECK(st.duplicate_addresses == 0);
    CHECK(st.same_page_pairs == 32u * 31u / 2u);
    kp_identity_summary();
  }

  /* Two pages split the table: the same complete grouping path must say so. */
  {
    struct kp_identity_stats st;
    kp_identity_window(32);
    for (size_t i = 0; i < 32; i++) {
      uintptr_t addr = 0xffffffc0a3af8000ULL +
                       (i < 16 ? 0 : 8 * 4096) + (i % 16) * 1024;
      CHECK(kp_identity_object(&g, i, addr) == 1);
      kp_identity_freeze(i, 'S');
    }
    for (size_t i = 0; i < 32; i++) {
      kp_identity_slot(i, "yes", 1, 1, 0, 0, 0, NULL);
    }
    kp_identity_stats(&st);
    CHECK(st.complete == 1);
    CHECK(st.distinct_pages == 2);
    CHECK(st.objects_per_page == 16);
    CHECK(st.co_residency == KP_CO_SPLIT);
    CHECK(st.same_page_pairs == 2u * (16u * 15u / 2u));
    kp_identity_summary();
  }

  /* The same address reported for two objects is counted, not silently
   * accepted as two pages of one. */
  {
    struct kp_identity_stats st;
    kp_identity_window(32);
    for (size_t i = 0; i < 32; i++) {
      uintptr_t addr = 0xffffffc0a3af8000ULL + (i == 1 ? 0 : i) * 1024;
      CHECK(kp_identity_object(&g, i, addr) == 1);
      kp_identity_freeze(i, 'S');
    }
    for (size_t i = 0; i < 32; i++) {
      kp_identity_slot(i, "yes", 1, 1, 0, 0, 0, NULL);
    }
    kp_identity_stats(&st);
    CHECK(st.duplicate_addresses == 1);
    CHECK(st.page_known == 32);
    /* Two rows for one address are one object: the page holds 31 distinct
     * objects here, so 31*30/2 pairs, not 32*31/2. */
    CHECK(st.same_page_pairs == 31u * 30u / 2u);
    kp_identity_summary();
  }

  /* The measured two-object case: slots 1 and 2 on one page, the rest with no
   * address.  It must report one same-page pair and no co-residency. */
  {
    struct kp_identity_stats st;
    kp_identity_window(32);
    CHECK(kp_identity_object(&g, 1, 0xffffffc0a3af8400ULL) == 1);
    CHECK(kp_identity_object(&g, 2, 0xffffffc0a3af8800ULL) == 1);
    for (size_t i = 0; i < 32; i++) {
      kp_identity_freeze(i, 'S');
      kp_identity_slot(i, "yes", 1, 1, 0, 0, 0, NULL);
    }
    kp_identity_stats(&st);
    CHECK(st.addressed == 2);
    CHECK(st.page_known == 2);
    CHECK(st.pages_observed == 1);
    CHECK(st.same_page_pairs == 1);
    CHECK(st.complete == 0);
    CHECK(st.co_residency == KP_CO_UNAVAILABLE);
    CHECK(st.co_reason != NULL && strcmp(st.co_reason, "unresolved-rows") == 0);
    kp_identity_summary();
  }

  /* A dump child's report carries its own verdict: the address its scan found,
   * the collisions that scan accepted, and the terminal state.  The parent
   * cannot re-run that check, so the report is the unit it validates.  Every
   * malformed form must be refused rather than recorded. */
  {
    struct kp_dump_report r;
    const char *bad[] = {
        "mm=ffffffc05adc0400 accepted=116 state=4",     /* no newline */
        "mm=ffffffc05adc0400 accepted=116 state=4\n\n", /* two lines */
        "mm=ffffffc05adc040 accepted=116 state=4\n",    /* short address */
        "mm=ffffffc05adc04000 accepted=116 state=4\n",  /* long address */
        "mm=FFFFFFC05ADC0400 accepted=116 state=4\n",   /* uppercase hex */
        "mm=ffffffc05adc0400 accepted= state=4\n",      /* no count */
        "mm=ffffffc05adc0400 accepted=116 state=44\n",  /* two digits */
        "mm=ffffffc05adc0400 accepted=116 state=4 \n",  /* trailing space */
        "mm=ffffffc05adc0400  accepted=116 state=4\n",  /* double space */
        "ffffffc05adc0400 accepted=116 state=4\n",      /* no field name */
        "mm=ffffffc05adc0400 accepted=116 state=x\n",   /* not a digit */
        "not a report\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      CHECK(kp_dump_report_parse(bad[i], strlen(bad[i]), &r) == 0);
    }
    CHECK(kp_dump_report_parse("", 0, &r) == 0);
    CHECK(kp_dump_report_parse(NULL, 10, &r) == 0);
    const char *good = "mm=ffffffc05adc0400 accepted=116 state=4\n";
    CHECK(kp_dump_report_parse(good, strlen(good), &r) == 1);
    CHECK(r.addr == 0xffffffc05adc0400ULL);
    CHECK(r.accepted == 116);
    CHECK(r.state == KP_DUMP_STATE_MM_FOUND);
  }

  /* Admission: a rejected report must leave the table untouched, and each
   * rejection carries its own reason so a failed scan can never read as an
   * identity.  The reasons below are the host-observable failure paths of the
   * collector: a child that died before reporting, one that died mid-line, one
   * whose scan ended without an mm, a zero address, a collision set under the
   * single-scan gate's bar, an address the geometry cannot resolve, and an
   * address another slot already owns. */
  {
    struct kp_identity_stats st;
    struct kp_geometry ag = measured();
    uintptr_t got = 1;
    const char *why = NULL;
    kp_identity_window(32);
    CHECK(kp_dump_admit(&ag, 1, NULL, 0, &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "no-report") == 0);
    CHECK(got == 0);
    CHECK(kp_dump_admit(&ag, 1, "mm=ffffffc05adc04", 16, &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "truncated-report") == 0);
    const char *garbage = "not a report\n";
    CHECK(kp_dump_admit(&ag, 1, garbage, strlen(garbage), &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "malformed-report") == 0);
    const char *notfound = "mm=ffffffc05adc0400 accepted=116 state=5\n";
    CHECK(kp_dump_admit(&ag, 1, notfound, strlen(notfound), &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "scan-state-not-found") == 0);
    const char *zero = "mm=0000000000000000 accepted=116 state=4\n";
    CHECK(kp_dump_admit(&ag, 1, zero, strlen(zero), &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "zero-address") == 0);
    const char *weak = "mm=ffffffc05adc0400 accepted=7 state=4\n";
    CHECK(kp_dump_admit(&ag, 1, weak, strlen(weak), &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "collision-set-too-small") == 0);
    struct kp_geometry broken = measured();
    broken.span = 3 * 4096;
    const char *ok = "mm=ffffffc05adc0400 accepted=116 state=4\n";
    CHECK(kp_dump_admit(&broken, 3, ok, strlen(ok), &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "geometry-check-failed") == 0);
    kp_identity_stats(&st);
    CHECK(st.rows == 0 && st.addressed == 0 && st.page_known == 0);
    /* The same report against the measured geometry is admitted. */
    CHECK(kp_dump_admit(&ag, 1, ok, strlen(ok), &got, &why) == 1);
    CHECK(got == 0xffffffc05adc0400ULL);
    CHECK(why != NULL && strcmp(why, "none") == 0);
    /* A second slot reporting the same object is refused and counted: two rows
     * for one address are not two objects, so not a co-residency pair. */
    CHECK(kp_dump_admit(&ag, 2, ok, strlen(ok), &got, &why) == 0);
    CHECK(why != NULL && strcmp(why, "duplicate-address") == 0);
    for (size_t i = 0; i < 32; i++) {
      kp_identity_slot(i, "yes", 1, 0, 0, 0, 0, NULL);
    }
    kp_identity_stats(&st);
    CHECK(st.addressed == 2);
    CHECK(st.page_known == 1);
    CHECK(st.invalid_geometry == 1); /* slot 3's address, page excluded */
    CHECK(st.duplicate_reports == 1);
    /* Slot 3's excluded row still carries the same raw address, so the literal
     * duplicate-address count sees it; it is not a page, not a pair and not an
     * identity. */
    CHECK(st.duplicate_addresses == 1);
    CHECK(st.same_page_pairs == 0);
    CHECK(st.co_residency == KP_CO_UNAVAILABLE);
    kp_identity_summary();
  }

  /* The collector's read path: one bounded wait that always closes the report
   * pipe.  A child that exits mid-line, never reports, or never answers must
   * leave no descriptor behind and must not stall the collector. */
  {
    char buf[64];
    int p[2];
    const char *line = "mm=ffffffc05adc0400 accepted=116 state=4\n";
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], line, strlen(line)) == (ssize_t)strlen(line));
    ssize_t used = kp_dump_read_report(p[0], buf, sizeof(buf), 1000);
    CHECK(used == (ssize_t)strlen(line));
    CHECK(memcmp(buf, line, (size_t)used) == 0);
    CHECK(fcntl(p[0], F_GETFD) == -1 && errno == EBADF);
    close(p[1]);

    /* The child died mid-line: the bytes arrived, the line did not. */
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "mm=ffffff", 9) == 9);
    close(p[1]);
    used = kp_dump_read_report(p[0], buf, sizeof(buf), 1000);
    CHECK(used == 9);
    CHECK(memchr(buf, '\n', (size_t)used) == NULL);
    CHECK(fcntl(p[0], F_GETFD) == -1);

    /* The child exited without reporting. */
    CHECK(pipe(p) == 0);
    close(p[1]);
    used = kp_dump_read_report(p[0], buf, sizeof(buf), 1000);
    CHECK(used == 0);
    CHECK(fcntl(p[0], F_GETFD) == -1);

    /* No writer and no EOF: the wait is bounded by the deadline and the pipe
     * is closed anyway. */
    struct timespec t0;
    struct timespec t1;
    CHECK(pipe(p) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    used = kp_dump_read_report(p[0], buf, sizeof(buf), 50);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L +
              (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    CHECK(used == 0);
    CHECK(ms >= 40 && ms < 5000);
    CHECK(fcntl(p[0], F_GETFD) == -1);
    close(p[1]);

    /* A bad descriptor or capacity is refused, and still closed. */
    CHECK(kp_dump_read_report(-1, buf, sizeof(buf), 10) == -1);
    CHECK(pipe(p) == 0);
    CHECK(kp_dump_read_report(p[0], buf, 1, 10) == -1);
    CHECK(fcntl(p[0], F_GETFD) == -1);
    close(p[1]);
  }

  /* The DONE counts must reconcile: a child that never reported is missing,
   * not rejected; a launched child is alive or gone; only a gone child can
   * have been reaped. */
  {
    size_t missing = 99;
    CHECK(kp_dump_account(2, 2, 2, 0, 2, 0, 0, &missing) == 1);
    CHECK(missing == 0);
    CHECK(kp_dump_account(2, 2, 1, 0, 2, 0, 0, &missing) == 1);
    CHECK(missing == 1);
    /* Received and refused is not missing. */
    CHECK(kp_dump_account(2, 2, 1, 1, 2, 0, 0, &missing) == 1);
    CHECK(missing == 0);
    /* A launched child that is neither alive nor gone cannot reconcile. */
    CHECK(kp_dump_account(2, 2, 2, 0, 1, 0, 0, &missing) == 0);
    /* More reports than slots cannot reconcile. */
    CHECK(kp_dump_account(2, 2, 2, 1, 2, 0, 0, &missing) == 0);
    /* Reaped above gone is impossible. */
    CHECK(kp_dump_account(2, 2, 2, 0, 1, 1, 2, &missing) == 0);
    CHECK(kp_dump_account(2, 2, 2, 0, 1, 1, 1, &missing) == 1);
    CHECK(missing == 0);
  }

  /* Reading the host's own slabinfo must not crash and must leave a
   * consistent structure whether or not the file is readable. */
  {
    struct kp_geometry host;
    kp_identity_read(&host);
    if (host.measured) {
      CHECK(host.objsize > 0 && host.objs_per_slab > 0);
      CHECK(host.span == (long)host.pages_per_slab * 4096);
      CHECK(host.span_agrees == 1);
    } else {
      CHECK(host.objsize == -1 && host.span == -1);
    }
  }

  if (failures) {
    printf("%d identity check(s) failed\n", failures);
    return 1;
  }
  printf("ok   identity: measured geometry resolves the address, a failed "
         "geometry check yields no page, and an incomplete table claims no "
         "grouping\n");
  return 0;
}
