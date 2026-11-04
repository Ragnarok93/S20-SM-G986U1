# Lifecycle ownership and concurrency matrix

Status: offline audit at `e0ba1c1`. No device run, no code change, no gate
change. Every row is a statement about the source at the pinned revision; rows
that rest on a log say so.

Sources: payload `src/` at `e0ba1c1`; kernel `~/s20_kernel_chxl1`, 4.19.113
(`kernel/sched/core.c` sha256 `6bd6cfc7…`, `kernel/locking/rtmutex.c` sha256
`36b18717…`, pinned in `punch-path-audit.md`).

Labels: **source** = read from the cited lines; **measured** = device log or
host test; **inferred** = follows from source plus a premise, stated;
**unestablished** = no observation either way.

## 1. Actors

| actor | created at | terminal state | reaped by |
| --- | --- | --- | --- |
| constructor thread (`load`) | `preload.c:388` | returns only when `run_exploit` returns | the `id` process exits |
| waiter | `main.c:349` | returns after `route_done` and `owner_chain_done` (`main.c:148-153`) | nobody (joinable, never joined) |
| owner | `main.c:350` | never: `for (;;) sleep(1)` (`main.c:184-186`) | process death |
| consumer | `main.c:351` | never: `while (!punch_consume_stop)` (`main.c:204`), and `punch_consume_stop` is only ever stored as 0 (`main.c:128`, `main.c:323`, `fops.c:232`) | process death |
| punch watchdog | `main.c:32-66` | exits on the parent's byte (`main.c:72-77`) or after 90 s (`main.c:48-52`) | `waitpid` on the success path (`main.c:79`) |
| dump slot children | `util.c:1396-1500` | park in `pause()` after reporting (`util.c:1498-1500`) | `PR_SET_PDEATHSIG` at `util.c:1407` |
| `pre_ctx` children | `util.c:1356-1382` | park in `pause()` | `PR_SET_PDEATHSIG` at `util.c:1356`; `release_ctx_children(&pre_ctx)` is never called |
| `pipe_prepare_child` | payload setup | killed and waited in the tail (`main.c:539-542`) | the payload |

## 2. Exit paths

### 2.1 `run_exploit` returns

| line | condition | threads started | watchdog | end state |
| --- | --- | --- | --- | --- |
| `main.c:477` | fops route disabled | no | no | clean |
| `main.c:490` | `KS_STOP_AFTER_SPRAY` | no | no | clean |
| `main.c:505` | page preparation failed | no | no | clean |
| `main.c:520` / `main.c:525` | W1 test pass / fail | yes | reaped | clean |
| `main.c:544` | summary path: kill pipe child, `sleep(5)`, return 0 | yes | reaped | threads still running at exit |

`_exit` paths (`main.c:408`, `414`, `419`, `424`, `432`, `437`) belong to the
H1A and slide test modes and terminate before the route.

### 2.2 Route-internal exits (`fops.c`)

| line | condition | effect |
| --- | --- | --- |
| `184-192` | page or fake table missing | returns without starting the route |
| `203`, `216`, `252` | retry page prepare, pipe, `F_DUPFD` failures | `break` out of the attempt loop |
| `351-357` | plant delivery failed | `break` before the punch |
| `431-452` | punch wait timeouts | not fatal: continues with `calls=0`/`success=0` |
| `543`, `548` | fops readback mismatch, or route settled | `break` |
| `556-557` | loop end | prints `pselect route done`, returns to `main.c:148` |

No source path returns 255: `main.c` returns 0, 3, 4, and the `_exit` paths use
0 or 2. A harness code of 255 therefore has no payload exit path behind it.

## 3. Ownership

