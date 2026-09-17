# Closed-loop binding QA plan

Date: 2026-09-17. Role: Architect/QA pre-audit. Offline only: no device
commands, no `src/` changes. Inputs reviewed:
`ghostlock-s3-run17-analysis/docs/specs/same-process-mm-closed-loop-design-review.md`,
`docs/specs/rootchain-run22-gate-audit.md`, both repos' collector sources,
and the run32/run33 evidence in `ghostlock-s3-run17-analysis`.

## 1. Phase 1 collector selection

**Verdict: none of the existing collectors satisfies the closed-loop Phase 1
requirements as-is. Base the closed-loop collector on the S3 collect v15
lineage (`ghostlock-s3-run17-analysis`, `build_s3_collect_v15.rsp`) and add a
minimal in-process `BUCKET_COLLECT_V1` export patch.**

Version disambiguation: "v15/v16" exists twice in this project. The M2
preload `preload.so` -> `so/ghostlock_alioth_v15.so`/`preload_v16.so`
(main repo, `docs/status.md:145-190`) is the JIT-chain kernelsnitch
preload, not a closed-loop collector. The S3 collect build v15
(`build_s3_collect_v15.rsp` -> `preload_slide_h1a_s3_collect_v15.so`) is
the run32 collection artifact.

| Candidate | Bucket export for offline solver | Live t2c/c2t/c2c for run22 gate | Verdict |
|---|---|---|---|
| v10.5 collector (`bucket_collect_v10_5.c`, `build_bucket_collect_v10_5.rsp`) | Yes, format only: emits `BUCKET_COLLECT_V1` (`src/bucket_collect_v10_5.c:1249`) with probe/pair rows (`:1309-1316`) | No: the file contains zero `t2c`/`c2t`/`c2c` occurrences; its model is shared-key calibration (`phase=shared-calib`, `:1301`; solver initval `offset|1` in `tools/shared_futex_corrected_scan.c:63`), not the raw-private victim model | Reject as base |
| S3 collect v15 (`build_s3_collect_v15.rsp`: `s3_main.c`, `slide_s3.c`, `util.c`, kernelsnitch) | No: exports `ks_collisions` rows with per-candidate votes (`src/util.c:366`), no `BUCKET_COLLECT_V1` writer; run32's bucket input was converted offline | Yes: `t2c/c2t/c2c` three-round votes per candidate (`src/util.c:366`), `S3-COLLECT verdict` (`:514-518`); v15 carries the pile tid drain/join fixes (`ghostlock-s3-run17-analysis/docs/status.md`, run30e/run32 section) | Accept as base, needs minimal export patch |
| M2 kernelsnitch v15/v16 (main repo `src/main.c` + `src/kernelsnitch/` + `src/util.c`) | No: `ks_export_collisions` writes the old `collision_addr=... t_before/t_after` format without votes (`src/util.c:331,359,363,372-399`); v15 crashed in export and v16 only fixed the index bug (`docs/status.md:145-190`) | No vote rows in the export | Reject |
| Main repo `src/slide_s3.c` | Not a collector: it is the deterministic S3 write chain used by the rootbridge; no bucket or vote export markers | n/a | n/a |

Reasons and implications:

1. The run22 gate requires live `t2c/c2t/c2c` evidence from the same
   process and mm (`docs/specs/rootchain-run22-gate-audit.md`, gate 8).
   Only the S3 collect lineage produces those vote groups on device.
2. The run32 lesson is directly reusable: `ks_collisions` votes were
   converted offline into `build/run32_as_bucket_input.tmp` and solved
   UNIQUE by `tools/pc_bucket_solver.c --stride 0x40`
   (`ghostlock-s3-run17-analysis/docs/qa-run32-candidate-verification.md`).
   The closed-loop patch must move that conversion in-process (port the
   v10.5 `export_result` writer, `bucket_collect_v10_5.c:1226-1341`, to
   render self-mm raw-private `class=c###` probe/pair rows) so the hashed
   bucket file is exactly what the offline solver consumes.
