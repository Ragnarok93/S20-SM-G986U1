# Punch-path audit — `consumer punch enter` and after

Status: offline source audit at `c9845a1`. No device run, no code change, no
gate change. Nothing here establishes the cause of the loss; it establishes what
the line starts and which of its steps can fault, block, or outlive the caller.

Sources, pinned by content:

| source | identity |
| --- | --- |
| payload | `src/` in this tree at `c9845a1` |
| kernel | `~/s20_kernel_chxl1`, `Makefile` 4.19.113 (Samsung CHXL1) |
| `kernel/sched/core.c` | sha256 `6bd6cfc7decb78d43f942b4e6b14511852c4f907bf29390288813b723bd1cfa7` |
| `kernel/locking/rtmutex.c` | sha256 `36b187173cfb3934a5c9d51952bce58c2f41701d3585326f18cd3965e46a21f9` |

Line numbers below refer to those two files at those hashes.

## 1. The measured boundary

| run | last payload line | `payload_exit` | device |
| --- | --- | --- | --- |
| 10:09 | `waiter futex returned ret=-1 errno=110` | 0 | LOST |
| 18:46 | `waiter futex returned ret=-1 errno=110` | 0 | LOST |
| 20:02 | `consumer punch enter tid=5202` | 0 | LOST |
| 18:25 | `KS_PUNCH_SKIPPED page=0` | 255 | ALIVE |
| 12 other runs | payload bail or harness timeout | 255 / 124 | ALIVE |

Two measured facts follow.

- `payload_exit` is the harness's `$?` for `timeout … adb shell "… id"`
  (`~/run_fast.sh:37-38`, outside this repository). No payload code prints that
  string.
- Every lost run reports 0; every surviving run reports 255 (payload bail) or
  124 (harness timeout). The 0 is therefore a property of the failed transport,
  not of a completed payload.

Only the 20:02 run reaches the punch. 10:09 and 18:46 stop at the plant stage,
after the futex returns and before `KS_PLANT_SHALLOW` is written.

## 2. The payload cannot have exited there

In source order:

1. `load()` (constructor, `preload.c:388`) calls `run_exploit()` synchronously.
2. `run_exploit()` reaches `run_main_route_threads()` (`main.c:509`).
3. `run_main_route_threads()` blocks in `while (!route_done) usleep(10000)`
   (`main.c:378`).
4. `route_done` is stored only by the waiter, after its plant/punch routine
   returns (`main.c:146-148`).
5. The last payload line is printed by the consumer before its first syscall
   (`main.c:279`). Its successors (`main.c:283`, `main.c:299`), the waiter's
   `shallow plant punched` (`main.c:139`), `main route done` (`main.c:385`) and
   `pipe-physrw-summary` (`main.c:528`) are all absent from the log.

Every path that ends in exit 0 is downstream of a line that never appeared, so
exit 0 here cannot be the payload's status. What the adb client returns on a
transport loss is not established by this audit; the corpus is 3 lost runs → 0
and 13 survivors → 255/124.

## 3. Userspace path after the line

| # | step | code | note |
| --- | --- | --- | --- |
| 1 | `sched_setattr(tid, SCHED_OTHER, nice=1)` | `main.c:281` → `util.c:790` | target is the waiter tid; `PSELECT_CONSUMER_NICE` is 1 (`common.h:188`) |
| 2 | on any error, `setpriority(PRIO_PROCESS, tid, 1)` | `util.c:801-810` | `set_user_nice()` (`core.c:4712`) does not call `rt_mutex_adjust_pi`; a fallback does not walk the chain |
| 3 | `punch_walk_marker()` | `fops.c:167` | opens `/dev/ashmem`, reads `ashmem_misc.fops` and compares it with `fake_fops` — the first kernel entry after the walk |
| 4 | `consumer_inflight=0`, `consumer punch done` | `main.c:298-300` | release signal for the waiter's spin (`fops.c:439-452`) |
| 5 | waiter returns, `route_done=1`, `FUTEX_UNLOCK_PI`, wait for owner | `main.c:148-153` | |
| 6 | main thread: `main route done`, watchdog byte, `waitpid` | `main.c:385-386` | |
| 7 | summary, return, `id` runs and exits | `main.c:528` | |

There is no cleanup between 4 and 7. `main.c` never joins the waiter, owner or
consumer thread (it has no `pthread_join` call); the leaked page is not released,
`ashmem_misc.fops` is not restored, and `punch_consume_stop` is only ever stored
as 0, never set to stop the consumer.

## 4. Kernel path

