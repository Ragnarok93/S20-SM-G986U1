# QA CI Status: dual-pile offline verification

Date: 2026-09-15 (Asia/Shanghai)  
Scope: offline PC self-check and static review only. No adb/device operation was
performed. Basis: `docs/adr/0001-mm-solve-model.md`, `docs/status.md`, and
`AGENTS.md`.

## Test Matrix

| # | Case | Result | Evidence | Key Output |
|---|---|---|---|---|
| 1 | Regenerate synthetic dual-pile dataset with `tools/make_dualpile_selfcheck.c` | PASS | `build/qa_dualpile_known.txt`, `build/dualpile_selfcheck_dataset.txt`, `build/dualpile_selfcheck_negative.txt` | `mm=0xffffffc002000000`, `bucket=218`, `collisions=8`, `negative_candidates=4` |
| 2 | Positive align8 search with `build/pc_candidate_solver_dualpile.exe` | PASS | `build/qa_dualpile_candidate.txt`, `build/qa_dualpile_candidate.stderr.txt` | `BEST MM=0xffffffc002000000 BUCKET=218 HITS=8/8 FOUND=1`, rc=0 |
| 3 | Positive 0x388/order-3 slab search with `build/pc_mm_slab_solver_dualpile.exe` | PASS | `build/qa_dualpile_slab.txt`, `build/qa_dualpile_slab.stderr.txt` | `BEST MM=0xffffffc002000000 BUCKET=218 HITS=8/8 FOUND=1`, rc=0 |
| 4 | Negative candidates excluded by align8 solver | PASS | `build/qa_dualpile_negative_candidate.txt`, `build/qa_dualpile_negative_candidate.stderr.txt` | 8GB scan, `HITS=0/4 FOUND=0`, rc=3 |
| 5 | Negative candidates excluded by slab388 solver | PASS | `build/qa_dualpile_negative_slab.txt`, `build/qa_dualpile_negative_slab.stderr.txt` | 8GB scan, `HITS=0/4 FOUND=0`, rc=3 |
| 6 | Solver ignores legacy `futex_addr=` | PASS | `build/qa_legacy_format_candidate.stderr.txt`, `build/qa_legacy_format_slab.stderr.txt`, `tools/pc_candidate_solver.c:77-91`, `tools/pc_mm_slab_solver.c:75-89` | Feeding `build/ks_collisions_run5_coll16.txt` produced `collision_unique=0`, rc=1 |
| 7 | `build/alioth/bin/preload.so` artifact identity | PASS | `build/alioth/bin/preload.so` | Size 208856 B; SHA256 `9BE36F9E8C0E54C221294DE5BBC2C8B21B956636B8531CE905F9BDD4B0486011` matches `docs/status.md` |

Commands actually used for the positive cases:

```text
build/pc_candidate_solver_dualpile.exe build/dualpile_selfcheck_dataset.txt \
  0xffffffc002000000 0xffffffc002000008 8 0x7ff 8

build/pc_mm_slab_solver_dualpile.exe build/dualpile_selfcheck_dataset.txt \
  0xffffffc002000000 0xffffffc002008000 0x388 3 0x7ff 8
```

Positive `CANDIDATE_STATS` for both solvers:

```text
collision_unique=8 candidate_unique=12 intersection=8
candidate_only_negative=4 collision_only=0
```

Negative `CANDIDATE_STATS` for both solvers:

```text
collision_unique=4 candidate_unique=0 intersection=0
candidate_only_negative=0 collision_only=4
```

## Solver Parsing

- Only lines beginning with `collision_addr=` enter the solving array.
- `candidate_addr=` is stored separately and is used only by the
  `CANDIDATE_STATS` loop; it does not affect `addrs[]`, the scan, or hit
  counting (`tools/pc_candidate_solver.c:70-114`,
  `tools/pc_mm_slab_solver.c:68-110`).
