# Evidence ledger

One row per claim. A claim is only "verified" at the confidence recorded here;
nothing inherits confidence from a neighbouring claim.

| # | Claim | Evidence | Expected if true | Observed | Reproducibility | Confidence | Outstanding uncertainty |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | A recovered address is real for this run | `KernelSnitch mm found` + hash re-check in `ks_pc_answer_matches_collisions` | every measured collision hashes equal under the candidate | accepted and saved as cache anchor; stale answers rejected | every cycle since the abort-hook fix | verified | scan cost 8–10 min; `KS_PC` answer path still waits 60 s when no file exists |
| 2 | One fresh 32-slot slab is built | `KS_BURST span_ok=1`, `KS_PRE leak_slot=1` | no second `slabdata`/`num_objs` jump across 32 allocations | holds in every delivered cycle | every cycle | verified | `span_ok` proves "no new slab appeared", not "every object is ours"; slot-level ownership is inferred |
| 3 | The scanning child sits in that slab | slot-1 construction + `span_ok` | the child's allocation is the second from the fresh page | `leak_slot=1` in the log | every cycle since the rebuild | verified by construction | a foreign allocation could in principle take slot 2; no per-slot proof exists |
| 4 | The emptying free reached the allocator | `d_empty` | `-1` means the free landed on a node-listed page | −1 in the quiesced cycle, 0 in earlier frozen cases | one confirmed attribution after the quiesce fix | demonstrated once | the counter also moved to −2 when a deferred filler free shared the window |
| 5 | The anchor slab was discarded | `KS_GATE d_num=-32 d_slabs=-1` | one slab leaves the cache in the free's window | seen once (10:09 cycle) | one cycle | demonstrated once | later cycles show `d_num=0`; the counter cannot say which slab left |
| 6 | The control slab discards on the same mechanics | `KS_CTL_FREE d_num=-32 d_slabs=-1` | same outcome as the anchor | seen in several cycles | repeatable | verified | it carries no address, so it cannot bind `base` |
| 7 | The send is the first order-3 allocation after the free | `KS_IMAGE_PIN gap_ms=0.054 same=1` | send on the freeing CPU, no allocation in between | 54–56 µs, same CPU | every delivered cycle | verified | the CPU-partial list and other CPUs' activity are not measured |
| 8 | The fragment's page is `base` | zero-image differential | the panic address moves off `+0x304` | unchanged (`+0x304`) in the one cycle run | single cycle | unverified / contradicted | the cycle did not have a proven discard, so the contradiction is not conclusive |
| 9 | The kernel consumed the planted bytes | post-hijack read-back | fake fops table is reachable | never observed | none | unverified | requires a passing probe first |
| 10 | `n->nr_partial >= s->min_partial` at the emptying free | none available | — | not measurable | — | **not observable** | `/proc/slabinfo` reports cache-wide totals, not `nr_partial` (see `accounting.md`) |
| 11 | `mm_struct` has CPU-partial caching, so `add_partial()` and `remove_full()` are unreachable | `KS_FREEZE_SAMPLE active == num` in 7 of 11 cycles, plus the cache's creation flags | a listed page would drop `active` by 31 | equality in the 7; 94/126 low in the other 4, which do not discriminate | repeatable | **inferred, not observed** | the "page was full" premise is ledger rows 2–3, not the counter; the conclusion assumes no concurrent activity masks a 31-object change; a `slub_debug=` boot argument or a root sysfs flag write is not observable; `cpu_partial=6` is derived, not read |
| 12 | `build.sh` fails when the compiler fails or an artifact is missing | injected `#error`; stub-`make` contract test | nonzero status, nothing packaged | exit 2 with `Error 1`; test passes | every run since the probe | verified | the contract test stubs `make`; the real NDK build is hosted on this machine only |
| 13 | The comment-only source edits did not change the built artifact | `sha256=ded45234…` identical before and after, and equal to `prebuilt/preload.so` | byte-identical output | identical | one build | verified | says nothing about whether the branch argument in row 11 is sound — a separate claim |
| 14 | A page identity is obtainable for more than one object per run | `KS_SURFACE per_object_address=unavailable reason=one-leak-one-address`; sysfs and tracepoint enable `EACCES` | another surface, or 31 more leak dumps | one address per dump; no other per-object source | measured once | verified as a limit | 31 further dumps at 8–10 min each is a different run mode (`identity-table.md`) |

## Retractions

A retracted claim stays here with what replaced it. It is never deleted, and it
is never cited as support.

| # | claim as recorded | where | retracted by | status |
| --- | --- | --- | --- | --- |
| R1 | `build.sh` exits 0 after a compiler error | `build.md` before `99c440a`, and the pass report that introduced it | injected `#error`: `make: *** … Error 1`, exit **2**; the `0` came from a `grep` in the invoking pipeline | **retracted** — the script propagates the failure; the defect was in the observation |
| R2 | `cpu_partial=6` was measured | `slub-branch-audit.md` as committed in `ad993c7` | `mm_cpu_partial_limit()` re-derives it from the measured `objsize`; `/sys/kernel/slab/mm_struct/*` is `EACCES` | **retracted** — the value is derived (D), never read |

## Rules this ledger imposes

1. Rows 8 and 9 may never be inferred from rows 2–7. An allocator cycle is not a
   placement result.
2. Row 10 is why the gate must be judged on the outcome (row 5), not on a
   precondition it cannot read.
3. Any new claim gets a row before it is used to justify another change.
4. Row 11 is a claim about the cache, not about one free: it licenses the
   reachability argument in `slub-branch-audit.md` and nothing about which
   branch a given free took. Its confidence is *inferred*; code and documents
   must not upgrade it.
5. Row 13 is a build claim only. Identical hashes never stand in for evidence
   about allocator behaviour.
6. Claims are retracted in the table above and stay visible; a superseded
   sentence in an old document is annotated at its source as well.
