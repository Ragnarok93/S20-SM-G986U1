# Data flow into the `+0x304` load

Status: static reconstruction from the verified stock image (`symbol-resolution.md`)
and the pinned tree. No device run, no build, no code change. Nothing here is a
runtime measurement; every value attributed to the device is unresolved.

The question this answers is narrow: **what can `x21` and `x8` be at
`ffffff8008146fa8`**, given the code path that reaches it.

## 1. The six arguments, as the prologue stores them

`rt_mutex_adjust_prio_chain(task, chwalk, orig_lock, next_lock, orig_waiter, top_task)`:

| argument | register | prologue instruction |
| --- | --- | --- |
| `task` | `x19` | `mov x19, x0` at `+0x2c` |
| `chwalk` | `w24` | `mov w24, w1` at `+0x28` |
| `orig_lock` | `x23` | `mov x23, x2` at `+0x44` |
| **`next_lock`** | **`x21`** | **`mov x21, x3` at `+0x40`** |
| `orig_waiter` | `x22`, `[x29-8]` | `mov x22, x4` at `+0x50`, `stur x4, [x29, #-8]` at `+0x5c` |
| `top_task` | `[sp, #16]` | `str x5, [sp, #16]` at `+0x58` |

So the lock whose tree root is read at the fault enters the function as
**`next_lock`** in `x21`, which `rt_mutex_adjust_pi()` computed one frame up as
`waiter->lock` (`kernel/locking/rtmutex.c:1139`, `next_lock = waiter->lock;`, with
`waiter = task->pi_blocked_on`).

## 2. The loop head re-validates `x21` against a task's blocked-on waiter

```
+0x64  add x26, x19, #0x8c8      ; &task->pi_lock
+0x68  mov x0, x26
+0x6c  bl  _raw_spin_lock_irq
+0x70  ldr x25, [x19, #2296]     ; waiter = task->pi_blocked_on      (:507, :518)
+0x74  cbz x25, out_unlock_pi    ; if (!waiter) return
+0x8c  ldr x8, [x25, #56]        ; waiter->lock
+0x90  cmp x21, x8
+0x94  b.ne out_unlock_pi        ; :537  if (next_lock != waiter->lock) bail -EDEADLK
```

This is the important structural fact: **before any tree access, the kernel proves
`x21 == task->pi_blocked_on->lock`.** `x21` is therefore never an arbitrary pointer.
It is a lock named by the blocked-on waiter of whichever task is at the head of the
chain, re-checked on every iteration. There are only two ways it is assigned:

| iteration | assignment | source |
| --- | --- | --- |
| first | the `next_lock` argument | `:1139`, from `rt_mutex_adjust_pi()` |
| later | `mov x21, x26` at `+0x660`, where `x26 = owner->pi_blocked_on->lock` (`+0x26c`) | `:759`, `next_lock = task_blocked_on_lock(task)` |

## 3. The path from the trylock to the load

```
+0x1a8 mov x0, x21
+0x1ac bl  _raw_spin_trylock     ; :585   reads *(u32 *)x21
       …  deadlock checks …
+0x1f8 tbnz w28, #0, +0x2f8      ; :613   the !requeue guard is not taken
+0x2f8 ldr x8, [x21, #24]        ; :661   leftmost = rb_first_cached(&lock->waiters)
+0x304 ldr x8, [x8, #56]         ;        w->lock        ← the recorded fault
```

`w28` is `requeue`, set to 1 at `+0x48` and only cleared by the deadlock-detection
comparison; the `tbnz` is why this copy of the inline is the prerequeue one at
`:661`, not the only other inlined copy in the function.

Two reads of `x21` precede the fault — the trylock at `+0x1ac` and the tree root at
`+0x2f8` — and both must have completed, because the recorded PC is the instruction
after them.

## 4. What the tree root holds, by iteration

`rt_mutex_dequeue` (`+0x32c`, `rb_erase_cached`) and `rt_mutex_enqueue` (`+0x3ec`)
re-insert the same waiter on every iteration, and the insert updates
`rb_leftmost`. So the word at `lock+0x18` has two different origins:

| iteration | word at `x21+0x18` | origin |
| --- | --- | --- |
| 1 | the planted value | the program wrote it; `:661` runs *before* the erase at `+0x318` |
| ≥2 | the node the previous iteration re-inserted | written by the kernel's own `rb_insert_color_cached`, so a real kernel pointer |

Under the planted graph the second case is benign twice over: the node is the stale
waiter (a kernel object) in iteration 2, and `fake_w0` (inside the page) in the
iteration after that.

## 5. The plant, at the two offsets the load touches

`src/util.c:1876-1878` and `:1911`:

```c
put64(p, LOCK_OFF + RT_MUTEX_WAITERS_OFF, fake_w0);
put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);   /* +0x18 */
put64(p, LOCK_OFF + RT_MUTEX_OWNER_OFF, fake_task | 1);
…
put64(p, W0_OFF + 0x38, fake_lock);                    /* the field this load reads */
```