| object | created | released | survives route return | survives process exit |
| --- | --- | --- | --- | --- |
| `page_base` order-3 page | `prepare_good_kernel_page` (`util.c:4530`) | never in this route: no release, close or re-prepare on the return paths | yes | no in this process: the spray that holds it is torn down at exit and the physical page returns to the allocator (**inferred**) |
| `fake_lock`, `fake_task`, `fake_fops` | inside that page (`util.c:1783-1787`) | with the page | yes | no |
| `ashmem_misc.fops` (global) | the walk's `rb_erase` via `fake_parent` (`util.c:1790`, `rtmutex.c:661-740`) | no restore on this route (only the W1 test keeps `fops_before`) | yes if the walk completed | **yes** — the kernel keeps a pointer into a page the process no longer owns |
| planted waiter (victim kernel stack) | pselect fd-set copy | kernel stack is reused by the next syscall or the return | no | no (task-scoped) |
| victim `pi_blocked_on` | left dangling by the requeue rollback (`main.c:355-375`) | cleared only when the kernel next touches the field; by construction it points into the stack | n/a | no (task-scoped) |
| dump slot children (2) | `util.c:1396` | PDEATHSIG on parent death; no explicit kill | yes (parked) | killed |
| dump scan mappings | `kernelsnitch_setup`, `util.c:1433` | `kernelsnitch_cleanup` only in a terminal state (`util.c:1488-1496`) | yes if non-terminal | freed with the child |
| punch watchdog | `main.c:32-66` | success path only (`main.c:68-78`) | yes if the route never returns | orphan for up to 90 s |
| watchdog pipe | `main.c:37` | `finish_punch_watchdog` (`main.c:70-75`) | yes | closed by exit |
| route threads | `main.c:349-351` | never joined | yes | killed by exit |
| `punch_watchdog_pid` / pids | `main.c:346` | never cleared | yes | stale |

Rows that describe physical page ownership after exit are **inferred** from the
spray lifecycle; no device measurement covers them. Everything else in the
table is **source**.

Two consequences, both **source**:

- The route has no owner for its fake page. Any exit path that returns leaves
  the page to process teardown, and the global `ashmem_misc.fops` write (if the
  walk reached it) outlives the process.
- `pre_ctx` children are the only child set without a `release_ctx_children`
  call. They are not leaked: `PR_SET_PDEATHSIG` kills them at parent death.
  They are simply never reaped by the payload.

## 4. Concurrency

| shared state | writer | reader | ordering | hazard if the ordering fails |
| --- | --- | --- | --- | --- |
| `route_done` | waiter (`main.c:148`) | constructor (`main.c:378`) | seq_cst atomic, 10 ms poll | a premature read would release the process while the route still runs; only `main.c:148` stores 1 |
| `waiter_waiting`, `owner_started` | waiter, owner | constructor (`main.c:353-355`) | seq_cst atomic, 1 ms sleep | the requeue could run before the target thread waits |
| `plant_ready` | waiter (`fops.c:361`) | consumer (`main.c:231-235`) | seq_cst atomic, spin | the punch could walk an unplanted stack |
| `consumer_calls`, `consumer_inflight` | consumer (`main.c:277-300`) | waiter (`fops.c:431-452`, `main.c:138`) | seq_cst; `inflight=0` is stored after every result update (`main.c:288-298`) | the waiter would log stale `last_ret`/`walked` |
| `punch_consume_go` | waiter (`main.c:130`), consumer (`main.c:238`, `main.c:303`, `fops.c:355`) | consumer (`main.c:205-216`) | seq_cst | a missed window means `calls=0` and a failed route |
| `punch_consume_stop` | never set to 1 | consumer (`main.c:204`) | — | the consumer can never terminate except by process death |
| `owner_chain_done` | owner (`main.c:171`) | waiter (`main.c:151`) | seq_cst, 1 ms sleep | the waiter would unlock before the chain is built |
| `page_base`, `fake_*` | constructor before threads; waiter on a retry (`fops.c:195-207`) | waiter only | plain writes, no atomic | a retry while attempt 1's punch is in flight would plant against a new page while the consumer still targets the old waiter |
| watchdog pipe | constructor (`main.c:72`) | watchdog child (`main.c:48-49`) | blocking pipe | a failed write leaves `waitpid` bounded only by the child's 90 s poll (`main.c:79`) |

## 5. Termination scenarios

