/* Host test for src/kernel_page/branch.c.
 *
 * Each case is one delta the cycle can actually produce, with the path set the
 * kernel's own conditionals leave open.  Run through tests/kernel_page/run.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel_page/branch.h"

int g_exlog_fd = -1;
char g_exlog_path[256] = "";

/* branch.c logs through the payload's log function; print it on the host. */
void exlog_writef(const char *level, const char *fmt, ...) {
  va_list ap;
  (void)level;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

static int failures;

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);  \
      failures++;                                            \
    }                                                        \
  } while (0)

static struct kp_free_delta delta(long a, long n, long s) {
  struct kp_free_delta d;
  d.d_active = a;
  d.d_num = n;
  d.d_slabs = s;
  d.objects = 32;
  d.sampled = 1;
  return d;
}

int main(void) {
  struct kp_free_verdict v;
  struct kp_free_delta d;

  /* Emptied page, discarded: -32 objects and one slab, and the free itself
   * made one more free object visible. */
  d = delta(-1, -32, -1);
  kp_free_classify(&d, &v);
  CHECK(v.paths == KP_PATH_DISCARD);
  CHECK(v.page_listed == 1);
  CHECK(v.inuse_at_free == 1 && v.inuse_exact == 1);
  CHECK(strcmp(v.state, "listed-discarded") == 0);

  /* One free object appeared, no page left: the page stayed listed.  The
   * counters cannot say whether it held another object (inuse >= 2) or
   * emptied and was kept below min_partial, so both stay in the set. */
  d = delta(-1, 0, 0);
  kp_free_classify(&d, &v);
  CHECK(v.paths == (KP_PATH_LISTED | KP_PATH_LISTED_REFUSED));
  CHECK(v.page_listed == 1);
  CHECK(v.inuse_at_free == -1 && v.inuse_exact == 0);
  CHECK(strcmp(v.state, "listed") == 0);

  /* Nothing moved: not on a node list.  Fastpath, FREE_FROZEN and the
   * full-page push into c->partial are all consistent. */
  d = delta(0, 0, 0);
  kp_free_classify(&d, &v);
  CHECK(v.paths == (KP_PATH_FASTPATH | KP_PATH_FROZEN | KP_PATH_FREEZE_PUSH));
  CHECK(v.page_listed == 0);
  CHECK(v.inuse_at_free == -1);
  CHECK(strcmp(v.state, "unlisted") == 0);

  /* Missing sample: nothing may be claimed. */
  d = delta(0, 0, 0);
  d.sampled = 0;
  kp_free_classify(&d, &v);
  CHECK(v.paths == KP_PATH_UNKNOWN);
  CHECK(v.page_listed == -1);
  CHECK(v.inuse_at_free == -1);
  CHECK(strcmp(v.state, "other") == 0);

  /* A slab's worth of objects without a slab change is not a discard, and a
   * slab change without the object count is not one either. */
  d = delta(-1, -32, 0);
  kp_free_classify(&d, &v);
  CHECK(v.paths == KP_PATH_UNKNOWN);
  d = delta(-1, 0, -1);
  kp_free_classify(&d, &v);
  CHECK(v.paths == KP_PATH_UNKNOWN);
  d = delta(-1, -64, -1);
  kp_free_classify(&d, &v);
  CHECK(v.paths == KP_PATH_UNKNOWN);

  /* Two free objects appearing is still a listed page, with inuse >= 2. */
  d = delta(-2, 0, 0);
  kp_free_classify(&d, &v);
  CHECK(v.paths == (KP_PATH_LISTED | KP_PATH_LISTED_REFUSED));

  /* Reporting must not crash for a missing control. */
  d = delta(-1, 0, 0);
  kp_free_classify(&d, &v);
  kp_free_report("test", &d, &v, -1, -1);
  kp_free_report("test", &d, &v, 1, 5230);

  if (failures) {
    printf("%d branch check(s) failed\n", failures);
    return 1;
  }
  printf("ok   branch: discard, listed residual and unlisted set classified\n");
  return 0;
}
