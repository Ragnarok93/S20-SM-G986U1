#!/usr/bin/env python3
"""Read-only KernelSU integration preflight for the Samsung SM-G986U1 kernel."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "src/targets/samsung-g986u1-4.19.113/kernelsu-port-profile.json"


def load_profile(path: Path) -> dict[str, Any]:
    profile = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(profile, dict) or profile.get("schema_version") != 1:
        raise ValueError("unsupported profile; expected schema_version=1")
    for key in ("device", "kernel", "integration", "stock_config_expectations"):
        if not isinstance(profile.get(key), dict):
            raise ValueError(f"profile missing object: {key}")
    kernel = profile["kernel"]
    for key in ("version", "source_root_default", "config_default", "preferred_defconfig"):
        if not isinstance(kernel.get(key), str) or not kernel[key]:
            raise ValueError(f"profile kernel.{key} must be a non-empty string")
    integration = profile["integration"]
    for key in ("config_symbol", "source_path"):
        if not isinstance(integration.get(key), str) or not integration[key]:
            raise ValueError(f"profile integration.{key} must be a non-empty string")
    if not integration["config_symbol"].startswith("CONFIG_"):
        raise ValueError("integration.config_symbol must start with CONFIG_")
    return profile


def parse_config(path: Path) -> dict[str, str]:
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


def inspect(source_root: Path, config_path: Path, profile: dict[str, Any],
            require_kernelsu: bool = False) -> tuple[list[tuple[str, str, str]], int]:
    checks: list[tuple[str, str, str]] = []
    failures = 0

    makefile = source_root / "Makefile"
    if not makefile.is_file():
        checks.append(("FAIL", "kernel source", f"missing {makefile}"))
        return checks, 1

    version_fields: dict[str, str] = {}
    for line in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^\s*(VERSION|PATCHLEVEL|SUBLEVEL)\s*=\s*(.*?)\s*$", line)
        if match:
            version_fields[match.group(1)] = match.group(2).split("#", 1)[0].strip()
    observed_version = ".".join(version_fields.get(k, "?") for k in ("VERSION", "PATCHLEVEL", "SUBLEVEL"))
    expected_version = profile["kernel"]["version"]
    version_ok = observed_version == expected_version
    checks.append(("PASS" if version_ok else "FAIL", "kernel version",
                   f"found={observed_version}; expected={expected_version}"))
    failures += int(not version_ok)

    required_paths = ("kernel", "arch/arm64", "drivers", "include/linux")
    missing = [item for item in required_paths if not (source_root / item).exists()]
    checks.append(("PASS" if not missing else "FAIL", "source tree layout",
                   "required kernel paths present" if not missing else "missing=" + ",".join(missing)))
    failures += int(bool(missing))

    preferred = source_root / profile["kernel"]["preferred_defconfig"]
    checks.append(("PASS" if preferred.is_file() else "INFO", "preferred defconfig",
                   str(preferred) if preferred.is_file() else f"not found: {preferred} (review actual vendor defconfig path)"))

    if not config_path.is_file():
        checks.append(("FAIL", "build config", f"missing {config_path}"))
        return checks, failures + 1

    values = parse_config(config_path)
    for key, expected in profile["stock_config_expectations"].items():
        observed = values.get(key)
        if observed is None:
            status, detail = "INFO", f"{key} absent from config (not treated as disabled)"
        elif observed == expected:
            status, detail = "PASS", f"found={observed}; expected={expected}"
        else:
            status, detail = "FAIL", f"found={observed}; expected={expected}"
            failures += 1
        checks.append((status, f"stock baseline {key}", detail))

    integration = profile["integration"]
    ksu_path = source_root / integration["source_path"]
    ksu_value = values.get(integration["config_symbol"])
    if ksu_path.is_dir() and ksu_value == integration["expected_config_value"]:
        checks.append(("PASS", "KernelSU source/config",
                       f"{integration['source_path']} exists and {integration['config_symbol']}={ksu_value}"))
    else:
        detail = (
            f"source_dir={'present' if ksu_path.is_dir() else 'missing'}; "
            f"{integration['config_symbol']}={ksu_value if ksu_value is not None else '<unset>'}; "
            "this preflight does not install or patch KernelSU"
        )
        status = "FAIL" if require_kernelsu else "INFO"
        checks.append((status, "KernelSU source/config", detail))
        failures += int(require_kernelsu)
    return checks, failures


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--source-root", type=Path, help="kernel source tree (defaults to profile path)")
    parser.add_argument("--config", type=Path, help="generated kernel .config (defaults under source root)")
    parser.add_argument("--require-kernelsu", action="store_true",
                        help="fail unless drivers/kernelsu and CONFIG_KSU=y are present")
    args = parser.parse_args(argv)

    try:
        profile = load_profile(args.profile)
        source_root = (args.source_root or Path(profile["kernel"]["source_root_default"])).expanduser().resolve()
        config_path = (args.config or (source_root / profile["kernel"]["config_default"])).expanduser().resolve()
        checks, failures = inspect(source_root, config_path, profile, args.require_kernelsu)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"PROFILE={profile.get('profile_id', '<unnamed>')}")
    print(f"SOURCE_ROOT={source_root}")
    print(f"KERNEL_CONFIG={config_path}")
    for status, name, detail in checks:
        print(f"{status}: {name}: {detail}")
    print(f"RESULT={'FAIL' if failures else 'PASS'} failures={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
