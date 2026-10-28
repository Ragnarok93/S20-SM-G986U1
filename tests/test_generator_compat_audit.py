#!/usr/bin/env python3
"""Regression tests for the read-only external-generator compatibility preflight."""
from __future__ import annotations

import importlib.util
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "audit_generator_compat.py"
SPEC = importlib.util.spec_from_file_location("audit_generator_compat", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot import compatibility audit at {MODULE_PATH}")
audit_module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = audit_module
SPEC.loader.exec_module(audit_module)


def make_boot(version: int, *, page_size: int = 4096) -> bytes:
    kernel_size = 0x100
    if version in (0, 1, 2):
        offset = page_size
        data = bytearray(max(4096, offset + kernel_size))
        struct.pack_into("<I", data, 36, page_size)
    else:
        offset = 4096
        data = bytearray(offset + kernel_size)
        struct.pack_into("<I", data, 20, {3: 0x62C, 4: 0x630}[version])
    data[:8] = b"ANDROID!"
    struct.pack_into("<I", data, 8, kernel_size)
    struct.pack_into("<I", data, 40, version)
    data[offset:offset + kernel_size] = b"K" * kernel_size
    return bytes(data)


def make_generator_tree(
    root: Path,
    versions: str,
    *,
    fallback: bool = True,
    legacy_support: bool = False,
    automatic_fallback_route: bool = True,
    validation_fail_closed: bool = False,
) -> None:
    vendor = root / "tools" / "vendor"
    vendor.mkdir(parents=True)
    legacy_branch = (
        "    if header_version in (0, 1, 2):\n"
        "        page_size = u32(boot, 36)\n"
        "        kernel_offset = page_size\n"
        if legacy_support
        else ""
    )
    parser_source = f"""
def extract_boot_kernel(path):
    header_version = u32(boot, 40)
{legacy_branch}    expected_header_sizes = {versions}
    return expected_header_sizes
"""
    (vendor / "generate_target.py").write_text(parser_source, encoding="utf-8")
    (vendor / "generate_target_2.py").write_text(parser_source, encoding="utf-8")
    if fallback:
        fallback_route = (
            "    except Exception as exc:\n"
            "        return fallback_generate_without_btf(args, profile_full, output, boot, config, kallsyms, exc)\n"
            if automatic_fallback_route
            else "    except Exception as exc:\n"
            "        fail(f'required BTF unavailable: {exc}')\n"
        )
        if validation_fail_closed:
            btf_validation = (
                "    try:\n"
                "        validate_btf_consumer_layout(btf)\n"
                "    except Exception as exc:\n"
                "        fail('BTF consumer layout validation failed; refusing to generate target.h')\n"
            )
            percpu_validation = (
                "    try:\n"
                "        validate_percpu_entry_task()\n"
                "    except Exception as exc:\n"
                "        fail('percpu entry_task validation failed; refusing to generate target.h')\n"
            )
        else:
            btf_validation = (
                "    try:\n"
                "        validate_btf_consumer_layout(btf)\n"
                "    except Exception as exc:\n"
                "        print('warning: cannot validate layout; continuing generation', file=sys.stderr)\n"
            )
            percpu_validation = (
                "    try:\n"
                "        validate_percpu_entry_task()\n"
                "    except Exception as exc:\n"
                "        print('warning: cannot validate percpu layout; continuing generation', file=sys.stderr)\n"
            )
        wrapper_source = '''
def template_target_path():
    name = getattr(args, "template_target", None) or os.environ.get("TARGET_TEMPLATE") or "rodin"
def fallback_generate_without_btf():
    warning = "values not overwritten above were inherited from template"
    output.write_text(warning)
def complete_generate():
    try:
        locate_btf()
''' + fallback_route + btf_validation + percpu_validation
    else:
        wrapper_source = "def main():\n    return 0\n"
    (root / "tools" / "generate_target.py").write_text(
        wrapper_source, encoding="utf-8"
    )


class GeneratorCompatibilityAuditTests(unittest.TestCase):
    def test_v2_boot_is_blocked_by_v3_v4_only_generator_parsers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generator_root = root / "generator"
            generator_root.mkdir()
            make_generator_tree(generator_root, "{3: 0x62C, 4: 0x630}")
            boot_path = root / "boot.img"
            boot_path.write_bytes(make_boot(2))

            output = io.StringIO()
            with redirect_stdout(output):
                result = audit_module.audit(generator_root, boot_path)

            report = output.getvalue()
            self.assertEqual(result, 1)
            self.assertIn("boot_header_version=2", report)
            self.assertIn("accepted_header_versions=3,4", report)
            self.assertIn("parser_compatibility=BLOCKED", report)
            self.assertIn("no_btf_fallback_risk=ACTIVE (automatic template inheritance route detected)", report)
            self.assertIn("did not write any files", report)
            self.assertFalse((generator_root / "src" / "targets").exists())

    def test_supported_header_map_does_not_hide_template_fallback_risk(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generator_root = root / "generator"
            generator_root.mkdir()
            make_generator_tree(
                generator_root,
                "{3: 0x62C, 4: 0x630}",
                legacy_support=True,
            )
            boot_path = root / "boot.img"
            boot_path.write_bytes(make_boot(2))

            output = io.StringIO()
            with redirect_stdout(output):
                result = audit_module.audit(generator_root, boot_path)

            report = output.getvalue()
            self.assertEqual(result, 2)
            self.assertIn("parser_compatibility=SUPPORTED_BY_STATIC_MAP (v2)", report)
            self.assertIn("no_btf_fallback_risk=ACTIVE (automatic template inheritance route detected)", report)
            self.assertIn("RESULT=PARTIAL (active template fallback needs review)", report)

    def test_fail_closed_wrapper_is_not_reported_as_active_template_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generator_root = root / "generator"
            generator_root.mkdir()
            make_generator_tree(
                generator_root,
                "{3: 0x62C, 4: 0x630}",
                legacy_support=True,
                automatic_fallback_route=False,
                validation_fail_closed=True,
            )
            boot_path = root / "boot.img"
            boot_path.write_bytes(make_boot(2))

            output = io.StringIO()
            with redirect_stdout(output):
                result = audit_module.audit(generator_root, boot_path)

            report = output.getvalue()
            self.assertEqual(result, 0)
            self.assertIn(
                "no_btf_fallback_risk=NO_DIRECT_AUTOMATIC_ROUTE_DETECTED",
                report,
            )
            self.assertIn("btf_layout_validation_fail_closed=yes", report)
            self.assertIn("percpu_validation_fail_closed=yes", report)
            self.assertIn("RESULT=HEADER_MAP_SUPPORTED_FAIL_CLOSED", report)

    def test_unknown_parser_structure_is_inconclusive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generator_root = root / "generator"
            generator_root.mkdir()
            make_generator_tree(generator_root, "None")
            boot_path = root / "boot.img"
            boot_path.write_bytes(make_boot(2))

            output = io.StringIO()
            with redirect_stdout(output):
                result = audit_module.audit(generator_root, boot_path)

            self.assertEqual(result, 2)
            self.assertIn("parser_compatibility=UNKNOWN", output.getvalue())


if __name__ == "__main__":
    unittest.main()
