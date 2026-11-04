"""Verify failure-boundary.md against the pinned source and the 16 recorded logs.

Checks three things:
  1. every load-bearing citation points at the line it claims;
  2. the boot chain the document reports is what the logs contain;
  3. the document's tables are well formed.
Exit status is nonzero on any mismatch.
"""
import pathlib
import re
import sys

repo = pathlib.Path(__file__).resolve().parents[3]
doc = repo / "docs/kernel-page/failure-boundary.md"
logs = repo / "docs/kernel-page/logs"

CITES = [
    ("src/main.c", 111, "waiter futex returned"),
    ("src/main.c", 279, "consumer punch enter"),
    ("src/main.c", 283, "consumer punch first ret"),
    ("src/main.c", 385, "main route done"),
    ("src/slide.c", 74, "int plant_waiter_install"),
    ("src/slide.c", 80, "sigaction"),
    ("src/slide.c", 83, "gettid"),
    ("src/slide.c", 84, "getpid"),
    ("src/slide.c", 162, "void plant_shallow"),
    ("src/slide.c", 191, "KS_PLANT_SHALLOW value"),
    ("src/util.c", 790, "long sched_setattr_tid"),
    ("src/util.c", 796, "SYS_sched_setattr"),
    ("src/util.c", 799, "sched_setattr tid="),
    ("src/util.c", 4801, "/data/local/tmp/ks_log.txt"),
    ("src/util.c", 4802, "O_WRONLY"),
    ("src/util.c", 4837, "void exlog_writef"),
    ("src/util.c", 4845, "fprintf(stdout"),
    ("src/util.c", 4846, "fflush(stdout)"),
    ("src/util.c", 4863, "write(g_exlog_fd"),
    ("src/util.c", 4865, "fsync(g_exlog_fd"),
]

problems = []

for rel, line, needle in CITES:
    path = repo / rel
    lines = path.read_text(errors="replace").split("\n")
    if line > len(lines):
        problems.append(f"{rel}:{line}: file has only {len(lines)} lines")
        continue
    if needle not in lines[line - 1]:
        problems.append(f"{rel}:{line}: expected {needle!r}, got {lines[line-1].strip()!r}")

text = doc.read_text()
runs = sorted(logs.glob("*.log"))

# 1. Every run wrote the crash-surviving log.
exlog = [f for f in runs if "exlog open path=/data/local/tmp/ks_log.txt" in f.read_text(errors="replace")]
if len(exlog) != len(runs):
    problems.append(f"exlog open in {len(exlog)}/{len(runs)} logs")

# 2. The on-device log was never retrieved: no [pid][level] line in any host log.
pulled = [f.name for f in runs if re.search(r"^\[\d+\]\[", f.read_text(errors="replace"), re.M)]
if pulled:
    problems.append(f"ks_log formatting found in host logs: {pulled}")

# 3. Boot chain: one shell reboot before the first loser, then a panic boot.
reasons = {}
for f in runs:
    for line in f.read_text(errors="replace").split("\n"):
        if line.startswith("bootreason_before="):
            reasons[f.name] = line.split("=", 1)[1].strip()
            break

shell_boots = [n for n, r in reasons.items() if r == "reboot,shell"]
panic_boots = [n for n, r in reasons.items() if r == "kernel_panic,null"]
if len(shell_boots) != 1 or shell_boots[0] != "20261103_100942.log":
    problems.append(f"expected exactly 10:09 on a shell reboot, got {shell_boots}")
if len(panic_boots) != 15:
    problems.append(f"expected 15 panic-boot starts, got {len(panic_boots)}")
if reasons.get("20261103_105018.log") != "kernel_panic,null":
    problems.append("the run after 10:09 is not on a panic boot")

# 4. The document states those numbers.
for phrase in ("`reboot,shell`", "`kernel_panic,null`"):
    if phrase not in text:
        problems.append(f"document does not mention {phrase}")
if "10:50:18 … 18:46:22 (14 runs)" not in text:
    problems.append("document does not carry the 14-run panic-boot row")

# 5. Table shape: every row of a contiguous table block uses the header's pipe count.
blocks, cur = [], []
for line in text.split("\n"):
    if line.startswith("|"):
        cur.append(line)
    else:
        if cur:
            blocks.append(cur)
        cur = []
if cur:
    blocks.append(cur)
for block in blocks:
    width = block[0].count("|")
    for line in block[1:]:
        if line.count("|") != width:
            problems.append(f"ragged table row: {line[:70]}")

if problems:
    for p in problems:
        print("FAIL:", p)
    sys.exit(1)

print(f"boundary ok: {len(CITES)} citations, {len(runs)} logs, "
      f"{len(panic_boots)} panic-boot starts, {len(blocks)} tables")
