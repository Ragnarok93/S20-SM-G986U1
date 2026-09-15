# Safe placement probe review (SP-1)

Date: 2026-09-15  
Scope: offline static audit only. No device command, no exploit-code change,
no build, and no change to `docs/specs/safe-placement-probe-design.md`.

## Verdict

**UNSAFE. Do not implement, build, deploy, or schedule SP-1.**

The design does not satisfy its own fail-closed requirement. The decisive
failure is not merely that the H1A-D marker-miss assumption is unproven; the
assumed marker-read success path itself can dereference `NULL` before the
claimed safe return.

## Findings

### F1. Marker-miss exit is not proven fail-closed

Claim under test: when the marker misses, the untouched real H1A-D PI cycle
normally exits with `EDEADLK`.

This is contradicted by the available device evidence:

```text
build/ks_log_h1a_d_run1.txt:9
H1A-D requeue ret=-1 errno=35 (expected ret=-1 errno=EDEADLK)

build/ks_log_h1a_d_run1.txt:16
H1A-D waiter futex_requeue_pi ret=-1 errno=110

build/ks_log_h1a_d_run1.txt:18-19
H1A punch enter tid=25730 nice=1
H1A punch watchdog timeout parent=25728 status=stuck
```

The pre-punch requeue gate returning `EDEADLK` does not prove that the later
`sched_setattr` PI walk will also terminate with `EDEADLK`. The observed run
reached the punch and then hung. Therefore `WALKED-WRONG-PLACEMENT` cannot be
declared a safe exit state. At most it is an unproven hypothesis.

### F2. The single-qword pselect shape is mechanically possible, but unsafe

For `nfds=320`, the pselect shape can place one exact qword at
`res_out[2] == waiter + 0x38 == waiter->lock`:

- `core_sys_select()` uses six 40-byte regions:
  `materials/kernelsrc/sm8250-a13-stable/fs/select.c:640-646`.
- It zeroes all three result regions first:
  `fs/select.c:649-651`.
- `do_select()` writes `*routp = res_out`:
  `fs/select.c:544-545`.
- The outer loop stops after the first word that produced readiness:
  `fs/select.c:551`.

Thus marking only `out` word 2 can produce exactly the 64-bit `P` value and a
return count of `popcount64(P)`, subject to the fd/readiness gates. This part
of the design is a conditional PASS.

It is not sufficient to make the probe safe. Because all other result words
are zeroed, the shaped stale waiter has:

```text
waiter.task  = 0
waiter.lock  = P
waiter.prio  = 0
```

### F3. The marker-read path dereferences `NULL`

The design claims that an empty-owner probe lock merely enqueues the waiter,
sees `owner == NULL`, releases the lock, and returns 0. It omits the wake path.

In the 4.19 materials tree,
`kernel/locking/rtmutex.c:661-706`, `rt_mutex_adjust_prio_chain()` does:

1. `prerequeue_top_waiter = rt_mutex_top_waiter(lock);`
2. `rt_mutex_enqueue(lock, waiter);`
3. sees `rt_mutex_owner(lock) == NULL`;
4. sees that the new top waiter differs from the old empty-tree top waiter;
5. executes
   `wake_up_process(rt_mutex_top_waiter(lock)->task);`.

With an empty probe lock, step 1 is `NULL`. After step 2 it is the newly
enqueued stale waiter. But SP-1's single-qword pselect shape has already set
`waiter.task == 0`, so step 5 becomes `wake_up_process(NULL)`.

Therefore the supposedly safe `PLACEMENT-CONFIRMED` path has a kernel NULL
dereference before returning 0. This alone makes SP-1 unsafe.

### F4. Nonzero `rb_root` is not a sufficient enqueue proof

`rt_mutex_enqueue()` does write the waiter node into
`lock->waiters.rb_root`
(`materials/kernelsrc/sm8250-a13-stable/kernel/locking/rtmutex.c:272-295`).
However, the design only defines a positive result as a nonzero
`probe_lock.waiters.rb_root`.

That is too weak:

- unrelated kernel writes to the prepared page can also make a qword nonzero;
- `rb_root` alone does not prove that the value is the stale waiter's
  `tree_entry`;
- it does not prove that `rb_leftmost` and `rb_root` agree;
- it does not prove that every planned SKB copy observed the same write;
- it is observable only after the unsafe punch, so it cannot act as a
  fail-closed precondition.

At minimum, any future design would need an exact postcondition such as
`wait_lock == 0`, `rb_root == rb_leftmost`, `rb_root` aligned and in the
expected kernel address class, and all planned readbacks identical. Even that
would be a stronger diagnostic, not a substitute for avoiding the unsafe walk.

## Required changes before any successor design

1. Do not issue `sched_setattr` while a stale waiter can have
   `task == 0` and a repointed `lock` unless a separately audited kernel path
   proves that no wake or task dereference occurs.
2. Do not rely on marker miss returning `EDEADLK`. A marker miss must suppress
   the punch, not trigger it.
3. Treat a single nonzero `rb_root` as insufficient evidence. Define an exact
   multi-field signature and require all readback copies to agree.
4. Add the post-punch H1A-D result to the checklist. Passing the pre-punch
   requeue gate is not evidence that the subsequent PI walk terminates safely.
5. Any successor that needs both `waiter.task` and `waiter.lock` must explain
   how both can be shaped without violating the 4.19 `do_select()` early-break
   rule, or must use a fundamentally different non-enqueuing probe.

No part of the current SP-1 design is approved for implementation.
