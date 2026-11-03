# Samsung Galaxy S20+ target-profile scaffold

**Status: target-derived and buildable; on-device behaviour unvalidated.** `target.h` is generated from the recorded firmware and kernel tree by `make samsung-layout-probe` + `make samsung-profile-derive`, and `make samsung-profile-gate` enforces its evidence tags before any link. See [Derived target profile](#derived-target-profile-2026-10-30). Do not hand-edit the header or copy the K40 table.

The device facts and pinned artifact/configuration expectations now live in [`device-profile.json`](device-profile.json), which is consumed by `tools/verify_samsung_firmware.py`. Run `make samsung-device-check` for a read-only ADB check of the connected device identity, Android build, ABI, and kernel release. Run `make samsung-source-check` to record the local kernel tree revision/origin, confirm its `4.19.113` source baseline, and list likely device defconfigs. The source-tree check is deliberately read-only and distinguishes source-version compatibility from proof that this source tree exactly matches the shipped firmware. These checks add repeatable compatibility gates; they do not by themselves mark the device port complete. Run `make samsung-config-compare` to compare the extracted stock `kernel.config` with `~/s20_kernel_chxl1/out-stock-layout/.config`; differences in SoC selection, ARM64 layout, futex/scheduler, symbol/debug, and security-hardening groups are core compatibility blockers, while other device/project changes are listed for explicit review. Add `SAMSUNG_CONFIG_STRICT=1` to fail on any config delta.

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

This inspection validates input layout and provenance only. It does not by itself establish the runtime memory map or structure layouts; those are derived separately from the vendor ELF and the matching kernel tree in the section below.

## KernelSU-Next / SUSFS compatibility assessment

The source/reference assessment is maintained in [`docs/samsung-g986u1-kernelsu-susfs-compatibility.md`](../../../docs/samsung-g986u1-kernelsu-susfs-compatibility.md). A Snapdragon S20-series 4.19.113 reference exists, but it disables RKP/KDP, DEFEX, PROCA, and CFP; its security-configuration changes must not be copied as the default for this target. The relevant SUSFS source lineage is the `kernel-4.19` branch, whose patch expectations must be reconciled with the exact KernelSU-Next revision before integration is claimed.

Current status: **reference reviewed; SUSFS integration not yet verified**. KernelSU hook mode, source patch compatibility, final configuration, preservation of Samsung security facilities, and build/runtime behavior remain separate gates. An absent line in the parsed stock config must not be treated as an enabled or disabled setting without explicit evidence.

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

The reported pass is evidence that these checks completed successfully in the user's WSL environment at that revision. At that revision the profile had no `target.h`; the derived header now in this directory is a later, separately gated step. The historical pass does **not** by itself validate runtime behaviour.

## Boot-header reference

The official [Android Open Source Project boot image header guide](https://source.android.com/docs/core/architecture/bootloader/boot-image-header) describes header v2's legacy layout and distinguishes it from the v3/v4 layout. The matching [AOSP `bootimg.h` definitions](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/android16-qpr2-release/include/bootimg/bootimg.h) document the v2 structure and page-aligned payload layout.

This repository's inspector has exercised the observed v2 image and v3/v4 synthetic fixtures. That proves the metadata parser's supported layouts and checks the observed kernel payload provenance; it does not change the upstream generator's independent compatibility limitations recorded below.

## Build-guard regression coverage

The build-guard test now checks both states: with the derived `target.h` present the evidence guard must not fire and the port path must parse; with the header temporarily removed all seven payload goals must fail with the evidence-only message. The full `make samsung-verify` command was reported as passing after the earlier guard test and the real-image inspection were integrated.

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

## Derivation and build gate (2026-10-30)

The pipeline is now complete: `target.h` is generated from this build's ELF, config and source tree, and a header that is incomplete, untagged or inherited from another device still cannot reach a build.

```bash
make samsung-profile-derive \
  SAMSUNG_VMEMMAP_START=0x... \
  SAMSUNG_LAYOUT_JSON="$HOME/firmware-check/g986u1-layout.json"
make samsung-profile-check     # evidence review: reports remaining [D] assumptions
make samsung-profile-gate      # what the payload build enforces
```

`tools/derive_samsung_target.py` composes the header from three sources and refuses to write anything if a value is missing:

| Source | Tag | Where it comes from |
|---|---|---|
| Symbol offsets | `[A]` | `.symtab` of the pinned vendor ELF (or a `System.map`) |
| Struct/allocator layout | `[B]` | `--layout` JSON derived from the matching source tree |
| Address-space geometry | `[C]` | 4.19 arm64 layout from `--va-bits`/`--text-offset`/`--phys-offset`/`--vmemmap-start` |

Undefined or zero-valued ELF symbols are rejected rather than resolved, `VMEMMAP_START` is never inferred, and target-selector/config flags are checked first. The emitted header records the SHA-256 of every input.

`tools/verify_samsung_target_profile.py --mode build` is the gate. It fails when a macro required by the payload is missing, untagged, still tagged `[D]`, numerically identical to another profile's build-specific value, inconsistent with the fixed 4.19 field order, or inconsistent with `KIMAGE_TEXT_BASE + <offset>`. The Makefile runs it automatically before any Samsung payload link, so a copied or guessed header cannot produce a binary.

Still required before the first build: the vendor ELF, `kernel.config`, and a layout JSON derived from the G986U1 source tree. Until they exist, `make PROJECT=samsung-g986u1-4.19.113 preload` continues to fail closed.

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
- Parser tests, firmware checks, and ABI smoke tests do not establish runtime memory layout; the derived header is a separate evidence tier. Values are never carried forward from another device.

## Local generator parser patch status (2026-10-28)

The pristine upstream generator source reviewed above rejects legacy boot-header v2 and requires a PE/COFF signature in the ARM64 Image. The user applied local patch scripts in the external WSL checkout at `/tmp/ghostlock-profile-gen`; those upstream files are not vendored or committed by this scaffold repository.

The user-reported `make samsung-generator-verify UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen` run passed:

- Both vendored parsers passed 13 synthetic cases each (26 total).
- Both parsers extracted the kernel payload from `~/stock-boot.img` and matched `/tmp/g986u1-stock-kernel.Image` byte-for-byte.
- Extracted payload SHA-256 matched `ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123`.
- The static source audit reported both parsers support legacy versions 0–2 alongside versions 3–4, with an explicit legacy page-size path.
- The audit reported `automatic_fallback_route=no`, `btf_validation_fail_closed=yes`, and `percpu_validation_fail_closed=yes`, ending with `RESULT=HEADER_MAP_SUPPORTED_FAIL_CLOSED (static inspection only)`.

This is a parser/provenance and source-inspection result. It does not validate BTF, structure layouts, target memory geometry, or generated profile values, and the audit does not execute the generator. The external checkout at `/tmp/ghostlock-profile-gen` is separate from this repository.

To make the legacy parser change reproducible, this repository now includes `tools/patch_legacy_boot_v2.py`, based on the locally tested patch utility, plus unit tests for check-only mode, backups, idempotency, `--no-backup`, and all-files preflight. Run its check target first:

```bash
make samsung-generator-patches-check \
  UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen
python3 -m unittest discover -s tests \
  -p 'test_patch_legacy_boot_v2.py' -v
python3 -m unittest discover -s tests \
  -p 'test_patch_optional_arm64_pe_stub.py' -v
```

The combined check target must report either `ALREADY_PATCHED` or `READY` for both legacy-v2 parser files, then report `already patched` or `patchable` for the optional PE-stub block. These checks make no source changes. After reviewing the complete check output, `make samsung-generator-patches UPSTREAM_GENERATOR_ROOT=/tmp/ghostlock-profile-gen` applies both source-only parser patches, with separate backups. Neither target runs profile generation or touches the derived `target.h` in this profile.

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

The derived `target.h` exists only because each required value has explicit evidence tied to this exact Samsung build. Never fill unresolved fields by copying constants from `oplus-4.19.157-perf`; the build gate rejects identical build-specific values.
## Derived target profile (2026-10-30)

`target.h` is generated, not hand-written, from the artifacts recorded above.

| Input | Pinned value |
|---|---|
| Vendor ELF | `~/firmware-check/vmlinux.elf`, sha256 `531444e3c98f8f7c4d2baefeda1386aca2b60a42030fcb1f4393c5e10b9154b4` |
| Raw Image | `kernel.bin`, sha256 `ffbc90af676558557eb917687736c11f15b560dbb3cb9ca237224da5acb14123` |
| Stock config | `kernel.config`, sha256 `ec6da4cfe412f7d0669c7d621751753375e32197a2602f98627eb95e71b9c0b2` |
| Layout source | `~/s20_kernel_chxl1` (no VCS metadata; key headers hashed below) |
| Layout JSON | `~/firmware-check/g986u1-layout.json`, sha256 `20d3f3ac0bd6f9b40dcb875e3f295d315d9b1217199b5dd21a3f0a7671015cae` |

Derivation chain:

1. `make samsung-layout-probe` copies `tools/samsung_probe/` under `build/`, compiles it as an out-of-tree module against `out-stock-layout` with clang 10.0.6 (the compiler named in the stock kernel banner) and the stock config, then `tools/read_samsung_layout.py` converts the `offsetof` data into the layout JSON. The reader fails if the tree does not match the recorded firmware anchors.
2. `make samsung-profile-derive` resolves 51 symbol values from `.symtab`, 107 layout values and 10 architecture constants. It writes nothing until all 166 required macros resolve.
3. `make samsung-profile-gate` (also a prerequisite of the `preload` link) enforces tags, ordering, alias arithmetic and the no-copy rule.

Values confirmed directly in the shipped image by disassembly, not only from the source tree:

| Field | Value | Evidence |
|---|---|---|
| `_text` / `KIMAGE_TEXT_BASE` | `0xffffff8008080000` | `.symtab` `_text` |
| `VMEMMAP_START` | `0xffffffbf00000000` | `mem_init` materialises the constant (`mov #0xffbf00000000`, `movk #0xffff, lsl #48`) |
| `TASK_REAL_CRED_OFF` | `0x7f0` | `get_task_cred`: `ldr x20, [x19, #2032]` |
| `TASK_PI_LOCK_OFF` | `0x8c8` | `task_blocks_on_rt_mutex`: `add x24, x2, #0x8c8` |
| `PIPE_NRBUFS_OFF` / `PIPE_BUFS_OFF` | `0x60` / `0xa0` | `pipe_write`: `[x20, #96]`, `[x20, #160]` |
| `MM_STRUCT_SZ` | `0x400` | SLUB object stride: `sizeof(struct mm_struct)` is `0x3c8`, but `SLAB_HWCACHE_ALIGN` rounds the cache to `ALIGN(0x3c8, 0x40) = 0x400`. The KernelSnitch scan walks this stride from a 32KB slab base, so a `0x3c8` grid never lands on a real object. Proven offline against the 2026-10-30 device export: at `0x400` with the kernel futex key order exactly one candidate in the 11GB direct map agrees 16/16 (`0xffffffc18f2e1400`, bucket `0x34c`); at `0x3c8`, or with the old swapped key order, none does. |
| `PIPE_INODE_INFO_STRUCT_SIZE` | `0xb0` | `sizeof(struct pipe_inode_info)` in the matching tree |

### Reviewed coincidences

Five derived values legitimately equal another profile's, so a pure no-copy rule
cannot tell them apart from inherited values. They are not exempted by name: each
is pinned to the exact reviewed value in
[`coincidence-review.json`](coincidence-review.json), together with the site in
the shipped image that confirms it. Build mode fails if a pin and the header
disagree, and it fails if a macro is exempted without a pin (or pinned without
being exempted), so an exemption cannot absorb a value nobody checked.

| Macro | Value | Independent confirmation in the shipped image |
|---|---|---|
| `FAKE_TASK_USAGE_OFF` | `0x68` | `copy_process`: `str w8, [x20, #104]` initialises the task refcount |
| `FAKE_TASK_PRIO_OFF` | `0xac` | `set_user_nice` reads `static_prio` at `[x19, #176]` and `state` sits at `[x19, #168]`; `prio` precedes `static_prio` |
| `FAKE_TASK_NORMAL_PRIO_OFF` | `0xb4` | `static_prio` = `0xb0`, and `normal_prio` follows it |
| `KMALLOC_PIPE_INDEX` | `0xa` | `alloc_pipe_info` sizes the ring as `n * 40` bytes (16 buffers = 640 B → cache index 10) |
| `KMALLOC_PIPE_OBJ_SIZE` | `0x400` | Same ring allocation: 640 B rounds up to the `0x400` kmalloc cache |

Key header hashes for the layout source (no `.git` is present in the supplied tree):

```text
ashmem.c       ade1f53b488ea092bf585c090245a2b0f6f4590cf3a282d6beb15ffe42f62a7d
hooks.c        60f99267a319cc802c9dced4bec52bc668f775ed416d7d57e74d0d8a8993544c
configfs/file  ecb9d77982bb42f1dc92d0c5b2e6304cf52785a57efb2971af9733430f07b746
sched.h        8afa0e078f953ec84b5f088e90e81a9d4212a4464f16f2a516a3f402e3f8a3e7
pipe_fs_i.h    907650c7b139ecb4e45933ba2450a8f484b53c3cfcabea1a047bad299b9e9395
memory.h       82db467c3cd3fb0ecc14473aa05b3c59116fdc4a512132a7178aa994a140d5fe
```

### Vendor divergences that needed code, not just numbers

- **SELinux.** This tree keeps the legacy `int selinux_enforcing` (not `selinux_state.enforcing`) and places it in `.kdp_ro` with `CONFIG_KDP_NS/DMAP/CRED=y`. A write from EL1 would fault, so `root.c` skips it when `SELINUX_ENFORCING_KDP_RO=1` and relies on the cred-SID patch.
- **ashmem name prefix.** The driver writes `dev/ashmem/` (11 bytes) at `struct ashmem_area.name`, so `ASHMEM_NAME_PREFIX_LEN=11` and every fake `configfs_buffer` field is shifted by 11. `configfs_buffer.count` (offset 0) is the fixed prefix and cannot be written; it remains a large positive value, which `simple_read_from_buffer` treats as an effectively unlimited source.
- **No `random_boot_id`.** The symbol is absent from this build (and from the whole image), so `SLIDE_BOOTID_DATA_PRESENT=0` and the cosmetic boot-id restore is compiled out. The slide route is unsupported on this target; the FOPS/configfs/pipe route is the port path.
- **kmalloc geometry.** `NR_KMALLOC_TYPES=2` (no `CONFIG_ZONE_DMA`; 4.19 has no per-memcg kmalloc cache dimension). The pipe-buffer array is 16 x 0x28 = 0x280 bytes, so `KMALLOC_PIPE_INDEX=10` and `KMALLOC_PIPE_OBJ_SIZE=0x400`.

### Build result

`make PROJECT=samsung-g986u1-4.19.113 preload` produces `build/samsung-g986u1-4.19.113/bin/preload.so` (ELF64 AArch64 shared object, `NEEDED libc.so` and `libdl.so`). This is a build-level result only. No on-device run has been performed. The staged procedure that must be followed, including the read-only pre-flight that blocks the first kernel write, is [the device-validation runbook](../../../docs/targets/samsung-g986u1-4.19.113-device-validation.md); its runtime gates remain unclaimed.

## Stock-kernel port preflight

This port uses the existing Samsung stock kernel. It does not integrate KernelSU/KernelSU Next into the kernel, modify the kernel tree, or rebuild the kernel. KernelSU and KernelSU Next are separate projects and are not requirements for this repository's port checks.

Run the read-only preflight against the local WSL source tree and its existing config:

```bash
make samsung-stock-kernel-check \
  SAMSUNG_KERNEL_SOURCE="$HOME/s20_kernel_chxl1" \
  SAMSUNG_SOURCE_CONFIG="$HOME/s20_kernel_chxl1/out-stock-layout/.config"

python3 -m unittest discover -s tests -p 'test_verify_stock_kernel_port.py' -v
```

The preflight verifies the source tree's Linux version, expected source layout, preferred vendor defconfig path, and recorded stock configuration expectations. It does not require `drivers/kernelsu` or `CONFIG_KSU`; neither is part of this repository's kernel scope. It reads files only and does not modify the kernel or its config.
