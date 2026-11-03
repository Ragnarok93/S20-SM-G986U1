/* Kernel-page experiment: shared interfaces.
 *
 * The experiment has two kinds of output that must never be conflated:
 *
 *   allocator_observation  what /proc/slabinfo and /proc/meminfo report
 *   experiment_state       which propositions the run actually established
 *
 * The counters in allocator_observation are defined by mm/slub.c in this
 * tree (4.19.113), not by this file:
 *
 *   get_slabinfo()            (mm/slub.c:6191)
 *     nr_slabs += node_nr_slabs(n);        -> slabdata
 *     nr_objs  += node_nr_objs(n);         -> num_objs
 *     nr_free  += count_partial(count_free)
 *     active_objs = num_objs - nr_free
 *
 * so every counter counts ONLY pages on n->partial.  A page frozen on a
 * CPU's c->partial list, the CPU's active page, and a full page on no list
 * are invisible to all three.  slabdata is literally n->nr_partial, the
 * quantity __slab_free() compares against s->min_partial
 * (mm/slub.c:3154), which is why it is also the right place to read the
 * discard precondition.
 */
#ifndef KERNEL_PAGE_H
#define KERNEL_PAGE_H

#include <stdint.h>
#include <stdio.h>

/* --- allocator_observation --------------------------------------------- */

/* Parse the mm_struct line out of an already-open /proc/slabinfo.  Returns
 * 1 when the line was found, 0 otherwise; any pointer may be NULL. */
int mm_slab_scan(FILE *f, long *active, long *num, long *slabs);

/* Same, but open/close the file.  open failure leaves -1 in every output,
 * which is distinguishable from a parsed zero. */
void mm_slab_counts_full(long *active, long *num, long *slabs);
void mm_slab_counts(long *active, long *slabs);

/* One-line report of a sample already taken, and a fresh sample with the raw
 * mm_struct line plus /proc/meminfo Slab:. */
void mm_slab_print(const char *tag, long active, long num, long slabs);
void mm_slab_log(const char *tag);

/* kmalloc-32k line, for the order-3 page accounting around the sends. */
void slab32_log(const char *tag);

/* --- experiment_state -------------------------------------------------- */

/* Each proposition is a separate status.  A run may verify the allocator
 * lifecycle while failing to establish object identity, and the report must
 * say exactly that instead of one success flag. */
enum kp_status {
  KP_ADDRESS_VERIFIED = 1u << 0,   /* recovered address re-checked against
                                    * this run's measured hash collisions */
  KP_ANCHOR_VERIFIED = 1u << 1,    /* one fresh 32-slot slab, whole burst
                                    * accounted, no second slab appeared */
  KP_ALLOCATOR_TRANSITION_VERIFIED = 1u << 2, /* the emptying free registered
                                    * in the allocator's own accounting */
  KP_RECLAMATION_VERIFIED = 1u << 3, /* the slab left the cache as a whole
                                    * (d_num = -objects, d_slabs = -1) */
  KP_OBJECT_IDENTITY_VERIFIED = 1u << 4, /* the image is known to occupy the
                                    * reclaimed page */
  KP_KERNEL_CONSUMPTION_VERIFIED = 1u << 5, /* the kernel is known to have
                                    * read the planted bytes */
};

void kp_result_reset(void);
void kp_result_mark(enum kp_status status);
int kp_result_has(enum kp_status status);

/* Print one line: verified=<names> unverified=<names> base=<addr>.
 * Identity and consumption are never marked by the preparation path; they
 * cannot be established from inside it. */
void kp_result_report(uintptr_t base);

const char *kp_status_name(enum kp_status status);

#endif /* KERNEL_PAGE_H */
