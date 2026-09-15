# ADR-0001: PC Futex Hash Word Order

## Status

Accepted

## Context

设备端 `futex_hash()` 基于 `futex_key_t.private`，其中
`address` 位于偏移 0，`mm` 位于偏移 8。  
`jhash2()` 按内存顺序读取 4 个 `uint32_t`。

## Decision

PC 端求解器必须使用以下顺序：

```c
w[0] = address_low;
w[1] = address_high;
w[2] = mm_low;
w[3] = mm_high;
```

并且 `initval` 使用 `addr & 0xfff`。

## Consequences

- 所有旧 PC 求解器结果作废。
- 必须用 `ks_collisions_run5_coll16.txt` 重新搜索。
- 设备端结果仍然是最终权威；PC 只用于离线预筛。

## Divergence Class

Class A：字序错误会导致完全错误的候选空间，必须硬修复。
