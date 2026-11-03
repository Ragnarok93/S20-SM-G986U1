/* branch.c — see branch.h.  One classifier, used for the anchor's emptying
 * free and for the control's free, so the two cycles are compared on the same
 * evidence instead of on two hand-written state strings.
 *
 * page->inuse is not readable from this process: /proc/slabinfo is cache-wide
 * and /sys/kernel/slab is denied to the shell (measured: EACCES on every
 * attribute).  The classifier therefore reports inuse as unavailable and
 * prints the residual values the surviving paths allow, rather than choosing
 * one.
 */
#include "common.h"
#include "branch.h"

#include <string.h>

void kp_free_classify(const struct kp_free_delta *d,
                      struct kp_free_verdict *out) {
  memset(out, 0, sizeof(*out));
  out->inuse_at_free = -1;
  out->page_listed = -1;
  out->path_names = "unknown";
  out->state = "other";
  if (d == NULL || !d->sampled || d->objects <= 0) {
    return;
  }
  if (d->d_num == -(long)d->objects && d->d_slabs == -1 && d->d_active < 0) {
    /* The page emptied and left the cache: slab_empty -> discard_slab. */
    out->paths = KP_PATH_DISCARD;
    out->page_listed = 1;
    out->inuse_at_free = 1; /* the free took the last reference */
    out->inuse_exact = 1;
    out->path_names = "discard";
    out->state = "listed-discarded";
    return;
  }
  if (d->d_num == 0 && d->d_slabs == 0 && d->d_active < 0) {
    /* Something on n->partial gained a free object.  Either the page stayed
     * listed with a live object (inuse at the free >= 2) or it emptied and
     * was kept because n->nr_partial < s->min_partial (inuse 1).  The
     * counters cannot separate those two, so both stay in the set. */
    out->paths = KP_PATH_LISTED | KP_PATH_LISTED_REFUSED;
    out->page_listed = 1;
    out->path_names = "listed,emptied-refused";
    out->state = "listed";
    return;
  }
  if (d->d_num == 0 && d->d_slabs == 0 && d->d_active == 0) {
    /* Nothing moved: the page was not on a node list.  A full page's first
     * free pushes it to c->partial, a frozen page's free is FREE_FROZEN, and
     * a free on this CPU's active slab never reaches __slab_free(). */
    out->paths = KP_PATH_FASTPATH | KP_PATH_FROZEN | KP_PATH_FREEZE_PUSH;
    out->page_listed = 0;
    out->path_names = "fastpath,frozen,freeze-push";
    out->state = "unlisted";
    return;
  }
}

void kp_free_report(const char *tag, const struct kp_free_delta *d,
                    const struct kp_free_verdict *v, int control_discard,
                    long gap_us) {
  const char *inuse = "unavailable";
  const char *inuse_src = "unavailable(page-inuse-not-exposed)";
  const char *residual = "unknown";
  if (v->inuse_exact) {
    inuse = "1";
    inuse_src = "derived(page-emptied)";
    residual = "none";
  } else if (v->paths & KP_PATH_LISTED) {
    residual = "listed:>=2,emptied-refused:1";
  } else if (v->paths & KP_PATH_FASTPATH) {
    residual = ">=1";
  }
  pr_info("KS_BRANCH tag=%s d_active=%ld d_num=%ld d_slabs=%ld paths=%s "
          "page_listed=%s inuse_at_free=%s inuse_src=%s residual=%s "
          "control=%s gap_us=%ld\n",
          tag, d->d_active, d->d_num, d->d_slabs, v->path_names,
          v->page_listed < 0 ? "unavailable" : (v->page_listed ? "1" : "0"),
          inuse, inuse_src, residual,
          control_discard < 0 ? "not-run"
                              : (control_discard ? "discard" : "no-discard"),
          gap_us);
}
