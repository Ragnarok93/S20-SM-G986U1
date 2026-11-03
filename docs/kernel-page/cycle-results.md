# Full cycle — 2026-11-03 12:21 (commit `3a58c2b`)

Environment: `KS_FOPS_ROUTE=1 KS_PLANT_ALL=1`, cache valid for the boot, so the
address scan was skipped and the cycle reached the gate in about nine minutes
including setup. The device stayed up and reported no new fault.

Log: `logs/20261103_122125.log`.

## Verbatim evidence

```
[*] KS_PIN group=pre fillers=31 ready=0 cpu0=30 other=1 ok=0
[*] KS_PIN group=control fillers=31 ready=0 cpu0=31 other=0 ok=0
[*] KS_SEEDER pid=17843 slots=32 ready=1 forked=33 cpu=1 parent_cpu=0 empty_cpu=1
[*] KS_TARGET_TGID pre=1 pid=5790
[*] KS_EMPTY_PLAN cpu=1 expect=1
[*] KS_DRAIN pushes=188 pages=6 need=5 proven=1 cpu=0
[*] KS_QUIESCE active=2024 num=2208 slabs=69 stable=1 window=gate
[*] KS_FREE_CPU pid=5790 cpu=1 expect=1 ok=1
[*] KS_DRAIN d=5 pid=-1 cpu=-1->-1 pobjects=6 limit=6 cross=1 empty=1 stage=after_unfreeze
[*] KS_GATE span_ok=1 crossed_at=5 min_partial=5 empty_cpu=1 registered=1 discarded=0 image=3 gap_ms=0.056 d_empty=-1 d_num=0 d_slabs=0 pin_bad=1 slot_partial=0 preconditions=0
[*] KS_RESULT verified=address,anchor,transition unverified=reclamation,identity,consumption base=ffffffc228048000
[*] KS_TARGET_FREE waited_ms=20 iso=1310/1696/53 gate=2024/2208/69 post=2022/2176/68 crossed_at=5
[*] KS_EMPTY_FREE kind=target-exit cpu=1->1 draining=1 ok=1
[*] KS_DISCARD post active_objs=2022 num_objs=2176 slabdata=68
[!] KS_DELIVERY skipped=1 reason=gate-preconditions base=ffffffc228048000
```

(The `KS_TARGET_FREE` line above is shown in the form this pass produces; the
cycle printed the earlier single-arrow form, which compared the isolation
sample with the post-cleanup sample and read as one transition that never
happened. Only the text changed.)

## What the numbers say

1. **The emptying free reached the page.** `registered=1` with `d_empty=-1` is
   the exact signature of one object leaving a page that is on `n->partial`:
   a listed page contributes its `inuse` to `active_objs`, one live object
   becomes zero, `−1`. A frozen page would have moved nothing (`d_empty=0`);
   the CPU fastpath would not have touched `inuse` either.
2. **The page did not leave the cache in that window.** `discarded=0` with
   `d_num=0 d_slabs=0` means no page was returned to the allocator while the
   free ran.
3. **It left afterwards.** The post-cleanup sample is `2022/2176/68`, which is
   `−32/−1` from the gate baseline and `−2` active. So the page was discarded
   after the gate window closed, not by the free the gate watched.
4. **The two together identify a deferred discard.** In `__slab_free()` a
   listed page whose `inuse` reaches zero is discarded only when
   `n->nr_partial >= s->min_partial` at that instant. The drain proved six
   pages had been added (`pages=6 need=5 proven=1`), so the count fell below
   five between the crossing and the free — tens of milliseconds in which
   other CPUs can discard pages from the same list. The page stayed on the
   list with `inuse=0` and a later unfreeze removed it.

This is the first cycle in which "the free reached the page" and "the page
left the cache" are separately visible, which is exactly what the tightened
gate was built to expose.

## The second, independent cause of refusal

`pin_bad=1`. All three CPU facts about the emptying free itself are true
(`pre=1` so no siblings, `KS_FREE_CPU … ok=1`, `KS_EMPTY_FREE … ok=1`). The
mismatch is one **anchor filler**:

- `KS_PIN group=pre … cpu0=30 other=1 ok=0` — one of the 31 filler frees ran
  on a CPU other than the one the crossing drains, so its page parked on a
  list the crossing never walks.
- `ready=0` in both pin groups: the per-child handshake never reports, even
  though the fallback `/proc/<pid>/stat` check does. This is the same
  visibility gap reported in earlier passes; it is documented, not changed,
  because altering readiness gating would change timing.

`pin_bad` currently mixes anchor fillers, control fillers, sibling retirement
and the target's CPU. A control-filler mismatch can therefore refuse the
anchor's gate. Documented, not changed: separating them would widen the gate,
which this pass must not do.

## Reporting defect found by this cycle

No `KS_STATE` line appeared. The reason is in `src/kernelsnitch/utils.h`:
`pr_error(...)` expands to `exlog_writef(...); exit(-1);`. The refusal line is
the process exiting, so the lifecycle print placed after it was unreachable —
and so was every cleanup step, which is why 1,654 children were still alive
when the run ended. The state line is now printed **before** the fatal call and
records `cleanup=skipped`; the fatal exit itself is unchanged.

## Established, and not established

Established: address recovery accepted the saved answer for this boot; the
anchor burst and control burst are whole-slab (`span_ok=1`); the drain added
six pages; the baseline was quiesced; the emptying free reached the anchor
page's `inuse`; the discard was deferred.

Not established: that the image occupies the page; that the later removal was
an unfreeze rather than another free (inferred from the model, not measured);
that a repeat cycle behaves the same way.

## The measurement this points to

If deferred discard is the mechanism, holding `n->nr_partial` above
`min_partial` from the crossing through the free should turn `discarded` to
`1` in the same window with nothing else changed. That is an experiment
change, so it was deliberately not made here.
