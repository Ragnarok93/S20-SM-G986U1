# Allocator accounting audit (SM-G986U1, 4.19.113)

Source of truth: `~/kernel_samsung_sm8250` (`VERSION/PATCHLEVEL/SUBLEVEL = 4/19/113`),
the SM8250 tree for this device. Every claim below cites `mm/slub.c` lines from
that tree, not from memory of upstream.

## What each counter is

`get_slabinfo()` (mm/slub.c:6191) under `CONFIG_SLUB_DEBUG`:

```c
for_each_kmem_cache_node(s, node, n) {
        nr_slabs += node_nr_slabs(n);
        nr_objs  += node_nr_objs(n);
        nr_free  += count_partial(n, count_free);
}
sinfo->active_objs = nr_objs - nr_free;
sinfo->num_objs    = nr_objs;
sinfo->num_slabs   = nr_slabs;
```

with

| helper | definition | line | scope |
| --- | --- | --- | --- |
| `node_nr_slabs(n)` | `atomic_long_read(&n->nr_slabs)` | 1117 | **all** slabs of the cache |
| `node_nr_objs(n)` | `atomic_long_read(&n->total_objects)` | 2627 | **all** objects of the cache |
| `count_free(page)` | `page->objects - page->inuse` | 2622 | per page |
| `count_partial(n, …)` | sum over `n->partial` | 2634 | **node partial list only** |

So in `/proc/slabinfo` for `mm_struct` on this kernel:

- `slabdata` = total slabs in the cache (full, partial, frozen, active).
- `num_objs` = total objects in those slabs.
- `active_objs` = `num_objs` − free objects **that sit on the node partial list**.

Free objects on a page frozen on a CPU's `c->partial`, on the CPU's active
page, or on any page not currently listed are **not** subtracted, i.e. they are
counted as active. This is why a sample can read `active_objs == num_objs`
(e.g. `KS_FREEZE_SAMPLE active=2432 num=2432 slabs=76`): at that instant no
free object was visible on the node list at all.

The earlier userspace model assumed all three counters described the node
partial list. They do not. That single mistake explains most of the confusing
deltas in the logs.

## What the free path actually does

`__slab_free()` (mm/slub.c:3083):

- `prior = page->freelist`, `was_frozen = page->frozen`.
- If `(!new.inuse || !prior) && !was_frozen`:
  - `!prior` (page was full) and the cache has CPU partial → `new.frozen = 1`,
    and after the cmpxchg `put_cpu_partial(s, page, 1)` parks it on `c->partial`.
    **No node-list accounting happens.**
  - otherwise → take `n->list_lock`; if `!new.inuse && n->nr_partial >= s->min_partial`
    → `slab_empty` (remove + `discard_slab`); if the page was not on the list
    before (`!prior`) it is added.
- If `was_frozen` → `FREE_FROZEN`, no list activity at all.

`unfreeze_partials()` (mm/slub.c:2424) walks `c->partial` **head first**, i.e.
newest page first:

- `!new.inuse && n->nr_partial >= s->min_partial` → queue for `discard_slab`;
- else → `add_partial(n, page, DEACTIVATE_TO_TAIL)`, which increments
  `n->nr_partial`.

`put_cpu_partial()` (mm/slub.c:2492): drains **before** pushing the new page,
when the existing head's accumulated `pobjects` exceeds `s->cpu_partial`;
`pobjects += page->objects - page->inuse` at push time.

## Consequences for the gate fields

| field | what it actually establishes | what it does **not** establish |
| --- | --- | --- |
| `d_empty` | the freeing object landed on a page that was on `n->partial` at sample time (its free lowered `nr_free` by one) | nothing about the discard; a page not on the list gives 0 |
| `d_num = -32` | one slab's worth of objects left the cache (`nr_slabs` and `total_objects` both fell) | which slab, or that it was this slab |
| `d_slabs = -1` | the cache's total slab count fell by one | `n->nr_partial` at any moment |
| `crossed_at = k` | k page-adds happened during the crossing (each added page with 1 live object drops `active_objs` by 31) | that `n->nr_partial >= 5` persisted until the emptying free |
| `slabdata` absolute value | total slabs in the cache | **not** `n->nr_partial`; it cannot be used to read the `min_partial` precondition |

Two arithmetic facts worth stating because the payload used to assume otherwise:

1. A page on `n->partial` contributes **exactly its `inuse`** to
   `active_objs`; a page on no list contributes all of its objects. So
   **adding** a listed page with `inuse=1` changes `active_objs` by **−31**
   (its 31 free objects become visible), not by +1, and freezing that same
   page moves nothing at all.
