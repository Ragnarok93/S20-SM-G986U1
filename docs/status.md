# Status

| Milestone | Status | Owner | Notes |
|---|---|---|---|
| 0 Workflow Bootstrap | completed | PM | docs scaffold created |
| 1 按 ADR-0001 拆分 collision/candidate 数据语义 + second-pile 交叉验证 | in-progress | Engineer | 双桶实现和离线自检已完成；QA 复跑 PASS（见 `docs/ci-status.md`）。等待新设备导出。旧 run5 数据仍不可作为同桶集合 |
| 2 Safe Device Validation | blocked | Engineer + QA | 2026-09-15 dual-pile 上机未得到 accepted collisions：second-pile `FUTEX_WAIT_PRIVATE` 返回 `EINVAL`；无求解数据，暂不上第三轮 |
| 3 Candidate Convergence | blocked | Architect + Engineer | blocked by M2 |
| 4 Root Chain | blocked | Engineer | blocked by M3 |

## Latest Facts

- PC 哈希字序修正 commit: `9fceb70`。
- 禁止复用候选: `0xffffffc02cd1ea00`。
- 历史数据 `build/ks_collisions_run5_coll16.txt` 已被 ADR-0001 判定为混桶
  语义，不得继续当作 16 个同桶地址。
- 2026-09-15: 新导出只把 second-pile 交集写入 `collision_addr=`；
  `candidate_addr=` 仅作对照/负样本统计。PC 求解器只解析
  `collision_addr=`。离线自检见
  `build/dualpile_selfcheck_summary.txt`（align8/slab388 均 8/8 命中，
  4 个异桶负样本排除）。
- 新 preload: `build/alioth/bin/preload.so`，208856B，
  SHA256 `9BE36F9E8C0E54C221294DE5BBC2C8B21B956636B8531CE905F9BDD4B0486011`。

### 2026-09-15 M2 上机复核 (v14)

`preload.so` 已发布为 myroot `so/ghostlock_alioth_v14.so`
（manifest version 12，commit `8eaf663`），线上 so 哈希与本地一致。
设备轮清空 answer/cache 后加载 208856/208856 字节，页面终端确认
PID 26119。第一 pile 有 timing 候选，但 second pile 的
`FUTEX_WAIT_PRIVATE` 返回 `EINVAL`；`ks_collisions.txt` 未更新。
因此没有 accepted collisions，也没有可求解的 dual-pile 导出。
同一失败在页面自动重试的二次观测中复现。Firefox 已 force-stop，
设备保持在线，无 panic/reboot。详细命令、输出与截图见
`build/m2_device_20260915_1400_summary.md`。
当前不应再以“重启重试”消耗上机轮次，应先离线排查 second-pile
锚点地址/值前置条件。

### 2026-09-15 QA 复核 (离线)

QA 使用重建后的 selfcheck 生成器重新生成数据并复跑两个 dual-pile 求解器:
align8 与 slab388 均 `8/8` 命中; 4 个异桶负样本在两个求解器的 8GB 扫描中
均 `FOUND=0`。旧 `futex_addr=` 数据被求解器静默跳过 (无显式报错),
输入旧 run5 数据时解析结果为 0。`candidate_addr=` 仅参与统计,
不进入求解。`preload.so` 为 208856B, SHA256 仍为
`9BE36F9E8C0E54C221294DE5BBC2C8B21B956636B8531CE905F9BDD4B0486011`。
导出格式与 accepted 集合语义、新增 mmap/munmap 配对均复核通过。
详细矩阵见 `docs/ci-status.md`。本轮未触碰设备。