| # | step | source | what it touches |
| --- | --- | --- | --- |
| 1 | `SYSCALL_DEFINE3(sched_setattr)`: copy attr, `find_process_by_pid`, `get_task_struct`, `sched_setattr(p, &attr)` | `core.c:5418-5447` | real task |
| 2 | `sched_setattr()` → `__sched_setscheduler(p, attr, true, true)` | `core.c:5257-5261` | `pi = true` is fixed at this call |
| 3 | `__sched_setscheduler`: policy and permission checks, priority update, rq unlock, `if (pi) rt_mutex_adjust_pi(p)` | `core.c:5213` | runs after `preempt_disable()` |
| 4 | `rt_mutex_adjust_pi`: `waiter = task->pi_blocked_on`; `rt_mutex_waiter_equal(waiter, task_to_waiter(task))`; `next_lock = waiter->lock` | `rtmutex.c:1126-1146`, equal at `253-265` | first reads of the planted waiter: its `prio`, then its `lock` |
| 5 | `rt_mutex_adjust_prio_chain`: re-read `pi_blocked_on`; `next_lock != waiter->lock`; `lock = waiter->lock`; `raw_spin_trylock(&lock->wait_lock)` | `rtmutex.c:507-589` | first write: the fake lock's wait_lock |
| 6 | owner test; `prerequeue_top_waiter = rt_mutex_top_waiter(lock)` | `rtmutex.c:600-661` | reads `lock->waiters.rb_node` and dereferences it as a waiter |
| 7 | requeue: `rt_mutex_dequeue(lock, waiter)`, `waiter->prio = task->prio`, `rt_mutex_enqueue(lock, waiter)` | `rtmutex.c:664-685` | writes rb links and prio into the fake waiter |
| 8 | owner branch: `task = rt_mutex_owner(lock)`, `get_task_struct(task)`, pi-tree swap, `rt_mutex_adjust_prio(task)` | `rtmutex.c:710-740`, `347-355` | refcount and `pi_waiters` operations against the fake task; `rb_erase` of `pi_tree_entry` is the fops write |
| 9 | no deferred work: the adjust path calls `wake_up_process` directly; the `wake_q` in this file belongs to unlock paths and drains before those return | `rtmutex.c:705`, `1058`, `1448` | |

Benign exits exist and would leave the device alive: `!waiter`
(`rtmutex.c:518`), `next_lock != waiter->lock` (`537`), equal priority (`569`),
`max_lock_depth` (`474`). The observed run cannot be assigned to one of them:
nothing after the call was written.

## 5. Loss modes this path allows

| mode | what it would leave | available now |
| --- | --- | --- |
| oops/panic inside the walk | dropbox `SYSTEM_LAST_KMSG`, pstore console-ramoops, boot reason | device absent; not readable |
| lockup/watchdog reset (spin with IRQs off) | no oops; watchdog reset record; pstore may be empty | device absent |
| USB/transport only | payload still running; `adb reconnect` succeeds | no device on the bus at all |
| payload exit | status of `id` | contradicted by §2: the route had not returned |

`bootreason=kernel_panic,null` is not evidence for this loss: it is present in
survivors too (`20261103_182504.log`), so the property is stale across boots.

## 6. Work that outlives the payload

- **Watchdog child** (`main.c:32-78`): polls 90 s, then `kill(parent_pid,
  SIGKILL)`; it is reaped only on the success path (`main.c:386`). If the payload
  dies first the child is orphaned and can signal a recycled pid 90 s later. It
  does nothing else to the device.
- **Kernel state**: in the `PAGE_PAYLOAD_FOPS` mode this run prepared
  (`util.c:1783-1792`), `fake_parent = ashmem_misc.fops - 8`, so a completed
  walk installs `fake_fops` in `ashmem_misc.fops`. Nothing in this route restores it, so any later
  `/dev/ashmem` open by any process would call a table in a page the payload
  owns.
- **Dangling `pi_blocked_on`**: it points into the victim's kernel stack. It
  disappears with the task, but another `sched_setattr` on that task before exit
  would walk the same stack bytes again.

## 7. What this audit does not establish

- which step faulted, or whether the walk hung;
- whether the leaked page was still owned by the payload at the punch;
- whether the handset rebooted or hung: nothing appeared on the bus after the
  loss.

The next run should observe this path, not change it: a second `adb` stream
(`dmesg -w`, `logcat -b kernel`) during the punch, then the boot reason, dropbox
`SYSTEM_LAST_KMSG` and pstore after reconnect. All of those need the handset.
