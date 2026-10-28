#!/usr/bin/env python3
"""Unit tests for the read-only Samsung S20+ device compatibility check."""
from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/verify_samsung_device.py"
SPEC = importlib.util.spec_from_file_location("verify_samsung_device", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot import verifier at {MODULE_PATH}")
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)
PROFILE_PATH = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"
PROFILE = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))


def good_properties() -> dict[str, str]:
    return {
        "ro.product.model": "SM-G986U1",
        "ro.product.device": "y2q",
        "ro.build.product": "y2q",
        "ro.build.version.release": "13",
        "ro.build.version.sdk": "33",
        "ro.product.cpu.abi": "arm64-v8a",
        "ro.product.cpu.abilist": "arm64-v8a,armeabi-v7a,armeabi",
        "ro.build.version.incremental": "G986U1UESCHXL1",
    }


class VerifySamsungDeviceTests(unittest.TestCase):
    def test_parse_getprop_output(self) -> None:
        values = module.parse_getprop_output(
            "[ro.product.model]: [SM-G986U1]\n[ro.build.version.release]: [13]\nnot a property\n"
        )
        self.assertEqual(values["ro.product.model"], "SM-G986U1")
        self.assertEqual(values["ro.build.version.release"], "13")
        self.assertEqual(len(values), 2)

    def test_matching_device_profile_passes_all_checks(self) -> None:
        checks = module.evaluate_profile(good_properties(), "4.19.113-27095354", PROFILE)
        self.assertTrue(checks)
        self.assertTrue(all(passed for _, passed, _ in checks), checks)

    def test_kernel_suffix_is_allowed_but_wrong_kernel_fails(self) -> None:
        passed = dict((name, ok) for name, ok, _ in module.evaluate_profile(
            good_properties(), "4.19.113-27095354-perf", PROFILE
        ))
        self.assertTrue(passed["kernel release"])
        failed = dict((name, ok) for name, ok, _ in module.evaluate_profile(
            good_properties(), "5.10.198-android13", PROFILE
        ))
        self.assertFalse(failed["kernel release"])

    def test_wrong_model_or_firmware_fails_closed(self) -> None:
        properties = good_properties()
        properties["ro.product.model"] = "SM-G986B"
        properties["ro.build.version.incremental"] = "OTHER_BUILD"
        checks = dict((name, ok) for name, ok, _ in module.evaluate_profile(
            properties, "4.19.113-27095354", PROFILE
        ))
        self.assertFalse(checks["model"])
        self.assertFalse(checks["firmware build"])

    def test_missing_canonical_device_property_can_use_other_device_prop(self) -> None:
        properties = good_properties()
        del properties["ro.product.device"]
        properties["ro.product.vendor.device"] = "y2q"
        checks = dict((name, ok) for name, ok, _ in module.evaluate_profile(
            properties, "4.19.113-27095354", PROFILE
        ))
        self.assertTrue(checks["device codename"])

    def test_wrong_android_release_and_abi_fail(self) -> None:
        properties = good_properties()
        properties["ro.build.version.release"] = "12"
        properties["ro.product.cpu.abi"] = "armeabi-v7a"
        properties["ro.product.cpu.abilist"] = "armeabi-v7a,armeabi"
        checks = dict((name, ok) for name, ok, _ in module.evaluate_profile(
            properties, "4.19.113-27095354", PROFILE
        ))
        self.assertFalse(checks["Android release"])
        self.assertFalse(checks["primary ABI"])


if __name__ == "__main__":
    unittest.main()