with `fake_lock = payload_base + LOCK_OFF` and `fake_w0 = payload_base + W0_OFF`
(`:1783-1784`). With this target's constants — `LOCK_OFF 0x1350`,
`W0_OFF 0x2220`, `RT_MUTEX_LEFTMOST_OFF 0x18`, `WAITER_LOCK_OFF 0x38`
(`src/targets/samsung-g986u1-4.19.113/target.h`) — the load depends on two separate words at two
separate addresses:

| # | word | address (`payload_base +`) | must hold |
| --- | --- | --- | --- |
| 1 | `lock->waiters.rb_leftmost` | `0x1350 + 0x18` = `0x1368` | `fake_w0` = `payload_base + 0x2220` |
| 2 | `fake_w0->lock` | `0x2220 + 0x38` = `0x2258` | `fake_lock` = `payload_base + 0x1350` |

The registers then read:

| register | intended value | distance from `fake_lock` |
| --- | --- | --- |
| `x21` | `fake_lock` | 0 |
| `x8` | `fake_w0` | `+0xed0` |
| effective address | `fake_w0 + 0x38` = `payload_base + 0x2258` | `+0xf08` |

`0x2220 - 0x1350 = 0xed0`, and the effective address is `0x2258 - 0x1350 =
0xf08`. Both field offsets are also the immediates in this window (`#24` and
`#56`), so the arithmetic is checked against the image as well as the header.
All four addresses lie inside the same 32 KiB page, which is a property of the
*intended* addresses — not a statement about the values the registers actually
held, and not a claim that either word still held its planted value at the fault.
`verify_symbol.py` recomputes all four addresses and both stored values from the
`target.h` constants.

## 6. What the frame excludes

- **The fault is not the `BUG_ON`.** Its trap is `brk #0x800` at `+0x838`; the PC is
  the load four instructions earlier.
- **`x21` was readable.** The trylock at `+0x1ac` read its first word and the
  `+0x2f8` load read `x21+0x18`; both completed. So a translation or permission
  fault on `x21` itself is excluded, and if `x21` is `fake_lock` then the planted
  page was mapped at that instant.
- **The fault is therefore about the value at `x21+0x18`, not about the lock's own
  mapping.** Whatever `x8` held named an address that was not readable at `+0x38`.

## 7. Families that survive the exclusion

| family | claim | what it predicts for a record we do not have |
| --- | --- | --- |
| F1a | the page kept its `0x41` fill at `+0x18` (`util.c:1831-1833` describes that fill) while `+0x00` was zeroed | `x8 = 0x4141414141414141`, non-canonical; `FAR = 0x4141414141414179` |
| F1b | the page was reused, so `+0x18` held another owner's data | `x8` inside a mapped region or not, depending on the new owner |
| F1c | `+0x18` held a user-range pointer (`bit 63` clear) | a permission fault under PAN, not a translation fault; `ESR`/`FAR` would separate it |
| F2 | `x21` was not `fake_lock` | the lock came from another task's `pi_blocked_on` that passed the `+0x90` check; its tree root was corrupt |

F1a and F1b are the same statement about *page state*; F1c is a statement about the
*value's range*; F2 is a statement about *which lock* the walk was on. They are not
exclusive: F2 plus F1b is possible.

Nothing in the disassembly prefers one. The only thing that can choose is a runtime
value — `FAR`, `ESR`, or one register — and none was preserved.

## 8. What the verified path does *not* say

- It does not say the planted page was reached, owned by the program, or intact.
- It does not say `x21` was `fake_lock`; it says `x21` was *a lock named by some
  task's `pi_blocked_on`* at that moment.
- It does not date the panic relative to the plant, the spray, or the futex return.
- It does not identify a faulting instruction in the caller: the caller's frame is
  not in the record.

## 9. What would narrow it, without changing the route

Ordered by cost, and none of them proposed as the next run:

1. A crash record with `FAR`/`ESR`. `FAR` is exactly `x8 + 0x38`, so it names the
   family outright: non-canonical (F1a), a mapped kernel address (F1b), a user-range
   address (F1c).
2. The probe's own readback (`util.c:4729-4733`) already compares
   `*(fake_lock+0x18)` with `fake_w0`, but only in page-probe mode. A punch run that
   reported the same word after the spray would date the change.
3. `KS_W1_ONLY_FIRST=1` (`util.c:1932-1939`) sets `fake_task->pi_blocked_on = 0`, so
   the chain exits at `:759` and never reaches a second iteration. Iteration 1 still
   reads the planted word at `:661`, so **a loss with that switch leaves the
   first-iteration word as the only candidate**, and survival would place the fault
   in the second-iteration path. Its information value depends on a run, so it is
   recorded here and not recommended.

## 10. Where this sits in the record

| claim | status |
| --- | --- |
| `x21` is the `next_lock` argument on entry | **verified from the image** |
| every iteration re-checks `x21 == pi_blocked_on->lock` | **verified from the image** |
| the load reads `*(x21+0x18) + 0x38` | **verified from the image** |
| the intended graph keeps both operands in one page | **derived from source** |
| which family produced the panic | **unresolved** |
| runtime `x21`, `x8`, `FAR`, `ESR`, backtrace | **unavailable** |
