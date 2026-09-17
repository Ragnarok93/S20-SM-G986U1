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
| 3 | Fail-closed waiting | PASS | answer wait is bounded at 15 minutes with 5-second heartbeat and 15-second watchdog; phase FAIL is one-shot and does not retry |
| 4 | Renderer ordering | PASS | mm answer, 45 B companion, and 94 B run22 gate are rendered only after answer validation PASS; run22 uses the selected live collector edge |
| 5 | Private-directory adaptation | PASS | controller rejects `/data/local/tmp` binding paths; rootchain path env defaults and gate logic remain unchanged |
| 6 | Host byte-contract QA | PASS | 29/29 cases: C45 11/11, C94 18/18; output in `build/closed_loop_binding_selftest.txt` |
| 7 | Closed-loop DSO build | PASS | NDK r26b API 34 `@build_closed_loop.rsp`; size 214632 B; SHA256 `7E453F63403554930A69D83C3DC6AE15C3BD0DA8B45B95FAFE67489F34771614` |
| 8 | Rootchain regression build | PASS | `@build_rootchain.rsp`; only the pre-existing unused `ks` warning; SHA256 `931616C603242AEA75D1C557251E4BF1407036D45C1B8488E85CA0135672E299` |

Build provenance:

```text
build/alioth/bin/preload_closed_loop.so
size=214632
sha256=7E453F63403554930A69D83C3DC6AE15C3BD0DA8B45B95FAFE67489F34771614
```
