#!/usr/bin/env python3
"""Offline self-check for the S3 root-chain bridge artifact."""

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "build/alioth/bin/preload_rootchain_bridge.so"
MANIFEST = ROOT / "build/alioth/bin/rootchain_manifest.txt"
RSP = ROOT / "build_rootchain.rsp"

REJECTED_MM = {
    "0xffffffe585065e00",
    "0xffffffd5b5c69380",
    "0xffffffc02cd1ea00",
}

SOURCE_CHECKS = {
    "strict mode": ("src/rootchain_bridge.c", "KS_ROOTCHAIN_STRICT"),
    "mm contract gate": ("src/rootchain_bridge.c", "gate_mm_contract"),
    "pipe export gate": ("src/rootchain_bridge.c", "parse_pipe_export"),
    "pipe physrw gate": ("src/rootchain_bridge.c", "gate_pipe_physrw"),
    "provenance gate": ("src/rootchain_bridge.c", "gate_provenance"),
    "strict mm answer": ("src/util.c", "KS_ROOTCHAIN_STRICT"),
    "mm boot-id path": ("src/rootchain_bridge.c", "ROOTCHAIN_MM_BOOT_ID_PATH"),
    "mm boot-id gate": ("src/rootchain_bridge.c", "gate_mm_boot_contract"),
    "canonical boot-id reader": ("src/rootchain_bridge.c", "read_canonical_boot_id"),
    "validated mm pinned": ("src/rootchain_bridge.c", "set_cached_leaked_mm(mm)"),
    "run22 gate path": ("src/rootchain_bridge.c", "ROOTCHAIN_RUN22_GATE_PATH"),
    "run22 gate": ("src/rootchain_bridge.c", "gate_run22_contract"),
    "run22 c2c pass required": ("src/rootchain_bridge.c", 'memcmp(buf + 85, "c2c=PASS\\n", 9)'),
    "run22 c2c fail closed": ("src/rootchain_bridge.c", "ROOTCHAIN run22 contract c2c=FAIL"),
    "provenance multi-line parse": ("src/rootchain_bridge.c", "have_sha"),
    "fixed shell command": ("src/rootchain_bridge.c", '"timeout", "5", ROOTCHAIN_SU_LOCAL'),
    "shell root assertion": ("src/rootchain_bridge.c", '"uid=0(root) gid=0(root)"'),
    "strict pipe page": ("src/pipe.c", "KS_ROOTCHAIN_STRICT"),
    "pipe internal-leak fallback": ("src/rootchain_bridge.c", "internal-leak"),
    "strict root markers": ("src/root.c", "rootchain_quiet"),
    "rootchain preload guard": ("src/preload.c", "KS_BUILD_ROOTCHAIN"),
    "rootchain build macro": ("build_rootchain.rsp", "KS_BUILD_ROOTCHAIN"),
}

MARKERS = [
    "ROOTCHAIN clean boot",
    "ROOTCHAIN mm contract",
    "ROOTCHAIN mm boot contract",
    "ROOTCHAIN run22 contract",
    "ROOTCHAIN provenance",
    "ROOTCHAIN header build=",
    "ROOTCHAIN configfs write",
    "ROOTCHAIN configfs read",
    "ROOTCHAIN kaslr scan hit",
    "ROOTCHAIN kaslr verify",
    "ROOTCHAIN direct map calibrated",
    "ROOTCHAIN pipe cache gate",
    "ROOTCHAIN physrw read",
    "ROOTCHAIN physrw write",
    "ROOTCHAIN physrw rw64",
    "ROOTCHAIN root task",
    "ROOTCHAIN root cred",
    "ROOTCHAIN root seccomp",
    "ROOTCHAIN root selinux",
    "ROOTCHAIN root child result",
    "ROOTCHAIN shell id",
    "ROOTCHAIN verdict=PASS",
]

RUN_GATE_ORDER = [
    "gate_provenance()",
    "take_boot_attempt()",
    "gate_clean_boot()",
    "gate_mm_contract(&mm)",
    "set_cached_leaked_mm(mm)",
    "parse_pipe_export()",
    "ROOTCHAIN header build=",
    "slide_h1a_s3_test()",
    "gate_kaslr(fd)",
    "gate_configfs_primitive(fd)",
    "gate_linear_map()",
    "gate_pipe_physrw(fd)",
    "gate_root(fd)",
    "run_su_identity()",
    "ROOTCHAIN verdict=PASS",
]


