# Sendmsg placement cross-audit

Date: 2026-09-16
Scope: offline cross-audit of the blocking TCP `sendmsg` GO candidate in
`docs/specs/kernel-frame-placement-enumeration.md`. No device access and no
exploit-code change.

## Verdict

**PASS.**

Failure conditions found: none.

The audited facts are independently supported by the target ELF and the
registered materials source tree. The placement math, adjacency, and raw
stability during the TCP blocking window are correct. The only caveat is that
this remains a placement-only result; downstream fake task/lock semantics are
not validated by this document.

## Evidence

ELF:

```text
k40rom/boot/kernel.elf
sha256 F73A2163A0A8311ED44C731951E8E5A7A789B1B08E1931C920632A9E3098B662
```

Source:

```text
materials/kernelsrc/sm8250-a13-stable
config build/.config
```

## Placement check

The target ELF shows:

```text
0xffffff80091f4268  sub  sp, sp, #0x90     __arm64_sys_sendmsg wrapper
0xffffff80091f3f98  sub  sp, sp, #0x190    ___sys_sendmsg child prologue
0xffffff80091f3fd0  add  x9, sp, #0x38     &iovstack[0]
0xffffff80091f3fd4  sub  x10, x29, #0x88   &address
0xffffff80091f4188  blr  x8                protocol sendmsg dispatch
```

With the wrapper entry SP as `T`, the child stack pointer at the blocking
dispatch is:

```text
C = T - 0x220
W = T - 0x198 .. T - 0x149
```

Therefore:

```text
iovstack       = C + 0x38
address        = C + 0xb8
iovstack size  = UIO_FASTIOV * sizeof(struct iovec) = 8 * 0x10 = 0x80
```

The two objects are exactly adjacent:

```text
iovstack[5].iov_base = C + 0x38 + 5 * 0x10 = C + 0x88 = W + 0x00
iovstack[5].iov_len  = C + 0x90             = W + 0x08
iovstack[6].iov_base = C + 0x98             = W + 0x10
iovstack[6].iov_len  = C + 0xa0             = W + 0x18
iovstack[7].iov_base = C + 0xa8             = W + 0x20
iovstack[7].iov_len  = C + 0xb0             = W + 0x28
address[0x00..0x1f]  = C + 0xb8..C + 0xd7   = W + 0x30..W + 0x4f
```

So `iovstack[5..7]` plus the first 32 bytes of `address` do form one
continuous, raw user-controlled 0x50-byte region over the stale `rt_waiter`.

The source tree agrees:

- `___sys_sendmsg()` declares `struct sockaddr_storage address` and
  `struct iovec iovstack[UIO_FASTIOV]`
  (`net/socket.c:2205-2226`).
- `copy_msghdr_from_user()` copies the address and imports the iov array
  (`net/socket.c:2154-2202`).
- `move_addr_to_kernel()` performs a raw user copy capped by
  `sizeof(struct sockaddr_storage)` (`net/socket.c:200-209`).
- `UIO_FASTIOV` is 8 (`include/uapi/linux/uio.h:27-28`), and native
  `struct iovec` is 0x10 bytes (`include/uapi/linux/uio.h:18-22`).

## TCP blocking stability

For a connected established TCP socket in the normal send path, the source
does not consume `msg_name` unless fastopen or deferred connect is selected.
The normal path instead waits in `sk_stream_wait_memory()` when write memory
is exhausted:

- `tcp_sendmsg_locked()` obtains the send timeout and enters the normal send
  loop (`net/ipv4/tcp.c:1207-1462`).
- `sk_stream_wait_memory()` waits on socket writability
  (`net/core/stream.c:118-182`).
- The connected-TCP normal path ignores `msg_name`/`msg_namelen`
  (`net/ipv4/tcp.c:2184-2186`).

The kernel may advance the `iov_iter`, but it does not rewrite the
`iovstack[]` array itself. Zero-length segments are not dereferenced. With
`msg_controllen=0`, no cmsg path rewrites the target bytes. Therefore the
copied iov/address bytes remain stable during the blocking window.

## iov_base and tree fields

`rw_copy_check_uvector()` validates `iov_base` even when `iov_len == 0`:

```c
if (type >= 0
    && unlikely(!access_ok(vrfy_dir(type), buf, len)))
        return -EFAULT;
```

at `fs/read_write.c:816-830`. On arm64, `access_ok()` reduces to
`addr + size <= addr_limit + 1` (`arch/arm64/include/asm/uaccess.h:85-110`).
With `size == 0`, values `0`, `1`, and ordinary user-range pointers pass the
range check; kernel pointers do not. A zero-length iov entry is never
dereferenced, so no backing mapping is required.

This is sufficient for the intended terminal rb-node fields that are carried
in `iov_base` slots (`0` or `1`). The fields that must remain zero are carried
in the corresponding `iov_len` slots and are therefore naturally zero. If a
future downstream contract required a real kernel pointer in one of the
`iov_base` slots, this placement would fail that contract, because
`access_ok()` would reject it.

## Failure conditions checked

The following failure conditions were checked and none was present:

- `iovstack` was not 0x40 bytes; ELF/source confirm 0x80 bytes.
- `iovstack` and `address` are exactly adjacent, with no gap.
- `address` is not overwritten by the connected-TCP normal send path.
- `iovstack[5..7]` are not rewritten during the blocking window.
- `access_ok()` accepts the required zero-length `iov_base` values.
- `msg_controllen=0` avoids cmsg rewriting.
- The normal TCP path does not enter fastopen/deferred-connect handling.

