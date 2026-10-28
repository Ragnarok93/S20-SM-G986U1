#!/usr/bin/env python3
"""Regression tests for Samsung kernel-source compatibility preflight."""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/verify_samsung_kernel_source.py"
PROFILE_PATH = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"
SPEC = importlib.util.spec_from_file_location("verify_samsung_kernel_source", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot import verifier at {MODULE_PATH}")
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)
PROFILE = module.load_profile(PROFILE_PATH)


def make_tree(root: Path, version: str = "4", patchlevel: str = "19", sublevel: str = "113",
              device_config: str = "y2q_usa_singlew_defconfig",
              selector_values: str = "CONFIG_MACH_Y2Q_USA_SINGLE=y\nCONFIG_SEC_Y2Q_PROJECT=y\n") -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "Makefile").write_text(
        f"VERSION = {version}\nPATCHLEVEL = {patchlevel}\nSUBLEVEL = {sublevel}\nEXTRAVERSION = \n",
        encoding="utf-8",
    )
    for path in ("kernel", "arch/arm64", "drivers", "include/linux", "arch/arm64/configs/vendor"):
        (root / path).mkdir(parents=True, exist_ok=True)
    (root / "arch/arm64/configs/vendor" / device_config).write_text(selector_values, encoding="utf-8")


class SamsungKernelSourceTests(unittest.TestCase):
    def test_profile_declares_expected_4_19_113_source_baseline(self) -> None:
        profile = module.load_profile(PROFILE_PATH)
        self.assertEqual(
            (profile["kernel_source"]["expected_version"],
             profile["kernel_source"]["expected_patchlevel"],
             profile["kernel_source"]["expected_sublevel"]),
            (4, 19, 113),
        )
        self.assertEqual(profile["device"]["model"], "SM-G986U1")
        self.assertEqual(profile["kernel_source"]["preferred_device_config"],
                         "vendor/y2q_usa_singlew_defconfig")

    def test_matching_source_tree_passes_baseline_and_target_defconfig(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "kernel"
            make_tree(root)
            checks, _, candidates = module.inspect_source_tree(root, PROFILE)
        statuses = {(name, level) for level, name, _ in checks}
        self.assertIn(("kernel version", "PASS"), statuses)
        self.assertIn(("source tree layout", "PASS"), statuses)
        self.assertIn(("preferred device config", "PASS"), statuses)
        self.assertIn(("defconfig selector CONFIG_MACH_Y2Q_USA_SINGLE", "PASS"), statuses)
        self.assertIn(("defconfig selector CONFIG_SEC_Y2Q_PROJECT", "PASS"), statuses)
        self.assertEqual(candidates, ["arch/arm64/configs/vendor/y2q_usa_singlew_defconfig"])

    def test_conflicting_target_selector_fails_source_check(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "kernel"
            make_tree(root, selector_values="CONFIG_MACH_Y2Q_USA_SINGLE=y\n# CONFIG_SEC_Y2Q_PROJECT is not set\n")
            checks, _, _ = module.inspect_source_tree(root, PROFILE)
        selector = next(item for item in checks if item[1] == "defconfig selector CONFIG_SEC_Y2Q_PROJECT")
        self.assertEqual(selector[0], "FAIL")
        self.assertIn("found=n; expected=y", selector[2])

    def test_unspecified_target_selector_is_reported_without_false_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "kernel"
            make_tree(root, selector_values="CONFIG_MACH_Y2Q_USA_SINGLE=y\n")
            checks, _, _ = module.inspect_source_tree(root, PROFILE)
        selector = next(item for item in checks if item[1] == "defconfig selector CONFIG_SEC_Y2Q_PROJECT")
        self.assertEqual(selector[0], "INFO")
        self.assertIn("generated .config", selector[2])

    def test_wrong_kernel_version_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "kernel"
            make_tree(root, sublevel="157")
            checks, _, _ = module.inspect_source_tree(root, PROFILE)
        version_check = next(item for item in checks if item[1] == "kernel version")
        self.assertEqual(version_check[0], "FAIL")
        self.assertIn("found=4.19.157", version_check[2])

    def test_missing_required_kernel_paths_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "kernel"
            root.mkdir()
            (root / "Makefile").write_text("VERSION = 4\nPATCHLEVEL = 19\nSUBLEVEL = 113\n", encoding="utf-8")
            checks, _, _ = module.inspect_source_tree(root, PROFILE)
        layout_check = next(item for item in checks if item[1] == "source tree layout")
        self.assertEqual(layout_check[0], "FAIL")
        self.assertIn("arch/arm64", layout_check[2])

    def test_missing_preferred_device_defconfig_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "kernel"
            make_tree(root, device_config="vendor_generic_defconfig")
            checks, _, candidates = module.inspect_source_tree(root, PROFILE)
        config_check = next(item for item in checks if item[1] == "device config candidates")
        preferred_check = next(item for item in checks if item[1] == "preferred device config")
        self.assertEqual(config_check[0], "WARN")
        self.assertEqual(preferred_check[0], "FAIL")
        self.assertEqual(candidates, [])

    def test_malformed_profile_without_kernel_source_fails(self) -> None:
        profile = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        del profile["kernel_source"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.json"
            path.write_text(json.dumps(profile), encoding="utf-8")
            with self.assertRaises(ValueError):
                module.load_profile(path)

    def test_preferred_config_path_must_be_safe_relative_path(self) -> None:
        profile = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        profile["kernel_source"]["preferred_device_config"] = "../other_defconfig"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.json"
            path.write_text(json.dumps(profile), encoding="utf-8")
            with self.assertRaises(ValueError):
                module.load_profile(path)

    def test_version_parser_reads_required_values(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            makefile = Path(tmp) / "Makefile"
            makefile.write_text(
                "# kernel\nVERSION = 4\nPATCHLEVEL = 19\nSUBLEVEL = 113\nEXTRAVERSION = -vendor\n",
                encoding="utf-8",
            )
            self.assertEqual(
                module.parse_kernel_version(makefile),
                {"VERSION": "4", "PATCHLEVEL": "19", "SUBLEVEL": "113", "EXTRAVERSION": "-vendor"},
            )

    def test_config_parser_preserves_explicit_disabled_setting(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / "defconfig"
            config.write_text("# CONFIG_SEC_Y2Q_PROJECT is not set\nCONFIG_MACH_Y2Q_USA_SINGLE=y\n", encoding="utf-8")
            values = module.parse_config_values(config)
        self.assertEqual(values["CONFIG_SEC_Y2Q_PROJECT"], "n")
        self.assertEqual(values["CONFIG_MACH_Y2Q_USA_SINGLE"], "y")


if __name__ == "__main__":
    unittest.main()