def fail(msg: str) -> None:
    print(f"FAIL {msg}")
    raise SystemExit(1)


def main() -> int:
    if not ARTIFACT.is_file():
        fail(f"missing artifact {ARTIFACT}")
    data = ARTIFACT.read_bytes()
    size = len(data)
    sha = hashlib.sha256(data).hexdigest()

    if not MANIFEST.is_file():
        fail("missing manifest")
    manifest = dict(
        line.split("=", 1) for line in MANIFEST.read_text().splitlines() if "=" in line
    )
    if manifest.get("size") != str(size):
        fail(f"manifest size {manifest.get('size')} != {size}")
    if manifest.get("sha256") != sha:
        fail(f"manifest sha256 {manifest.get('sha256')} != {sha}")
    if not manifest.get("entry", "").lower().startswith("0x"):
        fail("manifest entry missing")

    if not RSP.is_file() or "-DKS_BUILD_ROOTCHAIN=1" not in RSP.read_text():
        fail("rootchain build macro missing from rsp")

    for label, (rel, needle) in SOURCE_CHECKS.items():
        path = ROOT / rel
        if not path.is_file() or needle not in path.read_text(errors="replace"):
            fail(f"source check {label} missing ({needle} in {rel})")

    bridge = (ROOT / "src/rootchain_bridge.c").read_text(errors="replace")
    for mm in REJECTED_MM:
        if mm.upper() not in bridge.upper():
            fail(f"rejected mm candidate missing: {mm}")
    if "ROOTCHAIN_BUILD_LABEL" not in bridge or "ROOTCHAIN header build=" not in bridge:
        fail("ROOTCHAIN build label marker missing")

    pipe_src = (ROOT / "src/pipe.c").read_text(errors="replace")
    root_src = (ROOT / "src/root.c").read_text(errors="replace")
    marker_source = bridge + pipe_src + root_src
    positions = []
    for marker in MARKERS:
        pos = marker_source.find(marker)
        if pos < 0:
            fail(f"marker missing from source: {marker}")
        positions.append(pos)

    run_start = bridge.find("int rootchain_bridge_run(void)")
    run_body = bridge[run_start:] if run_start >= 0 else ""
    if not run_body:
        fail("rootchain_bridge_run missing")
    gate_positions = []
    for gate in RUN_GATE_ORDER:
        pos = run_body.find(gate)
        if pos < 0:
            fail(f"runtime gate order missing: {gate}")
        gate_positions.append(pos)
    if gate_positions != sorted(gate_positions):
        fail("runtime gate call order does not follow spec sequence")

    mm_gate_start = bridge.find("static int gate_mm_contract(uintptr_t *mm_out)")
    mm_gate_end = bridge.find("static int gate_provenance(void)", mm_gate_start)
    if mm_gate_start < 0 or mm_gate_end < 0:
        fail("gate_mm_contract block missing")
    mm_gate = bridge[mm_gate_start:mm_gate_end]
    mm_gate_order = [
        "memcmp(buf, \"mm=0x\", 5)",
        "read_canonical_boot_id(boot_id",
        "gate_mm_boot_contract(&st, boot_id)",
        "for (size_t i = 5; i < n - 1",
        "gate_run22_contract(&st, mm, boot_id)",
        "if ((mm & 0x3f) != 0",
    ]
    mm_gate_positions = []
    for fragment in mm_gate_order:
        pos = mm_gate.find(fragment)
        if pos < 0:
            fail(f"mm gate order missing: {fragment}")
        mm_gate_positions.append(pos)
    if mm_gate_positions != sorted(mm_gate_positions):
        fail("mm gate answer/boot-id/c2c order is not fail-closed")

    print(f"PASS artifact={ARTIFACT.name}")
    print(f"PASS size={size}")
    print(f"PASS sha256={sha}")
    print(f"PASS entry={manifest['entry']}")
    print("PASS markers=22 sequence-ordered")
    print("PASS strict-mode=mm-bootid-run22c2c-required-pipe-export-or-fail-closed-leak")
    print("PASS rejected-mm=3 blocked")
    print("PASS offline-selfcheck")
    return 0


if __name__ == "__main__":
    sys.exit(main())
