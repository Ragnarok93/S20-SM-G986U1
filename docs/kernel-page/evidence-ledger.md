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
| 15 | The table reports one row per object, and a page only when that object's measured address and a passing geometry check support it | `KS_ID_WINDOW`/`KS_ID_ROW`/`KS_ID_SUMMARY`; `tests/kernel_page/identity_test.c`; `logs/20261103_173750.log` | 32 rows sharing one window id; exactly one `page_identity=exact` while one address exists; no grouping number until every slot has a page | host tests pass; window `f0cf5e9a-26855883` emitted 32 rows, one exact page (`ffffffc255930000`) and 31 unresolved, gate refused, device alive | one device cycle (page-probe mode) | **verified on device (Cycle A)** | the exact page is the scan object's; no co-residency or occupancy follows from it; the complete-table grouping branch is host-tested with synthetic per-slot addresses (`co-resident`, `split`); the payload mode that collects 32 addresses is not built, so it has no device exercise |
| 16 | A page is derived only from a power-of-two measured span, so the mask is exact | device `pagesperslab=8` → span 32768; host test with a three-page span (12288) | `span = 2^k`; anything else reports invalid | 32768 accepted; 12288 rejected as `non-power-of-two-span` | one host test per case; one device measurement of the span | verified for this geometry | the order-3 compound alignment is a source fact, not probed; no other device geometry is exercised |
| 17 | Two objects of one burst on the same 32 KiB page, at consecutive slot offsets | `KS_OBJ_ADDR` for slots 1 and 2 in runs 18:25 and 18:46 | slots 1 and 2 resolve to the same page with offset 1024 and 2048 | run 18:25: page `ffffffc05adc0000`; run 18:46: page `ffffffc244738000` | twice, two slots each | **measured, two objects** | not a 32-row result; row 0 and 3–31 have no address; the 18:46 run left the device unreachable after the dumps |
| 18 | A dump child's report is validated as one record, and a refused report never becomes a page | `kp_dump_report_parse()` / `kp_dump_admit()`; `tests/kernel_page/identity_test.c`; payload build | every failure path carries its own reason and leaves no page; a duplicate is counted rather than paired | 12 malformed forms refused; `no-report`, `truncated-report`, `malformed-report`, `scan-state-not-found`, `zero-address`, `collision-set-too-small`, `geometry-check-failed` and `duplicate-address` all refused; `duplicate_reports=1`, `observed_same_page_pairs=0`; build 0 warnings, `sha256=3a80a4b3…` | host tests, every change | **verified on host only** | a geometry failure keeps its measured address as a row excluded from every page (schema rule 6); the device has not re-run since the loss; the build hash is a build claim, not allocator evidence |
| 19 | The collector's report read is bounded and always closes its pipe, and pairs count one per unique address | `kp_dump_read_report()` over real pipes; `kp_identity_stats()` pair arithmetic; `tests/kernel_page/identity_test.c` | a full line returns at the newline, EOF returns 0 or the partial bytes, no writer returns at the deadline, and the descriptor is closed on every path | 50 ms deadline observed between 40 ms and 5 s; `EBADF` after each case including a bad capacity; 32 distinct → 496 pairs, 31 distinct over one duplicate row → 465, two objects → 1 | host tests, every change | **verified on host only** | a wall-clock deadline is not a scheduling proof; `KS_ID_DUMP_ALIVE`, the reaping of a dead child and cleanup release are device-only and untested since the handset went unreachable |
| 20 | The collector's DONE counts reconcile, and a child that never reported is counted as missing | `kp_dump_account()`; `tests/kernel_page/identity_test.c` | `reported + rejected + missing = slots`, `alive + gone = launched`, `reaped <= gone` | consistent sets reconcile; one missing report gives `missing=1`; a launched-but-unobserved child and impossible counts both give `mismatch` | host tests, every change | **verified on host only** | the device's real `missing`/`gone`/`reaped` counts have not been observed since the handset went unreachable |

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
7. Row 15 licenses object identity only: never occupancy, `inuse`, or a
   co-residency statement while fewer than two rows carry a page.
8. Row 16's mask is valid for the measured span only; a table that has
   not checked the power-of-two condition may not print a page.
9. Row 17 is two objects, not co-residency of the burst. It may be cited
   as evidence that the construction places consecutive slots together,
   never as a complete page map.
10. Row 18 is a host claim about the collector's admission rule. It does not
    say the collector is safe on the device: only a two-slot run that ends
    with the handset alive can say that.
11. Row 19's pair count is arithmetic on distinct admitted identities, not a
    burst-level co-residency result. A pair may never be cited as two objects
    unless both rows carry their own measured address and page.
12. Row 20 is arithmetic on the collector's own counts. A `mismatch` is a
    finding: it is reported, never normalised away in a later summary, and no
    missing or unobserved child may be folded into `rejected` or `gone`.
