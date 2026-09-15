# Kernel frame placement enumeration for the dangling rt_waiter window

Date: 2026-09-16
Scope: offline static audit only. No device command, no kernel build, and no
exploit-code change was made. The evidence sources are:

```text
ELF    k40rom/boot/kernel.elf
sha256 F73A2163A0A8311ED44C731951E8E5A7A789B1B08E1931C920632A9E3098B662
source materials/kernelsrc/sm8250-a13-stable
config build/.config
```

## Executive verdict

**GO candidate: one.** Native blocking TCP `sendmsg` can place a continuous,
user-controlled 0x50-byte region exactly over the dangling `rt_waiter`
window. The useful object is not `address` alone; it is the tail of
`iovstack[8]` immediately followed by `address`.

All audited `futex/requeue/wait/timeout`, other network, `fs/poll/select`,
timer/nanosleep, and SysV msg entries are **NO-GO** or unavailable. In
particular, this report supersedes the blanket sendmsg-family NO-GO in
`docs/specs/sendmsg-frame-audit.md`: that audit omitted the adjacency between
`iovstack` and `address` and incorrectly recorded `iovstack` as 0x40 bytes.

## Constants and method

The native AArch64 syscall wrapper entry SP is `T`. On this build,
`pt_regs` is 0x110 bytes and `THREAD_SIZE` is 0x4000, so:

```text
T = task_stack_page + 0x4000 - 0x110
W = T-0x198 .. T-0x149        (half-open; 0x50 bytes)
```

The waiter field layout is:

```text
W+0x00 tree_entry.__rb_parent_color
W+0x08 tree_entry.rb_right
W+0x10 tree_entry.rb_left
W+0x18 pi_tree_entry.__rb_parent_color
W+0x20 pi_tree_entry.rb_right
W+0x28 pi_tree_entry.rb_left
W+0x30 task
W+0x38 lock
W+0x40 prio
W+0x48 deadline
```

"Raw" below means bytes copied from user into a stack object without semantic
normalization or later protocol rewriting. GO requires at least 0x50
contiguous such bytes across `W` while the syscall remains blocked.

## GO: native blocking TCP sendmsg

### ELF frame chain

The relevant symbols and prologues are:

```text
0xffffff80091f4268  sub  sp, sp, #0x90     __arm64_sys_sendmsg wrapper
0xffffff80091f42e0  mov  x2, sp            &msg_sys in wrapper frame
0xffffff80091f42f8  bl   ___sys_sendmsg
0xffffff80091f3f98  sub  sp, sp, #0x190    ___sys_sendmsg child
0xffffff80091f3fa0  add  x29, sp, #0x140
0xffffff80091f3fd0  add  x9, sp, #0x38     &iovstack[0]
0xffffff80091f3fd4  sub  x10, x29, #0x88   &address
0xffffff80091f4188  blr  x8                protocol sendmsg dispatch
```

Arithmetic:

```text
R       = T-0x90                       sendmsg wrapper SP
C       = R-0x190 = T-0x220            ___sys_sendmsg SP
iovstack = C+0x38 = T-0x1e8, size 0x80
address  = C+0xb8 = T-0x168, size 0x80
W        = C+0x88 .. C+0xd8
```

`iovstack` contributes `W+0x00..W+0x2f`; `address` contributes
`W+0x30..W+0x4f`. The two objects are adjacent, giving continuous raw
coverage of exactly 0x50 bytes.

### Source chain

`___sys_sendmsg()` declares both adjacent objects and points `msg_sys` at
`address` (`net/socket.c:2205-2226`). `copy_msghdr_from_user()` performs a
raw `move_addr_to_kernel()` copy into `msg_name` and calls `import_iovec()`
with the user iov array (`net/socket.c:2154-2202`). `move_addr_to_kernel()`
is a direct user copy capped at `sizeof(struct sockaddr_storage)` =
0x80 (`net/socket.c:200-209`; `include/uapi/linux/socket.h:8-20`).

`UIO_FASTIOV` is 8 (`include/uapi/linux/uio.h:27-28`), and each native
`struct iovec` is 0x10 bytes, so the stack iov array is 0x80 bytes.
`import_iovec()` uses that fast array when `msg_iovlen=8`
(`lib/iov_iter.c:1537-1554`). The copy loop is
`rw_copy_check_uvector()` (`fs/read_write.c:767-840`). Target iov lengths of
zero pass access checking without being dereferenced (`fs/read_write.c:816-830`).

