# Dropbox enumeration, 2026-11-04

Scope: read-only enumeration of `dumpsys dropbox` and a capture of every `_KP`
body. No payload run, no reboot, no change to the device. The first pass added 398
to the three recovery captures and inventoried the rest; the second captured all
55 and found the only `+0x304` register record in the set (§§5–7).

## 1. The complete listing

```sh
adb shell dumpsys dropbox | grep -a LAST_KMSG          # 118 lines
adb shell dumpsys dropbox --print <id> | grep panic_msg  # once per _KP id
```

is preserved as `logs/20261104_enumeration/` (`01` listing, `04` per-id
inventory, `05` id list, `02` the first capture, `06` capture log, `07` scan,
`08` the fault block). The listing holds **55 distinct `_KP` ids**, 23 written on
2026-11-01, 24 on 11-02 and 8 on 11-03. Each id appears twice in the listing with
two compressed sizes; the per-id print returns that id's blocks concatenated —
one or two of them, about 66 KB or 132 KB.

| count | property |
| --- | --- |
| 55 | distinct `_KP` ids |
| 54 | carry the identical `collect_rr_data` line, quoted below |
| 1 | `347_20261101_143931_KP`, the earliest, carries none |

```
collect_rr_data : upload_cause = KERNEL PANIC ( panic_msg = Oops PC = rt_mutex_adjust_prio_chain+0x304/0x858 )
```

The three lost runs are three of those ids (399, 400, 401). `398` is one of the
remaining five written on 11-03 and is the only one captured here.

## 2. What the count does and does not establish

**Measured:** 55 DropBox uploads tagged `_KP`, 54 of them carrying that exact
string; 52 were written before the earliest saved run log (10:09:42).

**An upload is not a fault.** What is **not established** is that the 54 are 54
distinct panics at that PC, or that each upload describes its own boot's crash.
The uploads are not independent
observations of a fault: they are collector outputs, and the string could be
re-emitted from a persisted record. The listing alone cannot separate "54 panics
with this PC" from "one persisted panic string, uploaded 54 times". The `_NP`
entries show the collector also runs without a panic string, so the tag does
depend on something — but not, from this evidence, on a fresh crash per upload.

Consequently this inventory may not be cited as a panic count. It may be cited
as: the recovery session's "exactly three `_KP` entries … and no others" was
false, and the DropBox history is far larger than three entries.

## 3. The 398 capture

`02-kp-398_20261103_080145.txt`, 132,616 bytes, sha256 `dfe04446…7671ff3`,
pulled and hashed before interpretation.

- Line 142 carries the same collector line as the other 53.
- No `Unable to handle`, no `Comm: id`, and no faulting-CPU register block. The
  six `CPU: N PID` blocks are idle `swapper` dumps at `lpm_cpuidle_enter`.
- The text is transport-corrupted in places and is preserved unedited.

So 398 adds a device-reported string and a capture, not a register set and not a
demonstrated extra panic.

## 4. What changes

- The recovery session's inventory sentence is retracted as **R6**: there are 55
  `_KP` ids, not three, and 54 carry the same message. 52 of them precede the
  earliest saved log; 3 are the lost runs.
- `398` (08:01:45) has no saved run: the earliest saved log begins 10:09:42. It
  cannot be tied to a plant, a punch, or a stage.
- The recurrence of the string across two earlier days is a fact about collector
  output. It is consistent with the same fault recurring, but it is not a count of
  faults, and it says nothing about which run or code path produced any one of them.

## 5. The full sweep: a body for every id

The listing was re-read immediately before the sweep and was byte-identical
(`4748bd20…`), so all 55 ids were still present when capture began — the oldest
entry sits near DropBox's retention edge, so that check is recorded, not assumed.

| count | property |
| --- | --- |
| 55 | ids with a captured body |
| 54 | bodies carrying the collector line above |
| 1 | `347_20261101_143931_KP`, still the only body without it |
| 1 | body carrying the `+0x304` oops and its registers (`387`) |
| 54 | bodies without a `+0x304` fault block |

`06-capture-log.tsv` records the file behind each id, `07-kp-scan.tsv` the per-id
scan, and `SHA256SUMS` covers every body. `398` was captured a second time during
the sweep and reproduced `dfe04446…7671ff3` byte-for-byte; the duplicate was
discarded so that one id has one body.

The sweep ran one `dumpsys dropbox --print <id>` per id. Run inline with an empty
variable, the same command prints the whole 9 MB database and looks like a per-id
capture; the recorded bodies all contain `Searching for: <id>`, and the capture
log keeps that check.

## 6. The one body with fault-time registers

`387_20261102_134935_KP` (uploaded 11-02 13:49:35, kernel uptime 20,943.64 s)
contains the kernel's own oops, not only the bootloader summary. Lines 1336–1366
are preserved verbatim as `08-kp-387-fault-context.txt`:

| quantity | value | how |
| --- | --- | --- |
| `pc` | `rt_mutex_adjust_prio_chain+0x304/0x858` | read |
| `lr` | `rt_mutex_adjust_prio_chain+0x2d4/0x858` | read |
| task | `Comm: id`, pid 14697, CPU 1 | read; the PID digit in the `CPU:` line is corrupted, 14697 comes from the log prefixes |
| `pstate` | `0x00400085` (`+PAN`, `-UAO`) | read |
| `ESR` | `0x96000005` — DABT current EL, translation fault level 1, read | read; the `ESR` label is corrupted, the value and the `DABT` line are clean |
| `x8` | `0xffffffffffffffff` | read |
| word stored at `sp` | `0xffffffffffffffff` | read from the stack dump at `0xffffff80283f3cb0` |
| effective address | `0x37` | derived: `x8 + 0x38` wraps to `0x37`, consistent with the `user pgtable` line and a level-1 translation fault |
| `x21` | not readable | the register line is corrupted in this body |

The stack word is independent corroboration: `+0x300` stores `x8` at `sp` before
the faulting load, and the dumped stack holds the same all-ones value at that
address. The fault is a translation fault on `0x37`, not a permission fault, and
not a dereference of the planted `0x41` fill.

## 7. What the sweep changes, and what it does not

Changed:

- The negative "no faulting-CPU register context exists in any recovery artifact"
  is **retracted** as R7. It was true of the four bodies read before this sweep
  and false of the 55 now captured.
- `register-dataflow.md` §7's F1a (`0x41` fill) and F1c (user pointer under PAN)
  are excluded for the one measured panic; a measured family enters that table.
- Row 41 records the measured register set.

Not changed:

- None of `399`, `400`, `401` carries a `+0x304` fault block. For the three *known
  losses* the registers remain unavailable; `387` is an 11-02 upload with no saved
  host run, and its values may not be transferred to them.
- `397` (11-03 07:44) does contain an oops in `sh` at a *different* PC, whose symbol
  line is corrupted. It is not this fault and is preserved but not analysed here;
  it is why the sweep counts `+0x304` fault blocks rather than "fault blocks".
- `x21` is unreadable even in `387`, so which lock was walked, and therefore F2,
  remains open.
- No punch return, route completion, delivery or root; occupancy and the 31
  identities remain unestablished.
