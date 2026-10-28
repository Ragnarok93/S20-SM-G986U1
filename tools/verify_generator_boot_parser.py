#!/usr/bin/env python3
"""Read-only regression checks for the external generator's boot-image parsers.

This exercises only extract_boot_kernel() in the two vendored implementations.
It does not call BTF/kallsyms analysis or generate profile files.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import struct
import sys
import tempfile
from pathlib import Path
from types import ModuleType


PARSER_PATHS = (
    Path("tools/vendor/generate_target.py"),
    Path("tools/vendor/generate_target_2.py"),
)
MODERN_HEADER_SIZES = {3: 0x62C, 4: 0x630}
MIN_KERNEL_SIZE = 0x10000


class CheckError(RuntimeError):
    pass


def load_module(path: Path, name: str) -> ModuleType:
    if path.is_symlink() or not path.is_file():
        raise CheckError(f"parser source missing or symlinked: {path}")
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise CheckError(f"cannot load parser module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def make_arm64_image(size: int = MIN_KERNEL_SIZE, *, include_pe: bool = True) -> bytes:
    if size < MIN_KERNEL_SIZE:
        raise ValueError("fixture kernel must meet generator's 64 KiB minimum")
    image = bytearray(size)
    struct.pack_into("<Q", image, 8, 0x80000)  # text_offset
    struct.pack_into("<Q", image, 16, size)    # image_size
    struct.pack_into("<Q", image, 24, 0)       # image_flags
    image[0x38:0x3C] = b"ARM\x64"
    # The PE/COFF stub is optional; the header field is reserved/zero when
    # the Image is not built with an EFI stub.
    if include_pe:
        struct.pack_into("<I", image, 60, 0x40)
        image[0x40:0x44] = b"PE\x00\x00"
    return bytes(image)


def make_boot(
    payload: bytes,
    *,
    version: int,
    page_size: int = 4096,
    declared_kernel_size: int | None = None,
    modern_header_size: int | None = None,
) -> bytes:
    declared_size = len(payload) if declared_kernel_size is None else declared_kernel_size
    if version in (0, 1, 2):
        offset = page_size
    else:
        offset = 4096
    boot = bytearray(max(4096, offset + len(payload)))
    boot[:8] = b"ANDROID!"
    struct.pack_into("<I", boot, 8, declared_size)
    struct.pack_into("<I", boot, 40, version)
    if version in (0, 1, 2):
        struct.pack_into("<I", boot, 36, page_size)
    elif version in (3, 4):
        header_size = MODERN_HEADER_SIZES[version] if modern_header_size is None else modern_header_size
        struct.pack_into("<I", boot, 20, header_size)
    boot[offset:offset + len(payload)] = payload
    return bytes(boot)


def expect_parse(module: ModuleType, boot_bytes: bytes, expected_payload: bytes, label: str) -> None:
    with tempfile.TemporaryDirectory(prefix="boot-parser-test-") as temporary:
        path = Path(temporary) / "boot.img"
        path.write_bytes(boot_bytes)
        result = module.extract_boot_kernel(path)
        if result.kernel != expected_payload:
            raise CheckError(
                f"{module.__name__}: {label}: extracted payload differs "
                f"(got {len(result.kernel)} bytes, expected {len(expected_payload)})"
            )
        expected_hash = hashlib.sha256(expected_payload).hexdigest()
        if result.kernel_sha256 != expected_hash:
            raise CheckError(f"{module.__name__}: {label}: kernel hash mismatch")


def expect_rejected(module: ModuleType, boot_bytes: bytes, expected_message: str, label: str) -> None:
    with tempfile.TemporaryDirectory(prefix="boot-parser-test-") as temporary:
        path = Path(temporary) / "boot.img"
        path.write_bytes(boot_bytes)
        try:
            module.extract_boot_kernel(path)
        except module.GenerationError as exc:
            if expected_message not in str(exc):
                raise CheckError(
                    f"{module.__name__}: {label}: unexpected error {exc!s}"
                ) from exc
        else:
            raise CheckError(f"{module.__name__}: {label}: malformed fixture was accepted")


def run_synthetic_tests(module: ModuleType) -> int:
    payload = make_arm64_image()
    cases = (
        (0, 2048, "legacy v0/page 2048"),
        (1, 4096, "legacy v1/page 4096"),
        (2, 8192, "legacy v2/page 8192"),
        (3, 4096, "modern v3"),
        (4, 4096, "modern v4"),
    )
    for version, page_size, label in cases:
        expect_parse(
            module,
            make_boot(payload, version=version, page_size=page_size),
            payload,
            label,
        )

    expect_rejected(
        module,
        make_boot(payload, version=2, page_size=0x3000),
        "page_size",
        "invalid legacy page size",
    )

    no_pe_payload = make_arm64_image(include_pe=False)
    for version, page_size in ((2, 4096), (3, 4096), (4, 4096)):
        expect_parse(
            module,
            make_boot(no_pe_payload, version=version, page_size=page_size),
            no_pe_payload,
            f"header v{version} ARM64 Image without optional PE stub",
        )
    expect_rejected(
        module,
        make_boot(payload, version=3, modern_header_size=0x630),
        "header_size",
        "modern v3 header-size mismatch",
    )

    invalid_pe_payload = bytearray(payload)
    struct.pack_into("<I", invalid_pe_payload, 60, 0x40)
    invalid_pe_payload[0x40:0x44] = b"NOPE"
    expect_rejected(
        module,
        make_boot(bytes(invalid_pe_payload), version=2),
        "PE/COFF stub magic",
        "nonzero PE offset without PE signature",
    )
    expect_rejected(
        module,
        make_boot(payload, version=5),
        "header",
        "unsupported header version",
    )
    expect_rejected(
        module,
        make_boot(payload, version=2, page_size=4096, declared_kernel_size=len(payload) * 2),
        "越界",
        "kernel range past EOF",
    )
    return len(cases) + 8


def verify_real_firmware(
    modules: list[ModuleType], boot_path: Path, image_path: Path, expected_version: int
) -> None:
    boot = boot_path.read_bytes()
    if len(boot) < 44 or boot[:8] != b"ANDROID!":
        raise CheckError(f"not a valid Android boot image: {boot_path}")
    version = struct.unpack_from("<I", boot, 40)[0]
    if version != expected_version:
        raise CheckError(
            f"expected boot header v{expected_version}, observed v{version} in {boot_path}"
        )
    image = image_path.read_bytes()
    expected_hash = hashlib.sha256(image).hexdigest()
    print(f"firmware_boot={boot_path} header_version={version} bytes={len(boot)}")
    print(f"reference_image={image_path} bytes={len(image)} sha256={expected_hash}")
    extracted_hashes: set[str] = set()
    for module in modules:
        result = module.extract_boot_kernel(boot_path)
        if result.kernel != image:
            raise CheckError(
                f"{module.__name__}: extracted kernel does not exactly match reference Image"
            )
        digest = hashlib.sha256(result.kernel).hexdigest()
        if digest != expected_hash or result.kernel_sha256 != expected_hash:
            raise CheckError(f"{module.__name__}: extracted kernel SHA-256 mismatch")
        extracted_hashes.add(digest)
        print(f"{module.__name__}: real v{version} payload exact-match PASS sha256={digest}")
    if len(extracted_hashes) != 1:
        raise CheckError("the vendored parser implementations disagree on extracted kernel bytes")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--generator-root",
        type=Path,
        default=Path("/tmp/ghostlock-profile-gen"),
        help="upstream generator checkout containing tools/vendor/",
    )
    parser.add_argument("--boot", type=Path, help="optional real firmware boot.img")
    parser.add_argument("--image", type=Path, help="raw extracted Image for byte-exact comparison")
    parser.add_argument("--expect-version", type=int, default=2)
    args = parser.parse_args(argv)
    if (args.boot is None) != (args.image is None):
        parser.error("--boot and --image must be supplied together")

    try:
        modules = [
            load_module(args.generator_root / relative, f"generator_parser_{index}")
            for index, relative in enumerate(PARSER_PATHS)
        ]
        total = 0
        for module in modules:
            count = run_synthetic_tests(module)
            total += count
            print(f"{module.__name__}: synthetic cases PASS ({count})")
        if args.boot is not None and args.image is not None:
            verify_real_firmware(modules, args.boot, args.image, args.expect_version)
        else:
            print("real firmware comparison: skipped (provide --boot and --image)")
        print(f"RESULT=PASS parser_implementations={len(modules)} synthetic_checks={total}")
        return 0
    except (OSError, CheckError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
