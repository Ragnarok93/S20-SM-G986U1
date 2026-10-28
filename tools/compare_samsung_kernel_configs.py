#!/usr/bin/env python3
"""Compare stock Samsung config with a source-built .config for device compatibility."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STOCK = Path.home() / "firmware-check/kernel.config"
DEFAULT_CANDIDATE = Path.home() / "s20_kernel_chxl1/out-stock-layout/.config"

# A delta in these groups can invalidate assumptions used by architecture,
# task/futex, symbol-inspection, or security compatibility code.
CRITICAL_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("soc-platform", re.compile(
        r"^CONFIG_(?:ARCH_QCOM|ARCH_MSM.*|MSM.*|QCOM.*|ARCH_EXYNOS.*|EXYNOS.*)$"
    )),
    ("arm64-memory-layout", re.compile(
        r"^CONFIG_(?:ARM64_VA_BITS(?:_39|_52)?|ARM64_PA_BITS(?:_.*)?|ARM64_4K_PAGES|ARM64_16K_PAGES|ARM64_64K_PAGES|"
        r"PHYS_ADDR_T_64BIT|RELOCATABLE|RANDOMIZE_BASE|RANDOMIZE_MODULE_REGION_FULL|"
        r"SPARSEMEM|SPARSEMEM_VMEMMAP|FLATMEM|DISCONTIGMEM|NUMA|MEMORY_HOTPLUG|MEMORY_HOTREMOVE|"
        r"ZONE_DMA|ZONE_DMA32|HIGHMEM|KASAN|DEBUG_VIRTUAL)$"
    )),
    ("futex-scheduler", re.compile(
        r"^CONFIG_(?:FUTEX(?:_PI)?|RT_MUTEXES|PREEMPT(?:_.*)?|HAVE_PREEMPT.*|HZ|"
        r"ARCH_HAS_PREEMPT_LAZY|NO_HZ(?:_.*)?|HZ(?:_.*)?)$"
    )),
    ("symbols-debug-metadata", re.compile(
        r"^CONFIG_(?:KALLSYMS(?:_ALL|_BASE_RELATIVE|_ABSOLUTE_PERCPU)?|IKCONFIG(?:_PROC)?|"
        r"DEBUG_INFO(?:_.*)?|DEBUG_KERNEL|FRAME_POINTER|UNWINDER_FRAME_POINTER|"
        r"STACKTRACE|VMLINUX_SYMBOLS)$"
    )),
    ("security-hardening", re.compile(
        r"^CONFIG_(?:SECURITY(?:_.*)?|SELINUX(?:_.*)?|RKP|KDP|DEFEX|SECURITY_DEFEX|"
        r"PROCA|CFP|FIVE|KNOX(?:_.*)?|SEC_(?:RKP|KDP|DEFEX|PROCA|CFP|KNOX|RESTRICT.*|.*(?:RKP|KDP|DEFEX|PROCA|CFP|ROOT|SECURITY).*)|HARDENED_USERCOPY|FORTIFY_SOURCE|"
        r"BUG_ON_DATA_CORRUPTION|STRICT_KERNEL_RWX|STRICT_MODULE_RWX|"
        r"ARM64_SW_TTBR0_PAN|PANIC_ON_OOPS|SLAB_FREELIST_HARDENED|SLAB_FREELIST_RANDOM)$"
    )),
)

DEVICE_SELECTION_PATTERN = re.compile(
    r"^CONFIG_(?:MACH_.*|BOARD_.*|SOC_.*|VENDOR_.*|BUILD_.*|PROJECT_.*)$"
)


def parse_config(path: Path) -> dict[str, str]:
    """Parse enabled, valued and explicitly disabled symbols; absent remains absent."""
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip()
        else:
            match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line.strip())
            if match:
                values[match.group(1)] = "n"
    return values


def classify_symbol(symbol: str) -> str:
    for group, pattern in CRITICAL_PATTERNS:
        if pattern.fullmatch(symbol):
            return group
    if DEVICE_SELECTION_PATTERN.fullmatch(symbol):
        return "device-selection"
    return "other"


def compare_configs(stock: dict[str, str], candidate: dict[str, str]) -> dict[str, object]:
    """Compare configs conservatively; absent keys never silently mean disabled."""
    shared = set(stock) & set(candidate)
    union = set(stock) | set(candidate)
    differences = sorted(key for key in shared if stock[key] != candidate[key])
    stock_only = sorted(set(stock) - set(candidate))
    candidate_only = sorted(set(candidate) - set(stock))

    critical_differences: list[tuple[str, str, str, str]] = []
    critical_unknowns: list[tuple[str, str]] = []
    noncritical_differences: list[tuple[str, str, str, str]] = []
    noncritical_unknowns: list[tuple[str, str]] = []

    for key in differences:
        item = (key, stock[key], candidate[key], classify_symbol(key))
        if item[3] in {name for name, _ in CRITICAL_PATTERNS}:
            critical_differences.append(item)
        else:
            noncritical_differences.append(item)

    for key in stock_only:
        item = (key, "stock-only")
        (critical_unknowns if classify_symbol(key) in {name for name, _ in CRITICAL_PATTERNS} else noncritical_unknowns).append(item)
    for key in candidate_only:
        item = (key, "candidate-only")
        (critical_unknowns if classify_symbol(key) in {name for name, _ in CRITICAL_PATTERNS} else noncritical_unknowns).append(item)

    return {
        "stock_count": len(stock),
        "candidate_count": len(candidate),
        "shared_count": len(shared),
        "stock_only": stock_only,
        "candidate_only": candidate_only,
        "differences": differences,
        "critical_differences": critical_differences,
        "critical_unknowns": sorted(critical_unknowns),
        "noncritical_differences": noncritical_differences,
        "noncritical_unknowns": sorted(noncritical_unknowns),
        "symbol_count": len(union),
    }


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-config", type=Path, default=DEFAULT_STOCK,
                        help="config extracted from the exact stock firmware")
    parser.add_argument("--candidate-config", type=Path, default=DEFAULT_CANDIDATE,
                        help="source-built .config being assessed")
    parser.add_argument("--show-all-differences", action="store_true",
                        help="print every changed or one-sided symbol, not only critical differences")
    parser.add_argument("--strict-all", action="store_true",
                        help="return nonzero for any changed/one-sided config symbol")
    args = parser.parse_args(argv)

    for label, path in (("stock config", args.stock_config), ("candidate config", args.candidate_config)):
        if not path.is_file():
            print(f"ERROR: {label} not found: {path}", file=sys.stderr)
            return 2

    try:
        stock = parse_config(args.stock_config)
        candidate = parse_config(args.candidate_config)
    except OSError as exc:
        print(f"ERROR: unable to read config: {exc}", file=sys.stderr)
        return 2

    result = compare_configs(stock, candidate)
    print("== Samsung source-config compatibility comparison ==")
    print(f"Stock config:     {args.stock_config}")
    print(f"Candidate config: {args.candidate_config}")
    print(f"Symbols: stock={result['stock_count']} candidate={result['candidate_count']} shared={result['shared_count']}")
    print(f"Core-group deltas: {len(result['critical_differences'])}")
    print(f"Core-group unknowns: {len(result['critical_unknowns'])}")
    print(f"Other value deltas: {len(result['noncritical_differences'])}")
    print(f"Other one-sided symbols: {len(result['noncritical_unknowns'])}")

    if result["critical_differences"]:
        print("\n== Core-group value differences ==")
        for key, stock_value, candidate_value, group in result["critical_differences"]:
            print(f"DIFF [{group}] {key}: stock={stock_value} candidate={candidate_value}")
    if result["critical_unknowns"]:
        print("\n== Core-group symbols missing from one config (state unknown) ==")
        for key, side in result["critical_unknowns"]:
            print(f"UNKNOWN [{classify_symbol(key)}] {key}: {side}")

    if args.show_all_differences:
        print("\n== All shared-symbol value differences ==")
        for key, stock_value, candidate_value, group in result["noncritical_differences"]:
            print(f"DIFF [{group}] {key}: stock={stock_value} candidate={candidate_value}")
        print("\n== All one-sided symbols ==")
        for key, side in result["noncritical_unknowns"]:
            print(f"UNKNOWN [{classify_symbol(key)}] {key}: {side}")

    core_ok = not result["critical_differences"] and not result["critical_unknowns"]
    all_ok = not result["differences"] and not result["stock_only"] and not result["candidate_only"]
    if core_ok:
        if all_ok:
            print("\nRESULT: EXACT_CONFIG_MATCH")
        else:
            print("\nRESULT: CORE_CONFIG_COMPATIBLE; non-core config changes still require device/build review")
    else:
        print("\nRESULT: CORE_CONFIG_REVIEW_REQUIRED")
    if args.strict_all and not all_ok:
        return 1
    return 0 if core_ok else 1


if __name__ == "__main__":
    sys.exit(main())
