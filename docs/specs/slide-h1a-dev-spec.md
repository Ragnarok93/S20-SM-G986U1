# Slide H1A Dev Spec (M5) — for the development agent

Read first: ADR-0003, `docs/specs/slide-h1a-audit-report.md`,
`docs/specs/slide-revival-test-plan.md`, AGENTS.md (Operating Safety).

## Hard Boundaries

- Do NOT edit `src/kernelsnitch/*`, `src/util.c`, or any M2-owned doc
  section (status.md M2/v15, ci-status.md). They are owned by another
  track in flight.
- Do NOT overwrite `build/alioth/bin/preload.so` — it is the M2 carrier.
- Default (no-env) behavior of the binary must remain byte-for-byte
  semantically identical for the M2 route.

## Tasks

D-1: Answer audit Q-1 (KASLR state on alioth) offline; append the answer
and evidence to `slide-h1a-audit-report.md`.

D-2: Implement an env-gated H1A punch test mode (`KS_H1A_TEST=1`):
- reuse existing components: fake page prepare (fops payload), v12
  ready-bitmap waiter encoding, terminal-form rb nodes, sched_setattr
  punch nice=1, punch watchdog;
- success = punch completes without spin; log markers:
  `H1A punch enter`, `H1A punch returned`, `H1A load=<n>`;
- exit after punch verdict; do NOT proceed to root stage;
- all logic lives in `src/slide.c`; `src/main.c` insertion must be <= 6
  lines at a single marked point, env-gated, so M2's path is untouched;
- update the stale "slide is dead" comment at `src/main.c:286-288` to
  reference ADR-0003.

D-3: Create `build_slide_h1a.rsp` modeled on `build_wakefix.rsp`,
output `build/alioth/bin/preload_slide_h1a.so`.

D-4: Log header must print self-audit markers confirming: v12 encoding
active, terminal rb nodes, nice=1, delta=0 (no ks_delta_index read).

## FD Protect Boundary (observed)

`open_slide_selected_fds()` dup2-sweeps fds 0..319
(`PSELECT_ROUTE_NFDS=320`). Long-lived fds below 320 are clobbered by
that sweep. Affected holders as of this note:

- `g_exlog_fd` (util.c) — relocated to `>= KS_FD_PROTECT_MIN`
  (`PSELECT_ROUTE_NFDS + 64 = 384`) at the top of
  `open_slide_selected_fds()` via `exlog_protect_fd()`.
- main-route punch watchdog parent write end (main.c
  `start_punch_watchdog`) — relocated immediately after
  `close(pfd[0])` via `ks_relocate_fd_min()`.
- H1A punch watchdog parent write end (slide.c
  `slide_h1a_start_watchdog`) — same relocation.

Mechanism: `fcntl(fd, F_DUPFD, 384)` then close the original fd. fds
already `>= 384` are left untouched. Waiter encoding
(`prepare_slide_select_fdsets`), `PSELECT_ROUTE_NFDS`, consumer timing
and kernel write path are unchanged.

## Build (pwsh, from repo root)

```powershell
cmd /c "D:\codeTool\ndk\android-ndk-r26b\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android35-clang.cmd @build_slide_h1a.rsp"
```

## Deliverables for auditor re-review

1. Diff limited to: `src/slide.c`, <=6 lines `src/main.c`, one new rsp,
   audit-report addendum (Q-1).
2. `build/alioth/bin/preload_slide_h1a.so` + SHA256.
3. A transcript of the offline self-check (markers, no device).

No device run may be scheduled until the auditor signs off.
