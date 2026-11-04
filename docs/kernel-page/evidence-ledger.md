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
| 17 | Two objects of one burst on the same 32 KiB page, at consecutive slot offsets | `KS_OBJ_ADDR` for slots 1 and 2 in runs 18:25, 18:46 and 20:02 | slots 1 and 2 resolve to the same page with offset 1024 and 2048 | run 18:25: page `ffffffc05adc0000`; run 18:46: page `ffffffc244738000`; run 20:02: page `ffffffc011830000` | three times, two slots each | **measured, two objects** | not a 32-row result; row 0 and 3–31 have no address; the 18:46 and 20:02 runs lost the device later in the same cycle (both inside the punch; rows 22, 33) |
| 18 | A dump child's report is validated as one record, and a refused report never becomes a page | `kp_dump_report_parse()` / `kp_dump_admit()`; `tests/kernel_page/identity_test.c`; payload build | every failure path carries its own reason and leaves no page; a duplicate is counted rather than paired | 12 malformed forms refused; `no-report`, `truncated-report`, `malformed-report`, `scan-state-not-found`, `zero-address`, `collision-set-too-small`, `geometry-check-failed` and `duplicate-address` all refused; `duplicate_reports=1`, `observed_same_page_pairs=0`; build 0 warnings, `sha256=3a80a4b3…` | host tests, every change | **verified on host only** | a geometry failure keeps its measured address as a row excluded from every page (schema rule 6); the device has not re-run since the loss; the build hash is a build claim, not allocator evidence |
| 19 | The collector's report read is bounded and always closes its pipe, and pairs count one per unique address | `kp_dump_read_report()` over real pipes; `kp_identity_stats()` pair arithmetic; `tests/kernel_page/identity_test.c` | a full line returns at the newline, EOF returns 0 or the partial bytes, no writer returns at the deadline, and the descriptor is closed on every path | 50 ms deadline observed between 40 ms and 5 s; `EBADF` after each case including a bad capacity; 32 distinct → 496 pairs, 31 distinct over one duplicate row → 465, two objects → 1 | host tests, every change | **verified on host only** | a wall-clock deadline is not a scheduling proof; `KS_ID_DUMP_ALIVE`, the reaping of a dead child and cleanup release are device-only and untested since the handset went unreachable |
| 20 | The collector's DONE counts reconcile, and a child that never reported is counted as missing | `kp_dump_account()`; `tests/kernel_page/identity_test.c` | `reported + rejected + missing = slots`, `alive + gone = launched`, `reaped <= gone` | consistent sets reconcile; one missing report gives `missing=1`; a launched-but-unobserved child and impossible counts both give `mismatch` | host tests, every change | **verified on host only** | the device's real `missing`/`gone`/`reaped` counts have not been observed since the handset went unreachable |
| 21 | The two-slot collector runs on the device with its own checks satisfied | `logs/20261103_200234.log`; run 20:02 | both reports written whole, admitted, both cleanups released, DONE accounting ok, no rejected or missing report | `bytes=40 of=40` for both slots; `verdict=accepted`; `cleanup=released` twice; `reported=2 rejected=0 missing=0 alive=2 open_pipes=0 accounting=ok`; one same-page pair | one device cycle | **verified on device (once)** | the DONE sample preceded slot 2's cleanup and caught it running; `alive` is point-in-time, and this run itself ended with the device lost, so collector safety under repetition is not established |
| 22 | The device loss is associated with the post-futex plant/punch stage, not with the dump collector | runs 10:09, 18:46, 20:02 vs 18:25 and the other 12 surviving runs | a run that enters the plant/punch stage loses the device; a run that bails before it stays alive | 10:09 ends at `waiter futex returned`; 18:46 ends there on the host stream but at `consumer punch enter tid=24878` in the persistent log; 20:02 ends at `consumer punch enter` in both; all three report `payload_exit=0` then `DEVICE=LOST`. 18:25 never planted (`KS_PUNCH_SKIPPED`): 255, alive. Twelve other runs: 255/124, alive | three losses, one per boot | **association, cause not established** | the punch is not instrumented past `consumer punch enter`; exit 0 is a transport property (`punch-path-audit.md` §2), not a payload status |
| 23 | The 20:02 loss is not explained by the dump collector or by a payload exit | `punch-path-audit.md` §2; the payload's own completion lines in `logs/20261103_200234.log` | a collector fault would leave a missing or gone child; a payload exit would print `main route done` and `pipe-physrw-summary` | collector reconciled before the punch (`accounting=ok`, `open_pipes=0`); no completion line exists; the constructor cannot return while `route_done` is unset | one cycle, one boot | **inferred from source, measured absence** | says nothing about what did happen at the punch, nor why the adb client returns 0 on a lost transport |
| 24 | The kernel work after `consumer punch enter` is the `sched_setattr` → `rt_mutex_adjust_pi` → chain walk, and it ends synchronously | source audit `punch-path-audit.md` §4; `core.c:5213`, `rtmutex.c:1126-1146` and `448-740`, file hashes in the document | every dereference of the planted waiter is inside that call; no work is deferred | mapped from source only; the run stops before the call returns | one source audit | **source-audited, execution unobserved** | which step ran, and whether the leaked page was still owned by the payload, are unknown |
| 25 | No payload exit path yields 255, so harness codes cannot be mapped to payload exits | `lifecycle-matrix.md` §2; `grep -rn "_exit(255)" src/` | every harness code corresponds to a source return or `_exit` | `main.c` returns 0, 3, 4 and the test paths `_exit(0/2)`; no source path produces 255, yet 255 appears on the twelve alive bails and 0 on the three losses | all 16 logs, one source pass | **source-audited** | the adb client's status mapping is still unexplained |
| 26 | The punch watchdog can outlive its parent and signal a reused pid | `lifecycle-matrix.md` §6; `main.c:34-52`, `main.c:72-79` | the child exits with the parent or checks `getppid()` before signalling | no parent-death guard, no `getppid()` check, stored pid, 90 s poll, then `kill(parent_pid, SIGKILL)`; `finish` waits for the child | source only, never exercised | **source-audited, not exercised** | whether a pid is reused inside 90 s on this device is unmeasured; it is a hazard statement, not an observed event |
| 27 | The route has no owner for its fake page, and its consumer and owner threads never terminate | `lifecycle-matrix.md` §3-4; `util.c:4530`, `main.c:173`, `main.c:204` | every return path releases or restores what it wrote, and each thread has a stop signal | no release or restore in the route; `punch_consume_stop` is only stored as 0; `owner_thread` loops forever | source only | **source-audited** | only reachable after a walk completes, which no lost run achieved |
| 28 | No recorded run has completed the punch, the route or the summary; the furthest event is `consumer punch enter` in 20:02 | `causal-timeline.md` §1-2; all 16 logs | a completed punch prints `consumer punch done`, a completed route `main route done`, a summary `pipe-physrw-summary` | zero occurrences of each in 16 logs; three runs reach the plant stage, one enters the punch, three are lost, thirteen survive | one extractor over every log | **measured** | says nothing about what happens after the punch returns; no post-return behaviour has a device observation |
| 29 | A run's host-stream boundary is its last *delivered* line; `pr_*` is `write()`+`fsync()`, so a line missing from the stream may still have been written (row 33) | `causal-timeline.md` §1; the logging path in `util.c`/`exlog` | the last line is the fault boundary; nothing after it is inferred | every host log ends at a line the next code step explains; 18:46's persistent log then continues four lines past it | one method applied to 16 logs, one cross-check on device | **measured, with one counter-example** | the stream cannot separate "not written" from "not delivered"; only `ks_log.txt` can |
| 30 | The 10:09 loss ended in a kernel panic | `bootreason_before` is `reboot,shell` at 10:09 and `kernel_panic,null` at 10:50, with no run between | the boot after the loss is a panic boot | exactly that transition; 15 later starts carry the same string | one transition, 16 logs | **inferred from the bootloader field** | the panic may have followed the payload's exit rather than occurred inside the plant; no unrecorded actor is excluded, only unsupported; no backtrace; the other two losses have no readable transition |
| 31 | Every run also writes each line, fsync'd, to `/data/local/tmp/ks_log.txt` | `exlog_writef` writes stdout, then `write`+`fsync`; `exlog open path=/data/local/tmp/ks_log.txt` in 16/16 logs | the file holds a pid-tagged, durable copy that outlives a panic | 16/16 opens; no `[pid][*]` line in any host log, so it was never retrieved | source + 16 logs | **verified as a mechanism; its content is unmeasured** | opened at offset 0 without truncation, so a later run leaves an earlier run's tail in place; write and fsync results are ignored |
| 32 | All three lost runs are kernel panics, and all three fault at the same PC | dropbox `_KP` entries 399/400/401, each with `upload_cause = KERNEL PANIC ( panic_msg = Oops PC = rt_mutex_adjust_prio_chain+0x304/0x858 )` | one entry per loss, same offset in each | 10:19:57, 19:04:48, 20:21:00; kernel `4.19.113-27095354` | one session, three entries | **measured** | the offset was pre-symbolization until row 36 resolved it; the record itself is a bootloader string, not a symbol |
| 33 | The 18:46 run planted and entered the punch; its host boundary was a delivery boundary | `ks_log.txt` block `[30624]`, lines 696–709 | four lines past `waiter futex returned`, ending `consumer punch enter tid=24878` | exactly those four lines, none of which the host stream carried | one pull, one block | **measured** | the block's earlier lines were overwritten by the 20:02 prefix, so its full transcript is not recoverable |
| 34 | The 20:02 run did not print `consumer punch first ret` | `ks_log.txt` block `[11056]` ends at `consumer punch enter tid=5202`, matching the host tail | persistence should equal delivery for this run | equal | one pull | **measured** | a line written after the last `fsync` would be invisible in both |
| 35 | The local kernel tree's `vmlinux` is a rebuild, not the device's build | `Linux version 4.19.113 (ubuntu@DESKTOP-…) … Wed Oct 28 06:17:31 CDT 2026` vs the device's `#1 SMP PREEMPT Fri Dec 27 14:53:28 KST 2024`; device `/proc/kallsyms` denied | its symbols and offsets may not be used to resolve the device's `+0x304` | version strings differ; BuildID `e17e6942…` has no device counterpart | one build, one device read | **measured** | nothing here proves the rebuild is layout-identical in any function, including this one |
| 36 | The device's `+0x304` is `ldr x8, [x8, #56]` in the stock image, the `w->lock` read inside `rt_mutex_top_waiter()` | byte chain `boot.img.lz4` → `boot.img` → `kernel.bin` (at `+0x1000`) → `vmlinux.elf`; symbol `ffffff8008146ca4`; `rtmutex_common.h:52-63` inlined at `rtmutex.c:661` | the next symbol is `0x858` later, which is the size the device printed | `f9401d08` at `ffffff8008146fa8`; `brk #0x800` at `+0x838` | one image, three independent agreements: build string, symbol size, `.kernel` bytes | **verified** | a static read: it says what the PC is, never what the registers held |
| 37 | The recorded PC is the load at `+0x304`, not the `BUG_ON` trap | the disassembly window in `symbol-resolution.md` §1 | a `BUG_ON` panic would report `+0x310` or `+0x838` | PC is `+0x304`; the trap is `brk #0x800` at `+0x838` | one image | **verified** | the oops status line that would name the abort class is not in any capture |
| 38 | At the panic, `lock->waiters.rb_leftmost` held a pointer that was not dereferenceable | `+0x2f8` reads `[lock,#0x18]` and succeeded; `+0x304` reads `[x8,#0x38]` and faulted | a mapped planted page would make the second read readable | the fault requires the word, the lock identity, or the mapping to have differed from the payload's intended image | source + one image | **derived, not measured** | candidates A–C in `symbol-resolution.md` §5; registers, `FAR` and `ESR` are unavailable, so none can be selected |
| 39 | At the fault, `x21` is a lock the kernel has just re-validated against a task's `pi_blocked_on`, and the tree-root word it reads is the planted one only on the first iteration | `register-dataflow.md`; image instructions at `+0x40`, `+0x8c-0x94`, `+0x2f8`; `rtmutex.c:507,518,537,585,613,661,759`; plant at `util.c:1876-1878,1911` | `x21 == task->pi_blocked_on->lock` before any tree read; `x8 = *(x21+0x18)` | the image enforces the check; the plant keeps `fake_lock`, `fake_w0` and the loaded field in one page | static, from the verified image | **verified from the image; runtime values unresolved** | the iteration is not identifiable from the PC, and the four families in `register-dataflow.md` §7 remain |

