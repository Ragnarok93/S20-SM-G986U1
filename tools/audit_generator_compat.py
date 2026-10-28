#!/usr/bin/env python3
"""Read-only compatibility preflight for an external Android boot target generator.

This reports boot-header support and visible no-BTF template-fallback patterns.
It never imports or executes the external generator, and never writes target.h.
"""
from __future__ import annotations

import argparse
import ast
from pathlib import Path
import struct
import sys
from typing import Any

MAGIC = b"ANDROID!"
MODERN_HEADER_SIZES = {3: 0x62C, 4: 0x630}
GENERATOR_RELATIVE_PATHS = (
    Path("tools/vendor/generate_target.py"),
    Path("tools/vendor/generate_target_2.py"),
)
WRAPPER_RELATIVE_PATH = Path("tools/generate_target.py")


class AuditError(RuntimeError):
    pass


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise AuditError(f"truncated boot header at 0x{offset:x}")
    return struct.unpack_from("<I", data, offset)[0]


def inspect_boot(path: Path) -> dict[str, int | str]:
    if path.is_symlink() or not path.is_file():
        raise AuditError(f"--boot must name a regular file, not a symlink: {path}")
    data = path.read_bytes()
    if len(data) < 4096 or data[:8] != MAGIC:
        raise AuditError(f"not a valid-sized Android boot image: {path}")

    version = u32(data, 40)
    kernel_size = u32(data, 8)
    if kernel_size <= 0:
        raise AuditError("boot header kernel_size is zero")

    if version in (0, 1, 2):
        page_size = u32(data, 36)
        if page_size < 2048 or page_size > 65536 or page_size & (page_size - 1):
            raise AuditError(f"invalid legacy page size: 0x{page_size:x}")
        kernel_offset = page_size
        layout = "legacy"
    elif version in (3, 4):
        header_size = u32(data, 20)
        expected = MODERN_HEADER_SIZES[version]
        if header_size != expected:
            raise AuditError(
                f"v{version} header_size mismatch: 0x{header_size:x}; expected 0x{expected:x}"
            )
        page_size = 4096
        kernel_offset = 4096
        layout = "modern"
    else:
        raise AuditError(f"unsupported Android boot header version: {version}")

    kernel_end = kernel_offset + kernel_size
    if kernel_end < kernel_offset or kernel_end > len(data):
        raise AuditError(
            f"kernel range [0x{kernel_offset:x}, 0x{kernel_end:x}) exceeds file size 0x{len(data):x}"
        )
    return {
        "version": version,
        "layout": layout,
        "page_size": page_size,
        "kernel_offset": kernel_offset,
        "kernel_size": kernel_size,
    }


def supported_header_versions(source_path: Path) -> set[int] | None:
    """Read expected_header_sizes assignments without importing generator code."""
    try:
        tree = ast.parse(source_path.read_text(encoding="utf-8"), filename=str(source_path))
    except (OSError, SyntaxError, UnicodeDecodeError) as exc:
        raise AuditError(f"cannot parse generator source {source_path}: {exc}") from exc

    versions: set[int] = set()
    found = False
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == "expected_header_sizes"
                   for target in node.targets):
            continue
        try:
            mapping: Any = ast.literal_eval(node.value)
        except (ValueError, TypeError):
            continue
        if isinstance(mapping, dict) and all(
            isinstance(key, int) and isinstance(value, int)
            for key, value in mapping.items()
        ):
            versions.update(mapping)
            found = True
    return versions if found else None


def fallback_flags(wrapper_path: Path) -> dict[str, bool]:
    try:
        source = wrapper_path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise AuditError(f"cannot read wrapper source {wrapper_path}: {exc}") from exc
    return {
        "fallback_function": "def fallback_generate_without_btf" in source,
        "default_rodin_template": 'or "rodin"' in source or "or 'rodin'" in source,
        "explicit_inheritance_warning": "inherited from template" in source,
        "writes_output": "output.write_text(" in source,
    }


def audit(generator_root: Path, boot_path: Path) -> int:
    boot = inspect_boot(boot_path)
    print("== Read-only generator compatibility preflight ==")
    print(f"boot_image={boot_path}")
    print(f"boot_header_version={boot['version']}")
    print(f"boot_header_layout={boot['layout']}")
    print(f"page_size={boot['page_size']}")
    print(f"kernel_offset=0x{boot['kernel_offset']:x}")
    print(f"kernel_size={boot['kernel_size']}")

    parser_results: list[tuple[Path, set[int] | None]] = []
    for relative in GENERATOR_RELATIVE_PATHS:
        path = generator_root / relative
        if path.is_symlink() or not path.is_file():
            raise AuditError(f"generator source missing or symlinked: {path}")
        versions = supported_header_versions(path)
        parser_results.append((path, versions))
        label = ",".join(str(v) for v in sorted(versions)) if versions is not None else "unknown"
        print(f"generator_parser={path} accepted_header_versions={label}")

    unknown = [str(path) for path, versions in parser_results if versions is None]
    if unknown:
        print("parser_compatibility=UNKNOWN (no literal expected_header_sizes map found)")
        print("RESULT=INCONCLUSIVE")
        return 2

    version = int(boot["version"])
    incompatible = [
        str(path) for path, versions in parser_results
        if versions is not None and version not in versions
    ]
    if incompatible:
        print(
            f"parser_compatibility=BLOCKED (boot header v{version} is not accepted by "
            + "; ".join(incompatible)
            + ")"
        )
    else:
        print(f"parser_compatibility=SUPPORTED_BY_STATIC_MAP (v{version})")

    wrapper = generator_root / WRAPPER_RELATIVE_PATH
    if wrapper.is_symlink() or not wrapper.is_file():
        raise AuditError(f"generator wrapper missing or symlinked: {wrapper}")
    flags = fallback_flags(wrapper)
    print("== No-BTF fallback source markers (static scan; not execution) ==")
    for name, enabled in flags.items():
        print(f"{name}={'yes' if enabled else 'no'}")
    if all(flags[key] for key in (
        "fallback_function", "default_rodin_template", "explicit_inheritance_warning", "writes_output"
    )):
        print("no_btf_fallback_risk=template inheritance path detected; output may retain unresolved macros")
    else:
        print("no_btf_fallback_risk=INCONCLUSIVE (expected marker set not fully found)")

    print("NOTE: this preflight did not import or execute the generator and did not write any files.")
    if incompatible:
        print("RESULT=BLOCKED")
        return 1
    if not all(flags.values()):
        print("RESULT=PARTIAL (review fallback behavior manually)")
        return 2
    print("RESULT=HEADER_MAP_SUPPORTED (fallback behavior still requires review)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator-root", required=True, type=Path,
                        help="root of the checkout containing tools/generate_target.py and tools/vendor/")
    parser.add_argument("--boot", required=True, type=Path, help="decompressed Android boot.img")
    args = parser.parse_args(argv)
    try:
        return audit(args.generator_root, args.boot)
    except (OSError, AuditError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
