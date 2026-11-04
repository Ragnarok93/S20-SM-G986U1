import argparse
import pathlib
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--ksrc", required=True)
args = ap.parse_args()

repo = pathlib.Path(__file__).resolve().parents[3]
doc = (repo / "docs/kernel-page/causal-timeline.md").read_text().split("\n")
logs = sorted((repo / "docs/kernel-page/logs").glob("*.log"))

problems = []

# 1. Table shape per block.
block = []
for i, line in enumerate(doc + [""], 1):
    if line.startswith("|"):
        block.append((i, line.count("|")))
    else:
        if len({n for _, n in block}) > 1:
            problems.append(("ragged table starting line %d" % block[0][0]))
        block = []

# 2. Run rows: 16, one per log.
runs = [l for l in doc if re.match(r"^\| \d\d:\d\d:\d\d \|", l)]
if len(runs) != len(logs):
    problems.append(("run rows", len(runs), "logs", len(logs)))

# 3. Counts recomputed from the logs.
lost = alive = gate = control = punchskip = plant = punch = complete = 0
for f in logs:
    text = f.read_text(errors="replace")
    lost += "DEVICE=LOST" in text
    alive += "DEVICE=ALIVE" in text
    gate += "KS_DELIVERY skipped" in text
    control += "KS_CONTROL slots=" in text and "payload_exit=124" in text
    punchskip += "KS_PUNCH_SKIPPED" in text
    plant += "waiter futex returned" in text
    punch += "consumer punch enter" in text
    complete += ("consumer punch done" in text or "main route done" in text
                 or "pipe-physrw-summary" in text)

expect = {"lost": 3, "alive": 13, "gate": 11, "control": 1, "punchskip": 1,
          "plant": 3, "punch": 1, "complete": 0}
got = {"lost": lost, "alive": alive, "gate": gate, "control": control,
       "punchskip": punchskip, "plant": plant, "punch": punch,
       "complete": complete}
for k, v in expect.items():
    if got[k] != v:
        problems.append((k, got[k], "expected", v))

# 4. The doc's count table agrees with the logs.
for label, value in [
    ("gate-refused, device alive", gate),
    ("control cycle, harness timeout", control),
    ("dumps completed, punch skipped", punchskip),
    ("plant stage entered", plant),
    ("punch entered", punch),
]:
    row = next((l for l in doc if l.startswith("| " + label)), None)
    if row is None:
        problems.append(("missing count row", label))
    elif "| %d " % value not in row:
        problems.append(("count mismatch", label, row))
if "| **punch returned** (`consumer punch done`) | **0** |" not in doc:
    problems.append("punch-return row missing")

# 5. Citations carry the claimed code.
cites = [
    ("src/fops.c", 361, "atomic_store(&plant_ready, 1)"),
    ("src/slide.c", 162, "void plant_shallow(void)"),
    ("src/slide.c", 216, "void plant_waiter_fake(uint64_t task"),
    ("kernel/locking/rtmutex.c", 1139, "next_lock = waiter->lock;"),
    ("kernel/locking/rtmutex.c", 585, "raw_spin_trylock(&lock->wait_lock)"),
    ("kernel/locking/rtmutex.c", 661, "prerequeue_top_waiter = rt_mutex_top_waiter(lock);"),
    ("src/util.c", 1790, "fake_parent = (data_addr(ASHMEM_MISC_FOPS) - 8) | 1;"),
    ("src/main.c", 283, "consumer punch first ret"),
    ("src/main.c", 148, "atomic_store(&route_done, 1)"),
    ("src/main.c", 385, "main route done"),
    ("src/main.c", 528, "pipe-physrw-summary"),
    ("src/main.c", 543, "sleep(5);"),
]
kroot = pathlib.Path(args.ksrc)
for path, lineno, needle in cites:
    base = kroot if path.startswith("kernel/") else repo
    lines = (base / path).read_text(errors="replace").split("\n")
    if lineno > len(lines) or needle not in lines[lineno - 1]:
        problems.append(("citation", path, lineno, needle))

print("problems:", len(problems))
for p in problems:
    print("  ", p)
sys.exit(1 if problems else 0)
