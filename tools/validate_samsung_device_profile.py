#!/usr/bin/env python3
"""Validate the evidence-only Samsung device compatibility profile.

This checks identity/provenance metadata only. It deliberately does not create
an executable target profile, derive memory parameters, or emit target.h.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REQUIRED_DEVICE = {
    "manufacturer": "Samsung",
    "model": "SM-G986U1",
    "codename": "y2q",
    "firmware_build": "G986U1UESCHXL1",
}
REQUIRED_SOFTWARE = {
    "android_release": "13",
    "android_sdk": "33",
    "kernel_release_prefix": "4.19.113-27095354",
    "primary_abi": "arm64-v8a",
    "kernel_architecture": "aarch64",
    "va_bits": 39,
}
REQUIRED_CONFIG = {
    "CONFIG_ARM64_VA_BITS_39": "y",
    "CONFIG_FUTEX": "y",
    "CONFIG_FUTEX_PI": "y",
    "CONFIG_RT_MUTEXES": "y",
    "CONFIG_PREEMPT": "y",
    "CONFIG_KALLSYMS": "y",
    "CONFIG_KALLSYMS_ALL": "y",
    "CONFIG_KALLSYMS_BASE_RELATIVE": "y",
    "CONFIG_SECURITY_SELINUX": "y",
    "CONFIG_IKCONFIG_PROC": "y",
}
REQUIRED_SELECTORS = {
    "CONFIG_SEC_Y2Q_PROJECT": "y",
    "CONFIG_MACH_Y2Q_USA_SINGLE": "y",
}
REQUIRED_ARTIFACTS = ("boot_image", "raw_kernel_image", "vendor_elf")


class ProfileError(ValueError):
    pass


def require_mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ProfileError(f"{label} must be a JSON object")
    return value


def require_exact_fields(actual: dict[str, Any], expected: dict[str, Any], label: str) -> None:
    for key, wanted in expected.items():
        if key not in actual:
            raise ProfileError(f"{label}.{key} is missing")
        if actual[key] != wanted:
            raise ProfileError(f"{label}.{key} mismatch: expected {wanted!r}")


def validate_profile(data: Any) -> list[str]:
    root = require_mapping(data, "profile")
    if root.get("schema_version") != 1:
        raise ProfileError("schema_version must be 1")
    if root.get("profile_id") != "samsung-g986u1-4.19.113":
        raise ProfileError("profile_id is not the expected Samsung target")
    if root.get("status") != "identity-check-only":
        raise ProfileError("status must remain identity-check-only")

    device = require_mapping(root.get("device"), "device")
    require_exact_fields(device, REQUIRED_DEVICE, "device")
    software = require_mapping(root.get("software"), "software")
    require_exact_fields(software, REQUIRED_SOFTWARE, "software")

    boot = require_mapping(root.get("boot_image"), "boot_image")
    require_exact_fields(
        boot,
        {"header_version": 2, "page_size": 4096, "kernel_offset": 4096},
        "boot_image",
    )
    if not isinstance(boot.get("kernel_size_bytes"), int) or boot["kernel_size_bytes"] <= 0:
        raise ProfileError("boot_image.kernel_size_bytes must be a positive integer")

    artifacts = require_mapping(root.get("artifacts"), "artifacts")
    for name in REQUIRED_ARTIFACTS:
        entry = require_mapping(artifacts.get(name), f"artifacts.{name}")
        digest = entry.get("sha256")
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise ProfileError(f"artifacts.{name}.sha256 must be a lowercase SHA-256 digest")

    config = require_mapping(root.get("kernel_config"), "kernel_config")
    require_exact_fields(require_mapping(config.get("required"), "kernel_config.required"),
                         REQUIRED_CONFIG, "kernel_config.required")
    require_exact_fields(
        require_mapping(config.get("target_selectors"), "kernel_config.target_selectors"),
        REQUIRED_SELECTORS,
        "kernel_config.target_selectors",
    )

    sections = root.get("elf_sections")
    if not isinstance(sections, list) or not sections:
        raise ProfileError("elf_sections must be a non-empty array")
    for index, section_value in enumerate(sections):
        section = require_mapping(section_value, f"elf_sections[{index}]")
        if not isinstance(section.get("name"), str) or not section["name"].startswith("."):
            raise ProfileError(f"elf_sections[{index}].name must be an ELF section name")
        if not isinstance(section.get("image_offset"), int) or section["image_offset"] < 0:
            raise ProfileError(f"elf_sections[{index}].image_offset must be non-negative")

    source = require_mapping(root.get("kernel_source"), "kernel_source")
    require_exact_fields(
        source,
        {
            "expected_version": 4,
            "expected_patchlevel": 19,
            "expected_sublevel": 113,
            "preferred_device_config": "vendor/y2q_usa_singlew_defconfig",
        },
        "kernel_source",
    )
    return [
        "device identity and compatibility metadata: PASS",
        "artifact digest syntax: PASS (digest values are not re-hashed by this command)",
        "scope: evidence-only; no executable target profile or target.h generated",
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile",
        type=Path,
        default=Path("src/targets/samsung-g986u1-4.19.113/device-profile.json"),
    )
    args = parser.parse_args(argv)
    try:
        if args.profile.is_symlink() or not args.profile.is_file():
            raise ProfileError(f"profile must be a regular non-symlink file: {args.profile}")
        data = json.loads(args.profile.read_text(encoding="utf-8"))
        messages = validate_profile(data)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ProfileError) as exc:
        print(f"PROFILE_VALIDATION=FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"profile={args.profile}")
    for message in messages:
        print(f"PASS: {message}")
    print("RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
