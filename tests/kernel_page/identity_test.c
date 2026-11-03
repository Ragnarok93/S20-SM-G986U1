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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
