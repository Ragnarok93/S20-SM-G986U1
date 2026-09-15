# Slide / H1A Evidence Matrix

Scope: independent evidence audit only. No device action, no exploit code
change, no new on-device run. All rows cite existing logs and docs. Each
conclusion is CONFIRMED, CONTRADICTED, or INSUFFICIENT based only on the
cited evidence.

## Failure Modes (reproducible)

| # | Claim | Verdict | Evidence |
|---|-------|---------|----------|
| F-1 | fd sweep can overwrite long-lived fds. `open_slide_selected_fds()` reuses fds `0..319`; `g_exlog_fd` and punch-watchdog parent pipe fds may live below that boundary, so both can be overwritten before punch completion. | CONFIRMED | `docs/status.md` "M5 fd-protect offline audit"; `docs/status.md` "H1A fd-protect device round" (log reached startup/H1A route but no `H1A punch enter/returned/success`); `build/ks_log_h1a_fdprotect_run1.txt` |
| F-2 | Missing mm answer sends H1A into full 8 GB / 44 GB device scan instead of proceeding to punch stage. | CONFIRMED | `docs/status.md` "H1A diff probe (runs 5-9)" run5 sat in full 8 GB kernelsnitch scan; `docs/status.md` "fd-protect validation and full-chain reboot" second round reached 44 GB device scan because no mm answer supplied; `build/ks_log_h1a_fdprotect_run1.txt` shows collision search without answer pre-load marker |
| F-3 | Stale rt_waiter placement is wrong: punch returns success (`ret=86 calls=1 success=1`) but PI chain never writes the controlled fake page. | CONFIRMED | `build/ks_log_h1a_leakprobe_run9_diff.txt` (`H1A punch returned ret=86 calls=1 success=1` then `KS_DIFF skb=0..3 bytes=32448 diffs=0`); `build/ks_log_h1a_leakprobe_run10_stride34.txt` same punch success and all 4 `diffs=0`; `docs/status.md` run9 conclusion "PI chain does not write anywhere in the controlled page" |
| F-4 | H1A-D (deadlock-shaped requeue) reaches the PI walk but the walk spins instead of terminating; 90s watchdog kills the parent but the kernel spin requires reboot. | CONFIRMED | `build/ks_log_h1a_d_run1.txt` (`requeue ret=-1 errno=35 (expected ret=-1 errno=EDEADLK)` then `punch watchdog timeout parent=25728 status=stuck`); `docs/status.md` H1A-D section (walk SPINS, `adb reboot` required) |
| F-5 | Full-chain walk/CFI stage is unsafe or semantically wrong: walk reports `walk_touched=0`, CFI probe is refused, and the device rebooted immediately after. | CONFIRMED | `build/ks_log_fullchain_m5_fdprotect_run1.txt` (`walk_touched=0 leak=00000000`, `refusing cfi probe`); `docs/status.md` "fd-protect validation and full-chain reboot" (device rebooted, uptime 0 min) |
| F-6 | v15 M2 export SIGSEGV is a separate candidate-index bug, not a slide/H1A failure mode. | INSUFFICIENT (out of scope) | `docs/status.md` "M2 v15 device round and v16 offline rebuild"; `build/m2_v15_run1_crashlog.txt`; M2 audit is outside slide/H1A matrix scope |

## Success Markers

| # | Claim | Verdict | Evidence |
|---|-------|---------|----------|
| S-1 | H1A punch-only success marker is `H1A punch returned ret=86 errno=0 calls=1 success=1`. | CONFIRMED | `build/ks_log_h1a_leakprobe_run9_diff.txt`; `build/ks_log_h1a_leakprobe_run10_stride34.txt`; `build/ks_log_h1a_fdprotect_run3.txt` |
| S-2 | H1A full-chain punch-stage success marker (pre-CFI) includes `H1A punch enter`, `H1A punch done`, `H1A punch returned`, and `H1A success calls=1 success=1`. | CONFIRMED | `build/ks_log_h1a_fdprotect_run3.txt` |
| S-3 | H1A run4 punch-only pass reached `H1A success calls=1 success=1` and exited with `rc=0`. | CONFIRMED | `build/ks_log_h1a_run4_pass.txt`; `docs/status.md` TC-01 On-Device Addendum |
| S-4 | Root success (`uid_after=0`) or `F_SETPIPE_SZ accepted` has never been reached on H1A. | CONFIRMED | `docs/status.md` M5 H1A full-chain round (result UNKNOWN, later FAIL, neither reached these markers); `docs/status.md` fd-protect validation (full-chain rebooted before these markers) |

## fd Protection Effect