For a connected TCP socket, `tcp_sendmsg_locked()` obtains the send timeout,
does not consume `msg_name` unless fastopen/deferred-connect is selected, and
waits in `sk_stream_wait_memory()` when write memory is exhausted
(`net/ipv4/tcp.c:1239-1248`; `net/ipv4/tcp.c:1422-1433`;
`net/core/stream.c:118-181`). Connected TCP ignores `msg_name` in the normal
connected-socket path (`net/ipv4/tcp.c:2184-2186`). Therefore the 32 raw
address bytes are not rewritten during the blocking wait.

### Placement contract

Use a connected, established TCP socket in the normal send path, not
fastopen. Set `msg_controllen=0`, set `msg_iovlen=8`, and set
`msg_namelen=32`. The first five iov entries can carry enough real TCP
payload to exhaust sndbuf; set `iov[5..7].iov_len=0` so those target bytes
remain stable.

The byte mapping is:

```text
window      object/value                waiter field
W+0x00      iov[5].iov_base             tree_entry.__rb_parent_color
W+0x08      iov[5].iov_len = 0          tree_entry.rb_right
W+0x10      iov[6].iov_base             tree_entry.rb_left
W+0x18      iov[6].iov_len = 0          pi_tree_entry.__rb_parent_color
W+0x20      iov[7].iov_base             pi_tree_entry.rb_right
W+0x28      iov[7].iov_len = 0          pi_tree_entry.rb_left
W+0x30      address[0x00..0x07]         task
W+0x38      address[0x08..0x0f]         lock
W+0x40      address[0x10..0x13]         prio
W+0x44      address[0x14..0x17]         padding
W+0x48      address[0x18..0x1f]         deadline
```

The iov `_base` values are user-controlled, but a nonzero base is constrained
by `access_ok()` because it is an iov pointer. With zero lengths, zero or
ordinary user-range bases are stable. The `address` bytes have no such
pointer-validation constraint and provide arbitrary control of `task`,
`lock`, `prio`, padding, and `deadline`.

At the blocking protocol call, SP is `C=T-0x220`. Deeper calls grow below
`C`; the window begins at `C+0x88`, so the blocking callees do not overwrite
the payload.

## Other audited candidates

### Network

| entry | ELF chain anchor | controlled object / verdict |
| --- | --- | --- |
| `sendmsg` | wrapper `0x91f4268`; child `0x91f3f98`; block `0x91f4188` | **GO**, `iovstack[5..7] + address[0..0x1f]` |
| `sendmmsg` | wrapper `0x91f4348`; common child `0x91f3f98`; block `0x91f4188` | NO-GO: continuous `iovstack+address` is at `T-0x2b8..T-0x1b8`, entirely below `W` |
| `sendto` | wrapper `0x91f36a8`; child `0x91f35a8`; protocol call `0x91f3638` | NO-GO: `address=C+8=T-0xd8`, above `W` |
| `recvfrom` | wrapper `0x91f37d8`; child `0x91f36d0`; protocol call `0x91f3768` | NO-GO: `address=C+8=T-0xd8`; also protocol-written, not raw |
| `connect` | wrapper `0x91f3580`; child `0x91f3460`; protocol call `0x91f3510` | NO-GO: `address=C+8=T-0xc8`, above `W` |
| `accept4` | wrapper `0x91f2f08`; child `0x91f2e38`; accept call `0x91f2fe4` | NO-GO: `address=C+8=T-0xe8`; protocol/getname writes it |
| `recvmsg` | inner child `0x91f4848`; protocol call `0x91f4928` | NO-GO: kernel `addr=C+0x18=T-0x2c8`; no contiguous raw 0x50 in `W` |
| `recvmmsg` | child `0x91f4de8`; inner child `0x91f4848`; block `0x91f4928` | NO-GO: copied region is below `W`, and source address is output-only |

Source anchors: `__sys_sendto()` (`net/socket.c:1921-1952`),
`__sys_recvfrom()` (`net/socket.c:1982-2016`),
`__sys_connect()` (`net/socket.c:1808-1827`),
`__sys_accept4()` (`net/socket.c:1702-1763`),
`__sys_sendmmsg()` (`net/socket.c:2335-2390`), and
`___sys_recvmsg()` (`net/socket.c:2408-2454`).

### fs/poll/select

