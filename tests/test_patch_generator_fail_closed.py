#!/usr/bin/env python3
"""Tests for the fail-closed generator patch utility.

Only temporary source fixtures are patched; no external generator checkout or
profile output is touched by this test suite.
"""
from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHER_PATH = ROOT / "tools" / "patch_generator_fail_closed.py"
SPEC = importlib.util.spec_from_file_location("patch_generator_fail_closed", PATCHER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import patcher at {PATCHER_PATH}")
patcher = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = patcher
SPEC.loader.exec_module(patcher)


def make_fixture_tree(root: Path) -> dict[Path, str]:
    originals: dict[Path, str] = {}
    for relative, replacements in patcher.PATCHES.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        source = "# test fixture before\n" + "\n".join(item.old.rstrip("\n") for item in replacements) + "\n# test fixture after\n"
        path.write_text(source, encoding="utf-8")
        originals[path] = source
    return originals


class PatchGeneratorFailClosedTests(unittest.TestCase):
    def test_check_mode_does_not_write_or_create_backups(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = patcher.main(["--root", str(root), "--check"])

            self.assertEqual(result, 0)
            self.assertIn("CHECK=PASS", output.getvalue())
            for path, original in originals.items():
                self.assertEqual(path.read_text(encoding="utf-8"), original)
                self.assertFalse(path.with_name(path.name + ".pre-fail-closed.bak").exists())

    def test_apply_creates_backups_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = patcher.main(["--root", str(root)])

            self.assertEqual(result, 0)
            self.assertIn("RESULT=PATCHED", output.getvalue())
            for path, original in originals.items():
                backup = path.with_name(path.name + ".pre-fail-closed.bak")
                updated = path.read_text(encoding="utf-8")
                self.assertEqual(backup.read_text(encoding="utf-8"), original)
                self.assertNotEqual(updated, original)
                for replacement in patcher.PATCHES[path.relative_to(root)]:
                    self.assertNotIn(replacement.old, updated)
                    self.assertIn(replacement.marker, updated)

            check_output = io.StringIO()
            with redirect_stdout(check_output):
                check_result = patcher.main(["--root", str(root), "--check"])
            self.assertEqual(check_result, 0)
            self.assertIn("already patched", check_output.getvalue())

            repeat_output = io.StringIO()
            with redirect_stdout(repeat_output):
                repeat_result = patcher.main(["--root", str(root)])
            self.assertEqual(repeat_result, 0)
            self.assertIn("already patched", repeat_output.getvalue())

    def test_preflight_failure_changes_no_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            missing = root / next(iter(patcher.PATCHES)).as_posix()
            missing.unlink()
            before = {
                path: path.read_text(encoding="utf-8")
                for path in originals
                if path != missing
            }

            errors = io.StringIO()
            with redirect_stderr(errors):
                result = patcher.main(["--root", str(root)])

            self.assertEqual(result, 1)
            self.assertIn("no files changed", errors.getvalue())
            for path, source in before.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-fail-closed.bak").exists())


if __name__ == "__main__":
    unittest.main()
