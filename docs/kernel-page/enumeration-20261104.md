# Dropbox enumeration, 2026-11-04

Scope: read-only enumeration of `dumpsys dropbox`, plus a capture of the one
`_KP` entry the recovery session did not keep. No payload run, no reboot, no
change to the device.

## 1. The complete listing

```sh
adb shell dumpsys dropbox | grep -a LAST_KMSG          # 118 lines
adb shell dumpsys dropbox --print <id> | grep panic_msg  # once per _KP id
```

is preserved as `logs/20261104_enumeration/` (`01` listing, `04` per-id
inventory, `05` id list, `02` a capture). The listing holds **55 distinct `_KP`
ids**, 23 written on 2026-11-01, 24 on 11-02 and 8 on 11-03. Each id appears
twice in the listing with two compressed sizes; `--print` returns one text
capture of about 132 KB.

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

## 5. What does not change

No `FAR`, `ESR`, `x8`, `x21`, backtrace or faulting-CPU register block exists in
any capture read so far. No punch return, no route completion, no delivery and no
root has been observed. Occupancy and the 31 object identities remain
unestablished.

Four captures (398–401) were read in full; the other 51 entries were inventoried
by id and `panic_msg` only, not captured. Their bodies remain unread.
