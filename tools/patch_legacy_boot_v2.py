#!/usr/bin/env python3
"""Patch the two vendored Android boot-image parsers for legacy v0-v2 layout.

Changes only boot-header parsing in tools/vendor/generate_target.py and
tools/vendor/generate_target_2.py. It retains strict v3/v4 header-size checks
and does not modify profile calculation logic or generate target.h.

The operation is preflighted across both files before writing. Original files
are backed up as .pre-legacy-v2.bak unless --no-backup is supplied.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import sys
import tempfile

OLD_BLOCK = '''    kernel_size = u32(boot, 8)
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

NEW_BLOCK = '''    kernel_size = u32(boot, 8)
    header_version = u32(boot, 40)

    # Legacy v0-v2 headers store page_size at byte offset 36. In these
    # formats, offset 20 is ramdisk_addr, not header_size. The kernel begins
    # at the first page boundary, so do not parse legacy images as v3/v4.
    if header_version in (0, 1, 2):
        page_size = u32(boot, 36)
        if (
            page_size < 2048
            or page_size > 65536
            or page_size & (page_size - 1)
        ):
            fail(f"legacy boot header page_size 非法: 0x{page_size:x}")
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
        fail(f"不支持的 Android boot header 版本: {header_version}")
'''

TARGETS = (
    Path("tools/vendor/generate_target.py"),
    Path("tools/vendor/generate_target_2.py"),
)
MARKER = "# Legacy v0-v2 headers store page_size at byte offset 36."


def read_target(path: Path) -> tuple[str, int]:
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"target must be an existing regular file (not symlink): {path}")
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise RuntimeError(f"not UTF-8 text: {path}") from exc
    return text, stat.S_IMODE(path.stat().st_mode)


def atomic_write(path: Path, content: str, mode: int) -> None:
    fd, temp_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temp_path = Path(temp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temp_path, mode)
        os.replace(temp_path, path)
    except Exception:
        try:
            temp_path.unlink()
        except OSError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=Path("/tmp/ghostlock-profile-gen"),
        help="external generator checkout (default: /tmp/ghostlock-profile-gen)",
    )
    parser.add_argument(
        "--check", action="store_true",
        help="check whether both files can be patched; do not write files",
    )
    parser.add_argument(
        "--no-backup", action="store_true",
        help="do not save .pre-legacy-v2.bak backups",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()

    planned: list[tuple[Path, str, str, int]] = []
    already_patched: list[Path] = []
    errors: list[str] = []

    # Preflight all targets before modifying either file.
    for relative in TARGETS:
        path = root / relative
        try:
            source, mode = read_target(path)
        except (OSError, RuntimeError) as exc:
            errors.append(str(exc))
            continue

        occurrences = source.count(OLD_BLOCK)
        if occurrences == 1:
            if source.count(MARKER):
                errors.append(f"{path}: both old block and patch marker are present")
                continue
            updated = source.replace(OLD_BLOCK, NEW_BLOCK, 1)
            planned.append((path, source, updated, mode))
        elif occurrences == 0 and source.count(MARKER) == 1:
            already_patched.append(path)
        else:
            errors.append(
                f"{path}: expected exactly one unpatched parser block or one patch marker; "
                f"old-block occurrences={occurrences}, marker occurrences={source.count(MARKER)}"
            )

    if errors:
        print("Preflight failed; no files were changed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 2

    for path in already_patched:
        print(f"ALREADY_PATCHED {path}")
    for path, _old, _new, _mode in planned:
        print(f"READY {path}")
    if args.check:
        if planned:
            print(f"CHECK_OK: {len(planned)} file(s) ready to patch; no files changed")
        else:
            print("CHECK_OK: both parser files are already patched")
        return 0

    if not args.no_backup:
        for path, _source, _updated, _mode in planned:
            backup = path.with_name(path.name + ".pre-legacy-v2.bak")
            if backup.exists() or backup.is_symlink():
                print(
                    f"ERROR: backup already exists; refusing to overwrite: {backup}",
                    file=sys.stderr,
                )
                return 2
        for path, source, _updated, _mode in planned:
            backup = path.with_name(path.name + ".pre-legacy-v2.bak")
            backup.write_text(source, encoding="utf-8", newline="")
            print(f"BACKUP {backup}")

    originals: list[tuple[Path, str, int]] = []
    try:
        for path, source, updated, mode in planned:
            originals.append((path, source, mode))
            atomic_write(path, updated, mode)
            print(f"PATCHED {path}")
    except Exception as exc:
        print(f"ERROR: write failed: {exc}; attempting rollback", file=sys.stderr)
        rollback_errors: list[str] = []
        for path, source, mode in reversed(originals):
            try:
                atomic_write(path, source, mode)
                print(f"ROLLED_BACK {path}", file=sys.stderr)
            except Exception as rollback_exc:
                rollback_errors.append(f"{path}: {rollback_exc}")
        for error in rollback_errors:
            print(f"ROLLBACK_ERROR {error}", file=sys.stderr)
        return 3

    print("DONE: legacy v0-v2 page-size parsing added; v3/v4 header-size checks retained")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