| scenario | how it ends | what remains | device exposure |
| --- | --- | --- | --- |
| bail before threads | return 3, 0 or 4 | `pre_ctx` children until PDEATHSIG; no kernel pointer written | none |
| summary path | kill pipe child, `sleep(5)`, exit 0 | route threads killed at exit; `ashmem_misc.fops` may still point into the page | a later `/dev/ashmem` open from any process can call a dangling table |
| parent killed by the watchdog | `SIGKILL` after 90 s, status 137 | threads die; the consumer may be inside the punch; dump children die by PDEATHSIG | if the punch was in flight, its kernel state is left mid-walk |
| parent dies without writing the watchdog byte | watchdog exits after 90 s and may signal a reused pid | watchdog orphan | none to the kernel; a process-signal hazard |
| device stops answering during the punch | nothing in the payload runs | unknown | measured: 3 losses, each within the 4 s health check |

## 6. Watchdog, specifically

Source facts (`main.c:32-78`):

- The parent pid is captured once (`main.c:34`) before the fork.
- The child's only inputs are the pipe and a 90 s `poll` (`main.c:48-52`).
- On timeout the child sends `SIGKILL` to the stored pid (`main.c:52`).
- The child sets no `PR_SET_PDEATHSIG`; that call exists for the leak and dump
  children (`util.c:1267`, `util.c:1407`) but not here.
- `finish_punch_watchdog` writes one byte and then waits unconditionally
  (`main.c:68-79`); the return value of `write` is ignored.

So a parent that exits before the write leaves a live child that, up to 90 s
later, signals whatever process now has that pid. **Source**, not exercised on
any host or device. The audit proposes no change; the property is stated so it
can be tested separately.

## 7. Observability

| record | distinguishes | reachable | status |
| --- | --- | --- | --- |
| harness exit code | 0 ⇔ lost, non-zero ⇔ alive | host | **measured, 3/3 and 13/13**; the 255/124 values have no matching payload exit path, so codes cannot be mapped to payload paths |
| `ro.boot.bootreason` | reboot cause | shell | **per boot, not per run**: the same string on 15 consecutive starts, so it classifies only a *transition* (`failure-boundary.md` §3); no boot id was recorded |
| `/data/local/tmp/ks_log.txt` | last executed line (fsync per line) | shell (0666) | **never retrieved**; written by all 16 runs from offset 0, without truncation |
| `dumpsys dropbox` `SYSTEM_LAST_KMSG` | panic vs none | shell (used by `~/quick_kp.sh`) | needs reconnect |
| pstore / `console-ramoops` | panic or lockup residue | root | not available to the shell |
| `dmesg`, `logcat -b kernel` | fault vs lockup | unverified | needs a device and a capability check |
| USB enumeration | on-bus vs off-bus | host | **measured**: absent now, no Samsung device on the bus |
| `sys.boot_completed`, uptime | rebooted vs hung | shell | needs a reconnect |

Per-run last-observed events, and the five-field evidence table for the walk,
are in `causal-timeline.md`. That table is the authority for what any run is
credited with: no run has yet completed the punch, the route or the summary.

## 8. Findings, ranked

1. **The watchdog can signal a reused pid.** No parent-death guard, stored pid,
   90 s window (`main.c:34-52`). *Source.* It cannot explain a device vanishing
   inside a punch call; it matters for any run that dies before the write.
2. **The fake page has no owner.** No path releases it, and the global
   `ashmem_misc.fops` write is not restored (`rtmutex.c:661-740`,
   `util.c:1790`). *Source.* Only reachable after a walk that completes, which
   no lost run achieved.
3. **The consumer and owner threads never terminate.** `punch_consume_stop` is
   never set, `owner_thread` loops forever (`main.c:184-186`, `main.c:204`).
   *Source.* The process is their only owner.
4. **`finish_punch_watchdog` can block for the full 90 s** if the byte write
   fails while the child keeps polling. *Source.*
5. **A lost run's last line is the only boundary.** 10:09 and 18:46 stop in the
   plant; 20:02 stops inside the punch. *Measured.* The cause of none is
   established, and no leftover from findings 1–4 can explain 10:09 or 18:46.

## 9. What a host test could settle

| proposition | test (proposed, not built) |
| --- | --- |
| watchdog stale signal | fork the same pattern with a shortened timeout and a reused pid, assert the signal target |
| watchdog wait bound | close the pipe's write end, assert `finish` blocks until the child's poll expires |
| no release path | static: assert no `munmap`/free/restore for `page_base` or the fops slot on any return path |
| exit-path coverage | enumerate `run_exploit` returns and assert the harness cannot produce 255 from any of them |
