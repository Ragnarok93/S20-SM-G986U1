/* Host test for src/kernel_page/run_state.c — the lifecycle properties, none of
 * which need the device.
 */
#include <stdio.h>
#include <string.h>

#include "kernel_page/run_state.h"

static int failures;

static void check(const char *name, int got, int want) {
  if (got == want) {
    printf("ok   %s\n", name);
    return;
  }
  failures++;
  printf("FAIL %s: got %d, want %d\n", name, got, want);
}

int main(void) {
  kp_run r;

  /* Initialization: an incomplete run is never reported as successful. */
  memset(&r, 0, sizeof(r));
  check("begin", kp_run_begin(&r, KP_STAGE_DRAIN), 1);
  check("running is not success", kp_run_report_ok(&r), 0);
  check("cannot finish while running", kp_run_finish(&r), 0);

  /* Ordering: no skipping, no going back. */
  check("skip drain->free rejected", kp_run_advance(&r, KP_STAGE_FREE), 0);
  check("advance to next", kp_run_advance(&r, KP_STAGE_BASELINE), 1);
  check("backwards rejected", kp_run_advance(&r, KP_STAGE_DRAIN), 0);
  check("cannot advance into done", kp_run_advance(&r, KP_STAGE_DONE), 0);

  /* Measurement: a missing read stays distinguishable from a real zero. */
  kp_run_sample(&r, 100, 3200, 100);
  kp_run_sample(&r, -1, -1, -1);
  check("one sample missing", (int)r.missing, 1);
  check("samples counted", (int)r.samples, 2);

  /* Validation gates delivery: a missing observation blocks it. */
  check("advance to free", kp_run_advance(&r, KP_STAGE_FREE), 1);
  check("deliver outside gate rejected", kp_run_deliver(&r), 0);
  check("advance to gate", kp_run_advance(&r, KP_STAGE_GATE), 1);
  check("deliver with missing sample rejected", kp_run_deliver(&r), 0);

  /* Refusal: terminal, must carry a reason, reported as refusal. */
  check("refusal without reason rejected", kp_run_refuse(&r, ""), 0);
  check("refusal", kp_run_refuse(&r, "gate-preconditions"), 1);
  check("outcome is refusal", (int)r.outcome, (int)KP_RUN_REFUSED);
  check("refusal is reported as refusal", kp_run_report_ok(&r), 1);
  check("advance after refusal rejected", kp_run_advance(&r, KP_STAGE_DONE), 0);
  check("deliver after refusal rejected", kp_run_deliver(&r), 0);
  check("finish after refusal", kp_run_finish(&r), 1);
  check("refusal reported ok", kp_run_report_ok(&r), 1);
  check("stage is done", (int)r.stage, (int)KP_STAGE_DONE);

  /* A clean run can deliver, and only then is the report ok. */
  memset(&r, 0, sizeof(r));
  check("begin at drain", kp_run_begin(&r, KP_STAGE_DRAIN), 1);
  check("drain->baseline", kp_run_advance(&r, KP_STAGE_BASELINE), 1);
  kp_run_sample(&r, 100, 3200, 100);
  check("baseline->free", kp_run_advance(&r, KP_STAGE_FREE), 1);
  check("free->gate", kp_run_advance(&r, KP_STAGE_GATE), 1);
  kp_run_sample(&r, 99, 3168, 99);
  check("deliver", kp_run_deliver(&r), 1);
  check("delivered report ok", kp_run_report_ok(&r), 1);
  check("finish delivered", kp_run_finish(&r), 1);
  check("delivered stage done", (int)r.stage, (int)KP_STAGE_DONE);
  check("refusal string absent", r.refusal == 0, 1);

  /* A failed validation cannot become successful through a later stage. */
  memset(&r, 0, sizeof(r));
  check("begin", kp_run_begin(&r, KP_STAGE_DRAIN), 1);
  check("advance", kp_run_advance(&r, KP_STAGE_BASELINE), 1);
  check("refuse", kp_run_refuse(&r, "no-crossing"), 1);
  check("cannot re-begin", kp_run_begin(&r, KP_STAGE_DRAIN), 1); /* resets */
  check("after reset running", kp_run_report_ok(&r), 0);

  /* Out-of-range start is rejected rather than clamped. */
  memset(&r, 0, sizeof(r));
  check("begin at done rejected", kp_run_begin(&r, KP_STAGE_DONE), 0);

  if (failures) {
    printf("%d case(s) failed\n", failures);
    return 1;
  }
  printf("all state cases passed\n");
  return 0;
}
