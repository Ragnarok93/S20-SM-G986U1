# Kernel-page verifiers

Run these commands from the repository root. Each script finds the repository
from its own path; the record is `docs/kernel-page/` and the capture directory is
the tracked `docs/kernel-page/logs/`.

External inputs:

- `--fw DIR` (`verify_symbol.py`): `kernel.bin`, `boot.img.lz4`, `boot.img`,
  `vmlinux.elf` and `kernel.config` for G986U1UESCHXL1 / 4.19.113-27095354.
- `--ksrc DIR` (`verify_symbol.py`, `verify_timeline.py`): the matching kernel
  source tree.

Toolchain: `lz4`, `nm` (from `binutils`), and `aarch64-linux-gnu-objdump` (from
`binutils-aarch64-linux-gnu`) must be on `PATH`.

```sh
python3 docs/kernel-page/verify/verify_symbol.py --fw /path/to/firmware-check --ksrc /path/to/s20_kernel_chxl1
python3 docs/kernel-page/verify/verify_boundary.py
python3 docs/kernel-page/verify/verify_timeline.py --ksrc /path/to/s20_kernel_chxl1
python3 docs/kernel-page/verify/verify_lifecycle.py
```

Expected output: `problems: 0`; `boundary ok: 20 citations, 16 logs, 15
panic-boot starts, 7 tables`; `problems: 0`; `bad citations: 0`.

## CI inputs

`inputs.sha256` pins the five firmware files and the two kernel source files the
verifiers read. The source tree is not a git checkout, so those file hashes are
the revision pin.

The workflow installs the three tools above and checks they resolve before
fetching the archives. It downloads the `fw/` and `ksrc/` archives from the URLs
in repository secrets `KS_FIRMWARE_URL` and `KS_KSRC_URL` (optionally authorized
by `KS_INPUTS_TOKEN`), unpacks them, and runs `sha256sum -c inputs.sha256`
before the four commands. Pack the archives with:

```sh
tar -C /path/to/firmware-check -cf firmware.tar kernel.bin boot.img.lz4 boot.img vmlinux.elf kernel.config
tar -C /path/to/s20_kernel_chxl1 -cf ksrc.tar kernel/locking/rtmutex.c kernel/locking/rtmutex_common.h
```

Upload the archives to a private release or object store and set the two
secrets to their URLs. Fork pull requests do not receive these secrets and
fail closed.