3. The patch is export-only: no pile/timing/vote logic changes. Drain,
   join, fd-baseline, and one-shot rules from S3 collect v15 are reused
   unchanged.

## 2. Binding contract test plan

All tests are host-side offline: parse/verify functions are exercised with
fixture files; no device interaction. Every FAIL case must produce the
exact fail marker from `docs/specs/rootchain-run22-gate-audit.md`.

### 2.1 BUCKET_ANSWER_V1 (8 fields, fail-closed)

Fields: `answer`, `boot_id`, `model`, `futex_hashsize`,
`bucket_file_sha256`, `candidates`, `ambiguous`, `answer_verdict`.

1. Positive: all 8 fields, device boot_id, hash of the round's bucket
   file, `candidates=1`, `ambiguous=0`, `answer_verdict=UNIQUE` -> accept.
2. Extra 9th field -> reject (fail-closed on extras).
3. Any missing field -> reject.
4. `boot_id` != current boot -> reject.
5. `bucket_file_sha256` != recomputed hash of exported bucket file -> reject.
6. `answer` not 16 lowercase hex -> reject.
7. `candidates` != 1 or `ambiguous` != 0 -> reject.
8. `answer_verdict` != UNIQUE -> reject.
9. `model` != `raw-private` -> reject.
10. `futex_hashsize` != `0x800` -> reject.

### 2.2 mm answer (22/23 B) and 45 B companion

Wire formats per `docs/specs/rootchain-run22-gate-audit.md` gates 4-6.

1. Positive 22 B `mm=0x<16lowerhex>\n` -> accept.
2. Positive 23 B CRLF variant -> accept.
3. Other lengths -> reject.
4. Companion exactly 45 B: `boot_id=<36 canonical lowercase UUID>\n` ->
   accept only when byte-identical to current `/proc/sys/kernel/random/boot_id`.
5. Companion UUID from another boot, non-canonical UUID, wrong prefix,
   length != 45, or missing LF -> reject.
6. Companion or mm mtime older than boot btime -> reject (boot scope).

### 2.3 94 B run22 gate

Layout: `boot_id=` (0-7), UUID (8-43), LF (44), `mm=0x` (45-49), 16 hex
(50-65), LF (66), `t2c=PASS` (67-74), LF (75), `c2t=PASS` (76-83), LF
(84), `c2c=PASS` (85-92), LF (93). Accept only when all four segments
match: current boot, accepted mm answer, and all three vote groups PASS.

### 2.4 Provenance manifest

1. Positive: `size=`, `sha256=` (64 hex), `entry=0x<off>` each on its own
   line; artifact via `ROOTCHAIN_ARTIFACT_PATH` matches size and sha -> accept.
2. Size mismatch, sha mismatch, non-hex sha, missing/zero entry, fields
   merged on one line -> reject each.
3. Artifact path missing or non-regular -> reject.

### 2.5 Boot lock

1. Absent -> bridge creates `boot_id=<...>\npid=<...>\n` and proceeds.
2. Same boot present -> refuse (`prior_attempt_boot`), no retry.
3. Different boot present -> treat as fresh boot, overwrite.
4. Phase controller must check lock absence before validate; presence is FAIL.

### 2.6 Private-dir env override

`ROOTCHAIN_MM_PATH`, `ROOTCHAIN_MM_BOOT_ID_PATH`,
`ROOTCHAIN_RUN22_GATE_PATH`, `ROOTCHAIN_MANIFEST_PATH`: env set -> private
path used; env unset or empty -> keep the current `/data/local/tmp`
defaults. Gate logic and byte contracts unchanged.

### 2.7 Phase binding

Captured at spawn, rechecked before validate and before rootchain:
`boot_id` equal; bucket-file SHA-256 equal; PID/TGID equal; at least one
of start time (`/proc/self/stat` field 22) or full stat line equal. A
same-PID-different-starttime scenario (PID reuse) must reject.

### 2.8 Collector drain / no residue

1. All pile tids joined (v15 handshake), worker count back to baseline.
2. fd count back to spawn baseline.
3. Collection mmaps/pipes unmapped/closed.
4. Any residue -> FAIL before validate.

