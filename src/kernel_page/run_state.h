/* run_state.h — the lifecycle of one kernel-page preparation, as a state
 * machine that can be tested without a device.
 *
 * Rules the model enforces (each has a host test):
 *   - stages advance one at a time, never backwards, never skipped;
 *   - a run that is still RUNNING can never be reported as successful;
 *   - a refusal is terminal and must carry a reason;
 *   - delivery is only possible from the gate stage, and never when a
 *     required observation is missing;
 *   - finishing is only possible from a terminal state, and the reported
 *     status is derived from the recorded state rather than passed in.
 *
 * Pure: no I/O, no globals.  src/util.c owns the printing.
 */
#ifndef KERNEL_PAGE_RUN_STATE_H
#define KERNEL_PAGE_RUN_STATE_H

enum kp_stage {
  KP_STAGE_INIT = 0,
  KP_STAGE_DRAIN,
  KP_STAGE_BASELINE,
  KP_STAGE_FREE,
  KP_STAGE_GATE,
  KP_STAGE_DONE
};

enum kp_outcome {
  KP_RUN_RUNNING = 0,
  KP_RUN_REFUSED,
  KP_RUN_DELIVERED
};

typedef struct kp_run {
  enum kp_stage stage;
  enum kp_outcome outcome;
  const char *refusal;   /* static string; required when REFUSED */
  unsigned samples;      /* observations recorded */
  unsigned missing;      /* observations that could not be read */
  int gate_reached;
} kp_run;

/* Start tracking at the first stage the caller actually observes.  Returns 0
 * for an out-of-range stage. */
int kp_run_begin(kp_run *run, enum kp_stage first);

/* Advance exactly one stage.  Returns 0 for a skip, a step backwards, after a
 * terminal outcome, or when moving to DONE (use kp_run_finish). */
int kp_run_advance(kp_run *run, enum kp_stage next);

/* Record one sample.  A negative value is the scan's "unreadable"; it is
 * counted as missing and stays distinguishable from a real zero. */
void kp_run_sample(kp_run *run, long active, long num, long slabs);

/* Terminal outcomes.  Refusal requires a non-empty reason; delivery requires
 * that the gate stage was reached and that no observation is missing. */
int kp_run_refuse(kp_run *run, const char *reason);
int kp_run_deliver(kp_run *run);

/* Mark cleanup complete.  Only valid from a terminal outcome. */
int kp_run_finish(kp_run *run);

/* Does the state support the claim being reported?  False while RUNNING, false
 * for a refusal without a reason, false for a delivery with a missing
 * observation.  This is the check the report path must use. */
int kp_run_report_ok(const kp_run *run);

const char *kp_stage_name(enum kp_stage stage);
const char *kp_outcome_name(enum kp_outcome outcome);

#endif /* KERNEL_PAGE_RUN_STATE_H */
