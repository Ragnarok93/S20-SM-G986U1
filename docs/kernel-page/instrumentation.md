# Sampling and attribution audit

What each line of the payload measures, when it measures it, and what the
numbers can and cannot establish. Source citations are to
`~/kernel_samsung_sm8250` (4.19.113) and `src/util.c` in this tree.

## The model the counters obey

```
get_slabinfo()                 mm/slub.c:6191
  slabdata  = sum(node_nr_slabs)      -> n->nr_slabs, inc/dec in
                                         allocate_slab()/free_slab()
                                         (mm/slub.c:1859, 1989)
  num_objs  = sum(node_nr_objs)       -> n->total_objects, same lifetime
  nr_free   = count_partial(free)     -> n->partial only (mm/slub.c:2634)
  active_objs = num_objs - nr_free
```

Two consequences, and everything below follows from them:

1. A page on `n->partial` contributes **exactly its `inuse`** to
   `active_objs` (its free objects are subtracted). A page on no list — full,
   frozen on a CPU, or the CPU's active page — contributes **all of its
   objects**.
2. `slabdata` is *not* `n->nr_partial`. The discard precondition
   (`n->nr_partial >= s->min_partial` at the free) has no column in
   `/proc/slabinfo`, and the sysfs attributes that would expose it
   (`/sys/kernel/slab/mm_struct/{min_partial,cpu_partial,objects,...}`) are
   denied to the `shell` user on this device (measured).

Device confirmation of the geometry, read from `/proc/slabinfo` itself:

```
mm_struct            620    768   1024   32    8 : tunables    0    0    0 : slabdata     24     24      0
```

`objsize=1024` (`s->size`), `objperslab=32`, `pagesperslab=8` = order 3, so
`mm_objs_per_slab = 32768/1024 = 32` matches the device. The `tunables` field
is all zeros because this build's `slabinfo_show_stats()` is empty — it carries
no information and must not be read as limits.

## Sample points

| marker | util.c | taken | used as |
| --- | --- | --- | --- |
| `KS_PRE`, `KS_SLOT`, `KS_BURST` | 2434, ~2370, 2441 | around every burst fork | fresh-slab boundary proof: `slabdata+1` and `num_objs+32` at one allocation, nothing across the other 31 |
| `KS_FREEZE`, `KS_FREEZE_SAMPLE` | 3347, 3374 | after the 31 filler frees and a 2 ms wait | shows the frozen page moved no counter (`active == num` when nothing is listed) |
| `KS_DRAIN` | 3461 | after each freezer kill, `a0`→`a1` | the only evidence that `unfreeze_partials()` ran: a drop of at least `min_partial*(objects-1)` |
| `KS_DRAIN_BASE` | 3474 | immediately after the drain | informational; superseded by `KS_QUIESCE` |
| `KS_QUIESCE` | in `[3a]` | **after** sibling retirement, immediately before the target's exit | the gate baseline |
| `KS_SAMPLE1/2` | 3641, 3643 | after the free and after the sends | the gate's `after` sample |
| `KS_GATE` | 3740 | computed from the two above | the one verdict |
| `KS_DISCARD` | after the sends | restates the gate's verdict; the `post=` triple is a later interval | reporting only |
| `KS_ARITH` | same | the three samples and their deltas; `gate=` is the gate's verdict | reporting only |
| `KS_STATE` | terminal paths | stage, outcome, missing-sample count, whether the report is supported | lifecycle |
| `KS_CTL_FREE` | 3673 | around the control's last child kill, baseline is `KS_SAMPLE2` | machinery check only |

### What this pass changed, and why

- The gate baseline used to be taken **before** `kill_tgid_extras(target)`.
  Every retired sibling's `mm_struct` free therefore landed inside the
  measured window. That is exactly the measured `d_empty=-2, d_num=-32,
  d_slabs=-1`: the anchor's discard (`-1`) plus one unrelated object freed on
  a listed page (`-1`). The baseline now sits after the retirement and
  immediately before `kill_child(target)`, so the window contains one mm free
  and the sends.
- `targeted` used to require only `discarded`. It now requires `registered`
  **and** `discarded`. Without that, a page whose 32 objects were all counted
  active (`-32/-32/-1`) and a window carrying an extra free (`-2/-32/-1`) both
  passed the gate — neither is the anchor's emptying free.

## Decision table

Every row is a case in `tests/kernel_page/gate_test.c`, which runs on the host
with no device.

| `d_active` | `d_num` | `d_slabs` | what happened | gate |
| --- | --- | --- | --- | --- |
| −1 | −32 | −1 | listed page with one live object emptied and discarded | **pass** |
| 0 | 0 | 0 | free hit a frozen page (`FREE_FROZEN`): nothing moved, discard deferred | refuse |
| −1 | 0 | 0 | free landed on a listed page that still has live objects | refuse |
| −2 | −32 | −1 | a discard plus one unrelated free on a listed page | refuse |
| −32 | −32 | −1 | an empty unlisted page discarded (all 32 were counted active) | refuse |
| −2 | −64 | −2 | two pages discarded | refuse |

The first row is the only one in which one free and one page are the same
event. Any other row is consistent with pages *we did not intend* leaving the
cache, which is why the gate no longer accepts them.

## The anchor/control asymmetry

### One verdict, three samples

`KS_GATE`, `KS_DISCARD` and `KS_ARITH` are three lines, not three opinions.
Only `KS_GATE` computes a verdict; the other two restate it and print their own
window's samples next to it. Before this pass `KS_DISCARD` carried a second
predicate over the post-cleanup sample and `KS_ARITH` printed `discarded=1`
when the counters were **flat** — the opposite of what the model says. Both
were removed; see `gate-review.md`.

A cycle whose baseline could not be taken (file open failed, or two reads never
agreed) now refuses: `kp_gate_facts.baseline_ok` is required. The gate can
therefore no longer compare a post-drain sample against a pre-drain baseline.

The control slab is built by the same burst and emptied by the same mechanics,
but it is **not** sampled the same way:

| | anchor | control |
| --- | --- | --- |
| baseline | quiesced (two agreeing reads, 2×2 ms) | single read, `KS_SAMPLE2` |
| window | the target's exit + the sends | everything between the anchor's send and the control's kill |
| identity | needs the leak (`base`) | known by construction |
| purpose | the delivery subject | proves the machinery can discard at all |

So `KS_CTL_FREE discarded=1` is evidence about the *allocator*, never about
`base`, and its `d_num` must not be compared with the anchor's `d_num` as if
they were one measurement. A control refusal is also weak evidence: its window
is not proven quiet, so a delayed callback can move the numbers either way.

## What cannot be established from this surface

1. `n->nr_partial` at the instant of the free — no column exposes it.
2. Which page left the cache — `num_objs`/`slabdata` are cache-wide totals.
3. That the fragment's page is `base` — no read of the page's contents.
4. That the kernel consumed the planted bytes — requires (3) first.

The zero-image differential is the only planned test that can falsify (3), and
it requires a gate that passes the first row of the table above.
