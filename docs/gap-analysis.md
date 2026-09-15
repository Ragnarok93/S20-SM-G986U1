# Gap Analysis

## Current State

- 目标内核和 alioth 专属偏移已确认。
- KernelSnitch 能采集 16 个碰撞和约 134 个候选地址。
- 设备端 `futex_hash()` 的字序已确认：`address` 在前，`mm` 在后。
- PC 端求解器此前使用相反字序，导致长期找不到精确解，已在
  commit `9fceb70` 修正。
- 候选 `0xffffffc02cd1ea00` 曾通过 5/5 哈希一致但导致内核 panic，
  已标记为不可复用。

## Gaps

| Gap | Impact | Owner |
|---|---|---|
| 修正字序后的全域扫描尚未完成 | 无法确认真实 `mm_struct` | Engineer |
| 缺少离线单测和固定测试向量 | 无法快速回归求解器 | QA |
| 单一碰撞桶仍可能被 timing 误报污染 | 可能再次 panic | Architect |
| 缺少双桶交叉验证机制 | 候选集无法进一步收敛 | Architect |
| 无 CI 状态文件 | 里程碑完成状态不直观 | QA |

## Risks

1. 错误 `mm_struct` 进入主链会导致内核 panic。
2. 全域 8B 扫描耗时长，需要控制搜索空间。
3. timing 噪声可能使真实碰撞被淹没在候选集中。

## Next Decision

优先完成修正后的离线求解器验证，再决定是否实现双桶交叉验证。
