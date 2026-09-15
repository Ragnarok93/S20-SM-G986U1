# Sendmsg H1A-S2 implementation design

Date: 2026-09-16

Scope: offline implementation design only. No device access, build, deploy,
run, or exploit-code change is approved by this document. This design
implements only the sendmsg replacement for the stale-waiter shaping stage
and explicitly excludes CFI, fops mutation, pipe-physrw, root, and the full
chain.

## Verdict

H1A-S2 is GO as a design only if it corrects two implementation constraints
in the raw shape contract. First, the iov field layout cannot encode a black
childless root; the zero-length-safe iov values `{NULL, 0}` encode childless
red roots, which remain safe for this exact dequeue/enqueue sequence. Second,
userspace cannot know the stale waiter's kernel stack address, so the original
`fake_lock.waiters = SW` sentence in
`docs/specs/sendmsg-pi-shape-design.md` cannot be implemented directly.

The S2 adaptation is:

```text
victim.pi_blocked_on
  -> stale waiter SW          (sendmsg-shaped)
  -> fake_lock FL
  -> proxy waiter W0          (known page terminal node)
  -> fake_task FT
  -> stop
```

No pselect path is used. No second chain owner, CFI target, or pipe stage is
armed. The page must be prepared in a new S2 payload mode; the existing FOPS
payload values are not safe for this one-hop contract.

## Frame and object addresses

Use the audited native sendmsg frame:

```text
T        = native sendmsg syscall wrapper entry SP
R        = T-0x90                 sendmsg wrapper SP
C        = R-0x190 = T-0x220      ___sys_sendmsg SP
iovstack = C+0x38  = T-0x1e8      eight struct iovec entries
address  = C+0xb8  = T-0x168      sockaddr_storage
W        = T-0x198                stale rt_mutex_waiter base
```

The target coverage is exactly `W..W+0x4f`:

```text
W+0x00..W+0x2f = iovstack[5..7]
W+0x30..W+0x4f = address[0x00..0x1f]
```

Deeper TCP callees grow below `C`, so they do not rewrite the window while the
call blocks.

## msghdr layout

The victim thread calls `sendmsg(sender_fd, &mh, MSG_NOSIGNAL)`.

```c
struct msghdr mh = {
    .msg_name       = &name,
    .msg_namelen    = 32,
    .msg_iov        = iov,
    .msg_iovlen     = 8,
    .msg_control    = NULL,
    .msg_controllen = 0,
    .msg_flags      = 0,
};
```

`name` is only raw carrier storage; the connected TCP path must ignore it.
Do not set `MSG_FASTOPEN`, `MSG_DONTWAIT`, `MSG_OOB`, `MSG_MORE`, or
`MSG_CORK`. Keep the socket blocking.

The user iov array must be exactly:

| index | iov_base | iov_len | purpose |
|---:|---|---:|---|
| 0 | valid writable user pointer | `0x10000` | forces the blocking TCP send |
| 1 | valid writable user pointer | `1` | total length exceeds sndbuf |
| 2 | valid writable user pointer | `1` | total length exceeds sndbuf |
| 3 | valid writable user pointer | `1` | total length exceeds sndbuf |
| 4 | valid writable user pointer | `1` | total length exceeds sndbuf |
| 5 | `NULL` | `0` | `tree_entry` is a childless red root |
| 6 | `NULL` | `0` | `tree_entry` has NULL right/left children |
| 7 | `NULL` | `0` | `pi_tree_entry` is a childless red root |

More precisely, the stack values decode as:

| W offset | value | stale waiter field |
|---:|---:|---|
| `+0x00` | `0` | childless red root, parent NULL |
| `+0x08` | `0` | `tree_entry.rb_right = NULL` |
| `+0x10` | `0` | `tree_entry.rb_left = NULL` |
| `+0x18` | `0` | childless red PI root, parent NULL |
| `+0x20` | `0` | `pi_tree_entry.rb_right = NULL` |
| `+0x28` | `0` | `pi_tree_entry.rb_left = NULL` |
| `+0x30` | `FT` | `task` |
| `+0x38` | `FL` | `lock` |
| `+0x40` | `130` | `prio` |
| `+0x44` | `0` | padding |
| `+0x48` | `0` | `deadline` |