### 2.9 Heartbeat

1. `MMLOOP heartbeat phase=<p> tick=<n>` at least every 15 s during
   wait-answer and between phase boundaries.
2. Two consecutive missed heartbeats -> watchdog FAIL, capture log tail,
   no restart in the same boot.

### 2.10 Smoke / closed-loop separation

1. `KS_MMLOOP_MODE=smoke` runs emit only `MMLOOP smoke ...` markers,
   never export a bucket file or answer.
2. Candidate validation records require `KS_MMLOOP_MODE=closed-loop` and
   the full phase marker sequence.

## 3. Host-side byte-contract test matrix (45 B / 94 B)

### 3.1 mm boot-id companion (45 B)

| ID | Case | Fixture | Expected |
|---|---|---|---|
| C45-01 | Positive | current UUID, exact 45 B | PASS |
| C45-02 | Short length | 44 B (missing LF) | reject `len` |
| C45-03 | Long length | 46 B (extra byte) | reject `len` |
| C45-04 | Wrong prefix | `bootid=` prefix | reject format |
| C45-05 | Stale boot | previous boot's UUID | reject boot_scope |
| C45-06 | Non-canonical UUID | uppercase / missing dashes | reject boot_scope |
| C45-07 | CRLF | 46 B with CRLF | reject `len` |
| C45-08 | No LF | `boot_id=<36>` only (44 B) | reject `len` |
| C45-09 | mtime boundary | mtime == boot btime | PASS (boundary inclusive) |
| C45-10 | mtime stale | mtime == btime - 1 | reject mtime |
| C45-11 | Template reuse | byte copy of a previous round's file | reject (boot mismatch) |

### 3.2 run22 gate (94 B)

| ID | Case | Fixture | Expected |
|---|---|---|---|
| C94-01 | Positive | current boot + accepted mm + three PASS | PASS |
| C94-02 | Short length | 93 B (missing final LF) | reject `len` |
| C94-03 | Long length | 95 B (extra field byte) | reject `len` |
| C94-04 | Stale boot | old boot UUID in segment 1 | reject format |
| C94-05 | mm mismatch | mm != accepted answer | reject format |
| C94-06 | mm case | uppercase hex digits | reject format |
| C94-07 | t2c downgraded | `t2c=FAIL` | reject |
| C94-08 | c2t downgraded | `c2t=FAIL` | reject |
| C94-09 | c2c downgraded | `c2c=FAIL` | reject with explicit c2c marker |
| C94-10 | c2c omitted | 85 B (no c2c segment) | reject `len` |
| C94-11 | Extra field appended | 94+N B | reject `len` |
| C94-12 | Field reorder | mm segment first | reject format |
| C94-13 | Rejected mm | historical rejected-mm value | reject via mm gate |
| C94-14 | Gate mtime stale | gate mtime < btime, mm fresh | reject mtime |
| C94-15 | mm mtime stale | mm mtime < btime, gate fresh | reject mtime |
| C94-16 | Template reuse | byte-identical copy from previous round | reject |
| C94-17 | Case attack | `t2c=pass` lowercase | reject format |
| C94-18 | Blank line spliced | extra `\n` inside, 94 B re-padded | reject format |

Total byte-contract cases: 11 (C45) + 18 (C94) = 29.

## 4. Execution notes

- Tests run against the parser functions extracted for QA (host build);
  the rootchain implementation is not modified in this audit.
- mtime boundary tests set file mtimes explicitly (`utimensat` host-side
  or fixture timestamps) relative to a recorded btime constant.
- Template-reuse detection additionally requires the phase controller to
  render gate files from live phase variables; a stored template file is
  itself a QA failure of the generation path.

## 5. Status

| Item | Result |
|---|---|
| Collector selection | S3 collect v15 base + minimal BUCKET_COLLECT_V1 export patch (see section 1) |
| Binding contract test plan | Defined, sections 2.1-2.10 |
| Byte-contract matrix | 29 cases defined, sections 3.1-3.2 |
| Device execution | Not authorized by this document; blocked on Approve condition 7 of the design review |
