#!/usr/bin/env python3
"""Tests for the legacy Android boot v0-v2 parser patcher."""
from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHER_PATH = ROOT / "tools" / "patch_legacy_boot_v2.py"
SPEC = importlib.util.spec_from_file_location("patch_legacy_boot_v2", PATCHER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import patcher at {PATCHER_PATH}")
patcher = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = patcher
SPEC.loader.exec_module(patcher)


def run_patcher(args: list[str]) -> int:
    with patch.object(sys, "argv", ["patch_legacy_boot_v2.py", *args]):
        return patcher.main()


def make_fixture_tree(root: Path) -> dict[Path, str]:
    originals: dict[Path, str] = {}
    for relative in patcher.TARGETS:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        source = "def extract_boot_kernel(path):\n" + patcher.OLD_BLOCK + "    return None\n"
        compile(source, str(path), "exec")
        path.write_text(source, encoding="utf-8")
        originals[path] = source
    return originals


class PatchLegacyBootV2Tests(unittest.TestCase):
    def test_check_mode_does_not_modify_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = run_patcher(["--root", str(root), "--check"])

            self.assertEqual(result, 0)
            self.assertIn("CHECK_OK:", output.getvalue())
            self.assertIn("READY", output.getvalue())
            for path, source in originals.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-legacy-v2.bak").exists())

    def test_apply_backups_both_parsers_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = run_patcher(["--root", str(root)])

            self.assertEqual(result, 0)
            self.assertIn("DONE:", output.getvalue())
            for path, source in originals.items():
                backup = path.with_name(path.name + ".pre-legacy-v2.bak")
                updated = path.read_text(encoding="utf-8")
                self.assertEqual(backup.read_text(encoding="utf-8"), source)
                self.assertIn(patcher.MARKER, updated)
                self.assertNotIn(patcher.OLD_BLOCK, updated)
                compile(updated, str(path), "exec")

            check_output = io.StringIO()
            with redirect_stdout(check_output):
                check_result = run_patcher(["--root", str(root), "--check"])
            self.assertEqual(check_result, 0)
            self.assertIn("ALREADY_PATCHED", check_output.getvalue())
            self.assertIn("CHECK_OK: both parser files are already patched", check_output.getvalue())

            repeat_output = io.StringIO()
            with redirect_stdout(repeat_output):
                repeat_result = run_patcher(["--root", str(root)])
            self.assertEqual(repeat_result, 0)
            self.assertIn("ALREADY_PATCHED", repeat_output.getvalue())

    def test_no_backup_option_is_respected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = run_patcher(["--root", str(root), "--no-backup"])

            self.assertEqual(result, 0)
            for path, source in originals.items():
                self.assertNotEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-legacy-v2.bak").exists())

    def test_preflight_failure_changes_no_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            missing = next(iter(originals))
            missing.unlink()
            before = {
                path: path.read_text(encoding="utf-8")
                for path in originals if path != missing
            }

            errors = io.StringIO()
            with redirect_stderr(errors):
                result = run_patcher(["--root", str(root)])

            self.assertEqual(result, 2)
            self.assertIn("Preflight failed", errors.getvalue())
            for path, source in before.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-legacy-v2.bak").exists())

    def test_duplicate_source_block_is_rejected_before_any_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            first = next(iter(originals))
            first.write_text(originals[first] + patcher.OLD_BLOCK, encoding="utf-8")
            before = {
                path: path.read_text(encoding="utf-8")
                for path in originals if path != first
            }
            errors = io.StringIO()
            with redirect_stderr(errors):
                result = run_patcher(["--root", str(root)])

            self.assertEqual(result, 2)
            self.assertIn("old-block occurrences=2", errors.getvalue())
            self.assertFalse(first.with_name(first.name + ".pre-legacy-v2.bak").exists())
            for path, source in before.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-legacy-v2.bak").exists())


if __name__ == "__main__":
    unittest.main()