## Retractions

A retracted claim stays here with what replaced it. It is never deleted, and it
is never cited as support.

| # | claim as recorded | where | retracted by | status |
| --- | --- | --- | --- | --- |
| R1 | `build.sh` exits 0 after a compiler error | `build.md` before `99c440a`, and the pass report that introduced it | injected `#error`: `make: *** … Error 1`, exit **2**; the `0` came from a `grep` in the invoking pipeline | **retracted** — the script propagates the failure; the defect was in the observation |
| R2 | `cpu_partial=6` was measured | `slub-branch-audit.md` as committed in `ad993c7` | `mm_cpu_partial_limit()` re-derives it from the measured `objsize`; `/sys/kernel/slab/mm_struct/*` is `EACCES` | **retracted** — the value is derived (D), never read |
| R3 | The paused dump children's mapped scan state is the leading suspect for the device loss | `identity-table.md` as committed in `51cf642`, repeated in the pass reports | run 20:02: both children released their scan state, both alive at DONE with `open_pipes=0`, and the payload ran ~18 more minutes through the spray and punch before the device dropped; the 18:25 abort before the punch left it alive | **retracted** — the loss tracks the punch stage, not the collector (row 22) |
| R5 | The 18:46 run did **not** reach the punch (R4's retraction, and row 22 as of `8c261c9`) | R4 above; `causal-timeline.md` §1 as committed in `8c261c9` | the recovered `[30624]` block ends at `consumer punch enter tid=24878`, after four plant lines the host never received (`recovery-20261104.md` §3) | **retracted** — the R4 retraction rested on the host stream's last line, which was a delivery boundary; the original claim stands |
| R4 | The 18:46 run reached the punch stage, and `payload_exit=0` was a clean payload exit | the 20:02 pass report and row 22 as committed in `c9845a1` | 18:46's last payload line is `waiter futex returned` and no lost run contains a completion line; 3 lost runs report 0 while 13 survivors report 255/124, so 0 is a transport status (`punch-path-audit.md` §§1–2) | **retracted** — 18:46 stopped in the plant stage |

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
13. Row 22 is an association over three cycles, not a cause. It may justify
    instrumenting the punch stage; it may never be cited as a proven panic
    cause, and no punch run is a safe way to collect more evidence about it.
14. Row 23 fixes the boundary of a lost run: its last payload line is the
    furthest step observed, and `payload_exit` from a dead transport may
    never be cited as a payload status. A run whose payload cannot have
    exited says nothing about its exit path.
15. Row 24 is a source path, not an observed branch. It may guide
    instrumentation; it may never be reported as the step the device took.
16. Rows 25-27 are properties of the code, not causes of any measured
    loss. Rows 26 and 27 may never be cited as the reason the handset
    disappeared: the two plant-stage losses stop before the walk and the
    20:02 loss stops inside it, so neither a post-walk leftover nor an
    orphaned watchdog can explain them.
17. Rows 28-29 fix the accounting rule: a run's stage is its last *delivered*
    or *persisted* line, whichever is later, and no hazard may be attached to
    a run whose boundary is earlier than that hazard's code. The post-punch
    hazards in rows 26-27 therefore have no run to attach to — yet.
18. Row 31's file is the only record that separates "last delivered" from
    "last executed". A run's boundary stays a *delivery* boundary until that
    file's tail is read, or the loss is placed on a boot (row 30).
19. A symbol resolution must come from the image whose own kallsyms produced
    the offset, evidenced by the printed size. A same-version rebuild (row 35)
    can never be substituted, and its `System.map` cannot resolve an
    intra-function offset even in principle.
20. Row 36 names an instruction, never a value. No sentence may read the
    panic as proof that the planted page was reached, owned, or mapped: that
    requires `x21`, `x8`, `FAR` or a memory snapshot, and none exists.
21. Row 39 is a static constraint. It may narrow which pointer could have been
    wrong; it can never say that `x21` was `fake_lock`, that the page was owned by
    the program, or which family occurred. Only `x21`, `x8`, `FAR`, `ESR` or a memory
    snapshot could, and none was preserved.
22. Every address in the static documents is recomputed by `verify_symbol.py`
    from the target's `target.h` constants: the two planted words, the two
    values they must hold, and their distances. A distance quoted in prose is
    never authority, and a document that disagrees with the constants fails
    the verifier. Such an address is derived, never observed: `payload_base+0x2258`
    is where the load would read if both planted words held, not evidence that
    the CPU accessed it, mapped that page, or reached that instruction with
    those operands.
