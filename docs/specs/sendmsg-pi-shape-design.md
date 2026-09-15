# Sendmsg PI-shape design: bounded rt_waiter, fake lock, fake task

Date: 2026-09-16

Scope: offline design only. This document does not implement, build, deploy,
or schedule an exploit. It designs the downstream contract for the raw
0x50-byte placement accepted in
`docs/specs/kernel-frame-placement-enumeration.md`.

## Verdict

Native blocking TCP `sendmsg` can encode a bounded one-hop PI shape:

```text
victim.pi_blocked_on
  -> stale rt_waiter
  -> fake_lock
  -> fake_task
  -> stop
```

The walk may touch `fake_lock.wait_lock`, the lock waiter tree, and the fake
task PI tree exactly once, then return from `sched_setattr` without entering a
longer owner chain, CFI, or pipe/root stages. This is a fake-task PI walk only,
not a claim that the full chain is safe.

The key design is a terminal-root rbtree state: the stale waiter is the sole
node in both the fake lock's waiter tree and the fake task's PI waiter tree.
Therefore the walk's dequeue/requeue operations do not traverse unknown kernel
parents or children. The fake task is configured so `rt_mutex_setprio()`
early-returns. `fake_task.pi_blocked_on` must be `NULL`, so the walk ends after
the first lock.

## Placement and assumptions

Use the placement contract verified in
`docs/specs/kernel-frame-placement-enumeration.md`:

```text
T  = native sendmsg syscall wrapper entry SP
W  = T-0x198 .. T-0x148     stale rt_mutex_waiter window
C  = T-0x220                ___sys_sendmsg SP
iovstack = C+0x38 = T-0x1e8
address  = C+0xb8 = T-0x168
```

The target is a connected, established TCP socket in the normal send path.
Use `msg_iovlen=8`, `msg_controllen=0`, and `msg_namelen=32`. The first five
iovec entries carry enough payload to block in TCP; entries 5..7 have zero
length and encode the waiter rb nodes. TCP must not use fastopen or deferred
connect, because those paths may consume `msg_name`.

The target kernel has:

```text
CONFIG_RT_MUTEXES=y
CONFIG_DEBUG_RT_MUTEXES=n
CONFIG_DEBUG_LOCK_ALLOC=n
CONFIG_DEBUG_SPINLOCK=n
```

Thus the 4.19 layouts are:

```text
struct rt_mutex_waiter:
  +0x00 tree_entry.__rb_parent_color
  +0x08 tree_entry.rb_right
  +0x10 tree_entry.rb_left
  +0x18 pi_tree_entry.__rb_parent_color
  +0x20 pi_tree_entry.rb_right
  +0x28 pi_tree_entry.rb_left
  +0x30 task
  +0x38 lock
  +0x40 prio                     int
  +0x48 deadline                 u64

struct rt_mutex:
  +0x00 wait_lock                qspinlock, 4 bytes
  +0x08 waiters.rb_root.rb_node
  +0x10 waiters.rb_leftmost
  +0x18 owner                    task | RT_MUTEX_HAS_WAITERS

alioth fake task PI offsets:
  +0x068 usage
  +0x0ac prio
  +0x0b4 normal_prio
  +0x8b4 pi_lock
  +0x8c0 pi_waiters.rb_root.rb_node
  +0x8c8 pi_waiters.rb_leftmost
  +0x8d0 pi_top_task
  +0x8d8 pi_blocked_on
```

## Byte contract

The sendmsg stack window maps as follows. `SW` is the stale waiter base and
`FT`/`FL` are `fake_task`/`fake_lock` addresses in the controlled page.

| W offset | sendmsg object | value | rt_waiter field |
|---:|---|---|---|
| `+0x00` | `iov[5].iov_base` | `1` | `tree_entry.__rb_parent_color` |
| `+0x08` | `iov[5].iov_len` | `0` | `tree_entry.rb_right` |
| `+0x10` | `iov[6].iov_base` | `1` | `tree_entry.rb_left` |
| `+0x18` | `iov[6].iov_len` | `0` | `pi_tree_entry.__rb_parent_color` |
| `+0x20` | `iov[7].iov_base` | `1` | `pi_tree_entry.rb_right` |
| `+0x28` | `iov[7].iov_len` | `0` | `pi_tree_entry.rb_left` |
| `+0x30` | `address[0x00..0x07]` | `FT` | `task` |
| `+0x38` | `address[0x08..0x0f]` | `FL` | `lock` |
| `+0x40` | `address[0x10..0x13]` | `0x00000082` | `prio = 130` |
| `+0x44` | `address[0x14..0x17]` | `0` | padding |
| `+0x48` | `address[0x18..0x1f]` | `0` | `deadline` |

The prio value is deliberately larger than the victim's `nice=1` scheduling
prio (`121`) so `rt_mutex_adjust_pi()` does not take the no-touch equal-priority
exit. `prio=130` is not a deadline priority, so the initial equality check does
not dereference deadline state.

## Fake lock contract

`FL` is `payload_base + LOCK_OFF` on the existing alioth controlled page.
Before the PI punch it must contain exactly:

