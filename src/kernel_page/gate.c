/* gate.c — see gate.h.  The comparisons here are the ones src/util.c used to
 * make inline; they are moved verbatim so a host test can exercise them.
 */
#include "gate.h"

void kp_gate_evaluate(const kp_gate_facts *facts, const kp_sample *before,
                      const kp_sample *after, kp_gate_verdict *out) {
  /* mm_slab_scan() reports a failed read as -1, never as a count.  A missing
   * sample must not be able to look like a small cache: with a one-object
   * cache, unreadable values would otherwise satisfy both deltas by accident. */
  if (before->active < 0 || before->num < 0 || before->slabs < 0 ||
      after->active < 0 || after->num < 0 || after->slabs < 0) {
    out->d_active = 0;
    out->d_num = 0;
    out->d_slabs = 0;
    out->registered = 0;
    out->discarded = 0;
    out->targeted = 0;
    return;
  }

  out->d_active = after->active - before->active;
  out->d_num = after->num - before->num;
  out->d_slabs = after->slabs - before->slabs;

  /* Exactly one object: the emptying free's own.  A frozen page moves no
   * counter at all, and a free that lands on a foreign listed page moves this
   * one without removing a slab. */
  out->registered =
      (before->active > 0 && after->active == before->active - 1) ? 1 : 0;

  /* One page's worth of objects and one page left the cache. */
  out->discarded = (before->num > 0 && before->slabs > 0 &&
                    facts->objects_per_slab > 0 &&
                    after->num == before->num - facts->objects_per_slab &&
                    after->slabs == before->slabs - 1)
                       ? 1
                       : 0;

  /* Both halves are required.  `discarded` alone is satisfied by a page whose
   * 32 objects were all counted active (d_active=-32) and by a window that
   * carries one unrelated free on top of a discard (d_active=-2, the measured
   * case); neither is the anchor's emptying free.  Only -1 and one page mean
   * the free that emptied the page is the free the counters saw. */
  out->targeted = (facts->span_ok == 1 && facts->target_freed &&
                   facts->empty_cpu == facts->want_cpu && out->registered &&
                   out->discarded && facts->baseline_ok &&
                   facts->pin_bad == 0 && facts->slot_partial == 0)
                      ? 1
                      : 0;
}
