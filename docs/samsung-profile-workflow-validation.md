# Samsung profile workflow validation

The Samsung target's `device-profile.json` is an evidence-backed compatibility
record, not the upstream generator's executable profile schema. Validate the
repository record with:

```bash
python3 tools/validate_samsung_device_profile.py
python3 -m unittest discover -s tests -p 'test_samsung_device_profile_validation.py' -v
```

This check validates device identity, expected kernel/config metadata, boot
layout, and SHA-256 field formatting. It does not recompute artifact hashes or
claim that the source-built kernel matches the shipped firmware.

The upstream generator's `--profile` input expects additional runtime/layout
fields such as `p0_phys_offset` and `p0_kernel_phys_load`. Those fields are
not safely inferred from the identity record or from the successful boot-parser
tests. The current Samsung profile therefore remains `identity-check-only`;
this validator must not be treated as successful end-to-end target generation,
and it intentionally does not emit `target.h`.
