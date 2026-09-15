# Sendmsg PI-shape adversarial audit

Date: 2026-09-16

Scope: offline static audit only. No device access, no build, no run, and no
exploit-code change. The target is
`docs/specs/sendmsg-pi-shape-design.md`.

## Verdict

**PASS** for the exact terminal-root contract, with one correction.

The bounded one-hop PI walk is supported by the 4.19 materials source and by
compiled immediates in the alioth ELF. The terminal-root erase/reinsert stays
inside the stale waiter and the controlled page. A non-NULL fake lock owner
does disable the ownerless wake branch. The fake task early return does avoid
the runqueue and scheduler class.

The correction is that the walk does read `fake_task->dl.deadline` before
reaching `rt_mutex_setprio()`. The design sentence saying that no alioth
`task_struct.dl` offset is required is wrong. For the direct sendmsg shape,
`FT+0x420` must be readable and initialized. The existing order-3 page layout
has `FAKE_TASK_OFF=0x3200`, so `FT+0x420` is `payload_base+0x3620` and remains
inside the controlled 0x8000 page. Thus this is a contract correction, not a
need for more source or a FAIL.

Evidence inputs:

```text
ELF    k40rom/boot/kernel.elf
sha256 F73A2163A0A8311ED44C731951E8E5A7A789B1B08E1931C920632A9E3098B662
source materials/kernelsrc/sm8250-a13-stable
config build/.config
```

The ELF has `.symtab` but no DWARF sections. The offsets below are therefore
corroborated by source-layout arithmetic and compiled immediates, not by debug
types.

## Source path check

The 4.19 call order matches the design:

1. `sched_setscheduler()` unlocks the runqueue and then calls
   `rt_mutex_adjust_pi(p)` (`kernel/sched/core.c:5319-5320`).
2. `rt_mutex_adjust_pi()` locks `task->pi_lock`, reads
   `task->pi_blocked_on`, and performs the waiter/victim priority compare. If
   unequal, it reads `waiter->lock`, unlocks `pi_lock`, takes a task reference,
   and calls `rt_mutex_adjust_prio_chain()` with `orig_lock=NULL`,
   `orig_waiter=NULL`, and `chwalk=RT_MUTEX_MIN_CHAINWALK`
   (`kernel/locking/rtmutex.c:1126-1146`).
3. The chain locks `task->pi_lock`, reads `task->pi_blocked_on`, checks
   `next_lock == waiter->lock`, and then takes `lock = waiter->lock`
   (`kernel/locking/rtmutex.c:516-579`).
4. `raw_spin_trylock(&lock->wait_lock)` succeeds only when
   `fake_lock.wait_lock == 0` (`kernel/locking/rtmutex.c:585-591`).
5. The requeue path records the current top waiter, dequeues the sole lock
   waiter, sets `waiter->prio` and `waiter->deadline` from the fake task, and
   enqueues it again (`kernel/locking/rtmutex.c:673-687`).
6. Because `rt_mutex_owner(lock)` is non-NULL, the ownerless wake block is
   skipped. The owner becomes `fake_task`, the fake task reference is taken,
   and its `pi_lock` is locked (`kernel/locking/rtmutex.c:698-716`).
7. Since the sole lock waiter is also the top waiter, the PI tree is dequeued
   and requeued, then `rt_mutex_adjust_prio(fake_task)` is called
   (`kernel/locking/rtmutex.c:723-725`).
8. `task_blocked_on_lock(fake_task)` returns NULL because
   `fake_task.pi_blocked_on == NULL`
   (`kernel/locking/rtmutex.c:380-383` and `759`).
9. The chain releases the fake task reference and returns 0
   (`kernel/locking/rtmutex.c:777-793`).

## ELF offset check

The compiled chain and setprio paths use the expected offsets. Representative
instructions are listed below; the symbol table has no function sizes, so the
addresses are code anchors rather than complete DWARF-derived ranges.

