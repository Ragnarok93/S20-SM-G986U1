# H1A-D EDEADLK 后 PI-chain spin 根因审计

Date: 2026-09-15
Scope: offline audit only. No device command, no exploit build, and no
`src/` change in this task.

Primary evidence:

- Device log: `build/ks_log_h1a_d_run1.txt`.
- H1A design: `src/slide.c` (`H1A-D` and `slide_h1a_pselect_punch()`).
- Kernel source: `materials/kernelsrc/sm8250-a13-stable`.
- Kernel ELF: `k40rom/boot/kernel.elf`,
  SHA256 `F73A2163A0A8311ED44C731951E8E5A7A789B1B08E1931C920632A9E3098B662`.
- H1A binary used in the cited log:
  `build/alioth/bin/preload_slide_h1a.so`,
  SHA256 `2E6815AA9865244FC62F4A04B04292B451C0C489FDA5DE637AE71B25FC0C3711`.

Related audits:

- `docs/specs/rt-waiter-placement-audit.md` fixes the stale-waiter frame and
  proves the v12 encoder cannot survive `do_select()` early break.
- `docs/specs/pselect-word0-fallback-audit.md` rejects the word-0-only shape.
- `docs/specs/safe-placement-probe-design.md` defines the only currently
  proposed fail-closed placement discriminator.

## Verdict

The H1A-D `FUTEX_CMP_REQUEUE_PI` result `-EDEADLK` is the expected gate: it
proves that the proxy requeue created and rolled back the victim's rt-waiter
state. The remaining punch hang is caused by that rollback leaving the
victim's `task->pi_blocked_on` pointing at a stack-local `rt_waiter` while
the subsequent pselect can only place a partial waiter shape.

The v12 H1A pselect path first zeroes the full output region and then, due to
`do_select()` early break, writes at most one readiness word into the stale
waiter window. In the nominal alioth frame it writes `waiter + 0x00`; it does
not write `waiter->lock` at `waiter + 0x38`. The PI walk therefore consumes
an incoherent stale waiter instead of a fully controlled one.

The kernel chain's `trylock`-failure path is an unbounded retry loop. The
exact 4.19 source is:

```c
if (!raw_spin_trylock(&lock->wait_lock)) {
    raw_spin_unlock_irq(&task->pi_lock);
    cpu_relax();
    goto retry;
}
```

at `kernel/locking/rtmutex.c:584-588`. Thus, once a non-NULL stale/fake
`waiter->lock` is reachable and its `rt_mutex.wait_lock` cannot be acquired,
the punch can spin forever. If the nominal frame is exact and
`waiter->lock == NULL`, the same partial shape predicts an immediate kernel
fault/hard wedge rather than a clean recoverable return. The available log
has no post-trylock PC, so it cannot distinguish "retry loop" from a
fault-equivalent wedge; both are NO-GO and both explain the watchdog/reboot
observation.

The H1A-D run is consistent with this mechanism:

```text
H1A-D requeue ret=-1 errno=35
H1A-D page prepared base=ffffffc1b1cf8000 ...
H1A-D waiter futex_requeue_pi ret=-1 errno=110
H1A punch setup nfds=320 timeout=0.200000
H1A punch enter tid=25730 nice=1
H1A punch watchdog timeout parent=25728 status=stuck
```

`errno=35` is `EDEADLK`; `errno=110` is the victim's later timed wakeup. The
punch never returns and the 90-second watchdog fires.

## State-machine trace

