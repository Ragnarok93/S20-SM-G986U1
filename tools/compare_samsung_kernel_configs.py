#!/usr/bin/env python3
"""Compare stock Samsung config with a source-built .config for device compatibility."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"
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
    r"^CONFIG_(?:MACH_.*|BOARD_.*|SOC_.*|VENDOR_.*|BUILD_.*|PROJECT_.*|SEC_.*_PROJECT)$"
)


def load_profile(path: Path) -> dict[str, Any]:
    profile = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(profile, dict) or profile.get("schema_version") != 1:
        raise ValueError("unsupported or malformed profile (expected schema_version=1)")
    config = profile.get("kernel_config")
    if not isinstance(config, dict):
        raise ValueError("profile missing kernel_config object")
    selectors = config.get("target_selectors")
    if not isinstance(selectors, dict) or not selectors:
        raise ValueError("profile kernel_config.target_selectors must be a non-empty object")
    for key, expected in selectors.items():
        if not isinstance(key, str) or not key.startswith("CONFIG_") or not isinstance(expected, str):
            raise ValueError("target_selectors must map CONFIG_* names to string values")
    return profile


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
    # Samsung's CONFIG_SEC_<DEVICE>_PROJECT symbols select variants, while
    # SEC_RKP/SEC_DEFEX-style symbols represent security functionality.
    if re.fullmatch(r"CONFIG_SEC_[A-Za-z0-9_]+_PROJECT", symbol):
        return "device-selection"
    for group, pattern in CRITICAL_PATTERNS:
        if pattern.fullmatch(symbol):
            return group
    if DEVICE_SELECTION_PATTERN.fullmatch(symbol):
        return "device-selection"
    return "other"


def device_selector_symbols(stock: dict[str, str], candidate: dict[str, str]) -> list[str]:
    """Return Samsung product/variant selectors present in either config."""
    pattern = re.compile(r"^CONFIG_(?:SEC_[A-Za-z0-9_]+_PROJECT|MACH_(?:X1Q|Y2Q|Z3Q)_[A-Za-z0-9_]+)$")
    return sorted(key for key in (set(stock) | set(candidate)) if pattern.fullmatch(key))


def evaluate_target_selectors(
    expected: dict[str, str], stock: dict[str, str], candidate: dict[str, str]
) -> list[tuple[str, str, str, str, bool]]:
    """Verify the active target-selection symbols in both stock and candidate configs."""
    checks: list[tuple[str, str, str, str, bool]] = []
    for key, wanted in sorted(expected.items()):
        stock_value = stock.get(key, "<absent>")
        candidate_value = candidate.get(key, "<absent>")
        checks.append((key, wanted, stock_value, candidate_value,
                       stock_value == wanted and candidate_value == wanted))
    return checks


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

    critical_groups = {name for name, _ in CRITICAL_PATTERNS}
    for key in stock_only:
        item = (key, "stock-only")
        (critical_unknowns if classify_symbol(key) in critical_groups else noncritical_unknowns).append(item)
    for key in candidate_only:
        item = (key, "candidate-only")
        (critical_unknowns if classify_symbol(key) in critical_groups else noncritical_unknowns).append(item)

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
        "stock_only_values": {key: stock[key] for key in stock_only},
        "candidate_only_values": {key: candidate[key] for key in candidate_only},
        "symbol_count": len(union),
    }


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE,
                        help="machine-readable device compatibility profile")
    parser.add_argument("--stock-config", type=Path, default=DEFAULT_STOCK,
                        help="config extracted from the exact stock firmware")
    parser.add_argument("--candidate-config", type=Path, default=DEFAULT_CANDIDATE,
                        help="source-built .config being assessed")
    parser.add_argument("--show-all-differences", action="store_true",
                        help="print every changed or one-sided symbol, not only critical differences")
    parser.add_argument("--strict-all", action="store_true",
                        help="return nonzero for any changed/one-sided config symbol")
    args = parser.parse_args(argv)

    try:
        profile = load_profile(args.profile)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: device profile metadata: {exc}", file=sys.stderr)
        return 2
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
    selectors = device_selector_symbols(stock, candidate)
    target_checks = evaluate_target_selectors(profile["kernel_config"]["target_selectors"], stock, candidate)
    target_ok = all(check[4] for check in target_checks)

    device = profile["device"]
    print(f"== Samsung source-config compatibility comparison: {profile['profile_id']} ==")
    print(f"Target: {device['model']} / {device['codename']} / {device['firmware_build']}")
    print(f"Stock config:     {args.stock_config}")
    print(f"Candidate config: {args.candidate_config}")
    print(f"Symbols: stock={result['stock_count']} candidate={result['candidate_count']} shared={result['shared_count']}")
    print(f"Core-group deltas: {len(result['critical_differences'])}")
    print(f"Core-group unknowns: {len(result['critical_unknowns'])}")
    print(f"Other value deltas: {len(result['noncritical_differences'])}")
    print(f"Other one-sided symbols: {len(result['noncritical_unknowns'])}")

    print("\n== Required target selectors ==")
    for key, wanted, stock_value, candidate_value, passed in target_checks:
        print(f"{'PASS' if passed else 'FAIL'} {key}: expected={wanted} stock={stock_value} candidate={candidate_value}")

    print("\n== Samsung product/variant selector values ==")
    if selectors:
        for key in selectors:
            stock_value = stock.get(key, "<absent>")
            candidate_value = candidate.get(key, "<absent>")
            print(f"SELECTOR {key}: stock={stock_value} candidate={candidate_value}")
    else:
        print("WARN no SEC_<PROJECT> or X1Q/Y2Q/Z3Q MACH selectors found")

    if result["critical_differences"]:
        print("\n== Core-group value differences ==")
        for key, stock_value, candidate_value, group in result["critical_differences"]:
            print(f"DIFF [{group}] {key}: stock={stock_value} candidate={candidate_value}")
    if result["critical_unknowns"]:
        print("\n== Core-group symbols missing from one config (state unknown) ==")
        for key, side in result["critical_unknowns"]:
            print(f"UNKNOWN [{classify_symbol(key)}] {key}: stock={stock.get(key, '<absent>')} candidate={candidate.get(key, '<absent>')} ({side})")

    if args.show_all_differences:
        print("\n== All shared-symbol value differences ==")
        for key, stock_value, candidate_value, group in result["noncritical_differences"]:
            print(f"DIFF [{group}] {key}: stock={stock_value} candidate={candidate_value}")
        print("\n== All one-sided symbols ==")
        for key, side in result["noncritical_unknowns"]:
            print(f"UNKNOWN [{classify_symbol(key)}] {key}: stock={stock.get(key, '<absent>')} candidate={candidate.get(key, '<absent>')} ({side})")

    core_ok = not result["critical_differences"] and not result["critical_unknowns"]
    all_ok = not result["differences"] and not result["stock_only"] and not result["candidate_only"]
    if not core_ok:
        print("\nRESULT: CORE_CONFIG_REVIEW_REQUIRED")
    elif not target_ok:
        print("\nRESULT: TARGET_SELECTOR_REVIEW_REQUIRED")
    elif all_ok:
        print("\nRESULT: EXACT_CONFIG_MATCH")
    else:
        print("\nRESULT: CORE_CONFIG_COMPATIBLE; non-core config changes still require device/build review")
    if args.strict_all and not all_ok:
        return 1
    return 0 if core_ok and target_ok else 1


if __name__ == "__main__":
    sys.exit(main())