| `FL` offset | value | meaning |
|---:|---|---|
| `+0x00..+0x03` | `0` | unlocked qspinlock |
| `+0x04..+0x07` | `0` | padding |
| `+0x08` | `SW + 0x00` | waiter tree root |
| `+0x10` | `SW + 0x00` | cached leftmost |
| `+0x18` | `FT\|1` | owner, with `RT_MUTEX_HAS_WAITERS` |

`SW + 0x00` is `tree_entry`. The single node is a black root with no parent and
no children. Because `fake_lock.owner` is non-`NULL`, the walk does not enter
the ownerless wake path:

```c
wake_up_process(rt_mutex_top_waiter(lock)->task);
```

The trylock failure path is forbidden by contract. `wait_lock` must be zero and
no other CPU may write it. A nonzero value would reach the unbounded
`cpu_relax(); goto retry;` loop in `rt_mutex_adjust_prio_chain()`.

## Fake task contract

`FT` is `payload_base + FAKE_TASK_OFF`. It is a passive sentinel task, not a
real schedulable task object. The following fields are required:

| `FT` offset | value | reason |
|---:|---|---|
| `+0x068` | `0x00000100` | balanced `get_task_struct()`/`put_task_struct()` |
| `+0x0ac` | `120` | fake task prio |
| `+0x0b4` | `120` | fake task normal prio |
| `+0x8b4` | `0` | unlocked `pi_lock` |
| `+0x8c0` | `SW + 0x18` | PI waiter tree root |
| `+0x8c8` | `SW + 0x18` | PI waiter cached leftmost |
| `+0x8d0` | `FT` | enables `rt_mutex_setprio()` early return |
| `+0x8d8` | `0` | terminates the owner chain |

`SW + 0x18` is `pi_tree_entry`. The stale waiter's `task` must be `FT`, not
`NULL` and not a real victim pointer. `fake_task.pi_blocked_on` must be zero;
the older fake-W0 chain shape is out of scope and must not be used for this
bounded sendmsg shape.

`fake_task.dl.deadline` is not dereferenced on the selected non-DL early-return
path. If a page builder wants the fake task to be defensively self-consistent,
it may zero that field, but no specific alioth `task_struct.dl` offset is
required by this walk.

## rbtree contract

Both trees use the same terminal-root invariant:

```text
tree_entry:
  __rb_parent_color = 1       black root, no parent
  rb_right          = 0
  rb_left           = 0

pi_tree_entry:
  __rb_parent_color = 1       black root, no parent
  rb_right          = 0
  rb_left           = 0
```

`fake_lock.waiters` and `fake_task.pi_waiters` each point at their corresponding
stale node as both `rb_root.rb_node` and `rb_leftmost`. This is intentionally
different from `RB_CLEAR_NODE()`: `RB_EMPTY_NODE()` requires
`__rb_parent_color == node`, which is impossible if the only writer to those
fields is a user-range `iov_base`. The terminal root is still safe for the
dequeue/requeue sequence performed by this walk.

The walk performs these bounded rb operations:

1. `rt_mutex_dequeue(fake_lock, stale_waiter)` erases the sole terminal root.
2. It updates `waiter->prio` to the victim's current prio.
3. `rt_mutex_enqueue(fake_lock, stale_waiter)` inserts the sole terminal root
   back as a black root.
4. `rt_mutex_dequeue_pi(fake_task, stale_waiter)` erases the sole terminal PI
   root.
5. `rt_mutex_enqueue_pi(fake_task, stale_waiter)` inserts it back as a black
   root.

All rb writes remain in the stale waiter, `fake_lock`, and `fake_task`. A shape
that introduces a second node, a nonterminal parent, or a nonterminal child is
outside this contract.

## Bounded walk trace

`rt_mutex_adjust_pi(victim)` observes:

```text
victim.pi_blocked_on       = SW
waiter->prio               = 130
waiter->deadline           = 0
task_to_waiter(victim)     = prio 121, deadline 0
waiter->lock               = FL
```

Because the priorities differ, it enters `rt_mutex_adjust_prio_chain()` with
`orig_lock=NULL`, `orig_waiter=NULL`, `next_lock=FL`, and `top_task=victim`.

Inside the chain:

1. `waiter = victim.pi_blocked_on = SW`.
2. `next_lock == waiter->lock == FL`.
3. `orig_waiter` is null, so the early ownerless check is skipped.
4. `raw_spin_trylock(&fake_lock.wait_lock)` succeeds.
5. `lock != orig_lock` and `rt_mutex_owner(lock) == fake_task != top_task`,
   so deadlock detection does not fire.
6. The requeue path erases and reinserts the sole lock waiter.
7. `fake_lock.owner` is non-`NULL`, so no wake path runs.
8. `task` becomes `fake_task`; its PI tree is erased and reinserted.
9. `rt_mutex_adjust_prio(fake_task)` reaches `rt_mutex_setprio(fake_task,
   fake_task)`. Because `pi_top_task == pi_task`, `prio == normal_prio == 120`,
   and the priority is not deadline priority, it returns before touching a run
   queue or scheduler class.
