# 387 — the `x21` slot

Source: `bodies/387_20261102_134935_KP.txt`, 132,652 bytes,
sha256 `81233afa079173e9b6c143198b4836390325c9c42850ad4daa2d04dfea928beb`.
Line numbers are 1-based text lines after Python's universal-newline decoding
(`Path.read_text()`), the numbering used by `08-kp-387-fault-context.txt` and
`07-kp-scan.tsv`. The body contains lone CRs, so raw byte line numbers differ.

## 1. The register line is lost

Line 1357 is the `x21`/`x20` printk. The block prints `x29`/`x28`, then
`x27`/`x26`, then `x23`/`x22`; after the `x22` value the line resumes at `x20:`
with no `x21` label and no value between them:

```
[... ] h220!00000000 000 000(x22: 02000... 008 ... K0: ... x20: ff`fbfc0%7f 9!%0...
```

No `x21` register label occurs between the `pc` line (1352) and the end of the
fault block (1366). A tolerant scan for a label (`x21:`/`X21:`, any case) in the
rest of the body finds three lines, all idle `swapper` blocks: 1524
(`swapper/4`), 1748 (`swapper/7`) and 1905 (`swapper/5`). Line 1689 belongs to
the `swapper/7` block but holds `x3`/`x4`, not `x21`; a bare substring match also
hits `0x210` in `ipi_cpu_stop+0xb0/0x210` (1827), which is an offset, not a
register label.

## 2. The frame slot

The stock image's prologue and the caller:

| address | word | instruction | effect |
| --- | --- | --- | --- |
| `SYM+0x1c` | `a90657f6` | `stp x22, x21, [sp, #96]` | incoming `x21` → `sp+0x68` |
| `SYM+0x20` | `a9074ff4` | `stp x20, x19, [sp, #112]` | incoming `x20` → `sp+0x70`, `x19` → `sp+0x78` |
| `adjust_pi+0x60` | `f9401d15` | `ldr x21, [x8, #56]` | `next_lock = waiter->lock` |
| `adjust_pi+0x78` | `aa1503e3` | `mov x3, x21` | fourth argument |
| `adjust_pi+0x9c` | `94000007` | `bl rt_mutex_adjust_prio_chain` | target `SYM` |

`SYM = 0xffffff8008146ca4`; `adjust_pi = 0xffffff8008146bec`. The callee's
`mov x21, x3` at `+0x40` is a self-copy, so the word saved at `sp+0x68` is the
initial `next_lock`.

The oops prints `sp = 0xffffff80283f3cb0` (line 1354; the same value is at stack
address `3c68`). The slot is `0xffffff80283f3d18`, printed in the dump line at
`3$10` (line 1393, `$` is a corrupt `d`):

| slot | printed word | value |
| --- | --- | --- |
| `3d10` | `dFae4\x1640 $ff&bbc2` | damaged (incoming `x22`) |
| `3d18` | `5\x17d083<0 fff&fdc0` | damaged (incoming `x21`) |
| `3d20` | `1159D508 ffffffc0` | `0xffffffc01159d508` |
| `3d28` | `1159cc40 ffffffc0` | `0xffffffc01159cc40` |

The two adjacent slots validate the frame map without the register line:
`0xffffffc01159d508 = task + 0x8c8` is the caller's `x20` (its `pi_lock`,
`add x20, x0, #0x8c8` at `adjust_pi+0x14`, unchanged at the call), and
`0xffffffc01159cc40` is the caller's `x19` (`task`). The last word on the `3$10`
row is followed by bytes of the next merged log entry; its eight printed
characters are `ffffffc0` and are intact. The register block prints the same task
value as `x9` (line 1363) and the same `pi_lock` value as `x26` (line 1356).

## 3. Iteration

`+0x2f8` loads `lock->waiters.rb_leftmost` and `+0x300` stores it at `[sp]`;
the dump at `3cb0` holds `0xffffffffffffffff`, so the tree root was all-ones. A
later iteration reaches `+0x2f8` only after `rt_mutex_enqueue` re-inserted the
waiter (`+0x3ec`), and `rb_insert_color_cached` would have written a node
pointer to `rb_leftmost`, not all-ones. All-ones therefore points to the first
iteration, where the word is the planted one. On that reading the saved initial
`next_lock` is the faulting `x21`; it is an inference, not a log line.

