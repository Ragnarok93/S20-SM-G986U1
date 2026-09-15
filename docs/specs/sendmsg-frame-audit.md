# sendmsg-family frame audit for the dangling rt_waiter window

Date: 2026-09-15
Scope: offline-only audit. No device command was run and no exploit code was
modified. Evidence sources are `k40rom/boot/kernel.elf` and the registered
kernel tree `materials/kernelsrc/sm8250-a13-stable`.

Kernel ELF:

```text
path     k40rom/boot/kernel.elf
size     57,126,645 bytes
sha256   F73A2163A0A8311ED44C731951E8E5A7A789B1B08E1931C920632A9E3098B662
```

## Verdict

**NO-GO.**

The native blocking `sendmsg`/`sendmmsg`/`recvmmsg` paths cannot place a
continuous, user-controlled 0x50-byte payload over the dangling
`rt_waiter` window:

```text
W  = T-0x198 .. T-0x149      (half-open: T-0x198 .. T-0x148)
W  size                      = 0x50
```

The best case is native `sendmsg`: its 128-byte `sockaddr_storage address`
only overlaps the upper 0x20 bytes of the waiter (`task`, `lock`, `prio`,
and `deadline`). The first 0x30 bytes, both rb nodes, remain unreachable.
`sendmmsg` misses the window with its copied address, and `recvmmsg` does
not copy a user address into its kernel `addr` at all. Because the required
control bytes are not contiguous, this route is rejected by the stated
mathematical criterion.

## Frame calibration

`T` is the native syscall-wrapper entry SP. On arm64, `struct pt_regs` is
0x110 bytes (`arch/arm64/include/asm/ptrace.h:148-169`), and this 4K-page +
VMAP-stack build has `THREAD_SIZE=0x4000`
(`arch/arm64/include/asm/memory.h:89-105`; `build/.config:242,670`).
Thus:

```text
T = task_stack_page + THREAD_SIZE - sizeof(struct pt_regs)
  = task_stack_page + 0x4000 - 0x110
```

The waiter layout follows `struct rt_mutex_waiter`
(`kernel/locking/rtmutex_common.h:27-42`) with debug fields disabled:

```text
waiter offset  field
0x00..0x17     tree_entry
0x18..0x2f     pi_tree_entry
0x30..0x37     task
0x38..0x3f     lock
0x40..0x47     prio
0x48..0x4f     deadline
```

In T-relative form:

```text
T-0x198  tree_entry
T-0x180  pi_tree_entry
T-0x168  task
T-0x160  lock
T-0x158  prio
T-0x150  deadline
T-0x148  one past the waiter
```

## Native `sendmsg` frame

ELF evidence:

```text
0xffffff80091f4268  sub  sp, sp, #0x90        ; sendmsg wrapper
0xffffff80091f42e0  mov  x2, sp              ; &msg_sys
0xffffff80091f3f98  sub  sp, sp, #0x190      ; inner __sys_sendmsg
0xffffff80091f3fa0  add  x29, sp, #0x140
0xffffff80091f3fd0  ___sys_sendmsg:
0xffffff80091f3fd4  sub  x10, x29, #0x88     ; &address
0xffffff80091f3fe0  str  x10, [x2]           ; msg_sys->msg_name
0xffffff80091f4188  blr  x8                  ; blocking protocol sendmsg
```

Arithmetic:

```text
E       = T
R       = E - 0x90                       ; wrapper SP
C       = R - 0x190 = E - 0x220          ; ___sys_sendmsg SP
&address= C + 0xb8 = E - 0x168
span    = E-0x168 .. E-0xe9              ; 0x80-byte sockaddr_storage
```

Intersection with the waiter:

```text
address - waiter = (T-0x168) - (T-0x198) = 0x30
overlap          = T-0x168 .. T-0x149
overlap size     = 0x20
```

Therefore only waiter offsets `0x30..0x4f` can be shaped. Offsets
`0x00..0x2f` are unreachable. The maximum continuous controlled coverage is
`0x20`, not the required `0x50`.

The source matches this path: `___sys_sendmsg()` declares
`sockaddr_storage address`, assigns `msg_sys->msg_name = &address`
(`net/socket.c:2205-2221`), and `copy_msghdr_from_user()` calls
`move_addr_to_kernel()` into `kmsg->msg_name` when `save_addr == NULL`
(`net/socket.c:2154-2185`). `move_addr_to_kernel()` is a raw user copy
limited to 128 bytes (`net/socket.c:201-210`). The blocking Unix path can
keep `sunaddr = msg->msg_name` and wait in `unix_wait_for_peer()`
(`net/unix/af_unix.c:1653,1796`; `net/unix/af_unix.c:1210-1228`), so the
failure is placement, not stability.

For completeness, the nearby user-derived locals in the same frame remain
below the window:

```text
ctl       = C+0x10  = T-0x210 .. T-0x1e1
iovstack  = C+0x38  = T-0x1e8 .. T-0x1a9
```

Neither closes the gap to `T-0x198`.

## Native `sendmmsg` frame

ELF evidence:

```text
0xffffff80091f4768  sub  sp, sp, #0x160        ; sendmmsg wrapper
0xffffff80091f4404  add  x2, sp, #0x10         ; &msg_sys
0xffffff80091f4408  add  x4, sp, #0x68         ; &used_address
0xffffff80091f4428  bl   0xffffff80091f3f98    ; common inner sendmsg
```

The common callee is the same `sub sp, sp, #0x190` body and the same
`&address = x29-0x88` construction shown above.

Arithmetic:

```text
E            = T
R            = E - 0x160                    ; sendmmsg SP
C            = R - 0x190 = E - 0x2f0        ; inner sendmsg SP
&address     = C + 0xb8 = E - 0x238
address span = E-0x238 .. E-0x1b9
```

This is entirely below the window:

```text
address end - W start = (T-0x1b8) - (T-0x198) = -0x20
```

so the copied user address has zero overlap with the waiter.

Other sendmmsg locals do not repair the coverage:

```text
&msg_sys      = R+0x10 = T-0x150
&used_address = R+0x68 = T-0xf8
```

`msg_sys`'s first qword overlaps only the waiter's `deadline`, but that
qword is `msg_name`, assigned by the kernel to `&address`; it is not raw
user-controlled data. `used_address` is above the window entirely. The
maximum continuous controlled coverage remains zero.

Source correspondence is direct: `__sys_sendmmsg()` passes `&msg_sys` and
`&used_address` to `___sys_sendmsg()` for every iteration
(`net/socket.c:2335-2389`).

## Native `recvmmsg` frame

ELF evidence:

```text
0xffffff80091f5270  sub  sp, sp, #0x70        ; do_sys_recvmmsg wrapper
0xffffff80091f52f0  add  x0, sp, #0x8         ; &timeout_sys
0xffffff80091f4de8  sub  sp, sp, #0xf0        ; __sys_recvmmsg
0xffffff80091f4eb0  add  x2, sp, #0x20        ; &msg_sys
0xffffff80091f4848  sub  sp, sp, #0x180       ; inner ___sys_recvmsg
0xffffff80091f486c  add  x12, sp, #0x18       ; &addr
0xffffff80091f48a0  str  x12, [x2]            ; msg_sys->msg_name
0xffffff80091f4928  blr  x8                   ; blocking protocol recvmsg
```

Arithmetic:

```text
E         = T
R         = E - 0x160                       ; __sys_recvmmsg SP
C         = R - 0x180 = E - 0x2e0           ; ___sys_recvmsg SP
&addr     = C + 0x18 = E - 0x2c8
addr span = E-0x2c8 .. E-0x249
```

No part of `addr` overlaps the window.

More importantly, this is not a raw user copy. `___sys_recvmsg()` declares
the kernel `addr`, sets `msg_sys->msg_name = &addr`, and passes `&uaddr`
as `save_addr` to `copy_msghdr_from_user()`
(`net/socket.c:2408-2431`). With `save_addr != NULL`, the user address is
only saved; the protocol later writes kernel `addr`, and
`move_addr_to_user()` copies it back out (`net/socket.c:2442-2454`).

The user-controlled `timeout_sys` lives in the wrapper frame at
`T-0x68`, far above the window. The `end_time` local lives at
`R+0x10 = T-0x150`; only its first qword touches the waiter's `deadline`,
and it is derived from timeout normalization rather than a continuous
0x50-byte raw payload. This cannot bridge the missing `0x00..0x47` span.

## User-controlled coverage matrix

`W-off` is the offset from `T-0x198`. "Overlap" is bytes inside
`T-0x198..T-0x149`. Raw means bytes copied from user without semantic
normalization.

| syscall/object | base T-relative | size | overlap | continuous raw coverage |
|---|---:|---:|---:|---:|
| `sendmsg` / `address` | `T-0x168` | `0x80` | `0x20` | waiter `0x30..0x4f` |
| `sendmsg` / `ctl` | `T-0x210` | `0x30` | `0` | none |
| `sendmsg` / `iovstack` | `T-0x1e8` | `0x40` | `0` | none |
| `sendmmsg` / `address` | `T-0x238` | `0x80` | `0` | none |
| `sendmmsg` / `used_address` | `T-0xf8` | `0x88` | `0` | none |
| `sendmmsg` / `msg_sys` | `T-0x150` | `0x50` | `8` | none; first qword is kernel `msg_name` |
| `recvmmsg` / `addr` | `T-0x2c8` | `0x80` | `0` | none; not user-copied |
| `recvmmsg` / `timeout_sys` | `T-0x68` | `0x10` | `0` | none |
| `recvmmsg` / `end_time` | `T-0x150` | `0x10` | `8` | none; normalized timeout, not raw 0x50 |

No combination inside one blocking syscall produces continuous raw control
across waiter offsets `0x00..0x4f`. In particular, `sendmsg` misses
`0x00..0x2f`; `sendmmsg` and `recvmmsg` provide no raw bridge; and the
isolated deadline qwords are either kernel pointers or normalized time.

## Blocking-point SP-relative offsets

At the protocol-dispatch blocking call, the inner SP is `C`:

| syscall | blocking site | C (T-relative) | controlled object | object base |
|---|---:|---:|---|---:|
| `sendmsg` | `0xffffff80091f4188` | `T-0x220` | `address` | `C+0xb8 = T-0x168` |
| `sendmmsg` | same inner site | `T-0x2f0` | `address` | `C+0xb8 = T-0x238` |
| `recvmmsg` | `0xffffff80091f4928` | `T-0x2e0` | `addr` | `C+0x18 = T-0x2c8` |

These offsets reproduce the T-relative intersections above. There is no
sign or offset ambiguity: adding the object size still fails to cover the
lower waiter fields.

## Conclusion

**NO-GO for sendmsg/sendmmsg/recvmmsg waiter shaping.**

The ELF and registered source agree. The largest continuous raw
user-controlled overlap is 0x20 bytes (`sendmsg.address`), while the
dangling waiter requires 0x50 contiguous bytes. The bytes at waiter offsets
`0x00..0x2f` remain unreachable on all three native paths. Do not build or
run a sendmsg-family replacement for the pselect result-bitmap encoder.