The first 32 bytes of `name` are copied into the stack `address` object and
must be initialized as follows:

```text
name[0x00..0x07] = FT
name[0x08..0x0f] = FL
name[0x10..0x13] = 0x00000082
name[0x14..0x1f] = 0
name[0x20..0x7f] = 0
```

Only the first 32 bytes are copied because `msg_namelen=32`; the remaining
`sockaddr_storage` stack bytes are irrelevant to the 0x50 window.

Zero-length entries 5..7 must use `NULL` bases. This relies on the audited
zero-length check in `rw_copy_check_uvector()`. It is also essential: any
nonzero `iov_base` would become a non-NULL rbtree child pointer.

## TCP blocking contract

Before creating the stale futex waiter:

1. Create one connected, established IPv4 TCP socket pair on loopback.
2. Set `TCP_NODELAY`; do not enable TCP fastopen.
3. Keep the sender blocking.
4. Temporarily set the sender nonblocking and fill the send path until `send`
   returns `EAGAIN`/`EWOULDBLOCK`.
5. Restore blocking mode.
6. Use `SIOCOUTQ` and require `outq >= sndbuf/2 - 4096` and `outq > 0`.
7. Do not read from the peer after this gate until the punch has returned.

The sendmsg total length is `0x10005`; with no readable peer and a full send
path it must block in the normal TCP send path. The receiver must not be
shutdown. The sender fd and peer fd must stay open until the round completes.

No socket operation is allowed on the victim thread between the timed
`FUTEX_WAIT_REQUEUE_PI` return and the `sendmsg` call. In particular, do not
log there, because those syscalls can overwrite the stack window before
sendmsg copies the carrier bytes.

## Page layout

`payload_base = page_base + SKB_DATA_DELTA`; on alioth, `SKB_DATA_DELTA=0`.
Every repeated `ORDER3_SIZE` chunk in the SKB payload must use the same S2
contract.

| object | offset from payload base | role |
|---|---:|---|
| `FL` | `0x1350` | fake rt_mutex |
| `W0` | `0x2220` | page-resident proxy waiter |
| `FT` | `0x3200` | passive fake task |
| fops table | `0x1000` | must be zeroed/not armed for S2 |
| scratch | `0x3000` | optional marker only |

### Fake lock FL

```text
FL+0x00 = 0
FL+0x04 = 0
FL+0x08 = W0+0x00
FL+0x10 = W0+0x00
FL+0x18 = FT | 1
```

`wait_lock` must be zero. The known W0 root prevents an empty-tree edge case
without requiring the unavailable SW address.

### Proxy waiter W0

```text
W0+0x00 = 1
W0+0x08 = 0
W0+0x10 = 0
W0+0x18 = 1
W0+0x20 = 0
W0+0x28 = 0
W0+0x30 = FT
W0+0x38 = FL
W0+0x40 = 140
W0+0x44 = 0
W0+0x48 = 0
W0+0x50 = 0
W0+0x58 = 0
```

W0 has a lower effective priority than SW (`140 > 130`), so the sendmsg-shaped
SW becomes the lock's top waiter after requeue. W0's two rb entries start as
terminal black roots.

### Fake task FT

Use the audited alioth task offsets:

```text
FT+0x068 = 0x100
FT+0x0ac = 120
FT+0x0b4 = 120
FT+0x8b4 = 0
FT+0x8c0 = W0+0x18
FT+0x8c8 = W0+0x18
FT+0x8d0 = FT
FT+0x8d8 = 0
```

The fake task is passive: it is not a schedulable task and has no real
scheduler object. `pi_top_task == pi_task`, equal prio/normal_prio, and
`pi_blocked_on == 0` make `rt_mutex_setprio()` return before scheduler code.

## Bounded walk

The expected sequence is:

1. `rt_mutex_adjust_pi(victim)` sees stale `SW` with prio 130 and victim prio
   121, so it enters the chain walk.
