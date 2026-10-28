# Samsung G986U1 / 4.19.113 port-validation checklist

**Scope:** provenance, configuration, ABI/build reproducibility, and documentation. This checklist deliberately does not derive exploit parameters or validate privilege-escalation payload stages.

## 1. Source-of-truth policy

- [x] Verify `stock-boot.img` against `boot.img.lz4` extracted from the original AP archive.
- [x] Verify the `.kernel` section of `vmlinux.elf` byte-for-byte against the raw kernel `Image` at offset `0`.
- [x] Verify `.kernel2` byte-for-byte at Image offset `0x29da458`.
- [x] Record SHA-256 hashes of the source artifacts.
- [ ] Keep a copy of the exact AP archive filename and `kernel.config` alongside any future report.
- [ ] Do not treat `out-stock-layout/vmlinux` or its symbol addresses as the shipped kernel.

## 2. Known build-entry-point inconsistency in the reference repository

At the inspected revision, `Makefile` defaulted `PROJECT` to `blazer-CP2A.260605.012`, while `build.sh` hardcoded `oplus-4.19.157-perf`; their NDK defaults also differed (`android-ndk-r29` versus `android-ndk-r27c`). The accompanying patch corrects this build-plumbing mismatch by requiring an explicit `PROJECT` and making `build.sh` delegate to the Makefile for compiler/API selection. The wrapper retains the existing `prebuilt/` copy behavior. The Samsung scaffold is intentionally not a selectable profile because it has no `target.h` and is not build-ready.

## 3. Target baseline checks

- [x] Device identifier recorded: `SM-G986U1`, family `y2q`.
- [x] Stock kernel release recorded: `4.19.113-27095354`.
- [x] Architecture recorded: AArch64.
- [x] Stock config records `CONFIG_ARM64_VA_BITS_39=y`.
- [x] Stock config includes futex PI and RT-mutex support.
- [ ] Document exact source tree commit/revision used for comparisons.
- [x] Prevent accidental profile selection: build now requires explicit `PROJECT`; utility goals `list-projects` and `clean` remain available without it.
- [x] Use one build path: `build.sh` delegates compiler/API/NDK selection to the Makefile.
- [ ] Review the full config diff rather than only the first page of output.
- [x] Record the userspace smoke-test toolchain separately from the kernel compiler: Android NDK r27c (`27.2.12479018`), Clang 18.0.3, Android API 33.


## 4. Userspace ABI smoke test

On the WSL host, Android NDK r27c at `$HOME/ndk/android-ndk-r27c` was tested with `aarch64-linux-android33-clang`:

- [x] Compiler target reports `aarch64-unknown-linux-android33`.
- [x] Minimal standalone PIE builds as ELF64 AArch64 and requests `/system/bin/linker64`.
- [x] Minimal shared library builds as ELF64 AArch64; its observed dynamic dependencies were `libc.so` and `libdl.so`.
- [x] Equivalent smoke test also built under API 35; API 33 is the more relevant baseline for this Android 13 device.
- [ ] Inspect the real project's declared libc/API symbol requirements separately before making any runtime compatibility claim.

Re-run from the repo root with `tools/check_android_abi.sh`. This test only verifies the compiler/ELF baseline; it does not build or validate the project payload or its runtime behavior.

## 5. Compatibility evidence ledger

For each future change, record the evidence source and confidence—not just a value:

| Area | Evidence required before marking verified |
|---|---|
| Kernel source behavior | Comparison to the closest available vendor source and disassembly of the verified stock ELF |
| Kernel config | `kernel.config` extracted from this exact firmware/build |
| Userspace ABI | Compiler target, API level, ELF class/machine, dynamic dependencies, and Android linker compatibility |
| Vendor kernel layout | Evidence specific to this build; a generic upstream 4.19 definition is not proof of a vendor layout |
| Memory map | Shipped-binary evidence and clearly distinguished runtime-only unknowns |
| Build reproducibility | One explicit project/profile selection and a recorded compiler/NDK version |

## 6. Stop conditions

Do not treat the Samsung profile as ready if any required value is inherited from the K40 profile without evidence; if the build silently selects a different `PROJECT`; if a build report confuses a source rebuild with the shipped ELF; or if ELF symbols are treated as defined when marked `UND`.

## 7. Current outcome

The firmware provenance is verified. The Samsung profile itself remains **documentation-only and not build-ready**. No exploit-specific target constants are supplied in this scaffold.