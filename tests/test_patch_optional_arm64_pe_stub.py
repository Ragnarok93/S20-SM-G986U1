#!/usr/bin/env python3
"""Regression tests for the optional ARM64 PE/COFF stub patcher."""
from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHER_PATH = ROOT / "tools" / "patch_optional_arm64_pe_stub.py"
SPEC = importlib.util.spec_from_file_location("patch_optional_arm64_pe_stub", PATCHER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import patcher at {PATCHER_PATH}")
patcher = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = patcher
SPEC.loader.exec_module(patcher)


def make_fixture_tree(root: Path) -> dict[Path, str]:
    originals: dict[Path, str] = {}
    for relative in patcher.RELATIVE_FILES:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        source = "def extract_boot_kernel(path):\n" + patcher.OLD + "    return None\n"
        compile(source, str(path), "exec")
        path.write_text(source, encoding="utf-8")
        originals[path] = source
    return originals


class PatchOptionalArm64PeStubTests(unittest.TestCase):
    def test_check_mode_does_not_modify_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = patcher.main(["--root", str(root), "--check"])

            self.assertEqual(result, 0)
            self.assertIn("CHECK=PASS", output.getvalue())
            self.assertIn("patchable", output.getvalue())
            for path, source in originals.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-optional-pe-stub.bak").exists())

    def test_apply_creates_backups_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            output = io.StringIO()
            with redirect_stdout(output):
                result = patcher.main(["--root", str(root)])

            self.assertEqual(result, 0)
            self.assertIn("RESULT=PATCHED", output.getvalue())
            for path, source in originals.items():
                backup = path.with_name(path.name + ".pre-optional-pe-stub.bak")
                updated = path.read_text(encoding="utf-8")
                self.assertEqual(backup.read_text(encoding="utf-8"), source)
                self.assertIn(patcher.PATCHED_MARKER, updated)
                self.assertNotIn(patcher.OLD, updated)
                compile(updated, str(path), "exec")

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

    def test_missing_or_ambiguous_source_fails_before_writes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            first = next(iter(originals))
            first.write_text(originals[first] + patcher.OLD, encoding="utf-8")
            before = {
                path: path.read_text(encoding="utf-8")
                for path in originals if path != first
            }
            errors = io.StringIO()
            with redirect_stderr(errors):
                result = patcher.main(["--root", str(root)])

            self.assertEqual(result, 1)
            self.assertIn("unexpected parser source", errors.getvalue())
            self.assertFalse(first.with_name(first.name + ".pre-optional-pe-stub.bak").exists())
            for path, source in before.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                self.assertFalse(path.with_name(path.name + ".pre-optional-pe-stub.bak").exists())

    def test_existing_backup_prevents_partial_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = make_fixture_tree(root)
            second = list(originals)[1]
            second.with_name(second.name + ".pre-optional-pe-stub.bak").write_text(
                "existing backup", encoding="utf-8"
            )
            errors = io.StringIO()
            with redirect_stderr(errors):
                result = patcher.main(["--root", str(root)])

            self.assertEqual(result, 1)
            self.assertIn("backup already exists", errors.getvalue())
            for path, source in originals.items():
                self.assertEqual(path.read_text(encoding="utf-8"), source)
                if path != second:
                    self.assertFalse(path.with_name(path.name + ".pre-optional-pe-stub.bak").exists())


if __name__ == "__main__":
    unittest.main()
