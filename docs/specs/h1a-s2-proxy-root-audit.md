# H1A-S2 proxy-root audit

Date: 2026-09-16

Scope: offline static audit only. No device access, build, run, or exploit-code
change. The target is the W0 proxy waiter and childless red root correction in
`docs/specs/sendmsg-h1a-s2-implementation-design.md`.

## Verdict

**PASS** for the exact S2 sequence as an offline contract, subject to two
explicit implementation gates:

1. The S2 page contract must explicitly initialize `FT+0x420`
   (`task_struct.dl.deadline`) to a readable value. Zero is sufficient on this
   non-DL path. The current S2 document inherits this from the terminal-root
   audit but does not list the offset in its fake-task field table.
2. The S2 implementation must preserve the exact lock-tree operation order. The
   walk does not literally erase W0 from `FL->waiters`; erasing the detached
   SW red root makes `FL->waiters.rb_root` NULL and orphans W0. This is safe
   only because `FL->wait_lock` is held and no tree read occurs between that
   erase and the SW enqueue.

Evidence inputs:

```text
source materials/kernelsrc/sm8250-a13-stable
ELF    k40rom/boot/kernel.elf
prior  docs/specs/sendmsg-pi-shape-adversarial-audit.md
design docs/specs/sendmsg-h1a-s2-implementation-design.md
```

## Initial state

The exact contract is:

```text
SW.tree_entry      = {0, 0, 0}
SW.pi_tree_entry   = {0, 0, 0}
SW.task            = FT
SW.lock            = FL
SW.prio            = 130

FL.wait_lock       = 0
FL.waiters.root    = W0+0x00
FL.waiters.leftmost = W0+0x00
FL.owner           = FT | 1

W0.tree_entry      = {1, 0, 0}      black terminal root
W0.pi_tree_entry   = {1, 0, 0}      black terminal root
W0.task            = FT
W0.lock            = FL
W0.prio            = 140

FT.pi_lock         = 0
FT.pi_waiters.root = W0+0x18
FT.pi_waiters.leftmost = W0+0x18
FT.pi_blocked_on   = NULL
FT.pi_top_task     = FT
FT.prio            = 120
FT.normal_prio     = 120
FT.usage           = positive (0x100)
FT+0x420           = mapped and initialized; zero is sufficient
```

The victim's `pi_blocked_on` still points to the sendmsg-shaped SW, whose
priority 130 differs from the victim's priority 121. Therefore
`rt_mutex_adjust_pi(victim)` enters `rt_mutex_adjust_prio_chain()` with
`orig_lock = orig_waiter = NULL`.

## 1. FL initial waiters=W0 and the W0-to-SW transition

`rt_mutex_adjust_prio_chain()` first calls
`rt_mutex_dequeue(lock, waiter)` with `waiter = SW`
(`kernel/locking/rtmutex.c:664`). Because SW is not the node actually stored in
`FL->waiters`, this is not an ordinary W0 dequeue:

- `RB_EMPTY_NODE(&SW->tree_entry)` is false: SW's `__rb_parent_color` is 0,
  while `RB_EMPTY_NODE()` requires it to equal the SW node address.
- `SW->tree_entry` has parent NULL and null children, so it is a detached
  childless red node.
- `__rb_erase_augmented()` sees parent NULL and no child and writes NULL to
  `FL->waiters.rb_root`.
- It does not call `rb_next()` because SW is not the cached leftmost; the
  cached leftmost is still W0 at that instant.
- W0 remains a terminal black node, but it is no longer referenced by the FL
  root. It is an orphan, not a live lock waiter.

`rt_mutex_enqueue(lock, SW)` then sees a NULL root, links SW as the root, and
colors it black. The insert also overwrites the stale W0 leftmost with SW. The
4.19 code has no read of `FL->waiters` between the erase and enqueue. Holding
`FL->wait_lock` prevents another PI walk from observing the temporary
`root=NULL, leftmost=W0` state. Therefore this sequence is bounded and safe for
the exact code order.

This is a narrow result. It does not make a detached red root generally safe in
an occupied rbtree. If any future code reads the tree between the erase and
insert, performs another dequeue, calls `rb_next()`/`rt_mutex_top_waiter()`, or
adds a second node, the orphaned W0/root state can become a crash or
inconsistent tree.

## 2. SW's zeroed rb fields

All three words of both SW rb entries may be zero in this exact sequence.

For `SW.tree_entry`, zero is a detached childless red node, not an
`RB_CLEAR_NODE()` self-pointer. `RB_EMPTY_NODE()` is false because a kernel
stack address is nonzero. The detached-node erase is safe only under the
occupied W0-root caveat above.

For `SW.pi_tree_entry`, zero remains untouched until after W0 has been dequeued
from FT's PI tree. `rt_mutex_enqueue_pi()` then uses `rb_link_node()` to set
the parent/color and child fields, and `__rb_insert()` colors the new sole root
black. No traversal dereferences SW's zeroed fields before that.

Zeroed rb fields are not safe if SW is already an inserted node, if the target
root is empty, if an extra node exists, or if SW's address could be zero. Those
states are outside the S2 envelope.

## 3. W0 PI-tree dequeue and SW enqueue

