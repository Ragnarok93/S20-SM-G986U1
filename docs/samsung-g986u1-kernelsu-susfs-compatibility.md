# SM-G986U1 KernelSU-Next + SUSFS compatibility notes

**Assessment status: reference reviewed; integration not yet claimed.**

Target is the Samsung Galaxy S20+ `SM-G986U1`, firmware `G986U1UESCHXL1`, Snapdragon 865, Linux `4.19.113-27095354`, Android 13 / One UI 5.1-era firmware. The source kernel checkout used during the work is `~/s20_kernel_chxl1`; the firmware provenance artifacts are under `~/firmware-check`.

## Reference findings

### Samsung S20-series 4.19.113 reference

[MickWu7/GalaxyS20_Series_KernelSU_Next_Susfs-x1q](https://github.com/MickWu7/GalaxyS20_Series_KernelSU_Next_Susfs-x1q) describes a Snapdragon 865 Galaxy S20/S20+/S20 Ultra port based on Android 13 / One UI 5.1, Linux 4.19.113, KernelSU-Next 12430 (1.0.5), and SUSFS 1.5.5. Its device naming identifies `y2q` as the S20+.

**Important constraint:** the reference explicitly says it disables Samsung RKP/KDP, DEFEX, PROCA, and CFP. That conflicts with this port's preservation requirement. Treat the repository as evidence about compatible kernel family and approximate component versions only; do not copy its security-configuration changes wholesale or mark the port compatible merely because its kernel release matches.

### KernelSU-Next non-GKI integration

The [official KernelSU-Next non-GKI integration guide](https://kernelsu-next.github.io/webpage/pages/how-to-integrate-for-non-gki.html) distinguishes a kprobe-based integration from manual hook integration. It recommends the kprobe path only when kprobes work reliably; a kernel boot failure after integration is a reason to investigate the hook path rather than assuming the configuration is sufficient.

The guide lists these settings for its kprobe path:
- `CONFIG_KPROBES=y`
- `CONFIG_KPROBE_EVENTS=y`
- `CONFIG_KSU_KPROBE_HOOKS=y`
- `CONFIG_KSU=y`

These are reference requirements, not a statement that the stock G986U1 config has them. The earlier attempted configuration query in the terminal failed due to a shell quoting/awk syntax error, so those values remain **unverified** until inspected successfully in `~/firmware-check/kernel.config`.

### SUSFS 4.19 source lineage

The current relevant upstream branch is [simonpunk/susfs4ksu `kernel-4.19`](https://gitlab.com/simonpunk/susfs4ksu/-/tree/kernel-4.19?ref_type=heads). Its branch listing identifies the 4.19 patch as SUSFS 1.5.5-era code. The project's [root README](https://gitlab.com/simonpunk/susfs4ksu) warns that the default `master` branch remains at 1.3.8 and that patches can differ by kernel version and even between kernels with the same version.

The `kernel-4.19` instructions describe manual/non-kprobe KSU hook integration for that SUSFS patch lineage and call out that patching against the expected KernelSU source can require adaptation. That means the exact KernelSU-Next commit used by this project must be pinned and compared against the SUSFS patch's expected tree before calling the pairing compatible. A patch that applies to original KernelSU is not automatically compatible with KernelSU-Next.

## Compatibility constraints

1. **Preserve Samsung security facilities by default.** Keep RKP/KDP, DEFEX, PROCA, CFP, SELinux, and related Samsung protections enabled unless a concrete source-level conflict is shown and explicitly assessed. The reference port's disabled-security configuration is not an acceptable default for this target.
2. **Separate the layers.** KernelSU-Next integration, SUSFS kernel changes, Kconfig/defconfig wiring, userspace SUSFS tooling, and any installer/package layer must be tracked independently.
3. **Pin upstream revisions.** Record the KernelSU-Next commit and the SUSFS `kernel-4.19` commit/tag that the patch series expects. Avoid mixing the SUSFS 1.3.8 default branch, the 1.5.5 4.19 patch line, and an unrelated KernelSU-Next revision without an explicit compatibility review.
4. **Verify integration rather than infer it.** The presence of Kconfig symbols alone does not prove the corresponding source hooks and SUSFS implementation are integrated. Likewise, a successful source patch application is not proof that the selected config enables the intended code.
5. **Do not mark complete prematurely.** SUSFS is not considered integrated until source changes, Kconfig dependencies, final config, successful build, and relevant runtime checks have all been verified. No SUSFS integration or device boot validation is claimed by this document.

## Next source-review sequence

1. Record the exact kernel-source baseline commit and any existing local modifications in `~/s20_kernel_chxl1`.
2. Record the chosen KernelSU-Next commit and inspect its non-GKI integration model for this 4.19 tree.
3. Review the SUSFS `kernel-4.19` patch and Kconfig against that exact KernelSU-Next revision; list patch rejects and semantic conflicts rather than silently adapting unrelated hunks.
4. Audit every change touching Samsung RKP/KDP, DEFEX, PROCA, CFP, SELinux, credential handling, and VFS/mount paths. The default disposition is to preserve existing protections.
5. Verify resulting Kconfig and defconfig wiring, then build and inspect the generated kernel/config artifacts. Record compile errors and unmet dependencies in a reproducible report.
6. Only after source/config/build validation should a separate runtime validation plan be considered. Source-level compatibility must not be reported as verified runtime compatibility.

## Sources

- [KernelSU-Next: Integrate for non-GKI devices](https://kernelsu-next.github.io/webpage/pages/how-to-integrate-for-non-gki.html)
- [SUSFS 4.19 branch](https://gitlab.com/simonpunk/susfs4ksu/-/tree/kernel-4.19?ref_type=heads)
- [SUSFS project README and compatibility warning](https://gitlab.com/simonpunk/susfs4ksu)
- [Galaxy S20-series KernelSU-Next + SUSFS reference](https://github.com/MickWu7/GalaxyS20_Series_KernelSU_Next_Susfs-x1q)
