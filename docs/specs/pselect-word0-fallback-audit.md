# pselect word-0 fallback audit (alioth 4.19.157)

Date: 2026-09-15
Scope: offline audit only. No device command, no exploit build, and no
`src/` change. The cited kernel is
`materials/kernelsrc/sm8250-a13-stable`; frame offsets were checked against
`k40rom/boot/kernel.elf` and `k40rom/boot/kallsyms.txt`.

Question: in the alioth dangling-waiter window, if pselect can shape only
`res_in` word 0 as a qword intended to be `fake_lock`, can the
`rt_mutex_setprio`/`sched_setattr` PI path reach a safe touch or the intended
fake-page state?

## Conclusion

**NO-GO.**

**Crash risk: HIGH** if `sched_setattr` is issued against the victim with the
stale `pi_blocked_on` and this word-0-only shape. The PI walk reads
`waiter->lock` at `waiter + 0x38`, but a `res_in` word-0 write lands at
`waiter + 0x00`. The remaining waiter bytes are zeroed by pselect, so
`waiter->lock == NULL` and the first `raw_spin_trylock(&lock->wait_lock)`
dereferences NULL. This is neither a safe touch nor the expected state.

Do not run this fallback, and do not issue a one-shot `sched_setattr` punch
with this shape.

## Alioth frame evidence

The alioth native pselect6 frame is:

```text
T = syscall wrapper entry SP
stack_fds = T - 0x210
res_in    = stack_fds + 120 = T - 0x198
res_out   = stack_fds + 160 = T - 0x170
stale rt_waiter = T - 0x198 .. T - 0x148
```

Device-build evidence:

- `__arm64_sys_pselect6` at `kernel.elf:0xffffff80082dfb40` executes
  `sub sp, sp, #0xa0`, then calls `core_sys_select` at
  `0xffffff80082dfd44`.
- `core_sys_select` at `kernel.elf:0xffffff80082def20` computes five words
  per 320-fd set and uses `add x19, sp, #0x50` at
  `0xffffff80082def90`; therefore `stack_fds = T - 0xa0 + 0x50 = T - 0x210`.
- In the same prologue, `res_in = x19 + 120`, `res_out = x19 + 160`, and
  `res_ex = x19 + 200`.
- At `0xffffff80082df184-0xffffff80082df1b0`, the build memsets all three
  result regions to zero before `do_select()`.
- `rt_mutex_adjust_pi` at `kernel.elf:0xffffff8008150178` loads
  `task->pi_blocked_on` at task offset `0x8d8`. At
  `0xffffff80081501b4` it loads `waiter->lock` from waiter offset `0x38`.
- `rt_mutex_adjust_prio_chain` at `kernel.elf:0xffffff8008150290` also loads
  the same `waiter->lock` from waiter offset `0x38` before trying to take
  `lock->wait_lock`.

This matches the materials tree, not an upstream input-region assumption.
The materials source places the six 40-byte result regions in the order
`in | out | ex | res_in | res_out | res_ex`
(`fs/select.c:606-646`) and zeroes `res_in/res_out/res_ex` before polling.

## Why word 0 cannot deliver fake_lock

For `nfds=320`, `struct rt_mutex_waiter` is `0x50` bytes and overlays the
result regions as follows:

```text
waiter field        waiter offset   pselect region
tree_entry.pc       +0x00           res_in word 0
tree_entry.right    +0x08           res_in word 1
tree_entry.left     +0x10           res_in word 2
pi_entry.pc         +0x18           res_in word 3
pi_entry.right      +0x20           res_in word 4
pi_entry.left       +0x28           res_out word 0
task                +0x30           res_out word 1
lock                +0x38           res_out word 2
prio                +0x40           res_out word 3
deadline            +0x48           res_out word 4
```

Writing the `fake_lock` qword into `res_in` word 0 therefore writes
`waiter->tree_entry.__rb_parent_color`, not `waiter->lock`. Because pselect
zeroes the other result words and `do_select()` stops after the first word
that produces readiness (`fs/select.c:514-552`), no later result word is
reliably written. In particular, `res_out` word 2 remains zero.

The `sched_setattr` trigger path is:

1. `__sched_setscheduler(..., pi=true)` calls `rt_mutex_adjust_pi(p)`
   (`kernel/sched/core.c:5319-5320`).
2. `rt_mutex_adjust_pi()` reads `p->pi_blocked_on`; for a normal victim,
   `waiter->prio == 0` is not equal to `task->prio`, so it does not return
   early (`kernel/locking/rtmutex.c:1126-1140`).
3. It passes `next_lock = waiter->lock == NULL` to
   `rt_mutex_adjust_prio_chain()`.
4. The chain sees `next_lock == waiter->lock == NULL`, then reads
   `lock = waiter->lock` and executes
   `raw_spin_trylock(&lock->wait_lock)`
   (`kernel/locking/rtmutex.c:537-589`).

With `lock == NULL`, step 4 is a NULL dereference. Putting `fake_lock` in
word 0 cannot prevent it because the state machine never reads word 0 as
`waiter->lock`.

## Downstream setprio contract

`rt_mutex_setprio()` is only reachable downstream of a successful chain-walk
step after the fake lock has been entered and its owner/PI waiter fields have
been validated (`kernel/sched/core.c:4695-4785`). It cannot repair a missing
`waiter->lock`, and it does not provide an alternate path around
`raw_spin_trylock(&lock->wait_lock)`.

For any one-qword lock-only idea, the separate contract also remains
unresolved: `task == 0` and `rt_mutex_setprio(fake_task, NULL)` must be
audited before it can be considered safe. That is not this word-0 fallback;
the placement mismatch alone is already disqualifying.

## Verification state

- Verified: result-region placement and zeroing in the device ELF.
- Verified: stale waiter starts at `T-0x198` and `waiter->lock` is at
  `+0x38`.
- Verified: materials-tree `rt_mutex_adjust_pi()` and
  `rt_mutex_adjust_prio_chain()` state machine.
- Not attempted: any device run or exploit-code change.

The prior H1A-D kernel-spin observation does not change this verdict; it is
additional evidence that an incorrectly linked stale waiter can force a
device-recovery event rather than a clean userland failure.