- Legacy `futex_addr=` falls into the final `else` and is skipped. The actual
  behavior is a silent skip, not an explicit parse error. This was confirmed
  with the old `run5_coll16` dataset: both new solvers returned rc=1 and
  reported zero parsed collisions or candidates.

## Export / Accepted Semantics

- `kernelsnitch.h` writes the target into `futex_addrs[0]`, the second-pile
  anchor into `futex_addrs[1]`, then appends only first-pile candidates that
  are also present in the second-pile candidate set
  (`src/kernelsnitch/kernelsnitch.h:541-562`).
- `accepted_idx[0]` is the target sentinel `(size_t)-1`; indices 1 and later
  identify futex slots for anchor and intersection entries
  (`src/kernelsnitch/kernelsnitch.h:494-511`).
- `util.c` exports exactly `accepted_count` rows as `collision_addr=`, using
  `futex_addrs[i]` and the timing entry indexed by `accepted_idx[i]`
  (`src/util.c:350-360`). It no longer exports the old `futex_addr=` form.
- `candidate_addr=` rows are separately emitted from the first pile with
  `pile=1` and from the second pile with `pile=2`
  (`src/util.c:362-374`).

## Resource Pairing

All newly added setup allocations have matching cleanup paths:

| Setup | Cleanup |
|---|---|
| `times2` (`kernelsnitch.h:361`) | `munmap` at `kernelsnitch.h:632-633` |
| `t_before2` (`kernelsnitch.h:362`) | `munmap` at `kernelsnitch.h:634-635` |
| `candidate1_idx` (`kernelsnitch.h:363`) | `munmap` at `kernelsnitch.h:636-637` |
| `candidate2_idx` (`kernelsnitch.h:364`) | `munmap` at `kernelsnitch.h:638-639` |
| `accepted_idx` (`kernelsnitch.h:365`) | `munmap` at `kernelsnitch.h:640-641` |

`pile2_anchor` and `pile2_anchor_id` are scalar state, not separate mappings.
The temporary first-pile baseline buffer is freed at
`src/kernelsnitch/kernelsnitch.h:443`.

## Evidence Checksums

| Evidence | Size | SHA256 |
|---|---:|---|
| `build/alioth/bin/preload.so` | 208856 | `9BE36F9E8C0E54C221294DE5BBC2C8B21B956636B8531CE905F9BDD4B0486011` |
| `build/alioth/bin/preload.so` (v15 corrected) | 209640 | `EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214` |
| `build/dualpile_selfcheck_dataset.txt` | 1307 | `F820B3450DC6247A2EB7A65E8A832681A5DCB75380EEE26773E381B6D4C4F8AE` |
| `build/dualpile_selfcheck_negative.txt` | 232 | `FA3EB581B3A59236AB895AADC09ECE82D56D327445F1E671DD71025A86F47175` |
| `build/qa_dualpile_candidate.txt` | 223 | `44CE1A957A330FB3DFE1C390FD64D58C15BDB4FCC3787B011090D527E89B1CB8` |
| `build/qa_dualpile_slab.txt` | 223 | `44CE1A957A330FB3DFE1C390FD64D58C15BDB4FCC3787B011090D527E89B1CB8` |
| `build/qa_dualpile_negative_candidate.stderr.txt` | 167 | `3CBC35FBC33D273758B0E1DF1265C5A8AD17608913BFD49EB191497CE3AFE03A` |
| `build/qa_dualpile_negative_slab.stderr.txt` | 167 | `3CBC35FBC33D273758B0E1DF1265C5A8AD17608913BFD49EB191497CE3AFE03A` |

## QA Notes

- The slab solver aligns its scan base with `start &= ~(slab_sz - 1)`
  (`tools/pc_mm_slab_solver.c:119-123`). Therefore an 8-byte scan window can
  contain zero slabs even when its start is the known mm. QA used the complete
  32KiB order-3 slab window for the positive slab test.
- This report records source/export behavior only. No device behavior is
  claimed; safe device validation remains blocked by milestone 2.

## 2026-09-15 M2 device round (v14)

