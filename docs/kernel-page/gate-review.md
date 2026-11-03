# Review of the gate change (`c73afae`) and this pass's follow-up

Scope: the predicate that decides whether an emptying free may be treated as
the anchor page's discard, the samples fed to it, and the lines that report it.
Everything below was checked by reading `src/util.c`, `src/kernel_page/*.c` and
the vendor `mm/slub.c`; the evidence is the host suite in
`tests/kernel_page/`. This is a self-review, not an independent one.

## 1. Counter scope and lifetime

`get_slabinfo()` (mm/slub.c:6191) sums per-node counters that
`allocate_slab()`/`free_slab()` maintain (1859, 1989) for `num_objs` and
`slabdata`, and walks `n->partial` for the free count. So `num_objs` and
`slabdata` cover every live page, and only the free count is node-scoped. The
gate's `discarded` test (`num −objects`, `slabs −1`) is therefore a statement
about the cache, not about a page.

**Verdict:** the arithmetic is consistent with the kernel. It cannot name the
page, and nothing in the gate claims it does.

## 2. Baseline consistency

Two problems were found and fixed:

1. **(fixed in `c73afae`)** the baseline was sampled before `kill_tgid_extras()`,
   so every retired sibling's mm free landed inside the window. That is the
   measured `d_empty=-2`.
2. **(fixed here)** nothing required that a baseline was actually taken. When
   `/proc/slabinfo` could not be opened (`post_f == NULL`) or the quiesce never
   stabilized, `cross_*` remained the pre-drain isolation sample, and the gate
   compared a post-drain sample against it. A drain that happened to discard a
   page could then produce a `−32/−1` delta across a window spanning the whole
   crossing. `kp_gate_facts.baseline_ok` now carries "a fresh baseline was read
   immediately before the free and two reads agreed", and `targeted` requires
   it.

**Verdict:** baseline consistency is now a precondition, not an assumption. The
cost is that a noisy cycle refuses; that is the measurement working.

## 3. One observation interval

The review question exposed three predicates in the same log:

| line | window it compared | defect |
| --- | --- | --- |
| `KS_GATE` | quiesce → post-send | correct interval |
| `KS_DISCARD verdict=` | baseline → **post-cleanup** sample | second predicate, different interval, could disagree with `KS_GATE` |
| `KS_ARITH … discarded=` | post-send → post-cleanup | called **flat** counters `discarded=1`, the opposite of the model |

Both are fixed: `KS_DISCARD` now restates the gate's verdict and prints the
later sample separately (`post=`), and `KS_ARITH` prints `gate=` from the same
verdict. No sample moved; only the claims changed.

## 4. Missing or stale observations read as valid

Three cases, all now covered by host tests:

1. unreadable sample values are `-1` from `mm_slab_scan`; the gate rejected
   them only by accident. With a one-object cache, `before (1,32,1)` and
   `after (-1,-1,-1)` satisfied both deltas. `kp_gate_evaluate()` now rejects
   any negative input before doing arithmetic.
2. a stale baseline (see §2) could satisfy both deltas legitimately-looking;
   `baseline_ok` blocks it.
3. `after (0,0,0)` from an unopened file is not "nothing changed" — it fails
   the deltas, and is reported as a refusal.

## 5. Refusal reported as refusal

`KS_ARITH` printed `discarded=1` while `KS_GATE` printed `discarded=0` in the
same cycle, and `KS_DISCARD` printed `ambiguous` whether the gate had passed or
not. Now every line that states a verdict derives it from `kp_gate_verdict`,
and the refusal paths print `KS_STATE outcome=refused report=1` with the
reason, so a refusal is machine-readable as a refusal.

## Defects found but deliberately not fixed

- **The control slab's window is not quiesced.** Its baseline is a single read
  taken after the anchor's send, so its `−32/−1` can absorb deferred activity
  and it can never support a claim about `base`. Fixing that means changing
  when the control free runs — forbidden in a validation-only pass.
- **`KP_ALLOCATOR_TRANSITION_VERIFIED` still marks when the free registered on
  a page that did not discard.** That is a true statement about the allocator
  transition but not about the anchor; the label is left as-is and the
  distinction is recorded in the ledger.
- **The gate cannot establish page identity.** It can only establish that one
  object and one page moved in a quiet window on the right CPU. Identity
  remains the unresolved property.

## Residual risk

Requiring `baseline_ok` and `registered` makes the gate strictly narrower. A
cycle that previously delivered may now refuse. That is intended: the previous
delivery carried a `−2` window, which the model says contained a second,
unrelated free. If every cycle now refuses with `stable=0`, the conclusion is
that the window is not quiet enough to measure, not that the predicate is
wrong.
