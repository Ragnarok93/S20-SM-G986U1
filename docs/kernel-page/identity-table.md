# Per-object identity table — specification

Status: **specification only.** No code change and no device cycle in this
commit. The current `KS_OBJ_ID` / `KS_ID_SLOT` / `KS_ID_SUMMARY` lines already
emit part of this schema; the gap analysis at the end says which parts are
missing and what each route to close them costs.

## Why a table rather than a verdict

Each row is an independent claim about one object. A row may not inherit
confidence from its neighbour, from the run's other rows, or from the gate's
outcome. The table is the product: a reader must be able to check, row by row,
which page the object was on and what its free did — and see `unavailable`
where the surface cannot answer.

## Schema

| field | allowed values | class | how it is obtained |
| --- | --- | --- | --- |
| `slot` | 0–31 | — | construction (allocation order in the burst) |
| `role` | `anchor` (slot 0) / `filler` (1–31) | — | construction |
| `object_address` | `0x…` or `unavailable(<reason>)` | **M** | a leak dump for *this* object, or the tracepoint `ptr` |
| `address_source` | `leak` / `tracepoint` / `none` | — | provenance of the address; a value without one is not admissible |
| `page_address` | `0x…` or `unresolved(<reason>)` | **D** from **M** | `object_address & ~(span − 1)`, span from measured `pagesperslab` |
| `page_geometry_agrees` | `0` / `1` | **M** vs **D** | `span_agrees`, `assumes_agree` from `KS_SURFACE` |
| `in_page_offset` | `0`–`span−1` or `unavailable` | **D** | `object_address & (span − 1)` |
| `allocated_at_freeze` | `yes` / `no` / `unknown` | **M** | `/proc/<pid>/stat` before the free |
| `free_observed` | `yes` / `no` / `unavailable` | **M** | `/proc/<pid>/stat` after the free |
| `counter_delta` | `(d_active, d_num, d_slabs)` or `unavailable` | **M** | `/proc/slabinfo` around the free; cache-wide |
| `delta_attribution` | `direct` / `consistent-with` / `unavailable` | — | project rule; `consistent-with` is the only honest value for a cache-wide delta |
| `page_identity` | `exact` / `unresolved(<reason>)` | — | `exact` only when `object_address` is measured |
| `page_occupancy_effect` | `unavailable(<reason>)` | — | no page-resolved source exists (see below) |

Example of the one row the current surface can fully populate:

```
slot=1 role=filler object_address=0xffffffc1f5e08400 address_source=leak
page_address=0xffffffc1f5e08000 page_geometry_agrees=1 in_page_offset=1024
allocated_at_freeze=yes free_observed=yes counter_delta=(-1,0,0)
delta_attribution=consistent-with page_identity=exact
page_occupancy_effect=unavailable(no-page-resolved-source)
```

and of the other 31:

```
slot=0 role=anchor object_address=unavailable(no-per-object-address)
address_source=none page_address=unresolved(no-object-address)
allocated_at_freeze=yes free_observed=yes counter_delta=(0,0,0)
delta_attribution=consistent-with page_identity=unresolved(no-object-address)
page_occupancy_effect=unavailable(no-page-resolved-source)
```

## Rules

1. A row gets `page_identity=exact` only from a measured address for **that
   object** and the measured span. Never from a zero counter delta, never from
   the anchor's address, never from `slot × objsize` arithmetic. The previous
   milestone's mistake was exactly this inference, and it is now a rule.
2. `counter_delta` is cache-wide whatever the row says. `delta_attribution`
   must therefore be `consistent-with` unless the measurement window is proved
   exclusive; `direct` exists for a future window that is.
3. `unavailable` always carries a reason token and is never rendered as `0`,
   `false`, `no effect` or a blank.
4. No co-residency verdict when fewer than two rows have a measured address.
   The summary reports `addressed=`, `page_known=`, `distinct_pages=` and
   `co_residency=unavailable(<reason>)` in that case.
5. Anchor and control rows are reported side by side, never merged: the control
   has no address and cannot borrow the anchor's.
6. A row whose `page_geometry_agrees=0` is reported but excluded from any page
   conclusion; the geometry check is the only falsifier in the table.

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
| slot arithmetic from the anchor address | page of slot 0 only | free | — | valid for one row; the rule above forbids extending it |

The leak route has a consequence worth stating before anyone builds it: the
scanning child must be alive for its dump, so 31 dumps serialize behind 31 live
children and the page cannot be emptied until the last dump finishes. The
"allocated at freeze" and "free observed" columns would then describe a burst
that has sat still for hours, not the 8–10 minute cycle. That is a separate run
mode with its own budget, and it must not be smuggled into the current cycle as
a flag.

## Acceptance criteria

**Cycle A — honest ceiling (current permissions).** 32 rows; exactly one
`page_identity=exact`; 31 `unresolved(no-object-address)`; every row's
`allocated_at_freeze`, `free_observed` and `counter_delta` measured; summary
carries `co_residency=unavailable(reason=one-address)`; gate, allocator
behaviour and delivery path unchanged; device alive.

**Cycle B — full table.** 32 measured addresses; `distinct_pages` and the
per-page grouping computed from them; `page_occupancy_effect` still
`unavailable`; only then a co-residency statement, and it replaces the
`6e8bf6b` interpretation rather than being merged with it.

## Gap against the current emission

| requirement | current line | gap |
| --- | --- | --- |
| per-row address and provenance | `addr=`, `addr_src=` on one row only | none for the ceiling; the other 31 already say `unavailable` |
| role | `slot=` only | add `role=` (anchor vs filler) |
| page and offset | `page=`, `page_src=` | present |
| allocated/free observed | `alive=` | keep, but `alive=zombie` must not read as "free observed" |
| counter delta | `d_active/d_num/d_slabs` | present, cache-wide, labelled |
| attribution | `effect=`, `effect_src=derived` | rename to the schema's `delta_attribution` vocabulary |
| summary | `slots=32 addressed=1 page_known=1 unavailable=31 reason=… effect_scope=cache-wide` | add `distinct_pages=`, `co_residency=` |

None of these gaps is an allocator or gate change; they are field names and one
summary counter. They are listed here so the next pass implements the schema
once, instead of inventing another line format per experiment.
