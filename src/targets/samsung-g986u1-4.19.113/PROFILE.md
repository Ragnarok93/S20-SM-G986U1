# Samsung Galaxy S20+ target-profile scaffold

**Status: documentation-only; not build-ready.** This directory intentionally does not contain `target.h`, so the existing Makefile cannot select it as an executable target. Do not copy the K40 `target.h` here and treat it as a Samsung profile.

## Verified identity

| Field | Value | Evidence |
|---|---|---|
| Device | Samsung Galaxy S20+ `SM-G986U1` | Device identity collected for this test device |
| Device family | `y2q` | Device/kernel identity collected during inspection |
| Firmware build | `G986U1UESCHXL1` | Original Samsung AP package filename and kernel banner |
| Android | Android 13 | Stock firmware identification |
| Kernel release | `4.19.113-27095354` | Stock kernel banner / uname collected for the device |
| Kernel architecture | ARM64 / AArch64 | Vendor ELF and raw Image file metadata |
| Kernel compiler | Clang 10.0.6 | Stock kernel build identification collected during inspection |
| Page size | 4 KiB | Stock ARM64 image and configuration context |
| VA configuration | `CONFIG_ARM64_VA_BITS_39=y` | `~/firmware-check/kernel.config` |

## Boot-image inspection results (2026-10-28)

The repository's `make samsung-inspect` target was run against the extracted firmware artifacts. Its output reported:

| Check | Result |
|---|---|
| Android boot header | Version 2, legacy layout |
| Page size | 4,096 bytes |
| Kernel payload offset | `0x1000` |
| Kernel payload size | 51,957,772 bytes (`0x318d00c`) |
| Kernel payload end | `0x318e00c` |
| Kernel payload SHA-256 | `ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123` |
| Extracted Image comparison | Exact byte-for-byte match |
| `CONFIG_IKHEADERS` | Not set in the parsed config |
| `CONFIG_DEBUG_INFO_BTF` | Not listed in the parsed config |
| BTF magic byte-pattern scan | Zero hits in the boot image, kernel payload, raw Image, and vendor ELF |

The byte comparison establishes that the payload selected by the legacy header parser matches the separately extracted raw Image. The BTF scan is only a byte-pattern heuristic: zero hits do not independently prove the absence of valid BTF metadata. Likewise, a config option not listed in the parsed file is not equivalent to a positive proof about every possible external metadata source.

This inspection validates input layout and provenance only. It does not establish the runtime memory map, structure layouts, or device-specific constants. The Samsung directory remains documentation-only and deliberately has no `target.h`.

## Verified stock configuration flags

The extracted stock config reports the following enabled:

- `CONFIG_FUTEX=y`
- `CONFIG_FUTEX_PI=y`
- `CONFIG_RT_MUTEXES=y`
- `CONFIG_PREEMPT=y`
- `CONFIG_KALLSYMS=y`
- `CONFIG_KALLSYMS_ALL=y`
- `CONFIG_KALLSYMS_BASE_RELATIVE=y`
- `CONFIG_SECURITY_SELINUX=y`
- `CONFIG_IKCONFIG_PROC=y`
- `CONFIG_ARM64_VA_BITS_39=y`

The local `out-stock-layout/.config` reported `CONFIG_RELOCATABLE=y` and `CONFIG_RANDOMIZE_BASE=y`, but those results came from the reference build config—not the extracted firmware config. Do not label them as stock facts until independently confirmed in `~/firmware-check/kernel.config`.

Run `python3 tools/verify_samsung_firmware.py` from the repository root to check the config and the recorded artifact digests/mappings. The script only validates provenance/configuration; it does not populate exploit constants.

## Verification run results (2026-10-28)

The user ran `make samsung-verify` in WSL from the repository root. The recorded command output showed:

- PASS: SHA-256 for the stock boot image, raw kernel Image, and reconstructed vendor ELF matched the pinned values below.
- PASS: all ten required stock configuration flags checked by `tools/verify_samsung_firmware.py`.
- PASS: vendor ELF `.kernel` section matched Image offset `0x0` for `0x2695260` bytes.
- PASS: vendor ELF `.kernel2` section matched Image offset `0x29da458` for `0x7b2bb4` bytes.
- PASS: Android NDK r27c, API 33 compiler target `aarch64-unknown-linux-android33`, AArch64 ELF64 PIE output, Android `/system/bin/linker64` interpreter, and AArch64 ELF64 shared-library smoke test.
- Overall verifier result: `checks passed; profile remains documentation-only`.

These results establish reproducible artifact identity, the checked configuration flags, the two recorded ELF section byte mappings, and basic NDK/ELF compatibility. They do not validate an executable Samsung exploit profile or runtime memory-layout assumptions.

## Current verification status (2026-10-28)

The user reported **PASS** for `make samsung-verify` after the real-image inspection step and expanded build-guard regression test were integrated. The combined target checks:

- pinned hashes, required stock config flags, and ELF-to-Image section mapping;
- the actual stock boot image against the extracted raw kernel Image, plus config/ELF metadata and heuristic BTF-signature scan;
- Android NDK r27c/API 33 AArch64 PIE and shared-library smoke tests;
- the eleven-method read-only inspection/build-guard regression suite, including all seven enumerated payload build goals.

The reported pass is evidence that these checks completed successfully in the user's WSL environment at that revision. It does **not** mean a Samsung payload is buildable or its runtime memory-layout assumptions are verified. The Samsung profile remains documentation-only and has no `target.h`.

## Boot-header reference

