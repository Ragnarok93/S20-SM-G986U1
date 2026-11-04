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