Scope: one planned device run plus one automatic page retry. No BL unlock, no
successful root, and no reboot/panic.

| # | Step | Result | Evidence | Key Output |
|---|---|---|---|---|
| 1 | Check device | PASS | `build/m2_device_20260915_1400_summary.md` | `8c08f79` / `product:alioth` / `model:M2012K11AC` / `device` |
| 2 | Verify artifact and online deployment | PASS | `build/m2_device_20260915_1400_summary.md` | local and online v14 both 208856 B, SHA256 `9BE36F9E8C0E54C221294DE5BBC2C8B21B956636B8531CE905F9BDD4B0486011`; online manifest version=12 |
| 3 | Atomic clear answer/cache | PASS | `build/m2_device_20260915_1400_summary.md` | both files 0 bytes, mode 666 |
| 4 | Spawn v14 | PASS | `build/m2_ui_1341.png` | upload `208856/208856`, PID 26119 |
| 5 | First-pile timing | PASS | `build/m2_ui_1344.png` | timing rows with `before/after` values were printed |
| 6 | Second-pile validation | FAIL | `build/m2_ui_pidlog_1351.png` | `FUTEX_WAIT_PRIVATE` returned `EINVAL` |
| 7 | Export check | FAIL | `build/m2_device_20260915_1400_summary.md` | old `ks_collisions.txt` mtime/size unchanged (`1789377513 4580`) |
| 8 | Solver check | NOT RUN | `build/m2_device_20260915_1400_summary.md` | no new dual-pile dataset existed |
| 9 | Stop condition | PASS | `build/m2_ui_after_stop_1400.png` | Firefox force-stopped after second identical failure; device remained online |

Commands used:

```powershell
Get-FileHash -Algorithm SHA256 build\alioth\bin\preload.so
Copy-Item build\alioth\bin\preload.so D:\project\my_py_project\myroot\so\ghostlock_alioth_v14.so
git -C D:\project\my_py_project\myroot commit -m 'Bump alioth so to v14 (dual-pile)'
git -C D:\project\my_py_project\myroot push origin main
curl.exe -sL https://ccp-p.github.io/myroot/manifest.json
curl.exe -sL https://ccp-p.github.io/myroot/so/ghostlock_alioth_v14.so
adb devices
adb shell "cd /data/local/tmp; : > ks_mm_answer.tmp; : > ghostlock_ks_cache.tmp; chmod 666 ks_mm_answer.tmp ghostlock_ks_cache.tmp; sync; mv -f ks_mm_answer.tmp ks_mm_answer.txt; mv -f ghostlock_ks_cache.tmp ghostlock_ks_cache.txt; chmod 666 ks_mm_answer.txt ghostlock_ks_cache.txt; sync"
adb shell "pm clear org.mozilla.firefox"
adb shell "am start -a android.intent.action.VIEW -d 'https://ccp-p.github.io/myroot/?v=14' org.mozilla.firefox"
```

Actual adb binary used was
`D:\software\commonTool\scrcpy-win64-v2.7\adb.exe`, because
`tools-k40\platform-tools\adb.exe` is absent on this host.

Failure excerpt:

```text
SYSCHK(__futex(inc_arg->uaddr, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0)): Invalid argument
[INFO] running su -c id...
/system/bin/sh: /data/local/tmp/su: inaccessible or not found
```

M2 remains blocked. The next device round is not justified until the
second-pile anchor preconditions are checked offline.

## 2026-09-15 second-pile EINVAL audit and v15 build (offline)

