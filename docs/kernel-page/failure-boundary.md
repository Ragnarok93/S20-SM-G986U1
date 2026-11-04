# Pre-completion failure boundary

Status: offline analysis at `4d5a8dd`. No device run, no payload change, no gate
change. The handset is absent (`adb devices` empty, no USB device in `usbipd`).

Scope: the two intervals in which every recorded loss occurred, what the host
logs can and cannot separate inside them, and the records that already exist on
the device and can be read after recovery without repeating the experiment.

Every claim here is one of three kinds, labelled as in `punch-path-audit.md`:
**S** = read from the pinned source, **M** = measured in a recorded run,
**I** = inferred. Nothing is upgraded by appearing in a sentence with a stronger
claim.

## 1. The two intervals

The plant boundary (10:09, 18:46) and the punch boundary (20:02) each contain
exactly one unobserved span. Both are delimited by print statements, so their
contents can be enumerated from source rather than guessed.

### Interval A — last plant line to first plant line

| # | operation | source | can it print? |
| --- | --- | --- | --- |
| 0 | `waiter futex returned ret=-1 errno=110` | `main.c:111` | yes — last line of 10:09 and 18:46 |
| 1 | `sigaction(PLANT_SIGNAL, …)` | `slide.c:74-82` | no; returns 0/1, both silent |
| 2 | `gettid`, `getpid` | `slide.c:83-84` | no |
| 3 | `getenv` (up to five) and `strtoull` | `slide.c:163-189` | no |
| 4 | `KS_PLANT_SHALLOW value=…` | `slide.c:191` | yes — first line of 20:02's plant |

Everything between the two print statements is either a userspace string parse
or one of three syscalls that touch the calling thread's signal table and ids.
None of them dereferences the fake lock, the sprayed page or any planted value;
`plant_shallow()` writes to the stack only **after** line 191 prints. So a loss
inside interval A cannot be attributed to the fake-page machinery on the basis of
the code in that interval — and equally, the interval is too short to explain a
90-second-scale hang.

That asymmetry is why the delivered-line boundary is not an execution boundary:
18:46's last line is the same as 10:09's, yet 18:46 ran with two dump children
and a completed page preparation that 10:09 did not have.

### Interval B — punch entry to punch return

| # | operation | source | can it print? |
| --- | --- | --- | --- |
| 0 | `consumer punch enter tid=…` | `main.c:279` | yes — last line of 20:02 |
| 1 | `syscall(SYS_sched_setattr, tid, &attr, 0)` | `util.c:796` | only on failure: `sched_setattr tid=… ret=… errno=…` (`util.c:799`) |
| 2 | `consumer punch first ret=…` | `main.c:283` | yes |

Two prints are possible after the entry line, and 20:02 delivered neither. The
failure warning is the interesting one: it fires *before* the `setpriority`
fallback, so its absence rules out the "the syscall returned an error and the
fallback ran" reading. What remains is that the syscall had not returned when the
stream ended, or the stream ended first.

## 2. Delivered versus executed

`exlog_writef()` (`util.c:4837-4866`) does two things per line:

1. `fprintf(stdout)` + `fflush` (`util.c:4845-4846`) — the stream the harness
   captures through `adb shell`.
2. `write(g_exlog_fd, …)` then **`fsync`** (`util.c:4863-4865`) to
   `/data/local/tmp/ks_log.txt`, which every run logs at startup
   (`[+] exlog open path=/data/local/tmp/ks_log.txt`) — all 16 logs, **M**.

So each printed line exists twice, and the second copy is durable. The host logs
answer "what was delivered"; the on-device file answers "what was executed".

| question | host log | `ks_log.txt` |
| --- | --- | --- |
| last line delivered | yes | yes |
| last line executed before a panic | no | yes, up to the last completed `fsync` |
| which pid wrote a line | no (prefix is the level only) | yes (`[pid][*] …`) |
| survives a reboot | n/a | yes, on `/data` |
| ever retrieved | n/a | **no** — no `adb pull` and no `[pid]`-prefixed line appears in any recorded log |

Two properties of the file matter for reading it:

- **Offset 0, no truncate.** `open(…, O_WRONLY | O_CLOEXEC, 0666)`
  (`util.c:4801`) neither appends nor truncates, so each run starts writing at
  offset 0 and a shorter run leaves the previous run's tail in place. Pid tags
  are what separate them. The 20:02 run (pid 11056) may therefore be followed in
  the same file by the tail of 18:46 (pid 30624); 10:09 (pid 5274) is the most
  likely to have been overwritten.
- **Write and fsync results are ignored** (`util.c:4863-4865`), so a failed
  flush loses a line silently. A file that ends mid-spell marks the last durable
  write, not necessarily the last attempted one.

What the file can prove is narrower than "where execution stopped":

- **Logging, not instructions.** A recovered line proves that one
  `exlog_writef` call reached its durable write. It does not prove the next
  source statement ran, and it cannot name the faulting instruction.
- **One history, several runs.** The file is a concatenation. Reconstruct
  each run by pid in file order; the physical tail is not necessarily any
  single run's output.
- **Missing is still ambiguous.** With the write and fsync results unchecked,
  an absent persistent line means the flush did not complete — the bound is
  narrower than the delivered one, not closed.

## 3. The boot chain, and the one loss it classifies

`ro.boot.bootreason` is fixed at boot, so its value at the *start* of a run
describes the boot that run is executing in — not the run itself. Read as a
sequence, the field is the only existing record that can put a loss on one side
of a reboot.

| runs | `bootreason_before` | reading |
| --- | --- | --- |
| 10:09:42 | `reboot,shell` | previous boot was a shell-requested reboot |
| 10:50:18 … 18:46:22 (14 runs) | `kernel_panic,null` | this boot was created by a kernel panic |
| 20:02:34 | `kernel_panic,null` | same string — same boot, or a second panic boot |

The transition between 10:09 and 10:50 has no run in between. 10:09 is therefore
the only recorded loss whose successor boot is a **different** boot: it ended in
a kernel panic (**I**, bootloader-recorded, no backtrace).

The reading rests on two assumptions, stated so they can be attacked:

1. **No unrecorded actor caused that panic.** The window contains exactly one
   payload run and no other logged activity, so the panic is attributed to
   10:09. A spontaneous panic from leftover state is not excluded, only
   unsupported.
2. **The panic may post-date the payload's exit.** The host's `payload_exit=0`
   is not a payload status (`punch-path-audit.md` §1), so the panic could have
   occurred after the process was already gone, not inside the plant.

That corrects the earlier statement in `causal-timeline.md` §5 that the field
"cannot classify any of these runs": it cannot classify a run from its own
start value, but a transition across a boot boundary does classify the loss in
between. What survives is this table, and nothing wider:

| run | loss mode | completion |
| --- | --- | --- |
| 10:09 | kernel panic, supported by the boot-reason transition | none |
| 18:46 | unresolved; panic possible | none |
| 20:02 | unresolved; panic possible | none |

18:46 and 20:02 stay unresolved for a different reason: `kernel_panic,null` would
be identical whether the phone rebooted again or never rebooted at all, and no
boot identity was recorded: the harness logs the reason but not `boot_id` or
`/proc/uptime`, so two consecutive panic boots are indistinguishable in the
record (**S** of the harness, **M** of every log). A panic for those runs is
consistent with the evidence and not established by it.

What this already excludes for 10:09, and suggests for the other two:

| failure mode | 10:09 | 18:46 / 20:02 |
| --- | --- | --- |
| userspace abort | excluded: the next boot is not `reboot,shell`, and an abort does not panic the kernel | open |
| USB transport loss alone | excluded: a transport loss does not produce a `kernel_panic` boot | open — would explain the missing lines without a reboot |
| kernel panic | **I** | consistent, unproven |
| lockup / watchdog reset | unlikely: those carry a different reason string on this bootloader (**S**, not exercised) | open |

## 4. Recovery checklist (no experiment, no payload)

Ordered by cost. Every item is a read of state the device already holds.

