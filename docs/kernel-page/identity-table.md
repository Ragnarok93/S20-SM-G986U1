# Per-object identity table — specification and emitted format

Status: **implemented** in `src/kernel_page/identity.c` and `identity.h`, and
emitted by one device cycle of the current route (**Cycle A** below,
`docs/kernel-page/logs/20261103_173750.log`). No allocator, gate or delivery
behaviour is part of this measurement.

## Why a table rather than a verdict

Each row is an independent claim about one object. A row may not inherit
confidence from its neighbour, from the run's other rows, or from the gate's
outcome. The table is the product: a reader must be able to check, row by row,
which page the object was on and what its free did — and see `unavailable`
where the surface cannot answer.

## Schema

| field | allowed values | class | how it is obtained |
| --- | --- | --- | --- |
| `window` | `<boot8>-<mono_ms>` | **M** | boot id + `CLOCK_MONOTONIC` at `kp_identity_window()` |
| `slot` | 0–31 | — | construction (allocation order in the burst) |
| `role` | `scan` (the leak child, slot 1), `target` (last slot, its exit is the emptying free), `filler` | — | construction order in `util.c` |
| `allocated_at_freeze` | `yes` / `no` / `unknown` | **M** | `/proc/<pid>/stat` read before any free (`kp_identity_freeze`) |
| `allocated_at_sample` | `yes` / `no` / `unavailable` | **M** | `/proc/<pid>/stat` read immediately before *this* row's free |
| `free_observed` | `yes` / `not-in-window` / `unavailable` | **M** | live at the sample and reaped by this row's kill / already exited / no observation |
| `object_address` | `0x…` or `unavailable(reason)` | **M** | a leak dump for *this* object, or the tracepoint `ptr` |
| `address_source` | `leak` / `tracepoint` / `none` | — | provenance of the address; a value without one is not admissible |
| `address_uniqueness` | `unique` / `repeated` / `none` | **M** vs rows | repeated if another row already carries the same address |
| `geom` | `objsize/objperslab/pagesperslab/span` or `unavailable` | **M**/**D** | geometry revision the page was derived with (`span` derived) |
| `page_address` | `0x…`, `invalid(<check>)`, or `unresolved(<reason>)` | **D** from **M** | `object_address & ~(span − 1)`, span from measured `pagesperslab` |
| `page_geometry_agrees` | encoded by `invalid(...)` | **M** vs **D** | `span_agrees` and `assumes_agree` from `KS_SURFACE` |
| `in_page_offset` | `0`–`span−1` or `unavailable` | **D** | `object_address & (span − 1)` |
| `counter_delta` | `d_active/d_num/d_slabs` or `unavailable` | **M** | `/proc/slabinfo` around the free; cache-wide |
| `delta_attribution` | `consistent-with` / `unavailable` | — | `consistent-with` is the only honest value for a cache-wide delta |
| `page_identity` | `exact` / `invalid(<check>)` / `unresolved(<reason>)` | — | `exact` only when `object_address` is measured and the geometry check passes |
| `page_occupancy_effect` | `unavailable(no-page-resolved-source)` | — | no page-resolved source exists (see below) |

`address_uniqueness=stale` is defined by the schema but has no emitting path:
an address is re-checked against this run's collision set before use, and a
failed check aborts before the table opens. `repeated` is the ambiguity the
field exists for, and every row sharing the address is marked.

## Rules

1. A row gets `page_identity=exact` only from a measured address for **that
   object** and the measured span. Never from a zero counter delta, never from
   the anchor's address, never from `slot × objsize` arithmetic.
2. `counter_delta` is cache-wide whatever the row says. `delta_attribution`
   must therefore be `consistent-with` unless the measurement window is proved
   exclusive.
3. `unavailable` always carries a reason token and is never rendered as `0`,
   `false`, `no effect` or a blank.
4. No co-residency verdict when fewer than two rows have a measured page.
   Below that the summary reports `unavailable(reason=addressed-too-few)` and
   prints `pages_observed=` separately from `distinct_pages=`.
5. Anchor and control rows are reported side by side, never merged: the control
   has no address and cannot borrow the anchor's.
6. A row whose geometry check fails is reported with `page_identity=invalid`
   and excluded from every page conclusion. The check is `span_agrees`
   (`objsize × objperslab == span`) **and** `assumes_agree` (measured fields
   equal the burst's own constants).
7. A derived page address keeps its source address (`addr=`), the geometry
   revision it was computed with (`geom=`), and its calculation provenance
   (`page_src=derived(measured-span=…)`) on the same row. A page address must
   never survive a failed geometry check, and it is never printed without those
   three fields.
8. `distinct_pages` and `objects_per_page` are stated only when every slot has
   a `page_identity=exact`; until then the summary says
   `distinct_pages=unavailable(pages=<known>-of-<slots>)`. `pages_observed` is
   the partial view and is labelled as such.
9. `allocated_at_freeze` and `allocated_at_sample` are different temporal
   claims and are never substituted for one another. The target's emptying free
   has no state sample between the crossing and the free, so its row reports
   `allocated_at_sample=unavailable` rather than reusing the freeze sample.

## Geometry and alignment (checked, not assumed)

The page base is `addr & ~(span − 1)`.  That is exact only under two conditions,
and the table checks both before it derives anything:

1. **`span` is a power of two.**  A slab of three pages has no mask that yields
   its base; such a geometry reports `page=invalid(non-power-of-two-span)`
   instead of masking with `span − 1`.  This device measures
   `pagesperslab=8`, so `span = 32768`, and an order-3 compound page is
   naturally aligned to that span — the mask recovers the compound head
   exactly.
2. **`objsize × objperslab == span`.**  A span that disagrees with its own
   object count means one of the three slabinfo fields is not what it claims;
   the row is invalid rather than resolved from the fields that happen to
   agree.

Both products are bounded at parse time (`pps <= LONG_MAX / PAGE_SIZE`,
`ops <= LONG_MAX / objsize`), so a crafted slabinfo line cannot overflow the
span or the agreement test.

The mask is tied to this cache's allocation path, not to compound pages in
general: `mm_cachep` objects come from SLUB's `alloc_slab_page()` →
`alloc_pages(order)`, with the order chosen by `calculate_order()` from
`slub_min_objects`/`slub_max_order` and capped by the architecture's maximum.
This tree measures `pagesperslab=8`, i.e. order 3; an order-*n* allocation is
`PAGE_SIZE << n` aligned, which is exactly the span the mask removes. A
different order would be measured as a different span and re-checked, not
assumed.

`identity_test.c` independently asserts the consequences on the resolved row:
`page % span == 0`, `offset < span`, and `0 ≤ slot_in_page < objperslab`.  A
derived slot outside the object count is reported as `match=0`; it is never
re-clamped into range.  Only the addressed row carries `page`, `offset` and
`slot_in_page`; every unresolved row prints them as `unavailable` with the
reason, so they cannot be read as identity, let alone occupancy.

## Emitted lines

One window header per run:

```
KS_ID_WINDOW window=b30aed65-282878665 boot=b30aed65 opened_mono_ms=282878665
slots=32 scope=cycle window_src=measured(boot_id,clock_monotonic)
```

The one measured address, resolved only when the geometry check passes:

```
KS_OBJ_ID window=… addr=ffffffc0a3af8400 addr_src=measured(leak)
addr_unique=unique geom=1024/32/8/32768 geom_src=measured(slabinfo)
page=ffffffc0a3af8000 page_src=derived(measured-span=32768) offset=1024
slot=1 slot_src=derived constructed_slot=1 match=1 page_identity=exact
```

and, when a measured field disagrees, `page=invalid(<check>)` with
`page_src=none` instead of a page.

One row per slot:

```
KS_ID_ROW window=… slot=0 role=filler allocated_at_freeze=yes
allocated_at_sample=yes free_observed=yes
addr=unavailable(no-per-object-address) addr_src=none addr_unique=none
geom=1024/32/8/32768 page=unresolved(no-object-address) page_src=none
offset=unavailable(no-object-address) slot_in_page=unavailable(no-object-address)
constructed_slot=1 match=unavailable page_identity=unresolved(no-object-address)
d_active=-1 d_num=0 d_slabs=0 d_src=measured
delta_attribution=consistent-with
```

The addressed row carries `addr=… page=… page_identity=exact` with the same
`geom` revision, `offset`, and `slot_in_page` it was derived with. A row whose
task had already exited reports `free_observed=not-in-window`; a row with no
sample reports `allocated_at_sample=unavailable`.

The summary closes the table:

```
KS_ID_SUMMARY window=… slots=32 rows=32 addressed=1 page_known=1 unresolved=31
invalid_geometry=0 duplicate_addresses=0 pages_observed=1
distinct_pages=unavailable(pages=1-of-32) objects_per_page=unavailable(incomplete)
co_residency=unavailable(reason=addressed-too-few) geom=1024/32/8/32768
geom_src=measured(slabinfo) occupancy=unavailable(no-page-resolved-source)
```

`rows` counts recorded rows; a run that dies early reports fewer rows rather
than padding the table. `unresolved` is `slots − page_known`; `invalid_geometry`
counts rows that had an address but failed the geometry check, so the two
failure kinds stay distinguishable.

## What the surface can supply (measured)

| surface | per-object address | per-page occupancy | status |
| --- | --- | --- | --- |
| `/proc/slabinfo` | no | no (cache-wide) | readable |
| `/sys/kernel/slab/mm_struct/*` | no | no (per-cache, not per-page) | `EACCES` |
| `/proc/kpageflags`, `/proc/kpagecount` | no | page flags only, no slab owner | `EACCES` |
| `kmem:kmem_cache_free` tracepoint | **yes**, `ptr` per free | no | enable/filter/id `EACCES`, `tracing_on=0` |
| `trace_pipe` | only enabled events | no | readable |
| KernelSnitch leak | yes, **one object per dump** | no | available, 8–10 min per dump |

So: per-object addresses are obtainable for exactly one object per leak dump;
per-page occupancy is obtainable from no surface at all. A cycle can therefore
prove which pages held objects, but never "this page's inuse was 1" — the
occupancy column stays `unavailable` even with a full address column.

## Routes to the missing 31 addresses

| route | yields | cost | precondition | verdict |
| --- | --- | --- | --- | --- |
| one leak dump per object | 31 more independent addresses | 31 × 8–10 min ≈ 4.1–5.2 h per cycle | 32 answer files (or one keyed table), each re-checked against its own collision set; all 32 children alive across the whole run | the only route available today; it is a different experiment, not a knob |
| privileged observer enabling `kmem_cache_free` | one `ptr` per free, with call sites | minutes | root or tracepoint access on the device; not available (`EACCES`, measured) | blocked; not planned around |
| sysfs per-page slab counters | nothing exists to read | — | root for the directory | cannot yield identity even with access |
| slot arithmetic from the anchor address | page of slot 0 only | free | — | forbidden by rule 1; measured geometry is the only input to a page |

The table accepts the per-slot addresses this route produces:
`kp_identity_object()` derives a page per slot with the same geometry checks
and prints `KS_OBJ_ADDR`, and the rows and summary use those pages instead of
the one anchor address. The host tests exercise both outcomes with synthetic
addresses (`co-resident` and `split`).

An opt-in collector now exists — see **Cycle B run mode** below — but it is
**experimental and not safe to run unattended**: the second two-slot attempt
left the device unreachable.

The leak route has a consequence worth stating before anyone builds it: the
scanning child must be alive for its dump, so 31 dumps serialize behind 31 live
children and the page cannot be emptied until the last dump finishes. The
"allocated at freeze" and "free observed" columns would then describe a burst
that has sat still for hours, not the 8–10 minute cycle. That is a separate run
mode with its own budget, and it must not be smuggled into the current cycle as
a flag.

## Acceptance criteria

**Cycle A — honest ceiling (current permissions).** One device cycle of the
existing route. 32 rows; exactly one `page_identity=exact`; 31
`unresolved(no-object-address)`; every row's `allocated_at_freeze`,
`allocated_at_sample` and `counter_delta` present; summary carries
`co_residency=unavailable(reason=addressed-too-few)`,
`distinct_pages=unavailable(pages=1-of-32)` and one window id that every row
shares; gate, allocator behaviour and delivery path unchanged; device alive.

**Cycle B — full table.** 32 measured addresses; `distinct_pages` and the
per-page grouping computed from them; `page_occupancy_effect` still
`unavailable`; only then a co-residency statement, and it replaces the
`6e8bf6b` interpretation rather than being merged with it.

Neither cycle may upgrade `inuse`, `prior`, `nr_partial` or the executed free
branch past unobserved: the table is object identity, not allocator state.

## Cycle A result (2026-11-03, window `f0cf5e9a-26855883`)

One page-probe cycle of the existing route. Log:
`docs/kernel-page/logs/20261103_173750.log`.

- 32 rows, all carrying the same `window=`, one line each; every slot reads
  `allocated_at_freeze=yes`.
- `addressed=1 page_known=1 unresolved=31 invalid_geometry=0
  duplicate_addresses=0 pages_observed=1`.
- The scan row: `addr=ffffffc255930400 page=ffffffc255930000 offset=1024
  slot_in_page=1 match=1 page_identity=exact`, `geom=1024/32/8/32768`.
- The target row: `role=target allocated_at_sample=unavailable
  free_observed=yes d_active=-1 d_num=0 d_slabs=0`.
- Summary: `distinct_pages=unavailable(pages=1-of-32)`,
  `co_residency=unavailable(reason=addressed-too-few)`.
- Gate refused (`registered=1 discarded=0 d_num=0 d_slabs=0`), the control page
  discarded (`d_num=-32 d_slabs=-1`), the 50 ms quiet window moved nothing, and
  the device stayed alive.

Same cycle, the control page was killed microseconds after the anchor's free:

| measurement | anchor row | control (`KS_CTL_TIGHT`) |
| --- | --- | --- |
| `d_active` | −1 | −1 |
| `d_num` | 0 | −32 |
| `d_slabs` | 0 | −1 |
| discard | no | yes |

What that comparison is: same cycle, same cache, same allocator configuration,
with the control's fillers frozen the same way. What it is not: proof of
equivalent page state. The control's discard proves that a page was reclaimed
on these mechanics; it does not establish that the anchor's page was empty, or
that the two frees met the same `slab_empty` decision. The quiet window
(0/0/0) bounds the claim to the measured interval, not to the state.

The one exact page is the scan object's own page. Nothing in this cycle says
the other 31 objects were on it, and nothing here measures occupancy.

## Cycle B run mode (`KS_ID_DUMPS`, experimental)

`KS_ID_DUMPS=N` makes slots 1…N run their own leak instead of the single leak
child.  The pipes are created before the burst so the fork sequence stays
back-to-back; each slot's child waits for a byte, runs
`kernelsnitch_setup()` of its own (the shared `ks` of the parent cannot be used
by serial scans), scans its own `mm`, writes the address back, pins to the
freeing CPU and pauses.  The parent signals one slot at a time, so exactly one
scan holds the cache; `KP_ID_DUMPS` was not run with more than two slots.

Measured, two attempts:

| run | slots | result | outcome |
| --- | --- | --- | --- |
| 2026-11-03 18:25 | 1, 2 | `ffffffc05adc0400` (offset 1024) and `ffffffc05adc0800` (offset 2048), both page `ffffffc05adc0000` | the cycle then aborted at the parent's leak re-check, which had no collision set because the scans ran in the children; handoff fixed |
| 2026-11-03 18:46 | 1, 2 | `ffffffc244738400` (offset 1024) and `ffffffc244738800` (offset 2048), both page `ffffffc244738000` | both scans completed; the cycle continued through the spray into the punch route and `payload_exit=0` at 19:05:02, then **the device went unreachable**.  The log does continue past the dumps — the earlier note that it ended there was wrong |
| 2026-11-03 20:02 | 1, 2 | `ffffffc011830400` (offset 1024) and `ffffffc011830800` (offset 2048), both page `ffffffc011830000` | collector validated (below); the cycle reached the plant and `consumer punch enter`, `payload_exit=0` at 20:20:00, then **the device went unreachable** |

Two independently scanned objects are on the same 32 KiB page at consecutive
slot offsets in all three runs.  That is the first per-object co-residency evidence
from a single burst, and it is consistent with the construction.  It is not a
32-row table: rows 0 and 3…31 have no address, so `co_residency` stays
`unavailable` in the summary.

The summary reports such pairs as `observed_same_page_pairs=`: two distinct
addresses on one page are one pair, and the field never becomes a co-residency
verdict while rows are missing.  A duplicate report — one address claimed by
two slots — is refused before it can be counted as a pair, and counted in
`duplicate_reports=` instead.

The pair arithmetic counts one object per unique address: `k(k−1)/2` for `k`
distinct addresses on one page, never rows.  `page_known` still counts rows
with an exact page, so a complete table that somehow carries one duplicate
reads `page_known=32 objects_per_page=31 observed_same_page_pairs=465`.

## Device validation (2026-11-03 20:02)

One two-slot cycle on the frozen build (`sha256=3a80a4b3…`) is the first device
run of the collector with the guarded cleanup.  Log:
[`logs/20261103_200234.log`](logs/20261103_200234.log).

| measured line | what it shows |
| --- | --- |
| `KS_ID_DUMP_REPORT slot=1 … result=written bytes=40 of=40` | the whole record was transferred |
| `KS_ID_DUMP slot=1 addr=ffffffc011830400 … verdict=accepted` | admitted; page `ffffffc011830000`, offset 1024 |
| `KS_ID_DUMP_CLEANUP slot=1 result=released` | scan state released before parking |
| slot 2 `…830800` (offset 2048), same page | `result=written bytes=40 of=40`, admitted, `result=released` |
| `KS_ID_DUMP_DONE slots=2 launched=2 reported=2 rejected=0 missing=0 alive=2 gone=0 reaped=0 open_pipes=0 accounting=ok` | the counts reconcile and both reporters held their slots at the sample |
| `KS_ID_SUMMARY … observed_same_page_pairs=1 co_residency=unavailable` | one pair, no co-residency verdict |

The DONE line was printed before slot 2's `KS_ID_DUMP_CLEANUP` arrived, and it
sampled slot 2 while it was still releasing (`state=R`).  That is the
point-in-time caveat in action: `alive=` says the child held its slot at that
instant, not that it stayed alive, and `accounting=ok` says the counters
reconcile, not that the device survived — which it did not.

The cycle then ran the page probe (`KS_PAGE_PROBE base=ffffffc011830000`), the
spray with matching readback (`KS_PAGE_PEEK … skb_match=1`), the plant
(`KS_PLANT_LANES … plant … hits=1 mismatch=0`) and `consumer punch enter`
before `payload_exit=0`; `adb` then refused and the USB device disappeared.

### What the loss is not

The earlier reading — that the paused dump children's mapped scan state caused
the loss — is not supported.  In this run both children released their scans,
both were alive at DONE with `open_pipes=0`, and the payload continued for
another ~18 minutes through the spray and punch before the device dropped.
Both full two-slot cycles (18:46 and 20:02) ended the same way, after
`payload_exit=0`; the aborted 18:25 attempt, which never reached the punch,
left the device alive.  The association is with the punch stage; the cause is
not established.

## Collector contract after the hardening (host-verified, not re-run on device)

Since that run the handset has stayed unreachable (`adb` refuses, no Samsung
USB device), so nothing below has been exercised on the device.

- **The report is one validated record.**  A dump child writes
  `mm=<16 hex> accepted=<n> state=<n>\n`.  The parent parses it strictly
  (`kp_dump_report_parse()`) and admits it (`kp_dump_admit()`) only when the
  child's own scan ended in `MM_FOUND`, accepted at least 8 collisions (the
  bar the single-scan gate applies), named a non-zero address, and the measured
  geometry resolves that address.  Each refusal has its own reason token —
  `no-report`, `truncated-report`, `malformed-report`, `scan-state-not-found`,
  `zero-address`, `collision-set-too-small`, `duplicate-address`,
  `geometry-check-failed` — and none of them touches the table.
- **Scan state is released before parking.**  The child stays alive because its
  `mm` is the slab object, but it tears the scan's mappings down once its state
  is terminal and logs `KS_ID_DUMP_CLEANUP result=released`; a non-terminal
  state keeps its mappings and logs `result=skipped reason=not-terminal`
  instead of tripping the assert inside `kernelsnitch_cleanup()`.
- **The mode is bounded.**  `KS_ID_DUMPS` is capped at 2 slots with a warning.
  Each slot's wait is one 900 s deadline (`kp_dump_read_report()`: a monotonic
  deadline, not a count of poll() timeouts), and the reader closes the report
  pipe on every path, so `open_pipes` stays 0 even when a child dies mid-line.
  The children carry `PR_SET_PDEATHSIG` so a dead parent does not leave them.
- **A report and its reporter are logged separately.**  The child writes the
  record before any cleanup and logs `KS_ID_DUMP_REPORT result=written
  bytes=<n> of=<n>` only when the whole record was transferred; a short or
  interrupted write is `write-failed` with its byte count, and the parent then
  refuses the short line as `truncated-report`.  (The record is ~42 bytes, far
  below `PIPE_BUF`, so one pipe write is atomic; the loop is defensive.)  The
  parent logs its own `verdict=accepted|rejected`, then `KS_ID_DUMP_ALIVE` per
  slot.  A child that dies during cleanup still has a written report — the
  liveness line is what says its slot no longer holds the object.
- **Ownership is the burst's, not the collector's.**  Every dump child is an
  entry in the burst's `pre_ctx.childs`, so the existing freeze and teardown
  kill it; the collector adds no second owner.  It reaps only a child that is
  already dead (`WNOHANG`) and never touches a live one, because a live child
  *is* the slab object under test.
- **The DONE counts must reconcile.**  `KS_ID_DUMP_DONE` carries `slots=
  launched= reported= rejected= missing= alive= gone= reaped= open_pipes=
  accounting=ok|mismatch`, with `reported + rejected + missing = slots` and
  `alive + gone = launched`.  A child that never reported is `missing`, never
  `rejected`; a launched child whose liveness was not observed makes the line
  `mismatch` instead of reading clean.
- **A duplicate is not an identity.**  A report whose address another slot
  already owns is refused and counted in `duplicate_reports=`, never folded
  into `observed_same_page_pairs=`.

Host checks: 12 malformed report forms refused; the eight refusal paths above
asserted in `tests/kernel_page/identity_test.c`; the read path driven over real
pipes (full line, mid-line EOF, no report, no writer, bad descriptor or
capacity) with the descriptor closed after every case and the empty wait
returning inside its 50 ms deadline; pair arithmetic checked at 32 distinct
(496), 31 distinct over a duplicate row (465) and the measured two-object table
(1); the accounting rule on consistent, missing-report, unobserved-child and
impossible-count sets.  The host suite and the payload build pass with 0
warnings (`sha256=3a80a4b3…`).  Still required before any
32-slot attempt: a two-slot run that finishes with the handset alive.  The
release path, the pipe accounting and the child-death path remain device-only
observations.

## Whether Cycle B is worth building

**Decision: defer.** Build the 32-dump run only if the next engineering step
turns on whether the burst shares one slab page.

- It would answer co-residency, which would revise `6e8bf6b`'s inference (that
  inference came from a drain delta, not from per-object addresses).
- It would not answer `page->inuse`, `n->nr_partial`, `prior`, or the retention
  reason. Occupancy stays `unavailable` even with 32 addresses.
- It costs 31 leak dumps at 8–10 minutes each plus the cycle, roughly 4–5 hours
  with all 32 children alive across the series — a different run mode, not a
  flag on this one.

Criterion to start it: a following experiment whose branch depends on "one page
or several". If the retention question can be attacked without the grouping —
as the same-cycle control differential now allows — those hours buy no
decision.

One consequence for the code: the complete-table grouping branch
(`distinct_pages`, `objects_per_page`, `co_residency=co-resident/split`) cannot
be reached while one address exists per run. It is deliberately unexercised
here, and Cycle B must bring the regression that covers it.