Scope: source audit, minimal pile2 hardening, and NDK rebuild. No deployment,
no device round, and no image-file access.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Kernel semantics | PASS | 4.19 source: value mismatch is `-EWOULDBLOCK` (`build/linux-4.19-futex.c:2584-2603`); `FUTEX_WAIT` forces bitset ANY (`:3523-3528`) |
| 2 | Device kernel semantics | PASS | `kernel.elf`: `get_futex_key` misalignment returns `-22` at `0xffffff8008198454`; `futex_wait` zero bitset returns `-22` at `0xffffff8008194cfc`; `futex_wait_setup` value mismatch returns `-11` at `0xffffff8008197c9c` |
| 3 | v14 root cause | PASS | v14 disassembly stores the loop counter into `candidate1_idx` at `0x1a16c-0x1a188`, then uses `candidate1_idx[0]` as a `ks->futexes[]` byte offset at `0x1a25c-0x1a284`. This produces a misaligned anchor such as `base+167` |
| 4 | Corrected candidate indexing | PASS | v15 stores the futex byte offset `id` into `candidate1_idx` (`src/kernelsnitch/kernelsnitch.h:450-452`), so every pile2 anchor remains 8-byte aligned |
| 5 | Anchor preconditions | PASS | v15 validates candidate range, 4-byte pointer alignment, and zero value before pile2 (`src/kernelsnitch/kernelsnitch.h:475-504`) |
| 6 | Pile2 operation preservation | PASS | after root-causing the alignment bug, pile2 is restored to the same `FUTEX_WAIT_PRIVATE, 0` operation as pile1 (`src/kernelsnitch/kernelsnitch.h:189-192`, `:513-515`) |
| 7 | NDK rebuild | PASS | NDK r26b android34 clang with `@build_wakefix_dualpile.rsp`, built in an isolated HEAD-based M2 source tree to exclude concurrent M5 slide changes |

New artifact:

```text
build/alioth/bin/preload.so
size=209640
sha256=EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214
```

The corrected v15 artifact is local-only. M2 safe-device validation remains
blocked until this build is deliberately scheduled for a new round.

## 2026-09-15 v15 QA recheck (offline)

Scope: post-push QA recheck of commit `d978b30`. No device was touched. The
rerun regenerated the self-check datasets and reused the committed dual-pile
solvers; rerun evidence is byte-identical to the prior QA evidence.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Candidate indices store futex byte offsets, not loop counters | PASS | `src/kernelsnitch/kernelsnitch.h:450-452` |
| 2 | Anchor selection checks range, 4-byte pointer alignment, and value | PASS | `src/kernelsnitch/kernelsnitch.h:480-489` |
| 3 | Pile2 uses the same `FUTEX_WAIT_PRIVATE, 0` op/val/val3 as pile1 | PASS | `src/kernelsnitch/kernelsnitch.h:189-192`, `:513-515` |
| 4 | Regenerate self-check dataset | PASS | `build/dualpile_selfcheck_dataset.txt`, `build/dualpile_selfcheck_negative.txt` |
| 5 | Positive align8 solver | PASS | `build/qa_rerun_candidate.txt`: `HITS=8/8 FOUND=1`, rc=0 |
| 6 | Positive slab388 solver | PASS | `build/qa_rerun_slab.txt`: `HITS=8/8 FOUND=1`, rc=0 |
| 7 | Negative candidates excluded by align8 solver | PASS | `build/qa_rerun_negative_candidate.stderr.txt`: `HITS=0/4 FOUND=0`, rc=3 |
| 8 | Negative candidates excluded by slab388 solver | PASS | `build/qa_rerun_negative_slab.stderr.txt`: `HITS=0/4 FOUND=0`, rc=3 |
| 9 | v15 artifact identity | PASS | size 209640; SHA256 `EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214` |
| 10 | mmap/munmap pairing for new dual-pile state | PASS | setup: `:352-365`; cleanup: `:631-641` |
| 11 | Anchor write remains value normalization only | PASS | `*anchor_word=0` only when nonzero, before pile2 (`:488-489`) |
| 12 | M2 artifact isolation from uncommitted M5 slide changes | PASS (indirect) | working-tree `git diff` has H1A additions in `slide.c`; artifact has no `h1a`/`H1A` symbols or strings; build used an isolated HEAD-based tree per v15 rebuild record |

Rerun commands used the same positive windows as the prior matrix and 8GB
negative scans with the solver-default `min_hits=7`:

