/* Host test for src/kernel_page/gate.c.
 *
 * Each case is one instant a cycle can actually produce, with the meaning the
 * kernel source gives it.  Run: cc -std=c11 -Wall -Wextra -Werror
 * -I src src/kernel_page/gate.c tests/kernel_page/gate_test.c -o build/host/gate_test
 */
#include <stdio.h>
#include <string.h>

#include "kernel_page/gate.h"

static int failures;

static kp_gate_facts facts_ok(void) {
  kp_gate_facts f;
  f.span_ok = 1;
  f.target_freed = 1;
  f.empty_cpu = 1;
  f.want_cpu = 1;
  f.pin_bad = 0;
  f.slot_partial = 0;
  f.objects_per_slab = 32;
  return f;
}

static kp_sample sample(long active, long num, long slabs) {
  kp_sample s;
  s.active = active;
  s.num = num;
  s.slabs = slabs;
  return s;
}

static void expect(const char *name, const kp_gate_verdict *v, int registered,
                   int discarded, int targeted) {
  int ok = v->registered == registered && v->discarded == discarded &&
           v->targeted == targeted;
  if (!ok) {
    failures++;
    printf("FAIL %s: got registered=%d discarded=%d targeted=%d, want %d/%d/%d\n",
           name, v->registered, v->discarded, v->targeted, registered,
           discarded, targeted);
    return;
  }
  printf("ok   %s (d=%ld/%ld/%ld)\n", name, v->d_active, v->d_num, v->d_slabs);
}

int main(void) {
  kp_gate_facts f = facts_ok();
  kp_gate_verdict v;

  /* 1. The result the chain needs: the page held one live object, the free
   *    landed on it, and the page left the cache. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(999, 3168, 99);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("listed page with one object, emptied", &v, 1, 1, 1);
  }

  /* 2. The free ran on a frozen page: FREE_FROZEN moves nothing.  The discard
   *    would happen at the next unfreeze, not here. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(1000, 3200, 100);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("frozen page (FREE_FROZEN)", &v, 0, 0, 0);
  }

  /* 3. The free registered but nothing was removed: it landed on a listed page
   *    that still has live objects, i.e. not the page the burst built. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(999, 3200, 100);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("free on a foreign listed page", &v, 1, 0, 0);
  }

  /* 4. One extra object moved in the window as well: the measured -2 case.
   *    A discard plus one unrelated free on a listed page reads -2/-32/-1 and
   *    must NOT pass, because the arithmetic no longer isolates one object. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(998, 3168, 99);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("discard plus one foreign object", &v, 0, 1, 0);
  }

  /* 5. An empty page discarded while it was never listed: all 32 objects were
   *    counted active, so the drop is -32 with the slab gone. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(968, 3168, 99);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("empty unlisted page discarded", &v, 0, 1, 0);
  }

  /* 6. The right arithmetic on the wrong CPU is not a delivery. */
  memset(&v, 0, sizeof(v));
  {
    kp_gate_facts bad = facts_ok();
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(999, 3168, 99);
    bad.empty_cpu = 0;
    kp_gate_evaluate(&bad, &before, &after, &v);
    expect("discard on the wrong CPU", &v, 1, 1, 0);
  }

  /* 7. A burst slot that came from a foreign partial slab voids the gate even
   *    when every counter agrees. */
  memset(&v, 0, sizeof(v));
  {
    kp_gate_facts bad = facts_ok();
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(999, 3168, 99);
    bad.slot_partial = 1;
    kp_gate_evaluate(&bad, &before, &after, &v);
    expect("slot not exclusive", &v, 1, 1, 0);
  }

  /* 8. Unreadable samples (-1) must never look like an event. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(-1, -1, -1);
    kp_sample after = sample(-1, -1, -1);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("unreadable samples", &v, 0, 0, 0);
  }

  /* 9. A two-page discard is not one page. */
  memset(&v, 0, sizeof(v));
  {
    kp_sample before = sample(1000, 3200, 100);
    kp_sample after = sample(998, 3136, 98);
    kp_gate_evaluate(&f, &before, &after, &v);
    expect("two slabs left the cache", &v, 0, 0, 0);
  }

  if (failures) {
    printf("%d case(s) failed\n", failures);
    return 1;
  }
  printf("all gate cases passed\n");
  return 0;
}