The official [Android Open Source Project boot image header guide](https://source.android.com/docs/core/architecture/bootloader/boot-image-header) describes header v2's legacy layout and distinguishes it from the v3/v4 layout. The matching [AOSP `bootimg.h` definitions](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/android16-qpr2-release/include/bootimg/bootimg.h) document the v2 structure and page-aligned payload layout.

This repository's inspector has exercised the observed v2 image and v3/v4 synthetic fixtures. That proves the metadata parser's supported layouts and checks the observed kernel payload provenance; it does not change the upstream generator's independent compatibility limitations recorded below.

## Build-guard regression coverage

The build-guard test checks all seven Makefile payload goals (`all`, `preload`, `s2`, `s3`, `rootchain`, `closed-loop`, and `hello-dso`) in dry-run mode and expects each to fail with the evidence-only guard while this profile has no `target.h`. The full `make samsung-verify` command was reported as passing after this test and the real-image inspection were integrated.

## Repeatable generator preflight

A read-only source preflight is available:

```bash
make samsung-generator-audit UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen
```

For a combined compatibility check, run:

```bash
make samsung-generator-verify \
  UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen
```

This first runs `tools/verify_generator_boot_parser.py`: synthetic cases exercise both vendored parser implementations, and the real Samsung v2 boot image must yield a kernel payload that matches the supplied raw `Image` byte-for-byte. It then runs the source-level compatibility audit, which checks the accepted boot-header layouts and whether no-BTF / failed-validation paths are demonstrably fail-closed. The command does not generate `target.h` or run the generator's profile-generation pipeline.

The default `UPSTREAM_GENERATOR_ROOT` is `/tmp/ghostlock-profile-gen`. The script reads the boot header and statically parses the external generator's `expected_header_sizes` map and `extract_boot_kernel` legacy-path logic. It also reports markers for the no-BTF fallback's default `rodin` template/inheritance path.

The audit does **not** import or execute the external generator and does not write `target.h`. Exit status 1 means it found a confirmed header-version incompatibility; exit status 2 means it could not confidently interpret the parser or complete the audit. With the currently reviewed upstream sources and this Samsung v2 image, `RESULT=BLOCKED` is expected. That result is a confirmed readiness blocker, not a regression in `make samsung-verify`.

The source audit is a lightweight static preflight, not a substitute for reviewing changes to the generator source if its implementation changes.

## Upstream generator compatibility audit (2026-10-28)

The findings immediately below describe the unpatched upstream baseline. The local WSL checkout was subsequently patched and tested; see **Current external-generator verification** below.

Reviewed the generator source from `woshimaniubi8/CVE-2026-43499-root-KernelSU`:

- Wrapper: [`tools/generate_target.py`](https://github.com/woshimaniubi8/CVE-2026-43499-root-KernelSU/blob/main/tools/generate_target.py) (reviewed blob `a2e6a40817a22dbc25b65f1f1c00f03faad4c0a1`).
- Vendored full generator: [`tools/vendor/generate_target.py`](https://github.com/woshimaniubi8/CVE-2026-43499-root-KernelSU/blob/main/tools/vendor/generate_target.py) (reviewed blob `67df9a0a3e87e9ff1ecb36af76fed29e6d154b03`).
- Vendored profile generator: [`tools/vendor/generate_target_2.py`](https://github.com/woshimaniubi8/CVE-2026-43499-root-KernelSU/blob/main/tools/vendor/generate_target_2.py) (reviewed blob `e1de6f952fe734455538bb839b4308b8e0f513ac`).

### Findings

1. **Boot header v2 is rejected explicitly by both vendored parsers.** Both define accepted header sizes only for versions 3 and 4 (`{3: 0x62C, 4: 0x630}`) and fail with a message that only v3/v4 are supported. The Samsung image we inspected is header v2, so the current generator path stops at boot parsing before BTF detection or generation. This is an explicit incompatibility, not a silent parse.
2. **The wrapper's no-BTF path is a soft failure for otherwise accepted images.** If BTF lookup fails, `complete_generate()` invokes `fallback_generate_without_btf()`. That function defaults the template to `rodin` unless `--template-target` or `TARGET_TEMPLATE` changes it, and then writes an output header.
3. **The fallback does inherit unresolved macros.** It rewrites known symbols and a subset of layout macros; other values remain from the chosen template. The emitted header and stderr do include warnings, and the function returns success (`0`), so this is not silent inheritance—but a successful exit/output is not proof of a complete or compatible profile.
4. **BTF validation also warns and continues.** The wrapper catches exceptions from `validate_btf_consumer_layout()` and `validate_percpu_entry_task()`, emits warnings, and continues toward output generation rather than treating failed validation as a hard error.
5. **No-BTF geometry includes assumptions.** The fallback synthesizes the direct-map end as `p0_page_offset + (1 << 36)` and computes a VMEMMAP start from a formula rather than proving those ranges from Samsung runtime evidence. Some missing symbol values can also leave template values untouched with a warning.

### Porting implications

- In the pristine upstream baseline, the generator's boot parser is incompatible with this Samsung image's confirmed v2 header.
- In that baseline, the no-BTF fallback can still emit a nominally successful header with inherited or assumed values; this must be separately corrected even after parser compatibility is added.
- The Samsung branch keeps the target profile documentation-only. Parser tests, firmware checks, and ABI smoke tests do not establish runtime memory layout or justify carrying forward values from another device.

## Local generator parser patch status (2026-10-28)

The pristine upstream generator source reviewed above rejects legacy boot-header v2 and requires a PE/COFF signature in the ARM64 Image. The user applied local patch scripts in the external WSL checkout at `/tmp/ghostlock-profile-gen`; those upstream files are not vendored or committed by this scaffold repository.

The user-reported `make samsung-generator-verify UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen` run passed:

- Both vendored parsers passed 13 synthetic cases each (26 total).
- Both parsers extracted the kernel payload from `~/stock-boot.img` and matched `/tmp/g986u1-stock-kernel.Image` byte-for-byte.
- Extracted payload SHA-256 matched `ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123`.
- The static source audit reported both parsers support legacy versions 0–2 alongside versions 3–4, with an explicit legacy page-size path.
- The audit reported `automatic_fallback_route=no`, `btf_validation_fail_closed=yes`, and `percpu_validation_fail_closed=yes`, ending with `RESULT=HEADER_MAP_SUPPORTED_FAIL_CLOSED (static inspection only)`.

This is a parser/provenance and source-inspection result. It does not validate BTF, structure layouts, target memory geometry, or generated profile values, and the audit does not execute the generator. The external checkout at `/tmp/ghostlock-profile-gen` is separate from this repository.

To make the legacy parser change reproducible, this repository now includes `tools/patch_generator_legacy_boot.py` and unit tests for check-only mode, backups, idempotency, equivalent pre-existing implementations, and all-files preflight. Run its check target first:

```bash
make samsung-generator-legacy-patch-check \
  UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen
python3 -m unittest discover -s tests \
  -p 'test_patch_generator_legacy_boot.py' -v
```

The check target must report either `already patched` or `patchable` for both parser files before applying anything. The Samsung profile remains documentation-only and no `target.h` has been added.

## Debug-metadata inventory (read-only)

Use the repository tool to inventory debug metadata in the reconstructed vendor ELF:

```bash
python3 tools/inspect_elf_debug_metadata.py "$HOME/firmware-check/vmlinux.elf"
```

The report includes ELF class/machine, build ID, section sizes, presence of DWARF core/support sections, BTF sections, separate-debug links, and whether GNU `readelf` can parse DWARF compilation-unit headers. It hashes the ELF in streaming chunks and does not print symbol names, symbol addresses, structure offsets, or derived profile values.

Interpretation is deliberately conservative:

- `.debug_info` / `.zdebug_info` plus `.debug_abbrev` / `.zdebug_abbrev` establish section presence only, not completeness.
- `dwarf_info_decode=PASS` means readelf parsed at least one compilation-unit header without warnings in the captured output; it does not prove that every type or variable required by a consumer is available.
- A `.gnu_debuglink`, `.gnu_debugaltlink`, `.debug_sup`, or index section can indicate external/separate debug data. The referenced artifact must be found and matched independently.
- `.symtab` presence is not by itself proof that all symbols are defined or that addresses are valid for the shipped kernel.
- BTF-section presence is reported separately from DWARF; neither format should be assumed available merely from the kernel version.

Regression tests: `python3 -m unittest tests.test_elf_debug_metadata -v` may not work because `tests/` is not a Python package; the portable command is `python3 -m unittest discover -s tests -p 'test_elf_debug_metadata.py' -v`.

## Source vmlinux debug-metadata inventory at `firmware-check/vmlinux.elf` (user-run, 2026-10-28)

For this port, the user identifies `~/firmware-check/vmlinux.elf` as the source vmlinux artifact to inspect. The user ran `tools/inspect_elf_debug_metadata.py` against that exact file. Reported results:

| Check | Result |
|---|---|
| File size | 58,474,904 bytes |
| SHA-256 | `531444e3c98f8f7c4d2baefeda1386aca2b60a42030fcb1f4393c5e10b9154b4` |
| ELF class / machine | ELF64 / AArch64 |
| Build ID | Not present |
| Section count | 8 |
| DWARF core sections | Absent |
| BTF sections | Absent |
| Separate-debug reference sections | Absent |
| Symbol-table sections | `.symtab` present (3,490,416 bytes) |
| DWARF decode | `NOT_PRESENT` |

Interpretation: the reconstructed vendor ELF has a regular symbol-table section, but the inventoried file does not expose DWARF core sections, BTF sections, or common separate-debug references. A `.symtab` section alone does not establish that the type/layout debug information needed by other tools exists. The lack of embedded references is not proof that no matching external debug artifact exists elsewhere.

The next read-only comparison is to inspect the *separately linked* source-tree vmlinux, if present, with the same tool:

```bash
python3 tools/inspect_elf_debug_metadata.py "$HOME/s20_kernel_chxl1/out-stock-layout/vmlinux"
```

Do not treat metadata from that source-built ELF as metadata from the shipped kernel merely because the source tree corresponds to the same firmware family. The source-built artifact is a comparison candidate only; matching provenance and binary correspondence must be established independently.

A convenience target is also available:

```bash
make samsung-debug-metadata
```

It inventories the reconstructed vendor ELF path selected by `SAMSUNG_VENDOR_ELF`; it does not derive symbols, offsets, or profile values.

## ELF structural validation

The inspector now also checks the ELF section-table count and indices, identifies file-backed sections whose ranges exceed the ELF file size, and runs GNU `readelf --lint` across the ELF header, sections, and symbol table. It reports only the lint status and diagnostic counts rather than echoing diagnostic text that could contain names or addresses. GNU `readelf --lint` is documented to emit warnings about possible problems in the examined file. See the [GNU readelf manual](https://sourceware.org/binutils/docs/binutils/readelf.html).

The optional `--strict-structure` mode returns nonzero if the section-table count/indices are inconsistent, any file-backed section exceeds the file boundary, lint reports warnings/errors, or a symbol table is only partially parsed. DWARF/BTF absence itself is not treated as an ELF structural error, since that is a metadata-availability finding rather than evidence of malformed ELF.

The `make samsung-verify` recipe now runs this strict structural check against `$(SAMSUNG_VENDOR_ELF)`. The user should rerun the full verifier after pulling the latest branch to establish the current ELF's structural status.

## Debug-artifact search in `firmware-check` (user-run, 2026-10-28)

The user searched `$HOME/firmware-check` to depth 3 for files named `vmlinux*`, `*.debug`, `*.dwo`, `*.dwp`, and `*.btf`. The only match was:

- `/home/ubuntu/firmware-check/vmlinux.elf` (58,474,904 bytes)

Combined with the inventory above, this establishes that the searched directory contains no matching sidecar debug files, and the sole matching ELF has no embedded DWARF core sections, BTF sections, or recognized separate-debug references. This is scoped to that search path/pattern/depth; it does not establish that no relevant metadata exists in the source tree, firmware package, build cache, or other directories.

The user's first symbol-summary run reported `.symtab` as fully parseable: 145,434 declared and parsed entries, 145,051 defined entries, 382 undefined entries, and one reserved index-zero entry. The category accounting patch following that run now excludes the reserved null entry from type/binding distribution totals; those revised distributions still need a fresh local run.

The next enhanced inventory reports aggregate symbol-table counts and type/binding categories without emitting symbol names or values. This can establish whether the existing `.symtab` is parseable and how many entries are defined versus undefined, but it still cannot establish the presence of DWARF type information or validate a target-specific profile.

## Artifact provenance ledger

These files were collected under WSL from the original Samsung firmware package:

| Artifact | Path used during inspection | SHA-256 |
|---|---|---|
| Original boot image | `~/stock-boot.img` | `bd8987c28ef0b9fdb56ad3e82ce3b45929c5e53717525769e5625dd1e17f7dc8` |
| Extracted raw kernel Image | `/tmp/g986u1-stock-kernel.Image` | `ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123` |
| Vendor ELF | `~/firmware-check/vmlinux.elf` | `531444e3c98f8f7c4d2baefeda1386aca2b60a42030fcb1f4393c5e10b9154b4` |
| Stock config | `~/firmware-check/kernel.config` | Not pinned here; run the verifier against the current file |

The original AP archive's `boot.img.lz4` was decompressed and compared against `~/stock-boot.img`; they were byte-identical. The vendor ELF sections `.kernel` and `.kernel2` were each compared in full against the raw Image at their corresponding mapped file offsets; both comparisons passed.

The vendor ELF's section/symbol metadata is incomplete/nonstandard. In particular, its `selinux_state` entry is `UND`, so do not interpret that entry as a normal defined symbol. The independently linked `out-stock-layout/vmlinux` is only a source-build cross-check, not the shipped kernel and not a source of interchangeable target values.

## Why this is not a K40 profile with a new name

The reference profile targets a different device/build: Qualcomm 4.19.157-perf, versus this Samsung kernel's 4.19.113 release. Same major/minor kernel family does not establish compatibility for memory layout, vendor code, allocator configuration, linker output, or structure offsets. Most importantly, this device's stock config says `CONFIG_ARM64_VA_BITS_39=y`; the K40 profile hardcodes a different virtual-address layout. The following data therefore remain **unfilled and unverified** in this scaffold:

- kernel virtual/direct-map/VMEMMAP geometry and physical-memory window;
- KASLR/runtime mapping assumptions;
- all per-build symbol and data addresses;
- structure and allocator layout values used by the payload;
- kernel-specific fake-object / spray geometry;
- end-to-end runtime behavior.

A future `target.h` must be created only after each required value has explicit evidence tied to this exact Samsung build. Never fill unresolved fields by copying constants from `oplus-4.19.157-perf`.