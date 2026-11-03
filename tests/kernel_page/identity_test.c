/* Host test for src/kernel_page/identity.c.
 *
 * The geometry used here is the one this device reported:
 *   mm_struct 808 992 1024 32 8 : ... : slabdata 31 31 0
 * so objsize 1024, objperslab 32, pagesperslab 8, span 32768.  The address is
 * the leaked mm of a delivered cycle, ffffffc0a3af8400.
 *
 * The point of the second case is that the resolution can come out false: a
 * span that is not the measured one puts the object on a different page and a
 * different slot, so a passing first case is evidence about the arithmetic,
 * not a tautology.
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
  g.measured = 1;
  g.span_agrees = 1;
  return g;
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

  /* No geometry: the page is reported unavailable, never a guessed base. */
  {
    struct kp_geometry none = measured();
    none.measured = 0;
    none.span = -1;
    CHECK(kp_identity_anchor(&none, 0xffffffc0a3af8400ULL, 1, &id) == 0);
    CHECK(id.page == 0);
  }

  /* A geometry whose fields do not agree is reported, not hidden. */
  {
    struct kp_geometry odd = measured();
    odd.span_agrees = 0;
    CHECK(kp_identity_anchor(&odd, 0xffffffc0a3af8400ULL, 1, &id) == 1);
  }

  /* The table rows must not crash for addressed or unaddressed slots, or for
   * a missing sample. */
  kp_identity_anchor(&g, 0xffffffc0a3af8400ULL, 1, &id);
  kp_identity_slot(1, "yes", 1, 0, 0, 0, &id);
  kp_identity_slot(0, "yes", 1, -1, 0, 0, &id);
  kp_identity_slot(2, "zombie", 1, 0, -32, -1, &id);
  kp_identity_slot(3, "gone", 0, 0, 0, 0, &id);
  kp_identity_summary(32, 1);

  /* Reading the host's own slabinfo must not crash and must leave a
   * consistent structure whether or not the file is readable. */
  {
    struct kp_geometry host;
    kp_identity_read(&host);
    if (host.measured) {
      CHECK(host.objsize > 0 && host.objs_per_slab > 0);
      CHECK(host.span == (long)host.pages_per_slab * 4096);
    } else {
      CHECK(host.objsize == -1 && host.span == -1);
    }
  }

  if (failures) {
    printf("%d identity check(s) failed\n", failures);
    return 1;
  }
  printf("ok   identity: measured geometry resolves the address; wrong span "
         "does not\n");
  return 0;
}
