/* identity.h — per-object slab-page identity, and what this surface allows.
 *
 * Every output line names the source of each value it prints:
 *
 *   measured     /proc/slabinfo geometry fields, the KernelSnitch object
 *                address, each slot's task state, each free's counter delta.
 *   derived      page base, in-page offset, slot index, and the reading of a
 *                delta as an occupancy effect — arithmetic on measured values.
 *   unavailable  a value this surface cannot supply, printed with its reason
 *                instead of an aggregate counter or a zero.
 */
#ifndef KP_IDENTITY_H
#define KP_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/* One mm_struct slab's geometry.  /proc/slabinfo reports
 * <objsize> <objperslab> <pagesperslab> for every cache, so the span of one
 * slab is measured here rather than assumed from the payload's constants. */
struct kp_geometry {
  long objsize;        /* measured: slabinfo field 3 */
  long objs_per_slab;  /* measured: slabinfo field 4 */
  long pages_per_slab; /* measured: slabinfo field 5 */
  long span;           /* derived: pages_per_slab * page size */
  int measured;        /* 1 when all three fields parsed */
  int span_agrees;     /* objsize * objs_per_slab == span */
};

/* One object resolved to its slab page.  addr is measured; page, offset and
 * slot are arithmetic on addr and the measured geometry. */
struct kp_identity {
  uintptr_t addr;
  uintptr_t page;
  size_t offset;
  long slot;
  int constructed_slot;
  int slot_match;
};

/* Read the geometry.  On failure every field stays -1 and measured stays 0,
 * which is distinguishable from a parsed zero. */
void kp_identity_read(struct kp_geometry *g);

/* One line: the measured geometry, where it came from, whether the payload's
 * own constants agree with it, and which surface attribute is denied.
 * Printed once per run, before the burst. */
void kp_identity_surface(const struct kp_geometry *g,
                         long assumed_objs_per_slab, long assumed_span);

/* Resolve the one object address this run has to its slab page.  Returns 1
 * when the derived slot equals the slot the burst built. */
int kp_identity_anchor(const struct kp_geometry *g, uintptr_t addr,
                       int constructed_slot, struct kp_identity *out);

/* One table row per burst slot.  `alive` is the task state measured before
 * the free ("yes", "zombie", "gone"); with sampled set, d_* are the
 * cache-wide counter changes that free produced, the only occupancy effect a
 * cache-wide counter can report. */
void kp_identity_slot(size_t slot, const char *alive, int sampled,
                      long d_active, long d_num, long d_slabs,
                      const struct kp_identity *anchor);

/* Close the table: how many objects got an address, and the reason the rest
 * cannot. */
void kp_identity_summary(size_t slots, size_t addressed);

#endif /* KP_IDENTITY_H */
