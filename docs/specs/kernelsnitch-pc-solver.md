# Spec: KernelSnitch PC Solver

## Schema

输入文件为 `ks_collisions*.txt`，关键行：

```text
boot_id=<uuid>
futex_hashsize=0x800
collisions=<n>
futex_addr=0x<16hex>
```

CLI:

```text
pc_candidate_solver <collisions.txt> <start> <end> <step> <mask> [min_hits]
pc_mm_slab_solver <collisions.txt> <start> <end> <obj_size> <order> <mask> [min_hits]
```

## Types

```c
typedef struct {
  uint64_t addr;
} CandidateAddress;

typedef struct {
  uint64_t mm;
  uint32_t bucket;
  size_t hits;
  size_t total;
} SolverHit;
```

## Errors

| Code | Condition |
|---|---|
| 2 | CLI 参数不足 |
| 1 | 输入文件不可读或候选数小于 2 |
| 3 | 扫描完成但没有命中 |

## Divergences

| Item | Reference | This Project | Class |
|---|---|---|---|
| hash word order | `address` before `mm` | same | A |
| search domain | full linear map | PC prefilter only | B |
| final authority | device brute force | PC result is advisory | B |

## Invariants

1. PC 端哈希字序必须与 ADR-0001 一致。
2. 输出候选必须是 64B 对齐或符合 slab 模型。
3. 输出不得直接进入主链，必须经过 ADR-0002 验证。