| Step | Actor | Kernel state | Evidence |
|---|---|---|---|
| S0 | victim | `FUTEX_LOCK_PI` acquires `pi_chain`; then enters `FUTEX_WAIT_REQUEUE_PI` on `f_wait`. `futex_wait_requeue_pi()` declares stack-local `rt_waiter`, initializes it, and sets `q.rt_waiter = &rt_waiter`. | `src/slide.c:1261-1277`; `kernel/futex.c:3311-3352` |
| S1 | owner | `FUTEX_LOCK_PI` acquires `pi_target`; then blocks on `pi_chain` held by the victim. | `src/slide.c:1294-1321` |
| S2 | requeue | `FUTEX_CMP_REQUEUE_PI` proxy-enqueues the victim's stack waiter onto `pi_target`. `task_blocks_on_rt_mutex()` sets `waiter->task`, `waiter->lock = pi_target`, and `victim->pi_blocked_on = waiter`. | `src/slide.c:1376-1382`; `kernel/locking/rtmutex.c:934-988` |
| S3 | requeue | The PI chain sees the victim -> target -> owner -> chain cycle and returns `-EDEADLK`. | `kernel/locking/rtmutex.c:589-603` |
| S4 | requeue | `rt_mutex_start_proxy_lock()` calls `remove_waiter()` on failure. That pre-fix code clears `current->pi_blocked_on`, where `current` is the requeue thread, not the victim. The victim's stale pointer survives. | `kernel/locking/rtmutex.c:1798-1805`; `kernel/locking/rtmutex.c:1068-1090` |
| S5 | requeue | `futex_requeue()` clears `this->pi_state`, puts the PI-state reference, and breaks without waking the victim. | `kernel/futex.c:2241-2266` |
| S6 | victim | Its original futex timeout wakes it before requeue. `handle_early_requeue_pi_wakeup()` returns `-ETIMEDOUT` and jumps to key cleanup. The later `rt_mutex_wait_proxy_lock()/cleanup/free_waiter` block is skipped. | `kernel/futex.c:3372-3378`; `kernel/futex.c:3419-3429` |
| S7 | victim | Userland sees `ret=-1 errno=110`, then calls pselect. The old stack-local waiter is still below SP and `victim->pi_blocked_on` still points to it. | log; `src/slide.c:1276-1282` |
| S8 | victim | `core_sys_select()` zeroes `res_in/res_out/res_ex`, then `do_select()` early-breaks after the first ready word. For the v12 shape this writes only the first word in the stale waiter and cannot establish `waiter->lock`. | `fs/select.c:649-651`; `fs/select.c:451-553`; `src/slide.c:107-176` |
| S9 | punch thread | `sched_setattr` -> `sched_setscheduler(..., pi=true)` -> `rt_mutex_adjust_pi(victim)`. It reads the stale `victim->pi_blocked_on`. | `kernel/sched/core.c:5319-5320`; `kernel/locking/rtmutex.c:1126-1152` |
| S10 | punch thread | `rt_mutex_adjust_prio_chain()` reads `waiter->lock` and attempts `raw_spin_trylock(&lock->wait_lock)`. A bad non-NULL lock can enter the retry loop; a NULL lock faults on the first spinlock dereference. | `kernel/locking/rtmutex.c:568-588`; `kernel/locking/rtmutex.c:537-589` |

## Frame mapping

The alioth native pselect6 frame is disassembly-verified in the placement
audit:

```text
T            = native syscall wrapper entry SP
stack_fds    = T - 0x210
res_in       = T - 0x198
res_out      = T - 0x170
stale waiter = T - 0x198 .. T - 0x148
waiter.lock  = waiter + 0x38 = T - 0x160
```

For `nfds=320`, each fd set is five words. The stale waiter overlays
`res_in` words 0..4 and `res_out` words 0..4:

```text
waiter field         offset   pselect location
tree_entry.pc        +0x00    res_in word 0
tree_entry.right     +0x08    res_in word 1
tree_entry.left      +0x10    res_in word 2
pi_entry.pc          +0x18    res_in word 3
pi_entry.right       +0x20    res_in word 4
pi_entry.left        +0x28    res_out word 0
task                 +0x30    res_out word 1
lock                 +0x38    res_out word 2
prio                 +0x40    res_out word 3
deadline             +0x48    res_out word 4
```

`zero_fd_set()` clears all five words of each result set before polling. If
the first ready word is `res_in` word 0, only `+0x00` is restored to `1`.
`+0x38`, the lock field, remains zero. If fd-0..2 restoration changes which
word first becomes ready, the outcome shifts to another partial word; it
still cannot write the complete 0x50-byte waiter.

## ELF evidence

The following addresses are from `k40rom/boot/kernel.elf`.

### Trigger path

```text
__arm64_sys_sched_setattr = 0xffffff8008103900
call sched_setscheduler   = 0xffffff8008103d18 -> 0xffffff8008102d98
return/result store       = 0xffffff8008103d28
```

The call sequence at `0xffffff8008103d08` passes the copied attributes in
`x1 = sp`, the task in `x0`, and `w2=w3=1`, matching the PI-enabling
`sched_setscheduler` path.

### PI adjustment

```text
rt_mutex_adjust_pi        = 0xffffff8008150190
read victim.pi_blocked_on = 0xffffff8008150264: ldr x25, [x19, #0x8d8]
read waiter.lock          = 0xffffff80081501a4: ldr x21, [x8, #0x38]
call prio_chain           = 0xffffff80081501e0 -> 0xffffff80081501f8
```

The same `waiter->lock` read also appears inside the chain body:

```text
0xffffff8008150280: ldr x8,  [x25, #0x38]
0xffffff8008150284: cmp x21, x8
0xffffff80081502f8: mov x0, x21
0xffffff80081502fc: bl  0xffffff8009500ad0   ; raw_spin_trylock
```

There are equivalent retry entry points at `0xffffff80081503a0` and
`0xffffff80081504c0`; the latter is followed by the retry sequence:

```text
0xffffff80081504c0: mov x0, x21
0xffffff80081504c4: bl  0xffffff8009500ad0
0xffffff80081504c8: cbnz w0, 0xffffff80081503cc
0xffffff80081504cc: mov x0, x26
0xffffff80081504d0: bl  0xffffff8009500d88
0xffffff80081504d4: mov x0, x26
0xffffff80081504d8: yield
0xffffff80081504dc: bl  0xffffff8009500c58
0xffffff80081504e0: ldr x25, [x19, #0x8d8]
0xffffff80081504e4: cbnz x25, 0xffffff8008150474
```

This is the binary image of `cpu_relax(); goto retry;`: release/reacquire the
victim PI lock, yield, reload `pi_blocked_on`, and try the same lock again.

### Spinlock primitive

```text
_raw_spin_trylock = 0xffffff8009500ad0
0xffffff8009500ae8: ldr   w10, [x0]
0xffffff8009500aec: cbnz  w10, 0xffffff8009500b18
0xffffff8009500b00: ldaxr w12, [x8]
```

So the first dependent operation on `waiter->lock` is a load of
`lock->wait_lock` at `rt_mutex + 0`. A NULL `waiter->lock` faults at this
load. A non-NULL lock whose first word is already held returns failure and
reaches the chain retry loop.

## Spin candidate conditions

Let `L = stale_waiter->lock`.

### C1: nominal partial shape, `L == 0`

The exact frame plus `zero_fd_set()` makes `L = 0`, and the first ready word
does not write `+0x38`. The chain calls `_raw_spin_trylock(NULL)`.
`0xffffff8009500ae8` then loads from address zero. The expected result is an
immediate kernel exception or hard wedge, not a graceful userspace error.

This is unsafe by itself. It must not be characterized as a safe "NULL exit".

### C2: non-NULL unrecoverable lock

If placement is shifted, if an earlier partial write leaves a stale non-NULL
pointer, or if the controlled fake lock is reached while its `wait_lock` word
remains held, then `raw_spin_trylock()` returns zero. The chain releases the
task PI lock, yields, reloads the unchanged stale `pi_blocked_on`, and jumps
to retry. Unless some other CPU changes `pi_blocked_on`, `waiter->lock`, or
the spinlock word, this loop does not terminate.

This is the only true unbounded-spin candidate and is the most direct
explanation for a watchdog followed by a required reboot.

### C3: real PI-cycle lock survives

If the stale waiter is not overlapped at all and still retains
`waiter->lock = pi_target`, a correct chain could detect
`rt_mutex_owner(lock) == top_task` and return `-EDEADLK`. That path is
destroyed by pselect's zeroing/partial-write behavior in the nominal frame.
The observed stuck punch is therefore not evidence that the real-cycle
detection path was preserved.

## Correct path if waiter/placement were valid

For a controlled PI walk, every byte of the 0x50-byte stale waiter must be
established before punch:

```text
tree_entry  = terminal red/black node as required by the target rbtree op
pi_entry    = terminal node
task        = controlled fake task
lock        = controlled fake lock
prio        = FAKE_WAITER_PRIO
deadline    = controlled/zero
```

The controlled lock page must have:

```text
rt_mutex.wait_lock  = 0
rt_mutex.waiters    = valid terminal/empty rb_root_cached
rt_mutex.owner      = intended owner, with all downstream task fields valid
```

With that complete contract, the expected paths are bounded:

1. `rt_mutex_adjust_pi()` reads the complete stale waiter.
2. It compares `waiter->lock` with the saved `next_lock` and reaches the
   intended controlled lock.
3. `raw_spin_trylock(&fake_lock->wait_lock)` succeeds.
4. The walk either inserts into a valid empty tree and returns when the
   owner is null, or follows a fully controlled owner chain to a normal
   `EDEADLK`/priority adjustment exit.
5. `sched_setattr` returns a finite result (`0` or an expected negative
   errno). The punch watchdog does not fire.

The current pselect output-region semantics cannot provide this. A word-0-only
or lock-only partial shape is explicitly NO-GO:

- word 0 writes `tree_entry.__rb_parent_color`, not `waiter->lock`;
- `do_select()` early break prevents reliably writing later words;
- restoring stdio fds over fd 0..2 makes even the first ready word
  unreliable;
- leaving `waiter->lock == NULL` produces C1;
- inventing a non-NULL lock without the full 0x50-byte and fake-task
  contract produces C2.

The only offline-approved direction is the fail-closed SP-1 placement
discriminator in `docs/specs/safe-placement-probe-design.md`, and only after
its static checklist is independently completed. It still does not constitute
a GO for the real H1A-D punch, CFI, or full chain.

## GO state

**NO-GO for another H1A-D punch or full-chain run.**

The cited log proves the kernel reached the PI walk but does not contain the
faulting PC or the lock value actually dereferenced. No complete offline proof
exists for a safe placement, and the current pselect design cannot shape the
required waiter. Any next device run without that proof would repeat the C1/C2
failure mode.
