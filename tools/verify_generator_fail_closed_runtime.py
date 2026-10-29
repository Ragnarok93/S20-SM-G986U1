#!/usr/bin/env python3
"""Synthetic runtime checks for the generator's fail-closed control flow.

Loads the patched generator entry points but replaces boot/config/kallsyms/BTF
analysis with in-memory stubs. It deliberately exercises only failure paths:
missing BTF, rejected BTF consumer layout, and rejected percpu entry_task
validation. It never reads firmware, computes device values, or writes target.h.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any

SCENARIOS = {
    "missing-btf": "synthetic missing BTF",
    "btf-layout": "BTF consumer layout validation failed; refusing to generate target.h",
    "percpu-layout": "percpu entry_task validation failed; refusing to generate target.h",
}


class CheckError(RuntimeError):
    pass


def import_source(path: Path, name: str) -> Any:
    if path.is_symlink() or not path.is_file():
        raise CheckError(f"required generator source missing or symlinked: {path}")
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise CheckError(f"cannot import generator source: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class FakeKallsyms:
    def one(self, _symbol: str) -> int:
        return 0x1000


def fake_boot() -> SimpleNamespace:
    return SimpleNamespace(
        kernel=b"synthetic kernel bytes used only by stubs",
        kernel_size=64,
        image_size=64,
        kernel_sha256="synthetic-only",
        text_offset=0x80000,
    )


def assert_no_output(path: Path, label: str) -> None:
    if path.exists():
        raise CheckError(f"{label}: output was created despite validation failure: {path}")


def check_wrapper(root: Path, scenario: str, output: Path) -> None:
    wrapper = import_source(root / "tools" / "generate_target.py", f"failclosed_wrapper_{scenario}")
    boot = fake_boot()
    btf = SimpleNamespace()

    def percpu_validator(*_args: Any) -> None:
        if scenario == "percpu-layout":
            raise RuntimeError("synthetic invalid percpu layout")

    btf.validate_percpu_entry_task = percpu_validator

    def locate_btf(*_args: Any) -> Any:
        if scenario == "missing-btf":
            raise RuntimeError("synthetic missing BTF")
        return btf

    def validate_btf(*_args: Any) -> None:
        if scenario == "btf-layout":
            raise RuntimeError("synthetic invalid consumer layout")

    fake_gt2 = SimpleNamespace(
        extract_boot_kernel=lambda _path: boot,
        extract_ikconfig=lambda _kernel: (b"", {}, None),
        complete_profile=lambda profile, _config, _size: dict(profile),
        recover_kallsyms=lambda *_args: FakeKallsyms(),
        locate_btf=locate_btf,
        validate_btf_consumer_layout=validate_btf,
        derive_pselect_layout=lambda *_args: (_ for _ in ()).throw(
            AssertionError("control flow continued beyond injected validation failure")
        ),
    )
    wrapper.load_module = lambda _name: fake_gt2
    fallback_calls: list[bool] = []

    def fallback_sentinel(*_args: Any) -> int:
        fallback_calls.append(True)
        output.write_text("synthetic fallback sentinel\n", encoding="utf-8")
        return 0

    wrapper.fallback_generate_without_btf = fallback_sentinel
    args = SimpleNamespace(
        project="synthetic-test",
        boot=Path("synthetic-boot.img"),
        llvm_objdump="unused-in-test",
        mm_struct_sz=None,
        ksnitch_collisions=4,
    )
    try:
        wrapper.complete_generate(args, {}, output)
    except wrapper.ToolError as exc:
        if SCENARIOS[scenario] not in str(exc):
            raise CheckError(
                f"wrapper/{scenario}: wrong refusal message: {exc}"
            ) from exc
    else:
        raise CheckError(f"wrapper/{scenario}: expected a fail-closed ToolError")

    if scenario == "missing-btf" and fallback_calls:
        raise CheckError("wrapper/missing-btf: degraded template fallback was called")
    assert_no_output(output, f"wrapper/{scenario}")


def check_vendor(root: Path, vendor_name: str, scenario: str, output: Path) -> None:
    path = root / "tools" / "vendor" / vendor_name
    module = import_source(path, f"failclosed_{vendor_name.replace('.', '_')}_{scenario}")
    boot_path = output.parent / "synthetic-boot.img"
    profile_path = output.parent / "synthetic-profile.json"
    xbl_path = output.parent / "synthetic-xbl-config.bin"
    boot_path.write_bytes(b"synthetic; extract_boot_kernel is stubbed")
    profile_path.write_text("{}\n", encoding="utf-8")
    xbl_path.write_bytes(b"synthetic; recover_xbl_profile is stubbed")
    boot = fake_boot()
    btf = SimpleNamespace()

    def percpu_validator(*_args: Any) -> None:
        if scenario == "percpu-layout":
            raise RuntimeError("synthetic invalid percpu layout")

    btf.validate_percpu_entry_task = percpu_validator

    def locate_btf(*_args: Any) -> Any:
        if scenario == "missing-btf":
            raise module.GenerationError("synthetic missing BTF")
        return btf

    def validate_btf(*_args: Any) -> None:
        if scenario == "btf-layout":
            raise RuntimeError("synthetic invalid consumer layout")

    module.extract_boot_kernel = lambda _path: boot
    module.extract_ikconfig = lambda _kernel: (b"", {}, None)
    module.recover_kallsyms = lambda *_args: FakeKallsyms()
    module.locate_btf = locate_btf
    module.validate_btf_consumer_layout = validate_btf
    module.complete_profile = lambda profile, _config, _size: dict(profile)
    module.load_profile = lambda _path: {}
    module.derive_pselect_layout = lambda *_args: module.fail(
        "TEST FAILURE: control flow continued past injected validation failure"
    )

    argv = ["--boot", str(boot_path)]
    if vendor_name == "generate_target.py":
        module.recover_xbl_profile = lambda *_args: SimpleNamespace(
            phys_offset=0x10000000,
            kernel_phys_load=0x10100000,
            kernel_region_size=0x200000,
            sha256="synthetic-only",
            dtb_offsets=(),
        )
        argv += ["--xbl-config", str(xbl_path)]
    else:
        argv += ["--profile", str(profile_path)]
    argv += ["-o", str(output)]

    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
        rc = module.main(argv)
    report = stderr.getvalue()
    if rc != 2:
        raise CheckError(f"{vendor_name}/{scenario}: expected exit code 2, got {rc}")
    if SCENARIOS[scenario] not in report:
        raise CheckError(
            f"{vendor_name}/{scenario}: expected fail-closed message not found; "
            f"stderr was: {report!r}"
        )
    assert_no_output(output, f"{vendor_name}/{scenario}")


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--generator-root",
        type=Path,
        default=Path("/tmp/ghostlock-profile-gen"),
        help="patched external generator checkout",
    )
    args = parser.parse_args()
    root = args.generator_root.resolve()
    if root.is_symlink() or not root.is_dir():
        print(f"ERROR: generator root must be a real directory: {root}", file=sys.stderr)
        return 1

    checks = 0
    try:
        with tempfile.TemporaryDirectory(prefix="generator-fail-closed-test-") as tmp:
            base = Path(tmp)
            for scenario in SCENARIOS:
                check_wrapper(root, scenario, base / f"wrapper-{scenario}.h")
                print(f"PASS wrapper/{scenario}: refused, no target output")
                checks += 1
            for vendor_name in ("generate_target.py", "generate_target_2.py"):
                for scenario in SCENARIOS:
                    check_vendor(root, vendor_name, scenario, base / f"{vendor_name}-{scenario}.h")
                    print(f"PASS {vendor_name}/{scenario}: refused, no target output")
                    checks += 1
        print(f"RESULT=PASS synthetic_fail_closed_checks={checks} target_outputs=0 firmware_access=0")
        return 0
    except (OSError, CheckError, AssertionError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