10. `fake_task.pi_blocked_on == NULL`, so `next_lock == NULL`.
11. The chain releases the task reference and returns `0`.

Expected user-visible result: `sched_setattr()` returns `0` or another finite
scheduling result without hanging. It must not proceed to CFI or any later
chain stage in this design round.

## Wake path

The wake path is disabled by construction because `fake_lock.owner` is non-
`NULL`. In particular, the ownerless branch

```c
if (!rt_mutex_owner(lock)) {
    if (prerequeue_top_waiter != rt_mutex_top_waiter(lock))
        wake_up_process(rt_mutex_top_waiter(lock)->task);
```

must not be reached. This removes the historical `wake_up_process(NULL)` risk.
The fake owner path still calls `rt_mutex_adjust_prio()`, so the fake task must
satisfy the early-return invariant above. A fake task with a mismatched
`pi_top_task` or prio would enter real scheduler code and is not covered.

## Cleanup and parked state

After the bounded walk:

- `fake_lock.wait_lock` is unlocked.
- `fake_task.pi_blocked_on` remains `NULL`.
- `fake_task.usage` is balanced and still has headroom.
- `waiter->prio` has become the victim's current prio, nominally `121`.
- `waiter->deadline` remains zero.
- `victim.pi_blocked_on` still points at the stale stack waiter.

The final condition is the cleanup gate: as long as the victim's scheduling
priority is not changed, a later `rt_mutex_adjust_pi(victim)` sees
`waiter->prio == victim->prio` and returns before dereferencing the fake lock.
Therefore, after the punch, do not issue another nice/priority change against
the victim and do not enqueue it on another rtmutex. If the page is to be reused
for another round, re-establish the full fake lock/fake task contract from a
clean payload; do not assume the parked trees are reusable.

The controlled page may be read back, but this contract does not define CFI
mutation, fops installation, or post-walk root primitives.

## User-range `iov_base` analysis

**Yes: the terminal-root shape remains satisfiable when `iov_base` is restricted
to user address space.** The rb pointer fields in the stale waiter are the only
fields constrained by `access_ok()`. Set them as:

```text
iov[5].iov_base = 1
iov[5].iov_len  = 0
iov[6].iov_base = 1
iov[6].iov_len  = 0
iov[7].iov_base = 1
iov[7].iov_len  = 0
```

The value `1` is a user-range pointer that decodes as `parent=NULL, black`; the
zero lengths decode as null children. The `msg_name` bytes are copied directly
and are not `access_ok()`-validated, so they can carry `fake_task`,
`fake_lock`, prio, padding, and deadline.

This contract does **not** require a self-referential `RB_EMPTY_NODE()` value.
That would require writing the kernel stack address of the rb node through
`iov_base`, which a user-only `iov_base` cannot do. It also does not require a
nonterminal parent or child pointer, which would similarly require kernel-range
`iov_base` values and is rejected.

If a future design needs a multi-node rbtree, a kernel-stack self-pointer, or a
node outside the controlled page, user-range `iov_base` is insufficient. The
safe alternatives are:

1. Use the terminal-root shape above, which intentionally has one node per
   tree; or
2. use a different rewrite primitive that can write kernel pointers into the
   stale waiter's rb fields; or
3. fall back to the no-touch early exit by making `waiter->prio` exactly equal
   to the victim's post-`sched_setattr` prio, but that does not prove placement
   or touch `fake_lock`.

## Failure classes

These are hard NO-GO conditions:

| condition | result |
|---|---|
| `wait_lock != 0` | unbounded retry/spin |
| `fake_lock.owner == NULL` | ownerless wake path |
| stale `waiter.task == NULL` | possible `wake_up_process(NULL)` |
| `fake_task.pi_blocked_on != NULL` | follows a second lock/owner chain |
| `fake_task.pi_top_task`, `prio`, or `normal_prio` mismatch | enters scheduler internals |
| nonterminal rb parent/child | dereferences outside controlled objects |
| `waiter->lock == NULL` | immediate NULL spinlock dereference |
| `waiter->prio == victim->prio` | safe but no-touch early return |

The `waiter->prio == victim->prio` case is fail-closed, but it is a fallback,
not the bounded-touch probe contract.

## Static gates before any implementation or device round

1. Verify the sendmsg frame offsets against the exact kernel ELF.
2. Dump the eight iovec entries and the first 32 address bytes before the
   blocking call.
3. Assert `msg_iovlen==8`, `msg_namelen==32`, `msg_controllen==0`, and
   `iov[5..7].iov_len==0`.
4. Assert `fake_lock.wait_lock==0`, both cached roots equal the stale nodes,
   and `fake_lock.owner == fake_task|1`.
5. Assert `fake_task.pi_blocked_on==0`, `pi_top_task==fake_task`,
   `prio==normal_prio==120`, and `usage >= 2`.
6. Assert the victim remains a normal `nice=1` SCHED_OTHER task and receives no
   additional priority change during the round.
7. Do not arm CFI unless this exact shape has independently demonstrated a
   bounded walk result under an approved probe design.