2. `SW->lock == FL`; `FL->wait_lock == 0` and trylock succeeds.
3. `FL->owner == FT`, not the victim, so deadlock detection does not fire.
4. The lock waiter requeue erases SW's childless red node, clears FL's W0
   root, and inserts SW as the sole terminal black root.
5. `FL->owner` remains non-NULL, so the ownerless `wake_up_process()` path is
   unreachable.
6. SW is the new top lock waiter. The proxy W0 is dequeued from FT's PI tree
   and SW is enqueued there.
7. `rt_mutex_adjust_prio(FT)` reaches `rt_mutex_setprio(FT, FT)` and returns
   early because `pi_top_task == FT`, `prio == normal_prio == 120`, and the
   priority is not deadline priority.
8. `FT->pi_blocked_on == 0`, so the chain ends.

This uses one controlled page node plus SW. It does not traverse an unknown
rbtree parent or child. The W0 adaptation is required because SW is not known
to userspace; a direct `fake_lock.waiters = SW` implementation is NO-GO.

## Thread and synchronization model

Use four execution contexts:

1. **Victim/waiter thread.** Pinned to core 0. Establishes the timed futex
   requeue stale waiter, then immediately calls blocking sendmsg.
2. **Owner thread.** Owns `f_pi_target`, then blocks on `f_pi_chain` held by
   the victim to produce the `-EDEADLK` requeue gate.
3. **Consumer thread.** Pinned to core 1. Waits for `s2_go`, delays, then
   issues exactly one `sched_setattr_tid(victim_tid, 1)`.
4. **Main thread.** Issues the requeue, observes completion, later unblocks
   sendmsg with one peer read, and collects the verdict.

Required atomics:

```text
s2_waiter_ready       s2_waiter_waiting      s2_owner_started
s2_owner_chain_done   s2_owner_stop          s2_route_done
s2_waiter_tid         s2_go                  s2_sendmsg_armed
s2_punch_allowed      s2_punch_calls         s2_punch_success
s2_punch_inflight     s2_sendmsg_ret         s2_sendmsg_errno
```

Ordering:

1. Prepare the S2 page and TCP gate before creating the stale waiter.
2. Start the watchdog after page preparation but before the futex gate.
3. Start owner and consumer. Wait until `s2_waiter_waiting` and
   `s2_owner_started` are both set.
4. Set `s2_go=1`, then issue `FUTEX_CMP_REQUEUE_PI`.
5. The victim waits at least 150 ms after observing `s2_go` before setting
   `s2_sendmsg_armed=1` and calling sendmsg.
6. The consumer delays 150 ms after `s2_go`, then aborts unless
   `s2_sendmsg_armed == 1` and `s2_punch_allowed == 1`.
7. The consumer increments `s2_punch_calls`, records `inflight=1`, calls
   `sched_setattr_tid`, logs ret/errno, then releases `inflight=0`.
8. Only after the consumer finishes may the main thread read one byte from
   the peer to unblock sendmsg.
9. The victim records sendmsg ret/errno, sets `s2_route_done=1`, and only on
   a successful bounded walk unlocks its PI-chain futex.

There must be exactly one consumer sched_setattr. No retry is permitted. Do
not change the victim's nice value again after the punch.

## Stale-waiter gate

The S2 round is armed only when all of these hold:

```text
FUTEX_CMP_REQUEUE_PI ret == -1
FUTEX_CMP_REQUEUE_PI errno == EDEADLK (35)
victim FUTEX_WAIT_REQUEUE_PI ret == -1
victim FUTEX_WAIT_REQUEUE_PI errno == ETIMEDOUT (110)
```

This reuses the already audited H1A-D stale-waiter precondition. It does not
reuse pselect. If either gate fails, set `s2_punch_allowed=0` and take the
normal fail path without a punch.

## Log markers

The implementation must emit these stable markers:

