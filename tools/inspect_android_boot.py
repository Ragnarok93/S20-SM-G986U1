#!/usr/bin/env python3
"""Read-only Android boot-image and firmware-metadata inspector.

Supports legacy Android boot headers v0-v2 and modern headers v3-v4. It reports
the kernel payload range/hash and can compare that range byte-for-byte with a
separately extracted raw Image. Optional config and ELF checks record whether
BTF-related options/signatures are present.

This is an input/provenance tool only: it does not parse exploit symbols,
derive offsets, create profile JSON, or write target.h.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path
from typing import Iterable

MAGIC = b"ANDROID!"
BTF_MAGIC = b"\x9f\xeb\x01\x00"
CONFIG_KEYS = (
    "CONFIG_IKCONFIG",
    "CONFIG_IKCONFIG_PROC",
    "CONFIG_IKHEADERS",
    "CONFIG_DEBUG_INFO",
    "CONFIG_DEBUG_INFO_BTF",
    "CONFIG_KALLSYMS",
    "CONFIG_KALLSYMS_ALL",
    "CONFIG_ARM64_VA_BITS",
    "CONFIG_ARM64_PA_BITS",
    "CONFIG_ARM64_4K_PAGES",
    "CONFIG_FUTEX",
    "CONFIG_FUTEX_PI",
    "CONFIG_RT_MUTEXES",
    "CONFIG_PREEMPT",
    "CONFIG_SECURITY_SELINUX",
)


class InspectError(Exception):
    pass


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise InspectError(f"truncated boot header at offset 0x{offset:x}")
    return struct.unpack_from("<I", data, offset)[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align_up(value: int, alignment: int) -> int:
    if alignment <= 0 or alignment & (alignment - 1):
        raise InspectError(f"invalid page size: {alignment}")
    return (value + alignment - 1) & ~(alignment - 1)


def parse_config(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip()
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            key = line[2:].split(" is not set", 1)[0]
            values[key] = "n"
    return values


def find_btf_signatures(data: bytes, limit: int = 10) -> tuple[int, list[int]]:
    positions: list[int] = []
    count = 0
    cursor = 0
    while True:
        pos = data.find(BTF_MAGIC, cursor)
        if pos < 0:
            break
        count += 1
        if len(positions) < limit:
            positions.append(pos)
        cursor = pos + 1
    return count, positions


def inspect(
    boot_path: Path,
    image_path: Path | None,
    config_path: Path | None,
    elf_path: Path | None,
    scan_btf: bool,
) -> int:
    boot = boot_path.read_bytes()
    if len(boot) < 4096 or boot[:8] != MAGIC:
        raise InspectError(f"{boot_path} is not a valid-sized Android boot image")

    version = u32(boot, 40)
    kernel_size = u32(boot, 8)
    if kernel_size == 0:
        raise InspectError("kernel_size is zero")

    if version in (0, 1, 2):
        page_size = u32(boot, 36)
        if not 2048 <= page_size <= 65536 or page_size & (page_size - 1):
            raise InspectError(f"invalid legacy page_size: 0x{page_size:x}")
        kernel_offset = page_size
        kernel_addr = u32(boot, 12)
        ramdisk_size = u32(boot, 16)
        ramdisk_addr = u32(boot, 20)
        layout = "legacy"
    elif version in (3, 4):
        header_size = u32(boot, 20)
        expected = {3: 0x62C, 4: 0x630}[version]
        if header_size != expected:
            raise InspectError(
                f"v{version} header_size mismatch: 0x{header_size:x}, expected 0x{expected:x}"
            )
        page_size = 4096
        kernel_offset = align_up(header_size, page_size)
        kernel_addr = None
        ramdisk_size = u32(boot, 12)
        ramdisk_addr = None
        layout = "modern"
    else:
        raise InspectError(f"unsupported boot header version: {version}")

    kernel_end = kernel_offset + kernel_size
    if kernel_end < kernel_offset or kernel_end > len(boot):
        raise InspectError(
            f"kernel range [0x{kernel_offset:x}, 0x{kernel_end:x}) exceeds file size 0x{len(boot):x}"
        )

    payload = boot[kernel_offset:kernel_end]
    print("== Android boot image inspection ==")
    print(f"boot_file={boot_path}")
    print(f"boot_size={len(boot)}")
    print(f"header_version={version}")
    print(f"header_layout={layout}")
    print(f"page_size={page_size}")
    print(f"kernel_offset=0x{kernel_offset:x}")
    print(f"kernel_size={kernel_size} (0x{kernel_size:x})")
    print(f"kernel_end=0x{kernel_end:x}")
    if kernel_addr is not None:
        print(f"kernel_load_address=0x{kernel_addr:x}")
        print(f"ramdisk_size={ramdisk_size} (0x{ramdisk_size:x})")
        print(f"ramdisk_load_address=0x{ramdisk_addr:x}")
    else:
        print(f"ramdisk_size={ramdisk_size} (0x{ramdisk_size:x})")
    print(f"payload_sha256={sha256(payload)}")
    if len(payload) >= 0x3C and payload[0x38:0x3C] == b"ARM\\x64":
        print("payload_signature=raw ARM64 Image")
    elif payload.startswith(b"\\x1f\\x8b"):
        print("payload_signature=gzip")
    elif payload.startswith((b"\\x02\\x21\\x4c\\x18", b"\\x04\\x22\\x4d\\x18")):
        print("payload_signature=LZ4")
    else:
        print(f"payload_signature=unrecognized first16={payload[:16].hex()}")

    comparison_failed = False
    image: bytes | None = None
    if image_path is not None:
        image = image_path.read_bytes()
        print()
        print(f"comparison_image={image_path}")
        print(f"comparison_size={len(image)}")
        print(f"comparison_sha256={sha256(image)}")
        if payload == image:
            print("comparison=PASS exact byte-for-byte match")
        elif payload.startswith(image):
            print("comparison=DIFFERENT; provided Image is an exact prefix of the boot kernel payload")
            print(f"payload_trailing_bytes={len(payload) - len(image)}")
            comparison_failed = True
        elif image.startswith(payload):
            print("comparison=DIFFERENT; boot kernel payload is an exact prefix of the provided Image")
            print(f"image_trailing_bytes={len(image) - len(payload)}")
            comparison_failed = True
        else:
            print("comparison=DIFFERENT; boot payload and provided Image differ within their shared range")
            if len(payload) != len(image):
                print(f"size_delta_payload_minus_image={len(payload) - len(image)}")
            comparison_failed = True

    if config_path is not None:
        settings = parse_config(config_path)
        print()
        print("== Selected stock kernel config ==")
        print(f"config_file={config_path}")
        for key in CONFIG_KEYS:
            print(f"{key}={settings.get(key, '<not listed>')}")
        if settings.get("CONFIG_DEBUG_INFO_BTF") == "y":
            print("btf_config=enabled according to config")
        else:
            print(
                "btf_config=not enabled in the parsed config "
                f"(value={settings.get('CONFIG_DEBUG_INFO_BTF', '<not listed>')})"
            )
        if settings.get("CONFIG_IKHEADERS") == "n":
            print("embedded_kernel_headers=disabled according to config")
        elif settings.get("CONFIG_IKHEADERS") == "y":
            print("embedded_kernel_headers=enabled according to config")
        else:
            print("embedded_kernel_headers=not established by this config")

    if scan_btf:
        scan_inputs: list[tuple[str, bytes]] = [
            (str(boot_path), boot),
            (f"{boot_path} kernel payload", payload),
        ]
        if image is not None:
            scan_inputs.append((str(image_path), image))
        if elf_path is not None:
            elf = elf_path.read_bytes()
            scan_inputs.append((str(elf_path), elf))
        print()
        print("== BTF magic signature scan (heuristic only) ==")
        for label, data in scan_inputs:
            count, positions = find_btf_signatures(data)
            first = ",".join(f"0x{pos:x}" for pos in positions) if positions else "none"
            print(f"{label}: signature_hits={count}; first_offsets={first}")
        print("NOTE: signature hits are candidates, not proof of valid BTF; zero hits alone cannot prove absence.")

    print()
    print("NOTE: no exploit symbols/offsets were derived and no profile or target.h was generated.")
    return 1 if comparison_failed else 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", required=True, type=Path, help="original Android boot.img")
    parser.add_argument("--image", type=Path, help="separately extracted raw kernel Image to compare")
    parser.add_argument("--config", type=Path, help="extracted stock kernel .config")
    parser.add_argument("--elf", type=Path, help="optional reconstructed vendor ELF to scan")
    parser.add_argument("--scan-btf", action="store_true", help="scan BTF magic signatures (heuristic only)")
    args = parser.parse_args()
    try:
        return inspect(args.boot, args.image, args.config, args.elf, args.scan_btf)
    except (OSError, InspectError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
