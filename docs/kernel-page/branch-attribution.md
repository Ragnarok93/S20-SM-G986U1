# Allocator-branch attribution

Which `__slab_free()` paths are **consistent with** a measured free, for the
anchor's emptying free and for the tight control, decided by one classifier on
identical evidence. It is not a trace of the branch that executed: the only
input is the cache-wide counter delta. The predicates behind every path are in
[slub-branch-audit.md](slub-branch-audit.md) with this tree's exact revision.

## What the deltas allow

| delta | paths consistent with it | inuse at the free |
| --- | --- | --- |
| `(−1, −objects, −1)` | `discard` only — the page emptied and left | `1` |
| `(−1, 0, 0)` | `listed` (a live object remained) **or** `emptied-refused` (`nr_partial < min_partial`) | `>= 2`, or `1` |
| `(0, 0, 0)` | `fastpath` **or** `FREE_FROZEN` **or** the full-page push | `>= 1` |
| anything else | `unknown` | unavailable |

`inuse_at_free` is the page's in-use count **before** this free's
decrement; `inuse_after_free` is one lower. A page can only be discarded when
`inuse_after_free == 0`, i.e. `inuse_at_free == 1`. The two listed residuals are
therefore pre-free `>= 2` (a live object remains) and pre-free `1` (the page
emptied but was retained), and a `(−1, 0, 0)` delta cannot tell them apart.
Kernel code and logs must not use the two counts interchangeably.

`page->inuse`, `page->frozen` and `n->nr_partial` are kernel-internal.
`/proc/slabinfo` is cache-wide and the per-cache `/sys` attributes are denied
(measured), and the only interface that would expose a freed object's address —
`kmem:kmem_cache_free` — cannot be enabled from this domain (measured; see the
audit). `inuse` is therefore reported `unavailable` with the residual values
the surviving paths allow, never guessed.

One input deserves the same care: `cpu_partial` is **derived** from the measured
object size by repeating `set_cpu_partial()`'s rule, not read — the sysfs file
is `EACCES`. The classifier does not use it; the crossing construction does.

## The line

```
KS_BRANCH tag=anchor basis=cache-wide d_active=-1 d_num=0 d_slabs=0
paths=listed,emptied-refused paths_src=consistent-with listed_signature=1
inuse_at_free=unavailable inuse_src=unavailable(page-inuse-not-exposed)
residual=listed:>=2,emptied-refused:1 control=no-discard gap_us=4755
```

- `basis=cache-wide` and `paths_src=consistent-with` say what the line is: a
  reading of aggregate counters, not of the page.
- `listed_signature` is a signature, not an observation of the page's list
  membership. It was named `page_listed` in commit `9bc6cf3`; logs from before
  this rename keep the old field name and are not rewritten.
- `residual` is the pair of `inuse` values the surviving paths allow.
- `control=` and `gap_us=` carry the tight control's own verdict and its
  distance from this free, or `not-run`. They are reported, not folded into the
  path set.

`KS_CTL_TIGHT` takes its `state=` token from this same classifier, so the two
lines cannot disagree:

| token | meaning |
| --- | --- |
| `listed-discarded` | emptied and left the cache |
| `listed` | the delta matches a node-list transition; the counters cannot say whether the page kept a live object or emptied and was refused |
| `unlisted` | no node-list transition: fastpath, `FREE_FROZEN`, or the full-page push |
| `other` | a delta the classifier does not recognise |

## Attribution rule

A cache-wide counter delta is attributable to the target free only when
competing cache activity over the same interval is excluded or independently
accounted for. Otherwise the result is a cache-wide signature consistent with
the target path — reported as such, never as proof of that path.

## Isolation of the measurement

The anchor's delta is read across exactly one free: `cross_*` is sampled after
the crossing has finished and before the target is killed, `empty_*` after the
free completes. Between them run only the image `sendmsg()` calls, which
allocate order-3 socket frags and no `mm_struct` object. The target is pinned
to `EMPTYING_CPU`, and the 50 ms window that follows is sampled separately
(`KS_QUIESCE … stable=1`) so a late change cannot be attributed to the free.

What this does **not** exclude: frees on other CPUs during the same
microseconds, and kernel threads. The classifier therefore reports the
signature as consistent with a path set, and the cycle's control is printed
beside it rather than used to narrow the set.