```text
H1A-S2 header route=sendmsg proxy-root=1 cfi=disabled
H1A-S2 page prepared base=<hex> lock=<hex> proxy=<hex> task=<hex>
H1A-S2 tcp sndbuf=<u32> effective=<u32> outq=<u32> gate=PASS
H1A-S2 gate requeue ret=<d> errno=<d> victim ret=<d> errno=<d>
H1A-S2 sendmsg armed tid=<d> iov_total=<u32> delay_ms=<d>
H1A-S2 punch enter tid=<d> nice=1
H1A-S2 punch first ret=<d> errno=<d>
H1A-S2 punch done tid=<d> ret=<d> errno=<d>
H1A-S2 sendmsg returned ret=<d> errno=<d>
H1A-S2 readback proxy_tree_self=<d> proxy_pi_self=<d> ft_pi_root=<hex>
H1A-S2 verdict=<PASS|FAIL> reason=<token>
```

Post-walk page evidence:

- `FL->wait_lock == 0`.
- `FL->owner == FT | 1`.
- `FL->waiters.rb_root` points at `SW+0x00`, which can be recognized only as
  a plausible kernel-range stack pointer; userspace does not know its value.
- `W0.tree_entry` remains a black childless terminal node
  (`+0x00 == 1`, `+0x08 == 0`, `+0x10 == 0`); it was not linked as SW's
  parent and is not the node erased by the SW requeue.
- If SW became top waiter, `W0.pi_tree_entry` also becomes an
  `RB_CLEAR_NODE()` self shape.
- `FT->pi_waiters.rb_root` and leftmost point at `SW+0x18`.
- `FT->pi_blocked_on`, prio, normal_prio, and `pi_top_task` remain at their
  pre-walk values.

`ks_leak_diff_dump()` may be run after the punch. Its result is evidence only;
it must not lead to CFI.

## Fail-closed conditions

Abort without punch if any of the following occurs:

| condition | reason |
|---|---|
| page/base/FL/W0/FT unavailable | no controlled page |
| any repeated S2 chunk mismatches the static page contract | corrupted payload |
| fops table is nonzero in S2 mode | accidental CFI state |
| TCP setup, fill, restore, or `SIOCOUTQ` gate fails | sendmsg would not block |
| requeue gate is not `-EDEADLK` | no stale victim state |
| victim wait gate is not `ETIMEDOUT` | no stale victim state |
| sendmsg returns before the consumer delay checks `s2_sendmsg_armed` | wrong stack/window |
| consumer sees `s2_go` consumed, blocked flag unset, or punch forbidden | race/gate failure |
| punch calls != 1 | duplicate or missing priority punch |
| consumer sched_setattr ret != 0 | walk did not finish normally |
| watchdog fires | possible unbounded kernel loop |

On failure, do not retry, do not refresh the page into a second attempt, do
not install fops, and do not run the full chain. A watchdog kill cannot clean
an in-kernel spin; the operator must treat a timeout as requiring a reboot
and preserve the single log.

## Watchdog boundary

Use a dedicated S2 watchdog with:

```text
H1A_S2_WATCHDOG_TIMEOUT_MS = 45000
H1A_S2_UNBLOCK_AFTER_MS    = 1000
```

Start it after page preparation and before the futex gate. Relocate its parent
write fd above `KS_FD_PROTECT_MIN`. The watchdog has exactly two outcomes:
write the completion byte on normal completion, or after 45 s log
`H1A-S2 watchdog timeout parent=<pid> status=stuck` and kill the parent.

The 45 s limit covers page-free futex setup, the 150 ms consumer delay, the
one-second post-punch unblock window, and cleanup. It is a process safety
net, not a kernel recovery mechanism. If the PI walk spins in `wait_lock`
retry, killing userspace will not make the kernel task disappear.

## Static implementation gates

1. Do not use pselect6, select, poll, sendto, or any earlier H1A encoder.
2. Verify the sendmsg ELF offsets against the exact target kernel before code.
3. Add a separate S2 entry point gated by `KS_H1A_S2_TEST=1`; do not repurpose
   the existing H1A/H1A-D tests.
4. Assert the full `msghdr`, iov, name, page, and TCP gate contract before
   arming.
5. Assert the requeue and victim-timeout gates before allowing the punch.
6. Keep the punch one-shot and make every post-punch priority change a hard
   error.
7. Treat the W0 proxy-root and childless-red-root variants as separate S2
   audit items; they correct the original one-node terminal-root contract
   because SW is unknown and iov cannot encode a black childless root.
