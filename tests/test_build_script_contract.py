#!/usr/bin/env python3
"""Behavioural tests for build.sh's failure contract.

build.sh is the only supported build entry point, so a failed compiler or a
missing artifact has to leave it with a nonzero status.  These tests run the
real script against a stub `make` on PATH, so they need no NDK and no device,
and they assert the contract rather than grepping the script for `set -e`.
"""
from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "build.sh"

# A fake make: it reads the PROJECT= argument the way the real rule does and
# then either fails, succeeds without producing anything, or produces both
# artifacts build.sh copies.
STUB_MAKE = """#!/usr/bin/env bash
set -u
proj=
for a in "$@"; do
  case "$a" in
    PROJECT=*) proj="${a#PROJECT=}" ;;
  esac
done
case "${STUB_MODE:-ok}" in
  fail)
    echo "make: *** [Makefile:238: preload.so] Error 1" >&2
    exit 2
    ;;
  empty)
    exit 0
    ;;
  ok)
    mkdir -p "build/$proj/bin" build/embed
    printf 'stub-preload\\n' > "build/$proj/bin/preload.so"
    printf 'stub-daemon\\n' > build/embed/su_daemon_aarch64_pie
    exit 0
    ;;
esac
exit 1
"""

PROJECT = "samsung-g986u1-4.19.113"


class BuildScriptContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        bindir = self.dir / "bin"
        bindir.mkdir()
        stub = bindir / "make"
        stub.write_text(STUB_MAKE, encoding="utf-8")
        stub.chmod(stub.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        # build.sh cds to its own directory, so a copy runs against this temp
        # root and never touches the repository.
        self.script = self.dir / "build.sh"
        self.script.write_text(SCRIPT.read_text(encoding="utf-8"), encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_build(self, mode: str) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        env["PATH"] = f"{self.dir / 'bin'}{os.pathsep}{env.get('PATH', '')}"
        env["STUB_MODE"] = mode
        env["PROJECT"] = PROJECT
        return subprocess.run(
            ["bash", str(self.script)],
            cwd=self.dir,
            env=env,
            capture_output=True,
            text=True,
        )

    def test_compiler_failure_is_not_swallowed(self) -> None:
        result = self.run_build("fail")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Error 1", result.stderr)
        # Nothing may be published as if the build had succeeded.
        self.assertFalse((self.dir / "prebuilt" / "preload.so").exists())

    def test_missing_artifact_fails(self) -> None:
        result = self.run_build("empty")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_success_packages_both_artifacts(self) -> None:
        result = self.run_build("ok")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            (self.dir / "prebuilt" / "preload.so").read_text(encoding="utf-8"),
            "stub-preload\n",
        )
        self.assertEqual(
            (self.dir / "prebuilt" / "su_daemon_aarch64_pie").read_text(encoding="utf-8"),
            "stub-daemon\n",
        )


if __name__ == "__main__":
    unittest.main()
