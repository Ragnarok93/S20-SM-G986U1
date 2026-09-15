# M2 device round - 2026-09-15 14:00 (Asia/Shanghai)

## Device and deployment

- Specified adb path `tools-k40\platform-tools\adb.exe` was absent; the
  directory contained only `README.md`. Device presence was checked with
  `D:\software\commonTool\scrcpy-win64-v2.7\adb.exe` and the K40 adb key was
  already installed.
- Device: `8c08f79`, `product:alioth`, `model:M2012K11AC`, state `device`.
- Local artifact: `build/alioth/bin/preload.so`, 208856 B,
  SHA256 `9BE36F9E8C0E54C221294DE5BBC2C8B21B956636B8531CE905F9BDD4B0486011`.
- Published as `myroot/so/ghostlock_alioth_v14.so`, manifest version 11 -> 12,
  myroot commit `8eaf663`.
- Online manifest reached `version=12` and `so/ghostlock_alioth_v14.so`.
- Online so SHA256 matched the local artifact exactly.

## Device round

1. Cleared answer/cache by atomic tmp + sync + mv:

```text
cd /data/local/tmp
: > ks_mm_answer.tmp
: > ghostlock_ks_cache.tmp
chmod 666 ks_mm_answer.tmp ghostlock_ks_cache.tmp
sync
mv -f ks_mm_answer.tmp ks_mm_answer.txt
mv -f ghostlock_ks_cache.tmp ghostlock_ks_cache.txt
chmod 666 ks_mm_answer.txt ghostlock_ks_cache.txt
sync
ls -l ks_mm_answer.txt ghostlock_ks_cache.txt
```

Output: both files were `0` bytes and mode `666`.

2. Cleared Firefox state and opened `https://ccp-p.github.io/myroot/?v=14`.
   The page uploaded exactly `208856/208856` and spawned PID 26119.
   Evidence: `build/m2_ui_1341.png`.

3. The process printed timing candidates such as
   `0000006f4ba8b538 (before=13 after=5699)`.
   Evidence: `build/m2_ui_1344.png`.

4. No new `/data/local/tmp/ks_collisions.txt` was produced. The old file
   remained `1789377513 4580`. PID 26119 ended as zombie; PID 26120 exited.

5. The page terminal showed:

```text
SYSCHK(__futex(inc_arg->uaddr, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0)): Invalid argument
running su -c id...
/system/bin/sh: /data/local/tmp/su: inaccessible or not found
```

Evidence: `build/m2_ui_pidlog_1351.png`.

6. A second observation occurred automatically after a page reload. It also
   produced no new export. Firefox was force-stopped to prevent further rounds.
   Evidence: `build/m2_ui_copy_1354.png`,
   `build/m2_ui_after_stop_1400.png`.

## Conclusion

- No accepted collisions were exported.
- Therefore the dual-pile solvers were not run and no mm candidate exists.
- The failure appears deterministic rather than a rare timing miss: the first
  pile produced candidates, but the second pile's `FUTEX_WAIT_PRIVATE` returned
  `EINVAL`.
- A third immediate device round is not justified. Fix the second-pile anchor
  validation offline first, especially its address/value preconditions, before
  spending another restart.
