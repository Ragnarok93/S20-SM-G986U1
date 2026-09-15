# Task-Pointer Acquisition Audit for the Sendmsg Route

Date: 2026-09-16  
Scope: offline-only source/log/document audit. No device access, no build,
and no exploit-code change.

## Verdict

**FAKE-TASK-ONLY.**

The currently leaked or derivable data before the sendmsg route does not
produce a valid victim `task_struct` address. The pre-punch primitives expose
`mm_struct`, futex collision addresses, the slab page base, and KASLR/delta.
They do not expose the victim `task_struct` pointer.

However, the sendmsg route does not require the real victim task pointer. A
controlled fake task inside the already prepared order-3 page can support a
bounded PI walk if, and only if, the complete 0x50-byte stale-waiter contract
is satisfied. This is sufficient for a limited walk, not a proof of the CFI,
pipe-physrw, or root stages.

## Evidence

### Pre-sendmsg leak surface

KernelSnitch exports only futex-side data:

- `collision_addr=` rows are accepted futex hash-collision addresses.
- `candidate_addr=` rows are diagnostic timing candidates, not accepted
  same-bucket addresses.
- The solver consumes only `collision_addr=` rows and produces `mm_struct`.

The relevant export logic is in `src/util.c:331-377`. The accepted answer is
only `mm=...` and is consumed as `cached_leaked_mm` in
`src/util.c:1221-1244`.

The device answer file is also mm-only:

```text
build/ks_mm_answer.txt: mm=0xffffffd0c9b0bc40
```

The current full-chain answer is likewise mm-only:

```text
build/ks_mm_answer_fullchain_m5_fdprotect_run1.txt
```

No field in this pre-fops surface yields `task_struct`.

### Post-sendmsg capability is not pre-sendmsg capability

`mm_struct.owner` is at `MM_OWNER_OFF=0x310` on alioth
(`src/targets/alioth-4.19.157-perf/target.h:128`). In principle, reading
`mm + 0x310` could recover the owner task, but the arbitrary kernel-read
primitive is the configfs/ashmem fops path. That primitive is installed only
after the CFI stage succeeds:

- `configfs_read_once()` is implemented at `src/util.c:1463-1528`.
- The CFI stage and fops install path are `src/fops.c:399-585`.
- `install_child_root()` calls `install_android_root()` only after the fops
  stage has already succeeded (`src/fops.c:396`).

Likewise, the existing task walk starts from `init_task.tasks`, not from the
KernelSnitch mm leak:

- `find_task_by_tgid()` is at `src/root.c:140-178`.
- `install_android_root()` derives `current_task_addr` from
  `init_task.tasks.prev` and only falls back to the TGID walk at
  `src/root.c:308-352`.

Both are post-fops root-stage capabilities. They do not satisfy the
pre-sendmsg requirement.

## Fake-task support for a limited PI walk

The controlled page already contains a self-contained fake task:

```text
fake_task  = base + FAKE_TASK_OFF (0x3200)
fake_w0    = base + W0_OFF        (0x2220)
fake_lock  = base + LOCK_OFF      (0x1350)
```

The offsets are alioth constants in
`src/targets/alioth-4.19.157-perf/target.h:94-128`. `prepare_skb_payload()`
computes `fake_task = payload_base + FAKE_TASK_OFF` and deliberately sets the
waiter's `task`, `task_group`, and `pi_top_task` to `fake_task` rather than
real `init_task`/`root_task_group` in FOPS mode
(`src/util.c:1078-1118`).

For a bounded walk, the following fields in the fake task are set:

```text
usage           = 0x100
prio            = 120
normal_prio     = 120
pi_lock         = 0
pi_waiters      = empty
pi_top_task     = fake_task
pi_blocked_on   = fake_w0
```

The relevant writes are in `src/util.c:1164-1183`.

This supports the limited shape previously required by
`docs/specs/h1a-d-spin-root-cause.md`: a complete stale waiter whose
`task` is a controlled fake task and whose `lock` is a controlled fake lock.
The sendmsg placement report already proves that native blocking TCP
`sendmsg` can write the full waiter window, including `task` and `lock`
(`docs/specs/kernel-frame-placement-enumeration.md`). Therefore the route can
encode `fake_task` without knowing the victim's real `task_struct`.

The kernel initially sets `waiter->task` to the victim's real task during
requeue (`kernel/locking/rtmutex.c:946-965`). That pointer is not leaked. The
sendmsg shape must therefore overwrite `waiter->task` with `fake_task`; merely
controlling `lock` is not enough for a fake-task-only walk.

## Risk boundary

The fake-task path is bounded only under the complete contract:

1. The full 0x50-byte stale waiter is written before punch.
2. `waiter.task` is exactly `fake_task`.
3. `waiter.lock` is exactly `fake_lock`.
4. The fake lock's `wait_lock` is free.
5. The fake waiter/tree and fake task PI fields remain coherent.

The current code appears designed to satisfy the first five conditions, but
there is no device proof that a full 0x50 sendmsg shape plus this fake-task
page reaches a clean finite exit on alioth. In particular, if the walk reaches
`rt_mutex_setprio()` on the fake task, the safe early-return path depends on
`p->pi_top_task == pi_task` and equal non-DL priority. If that early return is
not taken, scheduler-specific fields such as `sched_class`, runqueue state, or
`sched_task_group` become relevant and are not validated as a full task
object.

The prior word-0 and task-0 probes remain unsafe. The pselect fallback audit
shows that leaving `task == 0` can reach `wake_up_process(NULL)`
(`docs/specs/pselect-word0-fallback-audit.md`). A fake task avoids that, but
does not by itself make the downstream CFI/root path safe.

## Bottom line

- **No valid victim `task_struct` before sendmsg:** yes.
- **Can the route proceed without it:** yes, by using `fake_task`.
- **Does that prove full-chain safety:** no.

Classification remains **FAKE-TASK-ONLY** until a device run or a stronger
offline proof shows that the sendmsg-written fake waiter/fake task chain exits
cleanly and reaches the intended CFI stage without spin, fault, or scheduler
field dereference.
