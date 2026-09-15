# Vision

## Goal

在 Redmi K40 (alioth, SM8250, 8GB, Android 13, MIUI V816, kernel 4.19.157-perf)
上完成 GhostLock / CVE-2026-43499 提权链，最终获得稳定 root shell。

## Non-Goals

- 不解锁 bootloader。
- 不修改系统分区。
- 不追求跨机型通用性；当前只服务 alioth 4.19.157-perf。
- 不在文档中保存敏感设备标识或私有数据。

## Success Criteria

1. `adb shell id` 返回 `uid=0`。
2. KernelSnitch 能稳定给出正确 `mm_struct`，不再触发内核 panic。
3. 全流程可在一次会话内复现，失败时能安全退出或重启。
4. 每个里程碑都有对应 Spec、测试计划和状态记录。

## Constraints

- 设备保持 BL 锁定。
- 任何可能 panic 的步骤必须先有离线验证或可回退方案。
- 所有跨 Agent 协作信息必须落在 `docs/`，不依赖聊天上下文。
