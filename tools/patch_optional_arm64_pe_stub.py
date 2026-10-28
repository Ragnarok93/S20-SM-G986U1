#!/usr/bin/env python3
"""Patch optional ARM64 EFI PE/COFF stub validation in a generator checkout.

This narrowly adjusts extract_boot_kernel() in both vendored parser modules:
the mandatory ARM64 Image magic remains required, but the optional PE/COFF
header is checked only when the Image header supplies a nonzero PE offset.
The script never runs the generator or creates a target profile.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path


RELATIVE_FILES = (
    Path("tools/vendor/generate_target.py"),
    Path("tools/vendor/generate_target_2.py"),
)

OLD = r'''    if pe_offset >= len(kernel) or pe_offset & 3:
        fail(f"PE offset 非法: 0x{pe_offset:x}")
    if kernel[pe_offset:pe_offset + 4] != b"PE\x00\x00":
        fail("arm64 Image 的 PE/COFF stub magic 不匹配")
'''

NEW = r'''    # The ARM64 PE/COFF header is optional for a non-EFI boot path.
    # A zero offset means no PE stub is present; validate it strictly if set.
    if pe_offset:
        if pe_offset >= len(kernel) or pe_offset & 3:
            fail(f"PE offset 非法: 0x{pe_offset:x}")
        if kernel[pe_offset:pe_offset + 4] != b"PE\x00\x00":
            fail("arm64 Image 的 PE/COFF stub magic 不匹配")
'''

PATCHED_MARKER = "The ARM64 PE/COFF header is optional for a non-EFI boot path."


class PatchError(RuntimeError):
    pass


def plan(root: Path) -> list[tuple[Path, str, bool]]:
    plans: list[tuple[Path, str, bool]] = []
    for relative in RELATIVE_FILES:
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise PatchError(f"target must be a regular non-symlink file: {path}")
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise PatchError(f"cannot read {path}: {exc}") from exc
        old_count = source.count(OLD)
        patched_count = source.count(PATCHED_MARKER)
        if old_count == 1 and patched_count == 0:
            plans.append((path, source.replace(OLD, NEW, 1), False))
        elif old_count == 0 and patched_count == 1:
            plans.append((path, source, True))
        else:
            raise PatchError(
                f"unexpected parser source in {path}: old_block={old_count}, "
                f"patched_marker={patched_count}; no files changed"
            )
    return plans


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("/tmp/ghostlock-profile-gen"))
    parser.add_argument("--check", action="store_true",
                        help="validate that both source files match expected pre-patch or patched content")
    args = parser.parse_args(argv)
    try:
        plans = plan(args.root)
        if args.check:
            for path, _updated, already in plans:
                state = "already patched" if already else "patchable"
                print(f"{state}: {path}")
            print("CHECK=PASS (no files changed)")
            return 0

        # Complete preflight above before any writes; refuse to overwrite backups.
        for path, _updated, already in plans:
            if not already and path.with_name(path.name + ".pre-optional-pe-stub.bak").exists():
                raise PatchError(
                    f"backup already exists; inspect it before retrying: "
                    f"{path.with_name(path.name + '.pre-optional-pe-stub.bak')}"
                )
        for path, updated, already in plans:
            if already:
                print(f"already patched: {path}")
                continue
            backup = path.with_name(path.name + ".pre-optional-pe-stub.bak")
            shutil.copy2(path, backup)
            temporary = path.with_name(path.name + ".tmp-optional-pe-stub")
            try:
                temporary.write_text(updated, encoding="utf-8", newline="")
                os.replace(temporary, path)
            finally:
                if temporary.exists():
                    temporary.unlink()
            print(f"patched: {path}")
            print(f"backup:  {backup}")
        print("RESULT=PATCHED (optional PE stub check; other parser checks unchanged)")
        return 0
    except (OSError, PatchError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
