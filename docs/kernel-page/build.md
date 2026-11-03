# Build and artifact provenance

## Primary payload (verified)

| item | value |
| --- | --- |
| command | `PROJECT=samsung-g986u1-4.19.113 bash build.sh` |
| toolchain | NDK r27c at `~/ndk/android-ndk-r27c` (or `ANDROID_NDK_HOME` / `ANDROID_NDK_ROOT`), API 33 |
| compiler | `aarch64-linux-android33-clang` |
| output | `build/samsung-g986u1-4.19.113/bin/preload.so`, copied to `prebuilt/preload.so` |
| warnings | none (`-Wall -Wextra`) |
| current sha256 | printed by the build; the committed copy is identical to it |

The NDK is resolved by `Makefile` in this order: `ANDROID_NDK_HOME`,
`ANDROID_NDK_ROOT`, then three known local locations. Override with
`NDK_ROOT=…`. Nothing is pinned to one machine.

## Build contract (measured)

`build.sh` is the only supported entry point, so its exit status has to mean
what it says. Measured on this tree:

| check | method | observed |
| --- | --- | --- |
| compiler failure is not swallowed | append `#error` to `src/kernelsnitch/utils.h`, run `build.sh` | `make: *** [Makefile:238: build/…/preload.so] Error 1`, exit **2**; source restored, next run exit 0 |
| a missing artifact is a hard failure | both products are copied with `cp` under `set -euo pipefail` | a missing `build/$PROJECT/bin/preload.so` or `build/embed/su_daemon_aarch64_pie` exits nonzero |
| success is artifact-backed | stub-`make` runs in `tests/test_build_script_contract.py` | exit 0 only when both artifacts exist and are copied to `prebuilt/` |

An earlier note in this project claimed `build.sh` exits 0 after a compile
error. That was an artefact of the wrapper that invoked it (the reported status
came from a `grep` in the same pipeline), not the script. The injected-failure
run above is the measurement, and `tests/test_build_script_contract.py` pins
the contract without needing an NDK or a device.

## Secondary targets (unverified)

`s2`, `s3`, `rootchain` and `closed_loop` were **not built** in this
environment and are not verified:

- their recipes run a clang wrapper through `cmd /c`, i.e. they are
  Windows-shell targets, and the wrapper is not part of the repository;
- `build_s3.rsp` **does not exist** in the tree, so the s3 target cannot link
  as written;
- `build_s2.rsp` and `build_rootchain.rsp` list sources explicitly and were
  updated to include `src/kernel_page/{gate,run_state}.c`, but that edit is
  itself unverified.

The Makefile no longer embeds a developer's local NDK path. `S2_CC` defaults to
a wrapper derived from `S2_NDK_ROOT` (which falls back to the standard NDK
environment variables), and every secondary recipe now fails with an explicit
message when that wrapper is absent, instead of a confusing shell error.

## `prebuilt/su_daemon_aarch64_pie`

**Generated, reproducible, and embedded.**

| item | value |
| --- | --- |
| source | `src/su_daemon.c` |
| rule | `Makefile`: `$(EMBED_SU): src/su_daemon.c` → `build/embed/su_daemon_aarch64_pie` |
| embedding | `src/su_blob.S` line 7: `.incbin "build/embed/su_daemon_aarch64_pie"` |
| packaging | `build.sh` copies it to `prebuilt/su_daemon_aarch64_pie` |
| sha256 | `47a834c9451a8805e51566c9db1689b491ce297f71087c7a5a540dd255005d27` |

Verified in this tree: deleting the build product and rebuilding it from source
produces a byte-identical file (same sha256). It is tracked because
`build.sh` packages it and `su_blob.S` consumes it; it is not a vendor blob.

To confirm at any time:

```sh
rm -f build/embed/su_daemon_aarch64_pie
make build/embed/su_daemon_aarch64_pie
sha256sum build/embed/su_daemon_aarch64_pie prebuilt/su_daemon_aarch64_pie
```
