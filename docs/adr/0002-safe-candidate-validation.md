# ADR-0002: Safe Candidate Validation

## Status

Accepted

## Context

候选 `0xffffffc02cd1ea00` 曾通过 5/5 哈希一致性，但进入主链后触发
内核 panic。直接在设备端试错代价过高。

## Decision

任何新候选必须先通过以下两级验证：

1. PC 端满足更高命中数，例如 12/16 或更高。
2. 设备端只运行 `KS_MM_TEST=1`，确认候选被接受后立即退出。

只有两级都通过，才允许进入完整主链。

## Consequences

- 减少 panic 概率。
- 增加一次轻量设备测试。
- 若候选仍导致 panic，则必须回退到更高命中数或双桶验证。

## Divergence Class

Class A：安全约束，不可省略。
