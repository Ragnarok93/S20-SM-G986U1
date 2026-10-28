#!/usr/bin/env python3
"""Read-only inventory of ELF debug metadata; does not print symbol names or addresses.

Uses GNU readelf to report ELF identity, debug/BTF-related section presence,
separate-debug references, and whether readelf can parse DWARF compilation-unit
headers. It does not dump DWARF records, derive kernel offsets, or write outputs.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


SECTION_RE = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+"
    r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)(?:\s|$)"
)
HEADER_FIELDS = {
    "Class": re.compile(r"^\s*Class:\s*(.*?)\s*$", re.MULTILINE),
    "Data": re.compile(r"^\s*Data:\s*(.*?)\s*$", re.MULTILINE),
    "OS/ABI": re.compile(r"^\s*OS/ABI:\s*(.*?)\s*$", re.MULTILINE),
    "Type": re.compile(r"^\s*Type:\s*(.*?)\s*$", re.MULTILINE),
    "Machine": re.compile(r"^\s*Machine:\s*(.*?)\s*$", re.MULTILINE),
}
BUILD_ID_RE = re.compile(r"^\s*Build ID:\s*([0-9a-fA-F]+)\s*$", re.MULTILINE)

DWARF_INFO_NAMES = {".debug_info", ".zdebug_info"}
DWARF_ABBREV_NAMES = {".debug_abbrev", ".zdebug_abbrev"}
DWARF_STRING_NAMES = {
    ".debug_str",
    ".zdebug_str",
    ".debug_str_offsets",
    ".zdebug_str_offsets.dwo",
}
LINE_INFO_NAMES = {".debug_line", ".zdebug_line", ".debug_line_str"}
DWARF_SUPPORT_NAMES = {
    ".debug_addr", ".debug_aranges", ".debug_frame", ".debug_loc",
    ".debug_loclists", ".debug_ranges", ".debug_rnglists", ".debug_types",
    ".eh_frame", ".gdb_index", ".debug_names",
}
BTF_SECTION_NAMES = {".BTF", ".BTF.ext"}
SEPARATE_DEBUG_NAMES = {
    ".gnu_debuglink", ".gnu_debugaltlink", ".debug_sup", ".debug_cu_index",
    ".debug_tu_index",
}
SYMBOL_TABLE_NAMES = {".symtab", ".dynsym"}
SYMBOL_TABLE_HEADER_RE = re.compile(r"^\s*Symbol table '([^']+)' contains (\d+) entries:\s*$")
SYMBOL_ROW_RE = re.compile(
    r"^\s*(\d+):\s+\S+\s+\S+\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)(?:\s+.*)?$"
)


class MetadataError(RuntimeError):
    pass


@dataclass(frozen=True)
class Section:
    name: str
    kind: str
    size: int


@dataclass(frozen=True)
class SymbolTableSummary:
    table: str
    declared_entries: int
    parsed_entries: int
    defined_entries: int
    undefined_entries: int
    null_entries: int
    type_counts: tuple[tuple[str, int], ...]
    binding_counts: tuple[tuple[str, int], ...]


@dataclass(frozen=True)
class SymbolSummary:
    tables: tuple[SymbolTableSummary, ...]

    @property
    def declared_entries(self) -> int:
        return sum(table.declared_entries for table in self.tables)

    @property
    def parsed_entries(self) -> int:
        return sum(table.parsed_entries for table in self.tables)

    @property
    def defined_entries(self) -> int:
        return sum(table.defined_entries for table in self.tables)

    @property
    def undefined_entries(self) -> int:
        return sum(table.undefined_entries for table in self.tables)

    @property
    def null_entries(self) -> int:
        return sum(table.null_entries for table in self.tables)


@dataclass(frozen=True)
class Inspection:
    path: Path
    file_size: int
    sha256: str
    elf_fields: dict[str, str]
    build_id: str | None
    sections: tuple[Section, ...]
    dwarf_decode: str
    compilation_units: int
    dwarf_warning_count: int
    symbol_summary: SymbolSummary | None


def parse_symbol_summary(text: str) -> SymbolSummary:
    """Summarize symbol table counts without retaining or returning names or values."""
    mutable: list[dict[str, object]] = []
    current: dict[str, object] | None = None

    for line in text.splitlines():
        header = SYMBOL_TABLE_HEADER_RE.match(line)
        if header:
            current = {
                "table": header.group(1),
                "declared_entries": int(header.group(2)),
                "parsed_entries": 0,
                "undefined_entries": 0,
                "null_entries": 0,
                "type_counts": {},
                "binding_counts": {},
            }
            mutable.append(current)
            continue

        if current is None:
            continue
        row = SYMBOL_ROW_RE.match(line)
        if not row:
            continue
        entry_index, symbol_type, binding, _visibility, section_index = row.groups()
        current["parsed_entries"] = int(current["parsed_entries"]) + 1
        if entry_index == "0":
            # ELF symbol-table entry zero is the reserved STN_UNDEF null entry,
            # not an undefined external reference.
            current["null_entries"] = int(current["null_entries"]) + 1
        elif section_index == "UND":
            current["undefined_entries"] = int(current["undefined_entries"]) + 1
        type_counts = current["type_counts"]
        binding_counts = current["binding_counts"]
        assert isinstance(type_counts, dict)
        assert isinstance(binding_counts, dict)
        type_counts[symbol_type] = int(type_counts.get(symbol_type, 0)) + 1
        binding_counts[binding] = int(binding_counts.get(binding, 0)) + 1

    if not mutable:
        raise MetadataError("readelf symbol output contained no parseable symbol-table headers")

    tables: list[SymbolTableSummary] = []
    for item in mutable:
        parsed = int(item["parsed_entries"])
        undefined = int(item["undefined_entries"])
        null_entries = int(item["null_entries"])
        type_counts = item["type_counts"]
        binding_counts = item["binding_counts"]
        assert isinstance(type_counts, dict)
        assert isinstance(binding_counts, dict)
        tables.append(SymbolTableSummary(
            table=str(item["table"]),
            declared_entries=int(item["declared_entries"]),
            parsed_entries=parsed,
            defined_entries=parsed - undefined - null_entries,
            undefined_entries=undefined,
            null_entries=null_entries,
            type_counts=tuple(sorted((str(key), int(value)) for key, value in type_counts.items())),
            binding_counts=tuple(sorted((str(key), int(value)) for key, value in binding_counts.items())),
        ))
    return SymbolSummary(tuple(tables))


def parse_header(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for label, pattern in HEADER_FIELDS.items():
        match = pattern.search(text)
        if match:
            value = match.group(1).strip()
            # Avoid exposing entry-point/section addresses; only stable identity fields are retained.
            if label == "Type":
                value = value.split(" (", 1)[0].strip()
            fields[label] = value
    missing = sorted(set(HEADER_FIELDS) - fields.keys())
    if missing:
        raise MetadataError(f"readelf header output is missing fields: {', '.join(missing)}")
    return fields


def parse_sections(text: str) -> tuple[Section, ...]:
    result: list[Section] = []
    for line in text.splitlines():
        match = SECTION_RE.match(line)
        if not match:
            continue
        name, kind, _address, _offset, size = match.groups()
        try:
            result.append(Section(name=name, kind=kind, size=int(size, 16)))
        except ValueError as exc:
            raise MetadataError(f"invalid section size reported by readelf for {name}") from exc
    if not result:
        raise MetadataError("readelf section output contained no parseable section records")
    return tuple(result)


def run_readelf(readelf: str, args: Sequence[str], path: Path, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            [readelf, *args, str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise MetadataError(f"readelf command failed ({' '.join(args)}): {exc}") from exc
    if result.returncode:
        detail = (result.stderr.strip().splitlines() or result.stdout.strip().splitlines() or ["unknown error"])[0]
        raise MetadataError(f"readelf {' '.join(args)} failed: {detail}")
    return result


def inspect_elf(path: Path, readelf: str = "readelf") -> Inspection:
    if path.is_symlink() or not path.is_file():
        raise MetadataError(f"input must be a regular non-symlink file: {path}")
    executable = shutil.which(readelf)
    if executable is None:
        raise MetadataError(f"readelf executable not found: {readelf}")
    try:
        file_size = path.stat().st_size
        if file_size == 0:
            raise MetadataError("input file is empty")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except MetadataError:
        raise
    except OSError as exc:
        raise MetadataError(f"cannot read input: {path}: {exc}") from exc

    header = run_readelf(executable, ("-h", "-W"), path)
    sections_output = run_readelf(executable, ("-S", "-W"), path)
    notes = run_readelf(executable, ("-n", "-W"), path)
    fields = parse_header(header.stdout)
    sections = parse_sections(sections_output.stdout)
    build_match = BUILD_ID_RE.search(notes.stdout)
    build_id = build_match.group(1).lower() if build_match else None
    names = {section.name for section in sections}

    symbol_summary: SymbolSummary | None = None
    if names & SYMBOL_TABLE_NAMES:
        symbols = run_readelf(executable, ("-s", "-W"), path, timeout=180)
        symbol_summary = parse_symbol_summary(symbols.stdout)

    if names & DWARF_INFO_NAMES:
        try:
            dump = run_readelf(
                executable,
                ("--debug-dump=info", "--dwarf-depth=1", "--wide"),
                path,
                timeout=180,
            )
            units = sum(
                1 for line in dump.stdout.splitlines()
                if "Compilation Unit @" in line or line.strip().startswith("Compilation Unit:")
            )
            warnings = sum(
                1 for line in (dump.stdout + "\n" + dump.stderr).splitlines()
                if re.search(r"\b(?:warning|error):", line, re.IGNORECASE)
            )
            dwarf_decode = "PASS" if units > 0 and warnings == 0 else "PARTIAL"
        except MetadataError:
            units = 0
            warnings = 1
            dwarf_decode = "FAIL"
    else:
        units = 0
        warnings = 0
        dwarf_decode = "NOT_PRESENT"

    return Inspection(
        path=path,
        file_size=file_size,
        sha256=digest.hexdigest(),
        elf_fields=fields,
        build_id=build_id,
        sections=sections,
        dwarf_decode=dwarf_decode,
        compilation_units=units,
        dwarf_warning_count=warnings,
        symbol_summary=symbol_summary,
    )


def report(result: Inspection) -> None:
    names = {section.name for section in result.sections}
    print("== ELF debug-metadata inventory (read-only) ==")
    print(f"file={result.path}")
    print(f"file_size={result.file_size}")
    print(f"sha256={result.sha256}")
    for field in ("Class", "Data", "OS/ABI", "Type", "Machine"):
        print(f"elf_{field.lower().replace('/', '_')}={result.elf_fields[field]}")
    print(f"build_id={result.build_id or '<not present>'}")
    print(f"section_count={len(result.sections)}")

    if result.symbol_summary is None:
        print("symbol_table_entries=not_available")
    else:
        summary = result.symbol_summary
        print(f"symbol_table_entries_declared={summary.declared_entries}")
        print(f"symbol_table_entries_parsed={summary.parsed_entries}")
        print(f"symbol_table_entries_defined={summary.defined_entries}")
        print(f"symbol_table_entries_undefined={summary.undefined_entries}")
        print(f"symbol_table_null_entries={summary.null_entries}")
        for table in summary.tables:
            complete = "yes" if table.declared_entries == table.parsed_entries else "no"
            print(
                f"symbol_table={table.table} declared={table.declared_entries} "
                f"parsed={table.parsed_entries} defined={table.defined_entries} "
                f"undefined={table.undefined_entries} parse_complete={complete}"
            )
            if table.type_counts:
                print("  symbol_types=" + ", ".join(
                    f"{name}:{count}" for name, count in table.type_counts
                ))
            if table.binding_counts:
                print("  symbol_bindings=" + ", ".join(
                    f"{name}:{count}" for name, count in table.binding_counts
                ))

    groups = (
        ("dwarf_info", DWARF_INFO_NAMES),
        ("dwarf_abbrev", DWARF_ABBREV_NAMES),
        ("dwarf_strings", DWARF_STRING_NAMES),
        ("line_tables", LINE_INFO_NAMES),
        ("dwarf_support", DWARF_SUPPORT_NAMES),
        ("btf_sections", BTF_SECTION_NAMES),
        ("separate_debug_refs", SEPARATE_DEBUG_NAMES),
        ("symbol_table_sections", SYMBOL_TABLE_NAMES),
    )
    for label, group in groups:
        present = sorted(names & group)
        print(f"{label}={'present' if present else 'absent'}")
        for section in present:
            size = next(item.size for item in result.sections if item.name == section)
            print(f"  section={section} size={size}")

    print(f"dwarf_info_decode={result.dwarf_decode}")
    print(f"dwarf_compilation_units={result.compilation_units}")
    print(f"dwarf_warning_lines={result.dwarf_warning_count}")
    if names & DWARF_INFO_NAMES and names & DWARF_ABBREV_NAMES:
        print("dwarf_core_sections=present (section presence and parser check only)")
    else:
        print("dwarf_core_sections=incomplete_or_absent")
    if names & SEPARATE_DEBUG_NAMES:
        print("separate_debug_info=referenced_or_indexed; inspect referenced artifact availability separately")
    else:
        print("separate_debug_info=no known link/index section present")
    print("NOTE: no symbol names, symbol addresses, structure offsets, or generated profile values were emitted.")
    print("NOTE: section presence alone does not establish that debug metadata is complete or matches the running kernel.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="ELF file to inspect (e.g. reconstructed vendor vmlinux)")
    parser.add_argument("--readelf", default="readelf", help="GNU readelf executable or path")
    args = parser.parse_args(argv)
    try:
        report(inspect_elf(args.elf, args.readelf))
        return 0
    except MetadataError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