| # | record | how | access | settles |
| --- | --- | --- | --- | --- |
| 1 | boot identity | `adb shell cat /proc/uptime; cat /proc/sys/kernel/random/boot_id 2>/dev/null; getprop ro.boot.bootreason` | shell | whether the phone booted after 20:20:00, if `uptime` is shorter than the wall time elapsed since then; the reason then classifies the 20:02 loss. `boot_id` separates boots from each other but not from the unrecorded previous one |
| 2 | `/data/local/tmp/ks_log.txt` | `adb pull /data/local/tmp/ks_log.txt` | shell (0666) | last **executed** line per pid; whether 10:09 or 18:46 reached `KS_PLANT_SHALLOW` or `consumer punch enter` after the host stream died |
| 3 | dropbox `SYSTEM_LAST_KMSG` | `dumpsys dropbox --print <ID>` | shell, may deny | the panic backtrace: `pc`, `lr`, faulting function — the only record that names *where* the kernel died |
| 4 | pstore | `ls /sys/fs/pstore; cat console-ramoops` | root | same as 3 if the shell is denied; unavailable to this shell today (`lifecycle-matrix.md` §4) |
| 5 | `logcat -b crash` | `adb logcat -b crash -d` | shell | userspace aborts only; cannot show a kernel fault |
| 6 | current process state | `adb shell 'for p in /proc/[0-9]*; do printf "%s " "$p"; tr "\0" " " < "$p/cmdline" 2>/dev/null; echo; done'; getprop sys.boot_completed` | shell | whether a payload survivor exists. Capture every pid with its full argv, not the matches only: `cmdline` cannot show the preload, `/proc/<pid>/environ` is not readable across uids, and a negative result is not proof of absence |

### 4.1 One session, in this order

The reads are cheap enough to repeat, but a repeated read is not the same as the
first: a restart, a cleanup or a `kill` destroys the state item 6 is looking for.
Do all five before interpreting anything.

1. **Boot identity** (item 1), with the host's `date -Is` recorded beside it, so the
   uptime comparison has a timestamp to stand on.
2. **Process state** (item 6): every pid with its raw argv, saved verbatim even
   though most of it is noise.
3. **The persistent log** (item 2), complete. Record `ls -l` and `sha256sum` on the
   device before pulling; do not truncate, rotate or rewrite it on the device.
4. **Crash records** (items 3-4), if the shell is permitted to read them.
5. **Hash and archive** every artifact on the host before any of it is interpreted,
   in the form used by `logs/SHA256SUMS`.

No service restart, no process cleanup and no payload run until those five are done.
`adb shell` reads and `adb pull` restart nothing; restarting the host's adb server or
re-attaching the USB device is not a device reboot, but rebooting the phone discards
items 1-2.

Item 1 is the cheapest and is decisive for the 20:02 run, with two bounds. The host
clock is trustworthy, so `uptime < (now − 20:20:00)` proves a boot after 20:20 —
provided `uptime` counts suspend time, which 4.19's `fs/proc/uptime.c` does by
using boot time (**S**, upstream 4.19, not audited in this tree). It dates the boot to
the elapsed window, not to an instant.

A longer uptime is equally useful and equally bounded: it proves no reboot since
before 20:02, which rules out a reboot as the cause of that loss. It does **not**
show the payload is still running — the process could have exited, aborted in
userspace, or be stuck while only the transport failed. Item 6 and the persistent
log are what separate those. `boot_id` separates boots only against a value recorded
earlier; read for the first time now, it cannot place any historical run on either
side of a boot.

Item 2 is decisive for 10:09 and 18:46 in the opposite direction. If the file's
tail ends on the same line as the host log, execution really stopped there and
the panic happened inside three trivial syscalls — which would move the cause
away from the fake-page walk for those runs. If the file continues past the host
tail into `KS_PLANT_SHALLOW`, `KS_PLANT_LANES`, `consumer punch enter`, or a
`consumer punch first ret`, then those runs did reach the walk and only the
delivery of their lines was lost.

## 5. What this does not do

- It does not move the gate: 11 refusals, no delivery, no root.
- It does not change the punch, the plant, the allocator path, the timing, or
  the harness. Items 1 and 2 are reads.
- It does not decide the H1/H2 occupancy question. A panic backtrace would name
  the faulting code, not the anchor page's `inuse`.
- It does not make `bootreason` a diagnosis on its own: with no `boot_id` in the
  record, 18:46 and 20:02 remain consistent with several modes.
- It does not authenticate a kernel-path hypothesis. The verifier checks that this
  document matches the pinned source and the 16 logs; it says nothing about which
  allocator branch a free took or where a panic originated.
