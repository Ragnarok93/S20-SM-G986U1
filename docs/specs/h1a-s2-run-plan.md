# H1A-S2 single-shot run plan

Date: 2026-09-16

Scope: one offline-approved device attempt, if and only if explicitly
scheduled after this plan is reviewed. This document is not authorization to
run CFI, fops mutation, pipe-physrw, root, the full chain, or any retry.
No device command was issued while creating this plan.

## Artifact

```text
build/alioth/bin/preload_slide_h1a_s2.so
size=71832
sha256=B12FB37D7BBD7C5E218A36C4B65D54188AB43CEF0ECC871EB019B717780E2A06
```

The current artifact is the final-audit S2 build described in
`docs/specs/h1a-s2-final-audit.md` and `docs/status.md`. Do not rebuild,
relink, or substitute another binary for this attempt. If the local hash
differs, stop and re-audit the artifact.

References:

- `docs/specs/sendmsg-h1a-s2-implementation-design.md`
- `docs/specs/h1a-s2-proxy-root-audit.md`
- `AGENTS.md` Device Protocol and Operating Safety

## Hard gates

1. Exactly one `KS_H1A_S2_TEST=1` attempt per boot.
2. Use the isolated S2 entry only. Never also set `KS_H1A_TEST`,
   `KS_H1A_W_TEST`, `KS_H1A_D_TEST`, `KS_SLIDE_TEST`, or `KS_W1_TEST`.
3. Do not set a nonzero `KS_DELTA`. Do not create
   `/data/local/tmp/ks_delta_index.txt`.
4. CFI, fops mutation, pipe-physrw, root, and full-chain follow-up are
   prohibited even when the S2 verdict is PASS.
5. The process watchdog is 45 seconds. Watchdog timeout is recovery by
   reboot, not a prompt for another attempt.
6. Any FAIL, watchdog, crash, lost shell, panic, unexpected reboot, or
   unexplained marker sequence ends the boot session.
7. Foreground only. Do not use `nohup` or a background shell.
8. Do not clean residue with `pkill -f`. Prefer reboot before a scheduled
   attempt; otherwise record the PID and kill only that exact PID.

## Device state

Do this only on a fresh boot with no prior S2/H1A/full-chain attempt. If an
earlier probe ran during this boot, reboot first. The S2 attempt is then the
single allowed run in the new boot.

```powershell
$adb = 'D:\software\commonTool\scrcpy-win64-v2.7\adb.exe'
& $adb devices
& $adb shell 'uptime; cat /proc/loadavg'
& $adb shell 'ps -A | grep -E "preload_slide_h1a|preload\.so|kernelsnitch" | grep -v grep || true'
```

Abort unless:

- `1m` load is below `2.0`.
- There is no residue from a prior preload or scan process.
- The device is responsive and not thermally throttled after any earlier
  load-heavy work.

## Clean and logs

Clear the stale answer/cache, then recreate the fixed log path with mode
`666` before starting the binary. Pull any old evidence to the host before
clearing it.

```powershell
& $adb shell 'rm -f /data/local/tmp/ks_mm_answer.txt /data/local/tmp/ks_mm_answer.tmp /data/local/tmp/ghostlock_ks_cache.txt /data/local/tmp/ks_delta_index.txt /data/local/tmp/preload_slide_h1a_s2.so /data/local/tmp/preload_slide_h1a_s2.so.tmp'
& $adb shell ': > /data/local/tmp/ks_log.txt; chmod 666 /data/local/tmp/ks_log.txt; ls -l /data/local/tmp/ks_log.txt'
```

The binary cannot create `/data/local/tmp/ks_log.txt`; if mode or existence
is wrong, stop before running.

## Push and hash

Push the answer and binary to temporary names, sync, then move atomically.
The accepted mm answer is the historical H1A value:

```text
mm=0xffffffc1b1cfb840
```

```powershell
& $adb push .\build\ks_mm_answer_fullchain_m5_fdprotect_run1.txt /data/local/tmp/ks_mm_answer.tmp
& $adb shell 'sync; mv -f /data/local/tmp/ks_mm_answer.tmp /data/local/tmp/ks_mm_answer.txt; sync; wc -c /data/local/tmp/ks_mm_answer.txt; cat /data/local/tmp/ks_mm_answer.txt'
& $adb push .\build\alioth\bin\preload_slide_h1a_s2.so /data/local/tmp/preload_slide_h1a_s2.so.tmp
& $adb shell 'sync; mv -f /data/local/tmp/preload_slide_h1a_s2.so.tmp /data/local/tmp/preload_slide_h1a_s2.so; sync; chmod 666 /data/local/tmp/preload_slide_h1a_s2.so; sha256sum /data/local/tmp/preload_slide_h1a_s2.so; wc -c /data/local/tmp/preload_slide_h1a_s2.so'
```

Stop unless:

- The answer is exactly `22` bytes, ends with LF, and contains
  `mm=0xffffffc1b1cfb840`.
