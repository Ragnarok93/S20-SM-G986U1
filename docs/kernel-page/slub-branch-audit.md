# SLUB free-path audit (SM-G986U1)

Source: `mm/slub.c` from the SM8250 tree at commit
`86954d832818684841c25636ba9f579c33d313f5`, `VERSION=4 PATCHLEVEL=19
SUBLEVEL=113`. Every line range below is from that revision; line numbers move
between releases, the predicates do not.

## Call path

```
kmem_cache_free()            mm/slub.c:3272  (slab_free, then trace_kmem_cache_free)
  slab_free()                mm/slub.c:3256
    do_slab_free()           mm/slub.c:3212
      page == c->page ?      mm/slub.c:3237  -> freelist cmpxchg, no node list
      else                   mm/slub.c:3252  -> __slab_free()
__slab_free()                mm/slub.c:3083
```

`kmem_cache_free()` passes `cnt = 1` (3278), so the bulk-free paths
(`new.inuse -= cnt` with `cnt > 1`) do not apply to an `mm_struct` free.

## The paths, their predicates and their accounting

| path | predicate in `__slab_free()` | counter effect |
| --- | --- | --- |
| debug reject | `kmem_cache_debug(s) && !free_debug_processing(...)` (3090) | none; only for an invalid free |
| full-page push | `(!new.inuse \|\| !prior) && !was_frozen` and `kmem_cache_has_cpu_partial(s) && !prior` (3114) → `new.frozen = 1`, `put_cpu_partial(s, page, 1)` (3152) | none: the page leaves every list for `c->partial` |
| `FREE_FROZEN` | `was_frozen` (3159) | none |
| listed, page kept | enters the `n` lock (3124) because `prior != NULL`; `!new.inuse && n->nr_partial >= s->min_partial` false (3164) → "objects left" (3168) | `active −1` (one more free object on a listed page) |
| listed, emptied, refused | same entry; `!new.inuse` true but `n->nr_partial < s->min_partial` (3164) → falls through to 3168 as well | `active −1`; the empty page stays on `n->partial` |
| discard | `!new.inuse && n->nr_partial >= s->min_partial` (3164) → `slab_empty:` (3180) → `remove_partial()`/`remove_full()`, `discard_slab()` (3194) | `active −1`, `num_objs −objects`, `slabdata −1` |

Two paths in this function are not reachable for this cache:

- `add_partial(n, page, DEACTIVATE_TO_TAIL)` (3171) requires
  `!kmem_cache_has_cpu_partial(s)`. That is `!kmem_cache_debug(s)` under
  `CONFIG_SLUB_CPU_PARTIAL` (152), so it holds for `mm_struct`; the measured
  `cpu_partial=6` matches `set_cpu_partial()`'s `size >= 1024` arm (3748).
- `remove_full()` inside `slab_empty:` needs `prior == NULL`, i.e. a page on
  the node full list. With cpu partial enabled a full page is frozen onto
  `c->partial` instead, so the discard of a page this payload emptied takes
  `remove_partial()`.

`min_partial` is `set_min_partial(s, ilog2(s->size) / 2)` (3915): `ilog2(1024)
/ 2 = 5`, the value the gate prints.

## What the counters say about each path

`get_slabinfo()` (6190) reports `num_objs` and `slabdata` from the per-node
counters, and `active_objs = num_objs − count_partial(n, count_free)` (6202),
with `count_free(page) = page->objects − page->inuse` (2622). Only pages on
`n->partial` are counted in `nr_free`.

So a free's own contribution to `active_objs` is `−1` exactly when its page is
on `n->partial` at that moment, and `0` when the page is full, frozen, or this
CPU's active slab. A discard additionally removes one page's objects and one
slab. This is the whole of the observable surface; `page->inuse` itself is not
in any counter.

## The drain path

`unfreeze_partials()` (2424) applies the same `!new.inuse && n->nr_partial >=
s->min_partial` test (2462) to a page taken off `c->partial`, and either queues
it for `discard_slab()` (2479) or adds it to the node list tail. A slab can
therefore leave the cache because of a *drain*, not because of a free. The
gate's two samples bracket exactly one free — `cross_*` is read after the
crossing, `empty_*` after the target's free — and the image sends between them
allocate only order-3 socket frags, which move no `mm_struct` object.

## Observability: can `inuse` be read at all?

Measured on the device on 2026-11-03 as `shell` (uid 2000):

| surface | result |
| --- | --- |
| `/sys/kernel/slab/mm_struct/{object_size,objs_per_slab,order,partial,cpu_slabs,…}` | `EACCES` on every file |
| `/proc/slabinfo` | readable; cache-wide only (`active_objs`, `num_objs`, `objsize`, `objperslab`, `pagesperslab`, `slabdata`) |
| tracefs mounts | `/sys/kernel/tracing` and `/sys/kernel/debug/tracing`, mode `0555`, group `readtracefs` (3012), shell is a member |
| `/sys/kernel/tracing/events/kmem/` | listable |
| `events/kmem/kmem_cache_free/{enable,filter,id}` | `EACCES` |
| `trace_pipe` | readable; shows only the enabled `tracing_mark_write` events |
| `tracing_on` | `0` |
| `/proc/kpageflags`, `/proc/kpagecount`, `/proc/pagetypeinfo` | `EACCES` |
| `/proc/sys/kernel/perf_event_paranoid` | `-1` (permissive), but the tracepoint `id` is unreadable and SELinux is enforcing: unverified, not relied on |

The `kmem:kmem_cache_free` tracepoint would report `call_site` and `ptr`
(`include/trace/events/kmem.h:122-139`, `DEFINE_EVENT` at 148), and `ptr` is the
freed object's kernel address — precisely the per-object identity the previous
milestone could not obtain. It cannot be enabled from this domain, so that
remains a root-only route.

**Conclusion: there is no page-resolved `inuse` observation available without
root, and the classifier keeps the ambiguity explicit rather than closing it.**
