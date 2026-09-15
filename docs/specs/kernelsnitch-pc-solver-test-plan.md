# Test Plan: KernelSnitch PC Solver

## Scope

验证 PC 求解器与设备端 `futex_hash()` 字序一致，并保证候选输出可用于安全验证。

## Test Cases

| ID | Type | Input | Expected |
|---|---|---|---|
| TC-01 | unit | 已知 `addr`, `mm`, `hashsize=0x800` | PC hash 与设备端算法一致 |
| TC-02 | unit | 构造 4 个同桶地址 | solver 至少找到一个 4/4 命中 |
| TC-03 | integration | `ks_collisions_run5_coll16.txt`, 64B align | 输出 12/16 或更高命中 |
| TC-04 | integration | `ks_collisions_run5_coll16.txt`, 0x388/order-3 | 输出 12/16 或更高命中 |
| TC-05 | device | candidate + `KS_MM_TEST=1` | 设备接受候选且不 panic |
| TC-06 | negative | 已知坏候选 `0xffffffc02cd1ea00` | 禁止复用，测试跳过 |
| TC-07 | safety | candidate + `KS_MM_TEST=1` | 无 panic、无 bootloop |
| TC-08 | integration | dual-pile candidate sets | 交集显著缩小 |
| TC-09 | e2e | full chain | `adb shell id` 返回 `uid=0` |

## Commands

```pwsh
build\pc_candidate_solver_all.exe build\ks_collisions_run5_coll16.txt `
  0xffffffc000000000 0xffffffffffffffff 0x40 2047 12

build\pc_mm_slab_solver.exe build\ks_collisions_run5_coll16.txt `
  0xffffffc000000000 0xffffffffffffffff 0x388 3 2047 12
```

## Pass Rule

Milestone 1 完成需要 TC-01、TC-02 通过，并且 TC-03 或 TC-04 至少有一个命中。