| object and field | offset | ELF evidence |
|---|---:|---|
| `rt_mutex.wait_lock` | `+0x00` | the chain passes the lock base to trylock; source layout puts the spinlock first |
| `rt_mutex.waiters.rb_root.rb_node` | `+0x08` | walk traverses the lock tree from `lock+0x08` around `0xffffff8008150550` |
| `rt_mutex.waiters.rb_leftmost` | `+0x10` | `ldr x8,[x21,#0x10]` at `0xffffff80081504fc` and `0xffffff800815065c` |
| `rt_mutex.owner` | `+0x18` | owner reads/mask at `0xffffff80081503e8`, `0xffffff8008150430`, and `0xffffff8008150624` |
| `task.usage` | `+0x68` | reference increment/decrement uses `base+0x68` around `0xffffff80081501d8` and `0xffffff8008150408` |
| `task.prio` | `+0xac` | `ldr w8,[x20,#0xac]` at `0xffffff80081021f0` and `0xffffff8008102000` |
| `task.normal_prio` | `+0xb4` | `ldr w27,[x0,#0xb4]` at `0xffffff8008101f60` |
| `task.dl.deadline` | `+0x420` | `ldr x11,[x19,#0x420]` at `0xffffff8008150548` |
| `task.pi_lock` | `+0x8b4` | lock base `x19+0x8b4` around `0xffffff8008150268` |
| `task.pi_waiters.rb_root.rb_node` | `+0x8c0` | reads and tree insert base around `0xffffff8008150698` and `0xffffff8008150734` |
| `task.pi_waiters.rb_leftmost` | `+0x8c8` | `ldr x8,[x19,#0x8c8]` at `0xffffff80081507ec` |
| `task.pi_top_task` | `+0x8d0` | early-return compare `ldr x8,[x20,#0x8d0]` at `0xffffff8008101f7c` |
| `task.pi_blocked_on` | `+0x8d8` | chain reads at `0xffffff8008150274` and `0xffffff8008150804` |

The stale-waiter offsets also match the compiled chain:
`pi_tree_entry` is `+0x18`, `task` is `+0x30`, `lock` is `+0x38`, `prio` is
`+0x40`, and `deadline` is `+0x48`.

## Terminal-root rbtree proof

The lock-tree invariant is:

```text
SW+0x00 = 1          black root, parent NULL
SW+0x08 = 0          no right child
SW+0x10 = 0          no left child
FL+0x08 = SW+0x00    rb_root.rb_node
FL+0x10 = SW+0x00    rb_leftmost
```

The PI-tree invariant is the same with `SW+0x18` as the node and
`FT+0x8c0`/`FT+0x8c8` as the cached root/leftmost.

For the lock tree:

1. `RB_EMPTY_NODE(&waiter->tree_entry)` is false because the node value is
   `SW+0x00`, not `1` (`include/linux/rbtree.h:71-76`).
2. `rb_next()` sees no right child and decodes the parent as
   `(1 & ~3) == NULL`; it therefore returns NULL without chasing a kernel
   pointer (`lib/rbtree.c:518-547`).
3. `__rb_erase_augmented()` sets `*leftmost = NULL`, writes `root->rb_node =
   NULL`, and returns `rebalance = NULL` for a childless black root
   (`include/linux/rbtree_augmented.h:163-191`).
4. `RB_CLEAR_NODE()` then writes the node address into
   `waiter->tree_entry.__rb_parent_color`.
5. Reinsert starts from a NULL root. `rb_link_node()` writes the node's
   parent/color and two child fields, then stores the node as the root.
   `__rb_insert()` takes the `parent == NULL` root case, sets `*leftmost`,
   and colors the node black (`lib/rbtree.c:89-118`).

The PI-tree dequeue/enqueue is the same operation on `SW+0x18` and
`FT+0x8c0`/`FT+0x8c8`. Therefore the exact terminal-root state touches only
the stale waiter, the corresponding root field, and the corresponding
leftmost field. It does not traverse an unknown kernel parent or child.

This proof is void if either tree has a second node, a nonterminal parent, a
nonterminal child, or a leftmost pointer different from the actual sole node.

## Owner/wake check

`RT_MUTEX_HAS_WAITERS` is bit 0, and `rt_mutex_owner()` masks it off
(`kernel/locking/rtmutex_common.h:106-112`). With `fake_lock.owner =
fake_task | 1`, `rt_mutex_owner()` returns `fake_task`, not NULL.

