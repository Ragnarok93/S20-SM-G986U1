#!/usr/bin/env python3
"""Read-only ADB identity check for the Samsung SM-G986U1 device profile."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"
DEVICE_KEYS = (
    "ro.product.device",
    "ro.product.vendor.device",
    "ro.product.product.device",
    "ro.product.system.device",
    "ro.build.product",
)
ABI_KEYS = ("ro.product.cpu.abi", "ro.product.cpu.abilist", "ro.product.cpu.abilist32", "ro.product.cpu.abilist64")


def parse_getprop_output(output: str) -> dict[str, str]:
    """Parse Android getprop output; ignore lines that are not property records."""
    properties: dict[str, str] = {}
    for line in output.splitlines():
        match = re.match(r"^\[([^]]+)\]: \[(.*)\]$", line.strip())
        if match:
            properties[match.group(1)] = match.group(2)
    return properties


def evaluate_profile(
    properties: dict[str, str], kernel_release: str, kernel_arch: str, profile: dict[str, Any]
) -> list[tuple[str, bool, str]]:
    """Return named compatibility checks without executing commands or modifying state."""
    device = profile["device"]
    software = profile["software"]
    checks: list[tuple[str, bool, str]] = []

    model = properties.get("ro.product.model", "").strip()
    checks.append(("model", model == device["model"], f"found={model or '<missing>'}; expected={device['model']}"))

    codename = device["codename"].lower()
    observed_devices = [properties.get(key, "").strip().lower() for key in DEVICE_KEYS]
    matching_devices = [value for value in observed_devices if value == codename or value.startswith(codename + "-")]
    checks.append((
        "device codename",
        bool(matching_devices),
        f"found={','.join(matching_devices) if matching_devices else ','.join(v for v in observed_devices if v) or '<missing>'}; expected={codename}",
    ))

    android_release = properties.get("ro.build.version.release", "").strip()
    checks.append(("Android release", android_release == software["android_release"],
                   f"found={android_release or '<missing>'}; expected={software['android_release']}"))

    sdk = properties.get("ro.build.version.sdk", "").strip()
    checks.append(("Android SDK", sdk == software["android_sdk"],
                   f"found={sdk or '<missing>'}; expected={software['android_sdk']}"))

    abi_values = [properties.get(key, "").strip() for key in ABI_KEYS]
    abi_match = any(software["primary_abi"] in value.split(",") for value in abi_values if value)
    checks.append(("primary ABI", abi_match,
                   f"found={','.join(v for v in abi_values if v) or '<missing>'}; expected to include {software['primary_abi']}"))

    expected_kernel = software["kernel_release_prefix"]
    kernel_ok = kernel_release == expected_kernel or kernel_release.startswith(expected_kernel + "-")
    checks.append(("kernel release", kernel_ok,
                   f"found={kernel_release or '<missing>'}; expected prefix={expected_kernel}"))

    expected_arch = software["kernel_architecture"]
    checks.append(("kernel architecture", kernel_arch == expected_arch,
                   f"found={kernel_arch or '<missing>'}; expected={expected_arch}"))

    firmware = device["firmware_build"]
    matching_properties = [key for key, value in properties.items() if firmware in value]
    checks.append(("firmware build", bool(matching_properties),
                   f"found in {','.join(sorted(matching_properties)) if matching_properties else '<no property matched>'}; expected token={firmware}"))
    return checks


def run_adb(adb: str, serial: str | None, args: Iterable[str]) -> str:
    command = [adb]
    if serial:
        command.extend(["-s", serial])
    command.extend(args)
    result = subprocess.run(command, capture_output=True, text=True, timeout=20, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit status {result.returncode}"
        raise RuntimeError(f"command failed ({' '.join(command)}): {detail}")
    return result.stdout.strip()


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE, help="JSON device compatibility profile")
    parser.add_argument("--adb", default=os.environ.get("ADB", "adb"), help="ADB executable (default: adb or $ADB)")
    parser.add_argument("--serial", default=os.environ.get("ADB_SERIAL"), help="ADB serial; also accepts $ADB_SERIAL")
    args = parser.parse_args(argv)

    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8"))
        if not isinstance(profile, dict) or profile.get("schema_version") != 1:
            version = profile.get("schema_version") if isinstance(profile, dict) else None
            raise ValueError(f"unsupported or malformed profile schema_version={version!r}")
        if not isinstance(profile.get("device"), dict) or not isinstance(profile.get("software"), dict):
            raise ValueError("profile requires device and software objects")
        properties = parse_getprop_output(run_adb(args.adb, args.serial, ["shell", "getprop"]))
        kernel_release = run_adb(args.adb, args.serial, ["shell", "uname", "-r"])
        kernel_arch = run_adb(args.adb, args.serial, ["shell", "uname", "-m"])
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError, subprocess.TimeoutExpired, RuntimeError) as exc:
        print(f"ERROR: Samsung device check could not run: {exc}", file=sys.stderr)
        return 2

    device = profile["device"]
    print(f"== Read-only device compatibility check: {profile['profile_id']} ==")
    print(f"Target: {device['manufacturer']} {device['marketing_name']} ({device['model']}, {device['codename']})")
    failures = 0
    for name, passed, detail in evaluate_profile(properties, kernel_release, kernel_arch, profile):
        print(f"{'PASS' if passed else 'FAIL'} {name}: {detail}")
        failures += int(not passed)

    print("\nThis check verifies device/build identity only; it does not claim runtime behavior or kernel-port completeness.")
    print("RESULT: " + ("FAIL; device/build does not match this profile" if failures else "PASS; device/build identity matches"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
