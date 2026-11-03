# Per-object slab-page identity

Specification, evidence rules and the surface's ceiling:
[identity-table.md](identity-table.md). This document describes the lines as
implemented.

What this pass measures, for every slot of the anchor burst: the object's slab
page, whether it was allocated at the freeze, and what its free did to that
page's occupancy. Where the surface cannot answer, the line says
`unavailable` with the reason; nothing is filled in from an aggregate counter.

## Sources

| value | source | kind |
| --- | --- | --- |
| `objsize`, `objperslab`, `pagesperslab` | `/proc/slabinfo`, `mm_struct` line | measured |
| slab span | `pagesperslab` × page size | derived from measured |
| one object address | KernelSnitch leak of the scanning child's `mm` | measured |
| task state before a free | `/proc/<pid>/stat` | measured |
| counter change of one free | `/proc/slabinfo` after that free | measured |
| page base, in-page offset, slot index | arithmetic on the address with the measured geometry | derived |

The geometry is read in-cycle and printed once:

```
KS_SURFACE geometry=measured objsize=1024 objs_per_slab=32 pages_per_slab=8
span=32768 span_src=derived(measured-pagesperslab*page) span_agrees=1
assumed_objs_per_slab=32 assumed_span=32768 assumes_agree=1 sysfs=denied
errno=13 per_object_address=unavailable reason=one-leak-one-address
```

`assumes_agree` compares the payload's own constants (`ORDER3_SIZE`,
`MM_STRUCT_SZ`) with the kernel's numbers and prints 0 if they diverge, so the
line is falsifiable rather than descriptive.

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

`KS_OBJ_ID` resolves the one address the run has:

```
KS_OBJ_ID addr=ffffffc1f5e08400 addr_src=measured(leak) page=ffffffc1f5e08000
page_src=derived(measured-span=32768) offset=1024 slot=1 slot_src=derived
constructed_slot=1 match=1
```

`match` can come out false: the burst puts the leak child in the second slot of
the fresh page, so a correct resolution must land on slot 1 (offset 1024 of
32768). A different slab, or a span the kernel does not use, lands elsewhere —
`tests/kernel_page/identity_test.c` drives both cases.

`KS_ID_SLOT` is one row per slot:

```
KS_ID_SLOT slot=0 alive=yes d_src=measured d_active=0 d_num=0 d_slabs=0
effect=no-counter-moved effect_src=derived page=unavailable page_src=none
reason=no-per-object-address
```

and for the one addressed slot the same row carries `addr=… page=…` with
`addr_src=measured(leak)` and `page_src=derived(measured-geometry)`.

`KS_ID_SUMMARY` closes the table with how many objects got an address and why
the rest did not.

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
occupancy.
