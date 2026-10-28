#!/usr/bin/env python3
"""Verify Samsung SM-G986U1 artifact provenance and selected kernel config values."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"


def load_profile(path: Path) -> dict[str, Any]:
    profile = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(profile, dict) or profile.get("schema_version") != 1:
        raise ValueError("unsupported or malformed Samsung device profile (expected schema_version=1)")
    required = ("profile_id", "device", "software", "boot_image", "artifacts", "kernel_config", "elf_sections")
    missing = [key for key in required if key not in profile]
    if missing:
        raise ValueError("profile missing required fields: " + ", ".join(missing))
    for key in ("boot_image", "raw_kernel_image", "vendor_elf"):
        if key not in profile["artifacts"] or "sha256" not in profile["artifacts"][key]:
            raise ValueError(f"profile missing artifacts.{key}.sha256")
    if not isinstance(profile["kernel_config"].get("required"), dict):
        raise ValueError("profile kernel_config.required must be an object")
    if not isinstance(profile["elf_sections"], list) or not profile["elf_sections"]:
        raise ValueError("profile elf_sections must be a non-empty list")
    for section in profile["elf_sections"]:
        if not isinstance(section, dict) or not isinstance(section.get("name"), str) or not isinstance(section.get("image_offset"), int):
            raise ValueError("each elf_sections entry must have a string name and integer image_offset")
    return profile


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_config(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip()
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            key = line[2:].split(" is not set", 1)[0]
            values[key] = "n"
    return values


def locate_objcopy(explicit: str | None) -> str | None:
    if explicit:
        return explicit if Path(explicit).is_file() else None
    candidates: list[str] = []
    for executable in ("llvm-objcopy", "aarch64-linux-android-objcopy", "objcopy"):
        found = shutil.which(executable)
        if found:
            candidates.append(found)
    home = Path.home()
    candidates.extend([
        str(home / "toolchains-for-snapdragon/llvm-arm-toolchain-ship-10.0-master/bin/llvm-objcopy"),
        str(home / "toolchains-for-snapdragon/aarch64-linux-android-4.9-master/bin/aarch64-linux-android-objcopy"),
    ])
    for candidate in candidates:
        if Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def check_section_mapping(
    objcopy: str, elf: Path, image: Path, expected_image_size: int,
    sections: Iterable[tuple[str, int]],
) -> list[str]:
    messages: list[str] = []
    image_size = image.stat().st_size
    if image_size != expected_image_size:
        messages.append(f"FAIL raw Image size is {image_size:,}; expected {expected_image_size:,}")
        return messages
    with tempfile.TemporaryDirectory(prefix="samsung-elf-check-") as tmp:
        for section, offset in sections:
            output = Path(tmp) / (section.lstrip(".") + ".bin")
            cmd = [objcopy, "--dump-section", f"{section}={output}", str(elf)]
            result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if result.returncode != 0 or not output.exists():
                reason = result.stderr.strip() or f"exit {result.returncode}"
                messages.append(f"FAIL unable to extract {section}: {reason}")
                continue
            size = output.stat().st_size
            if offset + size > image_size:
                messages.append(f"FAIL {section} mapping exceeds Image bounds")
                continue
            equal = True
            remaining = size
            with image.open("rb") as stock, output.open("rb") as section_file:
                stock.seek(offset)
                while remaining:
                    count = min(1024 * 1024, remaining)
                    a = stock.read(count)
                    b = section_file.read(count)
                    if a != b or len(a) != count or len(b) != count:
                        equal = False
                        break
                    remaining -= count
            if equal:
                messages.append(f"PASS {section} matches Image bytes at offset 0x{offset:x} (0x{size:x} bytes)")
            else:
                messages.append(f"FAIL {section} differs from Image at mapped offset 0x{offset:x}")
    return messages


def main(argv: Iterable[str] | None = None) -> int:
    home = Path.home()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE, help="machine-readable target profile")
    parser.add_argument("--boot", type=Path, default=home / "stock-boot.img", help="decompressed stock boot image")
    parser.add_argument("--image", type=Path, default=Path("/tmp/g986u1-stock-kernel.Image"), help="raw kernel Image extracted from the stock boot image")
    parser.add_argument("--elf", type=Path, default=home / "firmware-check/vmlinux.elf", help="reconstructed vendor ELF")
    parser.add_argument("--config", type=Path, default=home / "firmware-check/kernel.config", help="stock kernel config")
    parser.add_argument("--objcopy", help="optional explicit path to llvm-objcopy or compatible objcopy")
    args = parser.parse_args(argv)

    try:
        profile = load_profile(args.profile)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"FAIL profile metadata: {exc}", file=sys.stderr)
        return 2

    expected_hashes = {
        "boot image": profile["artifacts"]["boot_image"]["sha256"],
        "raw kernel Image": profile["artifacts"]["raw_kernel_image"]["sha256"],
        "vendor ELF": profile["artifacts"]["vendor_elf"]["sha256"],
    }
    expected_flags: dict[str, str] = profile["kernel_config"]["required"]
    image_size = int(profile["boot_image"]["kernel_size_bytes"])
    sections = [(item["name"], int(item["image_offset"])) for item in profile["elf_sections"]]
    device = profile["device"]

    failures = 0
    expected_files = (("boot image", args.boot), ("raw kernel Image", args.image), ("vendor ELF", args.elf))
    print(f"== {device['model']} / {device['firmware_build']} artifact check ({profile['profile_id']}) ==")
    for label, path in expected_files:
        if not path.is_file():
            print(f"FAIL {label}: missing file: {path}")
            failures += 1
            continue
        digest = sha256(path)
        expected = expected_hashes[label]
        if digest == expected:
            print(f"PASS {label}: SHA-256 {digest}")
        else:
            print(f"FAIL {label}: SHA-256 {digest}; expected {expected}")
            failures += 1

    if args.config.is_file():
        config = parse_config(args.config)
        print(f"\n== Required stock configuration: {args.config} ==")
        for key, expected in expected_flags.items():
            actual = config.get(key, "<missing>")
            if actual == expected:
                print(f"PASS {key}={actual}")
            else:
                print(f"FAIL {key}: found {actual}; expected {expected}")
                failures += 1
    else:
        print(f"FAIL stock config missing: {args.config}")
        failures += 1

    if args.elf.is_file() and args.image.is_file():
        objcopy = locate_objcopy(args.objcopy)
        print("\n== Full ELF-section-to-Image comparison ==")
        if not objcopy:
            print("WARN objcopy not found; section-byte comparison skipped (pass --objcopy PATH to enable)")
        else:
            for message in check_section_mapping(objcopy, args.elf, args.image, image_size, sections):
                print(message)
                if message.startswith("FAIL"):
                    failures += 1
    else:
        print("\nWARN section-byte comparison skipped because ELF or Image is missing")

    print("\nRESULT: " + ("FAIL; review messages above" if failures else "checks passed; device profile remains identity/provenance-only"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
