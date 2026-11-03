# Full cycles and what they establish

Two cycles were run on the committed build. The second one corrects the
reading of the first, which is why both are recorded here.

## Cycle 1 — 2026-11-03 12:21 (`3a58c2b`)

```
[*] KS_DRAIN pushes=188 pages=6 need=5 proven=1 cpu=0
[*] KS_QUIESCE active=2024 num=2208 slabs=69 stable=1 window=gate
[*] KS_GATE … registered=1 discarded=0 … d_empty=-1 d_num=0 d_slabs=0 pin_bad=1 preconditions=0
[*] KS_CTL_FREE num=2208->2176 slabs=69->68 d_num=-32 d_slabs=-1 discarded=1
[*] KS_TARGET_FREE … gate=2024/2208/69 post=2022/2176/68
```

Initial reading: the emptying free registered on the anchor page (`-1`) but the
page did not leave during the gate window, and the post sample showed one page
gone — which looked like a *deferred* discard. The control's line was not taken
into account, and it changes the conclusion (see cycle 2).

Log: `logs/20261103_122125.log`.

## Cycle 2 — 2026-11-03 12:54 (`bfdee77` + quiet-window sample)

The only difference from cycle 1 is a diagnostic sample taken 50 ms after the
gate's sample and **before** the control's last child is killed. Nothing is
allocated or freed in that window, and no existing sample moved.

```
[*] KS_QUIESCE active=2137 num=2368 slabs=74 stable=1 window=gate
[*] KS_SETTLE ms=50 active=2136 num=2368 slabs=74 d_from_empty=0/0/0
[*] KS_CTL_FREE num=2368->2336 slabs=74->73 d_num=-32 d_slabs=-1 discarded=1
[*] KS_GATE span_ok=1 crossed_at=5 min_partial=5 empty_cpu=1 registered=1 discarded=0 image=3 gap_ms=0.059 d_empty=-1 d_num=0 d_slabs=0 pin_bad=1 slot_partial=0 preconditions=0
[*] KS_TARGET_FREE waited_ms=20 iso=1374/1664/52 gate=2137/2368/74 post=2135/2336/73 crossed_at=5
[*] KS_STATE stage=gate outcome=refused samples=2 missing=0 report=1 cleanup=skipped refusal=gate-preconditions base=ffffffc226b98000
```

### What this settles

1. **The page did not leave during the quiet window.** `d_from_empty=0/0/0`
   over 50 ms: no slab disappeared between the gate's sample and the control's
   kill.
2. **The only page that left did so at the control's free.** `2368→2336`,
   `74→73`, and the final sample is unchanged from that point.
3. **Therefore the page seen in cycle 1's control window was the control's
   page, not the anchor's.** In cycle 2 the same line appears with the quiet
   window in between, so the attribution is no longer ambiguous: the two
   candidate pages are separated by 50 ms of flat counters.
4. **The anchor's page is still in the cache, empty.** A page left on
   `n->partial` with `inuse == 0` has no live object, so no later free can
   discard it; `unfreeze_partials()` walks the per-CPU list, not the node list.
   The only ways it leaves are an allocation that reuses it (and a later exit
   that empties it again) or a future free that empties it — impossible here.
5. **The precondition is a race, not a property of the construction.** The
   anchor's free found `n->nr_partial < s->min_partial`; the control's free,
   milliseconds later, found it at or above the limit and discarded. Same
   mechanism, same drain, different instant.

`registered=1` still holds in both cycles: the free that emptied the anchor
page is the free the counters saw. What failed is only the count at that
instant.

`pin_bad=1` in cycle 2 again comes from an anchor filler (`KS_PIN group=pre …
other=1` in the log); the target's own CPU facts are all true.

## The correction

Cycle 1's `-32/-1` was **not** a deferred discard of the anchor page. It was
the control slab's page being discarded, and the anchor's page stayed. The
earlier document said the opposite; this is the measured replacement.

## Consequence for the next experiment

"Hold `nr_partial` through the free" is still the right hypothesis, but the
target is narrower than "add more pages": the control proves the count can be
at or above the limit microseconds later, so the process is oscillating around
`min_partial` at millisecond granularity. Any hold must therefore cover the
anchor's free *specifically*, and the control's own result can no longer be
used as evidence that the anchor's page discarded.

## Still not established

- That the anchor page was reused or discarded after the run ended.
- That the control page is the one that left (it is the only page we emptied
  in that window, and the counters moved at that kill, but the counters cannot
  name the page).
- Placement, and everything that depends on it.
