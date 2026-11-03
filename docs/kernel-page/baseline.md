# Baseline: kernel-page experiment (2026-11-03)

This is a frozen record of the current working state. It exists so that later
experimental changes can be judged against a known, warning-free build without
mixing the two.

**Placement is not validated.** Nothing in this baseline establishes that the
sent image occupies the reclaimed page. The zero-image differential that would
test it has not yet run inside a passing cycle.

## Artifact

| item | value |
| --- | --- |
| tree | `~/S20-SM-G986U1` (branch `codex/kernelsu-g986u1-port-tools-20261029`) |
| target | `samsung-g986u1-4.19.113` |
| build | `PROJECT=samsung-g986u1-4.19.113 bash build.sh` |
| toolchain | NDK r27c (`~/ndk/android-ndk-r27c`), API 33, `aarch64-linux-android33-clang` |
| output | `build/samsung-g986u1-4.19.113/bin/preload.so`, copied to `prebuilt/preload.so` |
| warnings | none (`-Wall -Wextra`) |
| kernel reference | `~/kernel_samsung_sm8250` (4.19.113, SM8250) |
| device | SM-G986U1, `shell` uid 2000, SELinux `u:r:shell:s0` enforcing |

## Smoke procedure (about 70 seconds, no kernel risk)

```sh
export ANDROID_SERIAL=<device>:5555          # TCP adb survives USB drops
adb push build/samsung-g986u1-4.19.113/bin/preload.so /data/local/tmp/ghostlock/preload.so
timeout 70 adb shell 'cd /data/local/tmp/ghostlock && \
  env KS_FOPS_ROUTE=1 KS_PLANT_ALL=1 KS_IMAGE_ZERO=1 \
  LD_PRELOAD=/data/local/tmp/ghostlock/preload.so /system/bin/id'
```

Expected within 70 s: `[+] startup context …`, `KS_LEAK child …`,
`KS_PRE … leak_slot=1`, `KS_BURST … span_ok=1`, `KS_CONTROL … span_ok=1`.
The run is then killed during the scan; the device stays up. Reap leftover
children afterwards (`pkill -9` on the `id` processes) so the next burst starts
from a clean cache.

A full cycle replaces the timeout with the run script and takes 8–10 minutes
(dominated by the address-recovery scan).

## Representative cycles (verbatim lines)

**Delivered, gate passed, probe executed, device panicked** — the furthest the
chain has reached (`~/runs/20261103_100942.log`):

```
[*] KS_PRE lead=2 fillers=32 leak_slot=1 slabs=62->63
[*] KS_BURST slots=32 boundary_slabs=63 after_slabs=63 span_ok=1 expect=32 plan_cpu=1
[*] KS_FREEZE fillers=31 target_pid=7249 cpu=0 empty_cpu=1
[*] KS_DRAIN pushes=170 pages=5 need=5 proven=1 cpu=0
[*] KS_GATE span_ok=1 crossed_at=5 min_partial=5 empty_cpu=1 registered=0 discarded=1 image=3 gap_ms=0.054 d_empty=-2 d_num=-32 d_slabs=-1 pin_bad=0 slot_partial=0 preconditions=1
[*] KS_IMAGE_PIN mode=immediate sends=3 of=3 base=ffffffc014f50000 warm=3 cpu=1 freeing_cpu=1 same=1 gap_ms=0.054
[*] KS_DISCARD verdict=match target_gap=-2/-32/-1 expect=-1/-32/-1 base=ffffffc014f50000
```
then `bootreason=kernel_panic,null`, `Oops PC = rt_mutex_adjust_prio_chain+0x304/0x858`.

**Refused, same machinery, different instant** (`~/runs/20261103_105018.log`):

```
[*] KS_QUIESCE active=2276 num=2432 slabs=76 stable=1
[*] KS_GATE span_ok=1 crossed_at=5 min_partial=5 empty_cpu=1 registered=1 discarded=0 image=3 gap_ms=0.056 d_empty=-1 d_num=0 d_slabs=0 pin_bad=0 slot_partial=0 preconditions=0
[*] KS_CTL_FREE num=2432->2400 slabs=76->75 d_num=-32 d_slabs=-1 discarded=1
[!] KS_DELIVERY skipped=1 reason=gate-preconditions base=ffffffc05b298000
```

The difference between these two cycles is the node-partial count at the free,
which no counter exposes — see `accounting.md`.

## Measured facts

1. Address recovery is re-checked against the run's own collision set; a stale
   answer file can no longer short-circuit the scan (it used to be accepted by
   the abort hook with no check, saved as the next cache anchor, then rejected).
2. The anchor burst builds one fresh 32-slot slab with the scanning child at
   slot 1: `KS_PRE … leak_slot=1`, `KS_BURST … span_ok=1`.
3. A whole-slab discard has been attributed to the anchor slab once
   (`d_num=-32 d_slabs=-1` in the emptying-free window).
4. The control slab discards on the same mechanics (`KS_CTL_FREE … -32/-1`).
5. The image send runs on the freeing CPU inside 54 µs (`same=1`, `gap_ms=0.054`).
6. When the probe runs, the fault is always `rt_mutex_adjust_prio_chain+0x304`.
7. Build is warning-free; the smoke run reaches both bursts.

## Hypotheses (not established)

1. That the fragment's page is `base` — contradicted, but not conclusively, by
   the one zero-image cycle that left the fault address unchanged.
2. That holding `n->nr_partial` above `s->min_partial` at the free is
   achievable by ordering alone.
3. That the budget sibling claim (`KS_BUDDY_CAPTURE`) prevents upward merge in
   the same window as the send.
4. That the planted field mapping at `+0x304` is correct for this kernel.

## Known-good code state

- `src/kernel_page.h` — observation + result-status interface, with the counter
  semantics from `mm/slub.c` documented.
- `src/kernel_page/allocator_observation.c` — the only reader of
  `/proc/slabinfo`.
- `src/kernel_page/experiment_state.c` — independent statuses, `KS_RESULT` line.
- `src/kernel_page/gate.c`, `gate.h` — the verdict arithmetic, pure and
  host-tested (`tests/kernel_page/gate_test.c`); `src/util.c` passes it two
  samples and prints `KS_GATE` exactly as before.
- `src/kernel_page/run_state.c`, `run_state.h` — the lifecycle model
  (`tests/kernel_page/state_test.c`); it observes and reports, it does not
  change any branch.
- `src/kernel_page/address_cache.c`, `address_cache.h` — parsing and
  validation for the saved anchor and the one-shot answer file
  (`tests/kernel_page/cache_test.c`), moved out of `src/util.c` without
  changing what is accepted or what is printed.
- `src/util.c` — coordinator; observation functions removed; statuses recorded
  at the three points where a proposition becomes established.
- `MM_STRUCT_SZ` has one definition (the target profile). The `0x500` fallback
  in `common.h` would have made `mm_objs_per_slab` 25 instead of 32 and is
  gone.