## 4. Every other copy

| candidate | location | value | why it is not a readable `x21` |
| --- | --- | --- | --- |
| register line | body line 1357 | label and value absent | nothing to read |
| callee frame | `3d18` | `5\x17d083<0 fff&fdc0` | three non-hex characters, and `d` does not fit the only linear-map shape |
| caller frame | `3d40` (`str x21, [sp, #16]` at `adjust_pi+0x8`) | not printed | `0x18` above the last dumped word (`3d28`) |
| exception frame | `3c18` (`pt_regs` `x21`, base `3b70` from the `pstate`/`sp`/`pc` matches) | not printed | `0x18` below the first dumped line (`3c30`) |
| other CPUs | 1524, 1748, 1905 | `ffffffc26ca61f88`, `0xffffffc26ca61f08` | `Comm: swapper/N` idle blocks, not `Comm: id`; 1689 is the same block's `x3`/`x4` line, not `x21` |
| second oops copy | — | — | one `pc : rt_mutex_adjust_prio_chain` in the body |
| `_NP` companion | — | — | 21 `_NP` ids, all at 18:12 or later; none names `387`; the captured 0805 NP has no `+0x304` line |
| re-pull | `dumpsys dropbox --print 387...` | sha256 `81233afa079173e9b6c143198b4836390325c9c42850ad4daa2d04dfea928beb` again | byte-identical; the damage is in the stored entry |
| persistent log | `/data/local/tmp/ks_log.txt` | userspace lines only | no kernel register context; it starts with the 11-03 runs |

## 5. The post-fault region

The rest of the body (lines 1367–1971) continues the fault's stack dump and
then the per-CPU saves for the idle CPUs and the reboot lines. The tolerant label scan returns
exactly three `x21` lines:

| line | block | value | why it is not the faulting `x21` |
| --- | --- | --- | --- |
| 1524 | `Comm: swapper/4`, CPU 4 | `ffffffc26ca61f88` | idle task's saved register |
| 1748 | `Comm: swapper/7`, CPU 7 | `0xffffffc26ca61f08` | idle task's saved register |
| 1905 | `Comm: swapper/5`, CPU 5 | `0xffffffc26ca61f08` | idle task's saved register |

No `Comm: id` block follows line 1366; every later `CPU:` header is an idle
`swapper` block, whose name is itself character-corrupt in some lines
(`suapper/4`, `qwapper/7`). The values are the idle task's registers on each
CPU, not a waiter or lock pointer from the faulting thread. The region holds no `FAR` label, the
string `sec_debug_panic_handler` does not occur anywhere in the body, and the
only other `pc :` line is the idle `lpm_cpuidle_enter+0x528/0x570` at 1683. The
body's own oops header is corrupt (`... NULL poilter 'drefaban#e ...`), so the
block is identified by its `pc :` line, not by a readable `Unable to handle
kernel` string; none occurs in the body.

## 6. Result

No 64-bit `x21` value is recoverable. The slot is identified and its provenance
is image-verified, but the printed word is damaged: `5\x17d083<0` has two
non-hex characters and `fff&fdc0` has one, and the `d` in the high half does
not fit the only linear-map shape. Six of the low word's eight characters are hex
(`5 d 0 8 3 0`), but the stream is corrupted, so their positions and the two
missing characters are not recoverable. Reading `&` as `f` still leaves
`fffffdc0`, not the linear-map high half `ffffffc0`, so the `d` is suspect too.
The post-fault scan adds no candidate:
the only other `x21` labels are idle-task saves on CPUs 4, 7 and 5. F2 (whether
the walk's lock was `fake_lock`) therefore remains undecided for `387` and for
the three losses.

Scope: this is `387`'s record. `387` has no saved host run and is not one of
`399`, `400` or `401`; none of its values may be written into a statement about
the three lost runs. `enumeration-20261104.md`, `register-dataflow.md`,
`symbol-resolution.md` and `recovery-20261104.md` each state the same scope and
none transfers a `387` value to `399`, `400` or `401`.