| entry | ELF chain anchor | verdict |
| --- | --- | --- |
| `pselect6` | wrapper `0x82dfb40`; core child `0x82def20`; core call `0x82dfd44`; schedule path in `do_select` | NO-GO: result masks overlap `W`, but are zeroed and kernel-written |
| `select` | wrapper `0x82df990`; common child `0x82def20` | NO-GO: same result-mask semantics |
| `poll` | wrapper `0x82dfff0`; child `0x82e0c98` | NO-GO: `stack_pps=C+0x60=T-0x400`; `revents` rewritten before waiting |
| `ppoll` | wrapper `0x82e0128`; poll child `0x82e0c98` | NO-GO: only an 8-byte sigmask; same poll rewrite rule |
| `epoll_pwait` | wrapper `0x831f6c0`; wait child `0x8320928` | NO-GO: no raw stack array; only an 8-byte sigmask |

`core_sys_select()` allocates six bitmaps in `stack_fds`
(`fs/select.c:597-643`). For `nfds=257..320`, the result masks exactly span
the waiter window, but `zero_fd_set()` clears all three result regions before
polling (`fs/select.c:645-651`). `do_select()` returns early after the first
word with readiness (`fs/select.c:514-552`), so it cannot reliably write all
0x50 bytes. `do_pollfd()` writes `pollfd->revents` before the wait
(`fs/select.c:810-837`), defeating raw reuse of the poll buffer.

### timer/nanosleep

| entry | ELF chain anchor | verdict |
| --- | --- | --- |
| `nanosleep` | wrapper `0x817fe30`; timer/block path `0x817ff08` | NO-GO: one 0x10 `timespec64` |
| `clock_nanosleep` | wrapper `0x8189590`; protocol dispatch `0x8189690` | NO-GO: one 0x10 `timespec64` |
| `timerfd_settime` | wrapper `0x8321e78`; child call `0x8321ec4` | NO-GO: `new`/`old` are 0x10 each and semantically normalized |
| `rt_sigtimedwait` | wrapper `0x80d7078`; child call `0x80d7180` | NO-GO: sigset/timeout/info do not form a raw 0x50 object in `W` |

Source anchors: `nanosleep` (`kernel/time/hrtimer.c:1815-1865`),
`clock_nanosleep` (`kernel/time/posix-timers.c:1201-1233`),
`timerfd_settime` (`fs/timerfd.c:440-545`), and
`rt_sigtimedwait` (`kernel/signal.c:3316-3345`).

### futex/requeue/wait/timeout

`futex_wait_requeue_pi()` has the target 0x50 stack object, but it is the
kernel-allocated `struct rt_mutex_waiter rt_waiter`, not a raw user-controlled
object. The futex user arguments are uaddr/val/timeout pointers and small
normalized timeout values. No 0x50 contiguous raw user stack object was found
on the wait/requeue/timeout paths. The known stale-waiter mechanics are in
`kernel/futex.c:3311-3352` and `kernel/locking/rtmutex.c:1788-1805`; the ELF
futex wrapper anchor is `0x8196c50`.

### SysV msg

`CONFIG_SYSVIPC` is disabled:

```text
build/.config:33  # CONFIG_SYSVIPC is not set
```

The native stubs return `-ENOSYS` directly:

```text
0xffffff80080ed338  mov x0, #-0x26; ret   __arm64_sys_msgrcv
0xffffff80080ed348  mov x0, #-0x26; ret   __arm64_sys_msgsnd
```

Therefore `msgsnd`/`msgrcv` cannot reach any blocking frame on this build.

## Corrections and limits

1. `docs/specs/sendmsg-frame-audit.md` is corrected by this report for the
   native non-compat TCP `sendmsg` path. Its `iovstack` size and conclusion
   were wrong; the ELF values `C+0x38` and `x29-0x88` prove adjacency.
2. This is a placement-only GO. It does not validate any downstream fake
   `task`/`lock` semantics or claim that the full PI chain is safe.
3. The audit covers native AArch64 syscall entry points named by the requested
   families. Compat syscall wrappers are out of scope for the native 64-bit
   route.

## Final classification

```text
GO      blocking TCP sendmsg (iovstack+address)
NO-GO   futex/requeue/wait/timeout
NO-GO   sendto, sendmmsg, recvfrom, recvmsg, recvmmsg, connect, accept4
NO-GO   select, pselect6, poll, ppoll, epoll_pwait
NO-GO   nanosleep, clock_nanosleep, timerfd_settime, rt_sigtimedwait
N/A     SysV msg (CONFIG_SYSVIPC=n, ENOSYS stubs)
```
