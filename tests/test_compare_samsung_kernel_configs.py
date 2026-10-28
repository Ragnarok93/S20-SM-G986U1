#!/usr/bin/env python3
"""Tests for conservative stock/source Samsung kernel-config comparison."""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/compare_samsung_kernel_configs.py"
SPEC = importlib.util.spec_from_file_location("compare_samsung_kernel_configs", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot import checker at {MODULE_PATH}")
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)


class CompareSamsungKernelConfigsTests(unittest.TestCase):
    def test_parser_records_explicitly_disabled_but_not_unlisted_symbols(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ".config"
            path.write_text(
                "CONFIG_FUTEX=y\n# CONFIG_FUTEX_PI is not set\nCONFIG_LOCALVERSION=\n",
                encoding="utf-8",
            )
            parsed = module.parse_config(path)
        self.assertEqual(parsed["CONFIG_FUTEX"], "y")
        self.assertEqual(parsed["CONFIG_FUTEX_PI"], "n")
        self.assertEqual(parsed["CONFIG_LOCALVERSION"], "")
        self.assertNotIn("CONFIG_SECURITY_SELINUX", parsed)

    def test_exact_match_has_no_deltas(self) -> None:
        cfg = {
            "CONFIG_ARM64_VA_BITS_39": "y",
            "CONFIG_FUTEX": "y",
            "CONFIG_FUTEX_PI": "y",
            "CONFIG_SECURITY_SELINUX": "y",
            "CONFIG_RKP": "y",
        }
        result = module.compare_configs(cfg, dict(cfg))
        self.assertEqual(result["critical_differences"], [])
        self.assertEqual(result["critical_unknowns"], [])
        self.assertEqual(result["differences"], [])
        self.assertEqual(result["stock_only"], [])
        self.assertEqual(result["candidate_only"], [])

    def test_device_selection_changes_are_separated_from_core_deltas(self) -> None:
        stock = {
            "CONFIG_ARM64_VA_BITS_39": "y",
            "CONFIG_FUTEX_PI": "y",
            "CONFIG_SECURITY_SELINUX": "y",
            "CONFIG_ARCH_QCOM": "n",
            "CONFIG_MACH_Y2Q": "y",
        }
        candidate = dict(stock)
        candidate["CONFIG_ARCH_QCOM"] = "y"
        candidate["CONFIG_MACH_Y2Q"] = "n"
        result = module.compare_configs(stock, candidate)
        self.assertEqual(result["critical_differences"], [])
        self.assertEqual(result["critical_unknowns"], [])
        self.assertEqual(len(result["noncritical_differences"]), 2)
        self.assertEqual({item[3] for item in result["noncritical_differences"]}, {"device-selection"})

    def test_va_layout_change_is_a_core_difference(self) -> None:
        stock = {"CONFIG_ARM64_VA_BITS_39": "y", "CONFIG_FUTEX_PI": "y"}
        candidate = {"CONFIG_ARM64_VA_BITS_39": "n", "CONFIG_FUTEX_PI": "y"}
        result = module.compare_configs(stock, candidate)
        self.assertEqual(len(result["critical_differences"]), 1)
        self.assertEqual(result["critical_differences"][0][0], "CONFIG_ARM64_VA_BITS_39")
        self.assertEqual(result["critical_differences"][0][3], "arm64-memory-layout")

    def test_missing_core_symbol_is_unknown_not_assumed_disabled(self) -> None:
        stock = {"CONFIG_SECURITY_SELINUX": "y", "CONFIG_FUTEX": "y"}
        candidate = {"CONFIG_FUTEX": "y"}
        result = module.compare_configs(stock, candidate)
        self.assertEqual(result["critical_differences"], [])
        self.assertIn(("CONFIG_SECURITY_SELINUX", "stock-only"), result["critical_unknowns"])

    def test_security_symbols_classify_as_core(self) -> None:
        for symbol in ("CONFIG_RKP", "CONFIG_KDP", "CONFIG_SECURITY_DEFEX", "CONFIG_PROCA",
                       "CONFIG_CFP", "CONFIG_SECURITY_SELINUX", "CONFIG_SEC_RKP",
                       "CONFIG_SEC_RESTRICT_ROOTING"):
            with self.subTest(symbol=symbol):
                self.assertEqual(module.classify_symbol(symbol), "security-hardening")

    def test_soc_selection_delta_is_core(self) -> None:
        stock = {"CONFIG_ARCH_QCOM": "y", "CONFIG_ARM64_VA_BITS_39": "y"}
        candidate = {"CONFIG_ARCH_QCOM": "n", "CONFIG_ARM64_VA_BITS_39": "y"}
        result = module.compare_configs(stock, candidate)
        self.assertEqual(len(result["critical_differences"]), 1)
        self.assertEqual(result["critical_differences"][0][0], "CONFIG_ARCH_QCOM")
        self.assertEqual(result["critical_differences"][0][3], "soc-platform")

    def test_futex_and_debug_symbols_classify_as_core(self) -> None:
        self.assertEqual(module.classify_symbol("CONFIG_FUTEX_PI"), "futex-scheduler")
        self.assertEqual(module.classify_symbol("CONFIG_RT_MUTEXES"), "futex-scheduler")
        self.assertEqual(module.classify_symbol("CONFIG_KALLSYMS_ALL"), "symbols-debug-metadata")


if __name__ == "__main__":
    unittest.main()
