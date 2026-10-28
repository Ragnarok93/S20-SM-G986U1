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

## Combined verification milestone

After the enabled-BTF reporting test was added and `samsung-verify` was updated to run the regression suite, the user reported the combined command passed. At that time, the suite contained ten test methods. Together with the earlier detailed verifier output, this records successful firmware artifact/config checks, ELF section byte comparisons, Android NDK API 33 ABI smoke checks, and the then-current inspector regression tests.

The additional build-guard regression test was then run as part of `make samsung-verify` and the user reported the full combined command passed. The guard verifies that `make PROJECT=samsung-g986u1-4.19.113 preload -n` is rejected while this directory lacks `target.h`. The passing metadata suite does not change the profile's documentation-only status.

## Build-guard regression coverage

The previously passing guard test covered the `preload` target. It has now been expanded to exercise the full set of Makefile payload goals (`all`, `preload`, `s2`, `s3`, `rootchain`, `closed-loop`, and `hello-dso`) in dry-run mode and assert each one fails with the evidence-only guard while this profile has no `target.h`.

The user then ran the combined `make samsung-verify` after the expansion and reported PASS. The current suite has 11 test methods; the build-guard test checks all seven payload goals in dry-run mode and asserts the evidence-only guard message. This is evidence that the Makefile guard is functioning for the enumerated goals in the tested local environment, not evidence that a Samsung payload is buildable.

## Upstream generator compatibility audit (2026-10-28)

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

- Current first blocker: the generator's boot parser is incompatible with this Samsung image's confirmed v2 header.
- Separate fallback risk: if header parsing is changed or a supported-format image is supplied, the current no-BTF fallback can still emit a nominally successful header with inherited or assumed values.
- Our Samsung branch keeps the target profile documentation-only. The validator and ABI smoke test passing do not clear either generator limitation or establish runtime layout.

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