The PI-tree transition is a normal terminal-root operation:

1. After SW becomes the sole lock-tree top,
   `rt_mutex_dequeue_pi(FT, W0)` is called (`rtmutex.c:723`).
2. W0 is a childless black root and is both FT's PI root and leftmost.
   `rb_next(W0)` returns NULL because there is no right child and its decoded
   parent is NULL.
3. `__rb_erase_augmented()` sets FT's PI root and leftmost to NULL. A black
   root with a NULL parent produces no rebalance.
4. `RB_CLEAR_NODE()` changes `W0.pi_tree_entry` to the expected self shape.
5. `rt_mutex_enqueue_pi(FT, SW)` links SW as the sole black root and sets both
   FT PI root and leftmost to `SW+0x18`.

Thus W0 and SW do not collide: W0 is cleared before SW is linked, and no parent
or child pointer reaches outside `W0`, `SW`, `FL`, and `FT`.

`rt_mutex_adjust_prio(FT)` then derives `pi_task` from
`task_top_pi_waiter(FT)->task == FT`. With `FT.pi_blocked_on == NULL`, the
chain reaches `out_put_task` after `rt_mutex_setprio()` returns. The fake-task
early-return contract remains `pi_top_task == FT`, `prio == normal_prio == 120`,
and a non-DL priority.

## 4. FT+0x420 deadline contract

The chain writes `waiter->deadline = task->dl.deadline` before the fake-task
`rt_mutex_setprio()` early return (`rtmutex.c:683`). ELF evidence in the prior
terminal-root audit shows the `FT+0x420` read at `0xffffff8008150548`.

The S2 implementation design states that the early return avoids scheduler
code, but it omits `FT+0x420` from the explicit fake-task field table. The
prior audit and status already require the offset, and the existing order-3
page covers `payload_base+0x3620`. This audit therefore makes the inherited
condition mandatory: the new S2 payload mode must explicitly leave or set
`FT+0x420` to a defined value; zero is sufficient because priority 120 is not
deadline priority.

## 5. Compatibility with the terminal-root audit

The proxy-root adaptation is compatible with the terminal-root audit only after
refining its scope:

- The original audit's SW-as-terminal-black-root proof applies directly to the
  final PI-tree state and to the final FL lock-tree state after SW enqueue.
- The initial FL state is now W0-as-terminal-black-root, while SW is a detached
  childless red root. The FL transition relies on the detached-node erase
  side effect documented above, not on a conventional W0 erase.
- W0 is used as the initial top waiter and as the initial FT PI-tree root. It
  is not a second live chain node and must never be reached as a second
  `pi_blocked_on` lock.
- `FT.pi_blocked_on = NULL` remains essential. Setting it to W0 would create a
  second PI iteration and void this one-hop audit.

The prior audit's owner/wake suppression, fake-task early return, refcount,
and `FT+0x420` conclusions carry over unchanged.

## Crash, spin, and fail-closed conditions

The PASS applies only when none of the following occurs:

| Condition | Effect |
|---|---|
| SW, FL, W0, or FT address/page unavailable or unmapped | fault or failed gate |
| SW.tree_entry or SW.pi_tree_entry has any nonzero child/parent | unknown rbtree dereference |
| SW.task != FT or SW.task invalid | bad owner/top-task dereference |
| SW.lock != FL or SW.lock invalid | bad spinlock dereference |
| SW.prio equals victim's current prio before punch | adjust_pi exits without controlled walk |
| victim priority changes after the walk | later adjust can leave the parked no-touch state |
| FL.wait_lock != 0 or becomes held | trylock fails and `cpu_relax(); goto retry` can spin forever |
| FL.waiters.root/leftmost not W0, or W0.tree_entry nonterminal | unknown rbtree dereference |
| FL.owner not FT with HAS_WAITERS set, is NULL, or is another bad pointer | ownerless wake, bad task ref, or deadlock path |
| FL.owner == victim task | `-EDEADLK` path instead of intended bounded walk |
| W0.pi_tree_entry nonterminal, or FT PI root/leftmost not W0 | unknown rbtree dereference |
| FT.pi_lock != 0 | fake spinlock contention can spin |
| FT.pi_waiters mutated concurrently | lost W0 erase or corrupted SW insert |
| FT.pi_blocked_on != NULL | second chain iteration; possible fault/spin outside one-hop proof |
| FT.pi_top_task != FT, prio != 120, or normal_prio != 120 | setprio misses early return and enters scheduler |
| FT+0x420 unmapped | fault while writing `SW->deadline` |
| FT.usage zero or not positive | task refcount underflow / destructor path |
| any concurrent mutation of SW, FL, W0, or FT | rbtree and PI invariants lost |
| more than one consumer punch, or any later priority change | re-enters or invalidates the no-touch cleanup state |

A userspace watchdog can kill the process but cannot recover an in-kernel
spin. A watchdog timeout remains reboot-only, as required by the S2 design.

## Result

The W0 proxy root and childless red root correction are sound for the exact
bounded walk. The audit does not approve device access, CFI, fops mutation,
pipe-physrw, root, a full chain, or an implementation built without the
explicit `FT+0x420` gate.
