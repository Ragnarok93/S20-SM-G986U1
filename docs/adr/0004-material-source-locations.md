# ADR-0004: Material Source Locations

Status: accepted (2026-09-15)

## Context

Kernel-code citations (fork.c, slub.c, rtmutex.c, select.c, rbtree.h) were
initially marked "checked-in kernelsrc" while `kernelsrc/` contained only
empty/broken clones. The audit could not verify them in-repo, and every new
agent had to ask the user where the source lives.

## Decision

1. All external reference material lives under `materials/` (git-ignored;
   junctions to real trees elsewhere on disk are allowed).
2. When the user provides a source, the receiving agent places it under
   `materials/`, then records provenance (repo URL, revision, device fit) in
   this ADR before using it. This file is the single registry.
3. Kernel-code claims are verified against the registered tree only.

## Layout And Intake

Use stable material paths so agents do not need to rediscover locations:

| Kind | Path |
|---|---|
| Kernel tree | `materials/kernelsrc/<vendor>-<soc>-<android>-<variant>` |
| ROM / boot images | `materials/rom/<device>-<build>` |
| Rootfs / userspace dump | `materials/rootfs/<device>-<build>` |
| Other external artifact | `materials/external/<name>` |

Intake flow: check this registry first; if absent, state the required material
kind and proposed target path to the user; after delivery, place it at the
proposed path, verify the tree/artifact is usable, and append a registry row.

## Registry

| Material | Path | Provenance | Fit |
|---|---|---|---|
| alioth kernel source | `materials/kernelsrc/sm8250-a13-stable` (junction to `D:\project\my_py_project\mozi9-kernel-proxy`) | https://github.com/combel123/mozi9-kernel-xiaomi-sm8250-mod (full tree, Makefile `4.19.157`) | Target SoC SM8250, target kernel 4.19.157-perf; correct default source for kernel-code citation checks |
| UtsavBalar sm8250 A13 | NOT registered: `kernelsrc/sm8250-a13-stable` clone is broken on Windows (`aux.c`/`aux.h` reserved names abort checkout; partial blob:none clone) | https://github.com/UtsavBalar1231/kernel_xiaomi_sm8250 | superseded by the mozi9 tree until a working checkout exists |

`kernelsrc/*` directories are broken leftovers and must not be cited.