```text
pc_candidate_solver_dualpile.exe dualpile_selfcheck_negative.txt 0x0 0x200000000 8 0x7ff
pc_mm_slab_solver_dualpile.exe dualpile_selfcheck_negative.txt 0x0 0x200000000 0x388 3 0x7ff
```

Evidence checksums:

```text
dualpile_selfcheck_dataset.txt F820B3450DC6247A2EB7A65E8A832681A5DCB75380EEE26773E381B6D4C4F8AE
dualpile_selfcheck_negative.txt FA3EB581B3A59236AB895AADC09ECE82D56D327445F1E671DD71025A86F47175
qa_rerun_candidate.txt 44CE1A957A330FB3DFE1C390FD64D58C15DBD4FCC3787B011090D527E89B1CB8
qa_rerun_slab.txt 44CE1A957A330FB3DFE1C390FD64D58C15DBD4FCC3787B011090D527E89B1CB8
qa_rerun_negative_candidate.stderr.txt 3CBC35FBC33D273758B0E1DF1265C5A8AD17608913BFD49EB191497CE3AFE03A
qa_rerun_negative_slab.stderr.txt 3CBC35FBC33D273758B0E1DF1265C5A8AD17608913BFD49EB191497CE3AFE03A
```

The isolation check is indirect: the local working tree still contains the
M5 slide edits, but the shipped M2 artifact contains neither H1A symbols nor
H1A strings. It does not independently reconstruct the isolated build tree.

## 2026-09-15 v16 QA recheck (offline)

Scope: post-push QA recheck of the v16 artifact. No device was touched.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | v16 artifact identity | PASS | `build/alioth/bin/preload_v16.so`: size 209688; SHA256 `E58CA62E1803D2E23280B8C3216AEF801D206DD7D149B045A0213BEF2BD7D749` |
| 2 | Build tree uses current `src/util.c` with `idx` / `KS_PAGE_SIZE` export semantics | PASS | `build/m2_v16_src_20260915/src/util.c:341-373` is identical to the working-tree file; artifact contains the new `configured_collisions=%zu`, `accepted_collisions=%zu`, and pile-tagged `candidate_addr=` format strings |
| 3 | Build tree uses the `d978b30` `kernelsnitch.h` | PASS | `git diff --no-index` between `d978b30:src/kernelsnitch/kernelsnitch.h` and `build/m2_v16_src_20260915/src/kernelsnitch/kernelsnitch.h` is clean; `build_wakefix_dualpile_v16.rsp` targets `preload_v16.so` |
| 4 | Regenerate dual-pile self-check dataset | PASS | `build/dualpile_selfcheck_dataset.txt`, `build/dualpile_selfcheck_negative.txt`; hashes unchanged from prior QA |
| 5 | Positive align8 solver | PASS | `build/qa_v16_candidate.txt`: `HITS=8/8 FOUND=1`, rc=0 |
| 6 | Positive slab388 solver | PASS | `build/qa_v16_slab.txt`: `HITS=8/8 FOUND=1`, rc=0 |
| 7 | Negative candidates excluded by align8 solver | PASS | `build/qa_v16_negative_candidate.stderr.txt`: `HITS=0/4 FOUND=0`, rc=3 |
| 8 | Negative candidates excluded by slab388 solver | PASS | `build/qa_v16_negative_slab.stderr.txt`: `HITS=0/4 FOUND=0`, rc=3 |
| 9 | M2 v15 run1 pile2 anchor / 16 accepted cross-log consistency | FAIL | `build/ks_log_m2_v15_run1.txt:111` has `found 16 accepted collisions (pile1=128 pile2=128)` and crashlog pid 31647 has SIGSEGV at `0x6fa1029140`; however that run log contains no `pile2 anchor ... align=0 value=0` line. The only matching anchor lines are `build/ks_log_h1a_run1.txt:141` pid 17359 and `build/ks_log_h1a_run3.txt:154` pid 20836 |

