#!/usr/bin/env python3
"""Patch the external generator's ARM64 boot parser for legacy Android headers.

The patch adds strict Android boot-header v0/v1/v2 page-size handling and
allows an ARM64 Image whose optional PE/COFF EFI-stub offset is zero. Modern
v3/v4 header-size validation and nonzero PE-offset validation remain strict.

This utility only modifies parser source in an external generator checkout. It
does not run the generator, inspect kernel symbols, derive offsets, or create
a target profile. All source blocks are preflighted before any file is written.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path
from typing import NamedTuple


class Replacement(NamedTuple):
    old: str
    new: str
    marker: str


LEGACY_HEADER_BLOCK = '''    kernel_size = u32(boot, 8)
    header_size = u32(boot, 20)
    header_version = u32(boot, 40)
    expected_header_sizes = {3: 0x62C, 4: 0x630}
    if header_version not in expected_header_sizes:
        fail(f"只支持 Android boot header v3/v4，实际 v{header_version}")
    if header_size != expected_header_sizes[header_version]:
        fail(
            f"boot header_size 与 v{header_version} 不符: "
            f"0x{header_size:x} != 0x{expected_header_sizes[header_version]:x}"
        )
    kernel_offset = align_up(header_size, 4096)
'''

LEGACY_HEADER_PATCH = '''    kernel_size = u32(boot, 8)
    header_version = u32(boot, 40)
    # Legacy Android boot header v0/v1/v2 compatibility.
    if header_version in (0, 1, 2):
        page_size = u32(boot, 36)
        if page_size < 2048 or page_size > 65536 or page_size & (page_size - 1):
            fail(f"invalid legacy boot page_size: 0x{page_size:x}")
        kernel_offset = page_size
    elif header_version in (3, 4):
        header_size = u32(boot, 20)
        expected_header_sizes = {3: 0x62C, 4: 0x630}
        if header_size != expected_header_sizes[header_version]:
            fail(
                f"boot header_size 与 v{header_version} 不符: "
                f"0x{header_size:x} != 0x{expected_header_sizes[header_version]:x}"
            )
        kernel_offset = align_up(header_size, 4096)
    else:
        fail(f"unsupported Android boot header version: v{header_version}")
'''

PE_STUB_BLOCK = '''    if pe_offset >= len(kernel) or pe_offset & 3:
        fail(f"PE offset 非法: 0x{pe_offset:x}")
    if kernel[pe_offset:pe_offset + 4] != b"PE\\x00\\x00":
        fail("arm64 Image 的 PE/COFF stub magic 不匹配")
'''

PE_STUB_PATCH = '''    # The ARM64 Image may omit the optional EFI PE/COFF stub (offset zero).
    if pe_offset != 0:
        if pe_offset >= len(kernel) or pe_offset & 3:
            fail(f"PE offset 非法: 0x{pe_offset:x}")
    if kernel[pe_offset:pe_offset + 4] != b"PE\\x00\\x00":
            fail("arm64 Image 的 PE/COFF stub magic 不匹配")
'''

PATCHES: dict[Path, tuple[Replacement, ...]] = {
    Path("tools/vendor/generate_target.py"): (
        Replacement(
            LEGACY_HEADER_BLOCK,
            LEGACY_HEADER_PATCH,
            "# Legacy Android boot header v0/v1/v2 compatibility.",
        ),
        Replacement(
            PE_STUB_BLOCK,
            PE_STUB_PATCH,
            "# The ARM64 Image may omit the optional EFI PE/COFF stub (offset zero).",
        ),
    ),
    Path("tools/vendor/generate_target_2.py"): (
        Replacement(
            LEGACY_HEADER_BLOCK,
            LEGACY_HEADER_PATCH,
            "# Legacy Android boot header v0/v1/v2 compatibility.",
        ),
        Replacement(
            PE_STUB_BLOCK,
            PE_STUB_PATCH,
            "# The ARM64 Image may omit the optional EFI PE/COFF stub (offset zero).",
        ),
    ),
}


class PatchError(RuntimeError):
    pass


def build_plan(root: Path) -> list[tuple[Path, str, bool]]:
    plan: list[tuple[Path, str, bool]] = []
    # Validate the entire checkout before modifying either parser.
    for relative, replacements in PATCHES.items():
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise PatchError(f"target must be a regular non-symlink file: {path}")
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise PatchError(f"cannot read {path}: {exc}") from exc

        updated = source
        file_already_patched = True
        for replacement in replacements:
            old_count = updated.count(replacement.old)
            marker_count = updated.count(replacement.marker)
            if old_count == 1 and marker_count == 0:
                updated = updated.replace(replacement.old, replacement.new, 1)
                file_already_patched = False
            elif old_count == 0 and marker_count == 1:
                continue
            else:
                raise PatchError(
                    f"unexpected source in {path}: old-block count={old_count}, "
                    f"replacement-marker count={marker_count} for "
                    f"{replacement.marker!r}; no files changed"
                )
        plan.append((path, updated, file_already_patched))
    return plan


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("/tmp/ghostlock-profile-gen"),
        help="external generator checkout containing tools/vendor/",
    )
    parser.add_argument("--check", action="store_true", help="preflight only; do not modify files")
    args = parser.parse_args(argv)

    try:
        plan = build_plan(args.root)
        if args.check:
            for path, _updated, already in plan:
                print(f"{'already patched' if already else 'patchable'}: {path}")
            print("CHECK=PASS (no files changed)")
            return 0

        for path, _updated, already in plan:
            backup = path.with_name(path.name + ".pre-legacy-boot.bak")
            if not already and backup.exists():
                raise PatchError(f"backup already exists; inspect before retrying: {backup}")

        for path, updated, already in plan:
            if already:
                print(f"already patched: {path}")
                continue
            backup = path.with_name(path.name + ".pre-legacy-boot.bak")
            temporary = path.with_name(path.name + ".tmp-legacy-boot")
            shutil.copy2(path, backup)
            try:
                temporary.write_text(updated, encoding="utf-8", newline="")
                os.replace(temporary, path)
            finally:
                if temporary.exists():
                    temporary.unlink()
            print(f"patched: {path}")
            print(f"backup:  {backup}")
        print("RESULT=PATCHED (legacy boot parsing added; no profile generated)")
        return 0
    except (OSError, PatchError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
