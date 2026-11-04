# Symbol resolution — `rt_mutex_adjust_prio_chain+0x304`

Status: resolved against the **stock firmware image**, not a rebuild. The device's
reported PC is now a single instruction, the structure fields it reads, and the
source line it was inlined from. The *value* of the pointer it read is still not
recoverable, and §5 records the one contradiction that value would settle.

No device run, no build, no payload change. Every step below is a read of bytes
that were already on this machine.

## 1. The instruction

| field | value |
| --- | --- |
| reported PC, the three lost runs (54 DropBox uploads carry the string) | `rt_mutex_adjust_prio_chain+0x304/0x858` |
| symbol start (stock image) | `ffffff8008146ca4`, local symbol `t` |
| symbol size | `0x858` — the distance to the next symbol, `rt_mutex_init_waiter` at `ffffff80081474fc` |
| faulting address | `ffffff8008146fa8` = `ffffff8008146ca4 + 0x304` |
| instruction | `f9401d08  ldr x8, [x8, #56]` |
| reads | `w->lock`, where `w = rb_entry(lock->waiters.rb_leftmost, struct rt_mutex_waiter, tree_entry)` |
| inlined from | `kernel/locking/rtmutex_common.h:52-63`, at the call site `kernel/locking/rtmutex.c:661` |