Conclusion: v16 artifact identity, source-tree provenance, and dual-pile
regression are PASS. The requested v15 log-consistency claim is not supported
by the named file and remains FAIL until the matching anchor-bearing M2 log
or corrected cross-reference is supplied.

## 2026-09-15 M2 v15 device round and v16 export rebuild

Scope: deployed v15, ran the approved M2 device round, collected logs, and
rebuilt an offline v16 after the export defect was found. No image-file access.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Online v15 identity | PASS | manifest version 13; `so/ghostlock_alioth_v15.so`; size 209640; SHA256 `EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214` |
| 2 | Device online | PASS | `8c08f79` remained connected |
| 3 | Pile2 EINVAL fix | PASS | log shows `pile2 anchor id=6574632 addr=0000006f5e445228 align=0 value=0` |
| 4 | Dual-pile completion | PASS | log reaches `found 16 accepted collisions (pile1=128 pile2=128)` |
| 5 | Collision export | FAIL | `ks_collisions.txt` stayed 0 bytes; SIGSEGV in `ks_export_collisions` |
| 6 | Solver | NOT RUN | no exported `collision_addr=` rows |
| 7 | M2 boundary | PASS | no answer backfill, no punch/fops, no extra retry |
| 8 | v16 offline rebuild | PASS | isolated HEAD tree plus working `src/util.c`; size 209688 |

Evidence files:

```text
build/ks_log_m2_v15_run1.txt
build/ks_collisions_m2_v15_run1.txt
build/m2_v15_run1_crashlog.txt
```

The crashlog locates the fault in `ks_export_collisions`; disassembly of the
v15 binary shows the candidate index being used directly as the timing-array
index. v16 now divides the byte offset by `KS_PAGE_SIZE` before indexing.

Offline v16 artifact:

```text
build/alioth/bin/preload_v16.so
size=209688
sha256=E58CA62E1803D2E23280B8C3216AEF801D206DD7D149B045A0213BEF2BD7D749
```

v16 is local-only and was not deployed.

## 2026-09-15 H1A run4 model-fit CI

Scope: offline-only hash-model discrimination. No device access, no deploy,
no answer backfill.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Model-fit tool builds | PASS | `tools/pc_model_fit.c`, `build/pc_model_fit.exe` |
| 2 | Positive self-check | PASS | `build/dualpile_selfcheck_dataset.txt` finds the known mm/bucket |
| 3 | Run4 16-address full scan | FAIL | 144 variants, 0 hits at `>=14/16` |
| 4 | Run4 15-address scan excluding target | FAIL | 144 variants, 0 hits at `>=14/15` |
| 5 | Run4 14-address dual-pile intersection scan | FAIL | 144 variants, 0 hits at `>=14/14` |
| 6 | Historical mm bucket check | FAIL | `0xffffffc1b1cfb840` gives all-unique buckets under the exact model |
| 7 | Binary key-order recheck | PASS | ADR-0001 updated to `mm@0/address@8` |

Artifacts:

```text
build/model_fit_20260915/scan_summary_16.txt
build/model_fit_20260915/scan_summary_15_excl_target.txt
build/model_fit_20260915/scan_summary_14_intersection.txt
build/model_fit_20260915/historical_mm_16.tsv
build/model_fit_20260915/kernel_futex_wake.dis
build/model_fit_20260915/kernel_get_futex_key.dis
```

## 2026-09-16 H1A-S3 pre-read gate QA