| # | Claim | Verdict | Evidence |
|---|-------|---------|----------|
| P-1 | fd-protect fix relocates `g_exlog_fd` and both punch-watchdog parent write fds above `PSELECT_ROUTE_NFDS + 64` before the fd sweep, without changing v12 pselect fd-bitmap encoding, consumer timing, CFI write path, or KernelSnitch collision semantics. | CONFIRMED | `docs/status.md` "M5 fd-protect offline audit" |
| P-2 | fd-protect artifact `preload_slide_h1a.so` (217456 B, SHA256 `5551CA...AD00`) preserved the previously missing chain markers on a low-load punch-only round. | CONFIRMED | `docs/status.md` "fd-protect validation and full-chain reboot"; `build/ks_log_h1a_fdprotect_run3.txt` shows the full marker chain |
| P-3 | First `rc=137` observation was an insufficient observation window, not an fd-protect startup failure; a later round confirmed the process continued into collision search. | CONFIRMED | `docs/status.md` "fd-protect validation and full-chain reboot"; `build/ks_log_h1a_fdprotect_run1.txt` and `build/ks_log_h1a_fdprotect_run2.txt` |
| P-4 | fd protection is no longer the cause of missing punch logs. | CONFIRMED | `docs/status.md` "fd-protect validation and full-chain reboot" explicitly states this; F-3/F-4 show the remaining issue is walk/placement |
| P-5 | fd-protect fix also requires skipped-spray memfds to be set to `-1` before close loops; otherwise calloc-zero fds hit closed stdio fds and trip `SYSCHK(close)` EBADF abort. | CONFIRMED | `docs/status.md` H1A diff probe run5 root causes; `build/ks_log_h1a_leakprobe_run8_fdguard.txt` ends with `SYSCHK(close(pre_ctx.memfds[i])): Bad file descriptor` |

## Residual Processes / Device Hygiene

| # | Claim | Verdict | Evidence |
|---|-------|---------|----------|
| R-1 | Run5-era orphan `id` processes can survive and pollute load/observations; ~1400 clone children were found and killed, device stayed online. | CONFIRMED | `docs/status.md` H1A diff probe run5; `build/ks_log_h1a_leakprobe_run5_diff_only.txt` shows a fresh H1A run starting while collision search was still active from a prior process context |
| R-2 | A parallel H1A `id` process (PID 17004) contaminated an M2 log and was stopped; M2 PID 31647 was left untouched. | CONFIRMED | `docs/status.md` "M2 v15 device round and v16 offline rebuild" |
| R-3 | H1A-D watchdog killed the parent as designed, but kernel spin still required reboot; device recovered via `adb reboot` (uptime 0 min). | CONFIRMED | `build/ks_log_h1a_d_run1.txt`; `docs/status.md` H1A-D section |
| R-4 | Every killed run must be followed by `ps` residue verification; this is a documented post-run requirement, not yet a standalone automated check. | INSUFFICIENT (procedure stated, enforcement evidence absent) | `docs/status.md` H1A-D section ("verify `ps` residue after every killed run") |

## Placement / Model Conclusions

| # | Claim | Verdict | Evidence |
|---|-------|---------|----------|
| M-1 | Stale rt_waiter is the stack-local `struct rt_mutex_waiter rt_waiter` in `futex_wait_requeue_pi()`, not an SLUB object; kmalloc-96/mm-page hypotheses are dead. | CONFIRMED | `docs/specs/rt-waiter-placement-audit.md` Verdict and Exact 4.19 path; cited source lines in that doc |
| M-2 | mm cache stride is 0x3c0 (34 objs/slab), not 36; ORDER3_SIZE=0x8000 stands; 4K pages confirmed. | CONFIRMED | `docs/status.md` H1A-W placement probe section; `docs/specs/rt-waiter-placement-audit.md` cache/stride analysis |
| M-3 | H1A-W userspace placement probe cannot observe kernel-stack SP on this kernel (`kstkesp=0`, `/proc/stack EACCES`, ptrace EPERM), so placement remains PLACEMENT-UNKNOWN by that probe alone. | CONFIRMED | `build/ks_log_h1a_w_run2.txt`; `docs/status.md` H1A-W section |
| M-4 | Spray count/stride is not the blocker for missing fake-page writes; stride-34 run still produced punch success and `diffs=0` on all four SKB readbacks. | CONFIRMED | `build/ks_log_h1a_leakprobe_run10_stride34.txt`; `docs/status.md` H1A-W section |
| M-5 | `0xffffffc1b1cfb840` is reproduced as the accepted mm answer across H1A rounds and is used to skip the kernelsnitch scan. | CONFIRMED | `build/ks_log_h1a_leakprobe_run9_diff.txt`; `build/ks_log_h1a_fdprotect_run3.txt`; `build/ks_log_h1a_d_run1.txt`; `docs/status.md` multiple M5 sections |
| M-6 | 144 enumerated futex hash models found 0 concentrated-bucket hits for run4's 16-address set, the 15-address subset excluding the target, and the 14-address intersection. This contradicts the assumption that the historical mm target forms a concentrated bucket under the tested models. | CONFIRMED | `build/model_fit_20260915/scan_summary_16.txt`; `scan_summary_15_excl_target.txt`; `scan_summary_14_intersection.txt`; `docs/status.md` H1A run4 hash-model fit |
| M-7 | Correct stale-waiter placement on this specific kernel stack is known. | INSUFFICIENT | F-3/F-4/M-3 show placement or frame linkage is still wrong; no existing log confirms the correct placement |
| M-8 | Slide pselect route is a confirmed dead end on alioth. | CONTRADICTED | H1A-D reproduced the dangling-waiter precondition (`errno=35`/EDEADLK gate PASSED) and the punch reached the PI walk (`docs/status.md` H1A-D section); route is still blocked by placement/spin, but not dead |

## Current Blocking Question

The remaining blocker is not collision collection, mm-answer acceptance,
fd protection, or spray stride. It is the stale-waiter/fake-page linkage:
punch can reach the PI walk (H1A-D), but the walk either never touches the
controlled page (H1A run9/run10) or spins once the EDEADLK-shaped
dangling-waiter path is forced (H1A-D). No further full-chain device run is
supported by the cited evidence until placement/frame math is corrected and
validated with a safe probe.