The ownerless wake path is:

```c
if (!rt_mutex_owner(lock)) {
    if (prerequeue_top_waiter != rt_mutex_top_waiter(lock))
        wake_up_process(rt_mutex_top_waiter(lock)->task);
```

at `kernel/locking/rtmutex.c:698-706`. The non-NULL owner skips this block
entirely. The ELF branch at `0xffffff8008150624` also treats owner values
`0` and `1` as the ownerless path and follows the non-NULL owner path
otherwise.

One nuance: with the exact sole unchanged-top node, `owner == NULL` would not
necessarily call `wake_up_process()`, because the top waiter may be unchanged.
That is still outside the contract and becomes dangerous if the top changes or
the tree is inconsistent. The required non-NULL owner removes the branch
unconditionally.

## Fake-task early-return check

`rt_mutex_adjust_prio()` derives `pi_task` from
`task_top_pi_waiter(fake_task)->task` (`kernel/locking/rtmutex.c:337-350`).
With `SW.task == fake_task`, that yields `pi_task == fake_task`.

`rt_mutex_setprio(fake_task, fake_task)` computes:

```c
prio = pi_task ? min(pi_task->prio, p->normal_prio) : p->normal_prio;
```

With both fields equal to 120, `prio == 120`. It then returns before
`__task_rq_lock()` when:

```c
p->pi_top_task == pi_task && prio == p->prio && !dl_prio(prio)
```

(`kernel/sched/core.c:4704-4712`). The ELF path checks `FT+0x8d0`, `FT+0xac`,
and the priority sign/value before returning at `0xffffff8008102470`. No
runqueue lock, dequeue, enqueue, scheduler-class pointer, or class-change call
is reached on this path.

The correction is separate: the chain itself reads `fake_task->dl.deadline`
while updating the waiter at `kernel/locking/rtmutex.c:683`, and ELF shows the
`FT+0x420` read at `0xffffff8008150548`. Therefore the design must require
`FT+0x420` to be mapped and initialized. Zero is a safe value for this non-DL
walk.

## Crash and failure conditions

No crash condition is present under the exact contract. The following
violations are outside the PASS envelope:

| condition | effect |
|---|---|
| `waiter->lock == NULL` or invalid | NULL/bad spinlock dereference |
| `fake_lock.wait_lock != 0` | trylock failure and unbounded retry/spin |
| lock tree root or leftmost not `SW+0x00` | wrong top waiter or unknown rbtree access |
| lock-tree parent/child nonterminal | dereferences outside controlled objects |
| `fake_lock.owner` NULL or invalid | ownerless wake possible; bad owner reference |
| `waiter->task == NULL` or invalid | bad PI top task reaches `rt_mutex_setprio()` |
| PI-tree root or leftmost not `SW+0x18` | wrong PI waiter or unknown rbtree access |
| PI-tree parent/child nonterminal | dereferences outside controlled objects |
| `fake_task.pi_blocked_on != NULL` | `task_blocked_on_lock()` follows a second lock |
| `fake_task.pi_top_task != fake_task` | setprio misses early return and enters scheduler |
| `fake_task.prio != 120` or `normal_prio != 120` | setprio misses early return |
| `fake_task.pi_lock != 0` | spinlock is not initially unlocked |
| `fake_task.usage` can underflow | fake task destructor/free path |
| `FT+0x420` (`dl.deadline`) unmapped | chain crashes while writing `waiter->deadline` |
| concurrent mutation of `SW`, `FL`, or `FT` | invariant and rbtree shape lost |
| victim priority changes after the walk | later adjust may leave the parked no-touch state |

The `waiter->prio == victim->prio` case remains fail-closed: it returns before
dereferencing the fake lock, but it is not the bounded-touch probe contract.

## Final audit result

**PASS.** The design is valid only for the exact terminal-root shape,
`fake_lock.owner = fake_task | 1`, `fake_task.pi_blocked_on = NULL`,
`pi_top_task = fake_task`, `prio = normal_prio = 120`, and a readable,
initialized `fake_task+0x420`. Any multi-node rbtree, different owner, stale
PI pointer, or scheduler-field mismatch is a NO-GO.
