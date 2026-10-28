#!/usr/bin/env python3
"""Make external generator validation fail closed instead of emitting degraded profiles.

This patch only changes error handling:
- missing BTF no longer falls back to a template-backed target.h;
- failed BTF consumer-layout validation is fatal;
- failed percpu entry_task validation is fatal.

It does not discover metadata, calculate target-specific values, run the generator,
or produce target.h. It operates on exact source blocks and refuses ambiguous edits.
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


PATCHES: dict[Path, tuple[Replacement, ...]] = {
    Path("tools/generate_target.py"): (
        Replacement(
            '''    except Exception as exc:
        return fallback_generate_without_btf(args, profile_full, output, boot, config, kallsyms, exc)
''',
            '''    except Exception as exc:
        fail(f"无法定位该内核的 BTF，拒绝使用模板回退生成 target.h: {exc}")
''',
            "拒绝使用模板回退生成 target.h",
        ),
        Replacement(
            '''    try:
        gt2.validate_btf_consumer_layout(btf)
    except Exception as exc:
        print(f"警告: 无法验证 target.h 所需结构字段偏移，继续生成: {exc}", file=sys.stderr)
''',
            '''    try:
        gt2.validate_btf_consumer_layout(btf)
    except Exception as exc:
        fail(f"BTF consumer layout validation failed; refusing to generate target.h: {exc}")
''',
            "BTF consumer layout validation failed; refusing to generate target.h",
        ),
        Replacement(
            '''    try:
        btf.validate_percpu_entry_task(kallsyms.one("__per_cpu_start"), kallsyms.one("__entry_task"))
    except Exception as exc:
        print(f"警告: 无法验证 percpu entry_task 布局，继续生成: {exc}", file=sys.stderr)
''',
            '''    try:
        btf.validate_percpu_entry_task(kallsyms.one("__per_cpu_start"), kallsyms.one("__entry_task"))
    except Exception as exc:
        fail(f"percpu entry_task validation failed; refusing to generate target.h: {exc}")
''',
            "percpu entry_task validation failed; refusing to generate target.h",
        ),
    ),
    Path("tools/vendor/generate_target.py"): (
        Replacement(
            '''        try:
            validate_btf_consumer_layout(btf)
        except Exception as exc:
            print(f"警告: 无法验证 target.h 所需结构字段偏移，继续生成: {exc}", file=sys.stderr)
''',
            '''        try:
            validate_btf_consumer_layout(btf)
        except Exception as exc:
            fail(f"BTF consumer layout validation failed; refusing to generate target.h: {exc}")
''',
            "BTF consumer layout validation failed; refusing to generate target.h",
        ),
        Replacement(
            '''        try:
            btf.validate_percpu_entry_task(
                kallsyms.one("__per_cpu_start"), kallsyms.one("__entry_task")
            )
        except Exception as exc:
            print(f"警告: 无法验证 percpu entry_task 布局，继续生成: {exc}", file=sys.stderr)
''',
            '''        try:
            btf.validate_percpu_entry_task(
                kallsyms.one("__per_cpu_start"), kallsyms.one("__entry_task")
            )
        except Exception as exc:
            fail(f"percpu entry_task validation failed; refusing to generate target.h: {exc}")
''',
            "percpu entry_task validation failed; refusing to generate target.h",
        ),
    ),
    Path("tools/vendor/generate_target_2.py"): (
        Replacement(
            '''        try:
            validate_btf_consumer_layout(btf)
        except Exception as exc:
            print(f"警告: 无法验证 target.h 所需结构字段偏移，继续生成: {exc}", file=sys.stderr)
''',
            '''        try:
            validate_btf_consumer_layout(btf)
        except Exception as exc:
            fail(f"BTF consumer layout validation failed; refusing to generate target.h: {exc}")
''',
            "BTF consumer layout validation failed; refusing to generate target.h",
        ),
        Replacement(
            '''        try:
            btf.validate_percpu_entry_task(
                kallsyms.one("__per_cpu_start"), kallsyms.one("__entry_task")
            )
        except Exception as exc:
            print(f"警告: 无法验证 percpu entry_task 布局，继续生成: {exc}", file=sys.stderr)
''',
            '''        try:
            btf.validate_percpu_entry_task(
                kallsyms.one("__per_cpu_start"), kallsyms.one("__entry_task")
            )
        except Exception as exc:
            fail(f"percpu entry_task validation failed; refusing to generate target.h: {exc}")
''',
            "percpu entry_task validation failed; refusing to generate target.h",
        ),
    ),
}


class PatchError(RuntimeError):
    pass


def build_plan(root: Path) -> list[tuple[Path, str, bool]]:
    plan: list[tuple[Path, str, bool]] = []
    # Validate all exact blocks for all files before writing any file.
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
    parser.add_argument("--root", type=Path, default=Path("/tmp/ghostlock-profile-gen"))
    parser.add_argument("--check", action="store_true", help="preflight only; modify no files")
    args = parser.parse_args(argv)
    try:
        plan = build_plan(args.root)
        if args.check:
            for path, _updated, already in plan:
                print(f"{'already patched' if already else 'patchable'}: {path}")
            print("CHECK=PASS (no files changed)")
            return 0

        for path, _updated, already in plan:
            backup = path.with_name(path.name + ".pre-fail-closed.bak")
            if not already and backup.exists():
                raise PatchError(f"backup already exists; inspect before retrying: {backup}")

        for path, updated, already in plan:
            if already:
                print(f"already patched: {path}")
                continue
            backup = path.with_name(path.name + ".pre-fail-closed.bak")
            temporary = path.with_name(path.name + ".tmp-fail-closed")
            shutil.copy2(path, backup)
            try:
                temporary.write_text(updated, encoding="utf-8", newline="")
                os.replace(temporary, path)
            finally:
                if temporary.exists():
                    temporary.unlink()
            print(f"patched: {path}")
            print(f"backup:  {backup}")
        print("RESULT=PATCHED (required metadata validation now fails closed)")
        return 0
    except (OSError, PatchError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
