#!/usr/bin/env python3
"""Read-only compatibility check for the Samsung G986U1 kernel source tree."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "src/targets/samsung-g986u1-4.19.113/device-profile.json"


def load_profile(path: Path) -> dict[str, Any]:
    profile = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(profile, dict) or profile.get("schema_version") != 1:
        raise ValueError("unsupported or malformed profile (expected schema_version=1)")
    source = profile.get("kernel_source")
    if not isinstance(source, dict):
        raise ValueError("profile missing kernel_source object")
    for key in ("expected_version", "expected_patchlevel", "expected_sublevel"):
        if not isinstance(source.get(key), int):
            raise ValueError(f"profile kernel_source.{key} must be an integer")
    for key in ("required_paths", "device_config_tokens"):
        if not isinstance(source.get(key), list) or not all(isinstance(x, str) for x in source[key]):
            raise ValueError(f"profile kernel_source.{key} must be a string array")
    preferred = source.get("preferred_device_config")
    if not isinstance(preferred, str) or not preferred:
        raise ValueError("profile kernel_source.preferred_device_config must be a non-empty path")
    preferred_path = PurePosixPath(preferred)
    if preferred_path.is_absolute() or ".." in preferred_path.parts:
        raise ValueError("profile kernel_source.preferred_device_config must be a safe relative path")
    kernel_config = profile.get("kernel_config")
    selectors = kernel_config.get("target_selectors") if isinstance(kernel_config, dict) else None
    if not isinstance(selectors, dict) or not selectors:
        raise ValueError("profile kernel_config.target_selectors must be a non-empty object")
    for key, expected in selectors.items():
        if not isinstance(key, str) or not key.startswith("CONFIG_") or not isinstance(expected, str):
            raise ValueError("target_selectors must map CONFIG_* names to string values")
    return profile


def parse_kernel_version(makefile: Path) -> dict[str, str]:
    """Read the canonical VERSION/PATCHLEVEL/SUBLEVEL fields from Linux Makefile."""
    values: dict[str, str] = {}
    for line in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^\s*(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)\s*=\s*(.*?)\s*$", line)
        if match:
            values[match.group(1)] = match.group(2).split("#", 1)[0].strip()
    return values


def parse_config_values(path: Path) -> dict[str, str]:
    """Parse explicit Kconfig assignments and '# CONFIG_X is not set' entries."""
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip()
        else:
            match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line.strip())
            if match:
                values[match.group(1)] = "n"
    return values


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_metadata(root: Path) -> dict[str, str] | None:
    """Return git identity if available; never fetch or mutate the source checkout."""
    def git(*args: str) -> str | None:
        result = subprocess.run(
            ["git", "-C", str(root), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=10,
            check=False,
        )
        if result.returncode:
            return None
        return result.stdout.strip()

    commit = git("rev-parse", "HEAD")
    if not commit:
        return None
    top = git("rev-parse", "--show-toplevel") or ""
    remote = git("remote", "get-url", "origin") or ""
    branch = git("branch", "--show-current") or "<detached>"
    dirty = bool(git("status", "--porcelain"))
    return {
        "commit": commit,
        "top": top,
        "remote": remote or "<no origin remote>",
        "branch": branch,
        "dirty": "yes" if dirty else "no",
    }


def inspect_source_tree(root: Path, profile: dict[str, Any]) -> tuple[list[tuple[str, str, str]], dict[str, str] | None, list[str]]:
    """Return (checks, git metadata, device config candidates)."""
    source_profile = profile["kernel_source"]
    checks: list[tuple[str, str, str]] = []

    makefile = root / "Makefile"
    if not makefile.is_file():
        checks.append(("FAIL", "kernel Makefile", f"missing {makefile}"))
        return checks, git_metadata(root), []

    version = parse_kernel_version(makefile)
    expected = {
        "VERSION": str(source_profile["expected_version"]),
        "PATCHLEVEL": str(source_profile["expected_patchlevel"]),
        "SUBLEVEL": str(source_profile["expected_sublevel"]),
    }
    missing = [key for key in expected if key not in version]
    if missing:
        checks.append(("FAIL", "kernel version", "Makefile missing fields: " + ", ".join(missing)))
    else:
        found = ".".join(version[key] for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL"))
        wanted = ".".join(expected[key] for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL"))
        checks.append(("PASS" if found == wanted else "FAIL", "kernel version",
                       f"found={found}; expected={wanted}"))
    if "EXTRAVERSION" in version:
        checks.append(("INFO", "EXTRAVERSION", version["EXTRAVERSION"] or "<empty>"))

    missing_paths = [entry for entry in source_profile["required_paths"] if not (root / entry).exists()]
    checks.append(("PASS" if not missing_paths else "FAIL", "source tree layout",
                   "required kernel source paths present" if not missing_paths
                   else "missing: " + ", ".join(missing_paths)))

    config_dir = root / "arch/arm64/configs"
    candidates: list[Path] = []
    if config_dir.is_dir():
        tokens = tuple(token.lower() for token in source_profile["device_config_tokens"])
        candidates = sorted(
            path for path in config_dir.rglob("*")
            if path.is_file() and path.suffix in ("", ".config", ".defconfig")
            and any(token in path.name.lower() for token in tokens)
        )
    candidate_names = [str(path.relative_to(root)) for path in candidates]
    if candidate_names:
        checks.append(("INFO", "device config candidates", ", ".join(candidate_names)))
    else:
        checks.append(("WARN", "device config candidates",
                       "no config filename matched " + ", ".join(source_profile["device_config_tokens"])
                       + "; inspect the vendor tree's selected defconfig manually"))

    preferred_rel = str(PurePosixPath(source_profile["preferred_device_config"]))
    preferred_path = config_dir / preferred_rel
    if preferred_path.is_file():
        checks.append(("PASS", "preferred device config", preferred_rel))
        expected_selectors = profile["kernel_config"]["target_selectors"]
        preferred_values = parse_config_values(preferred_path)
        for symbol, wanted in sorted(expected_selectors.items()):
            found = preferred_values.get(symbol)
            if found == wanted:
                checks.append(("PASS", f"defconfig selector {symbol}", f"{preferred_rel}: {found}"))
            elif found is None:
                checks.append(("INFO", f"defconfig selector {symbol}",
                               f"{preferred_rel}: not explicitly set; confirm in the generated .config"))
            else:
                checks.append(("FAIL", f"defconfig selector {symbol}",
                               f"{preferred_rel}: found={found}; expected={wanted}"))
    else:
        checks.append(("FAIL", "preferred device config",
                       f"missing {preferred_rel} under arch/arm64/configs"))

    metadata = git_metadata(root)
    if metadata:
        checks.append(("PASS", "source revision", f"commit={metadata['commit']}"))
        checks.append(("INFO", "source origin", metadata["remote"]))
        checks.append(("INFO", "source branch", metadata["branch"]))
        checks.append(("WARN" if metadata["dirty"] == "yes" else "PASS", "source worktree",
                       "dirty; commit/diff must be recorded" if metadata["dirty"] == "yes" else "clean"))
    else:
        checks.append(("WARN", "source revision",
                       "no Git HEAD found; record the Samsung source archive/release and its checksum"))
    return checks, metadata, candidate_names


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE, help="machine-readable device profile")
    parser.add_argument("--source-root", type=Path, help="kernel source root (default comes from the profile)")
    args = parser.parse_args(argv)

    try:
        profile = load_profile(args.profile)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: could not load device profile: {exc}", file=sys.stderr)
        return 2

    source_root = args.source_root or Path(profile["kernel_source"]["default_path"]).expanduser()
    source_root = source_root.expanduser().resolve()
    if not source_root.is_dir():
        print(f"ERROR: kernel source root does not exist: {source_root}", file=sys.stderr)
        return 2

    checks, metadata, candidates = inspect_source_tree(source_root, profile)
    device = profile["device"]
    print(f"== Kernel source compatibility: {profile['profile_id']} ==")
    print(f"Target: {device['model']} / {device['codename']} / {device['firmware_build']}")
    print(f"Source root: {source_root}")
    for level, name, detail in checks:
        print(f"{level} {name}: {detail}")
    if candidates:
        print("\nCandidate config file digests:")
        for relative in candidates:
            try:
                digest = sha256(source_root / relative)
                print(f"INFO {relative} sha256={digest}")
            except OSError as exc:
                print(f"WARN {relative}: hash unavailable ({exc})")
    if metadata:
        print("\nGit provenance:")
        for key in ("top", "commit", "remote", "branch", "dirty"):
            print(f"  {key}={metadata[key]}")
    print("\nThis checks kernel-source family/version and records source provenance. It does not prove exact source-to-firmware identity or payload/runtime compatibility.")
    failures = sum(level == "FAIL" for level, _, _ in checks)
    print("RESULT: " + ("FAIL; source tree does not match expected kernel baseline" if failures
                        else "PASS; source baseline matches expected Linux version"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
