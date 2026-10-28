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