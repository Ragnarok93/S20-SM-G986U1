#!/usr/bin/env python3
"""Read-only Android boot-image inspector for offline provenance checks.

Supports legacy boot header v0-v2 and modern header v3-v4. It reports the
kernel payload range/hash and optionally compares that exact byte range with a
separately extracted kernel Image. It deliberately does not parse symbols,
derive offsets, create profile JSON, or write target.h.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"ANDROID!"


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


def inspect(boot_path: Path, image_path: Path | None) -> int:
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

    if image_path is not None:
        image = image_path.read_bytes()
        print()
        print(f"comparison_image={image_path}")
        print(f"comparison_size={len(image)}")
        print(f"comparison_sha256={sha256(image)}")
        if payload == image:
            print("comparison=PASS exact byte-for-byte match")
        else:
            print("comparison=DIFFERENT (payload and provided Image are not byte-identical)")
            if len(payload) != len(image):
                print(f"size_delta_payload_minus_image={len(payload) - len(image)}")
            return 1

    print("NOTE: this is input/provenance inspection only; no profile or target header was generated.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", required=True, type=Path, help="original Android boot.img")
    parser.add_argument("--image", type=Path, help="separately extracted raw kernel Image to compare")
    args = parser.parse_args()
    try:
        return inspect(args.boot, args.image)
    except (OSError, InspectError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
