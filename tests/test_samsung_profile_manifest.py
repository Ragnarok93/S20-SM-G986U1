#!/usr/bin/env python3
"""Tests for the machine-readable Samsung device profile and verifier contract."""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE_PATH = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"
MODULE_PATH = ROOT / "tools/verify_samsung_firmware.py"
SPEC = importlib.util.spec_from_file_location("verify_samsung_firmware", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot import verifier at {MODULE_PATH}")
verifier = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verifier
SPEC.loader.exec_module(verifier)


class SamsungProfileManifestTests(unittest.TestCase):
    def test_profile_records_known_device_and_firmware_baseline(self) -> None:
        profile = verifier.load_profile(PROFILE_PATH)
        self.assertEqual(profile["device"]["model"], "SM-G986U1")
        self.assertEqual(profile["device"]["codename"], "y2q")
        self.assertEqual(profile["device"]["firmware_build"], "G986U1UESCHXL1")
        self.assertEqual(profile["software"]["kernel_release_prefix"], "4.19.113-27095354")
        self.assertEqual(profile["software"]["android_sdk"], "33")
        self.assertEqual(profile["boot_image"]["header_version"], 2)
        self.assertEqual(profile["boot_image"]["page_size"], 4096)
        self.assertEqual(profile["boot_image"]["kernel_offset"], 4096)

    def test_hashes_config_requirements_and_elf_section_map_are_complete(self) -> None:
        profile = verifier.load_profile(PROFILE_PATH)
        for name in ("boot_image", "raw_kernel_image", "vendor_elf"):
            digest = profile["artifacts"][name]["sha256"]
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
        self.assertEqual(profile["boot_image"]["kernel_size_bytes"], 51_957_772)
        self.assertEqual(profile["kernel_config"]["required"]["CONFIG_ARM64_VA_BITS_39"], "y")
        self.assertEqual(profile["kernel_config"]["required"]["CONFIG_FUTEX_PI"], "y")
        sections = {item["name"]: item["image_offset"] for item in profile["elf_sections"]}
        self.assertEqual(sections[".kernel"], 0)
        self.assertEqual(sections[".kernel2"], 0x29DA458)

    def test_malformed_profile_fails_validation(self) -> None:
        profile = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        del profile["artifacts"]["raw_kernel_image"]
        with tempfile.TemporaryDirectory() as tmp:
            bad_path = Path(tmp) / "bad-profile.json"
            bad_path.write_text(json.dumps(profile), encoding="utf-8")
            with self.assertRaises(ValueError):
                verifier.load_profile(bad_path)


if __name__ == "__main__":
    unittest.main()
