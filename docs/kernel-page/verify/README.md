# Kernel-page verifiers

Run these commands from the repository root. Each script finds the repository
from its own path; the record is `docs/kernel-page/` and the capture directory is
the tracked `docs/kernel-page/logs/`.

External inputs:

- `--fw DIR` (`verify_symbol.py`): `kernel.bin`, `boot.img.lz4`, `boot.img`,
  `vmlinux.elf` and `kernel.config` for G986U1UESCHXL1 / 4.19.113-27095354.
- `--ksrc DIR` (`verify_symbol.py`, `verify_timeline.py`): the matching kernel
  source tree.

```sh
python3 docs/kernel-page/verify/verify_symbol.py --fw /path/to/firmware-check --ksrc /path/to/s20_kernel_chxl1
python3 docs/kernel-page/verify/verify_boundary.py
python3 docs/kernel-page/verify/verify_timeline.py --ksrc /path/to/s20_kernel_chxl1
python3 docs/kernel-page/verify/verify_lifecycle.py
```

Expected output: `problems: 0`; `boundary ok: 20 citations, 16 logs, 15
panic-boot starts, 7 tables`; `problems: 0`; `bad citations: 0`.
