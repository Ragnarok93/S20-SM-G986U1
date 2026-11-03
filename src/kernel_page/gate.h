/* gate.h — the one place that decides whether the emptying free did what the
 * chain needs, expressed as arithmetic over two /proc/slabinfo samples.
 *
 * Pure: no I/O, no globals, no printing.  src/util.c owns the sampling and the
 * KS_GATE format; this file owns the comparison, so the decision can be tested
 * on the host against the counter model in mm/slub.c instead of on a device.
 *
 * The model (verified against ~/kernel_samsung_sm8250/mm/slub.c, 4.19.113):
 *   active_objs = total_objects - free objects on n->partial
 *   num_objs    = total_objects over every live page of the cache
 *   slabdata    = n->nr_slabs, the number of live pages
 * A listed page contributes its inuse to active_objs; an unlisted one
 * contributes all of its objects.  So freeing the last object of a listed
 * 32-object page and discarding it reads d_active=-1, d_num=-32, d_slabs=-1.
 */
#ifndef KERNEL_PAGE_GATE_H
#define KERNEL_PAGE_GATE_H

typedef struct kp_sample {
  long active;
  long num;
  long slabs;
} kp_sample;

typedef struct kp_gate_facts {
  int span_ok;        /* the burst built exactly one fresh slab */
  int target_freed;   /* the emptying free actually ran */
  int empty_cpu;      /* CPU the free ran on */
  int want_cpu;       /* CPU the chain requires */
  int pin_bad;        /* any pin or CPU proof failed */
  int slot_partial;   /* a burst slot came from a foreign partial slab */
  int baseline_ok;    /* a fresh baseline was read immediately before the
                       * free and two reads agreed; 0 means the samples span
                       * other events and the window is not one event wide */
  long objects_per_slab;
} kp_gate_facts;

typedef struct kp_gate_verdict {
  long d_active;
  long d_num;
  long d_slabs;
  int registered;  /* active fell by exactly one: the free reached the page's
                    * inuse, which the do_slab_free fastpath does not touch */
  int discarded;   /* num fell by a whole slab and slabdata by one */
  int targeted;    /* the gate the chain fires on: registered AND discarded,
                    * on the right CPU, with no foreign burst slot */
} kp_gate_verdict;

/* Evaluate.  `before` is the quiesced baseline taken immediately before the
 * emptying free, `after` the sample taken after the free and the sends. */
void kp_gate_evaluate(const kp_gate_facts *facts, const kp_sample *before,
                      const kp_sample *after, kp_gate_verdict *out);

#endif /* KERNEL_PAGE_GATE_H */