2. Discarding a page whose `inuse` was `k` therefore reads `d_active = −k`,
   `d_num = −32`, `d_slabs = −1`. The anchor's page (one live object) gives
   `−1/−32/−1`; an empty unlisted page gives `−32/−32/−1`. The two are
   distinguishable, which is what the gate's new `registered` requirement
   uses.

So the emptying free, when it lands on a listed page and the page is then
discarded, gives exactly `d_empty=-1, d_num=-32, d_slabs=-1`; and when the page
is frozen at the free and discarded later, it gives `d_empty=0` at the free and
`−32/−1` at the later discard. Both are consistent with the source.

## The anchor/control discrepancy, restated

Observed in one cycle:

```
KS_DRAIN pushes=170 pages=5 need=5 proven=1 cpu=0
KS_DRAIN_BASE active=2074 num=2272 slabs=71
KS_CTL_FREE  num=2272->2272 slabs=71->71  d_num=0   d_slabs=0  discarded=0
KS_GATE ...  d_empty=-2 d_num=-32 d_slabs=-1 discarded=1
```

and in a later cycle:

```
KS_QUIESCE active=2306 num=2432 slabs=76 stable=1
KS_GATE    ... d_empty=-1 d_num=0 d_slabs=0 discarded=0
KS_CTL_FREE num=2432->2400 slabs=76->75 d_num=-32 d_slabs=-1 discarded=1
```

These are not two different allocator behaviours for identical inputs. They are
the same behaviour sampled at different instants:

- The control "refusing" in cycle 1 is a control whose free landed on a page
  that was still frozen; the discard showed up later (as cycle 2 shows for the
  control).
- The anchor "discarding" in cycle 1 (`d_num=-32`) and not in cycle 2
  (`d_num=0`) differs by whether `n->nr_partial` was at or above
  `s->min_partial` **at the free** — a value neither cycle measured, and which
  no counter in `/proc/slabinfo` exposes.
- The `crossed_at=5` proof is a proof about the crossing instant: five pages
  were added to `n->partial` then. Between that instant and the emptying free,
  unrelated frees on other CPUs can discard pages from that same list and take
  the count back below 5. That is the whole discrepancy.

## Device-side confirmation

`/proc/slabinfo` itself reports the geometry the payload assumes:

```
mm_struct            620    768   1024   32    8 : tunables    0    0    0 : slabdata     24     24      0
```

`objsize` is `s->size` (mm/slab_common.c:1344 prints it directly), so
`mm_objs_per_slab = 32768/1024 = 32` and the order is 3 — measured, not
assumed. The `tunables` triplet is all zeros because this build's
`slabinfo_show_stats()` is empty; it carries no limits.

The sysfs attributes that would expose the limits are denied to the `shell`
user (measured):

```
cat: /sys/kernel/slab/mm_struct/min_partial: Permission denied
cat: /sys/kernel/slab/mm_struct/cpu_partial: Permission denied
```

so `min_partial`/`cpu_partial` remain derived from `s->size` by the vendor
formulae (`set_min_partial`: `ilog2(size)/2` clamped to [5,10] → 5;
`set_cpu_partial`: `size >= 1024` → 6) rather than read.

## Conclusion of the audit

1. The counters are now understood and documented against this kernel; the
   comments that claimed "only node-partial pages are counted" were wrong and
   have been replaced in `src/kernel_page.h` and
   `src/kernel_page/allocator_observation.c`.
2. The count can be read indirectly, at 32-object granularity, with a page of
   the same construction freed within milliseconds. `d_active < 0` proves the
   page was on `n->partial`; a discard then proves `n->nr_partial >=
   s->min_partial` at that instant. A page whose free moves nothing was still
   frozen and answers nothing (measured 2026-11-03 13:41). Measured 13:53:
   `KS_CTL_TIGHT … state=listed-discarded`, 5.4 ms after the anchor's free.
3. With the count known, the anchor's own free separates the remaining
   failures: one object visible and no page removed means the page still held a
   live object. That is what 13:53 measured, so the count was not the blocker
   and no `nr_partial` hold could have helped; the next defect is anchor-slab
   ownership, not the threshold.
4. The gate's two halves stay as they are — one object (`active_objs −1`)
   **and** one page (`num_objs −32`, `slabdata −1`), in
   `src/kernel_page/gate.c`, tested in `tests/kernel_page/gate_test.c`. Nothing
   in this correction weakens it.
5. The measured `d_empty=-2` is not a discard failure: it is one discard plus
   one unrelated object freed on a listed page, and the unrelated object was
   a retired sibling's `mm_struct` landing inside the window. The baseline has
   been moved past that retirement — see `instrumentation.md`.
