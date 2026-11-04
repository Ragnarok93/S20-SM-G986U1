import pathlib
import sys

repo = pathlib.Path(__file__).resolve().parents[3]
doc = (repo / "docs/kernel-page/lifecycle-matrix.md").read_text().split("\n")

bad = []
block = []
for i, line in enumerate(doc + [""], 1):
    if line.startswith("|"):
        block.append((i, line.count("|")))
    else:
        if len({n for _, n in block}) > 1:
            bad.append([i for i, _ in block][0])
        block = []
print("ragged table blocks:", bad if bad else "none")

cites = [
    ("src/preload.c", 388, "__attribute__((constructor)) static void load(void)"),
    ("src/main.c", 349, "pthread_create(&waiter"),
    ("src/main.c", 350, "pthread_create(&owner"),
    ("src/main.c", 351, "pthread_create(&consumer"),
    ("src/main.c", 148, "atomic_store(&route_done, 1)"),
    ("src/main.c", 151, "while (!atomic_load(&owner_chain_done))"),
    ("src/main.c", 171, "atomic_store(&owner_chain_done, 1)"),
    ("src/main.c", 173, "for (;;)"),
    ("src/main.c", 204, "while (!atomic_load(&punch_consume_stop))"),
    ("src/main.c", 128, "atomic_store(&punch_consume_stop, 0)"),
    ("src/main.c", 323, "atomic_store(&punch_consume_stop, 0)"),
    ("src/fops.c", 232, "atomic_store(&punch_consume_stop, 0)"),
    ("src/main.c", 34, "pid_t parent_pid = getpid()"),
    ("src/main.c", 36, "if (pipe(pfd) != 0)"),
    ("src/main.c", 48, "struct pollfd p"),
    ("src/main.c", 52, "kill(parent_pid, SIGKILL)"),
    ("src/main.c", 68, "static void finish_punch_watchdog"),
    ("src/main.c", 72, "if (pfd >= 0)"),
    ("src/main.c", 79, "waitpid(pid, &status, 0)"),
    ("src/main.c", 346, "punch_watchdog_pid = start_punch_watchdog"),
    ("src/main.c", 361, "FUTEX_CMP_REQUEUE_PI"),
    ("src/main.c", 353, "while (!atomic_load(&waiter_waiting)"),
    ("src/main.c", 378, "while (!atomic_load(&route_done))"),
    ("src/main.c", 385, "main route done"),
    ("src/main.c", 477, "return 3;"),
    ("src/main.c", 490, "return 0;"),
    ("src/main.c", 505, "return 4;"),
    ("src/main.c", 520, "return 0;"),
    ("src/main.c", 525, "return 3;"),
    ("src/main.c", 539, "if (pipe_prepare_child > 0)"),
    ("src/main.c", 543, "sleep(5);"),
    ("src/main.c", 544, "return 0;"),
    ("src/main.c", 130, "atomic_store(&punch_consume_go, 1)"),
    ("src/main.c", 277, "atomic_store(&consumer_inflight, 1)"),
    ("src/main.c", 288, "if (sched_ret == 0)"),
    ("src/main.c", 298, "atomic_store(&consumer_inflight, 0)"),
    ("src/main.c", 303, "atomic_store(&punch_consume_go, 0)"),
    ("src/fops.c", 361, "atomic_store(&plant_ready, 1)"),
    ("src/fops.c", 355, "atomic_store(&punch_consume_go, 0)"),
    ("src/fops.c", 195, "if (route_attempt != 1 && !plant_probe)"),
    ("src/fops.c", 431, "for (volatile long spin"),
    ("src/fops.c", 452, "atomic_store(&punch_consume_go, 0)"),
    ("src/fops.c", 543, "if (cfi_last_step == 4)"),
    ("src/fops.c", 548, "if (route_verified || cfi_dirty_seen"),
    ("src/fops.c", 556, "pselect route done"),
    ("src/util.c", 1396, "static pid_t clone_dump_slot(size_t slot)"),
    ("src/util.c", 1407, "PR_SET_PDEATHSIG"),
    ("src/util.c", 1433, "ks = kernelsnitch_setup(MM_STRUCT_SZ"),
    ("src/util.c", 1488, "if (ks != NULL && (ks->state == KERNELSNITCH_MM_FOUND"),
    ("src/util.c", 1490, "(void)kernelsnitch_cleanup(ks);"),
    ("src/util.c", 1498, "for (;;)"),
    ("src/util.c", 1356, "PR_SET_PDEATHSIG"),
    ("src/util.c", 1380, "for (;;)"),
    ("src/util.c", 1783, "fake_lock = payload_base + LOCK_OFF"),
    ("src/util.c", 1790, "fake_parent = (data_addr(ASHMEM_MISC_FOPS) - 8) | 1;"),
    ("src/util.c", 4530, "uintptr_t prepare_good_kernel_page(int payload_mode)"),
    ("src/util.c", 2449, "static void release_ctx_children"),
    ("src/util.c", 1267, "PR_SET_PDEATHSIG"),
]

missing = []
for path, lineno, needle in cites:
    lines = (repo / path).read_text(errors="replace").split("\n")
    if lineno > len(lines) or needle not in lines[lineno - 1]:
        got = lines[lineno - 1] if lineno <= len(lines) else "<EOF>"
        missing.append((path, lineno, needle, got.strip()[:70]))
print("bad citations:", len(missing))
for m in missing:
    print("  ", m)
sys.exit(1 if (bad or missing) else 0)
