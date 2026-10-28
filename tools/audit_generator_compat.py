#!/usr/bin/env python3
"""Read-only compatibility preflight for an external Android boot target generator.

This reports boot-header support and visible no-BTF template-fallback patterns.
It never imports or executes the external generator, and never writes target.h.
"""
from __future__ import annotations

import argparse
import ast
import re
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


def parser_capabilities(source_path: Path) -> tuple[set[int] | None, bool]:
    """Return the modern header-size map and whether a real legacy path is present.

    A v2 key in a modern header-size map is not sufficient: legacy boot images
    obtain the kernel offset from page_size at byte offset 36, unlike v3/v4.
    """
    try:
        source = source_path.read_text(encoding="utf-8")
        tree = ast.parse(source, filename=str(source_path))
    except (OSError, SyntaxError, UnicodeDecodeError) as exc:
        raise AuditError(f"cannot parse generator source {source_path}: {exc}") from exc

    versions: set[int] = set()
    found_map = False
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
            found_map = True

    extract_fn = next(
        (
            node for node in ast.walk(tree)
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == "extract_boot_kernel"
        ),
        None,
    )
    function_source = ast.get_source_segment(source, extract_fn) if extract_fn else None
    if function_source is None:
        return (versions if found_map else None), False

    has_legacy_version_branch = bool(re.search(
        r"header_version\s+in\s*\(\s*0\s*,\s*1\s*,\s*2\s*\)"
        r"|header_version\s*<\s*3",
        function_source,
    ))
    reads_legacy_page_size = bool(re.search(
        r"(?:page_size|page)\s*=\s*u32\(boot\s*,\s*36\s*\)",
        function_source,
    ))
    uses_legacy_page_offset = bool(re.search(
        r"kernel_offset\s*=\s*page_size",
        function_source,
    ))
    legacy_path = (
        has_legacy_version_branch
        and reads_legacy_page_size
        and uses_legacy_page_offset
    )
    return (versions if found_map else None), legacy_path


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
        "automatic_fallback_route": bool(re.search(
            r"except Exception as exc:\s*return fallback_generate_without_btf\s*\(",
            source,
        )),
        "btf_validation_fail_closed": (
            "BTF consumer layout validation failed; refusing to generate target.h" in source
        ),
        "percpu_validation_fail_closed": (
            "percpu entry_task validation failed; refusing to generate target.h" in source
        ),
    }


def validation_flags(source_path: Path) -> dict[str, bool]:
    """Check fail-closed validation markers in one generator source file."""
    try:
        source = source_path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise AuditError(f"cannot read validation source {source_path}: {exc}") from exc
    return {
        "btf_validation_fail_closed": (
            "BTF consumer layout validation failed; refusing to generate target.h" in source
        ),
        "percpu_validation_fail_closed": (
            "percpu entry_task validation failed; refusing to generate target.h" in source
        ),
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

    parser_results: list[tuple[Path, set[int] | None, bool]] = []
    for relative in GENERATOR_RELATIVE_PATHS:
        path = generator_root / relative
        if path.is_symlink() or not path.is_file():
            raise AuditError(f"generator source missing or symlinked: {path}")
        versions, legacy_path = parser_capabilities(path)
        parser_results.append((path, versions, legacy_path))
        if versions is None:
            label = "unknown"
        else:
            combined = set(versions)
            if legacy_path:
                combined.update((0, 1, 2))
            label = ",".join(str(v) for v in sorted(combined))
        print(
            f"generator_parser={path} accepted_header_versions={label} "
            f"legacy_path={'yes' if legacy_path else 'no'}"
        )

    unknown = [str(path) for path, versions, _legacy in parser_results if versions is None]
    if unknown:
        print("parser_compatibility=UNKNOWN (no literal expected_header_sizes map found)")
        print("RESULT=INCONCLUSIVE")
        return 2

    version = int(boot["version"])
    incompatible = [
        str(path) for path, versions, legacy_path in parser_results
        if versions is not None and (
            version not in versions if version >= 3 else not legacy_path
        )
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
    fallback_markers = all(flags[key] for key in (
        "fallback_function", "default_rodin_template", "explicit_inheritance_warning", "writes_output"
    ))
    if fallback_markers and flags["automatic_fallback_route"]:
        print("no_btf_fallback_risk=ACTIVE (automatic template inheritance route detected)")
    elif fallback_markers:
        print("no_btf_fallback_risk=NO_DIRECT_AUTOMATIC_ROUTE_DETECTED (fallback function remains in source)")
    else:
        print("no_btf_fallback_risk=INCONCLUSIVE (fallback marker set not fully found)")
    print(f"btf_layout_validation_fail_closed={'yes' if flags['btf_validation_fail_closed'] else 'no'}")
    print(f"percpu_validation_fail_closed={'yes' if flags['percpu_validation_fail_closed'] else 'no'}")

    parser_validation_fail_closed = True
    for relative in GENERATOR_RELATIVE_PATHS:
        parser_path = generator_root / relative
        parser_flags = validation_flags(parser_path)
        print(
            f"{relative}: "
            f"btf_validation_fail_closed={'yes' if parser_flags['btf_validation_fail_closed'] else 'no'} "
            f"percpu_validation_fail_closed={'yes' if parser_flags['percpu_validation_fail_closed'] else 'no'}"
        )
        parser_validation_fail_closed = (
            parser_validation_fail_closed
            and parser_flags["btf_validation_fail_closed"]
            and parser_flags["percpu_validation_fail_closed"]
        )

    print("NOTE: this preflight did not import or execute the generator and did not write any files.")
    if incompatible:
        print("RESULT=BLOCKED")
        return 1
    if fallback_markers and flags["automatic_fallback_route"]:
        print("RESULT=PARTIAL (active template fallback needs review)")
        return 2
    if not (
        flags["btf_validation_fail_closed"]
        and flags["percpu_validation_fail_closed"]
        and parser_validation_fail_closed
    ):
        print("RESULT=PARTIAL (validation failure handling is not confirmed fail-closed in every generator entry point)")
        return 2
    if fallback_markers:
        print("RESULT=HEADER_MAP_SUPPORTED_FAIL_CLOSED (static inspection only)")
        return 0
    print("RESULT=PARTIAL (fallback implementation markers were not found)")
    return 2


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