- The device file size is `71832`.
- The device SHA256 is
  `b12fb37d7bbd7c5e218a36c4b65d54188ab43cef0ecc871eb019b717780e2a06`.

## Environment and launch

Run once, in the foreground, with only the S2 gate:

```powershell
& $adb shell 'KS_H1A_S2_TEST=1 KS_DELTA=0 /system/bin/linker64 /data/local/tmp/preload_slide_h1a_s2.so; echo __rc=$?'
```

Before launch, verify no other test-gate variable is present in the adb shell
environment. Do not wrap the command in a background runner and do not add an
external retry loop.

## Watchdog behavior

The binary starts its own 45-second watchdog after page preparation and before
the futex gate. The log must show:

```text
H1A-S2 watchdog started pid=<pid> parent=<pid> timeout_ms=45000
```

If this marker is absent, treat the run as a failed setup. Do not launch a
second process. If the log later shows:

```text
H1A-S2 watchdog timeout parent=<pid> status=stuck
```

the process watchdog has fired. Preserve the log if adb still responds, then
reboot. A watchdog kill does not recover an in-kernel spin.

## Log capture and evidence

After the process exits, read and preserve the log before any reboot:

```powershell
& $adb shell 'tail -n 160 /data/local/tmp/ks_log.txt'
& $adb pull /data/local/tmp/ks_log.txt .\build\ks_log_h1a_s2_run1.txt
```

The minimum evidence set is:

```text
H1A-S2 header route=sendmsg proxy-root=1 cfi=disabled
H1A-S2 page prepared base=<hex> lock=<hex> proxy=<hex> task=<hex>
H1A-S2 tcp sndbuf=<u32> effective=<u32> outq=<u32> gate=PASS
H1A-S2 watchdog started pid=<pid> parent=<pid> timeout_ms=45000
H1A-S2 gate requeue ret=-1 errno=35 victim ret=-1 errno=110
H1A-S2 sendmsg armed tid=<pid> iov_total=65540 delay_ms=150
H1A-S2 punch enter tid=<pid> nice=1
H1A-S2 punch first ret=0 errno=0
H1A-S2 punch done tid=<pid> ret=0 errno=0
H1A-S2 sendmsg returned ret=<nonzero> errno=<d>
H1A-S2 readback proxy_tree_self=<d> proxy_pi_self=<d> ft_pi_root=<hex>
H1A-S2 verdict=PASS reason=gate-pass
```

A safe success is read-only evidence collection at the S2 boundary. It is
still not permission to continue into CFI, fops, pipe-physrw, root, or the
full chain. If the `readback` marker is missing, classify the round as
INSUFFICIENT-EVIDENCE and do not schedule follow-on work from it.

## Failure determination

Treat any of these as FAIL:

- device hash or size mismatch;
- stale answer/cache or wrong answer length;
- missing `cfi=disabled`, page, TCP gate, gate, armed, punch, sendmsg,
  readback, or verdict marker;
- gate is not `requeue ret=-1 errno=35 victim ret=-1 errno=110`;
- punch count is not exactly one, or punch return is not zero;
- `sendmsg` returns early before the armed/allowed gate;
- verdict is not `PASS reason=gate-pass`;
- the watchdog fires, process crashes, shell is lost, device reboots, or
  load climbs abnormally after punch.

On FAIL: stop, preserve the log if possible, and reboot. Never refresh the
page, retry the carrier, add a second punch, or advance the chain in the same
boot.

## Recovery

If adb remains responsive after FAIL or watchdog:

```powershell
& $adb shell 'uptime; cat /proc/loadavg'
& $adb pull /data/local/tmp/ks_log.txt .\build\ks_log_h1a_s2_run1_fail.txt
& $adb reboot
& $adb wait-for-device
& $adb shell 'uptime; cat /proc/loadavg'
```

If `adb reboot` is unresponsive or the device is wedged, use the physical
long-press power recovery path. After recovery, verify uptime reset and
device responsiveness. Do not delete or edit the local saved log.

The first action after recovery is evidence review and offline diagnosis.
There is no automatic second S2 attempt and no escalation to any downstream
kernel stage.

## Linker startup blocker

The real S2 binary is currently blocked before exploit execution by a
linker-startup crash. Current evidence is process return code `139`,
`SEGV_ACCERR`, logcat fault address `0x769aee9000`, PC at offset `0x1000` in
the `.so`, and entry point `0x0`. This signature means the observed S2 launch
did not produce usable evidence about the S2 chain.

Before scheduling another real S2 device attempt, first build and run a
minimal linker-startup hello DSO through `/system/bin/linker64`. That probe
must print its expected nonempty marker to the captured log and exit with
`rc=0`. If it fails, fix only the packaging/linker startup path offline and
repeat the hello probe on a new boot.

Do not retry the real S2 probe until the minimal hello DSO passes cleanly.
The hello probe is also one attempt per boot; a failed hello probe requires
evidence capture and reboot before any further device action.
