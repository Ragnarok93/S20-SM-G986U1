#!/usr/bin/env python3
"""Offline provenance/config check for the Samsung SM-G986U1 stock artifacts.

This tool checks known SHA-256 digests, stock config flags, and (when objcopy
is available) full byte-for-byte mapping of the two executable sections in the
reconstructed vendor ELF to the raw Image. It does not infer target addresses
or produce an executable target profile.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable

EXPECTED_SHA256 = {
    "boot image": "bd8987c28ef0b9fdb56ad3e82ce3b45929c5e53717525769e5625dd1e17f7dc8",
    "raw kernel Image": "ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123",
    "vendor ELF": "531444e3c98f8f7c4d2baefeda1386aca2b60a42030fcb1f4393c5e10b9154b4",
}
EXPECTED_FLAGS = {
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
IMAGE_SIZE = 51_957_772
# (section name, section VMA-relative Image offset)
SECTIONS = ((".kernel", 0x0), (".kernel2", 0x29DA458))


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


def check_section_mapping(objcopy: str, elf: Path, image: Path) -> list[str]:
    messages: list[str] = []
    image_size = image.stat().st_size
    if image_size != IMAGE_SIZE:
        messages.append(f"FAIL raw Image size is {image_size:,}; expected {IMAGE_SIZE:,}")
        return messages
    with tempfile.TemporaryDirectory(prefix="samsung-elf-check-") as tmp:
        for section, offset in SECTIONS:
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
    parser.add_argument("--boot", type=Path, default=home / "stock-boot.img", help="decompressed stock boot image")
    parser.add_argument("--image", type=Path, default=Path("/tmp/g986u1-stock-kernel.Image"), help="raw kernel Image extracted from the stock boot image")
    parser.add_argument("--elf", type=Path, default=home / "firmware-check/vmlinux.elf", help="reconstructed vendor ELF")
    parser.add_argument("--config", type=Path, default=home / "firmware-check/kernel.config", help="stock kernel config")
    parser.add_argument("--objcopy", help="optional explicit path to llvm-objcopy or compatible objcopy")
    args = parser.parse_args(argv)

    failures = 0
    expected_files = (("boot image", args.boot), ("raw kernel Image", args.image), ("vendor ELF", args.elf))
    print("== Samsung SM-G986U1 / G986U1UESCHXL1 artifact check ==")
    for label, path in expected_files:
        if not path.is_file():
            print(f"FAIL {label}: missing file: {path}")
            failures += 1
            continue
        digest = sha256(path)
        expected = EXPECTED_SHA256[label]
        if digest == expected:
            print(f"PASS {label}: SHA-256 {digest}")
        else:
            print(f"FAIL {label}: SHA-256 {digest}; expected {expected}")
            failures += 1

    if args.config.is_file():
        config = parse_config(args.config)
        print(f"\n== Required stock configuration: {args.config} ==")
        for key, expected in EXPECTED_FLAGS.items():
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
            for message in check_section_mapping(objcopy, args.elf, args.image):
                print(message)
                if message.startswith("FAIL"):
                    failures += 1
    else:
        print("\nWARN section-byte comparison skipped because ELF or Image is missing")

    print("\nRESULT: " + ("FAIL; review messages above" if failures else "checks passed; profile remains documentation-only"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
