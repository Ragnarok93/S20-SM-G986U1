# Allocator-branch attribution

Which `__slab_free()` path a measured free took, for the anchor's emptying
free and for the tight control, decided by one classifier on identical
evidence. The classifier never returns one path where the cache-wide counters
leave two open.

## The branches, from this tree

`__slab_free()` (`mm/slub.c:3083`) for a page that is not this CPU's active
slab:

| condition | branch | counter effect |
| --- | --- | --- |
| `(!new.inuse \|\| !prior) && !was_frozen` and `!prior` | `new.frozen = 1`, `put_cpu_partial()` (`:3130`, `:3151`) | none |
| `(!new.inuse \|\| !prior) && !was_frozen` and `prior` | listed; then `!new.inuse && n->nr_partial >= s->min_partial` → `slab_empty` → `discard_slab()` (`:3164`) | `num −objects`, `slabs −1`, `active −1` |
| same, but the page keeps an object | listed, `inuse--` (`:3169`) | `active −1` |
| `was_frozen` | `FREE_FROZEN` (`:3157`) | none |
| page is this CPU's `c->page` | `do_slab_free()` fastpath, never reaches `__slab_free()` | none |

## What the counters can and cannot separate

| delta | paths that remain |
| --- | --- |
| `(−1, −objects, −1)` | `discard` only — the page emptied and left |
| `(−1, 0, 0)` | `listed` (a live object remained, so `inuse at the free ≥ 2`) **or** `emptied-refused` (`inuse 1`, `nr_partial < min_partial`) |
| `(0, 0, 0)` | `fastpath` **or** `FREE_FROZEN` **or** the full-page push |
| anything else | `unknown` |

`page->inuse`, `page->frozen` and `n->nr_partial` are kernel-internal.
`/proc/slabinfo` is cache-wide and every attribute under `/sys/kernel/slab` is
denied to the shell (measured: `EACCES`), so `inuse_at_free` is reported
`unavailable` with the residual values the surviving paths allow. It is never
guessed.

## The lines

```
KS_BRANCH tag=anchor d_active=-1 d_num=0 d_slabs=0 paths=listed,emptied-refused
page_listed=1 inuse_at_free=unavailable
inuse_src=unavailable(page-inuse-not-exposed)
residual=listed:>=2,emptied-refused:1 control=discard gap_us=…
KS_BRANCH tag=control d_active=-1 d_num=-32 d_slabs=-1 paths=discard
page_listed=1 inuse_at_free=1 inuse_src=derived(page-emptied) residual=none
control=not-run gap_us=-1
```

The control's line carries `control=not-run` because its own free is the one
being classified; the anchor's carries the control's result and the gap
between the two frees. `KS_CTL_TIGHT` now takes its `state=` token from this
classifier, so the two can never disagree:

| token | meaning |
| --- | --- |
| `listed-discarded` | emptied and left the cache |
| `listed` | listed; the counters cannot say whether it emptied and was refused or kept a live object |
| `unlisted` | not on any node list: fastpath, `FREE_FROZEN` or the full-page push |
| `other` | a delta the classifier does not recognise |

The old `listed-refused` token claimed a refusal the counters cannot prove and
`frozen` claimed a freeze they cannot prove; both are replaced.

## What this establishes in a cycle

For the anchor's emptying free: it reached `__slab_free()` (not the fastpath),
the page was on a node list (not frozen, not full), and it either kept a live
object or was refused below `min_partial`. The tight control `discard`s
microseconds later, which is evidence that the count condition held for its own
free; the anchor line prints that gap rather than folding it into the verdict.