Scope: source-level Gate 4/5 implementation, NDK r26b rebuild, and offline
artifact verification only. No adb/device operation, on-device run, retry, or
push was performed.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Gate placement | PASS | `src/slide_s3.c`: S3-only helper runs after the ashmem handle marker and before `s2_tcp_gate()`; S2 never reaches it |
| 2 | Misc target pre-read | PASS | requires an 8-byte read, a kernel pointer, and `value != fake_fops`; emits exact `misc pre-read ... gate=PASS` only after all checks |
| 3 | Parent-left pre-read | PASS | requires an 8-byte read and `value != fake_w0+0x18`; emits exact `parent-left pre-read ... right-gate=PASS` only after all checks |
| 4 | Fail-closed order | PASS | either gate failure returns before TCP setup, watchdog creation, thread arming, sendmsg, punch, and S3 write/readback |
| 5 | NDK r26b rebuild | PASS | `aarch64-linux-android34-clang.cmd @build_s3.rsp`; only the pre-existing `kernelsnitch.h` unused `ks` warning |
| 6 | Dynamic export surface | PASS | `llvm-nm -D --defined-only` exposes only `T main` |
| 7 | Marker strings in artifact | PASS | both PASS marker strings are present in the DSO |
| 8 | Artifact identity | PASS | size 79904 B; SHA256 `AB44CE0AA39BA9BAECA14ADE4AA07903E9DDBB5A7B3517A4EF0DB54D93351813`; entry `0x106A8` |

Canonical artifact provenance:

```text
build/alioth/bin/preload_slide_h1a_s3.so
size=79904
sha256=AB44CE0AA39BA9BAECA14ADE4AA07903E9DDBB5A7B3517A4EF0DB54D93351813
entry=0x106A8
```

## 2026-09-17 closed-loop binding QA (same-process mm loop)

Scope: offline pre-audit only. No adb/device command, no `src/` change, and
no device run was performed. Deliverable:
`docs/specs/closed-loop-binding-qa.md`.

| # | Item | Result | Evidence |
|---|---|---|---|
| 1 | Phase 1 collector selection | S3 collect v15 base + minimal `BUCKET_COLLECT_V1` export patch required; v10.5 and M2 v15/v16 rejected as bases | `docs/specs/closed-loop-binding-qa.md` section 1; `bucket_collect_v10_5.c:1249,1301,1309-1316` (no t2c), S3 collect `util.c:366,514-518` (live t2c/c2t/c2c), main `util.c:331,359-399` (no votes), `docs/status.md:145-190` (M2 v15/v16) |
| 2 | Binding contract test plan | Defined for 8-field answer, 22/23+45 B, 94 B gate, manifest, boot lock, env override, phase binding, drain, heartbeat, smoke separation | `docs/specs/closed-loop-binding-qa.md` sections 2.1-2.10 |
| 3 | Host-side byte-contract matrix | 29 cases (11 companion + 18 run22 gate), all offline-executable specs, no implementation written | `docs/specs/closed-loop-binding-qa.md` sections 3.1-3.2 |
| 4 | Device authorization | Blocked pending code-level QA of binding-artifact generation (design-review Approve condition 7) | `ghostlock-s3-run17-analysis/docs/specs/same-process-mm-closed-loop-design-review.md` |

## 2026-09-17 closed-loop phase controller DSO (offline)

Scope: offline implementation and build/QA only. No adb/device command, no
device run, no `KS_MM_TEST`, and no main-chain behavior change.

| # | Check | Result | Evidence / notes |
|---|---|---|---|
| 1 | S3 collect v15 reuse boundary | PASS | `src/closed_loop/util.c` keeps pile/timing/vote/drain/join flow and adds only in-process `BUCKET_COLLECT_V1` rendering; live `t2c/c2t/c2c` is captured in `closed_loop_collector_state()` before cleanup and is not reparsed from the bucket |
| 2 | Phase controller state machine | PASS | `src/closed_loop/phase_controller.c` runs SPAWN -> COLLECT -> WAIT_ANSWER -> VALIDATE -> ROOTCHAIN in one process; no exec/fork-of-self; rechecks boot_id, bucket SHA256, PID/TGID, and `/proc/self/stat` starttime at phase boundaries |
| 3 | Fail-closed waiting | PASS | answer wait is bounded at 15 minutes with a 5-second heartbeat and a 15-minute phase budget; phase FAIL is one-shot and does not retry |
| 4 | Renderer ordering | PASS | mm answer, 45 B companion, and 94 B run22 gate are rendered only after answer validation PASS; run22 uses the selected live collector edge |
| 5 | Private-directory adaptation | PASS | controller rejects `/data/local/tmp` binding paths and canonicalizes/resolves the private parent before acceptance; rootchain path env defaults and gate logic remain unchanged |
| 6 | Host byte-contract QA | PASS | 29/29 cases: C45 11/11, C94 18/18; output in `build/closed_loop_binding_selftest.txt` |
| 7 | Closed-loop DSO build | PASS | NDK r26b API 34 `@build_closed_loop.rsp`; size 215856 B; SHA256 `6B1A84390DD81E7D820678B5BF1ED8E090314F21DA80A6611AC7D1ABCAC4C5A9` |
| 8 | Rootchain regression build | PASS | `@build_rootchain.rsp`; only the pre-existing unused `ks` warning; SHA256 `931616C603242AEA75D1C557251E4BF1407036D45C1B8488E85CA0135672E299` |

