#!/usr/bin/env python3
"""Regression tests for the read-only Samsung stock-kernel port preflight."""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/verify_stock_kernel_port.py"
PROFILE_PATH = ROOT / "src/targets/samsung-g986u1-4.19.113/stock-kernel-port-profile.json"
SPEC = importlib.util.spec_from_file_location("verify_stock_kernel_port", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import verifier at {MODULE_PATH}")
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)
PROFILE = module.load_profile(PROFILE_PATH)


def make_tree(root: Path, version: str = "4.19.113", config: str | None = None) -> tuple[Path, Path]:
    root.mkdir(parents=True, exist_ok=True)
    major, minor, patch = version.split(".")
    (root / "Makefile").write_text(f"VERSION = {major}\nPATCHLEVEL = {minor}\nSUBLEVEL = {patch}\n", encoding="utf-8")
    for rel in ("kernel", "arch/arm64", "drivers", "include/linux"):
        (root / rel).mkdir(parents=True, exist_ok=True)
    config_path = root / "out-stock-layout/.config"
    config_path.parent.mkdir(parents=True, exist_ok=True)
    if config is None:
        config = "\n".join(f"{key}={value}" for key, value in PROFILE["stock_config_expectations"].items()) + "\n"
    config_path.write_text(config, encoding="utf-8")
    return root, config_path


class StockKernelPortPreflightTests(unittest.TestCase):
    def test_profile_is_bound_to_device_and_stock_kernel(self) -> None:
        self.assertEqual(PROFILE["device"]["model"], "SM-G986U1")
        self.assertEqual(PROFILE["device"]["codename"], "y2q")
        self.assertEqual(PROFILE["kernel"]["version"], "4.19.113")
        self.assertFalse(PROFILE["port_scope"]["kernel_modification"])
        self.assertFalse(PROFILE["port_scope"]["kernel_rebuild"])

    def test_stock_baseline_passes_without_any_kernelsu_files_or_config(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, config = make_tree(Path(tmp) / "kernel")
            checks, failures = module.inspect(root, config, PROFILE)
        self.assertEqual(failures, 0)
        self.assertIn(("PASS", "kernel version", "found=4.19.113; expected=4.19.113"), checks)
        self.assertFalse(any("KernelSU" in name or "KSU" in name for _, name, _ in checks))

    def test_wrong_kernel_version_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, config = make_tree(Path(tmp) / "kernel", version="4.19.157")
            checks, failures = module.inspect(root, config, PROFILE)
        self.assertGreater(failures, 0)
        self.assertEqual(next(item for item in checks if item[1] == "kernel version")[0], "FAIL")

    def test_stock_config_mismatch_fails(self) -> None:
        config = "\n".join(
            f"{key}={'n' if key == 'CONFIG_FUTEX_PI' else value}"
            for key, value in PROFILE["stock_config_expectations"].items()
        ) + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            root, config_path = make_tree(Path(tmp) / "kernel", config=config)
            checks, failures = module.inspect(root, config_path, PROFILE)
        self.assertGreater(failures, 0)
        item = next(item for item in checks if item[1] == "stock baseline CONFIG_FUTEX_PI")
        self.assertEqual(item[0], "FAIL")

    def test_missing_config_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, config = make_tree(Path(tmp) / "kernel")
            config.unlink()
            checks, failures = module.inspect(root, config, PROFILE)
        self.assertGreater(failures, 0)
        self.assertTrue(any(name == "stock build config" and status == "FAIL" for status, name, _ in checks))


if __name__ == "__main__":
    unittest.main()