The window, with `x21 = lock` and `x25 = waiter` (the task's blocked-on waiter):

| offset | address | word | instruction | meaning |
| --- | --- | --- | --- | --- |
| `+0x2f8` | `ffffff8008146f9c` | `f9400ea8` | `ldr x8, [x21, #24]` | `leftmost = rb_first_cached(&lock->waiters)` |
| `+0x2fc` | `ffffff8008146fa0` | `b40000c8` | `cbz x8, +0x314` | `if (leftmost)` — NULL returns `w = NULL` |
| `+0x300` | `ffffff8008146fa4` | `f90003e8` | `str x8, [sp]` | `w = rb_entry(leftmost, …)` |
| **`+0x304`** | **`ffffff8008146fa8`** | **`f9401d08`** | **`ldr x8, [x8, #56]`** | **`w->lock` — the recorded fault** |
| `+0x308` | `ffffff8008146fac` | `eb15011f` | `cmp x8, x21` | `w->lock != lock` |
| `+0x30c` | `ffffff8008146fb0` | `54000060` | `b.eq +0x318` | consistent: continue |
| `+0x310` | `ffffff8008146fb4` | `1400014a` | `b +0x838` | inconsistent: BUG |
| `+0x314` | `ffffff8008146fb8` | `f90003ff` | `str xzr, [sp]` | `w = NULL` |
| `+0x318` | `ffffff8008146fbc` | `f9400328` | `ldr x8, [x25]` | `rt_mutex_dequeue(lock, waiter)` begins |

`+0x838` is `ffffff80081474dc`: `d4210000  brk #0x800`, the `BUG_ON(w->lock != lock)`
trap. The recorded PC is four instructions before it, on the load, so the panic is
a fault at that read, **not** the `BUG_ON` trap firing.

## 2. Provenance: this is the device's own image

The chain below is byte-level, not a version-string comparison.

| step | artifact | check | result |
| --- | --- | --- | --- |
| firmware | `AP_G986U1UESCHXL1_G986U1UESCHXL1_…_OS13.tar.md5` | the archive name carries the device's PDA | matches the device's build |
| boot partition | `boot.img.lz4` → `lz4 -dc` | equals `boot.img` | identical, 67,108,864 B |
| kernel payload | `kernel.bin`, at `boot.img + 0x1000` | arm64 Image header: magic `ARM\x64` at `0x38`, `text_offset = 0x80000`; whole region byte-identical to its offset in `boot.img` | present, identical |
| symbol table | `vmlinux.elf` (built by `vmlinux-to-elf` from `kernel.bin`) | its `.kernel` segment at file offset `0x40 + (vaddr − 0xffffff8008080000)` is byte-identical to `kernel.bin[:0x2695260]` | identical |
| build string | inside that image | `Linux version 4.19.113-27095354 (dpi@21DJ7C20) (clang version 10.0.6 for Android NDK) #1 SMP PREEMPT Fri Dec 27 14:53:28 KST 2024` | equals the device's `uname -v` and `/proc/version` |
| **size agreement** | device printed `/0x858` | next symbol in this image is `0x858` away | **matches** |

The identification rests on the byte comparisons and the embedded build metadata; the
size agreement is corroboration, not unique proof. `/0x858` is produced by the
device's own kallsyms lookup, which stores no sizes and reports the distance to the
next symbol, so an equal distance is consistent with this being the image that
formatted the panic — it is not independently sufficient on its own.

| file | bytes | sha256 |
| --- | ---: | --- |
| `kernel.bin` | 51,957,772 | `ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123` |
| `boot.img.lz4` | 25,597,493 | `02099ccbfb423292b631f228b8d3163b9a601f4a058360a4bca93f71f8e50a78` |
| `boot.img` | 67,108,864 | `bd8987c28ef0b9fdb56ad3e82ce3b45929c5e53717525769e5625dd1e17f7dc8` |
| `vmlinux.elf` | 58,474,904 | `531444e3c98f8f7c4d2baefeda1386aca2b60a42030fcb1f4393c5e10b9154b4` |
| `kernel.config` | 190,938 | `ec6da4cfe412f7d0669c7d621751753375e32197a2602f98627eb95e71b9c0b2` |

These are not new inputs. The generated `src/targets/samsung-g986u1-4.19.113/target.h`
banner already records `kernel_image`, `kernel_config` and `vendor_elf` with these
three hashes, so the image that produced the symbol is the same image the project's
offset generator consumes. The local October 2026 rebuild (ledger row 35,
`rt_mutex_adjust_prio_chain = ffffff8008146c04`, `0xa0` earlier) is not involved and
never was.

## 3. Source mapping

`kernel/locking/rtmutex_common.h:52-63`:

```c
static inline struct rt_mutex_waiter *
rt_mutex_top_waiter(struct rt_mutex *lock)
{
	struct rb_node *leftmost = rb_first_cached(&lock->waiters);
	struct rt_mutex_waiter *w = NULL;

	if (leftmost) {
		w = rb_entry(leftmost, struct rt_mutex_waiter, tree_entry);
		BUG_ON(w->lock != lock);
	}
	return w;
}
```

`kernel/locking/rtmutex.c:661`, in the requeue path of `rt_mutex_adjust_prio_chain`
(right after `raw_spin_trylock(&lock->wait_lock)` succeeded and the deadlock check
passed):

```c
	prerequeue_top_waiter = rt_mutex_top_waiter(lock);
```

then `:664  rt_mutex_dequeue(lock, waiter);`, which is the `rb_erase_cached` at
`+0x318`. That is what places this inlined copy at `+0x2f8`: it is the
*prerequeue* top-waiter read, not the deadlock-detection one at `:677`.

The two offsets in the faulting expression are the ones this project already
documents for the target:

| field | offset | source |
| --- | ---: | --- |
| `rt_mutex.waiters.rb_leftmost` | `0x18` | `RT_MUTEX_LEFTMOST_OFF`; `CONFIG_DEBUG_QSPINLOCK_OWNER=y` makes `arch_spinlock_t` 16 B, so `waiters` starts at `0x10` and its `rb_leftmost` at `0x18` |
| `rt_mutex_waiter.lock` | `0x38` | `WAITER_LOCK_OFF`, `FAKE_WAITER_LOCK_OFF` |

The disassembly agrees with both (`[x21,#24]`, `[x8,#56]`), which is an independent
confirmation of the layout the profile derives from source.

## 4. What the payload plants in that word

This is the part that makes the fault informative. `src/util.c:1872-1878` writes the
lock's tree root and owner deliberately, and its own comment names this check:

```c
      /* The walk reads rb_leftmost as the top waiter and checks
       * top_waiter->lock (+0x38) against this lock before following the
       * chain to the owner and the PI-tree erase.  Leave fake_w0 there so
       * that erase is reachable; a zero here ends the walk at +0x300 and
       * never performs the write. */
      put64(p, LOCK_OFF + RT_MUTEX_WAITERS_OFF, fake_w0);
      put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);
      put64(p, LOCK_OFF + RT_MUTEX_OWNER_OFF, fake_task | 1);
```

with `fake_lock = payload_base + LOCK_OFF` and `fake_w0 = payload_base + W0_OFF`
(`src/util.c:1783-1784`; `LOCK_OFF = 0x1350`, `W0_OFF = 0x2220`, page `0x8000`).

So the intended object graph is two separate stores at two addresses: the word
at `payload_base+0x1368` (`lock+0x18`) holds `payload_base+0x2220`, and the word
at `payload_base+0x2258` (`waiter+0x38`) holds `payload_base+0x1350`. Both must
survive to make the `cmp` at `+0x308` succeed. The two objects are `0xed0` apart
and both inside one 32 KiB page, because both are `payload_base + offset` with
the order-3 base the leak supplied.
It is a layout property, and not a mapping or retention claim: nothing in the
record shows the page was mapped, or that the load reached `payload_base+0x2258`. `verify_symbol.py` recomputes both addresses
and both stored values from the target's own `target.h` constants rather than
from this prose.

## 5. The contradiction the instruction creates

A read at `lock+0x18` (`+0x2f8`) **succeeded** and a read at `leftmost+0x38`
(`+0x304`) **faulted**, in the same frame. If `lock` were the planted lock *and*
the word at `lock+0x18` were the planted `fake_w0`, the second read would land in
the same 32 KiB page as the first and would be readable — so the fault requires
that at least one of those two conditions did not hold at that moment.

Same-page is a property of the *intended* addresses, not of the values the registers
actually held, so it constrains the explanation without settling it. Four families
survive, and `register-dataflow.md` derives them from the verified path:

| family | claim | what only a runtime record could show |
| --- | --- | --- |
| wrong `leftmost` value | `lock+0x18` did not hold `fake_w0` | `x8`, or `FAR` |
| wrong lock pointer | `x21` was not the planted `fake_lock` | `x21` |
| invalid waiter pointer | the word named memory that is not readable | `FAR`/`ESR` |
| translation or permission fault | the access failed at the MMU rather than by value | `ESR` |

None of the four can be selected from what was preserved. The crash records contain
the bootloader's summary and per-CPU idle context saves only; the kernel's own oops
text, the faulting CPU's registers, `FAR` and `ESR` are not readable in any of the
four 132 KB captures, and `pimem_esr` is the bootloader's own register, `0x0`.

This is a sharper statement than "the walk died": the recorded PC is inconsistent with
the image the program intends *unless* the state at that word differed from the intent.
The first family is the one the project's own history makes plausible — the same
page-state question as the missing 31 identities — but it is a hypothesis, not a
measurement.

Three evidence categories are kept apart throughout: **measured** (the entries tied to
the three lost runs report the same symbol and offset), **verified against the device image** (the offset resolves
to `ldr x8, [x8, #56]`), and **unresolved** (the register values, the effective address,
the exception syndrome, and the identity of the lock and waiter).

## 6. What this settles, and what it does not

Settled:

- The faulting instruction, its symbol, its inlined source, and the structure fields
  it reads, from the same image whose kallsyms formatted the device's PC.
- The panic is a fault on the load, not the `BUG_ON` trap four instructions later.
- The chain had already taken `lock->wait_lock` and passed the deadlock check; it
  reached the *prerequeue* top-waiter read of the requeue path.
- The `+0x304` offset did not move across the three losses, and 54 DropBox uploads
  carry the same string (`enumeration-20261104.md`); an upload is not an independent
  fault, so that is a recurrence of collector output, not a panic count. The offset
  is unrelated to the local rebuild's layout.

Not settled:

| question | why not |
| --- | --- |
| which lock (`x21`) | registers unavailable |
| which pointer (`x8`) and the fault address | registers, `FAR`, `ESR` unavailable |
| whether the planted page was still mapped and still owned by the payload | no memory snapshot; candidates A–C above |
| the call path from userspace | no backtrace; the stack in the dump is unreadable |
| causation by the route | still an inference from the execution record, not from this instruction |
| occupancy, the 31 identities, delivery, root | untouched by this work |

## 7. Acceptance criteria

| criterion | status |
| --- | --- |
| artifact provenance matches the firmware and the kernel build | **met** — byte chain from the AP archive's `boot.img.lz4`, and the image's own build string equals the device's |
| its `rt_mutex_adjust_prio_chain` symbol is verified | **met** — `ffffff8008146ca4`, and the next symbol reproduces the device's `0x858` size |
| the calculated address lies within the symbol's recorded size | **met** — `0x304 < 0x858` |
| the instruction is disassembled from that verified image | **met** — `f9401d08`, with five instructions either side |
| it is mapped to the matching vendor source, with surroundings | **met** — `rtmutex_common.h:52-63` inlined at `rtmutex.c:661`, `break #0x800` at `+0x838` |
| operand interpretation is kept separate from what the register dump establishes | **met** — the dump establishes nothing; §5 is written as a disjunction |
| a faulting PC/LR or backtrace from the crash record | **not met** — unavailable in all four captures; this is the remaining gap |

## 8. Reproduction

```sh
# identity
lz4 -dc boot.img.lz4 | cmp - boot.img
python3 - <<'PY'
kb, bi = open("kernel.bin","rb").read(), open("boot.img","rb").read()
assert bi.find(kb[:0x1000]) == 0x1000
PY

# symbol and size
nm -n --defined-only vmlinux.elf | grep -E ' (rt_mutex_adjust_prio_chain|rt_mutex_init_waiter)$'

# instruction
aarch64-linux-gnu-objdump -d \
  --start-address=0xffffff8008146ca4 \
  --stop-address=0xffffff80081474fc vmlinux.elf | sed -n '/6fa0:/,/6fc0:/p'
```

The host verifier `verify_symbol.py` (kept with the other pass verifiers, not in the
repository) re-runs the same checks and fails if any of them stops holding. The
register-level follow-up is `register-dataflow.md`.

## 9. Two notes for the record

1. `src/targets/samsung-g986u1-4.19.113/target.h:215` annotates
   `RT_MUTEX_WAITERS_OFF` with `(see +0x2f8 add x1,x21,#0x10)`. The value `0x10` is
   right, but in this image `add x1, x21, #0x10` is at `+0x324` (the `rb_erase_cached`
   argument); `+0x2f8` is `ldr x8, [x21, #24]`. The file is generated, so this is
   recorded rather than hand-edited.
2. `docs/specs/sendmsg-pi-shape-adversarial-audit.md` places `rb_leftmost` at `+0x10`
   for a different target (the alioth ELF, no `DEBUG_QSPINLOCK_OWNER`, `waiters` at
   `+0x08`). Both are correct for their own build; neither may be carried across.
