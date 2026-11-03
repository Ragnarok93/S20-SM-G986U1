/* branch.h — which __slab_free() path a measured free is consistent with.
 *
 * The kernel does not expose page->inuse, page->frozen or the node partial
 * count to this process, so a free is classified by what the cache-wide
 * counters did and the classifier names the paths CONSISTENT WITH that delta.
 * It is not a trace of the branch executed, and it never returns a single
 * path where the counters cannot separate two.
 *
 * Attribution rule: a cache-wide delta belongs to the target free only when
 * competing cache activity in the same interval is excluded or independently
 * accounted for. Otherwise it is a signature consistent with the path, and
 * the line says so (basis=cache-wide, paths_src=consistent-with).
 *
 * mm/slub.c (4.19.113) branches this way for a free on a page that is not
 * this CPU's active slab:
 *
 *   __slab_free():
 *     prior       = page->freelist;
 *     was_frozen  = page->frozen;
 *     if ((!new.inuse || !prior) && !was_frozen):
 *         if (has_cpu_partial && !prior)
 *                      -> new.frozen = 1; put_cpu_partial()   (full page)
 *         else if (!new.inuse && n->nr_partial >= s->min_partial)
 *                      -> slab_empty, discard_slab()          (emptied)
 *         else         -> inuse--, page stays listed          (listed)
 *     if (was_frozen) -> FREE_FROZEN, nothing moves
 *
 * has_cpu_partial(s) holds for mm_struct: the first free of a full page
 * leaves active==num (nothing listed), which the else-branch would have
 * contradicted by 31.  It is derived from source plus that measured
 * signature, not read from sysfs; see docs/kernel-page/slub-branch-audit.md.
 *
 * do_slab_free() takes the page off this CPU's freelist before __slab_free()
 * is reached at all.
 */
#ifndef KP_BRANCH_H
#define KP_BRANCH_H

#include <stddef.h>

enum kp_free_path {
  KP_PATH_UNKNOWN = 0,
  KP_PATH_FASTPATH = 1u << 0,        /* do_slab_free: c->page, no counter move */
  KP_PATH_FROZEN = 1u << 1,          /* FREE_FROZEN, no counter move */
  KP_PATH_FREEZE_PUSH = 1u << 2,     /* full page -> put_cpu_partial, no move */
  KP_PATH_LISTED = 1u << 3,          /* listed, inuse--, page stays */
  KP_PATH_LISTED_REFUSED = 1u << 4,  /* listed, emptied, nr_partial < min */
  KP_PATH_DISCARD = 1u << 5,         /* listed, emptied, discarded */
};

/* One free's measured effect.  sampled = 0 when either sample is missing. */
struct kp_free_delta {
  long d_active;
  long d_num;
  long d_slabs;
  int objects;  /* objects per slab, measured from slabinfo */
  int sampled;
};

struct kp_free_verdict {
  unsigned paths;       /* bitmask of enum kp_free_path: consistent with */
  int listed_signature; /* 1: the delta matches a node-list transition,
                         * 0: it matches none, -1: no sample.  A signature,
                         * not an observation of the page's list membership. */
  int inuse_at_free;    /* 1, 2 (means >= 2), or -1 when not derivable */
  int inuse_exact;      /* 1 only when the path fixes inuse to 1 */
  const char *path_names;  /* stable text for a log line */
  const char *state;       /* the four-state token */
};

void kp_free_classify(const struct kp_free_delta *d,
                      struct kp_free_verdict *out);

/* Print one attribution line for a free.  control_discard/gap_us are the
 * tight control's result and its distance from this free; they are reported,
 * not used to narrow the path set. */
void kp_free_report(const char *tag, const struct kp_free_delta *d,
                    const struct kp_free_verdict *v, int control_discard,
                    long gap_us);

#endif /* KP_BRANCH_H */
