/* run_state.c — see run_state.h.  No I/O: the caller prints the state and the
 * caller decides when to advance; this file only refuses to record an
 * impossible history.
 */
#include "run_state.h"

int kp_run_begin(kp_run *run, enum kp_stage first) {
  if (!run) return 0;
  if (first < KP_STAGE_INIT || first >= KP_STAGE_DONE) return 0;
  run->stage = first;
  run->outcome = KP_RUN_RUNNING;
  run->refusal = 0;
  run->samples = 0;
  run->missing = 0;
  run->gate_reached = (first == KP_STAGE_GATE) ? 1 : 0;
  return 1;
}

int kp_run_advance(kp_run *run, enum kp_stage next) {
  if (!run) return 0;
  if (run->outcome != KP_RUN_RUNNING) return 0;
  if (next >= KP_STAGE_DONE) return 0;
  if ((int)next != (int)run->stage + 1) return 0;
  run->stage = next;
  if (next == KP_STAGE_GATE) {
    run->gate_reached = 1;
  }
  return 1;
}

void kp_run_sample(kp_run *run, long active, long num, long slabs) {
  if (!run) return;
  run->samples++;
  if (active < 0 || num < 0 || slabs < 0) {
    run->missing++;
  }
}

int kp_run_refuse(kp_run *run, const char *reason) {
  if (!run) return 0;
  if (run->outcome != KP_RUN_RUNNING) return 0;
  if (reason == 0 || reason[0] == 0) return 0;
  run->outcome = KP_RUN_REFUSED;
  run->refusal = reason;
  return 1;
}

int kp_run_deliver(kp_run *run) {
  if (!run) return 0;
  if (run->outcome != KP_RUN_RUNNING) return 0;
  if (run->stage != KP_STAGE_GATE || !run->gate_reached) return 0;
  if (run->missing != 0) return 0;
  run->outcome = KP_RUN_DELIVERED;
  return 1;
}

int kp_run_finish(kp_run *run) {
  if (!run) return 0;
  if (run->outcome == KP_RUN_RUNNING) return 0;
  run->stage = KP_STAGE_DONE;
  return 1;
}

int kp_run_report_ok(const kp_run *run) {
  if (!run) return 0;
  if (run->outcome == KP_RUN_RUNNING) return 0;
  if (run->outcome == KP_RUN_REFUSED) {
    return run->refusal != 0 && run->refusal[0] != 0;
  }
  /* delivered: the gate must have been reached and every observation read */
  return run->gate_reached == 1 && run->missing == 0;
}

const char *kp_stage_name(enum kp_stage stage) {
  switch (stage) {
    case KP_STAGE_INIT:
      return "init";
    case KP_STAGE_DRAIN:
      return "drain";
    case KP_STAGE_BASELINE:
      return "baseline";
    case KP_STAGE_FREE:
      return "free";
    case KP_STAGE_GATE:
      return "gate";
    case KP_STAGE_DONE:
      return "done";
  }
  return "?";
}

const char *kp_outcome_name(enum kp_outcome outcome) {
  switch (outcome) {
    case KP_RUN_RUNNING:
      return "running";
    case KP_RUN_REFUSED:
      return "refused";
    case KP_RUN_DELIVERED:
      return "delivered";
  }
  return "?";
}
