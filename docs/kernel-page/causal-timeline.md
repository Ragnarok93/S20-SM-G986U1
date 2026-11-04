# Causal timeline of the recorded runs

Status: offline consolidation at `8c261c9`; §5 corrected against `4d5a8dd`. No device run, no code change.

Rule for every row: **the last line the payload wrote is the furthest event that
run is credited with.** Nothing after it is inferred, and no run is credited
with a completion it did not print. The harness exit code is recorded as a
harness fact, not as a payload status (`punch-path-audit.md` §1-2).

Flags in the table: `A` = `KS_PLANT_ALL=1`, `F` = `KS_FOPS_ROUTE=1`,
`Z` = `KS_IMAGE_ZERO=1`, `P` = `KS_PAGE_PROBE=1`, `D` = `KS_ID_DUMPS=2`,
`W` = `KS_PC_WAIT=0`.

## 1. Every run

| run (local) | flags | last observed payload event | stage reached | exit | device |
| --- | --- | --- | --- | --- | --- |
| 10:09:42 | A F | `waiter futex returned ret=-1 errno=110` | plant | 0 | LOST |
| 10:50:18 | A F Z | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 11:07:53 | A F Z | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 11:39:50 | A F Z | `KS_CONTROL slots=32 … span_ok=1` | control cycle, no gate line | 124 (harness timeout) | ALIVE |
| 12:21:25 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 12:54:00 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 13:41:35 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 13:53:52 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 14:06:42 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 14:22:51 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 15:51:58 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 16:18:18 | A F | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate | 255 | ALIVE |
| 17:37:50 | A P | `KS_DELIVERY skipped=1 reason=gate-preconditions` | gate (after a page probe) | 255 | ALIVE |
| 18:25:04 | A P D W | `KS_PUNCH_SKIPPED page=0: fake-page preparation failed` | dumps, then page-prep failure | 255 | ALIVE |
| 18:46:22 | A P D W | `waiter futex returned ret=-1 errno=110` | plant | 0 | LOST |
| 20:02:34 | A P D W | `consumer punch enter tid=5202` | punch entered | 0 | LOST |

## 2. Counts, and what has never happened

| event | runs |
| --- | --- |
| gate-refused, device alive | 11 |
| control cycle, harness timeout | 1 |
| dumps completed, punch skipped | 1 |
| plant stage entered | 3 (10:09, 18:46, 20:02) |
| `KS_PLANT_SHALLOW` / `KS_PLANT_LANES` | 1 (20:02) |
| punch entered | 1 (20:02) |
| **punch returned** (`consumer punch done`) | **0** |
| **route completed** (`main route done`) | **0** |
| **summary printed** (`pipe-physrw-summary`) | **0** |
| device lost | 3 |

The punch call has never returned on this device, in any recorded run. Every
claim about what happens after it returns — the marker read, the route
completion, the five-second tail, the process exit — is source-only.

## 3. What each boundary establishes

| boundary | runs | establishes | does not establish |
| --- | --- | --- | --- |
| gate | 11 | preparation and the refusal are safe: 11 refusals, 13 survivors | anything about the plant or punch |
| control | 1 | a 32-allocation cycle completes under the harness timeout | why it timed out |
| dumps then skip | 1 | the collector can complete and then decline to plant | that the collector is safe in a run that does plant (20:02 lost) |
| plant | 2 | the loss occurs between the futex return and the first plant log | whether the plant copy ran, or whether `pi_blocked_on` was non-null |
| punch | 1 | the loss occurs after the punch is entered and before it returns | which step inside the walk ran, or whether it faulted or hung |

## 4. Evidence strength per operation

`source reachable` = a path exists; `preconditions` = the run's own logs show
the required inputs exist; `execution` = a line proves entry; `completion` = a
return or readback proves it finished; `persistent` = the resulting kernel state
was measured afterwards.

| operation | source reachable | preconditions established | execution observed | completion observed | persistent effect observed |
| --- | --- | --- | --- | --- | --- |
| pselect fd-set plant | `fops.c:361`, `slide.c:162-232` | futex returned (10:09, 18:46, 20:02); `plant_ready=1` in 20:02 | `KS_PLANT_SHALLOW` / `KS_PLANT_LANES` (20:02 only) | partial: `hits=1 mismatch=0` proves the signal frame, not the kernel-stack copy | none (stack-scoped) |
| `waiter->lock` read | `rtmutex.c:1139` | fake prio 130 vs victim 120 (20:02 log) | no | no | no |
| fake lock write (`trylock`) | `rtmutex.c:585` | same as above | no | no | no |
| `rb_erase` fops write | `rtmutex.c:661-740` with `util.c:1790` | fake triple printed: `pc`, `right`, `left` (20:02) | no | no | no |
| `ashmem_misc.fops` install | same path | same as above | no | no | no: the readback result is only printed in `consumer punch done`, which no run produced |
| punch return | `main.c:283` | punch entered (20:02) | yes (20:02) | **no** | n/a |
| route completion | `main.c:148`, `385` | never reached | no | no | n/a |
| summary / exit | `main.c:528-544` | never reached | no | no | n/a |
| page and fops after exit | `lifecycle-matrix.md` §3 | never reached | no | no | no |

The fops write is the operation most often treated as established: the triple it
would use is logged, but no log line proves the walk reached it, and no readback
was ever taken. It is **source reachable with preconditions present**, nothing
more.

## 5. The causal boundary, restated

- Pre-plant: 11 gate runs and one control cycle. No plant, no loss.
- Plant: 10:09 and 18:46. The futex returned, the plant routine began, and the
  device stopped before its first log line.
- Punch: 20:02. The plant was logged, the lanes were logged, the punch was
  entered, and the device stopped before the syscall returned.
- Post-punch: never observed. Every statement about it — the fops install, the
  dangling page, the watchdog's fate — is a source property awaiting a run that
  returns, not a description of anything that happened.

`bootreason=kernel_panic,null` is not used above as a per-run field: it appears
on all 13 survivors as well. It does classify one loss when read across a boot
boundary — 10:09 starts on `reboot,shell`, 10:50 starts on `kernel_panic,null`,
and no run lies between them (`failure-boundary.md` §3). The other two losses
cannot be placed on a boot, because no boot identity was recorded.

## 6. What would tighten this table

Only observation, not change: a run that reaches the punch with a second `adb`
stream (`dmesg -w`, `logcat -b kernel`), and after any reconnect a read of
dropbox `SYSTEM_LAST_KMSG` (shell-reachable) and pstore if the shell is ever
permitted it. Those records would populate the `completion` and `persistent`
columns for the walk and classify the loss mode. The handset is absent; no run
is possible.
