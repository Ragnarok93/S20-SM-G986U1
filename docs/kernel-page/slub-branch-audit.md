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

`min_partial` is `set_min_partial(s, ilog2(s->size) / 2)` (3915): `ilog2(1024)
/ 2 = 5`, the value the gate prints.

## Evidence classes

Every statement below is tagged with the class of evidence it comes from. A
conclusion may only lean on the class named here; a configuration fact is not
an observed execution and a derived number is not a reading.

| class | what it is | how it is obtained | may not be used to |
| --- | --- | --- | --- |
| **C** config | compile-time and creation-time properties | this source tree, the cache's creation site | prove what any single free did |
| **M** measured | device readings | `/proc/slabinfo`, counter deltas, log lines | name an internal value the surface does not expose |
| **D** derived | arithmetic repeated from a source rule | the payload's own computation | claim to be a reading |
| **P** predicate | a branch condition proven from source | the code quoted here | say which branch ran |
| **U** unobserved | runtime outcomes the surface cannot show | — | be filled in by C, M, D or P |

Two things the table already rules out: `page->inuse`, `page->frozen`, `prior`
and `n->nr_partial` are all class **U**. No amount of **C**/**M**/**D**/**P**
evidence turns them into a measurement, which is why `KS_BRANCH` reports a path
*set* and `inuse_at_free=unavailable`.

## `cpu_partial` is derived, not read

The payload's `mm_cpu_partial_limit()` (`src/util.c`) repeats the vendor's rule:

```c
if (s->size >= PAGE_SIZE)      2
else if (s->size >= 1024)      6      /* mm_struct: s->size == 1024 */
else if (s->size >= 256)      13
else                          30
```

with `s->size` taken from the **measured** slabinfo fields `objsize=1024`,
`objperslab=32`, `pagesperslab=8` (class **M**). The value `6` is therefore a
class **D** prediction. It is *not* read from
`/sys/kernel/slab/mm_struct/cpu_partial`: every file in that directory returns
`EACCES` to this shell (class **M**, see the observability table). An earlier
draft of this document called it "measured"; that was wrong and is corrected
here.

`set_cpu_partial()` is called once at cache creation (`__kmem_cache_create`,
3917) and again only from the sysfs `cpu_partial_store` (5299), which refuses a
nonzero value when `!kmem_cache_has_cpu_partial(s)` (5296). So a cache that
shows `cpu_partial != 0` cannot be a debug cache.

## Both "unreachable path" claims reduce to one proposition

`add_partial()` (3171) needs `!kmem_cache_has_cpu_partial(s)`, and
`remove_full()` inside `slab_empty:` needs `prior == NULL`. Both are unreachable
only if `kmem_cache_has_cpu_partial(mm_struct)` is true. That predicate is

```c
#define kmem_cache_has_cpu_partial(s)  (!kmem_cache_debug(s))   /* slub.c:152 */
#define kmem_cache_debug(s)            (s->flags & SLAB_DEBUG_FLAGS)  /* 138 */
#define SLAB_DEBUG_FLAGS  (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | \
                           SLAB_TRACE | SLAB_CONSISTENCY_CHECKS)      /* mm/slab.h:141 */
```

so the chain is:

1. **C**: `mm_cachep` is created at `kernel/fork.c:2663` with
   `SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT` — no debug flag, so
   `kmem_cache_debug` was false at creation for any `CONFIG_SLUB_CPU_PARTIAL=y`
   build.
2. **M**: the freeze sample. The anchor page is full before the 31 filler
   frees, so its first free has `prior == NULL`. With `has_cpu_partial` false
   that free takes the `else` branch (3126), takes the node lock and reaches
   `add_partial` (3171): the page is listed with 31 free objects and
   `active_objs` falls by 31. With `has_cpu_partial` true it takes 3114 and
   `put_cpu_partial()` instead, and no counter moves. `KS_FREEZE_SAMPLE` is
   read 2 ms after the filler frees; in **7 of the 11 recorded cycles** it
   reads `active == num` exactly (`docs/kernel-page/logs/20261103_105018.log:385`:
   `active=2432 num=2432`), which the `add_partial` outcome contradicts. The
   other four cycles read `active` 94 or 126 below `num` — unrelated pages
   were listed in that window, so those samples do not discriminate and are
   counted neither way. Conclusion: `has_cpu_partial(mm_struct)` was **true at
   the free itself** in those seven cycles.

   Caveat from the same surface: the counter is cache-wide, so a coincident
   allocation of exactly 31 free objects off the node list inside the 2 ms
   settle would mask the `add_partial` effect. Nothing in the logs shows such
   activity and exact equality to the object would be required, but the
   construction does not exclude it.
3. **P**: with that proposition true, `add_partial` (3171) cannot run, and at
   `slab_empty:` `n != NULL` implies the lock-taking branch was entered, which
   implies `prior != NULL` (the `!prior` case went to 3114 instead); a retried
   cmpxchg clears `n` at the top of the loop (3119-3122), so a successful
   iteration with `n` set has `prior != NULL` and takes `remove_partial()`.
   `remove_full()` is therefore unreachable for this cache.

**Residual (class U):** `s->flags` can still be changed after creation by a
`slub_debug=` boot argument covering this cache, or by a root write to the
`sanity_checks` / `trace` / `red_zone` / `poison` / `store_user` attributes
(`mm/slub.c:5437-5540`; `validate` only runs `validate_slab_cache` and sets no
flag, 5543). Neither is observed, the experiment performs no sysfs
write, and the exact-packing geometry (1024 bytes × 32 objects = one order-3
span) leaves no room for red-zone or poison padding — but `SLAB_STORE_USER`,
`SLAB_TRACE` and `SLAB_CONSISTENCY_CHECKS` do not change geometry, so geometry
alone cannot exclude them. If one of those were enabled, `has_cpu_partial`
would be false, `add_partial`/`remove_full` would become reachable, and the
`active == num` signature would have read `active == num − 31` instead.

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

That is an argument about *which* free the window brackets, not a proof that no
other CPU freed an `mm_struct` inside it. See the attribution rule in
[branch-attribution.md](branch-attribution.md).

## Observability: can `inuse` be read at all?

Measured on the device on 2026-11-03 as `shell` (uid 2000), class **M**:

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
remains a root-only route. Note what it would and would not settle: a `ptr`
identifies the freed *object* and its page, which closes identity, but it
carries neither `page->inuse` nor list membership, so the `listed` versus
`emptied-refused` residual would survive it.

**Conclusion: there is no page-resolved `inuse` observation available without
root, and the classifier keeps the ambiguity explicit rather than closing it.**
