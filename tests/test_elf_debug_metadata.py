#!/usr/bin/env python3
"""Tests for the read-only ELF debug-metadata inventory."""
from __future__ import annotations

import hashlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "inspect_elf_debug_metadata.py"
SPEC = importlib.util.spec_from_file_location("inspect_elf_debug_metadata", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import inspector at {MODULE_PATH}")
inspector = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = inspector
SPEC.loader.exec_module(inspector)


HEADER = """ELF Header:
  Class:                             ELF64
  Data:                              2's complement, little endian
  OS/ABI:                            UNIX - System V
  Type:                              REL (Relocatable file)
  Machine:                           AArch64
"""
SECTIONS_WITH_DWARF = """There are 6 section headers:
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            0000000000000000 000000 000000 00      0   0  0
  [ 1] .debug_info       PROGBITS        0000000000000000 000040 000100 00      0   0  1
  [ 2] .debug_abbrev     PROGBITS        0000000000000000 000140 000080 00      0   0  1
  [ 3] .debug_str        PROGBITS        0000000000000000 0001c0 000060 01  MS  0   0  1
  [ 4] .symtab           SYMTAB          0000000000000000 000220 000010 18      5   1  8
  [ 5] .BTF              PROGBITS        0000000000000000 000230 000028 00      0   0  4
"""
SECTIONS_WITHOUT_DWARF = """There are 3 section headers:
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            0000000000000000 000000 000000 00      0   0  0
  [ 1] .text             PROGBITS        0000000000000000 000040 000100 00  AX  0   0 16
  [ 2] .dynsym           DYNSYM          0000000000000000 000140 000020 18   A  3   1  8
"""
NOTES = """Displaying notes found in: .note.gnu.build-id
  GNU                  0x00000014 NT_GNU_BUILD_ID
    Build ID: abcdef0123456789
"""
SYMBOLS = """Symbol table '.symtab' contains 3 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000001000    24 FUNC    GLOBAL DEFAULT    1 _start
     2: 0000000000000000     0 OBJECT  GLOBAL DEFAULT  UND external_item
"""
SYMBOLS_NO_DWARF = """Symbol table '.dynsym' contains 2 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000002000    12 FUNC    GLOBAL DEFAULT    1 external_function
"""
DWARF_INFO = """Contents of the .debug_info section:
  Compilation Unit @ offset 0:
   Length:        0x80
   Version:       5
  Compilation Unit @ offset 0x84:
   Length:        0x40
   Version:       5
"""


class ElfDebugMetadataTests(unittest.TestCase):
    def _mock_readelf(
        self,
        has_dwarf: bool = True,
        lint_output: str = "",
        sections_override: str | None = None,
    ):
        sections = sections_override or (
            SECTIONS_WITH_DWARF if has_dwarf else SECTIONS_WITHOUT_DWARF
        )

        def fake_run(command, **kwargs):
            args = command[1:-1]
            if args == ["-h", "-W"]:
                stdout = HEADER
            elif args == ["-S", "-W"]:
                stdout = sections
            elif args == ["-n", "-W"]:
                stdout = NOTES
            elif args == ["-s", "-W"]:
                stdout = SYMBOLS if has_dwarf else SYMBOLS_NO_DWARF
            elif args == ["--lint", "-h", "-S", "-s", "-W"]:
                stdout = lint_output
            elif args == ["--debug-dump=info", "--dwarf-depth=1", "--wide"]:
                stdout = DWARF_INFO
            else:
                return subprocess.CompletedProcess(command, 1, stdout="", stderr="unexpected arguments")
            return subprocess.CompletedProcess(command, 0, stdout=stdout, stderr="")

        return fake_run

    def test_reports_dwarf_sections_and_compilation_unit_parse(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(inspector.subprocess, "run", side_effect=self._mock_readelf()):
                result = inspector.inspect_elf(path)

            self.assertEqual(result.elf_fields["Class"], "ELF64")
            self.assertEqual(result.elf_fields["Machine"], "AArch64")
            self.assertEqual(result.build_id, "abcdef0123456789")
            self.assertEqual(result.sha256, hashlib.sha256(b"E" * 0x1000).hexdigest())
            names = {section.name for section in result.sections}
            self.assertIn(".debug_info", names)
            self.assertIn(".debug_abbrev", names)
            self.assertIn(".BTF", names)
            self.assertEqual(result.dwarf_decode, "PASS")
            self.assertEqual(result.compilation_units, 2)
            self.assertEqual(result.declared_section_count, 6)
            self.assertEqual(len(result.sections), 6)
            self.assertTrue(result.section_table_consistent)
            self.assertEqual(result.out_of_bounds_sections, 0)
            self.assertEqual(result.readelf_lint_status, "PASS")
            self.assertEqual(result.readelf_lint_warning_count, 0)
            self.assertEqual(result.readelf_lint_error_count, 0)
            self.assertIsNotNone(result.symbol_summary)
            assert result.symbol_summary is not None
            self.assertEqual(result.symbol_summary.declared_entries, 3)
            self.assertEqual(result.symbol_summary.parsed_entries, 3)
            self.assertEqual(result.symbol_summary.defined_entries, 1)
            self.assertEqual(result.symbol_summary.undefined_entries, 1)
            self.assertEqual(result.symbol_summary.null_entries, 1)
            self.assertEqual(
                result.symbol_summary.parsed_entries,
                result.symbol_summary.defined_entries
                + result.symbol_summary.undefined_entries
                + result.symbol_summary.null_entries,
            )
            self.assertIn(("FUNC", 1), result.symbol_summary.tables[0].type_counts)
            self.assertEqual(
                sum(count for _name, count in result.symbol_summary.tables[0].type_counts),
                result.symbol_summary.parsed_entries - result.symbol_summary.null_entries,
            )
            self.assertEqual(
                sum(count for _name, count in result.symbol_summary.tables[0].binding_counts),
                result.symbol_summary.parsed_entries - result.symbol_summary.null_entries,
            )
            # The summary deliberately retains no symbol strings or symbol values.
            self.assertNotIn("_start", repr(result.symbol_summary))
            self.assertNotIn("0000000000001000", repr(result.symbol_summary))

    def test_reports_missing_dwarf_without_running_debug_dump(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            fake_run = self._mock_readelf(has_dwarf=False)
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(inspector.subprocess, "run", side_effect=fake_run) as run:
                result = inspector.inspect_elf(path)

            self.assertEqual(result.dwarf_decode, "NOT_PRESENT")
            self.assertEqual(result.compilation_units, 0)
            self.assertEqual(result.declared_section_count, 3)
            self.assertEqual(len(result.sections), 3)
            self.assertTrue(result.section_table_consistent)
            self.assertEqual(result.out_of_bounds_sections, 0)
            self.assertEqual(result.readelf_lint_status, "PASS")
            self.assertIsNotNone(result.symbol_summary)
            assert result.symbol_summary is not None
            self.assertEqual(result.symbol_summary.declared_entries, 2)
            self.assertEqual(result.symbol_summary.defined_entries, 1)
            self.assertEqual(result.symbol_summary.undefined_entries, 0)
            self.assertEqual(result.symbol_summary.null_entries, 1)
            self.assertEqual(
                sum(count for _name, count in result.symbol_summary.tables[0].type_counts),
                result.symbol_summary.parsed_entries - result.symbol_summary.null_entries,
            )
            self.assertEqual(run.call_count, 5)


    def test_readelf_lint_warnings_are_counted_without_dumping_details(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(
                     inspector.subprocess,
                     "run",
                     side_effect=self._mock_readelf(
                         has_dwarf=False,
                         lint_output="readelf: Warning: section metadata diagnostic fixture",
                     ),
                 ):
                result = inspector.inspect_elf(path)

            self.assertEqual(result.readelf_lint_status, "WARN")
            self.assertEqual(result.readelf_lint_warning_count, 1)
            self.assertEqual(result.readelf_lint_error_count, 0)

    def test_symbol_summary_parser_rejects_unrecognized_output(self) -> None:
        with self.assertRaisesRegex(inspector.MetadataError, "no parseable symbol-table"):
            inspector.parse_symbol_summary("no symbol tables here")

    def test_rejects_symlink_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "real.elf"
            target.write_bytes(b"fixture")
            link = root / "link.elf"
            link.symlink_to(target)
            with self.assertRaisesRegex(inspector.MetadataError, "non-symlink"):
                inspector.inspect_elf(link)

    def test_rejects_missing_header_fields(self) -> None:
        with self.assertRaisesRegex(inspector.MetadataError, "missing fields"):
            inspector.parse_header("ELF Header:\n  Class: ELF64\n")

    def test_section_parser_returns_indices_ranges_and_declared_count(self) -> None:
        sections = inspector.parse_sections(SECTIONS_WITH_DWARF)
        info = next(section for section in sections if section.name == ".debug_info")
        self.assertEqual(info.index, 1)
        self.assertEqual(info.offset, 0x40)
        null_section = next(section for section in sections if section.index == 0)
        self.assertEqual(null_section.name, "")
        self.assertEqual(null_section.kind, "NULL")
        self.assertEqual(info.size, 0x100)
        self.assertEqual(info.kind, "PROGBITS")
        self.assertEqual(inspector.parse_declared_section_count(SECTIONS_WITH_DWARF), 6)

    def test_strict_structure_rejects_declared_section_count_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            mismatched_sections = SECTIONS_WITH_DWARF.replace(
                "There are 6 section headers:", "There are 7 section headers:"
            )
            output = io.StringIO()
            errors = io.StringIO()
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(
                     inspector.subprocess,
                     "run",
                     side_effect=self._mock_readelf(sections_override=mismatched_sections),
                 ), redirect_stdout(output), redirect_stderr(errors):
                result = inspector.main([str(path), "--strict-structure"])

            self.assertEqual(result, 1)
            self.assertIn("section_indices_consistent=no", output.getvalue())
            self.assertIn("STRUCTURE_VALIDATION=FAIL", errors.getvalue())

    def test_strict_structure_rejects_out_of_bounds_file_backed_section(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            bad_range_sections = SECTIONS_WITH_DWARF.replace(
                "000040 000100", "000ff0 000100", 1
            )
            errors = io.StringIO()
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(
                     inspector.subprocess,
                     "run",
                     side_effect=self._mock_readelf(sections_override=bad_range_sections),
                 ), redirect_stdout(io.StringIO()), redirect_stderr(errors):
                result = inspector.main([str(path), "--strict-structure"])

            self.assertEqual(result, 1)
            self.assertIn("1 file-backed section range(s) exceed file size", errors.getvalue())

    def test_strict_structure_rejects_readelf_lint_warning(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            errors = io.StringIO()
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(
                     inspector.subprocess,
                     "run",
                     side_effect=self._mock_readelf(
                         has_dwarf=False,
                         lint_output="readelf: Warning: synthetic lint warning",
                     ),
                 ), redirect_stdout(io.StringIO()), redirect_stderr(errors):
                result = inspector.main([str(path), "--strict-structure"])

            self.assertEqual(result, 1)
            self.assertIn("readelf lint status is WARN", errors.getvalue())

    def test_strict_structure_passes_consistent_elf_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"E" * 0x1000)
            output = io.StringIO()
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(inspector.subprocess, "run", side_effect=self._mock_readelf()), \
                 redirect_stdout(output):
                result = inspector.main([str(path), "--strict-structure"])

            self.assertEqual(result, 0)
            self.assertIn("STRUCTURE_VALIDATION=PASS", output.getvalue())


if __name__ == "__main__":
    unittest.main()
