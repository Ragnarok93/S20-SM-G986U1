#!/usr/bin/env python3
"""Regression tests for read-only Android boot-image inspection.

These tests cover parser/metadata behavior only; they do not test exploit code,
kernel symbol discovery, or target-profile generation.
"""
from __future__ import annotations

import importlib.util
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "inspect_android_boot.py"
SPEC = importlib.util.spec_from_file_location("inspect_android_boot", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot import inspector at {MODULE_PATH}")
inspector = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = inspector
SPEC.loader.exec_module(inspector)


def make_legacy_boot(payload: bytes, *, version: int = 2, page_size: int = 4096) -> bytes:
    """Build a minimal synthetic legacy boot image for parser regression tests."""
    boot = bytearray(page_size + len(payload))
    boot[:8] = b"ANDROID!"
    struct.pack_into("<I", boot, 8, len(payload))  # kernel_size
    struct.pack_into("<I", boot, 12, 0x8000)       # kernel_addr
    struct.pack_into("<I", boot, 16, 0)            # ramdisk_size
    struct.pack_into("<I", boot, 20, 0)            # ramdisk_addr
    struct.pack_into("<I", boot, 36, page_size)
    struct.pack_into("<I", boot, 40, version)
    boot[page_size:] = payload
    return bytes(boot)


def make_arm64_payload() -> bytes:
    payload = bytearray(0x100)
    payload[0x38:0x3C] = bytes((0x41, 0x52, 0x4D, 0x64))
    return bytes(payload)


class InspectAndroidBootTests(unittest.TestCase):
    def test_v2_legacy_image_and_exact_kernel_match(self) -> None:
        payload = make_arm64_payload()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot_path = root / "boot.img"
            image_path = root / "Image"
            boot_path.write_bytes(make_legacy_boot(payload))
            image_path.write_bytes(payload)

            output = io.StringIO()
            with redirect_stdout(output):
                result = inspector.inspect(
                    boot_path, image_path, None, None, False
                )

            self.assertEqual(result, 0)
            report = output.getvalue()
            self.assertIn("header_version=2", report)
            self.assertIn("header_layout=legacy", report)
            self.assertIn("kernel_offset=0x1000", report)
            self.assertIn("comparison=PASS exact byte-for-byte match", report)
            self.assertIn("payload_signature=raw ARM64 Image", report)

    def test_kernel_payload_mismatch_returns_failure(self) -> None:
        payload = make_arm64_payload()
        wrong_image = bytes([payload[0] ^ 0xFF]) + payload[1:]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot_path = root / "boot.img"
            image_path = root / "Image"
            boot_path.write_bytes(make_legacy_boot(payload))
            image_path.write_bytes(wrong_image)

            output = io.StringIO()
            with redirect_stdout(output):
                result = inspector.inspect(
                    boot_path, image_path, None, None, False
                )

            self.assertEqual(result, 1)
            self.assertIn("comparison=DIFFERENT", output.getvalue())

    def test_unsupported_header_version_fails_with_cli_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            boot_path = Path(tmp) / "boot.img"
            boot_path.write_bytes(make_legacy_boot(make_arm64_payload(), version=5))

            stdout = io.StringIO()
            stderr = io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                result = inspector.main(["--boot", str(boot_path)])

            self.assertEqual(result, 2)
            self.assertIn("unsupported boot header version: 5", stderr.getvalue())

    def test_kernel_range_past_eof_fails_with_cli_error(self) -> None:
        payload = make_arm64_payload()
        boot = bytearray(make_legacy_boot(payload))
        struct.pack_into("<I", boot, 8, len(payload) + 0x1000)
        with tempfile.TemporaryDirectory() as tmp:
            boot_path = Path(tmp) / "boot.img"
            boot_path.write_bytes(boot)

            stderr = io.StringIO()
            with redirect_stderr(stderr):
                result = inspector.main(["--boot", str(boot_path)])

            self.assertEqual(result, 2)
            self.assertIn("exceeds file size", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
