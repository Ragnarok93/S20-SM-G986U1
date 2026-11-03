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
| 11 | The `mm_struct` slab geometry is read, not assumed | `KS_SURFACE geometry=measured` | the kernel's own objsize/objperslab/pagesperslab | `objsize=1024 objs_per_slab=32 pages_per_slab=8 span=32768`, `assumes_agree=1` | every cycle (slabinfo) | verified | per-cache `/sys/kernel/slab` attributes stay denied (`sysfs=denied errno=13`) |
| 12 | The leaked object resolves to slot 1 of its 32 KiB page | `KS_OBJ_ID` | a correct address and span must land on the constructed slot | `offset=1024 slot=1 match=1` | one cycle | derived and checkable | proves the address is grid-consistent with the construction; not that the page is the burst slab |
| 13 | Page membership for the 31 filler objects | `KS_ID_SLOT … page=unavailable` | — | not obtainable | — | **not observable** | one address per leak; every counter is cache-wide |
| 14 | Each slot's free landed, and its counter effect | `KS_ID_SLOT d_src=measured` | a frozen page's free moves nothing; a listed page's free moves `active` by −1 | 31 × `d_active=0`, slot 31 `d_active=−1` | one cycle | demonstrated once | a zero delta is ambiguous between co-residency on a frozen page and a different full page |
| 11 | The counter model is the vendor's, not inferred | `mm/slub.c` 6191/1859/1989/2634 read in this tree; device slabinfo geometry (32 objects, order 3) | `num_objs`+32 and `slabdata`+1 at one allocation; no movement across 31 more | holds in every burst | every cycle | verified | `active_objs` is a derived quantity (`total_objects - node-partial free`), not an allocated-object count |
| 12 | The anchor's window contains exactly one mm free | `KS_QUIESCE` placement after sibling retirement (this pass) | `d_empty=-1` without sibling frees in the window | not yet measured on device | — | **unverified** | the code change is tested only on the host (`tests/kernel_page/gate_test.c`); needs one delivered cycle |
| 13 | `slabdata` equals `n->nr_partial` | old comment in `kernel_page.h` | — | **false**: it is `n->nr_slabs` | — | refuted | corrected in this pass; no gate field may use it as `nr_partial` |
| 14 | A baseline that was never proven quiet cannot pass the gate | `kp_gate_facts.baseline_ok` + host case 10 | perfect deltas with `baseline_ok=0` refuse | refuses on host | 1 case | verified on host | unexercised on device: no cycle has yet run with the new requirement |
| 15 | Every log line that states a verdict states the same one | `gate-review.md` §3 | no line can print `discarded=1` while the gate says otherwise | three predicates collapsed into one | source + host | verified | `KS_CTL_FREE` remains its own check and is documented as such |

## Rules this ledger imposes

1. Rows 8 and 9 may never be inferred from rows 2–7. An allocator cycle is not a
   placement result.
2. Row 10 is why the gate must be judged on the outcome (row 5), not on a
   precondition it cannot read.
3. Any new claim gets a row before it is used to justify another change.
4. Row 13 is not a gap to be closed by a better reading of the same counters: an
   object identity needs an address, and the leak returns one per scan.