Build provenance:

```text
build/alioth/bin/preload_closed_loop.so
size=215856
sha256=6B1A84390DD81E7D820678B5BF1ED8E090314F21DA80A6611AC7D1ABCAC4C5A9
```

## 2026-09-17 closed-loop audit rework (offline)

Scope: reviewer rework for watchdog budgets, rootchain manifest provenance,
binding reuse, private-path canonicalization, render cleanup, and live edge
pass-through. No adb/device command, on-device run, or boot retry occurred.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Per-phase watchdog budgets | PASS | collect=600s, wait-answer=900s, validate=600s, rootchain=300s; `alarm(0)` follows rootchain completion (`src/closed_loop/phase_controller.c`) |
| 2 | Heartbeat contract | PASS | heartbeat logs and re-arms only the current phase's full budget; no 15-second false watchdog |
| 3 | Binding phase recheck | PASS | compares boot_id, bucket SHA256, PID/TGID, and stat starttime only; does not compare the mutable `/proc/self/stat` line |
| 4 | Private-path canonicalization | PASS | absolute path, private prefix, explicit `..` rejection, non-link/non-directory final component handling, real directory parent, and resolved parent prefix check in `src/closed_loop/util.c` |
| 5 | Render-time private-path check | PASS | answer/companion/gate render and atomic write recheck the path before touching it |
| 6 | Render-failure cleanup | PASS | all prior binding artifacts are removed before rendering and removed again on partial failure |
| 7 | Live run22 evidence | PASS | renderer receives t2c/c2t/c2c PASS booleans from the live collector edge state, not hardcoded values |
| 8 | Rootchain manifest provenance | PASS | actual ELF entry `0x25950` from llvm-readelf for the 177936-byte artifact |
| 9 | Closed-loop NDK build | PASS | size 215856; SHA256 `6B1A84390DD81E7D820678B5BF1ED8E090314F21DA80A6611AC7D1ABCAC4C5A9` |
| 10 | Rootchain NDK build | PASS | size 177936; SHA256 `931616C603242AEA75D1C557251E4BF1407036D45C1B8488E85CA0135672E299` |
| 11 | Binding selftest | PASS | `TOTAL=29 PASS=29 FAIL=0` in `build/closed_loop_binding_selftest_rework.txt` |
| 12 | Rootchain offline selfcheck | PASS | artifact size/hash/entry, 22 ordered markers, and strict mode all PASS in `build/rootchain_selfcheck_rework.txt` |

Canonical reworked artifact provenance:

```text
closed-loop size=215856 sha256=6b1a84390dd81e7d820678b5bf1ed8e090314f21da80a6611ac7d1abcac4c5a9
rootchain size=177936 sha256=931616c603242aea75d1c557251e4bf1407036d45c1b8488e85ca0135672e299
rootchain entry=0x25950
```

The rootchain bridge artifact intentionally remains byte-identical because this
rework did not change its sources. Its manifest was corrected to reference the
actual current artifact and ELF entry, and `tools/rootchain_selfcheck.py` now
passes.
