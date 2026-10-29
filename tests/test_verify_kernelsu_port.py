#!/usr/bin/env python3
"""Regression tests for the read-only Samsung KernelSU port preflight."""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/verify_kernelsu_port.py"
PROFILE_PATH = ROOT / "src/targets/samsung-g986u1-4.19.113/kernelsu-port-profile.json"
SPEC = importlib.util.spec_from_file_location("verify_kernelsu_port", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import verifier at {MODULE_PATH}")
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)
PROFILE = module.load_profile(PROFILE_PATH)


def make_tree(root: Path, version: str = "4.19.113", config: str | None = None,
              with_kernelsu: bool = False) -> tuple[Path, Path]:
    root.mkdir(parents=True, exist_ok=True)
    major, minor, patch = version.split(".")
    (root / "Makefile").write_text(
        f"VERSION = {major}\nPATCHLEVEL = {minor}\nSUBLEVEL = {patch}\n",
        encoding="utf-8",
    )
    for rel in ("kernel", "arch/arm64", "drivers", "include/linux"):
        (root / rel).mkdir(parents=True, exist_ok=True)
    if with_kernelsu:
        (root / "drivers/kernelsu").mkdir(parents=True, exist_ok=True)
    config_path = root / "out-stock-layout/.config"
    config_path.parent.mkdir(parents=True, exist_ok=True)
    if config is None:
        config = "\n".join(f"{key}={value}" for key, value in PROFILE["stock_config_expectations"].items()) + "\n"
    if with_kernelsu:
        config += "CONFIG_KSU=y\n"
    config_path.write_text(config, encoding="utf-8")
    return root, config_path


class KernelSUPortPreflightTests(unittest.TestCase):
    def test_profile_is_bound_to_sm_g986u1_stock_kernel(self) -> None:
        self.assertEqual(PROFILE["device"]["model"], "SM-G986U1")
        self.assertEqual(PROFILE["device"]["codename"], "y2q")
        self.assertEqual(PROFILE["kernel"]["version"], "4.19.113")
        self.assertEqual(PROFILE["integration"]["config_symbol"], "CONFIG_KSU")

    def test_stock_baseline_passes_and_unintegrated_kernelsu_is_informational(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, config = make_tree(Path(tmp) / "kernel")
            checks, failures = module.inspect(root, config, PROFILE)
        self.assertEqual(failures, 0)
        self.assertIn(("PASS", "kernel version", "found=4.19.113; expected=4.19.113"), checks)
        ksu = next(item for item in checks if item[1] == "KernelSU source/config")
        self.assertEqual(ksu[0], "INFO")

    def test_require_kernelsu_fails_when_source_or_config_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, config = make_tree(Path(tmp) / "kernel")
            checks, failures = module.inspect(root, config, PROFILE, require_kernelsu=True)
        self.assertGreater(failures, 0)
        self.assertEqual(next(item for item in checks if item[1] == "KernelSU source/config")[0], "FAIL")

    def test_integrated_source_and_config_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, config = make_tree(Path(tmp) / "kernel", with_kernelsu=True)
            checks, failures = module.inspect(root, config, PROFILE, require_kernelsu=True)
        self.assertEqual(failures, 0)
        self.assertEqual(next(item for item in checks if item[1] == "KernelSU source/config")[0], "PASS")

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


if __name__ == "__main__":
    unittest.main()
