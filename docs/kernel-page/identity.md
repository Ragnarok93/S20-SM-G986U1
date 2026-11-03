# Per-object slab-page identity

Specification, evidence rules and the surface's ceiling:
[identity-table.md](identity-table.md). This document describes the lines as
implemented.

For every slot of the anchor burst the table carries: the object's slab page
(when the surface can prove one), whether it was allocated at the freeze and at
its own sample, and what its free did to the cache-wide counters. Cycle A
(`docs/kernel-page/logs/20261103_173750.log`) emitted 32 rows with one exact
page and 31 unresolved; the result is recorded in
[identity-table.md](identity-table.md#cycle-a-result-2026-11-03-window-f0cf5e9a-26855883). Where the
surface cannot answer, the line says `unavailable` with the reason; nothing is
filled in from an aggregate counter. Every row also carries the observation
window id, so a row can only belong to the cycle it was measured in.

## Sources

| value | source | kind |
| --- | --- | --- |
| `objsize`, `objperslab`, `pagesperslab` | `/proc/slabinfo`, `mm_struct` line | measured |
| slab span | `pagesperslab` × page size | derived from measured |
| one object address | KernelSnitch leak of the scanning child's `mm` | measured |
| task state at the freeze | `/proc/<pid>/stat` before any free | measured |
| task state before a row's free | `/proc/<pid>/stat` at that sample | measured |
| counter change of one free | `/proc/slabinfo` after that free | measured |
| page base, in-page offset, slot index | arithmetic on the address with the measured geometry | derived |

The geometry is read in-cycle and printed once:

```
KS_SURFACE geometry=measured objsize=1024 objs_per_slab=32 pages_per_slab=8
span=32768 span_src=derived(measured-pagesperslab*page) span_agrees=1
assumed_objs_per_slab=32 assumed_span=32768 assumes_agree=1 sysfs=denied
errno=13 per_object_address=unavailable reason=one-leak-one-address
geometry=1024/32/8/32768
```

`assumes_agree` compares the payload's own constants (`ORDER3_SIZE`,
`MM_STRUCT_SZ`) with the kernel's numbers and prints 0 if they diverge, so the
line is falsifiable rather than descriptive. Both `span_agrees` and
`assumes_agree` gate a page identity: a page is never derived from half a
geometry.

## What the surface does not supply

1. **An address for any object but one.** The leak walks candidates inside one
   process's `mm` and stops at the first whose futex hashes agree with that
   process's collision set (`src/kernelsnitch/kernelsnitch.h:283-300`). One
   scan yields one address; the other 31 slots would need 31 more scans
   (8–10 minutes each).
2. **Any per-page occupancy.** `/proc/slabinfo` aggregates the whole cache, and
   every per-cache attribute under `/sys/kernel/slab` is denied to the shell:
   `sysfs=denied errno=13` (EACCES on `object_size`; the rest of the directory
   is gated the same way).

A free on a page that is not on `n->partial` moves no counter at all
(`allocator_observation.c`), so a `d_active=0` row means "this free was not
visible", never "this object was elsewhere". The two readings a cache-wide
counter cannot separate are co-residency on a frozen page and a foreign full
page.

## The lines

`KS_ID_WINDOW` opens the table with the id every row repeats:

```
KS_ID_WINDOW window=b30aed65-282878665 boot=b30aed65
opened_mono_ms=282878665 slots=32 scope=cycle
window_src=measured(boot_id,clock_monotonic)
```

`KS_OBJ_ID` resolves the one address the run has:

```
KS_OBJ_ID window=… addr=ffffffc1f5e08400 addr_src=measured(leak)
addr_unique=unique geom=1024/32/8/32768 geom_src=measured(slabinfo)
page=ffffffc1f5e08000 page_src=derived(measured-span=32768) offset=1024
slot=1 slot_src=derived constructed_slot=1 match=1 page_identity=exact
```

`match` can come out false: the burst puts the leak child in the second slot of
the fresh page, so a correct resolution must land on slot 1 (offset 1024 of
32768). A different slab, or a span the kernel does not use, lands elsewhere —
`tests/kernel_page/identity_test.c` drives both cases. When a measured field
disagrees, the line prints `page=invalid(<check>) page_src=none` and no page.

`KS_ID_ROW` is one row per slot:

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

`role` is `scan` for the leak child (slot 1), `target` for the last slot (its
exit is the emptying free), `filler` otherwise. The two temporal fields are
separate claims: `allocated_at_freeze` comes from the pre-free sample of all 32
tasks, `allocated_at_sample` from the read immediately before that row's own
free. The target has no sample between the crossing and its free, so its row
reports `allocated_at_sample=unavailable` rather than borrowing the freeze
value. A task that had already exited reports
`free_observed=not-in-window`.

The addressed row carries the same `geom` revision it was derived with, plus
`offset=1024 slot_in_page=1 page_identity=exact` and
`page_src=derived(measured-span=32768)`; the source address is on the same
line, so the arithmetic can be re-done by hand.

`KS_ID_SUMMARY` closes the table:

```
KS_ID_SUMMARY window=… slots=32 rows=32 addressed=1 page_known=1 unresolved=31
invalid_geometry=0 duplicate_addresses=0 pages_observed=1
distinct_pages=unavailable(pages=1-of-32) objects_per_page=unavailable(incomplete)
co_residency=unavailable(reason=addressed-too-few) geom=1024/32/8/32768
geom_src=measured(slabinfo) occupancy=unavailable(no-page-resolved-source)
```

`rows` counts recorded rows, so an aborted run reports fewer rows instead of a
padded table. `unresolved` is `slots − page_known`, and `invalid_geometry`
counts rows that had an address but failed a geometry check. The grouping
numbers (`distinct_pages`, `objects_per_page`) are stated only when every slot
has an exact page; `pages_observed` is the partial view, labelled by its
`pages=<known>-of-<slots>` value.

## Cost, measured

`KS_ID_COST slots=31 loop_us=156717 sample_us=23774`: the 31 sampling reads
cost 23.8 ms (0.77 ms each) and the loop's remaining 133 ms is the 31
kill+reap calls that the bulk loop this replaces also paid. With the probe in
place the drain revealed the same 133 free objects, the control discarded on
the same mechanics, and the gate returned the same verdict as the
uninstrumented cycle, so at the resolution these counters have the probe does
not change the state it measures.

The one thing that does change is the window's length, which is why the
per-slot deltas are reported as one free's effect and never as a page's
occupancy. The freeze-time state sample adds no `/proc` traffic: it reuses the
reads the ownership check (`KS_SLAB_ALIVE`) already makes.
