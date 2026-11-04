#!/usr/bin/env python3
"""Recompute every claim in docs/kernel-page/symbol-resolution.md.

Fails loudly if the image, the symbol, the size agreement, the instruction bytes,
the source lines or the document's own quoted values stop matching. The firmware
artifacts live outside the repository; pass them with --fw and --ksrc.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

BASE = 0xFFFFFF8008080000
SYM = 0xFFFFFF8008146CA4
SIZE = 0x858
FAULT = SYM + 0x304
TRAP = SYM + 0x838
KERNEL_TEXT_SIZE = 0x2695260
BUILD_STRING = (
    "Linux version 4.19.113-27095354 (dpi@21DJ7C20) "
    "(clang version 10.0.6 for Android NDK) #1 SMP PREEMPT "
    "Fri Dec 27 14:53:28 KST 2024"
)
HASHES = {
    "kernel.bin": "ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123",
    "boot.img.lz4": "02099ccbfb423292b631f228b8d3163b9a601f4a058360a4bca93f71f8e50a78",
    "boot.img": "bd8987c28ef0b9fdb56ad3e82ce3b45929c5e53717525769e5625dd1e17f7dc8",
    "vmlinux.elf": "531444e3c98f8f7c4d2baefeda1386aca2b60a42030fcb1f4393c5e10b9154b4",
    "kernel.config": "ec6da4cfe412f7d0669c7d621751753375e32197a2602f98627eb95e71b9c0b2",
}

ap = argparse.ArgumentParser()
ap.add_argument("--fw", required=True)
ap.add_argument("--ksrc", required=True)
ap.add_argument("--repo", default=str(pathlib.Path(__file__).resolve().parents[3]))
args = ap.parse_args()

fw = pathlib.Path(args.fw)
ksrc = pathlib.Path(args.ksrc)
repo = pathlib.Path(args.repo)
problems = []


def check(ok, label, detail=""):
    print(("ok   " if ok else "FAIL ") + label + (("  " + detail) if detail and not ok else ""))
    if not ok:
        problems.append(label)


def read(name):
    return (fw / name).read_bytes()


def sha(data):
    return hashlib.sha256(data).hexdigest()


# 1. artifact hashes
data = {}
for name, want in HASHES.items():
    data[name] = read(name)
    check(sha(data[name]) == want, "sha256 " + name, sha(data[name])[:16])

kb = data["kernel.bin"]
bi = data["boot.img"]
elf = data["vmlinux.elf"]

# 2. boot.img.lz4 decompresses to boot.img
out = subprocess.run(["lz4", "-dc", str(fw / "boot.img.lz4")], capture_output=True)
check(out.returncode == 0 and out.stdout == bi, "boot.img.lz4 -> boot.img")

# 3. kernel.bin is the Image at boot.img+0x1000, whole region identical
off = bi.find(kb[:0x1000])
check(off == 0x1000, "kernel.bin offset in boot.img", hex(off) if off >= 0 else "absent")
check(off >= 0 and bi[off:off + len(kb)] == kb, "kernel.bin region identical")
check(kb[0x38:0x3C] == b"ARMd", "arm64 Image magic", kb[0x38:0x3C].hex())
check(int.from_bytes(kb[8:16], "little") == 0x80000, "text_offset 0x80000")

# 4. the ELF's first segment is that same image
check(elf[0x40:0x40 + KERNEL_TEXT_SIZE] == kb[:KERNEL_TEXT_SIZE], "ELF .kernel == kernel.bin")
check(BUILD_STRING.encode() in kb, "build string inside the image")

# 5. symbol and the size agreement
nm = subprocess.run(["nm", "-n", "--defined-only", str(fw / "vmlinux.elf")],
                    capture_output=True, text=True).stdout
syms = {}
for line in nm.splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16)
check(syms.get("rt_mutex_adjust_prio_chain") == SYM, "symbol rt_mutex_adjust_prio_chain",
      hex(syms.get("rt_mutex_adjust_prio_chain", 0)))
check(syms.get("rt_mutex_init_waiter") == SYM + SIZE, "next symbol is 0x858 later",
      hex(syms.get("rt_mutex_init_waiter", 0)))

# 6. the instruction, from the image's own bytes and from objdump
def image_word(addr):
    return int.from_bytes(kb[addr - BASE:addr - BASE + 4], "little")

check(image_word(FAULT) == 0xF9401D08, "word at +0x304 is ldr x8,[x8,#56]", hex(image_word(FAULT)))
check(image_word(TRAP) == 0xD4210000, "word at +0x838 is brk #0x800", hex(image_word(TRAP)))
dis = subprocess.run(
    ["aarch64-linux-gnu-objdump", "-d",
     "--start-address=" + hex(FAULT), "--stop-address=" + hex(FAULT + 0x10),
     str(fw / "vmlinux.elf")],
    capture_output=True, text=True).stdout
check("ldr\tx8, [x8, #56]" in dis, "objdump at +0x304")
check(hex(FAULT)[2:] in dis, "objdump start address matches")

# 7. source lines
common = (ksrc / "kernel/locking/rtmutex_common.h").read_text().split("\n")
rt = (ksrc / "kernel/locking/rtmutex.c").read_text().split("\n")
check("rt_mutex_top_waiter(struct rt_mutex *lock)" in common[52], "rtmutex_common.h:53 signature",
      common[52])
check("BUG_ON(w->lock != lock);" in common[59], "rtmutex_common.h:60 BUG_ON", common[59])
check("prerequeue_top_waiter = rt_mutex_top_waiter(lock);" in rt[660],
      "rtmutex.c:661 prerequeue top waiter", rt[660])
check("rt_mutex_dequeue(lock, waiter);" in rt[663], "rtmutex.c:664 dequeue", rt[663])

# 8. the payload's own plant, which §4 quotes
util = (repo / "src/util.c").read_text().split("\n")
plant = [i for i, l in enumerate(util, 1)
         if "put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);" in l]
check(len(plant) >= 1, "util.c plants rb_leftmost = fake_w0")
comment = [i for i, l in enumerate(util, 1) if "top_waiter->lock (+0x38)" in l]
check(bool(comment), "util.c comment naming the +0x38 check")
check(any(0 < c < p <= c + 12 for c in comment for p in plant),
      "the comment introduces that plant", str((comment, plant)))

# 9. the documents
doc = (repo / "docs/kernel-page/symbol-resolution.md").read_text()
for needle in (
    "rt_mutex_adjust_prio_chain+0x304",
    "ffffff8008146ca4", "ffffff8008146fa8", "0x858",
    "f9401d08", "brk #0x800", "rtmutex_common.h:52-63", "rtmutex.c:661",
    "0xed0", "**met**", "**not met**",
):
    check(needle in doc, "doc quotes " + needle)

# 9b. the two planted words, recomputed from the target's own layout
th = (repo / "src/targets/samsung-g986u1-4.19.113/target.h").read_text()


def cval(name):
    m = re.search(r"^#define %s (0x[0-9A-Fa-f]+)" % name, th, re.M)
    return int(m.group(1), 16) if m else None


layout = (cval("LOCK_OFF"), cval("W0_OFF"),
          cval("RT_MUTEX_LEFTMOST_OFF"), cval("WAITER_LOCK_OFF"))
check(layout == (0x1350, 0x2220, 0x18, 0x38),
      "target.h keeps the measured layout",
      str([hex(v) if v is not None else "missing" for v in layout]))
if None not in layout:
    lock_off, w0_off, leftmost_off, waiter_lock_off = layout
    rb_leftmost = lock_off + leftmost_off       # the word at lock+0x18
    waiter_lock = w0_off + waiter_lock_off      # the word at waiter+0x38
    d_obj = w0_off - lock_off
    d_eff = waiter_lock - lock_off
    check((rb_leftmost, waiter_lock, d_obj, d_eff) ==
          (0x1368, 0x2258, 0xed0, 0xf08),
          "planted words are 0x1368/0x2258, objects 0xed0 apart, load at +0xf08",
          str([hex(v) for v in (rb_leftmost, waiter_lock, d_obj, d_eff)]))
    page = 0x8000
    check(all(0 <= a < page for a in
              (lock_off, w0_off, rb_leftmost, waiter_lock, waiter_lock + 8)),
          "both words and both dereferences stay inside the 0x8000 page")
    check("W0_OFF + %#x" % waiter_lock_off in util[1910],
          "util.c writes waiter->lock at the header's offset", util[1910])

    # 9c. the header's two field offsets against the image's own immediates.
    # Each side is checked separately so a self-consistent header cannot drift
    # away from the instruction the device actually faulted on.
    def ldr_uimm(word):
        if (word >> 22) & 0x3FF != 0x3E5:      # LDR (immediate), 64-bit, unsigned
            return None, None
        return (((word >> 10) & 0xFFF) * 8, (word >> 5) & 0x1F)

    imm_leftmost, rn_leftmost = ldr_uimm(image_word(SYM + 0x2F8))
    imm_lock, rn_lock = ldr_uimm(image_word(SYM + 0x304))
    check(imm_leftmost == leftmost_off,
          "image #24 equals RT_MUTEX_LEFTMOST_OFF (%#x)" % leftmost_off,
          "image %s" % (("#%d" % imm_leftmost) if imm_leftmost is not None else "not a load"))
    check(imm_lock == waiter_lock_off,
          "image #56 equals WAITER_LOCK_OFF (%#x)" % waiter_lock_off,
          "image %s" % (("#%d" % imm_lock) if imm_lock is not None else "not a load"))
    check(rn_leftmost == 21 and rn_lock == 8,
          "the two loads use x21 then x8 as their base",
          str((rn_leftmost, rn_lock)))
    for needle in ("0x1350", "0x2220", "0x1368", "0x2258", "0xed0"):
        check(needle in doc, "symbol-resolution quotes " + needle)
    for stale in ("0x1340", "0xee0", "0xf18"):
        check(stale not in doc, "symbol-resolution has no stale " + stale)
    check("not a mapping or retention claim" in doc,
          "symbol-resolution: same page is a layout property")
# 11. the new document's tables are not ragged
block, ragged = [], []
for line in doc.split("\n") + [""]:
    if line.startswith("|"):
        block.append(line.count("|"))
    else:
        if len(set(block)) > 1:
            ragged.append(block)
        block = []
check(not ragged, "symbol-resolution tables are rectangular", str(ragged[:2]))

rec = (repo / "docs/kernel-page/recovery-20261104.md").read_text()
check("**Resolved** in `symbol-resolution.md`" in rec, "recovery doc records the resolution")
led = (repo / "docs/kernel-page/evidence-ledger.md").read_text()
for row in ("| 36 |", "| 37 |", "| 38 |"):
    check(row in led, "ledger row " + row.strip("| "))
check("19. A symbol resolution must come from the image" in led, "ledger rule 19")

# 10. the data-flow document's instructions decode to the words it relies on
flow = (repo / "docs/kernel-page/register-dataflow.md").read_text()
words = {
    0x28: 0x2A0103F8, 0x2C: 0xAA0003F3, 0x40: 0xAA0303F5, 0x44: 0xAA0203F7,
    0x48: 0x5280003C, 0x50: 0xAA0403F6, 0x58: 0xF9000BE5, 0x5C: 0xF81F83A4,
    0x64: 0x9123227A, 0x70: 0xF9447E79, 0x74: 0xB40034F9, 0x8C: 0xF9401F28,
    0x90: 0xEB0802BF, 0x94: 0x540033E1, 0x1A8: 0xAA1503E0, 0x1AC: 0x94609971,
    0x1F8: 0x3700081C, 0x26C: 0xF9401D1A, 0x2F8: 0xF9400EA8, 0x304: 0xF9401D08,
    0x32C: 0x94604F53, 0x3EC: 0x94604ED1, 0x660: 0xAA1A03F5, 0x838: 0xD4210000,
}
for off, want in words.items():
    check(image_word(SYM + off) == want, "word at +%#x" % off, hex(image_word(SYM + off)))
for label in ("+0x40", "+0x8c", "+0x2f8", "+0x660", "F1a", "F1c", "F2", "+0xed0"):
    check(label in flow, "dataflow doc cites " + label)
for needle in ("0x1350", "0x2220", "0x1368", "0x2258", "0xed0", "0xf08"):
    check(needle in flow, "register-dataflow quotes " + needle)
for stale in ("0x1340", "0xee0", "0xf18"):
    check(stale not in flow, "register-dataflow has no stale " + stale)
for tier in ("image-verified", "header-derived", "runtime-unresolved"):
    check(tier in flow, "dataflow tier " + tier)
check("not a record that the CPU accessed it" in flow,
      "dataflow: effective address is conditional")
check("decodes those two immediates" in flow,
      "dataflow: header offsets are checked against the image")
for line, text in (
    (507, "waiter = task->pi_blocked_on;"),
    (518, "if (!waiter)"),
    (537, "if (next_lock != waiter->lock)"),
    (585, "if (!raw_spin_trylock(&lock->wait_lock)) {"),
    (613, "if (!requeue) {"),
    (661, "prerequeue_top_waiter = rt_mutex_top_waiter(lock);"),
    (759, "next_lock = task_blocked_on_lock(task);"),
):
    check(text in rt[line - 1], "rtmutex.c:%d" % line, rt[line - 1])
for line, text in (
    (1877, "put64(p, LOCK_OFF + RT_MUTEX_LEFTMOST_OFF, fake_w0);"),
    (1911, "put64(p, W0_OFF + 0x38, fake_lock);"),
    (1939, "getenv(\"KS_W1_ONLY_FIRST\") ? 0 : fake_w0);"),
):
    check(text in util[line - 1], "util.c:%d" % line, util[line - 1])

# 11. ledger table widths: every row in the claim table has 9 cells
bad = []
for line in led.split("\n"):
    if line.startswith("| ") and not line.startswith("| # |"):
        cells = line.count("|") - 1
        if cells not in (5, 8):
            bad.append((line[:40], cells))
check(not bad, "ledger tables keep their shape (8 or 5 cells)", str(bad[:3]))

check("| 39 |" in led, "ledger row 39")
check("21. Row 39 is a static constraint." in led, "ledger rule 21")
check("| 40 |" in led, "ledger row 40: the fourth panic")
check("R6" in led, "ledger retraction R6")
check("23. A device-record count is a claim" in led, "ledger rule 23: listings are exhaustive or absent")

# 9d. the enumeration's own artifacts and its arithmetic
enum = repo / "docs/kernel-page/enumeration-20261104.md"
check(enum.exists(), "enumeration doc exists")
et = enum.read_text() if enum.exists() else ""
for needle in ("55 distinct", "54", "347", "R6", "no saved run",
               "An upload is not a fault"):
    check(needle in et, "enumeration doc covers " + needle)
base = repo / "docs/kernel-page/logs/20261104_enumeration"
listing = base / "01-dropbox-last-kmsg-list.txt"
if listing.exists():
    ids = sorted({l.split("SYSTEM_LAST_KMSG_")[1].split("_KP")[0]
                  for l in listing.read_text(errors="replace").splitlines() if "_KP" in l})
    check(len(ids) == 55, "listing holds 55 distinct _KP ids", str(len(ids)))
    check(all(any(i.startswith(x + "_") for i in ids) for x in ("398", "399", "400", "401")),
          "the four captured ids are in the listing")
else:
    check(False, "listing preserved")
inv = base / "04-kp-inventory.tsv"
if inv.exists():
    rows = [l.split("\t") for l in inv.read_text(errors="replace").splitlines() if l]
    same = [r for r in rows if "rt_mutex_adjust_prio_chain+0x304/0x858" in (r[1] if len(r) > 1 else "")]
    blank = [r for r in rows if len(r) < 2 or not r[1]]
    check(len(rows) == 55, "inventory has one row per id", str(len(rows)))
    check(len(same) == 54, "54 entries carry the identical panic_msg", str(len(same)))
    check(len(blank) == 1 and blank[0][0].startswith("SYSTEM_LAST_KMSG_347_"),
          "only 347 lacks a panic_msg", str(blank[:1]))
    pre = [r for r in rows
           if r[0].split("_")[4] + "_" + r[0].split("_")[5] < "20261103_100942"]
    check(len(pre) == 52, "52 uploads precede the first saved log", str(len(pre)))
else:
    check(False, "inventory preserved")
kp = repo / "docs/kernel-page/logs/20261104_enumeration/02-kp-398_20261103_080145.txt"
if kp.exists():
    raw = kp.read_bytes()
    check(hashlib.sha256(raw).hexdigest().startswith("dfe04446"),
          "398 capture hash", hashlib.sha256(raw).hexdigest())
    txt = raw.decode("utf-8", "replace")
    check("rt_mutex_adjust_prio_chain+0x304/0x858" in txt, "398 reports the +0x304 PC")
    check("Unable to handle" not in txt, "398 holds no oops header")
else:
    check(False, "398 capture preserved")
check("22. Every address in the static documents is recomputed" in led,
      "ledger rule 22: addresses come from the constants")
check("Such an address is derived, never observed" in led,
      "ledger rule 22: derived, not observed")


# 13. every _KP id has a body, and the scan and hashes are recomputed from it
enum = repo / "docs/kernel-page/logs/20261104_enumeration"
ids = [l.strip() for l in (enum / "05-kp-ids.txt").read_text().splitlines() if l.strip()]
check(len(ids) == 55, "55 ids listed", str(len(ids)))

bodies = {}
for eid in ids:
    short = eid.replace("SYSTEM_LAST_KMSG_", "")
    cand = [enum / "bodies" / (short + ".txt"),
            enum / ("02-kp-" + short.replace("_KP", "") + ".txt")]
    found = [c for c in cand if c.exists()]
    check(len(found) == 1, "one body for " + short, str([str(c) for c in found]))
    if found:
        bodies[eid] = found[0]

check(len(bodies) == 55, "55 bodies present", str(len(bodies)))
scan = [l.split("\t") for l in (enum / "07-kp-scan.tsv").read_text().splitlines()]
check(scan[0][0] == "id" and len(scan[0]) == 12, "scan table header")
rows = scan[1:]
check(len(rows) == 55, "55 scan rows", str(len(rows)))

panic = fault = 0
blank = []
for eid, row in zip(ids, rows):
    check(row[0] == eid, "scan row matches id order")
    check(len(row) == 12, "scan row width " + eid, str(len(row)))
    path = bodies.get(eid)
    raw = path.read_bytes() if path else b""
    digest = hashlib.sha256(raw).hexdigest()
    check(row[3] == digest, "scan hash for " + eid)
    check(row[2] == str(len(raw)), "scan size for " + eid)
    txt = raw.decode("utf-8", "replace")
    want_panic = len(re.findall(re.escape("rt_mutex_adjust_prio_chain+0x304/0x858"), txt))
    want_pc = len(re.findall(r"pc\s*:\s*rt_mutex_adjust_prio_chain", txt))
    check(row[4] == str(want_panic), "panic line recount " + eid)
    check(row[5] == str(want_pc), "oops recount " + eid)
    verdict = "fault-register-context" if want_pc else "no-fault-register-context"
    check(row[10] == verdict, "verdict recount " + eid)
    sl = next((l for l in txt.split("\n") if "3$10" in l), "")
    sw = sl.split()
    si = sw.index("3$10") + 1 if "3$10" in sw else -1
    badc = [c for c in (sw[si + 2] + sw[si + 3]) if c not in "0123456789abcdefABCDEF"] if si >= 0 else []
    want_x = "slot-damaged" if want_pc and len(badc) == 3 else "-"
    check(row[11] == want_x, "x21 status " + eid)
    panic += 1 if want_panic else 0
    fault += 1 if want_pc else 0
    if not want_panic:
        blank.append(eid)
check(panic == 54, "54 bodies carry the collector line", str(panic))
check(blank == ["SYSTEM_LAST_KMSG_347_20261101_143931_KP"], "only 347 lacks it", str(blank))
check(fault == 1, "exactly one body has a fault block", str(fault))
flagged = [row[0] for row in rows if row[10] == "fault-register-context"]
check(flagged == ["SYSTEM_LAST_KMSG_387_20261102_134935_KP"],
      "the fault block is 387", str(flagged))

# the three losses still have no register block
for short in ("399_20261103_101957_KP", "400_20261103_190448_KP", "401_20261103_202100_KP"):
    body = bodies.get("SYSTEM_LAST_KMSG_" + short)
    if body is None:
        check(False, "loss body " + short)
        continue
    txt = body.read_text("utf-8", "replace")
    check("pc : rt_mutex_adjust_prio_chain" not in txt, "loss body " + short + " has no +0x304 oops")

# 14. every hash in SHA256SUMS is the hash of the file it names
sums = (enum / "SHA256SUMS").read_text().splitlines()
listed = {}
for line in sums:
    h, name = line.split("  ", 1)
    listed[name] = h
check(len(sums) == 63, "SHA256SUMS covers 9 numbered files and 54 bodies", str(len(sums)))
for name, want in listed.items():
    target = enum / name
    if not target.exists():
        check(False, "hash target missing: " + name)
        continue
    check(hashlib.sha256(target.read_bytes()).hexdigest() == want, "hash matches " + name)
for eid, path in bodies.items():
    rel = str(path.relative_to(enum))
    check(rel in listed, "body hashed: " + rel)
for name in ("06-capture-log.tsv", "07-kp-scan.tsv", "08-kp-387-fault-context.txt",
             "09-kp-387-x21-recovery.md"):
    check(name in listed, "artifact hashed: " + name)

# 15. the extract is exactly the oops block of 387, and the values are in it
body387 = bodies["SYSTEM_LAST_KMSG_387_20261102_134935_KP"]
blines = body387.read_text("utf-8", "replace").split("\n")
want_block = "\n".join(blines[1335:1366]) + "\n"
extract = (enum / "08-kp-387-fault-context.txt").read_text()
check(extract == want_block, "extract equals lines 1336-1366 of 387")
for needle in ("rt_mutex_adjust_prio_chain+0x304/0x858",
               "x8 : ffffffffffffffff",
               "0x96000005",
               "PID: 14696",
               "id"):
    check(needle in extract, "extract carries " + needle)

# 16. the documents state the recomputed result, and not the retracted one
edoc = (repo / "docs/kernel-page/enumeration-20261104.md").read_text()
flowdoc = (repo / "docs/kernel-page/register-dataflow.md").read_text()
for needle in ("55 | ids with a captured body",
               "54 | bodies carrying the collector line above",
               "body carrying the `+0x304` oops and its registers (`387`)",
               "0xffffffffffffffff",
               "0x96000005",
               "x21` | no readable register; the frame slot at `sp+0x68` is printed, but its word is damaged",
               "`x21` has no readable register even in `387`"):
    check(needle in edoc, "enumeration states: " + needle)
check("No `FAR`, `ESR`, `x8`, `x21`, backtrace or faulting-CPU register block exists" not in edoc,
      "retracted negative is gone")
check("## 11. The one measured register set" in flowdoc, "dataflow §11 present")
check("| F3 (measured once) |" in flowdoc, "dataflow F3 row present")
check("| 41 |" in led, "ledger row 41")
check("| R7 |" in led and "55-body sweep" in led, "retraction R7")
check("24. Row 41 is one upload, not one of the lost runs." in led, "ledger rule 24")



# 17. the x21 record: tolerant parse, every other copy, and the scope
bl = body387.read_text("utf-8", "replace").split("\n")
slot = next(l for l in bl if "3$10" in l)
tok = slot.split()
si = tok.index("3$10") + 1
w = tok[si:si + 8]
check(len(w) == 8, "3d10 row has eight 32-bit words", str(w))
bad = [c for c in w[2] + w[3] if c not in "0123456789abcdefABCDEF"]
check(len(bad) == 3, "three characters in the x21 slot are not hex", repr(bad))
check(w[4] == "1159D508" and w[5] == "ffffffc0", "3d20 is the caller's pi_lock", str(w[4:6]))
check(w[6] == "1159cc40" and w[7].startswith("ffffffc0"), "3d28 is the caller's task", str(w[6:8]))
check(not any(re.search(r"\bx21\s*:", l, re.I) for l in bl[1335:1366]),
      "no x21 label in the fault block")
tail = [(n, l) for n, l in enumerate(bl[1366:], 1367) if re.search(r"\bx21\s*:", l, re.I)]
check([n for n, _ in tail] == [1524, 1748, 1905], "three x21 labels after the block",
      str([n for n, _ in tail]))
check(all(re.search(r"apper/\d", l, re.I) for _, l in tail), "all three are swapper blocks")
check(not any("Comm: id" in l for l in bl[1366:]), "no Comm: id block after the fault")
check(not any("FAR" in l for l in bl), "no FAR label in the body")
check("sec_debug_panic_handler" not in body387.read_text("utf-8", "replace"),
      "no panic-handler block in the body")
check([n for n, l in enumerate(bl, 1) if re.search(r"pc\s*:", l)] == [1352, 1683],
      "only the fault and one idle pc line")

xdoc = (enum / "09-kp-387-x21-recovery.md").read_text()
for needle in ("## 5. The post-fault region", "## 6. Result",
               "| 1524 | `Comm: swapper/4`, CPU 4 |",
               "| 1748 | `Comm: swapper/7`, CPU 7 |",
               "| 1905 | `Comm: swapper/5`, CPU 5 |",
               "lpm_cpuidle_enter+0x528/0x570", "no `FAR` label",
               "the only other `x21` labels are idle-task saves"):
    check(needle in xdoc, "x21 extract states: " + needle)
check("Scope:" in xdoc and "`399`, `400` or `401`" in xdoc, "x21 extract scope")
symdoc = (repo / "docs/kernel-page/symbol-resolution.md").read_text()
recdoc = (repo / "docs/kernel-page/recovery-20261104.md").read_text()
for name, doc in (("enumeration", edoc), ("dataflow", flowdoc),
                  ("symbol", symdoc), ("recovery", recdoc)):
    check("387" in doc and "not one of" in doc, "scope names 387 in " + name)
check("no `x21` is readable even there" in led, "ledger rule 24 wording")
check("`x21` register print lost, its one frame copy damaged" in led, "ledger row 41 x21")

print("\nproblems:", len(problems))
for p in problems:
    print("  ", p)
sys.exit(1 if problems else 0)
