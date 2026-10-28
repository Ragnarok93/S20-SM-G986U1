#!/usr/bin/env python3
"""Tests for the read-only ELF debug-metadata inventory."""
from __future__ import annotations

import hashlib
import importlib.util
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
  [ 1] .debug_info       PROGBITS        0000000000000000 000040 000100 00      0   0  1
  [ 2] .debug_abbrev     PROGBITS        0000000000000000 000140 000080 00      0   0  1
  [ 3] .debug_str        PROGBITS        0000000000000000 0001c0 000060 01  MS  0   0  1
  [ 4] .symtab           SYMTAB          0000000000000000 000220 000010 18      5   1  8
  [ 5] .BTF              PROGBITS        0000000000000000 000230 000028 00      0   0  4
"""
SECTIONS_WITHOUT_DWARF = """There are 2 section headers:
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 1] .text             PROGBITS        0000000000000000 000040 000100 00  AX  0   0 16
  [ 2] .dynsym           DYNSYM          0000000000000000 000140 000020 18   A  3   1  8
"""
NOTES = """Displaying notes found in: .note.gnu.build-id
  GNU                  0x00000014 NT_GNU_BUILD_ID
    Build ID: abcdef0123456789
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
    def _mock_readelf(self, has_dwarf: bool = True):
        sections = SECTIONS_WITH_DWARF if has_dwarf else SECTIONS_WITHOUT_DWARF

        def fake_run(command, **kwargs):
            args = command[1:-1]
            if args == ["-h", "-W"]:
                stdout = HEADER
            elif args == ["-S", "-W"]:
                stdout = sections
            elif args == ["-n", "-W"]:
                stdout = NOTES
            elif args == ["--debug-dump=info", "--dwarf-depth=1", "--wide"]:
                stdout = DWARF_INFO
            else:
                return subprocess.CompletedProcess(command, 1, stdout="", stderr="unexpected arguments")
            return subprocess.CompletedProcess(command, 0, stdout=stdout, stderr="")

        return fake_run

    def test_reports_dwarf_sections_and_compilation_unit_parse(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"ELF metadata fixture")
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(inspector.subprocess, "run", side_effect=self._mock_readelf()):
                result = inspector.inspect_elf(path)

            self.assertEqual(result.elf_fields["Class"], "ELF64")
            self.assertEqual(result.elf_fields["Machine"], "AArch64")
            self.assertEqual(result.build_id, "abcdef0123456789")
            self.assertEqual(result.sha256, hashlib.sha256(b"ELF metadata fixture").hexdigest())
            names = {section.name for section in result.sections}
            self.assertIn(".debug_info", names)
            self.assertIn(".debug_abbrev", names)
            self.assertIn(".BTF", names)
            self.assertEqual(result.dwarf_decode, "PASS")
            self.assertEqual(result.compilation_units, 2)

    def test_reports_missing_dwarf_without_running_debug_dump(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vmlinux.elf"
            path.write_bytes(b"ELF metadata fixture")
            fake_run = self._mock_readelf(has_dwarf=False)
            with patch.object(inspector.shutil, "which", return_value="/usr/bin/readelf"), \
                 patch.object(inspector.subprocess, "run", side_effect=fake_run) as run:
                result = inspector.inspect_elf(path)

            self.assertEqual(result.dwarf_decode, "NOT_PRESENT")
            self.assertEqual(result.compilation_units, 0)
            self.assertEqual(run.call_count, 3)

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

    def test_section_parser_returns_sizes_without_exposing_addresses(self) -> None:
        sections = inspector.parse_sections(SECTIONS_WITH_DWARF)
        info = next(section for section in sections if section.name == ".debug_info")
        self.assertEqual(info.size, 0x100)
        self.assertEqual(info.kind, "PROGBITS")


if __name__ == "__main__":
    unittest.main()
