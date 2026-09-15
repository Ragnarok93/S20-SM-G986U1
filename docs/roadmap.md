# Roadmap

## Milestone 0: Workflow Bootstrap

Owner: PM  
Status: completed  
Deliverables:

- `docs/vision.md`
- `docs/gap-analysis.md`
- `docs/roadmap.md`
- `docs/adr/0001-pc-futex-hash-word-order.md`
- `docs/specs/kernelsnitch-pc-solver.md`
- `docs/specs/kernelsnitch-pc-solver-test-plan.md`
- `docs/status.md`

Exit Criteria:

- 文件齐全。
- 每个 Gap 都能映射到至少一个任务。

## Milestone 1: PC Solver Correctness

Owner: Engineer  
Status: in-progress  
Depends On: Milestone 0  
Tasks:

1. 修正 PC 求解器哈希字序。
2. 增加最小哈希单测或固定向量测试。
3. 用 `ks_collisions_run5_coll16.txt` 重跑 64B 对齐扫描。
4. 若无解，再跑 0x388/order-3 slab 模型。

Exit Criteria:

- 测试计划中 TC-01 到 TC-04 全部通过。
- 至少得到一个候选 `mm_struct` 或明确证明该搜索空间无解。

## Milestone 2: Safe Device Validation

Owner: Engineer + QA  
Status: pending  
Depends On: Milestone 1  
Tasks:

1. 只使用 `KS_MM_TEST=1` 验证候选，不进入主链。
2. 记录设备是否接受候选。
3. 只有在候选稳定通过后，才允许进入主链。

Exit Criteria:

- TC-05 到 TC-07 通过。
- 未发生 panic。

## Milestone 3: Candidate Convergence

Owner: Architect + Engineer  
Status: pending  
Depends On: Milestone 2  
Tasks:

1. 设计双桶或对照 pile 交叉验证。
2. 修改设备端碰撞采集，导出两个桶的候选集。
3. 修改 PC 求解器支持交集搜索。

Exit Criteria:

- 候选集显著收敛。
- TC-08 通过。

## Milestone 4: Root Chain

Owner: Engineer  
Status: pending  
Depends On: Milestone 3  
Tasks:

1. 使用收敛后的 `mm_struct` 进入 FOPS / configfs / pipe physrw。
2. 验证 KASLR 和 root shell。
3. 记录失败点和修复。

Exit Criteria:

- TC-09 通过，`adb shell id` 返回 `uid=0`。
