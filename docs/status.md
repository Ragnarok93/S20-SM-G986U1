# Status

| Milestone | Status | Owner | Notes |
|---|---|---|---|
| 0 Workflow Bootstrap | completed | PM | docs scaffold created |
| 1 按 ADR-0001 拆分 collision/candidate 数据语义 + second-pile 交叉验证 | in-progress | Engineer | 双桶实现和离线自检已完成；QA 复跑 PASS（见 `docs/ci-status.md`）。等待新设备导出。旧 run5 数据仍不可作为同桶集合 |
| 2 Safe Device Validation | blocked | Engineer + QA | 2026-09-15 dual-pile 上机未得到 accepted collisions；已完成离线 EINVAL 审计并重建 v15 修复产物，尚未部署或验证 |
| 3 Candidate Convergence | blocked | Architect + Engineer | blocked by M2 |
| 4 Root Chain | blocked | Engineer | blocked by M3 |
| 5 Slide Route Revival (parallel) | in-progress | Architect + Engineer | ADR-0003: 重开 slide pselect 路线（H1a=组件全组合，H1b=内触发移植）；与 M2 并行，先到先得 |

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
- 2026-09-15 second-pile EINVAL 已定位为 v14 把 pile 循环计数当作
  futex 字节偏移。离线修正并用 NDK r26b 重建 preload:
  `build/alioth/bin/preload.so`，209640B，
  SHA256 `EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214`。
  该产物未发布、未部署，也未包含并行 M5 改动。

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

### 2026-09-15 second-pile EINVAL 离线审计 (v15)

4.19 证据链：

- `futex_wait_setup()` 读取 `uaddr` 后，值不匹配只返回
  `-EWOULDBLOCK` (`build/linux-4.19-futex.c:2584-2603`)。
- `do_futex()` 在 `FUTEX_WAIT` 分支把 `val3` 强制为
  `FUTEX_BITSET_MATCH_ANY` (`build/linux-4.19-futex.c:3523-3528`)。
- K40 `kernel.elf` 反汇编同样确认：`get_futex_key` 的
  4 字节非对齐路径加载 `-22` (`0xffffff8008198454`)；
  `futex_wait` 的 zero-bitset 路径加载 `-22`
  (`0xffffff8008194c9c-0xffffff8008194cfc`)；`futex_wait_setup`
  的值不匹配路径加载 `-11` (`0xffffff8008197c9c`)。

代码复核：

- `__measure()` 只调用 `FUTEX_WAKE_PRIVATE`，不写锚点字，也不留 waiter
  (`src/kernelsnitch/kernelsnitch.h:210-228`)。
- 候选 id 公式是 page-offset `|` 低 12 位项，两者均为 8 的倍数，
  因此候选锚点均为 8 字节对齐，当然也满足内核 4 字节要求。
- 候选 id 不使用 `futexes[0]` 基线槽位；`inc_futex[ID]` 与大映射
  `futexes[]` 是不同用户地址。
- `inc_arg->uaddr` 指向 `ks->futexes[]` 映射，waiter 自持有
  `inc_arg`，唤醒后自行 `free()`；pile1/pile2 的映射生命周期都覆盖
  waiter 生命周期。

根因实锤：v14 二进制把 phase-2 的循环计数 `i` 存入
`candidate1_idx`（`0x1a16c-0x1a188`），随后把
`candidate1_idx[0]` 当作 `ks->futexes[]` 的字节偏移
（`0x1a25c-0x1a284`）。设备日志里首个候选的 futex 偏移低 12 位是
`0x538`，对应的起始循环索引 `i=167`；v14 的 pile2 锚点实际是
`base+167`，`167 % 4 != 0`，正好进入 `get_futex_key()` 的
非对齐 `-EINVAL` 路径。这不是值不匹配，也不是 waiter 生命周期问题。

v15 的最小修复是在记录候选时保存字节偏移 `id`，不是循环计数
`i`（`src/kernelsnitch/kernelsnitch.h:450-452`），并在创建 pile2
waiter 前校验/记录 anchor id、范围、4 字节对齐和当前值
（`:475-504`）。修正索引后，候选锚点天然 8 字节对齐；pile2 恢复为
与 pile1 相同的 raw `FUTEX_WAIT_PRIVATE, 0`（`:513-515`），pile1
调用路径不变（`:189-192`）。

### 2026-09-15 M5 立项 (ADR-0003, slide 路线重开, 并行轨道)

- 新上游仓库已 clone: `D:\project\my_py_project\CVE-2026-43499-root-KernelSU`
  (woshimaniubi8/CVE-2026-43499-root-KernelSU)。
- 工程侧 diff 结论: 该仓库无 4.19 target；rodin 的 slide 布局把 waiter
  写进 in/out/ex 输入区，在 alioth 4.19 永远打不中（waiter 在
  res_in/res_out 输出区，v12 已实锤）；其 loggers/bootid 泄漏树节点在
  4.19 会自旋（R1-R4 已证）。rodin 路线不能直接移植。
- 可取之处: 内触发 pselect 路线（绕 bitmap 重写窗口）作为 H1b 备选；
  ksud late-load 集成用于 root 后阶段（MIUI 无 DEFEX，q4q 死路在此
  可能可行）；target.h 生成器可用于离线交叉验证 alioth 偏移。
- 放弃后新增事实: 4.19 线性映射无随机化，delta 恒为 0 —— slide 路线
  当时的校准阻塞已自然消解。H1a = v12 位图编码 + 终止形态 rb 节点 +
  nice=1 + delta=0 内置 + KernelSnitch KASLR，组件全部各自验证过，
  仅组合未上机。
- QA 验证计划: `docs/specs/slide-revival-test-plan.md`。

### 2026-09-15 M5 基线审计 (审计员报告)

- 关键事实: slide 泄漏路线当前编译期跳过（`main.c:286-292`），但 punch
  walk（`fops.c do_pselect_fake_lock_route`，sched_setattr 触发）仍是
  活跃劫持机制，且与 M2 路线共享 —— M5 与 M2 汇合于同一个 punch 阶段，
  互补不冲突，但产物必须分离。
- v15 改动仅限 `kernelsnitch.h`（候选索引修正 + pile2 anchor 校验/日志），
  与 slide.c 零重叠，已核实。
- delta 运行时默认已是 0（`util.c:196-208`）；H1A 上机时禁止创建
  `ks_delta_index.txt`。
- `KS_SLIDE_TEST` 走的是泄漏槽路线（4.19 自旋风险），禁止作为 H1A
  测试载体。
- 审计范围声明: 本线程仅审计；开发派发给独立 agent 会话，任务书
  `docs/specs/slide-h1a-dev-spec.md`；审计员复审通过前不排上机。

### 2026-09-15 v15 QA 复核 / M2 重试就绪

commit `d978b30` 的 EINVAL 修复已由 QA 离线复核通过：候选索引为 futex 字节
偏移，pile2 锚点做范围/4 字节对齐/值校验，且 pile2 与 pile1 使用相同的
`FUTEX_WAIT_PRIVATE, 0`。selfcheck 正向两个 solver 均 `8/8 FOUND=1`，4 个
异桶负样本在 8GB 扫描中均 `HITS=0/4 FOUND=0`。mmap/munmap 配对与锚点归零
写入也复核通过。

`preload.so` 为 209640 字节，SHA256
`EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214`。
工作树中的 M5 slide 改动未进入该 M2 产物的可观察符号/字符串；v15 仍为
本地未发布状态。

结论：M2 重试的离线前置条件已全部就绪，仅剩安全上机轮次排期；本轮 QA
未触碰设备。

### 2026-09-15 M2 v15 device round and v16 offline rebuild

v15 was published to myroot as `so/ghostlock_alioth_v15.so` with manifest
version 13, commit `9c987ba`, and the online cache-busted so hash matched the
local artifact (`209640B`,
`EF97DF4EA9BAD76C350B12E0C493A49BC350B7CE6310F4B6200131CE41430214`).
Device `8c08f79` stayed online throughout.

The important M2 precondition passed on device: pile2 ran normally and logged

```text
pile2 anchor id=6574632 addr=0000006f5e445228 align=0 value=0
```

It then reached `found 16 accepted collisions (pile1=128 pile2=128)`.
However, `ks_collisions.txt` remained 0 bytes and the process terminated with
SIGSEGV in `ks_export_collisions`. Evidence:
`build/ks_log_m2_v15_run1.txt`,
`build/ks_collisions_m2_v15_run1.txt`, and
`build/m2_v15_run1_crashlog.txt`. A parallel H1A `id` process (PID 17004)
contaminated the same log and was stopped; M2 PID 31647 was left untouched.

Root cause of the export crash: the deployed v15 artifact was built from an
isolated HEAD tree, but the candidate-export fix in `src/util.c` was still
uncommitted in the working tree. The old binary used `candidate_idx[i]` as a
direct `t_before[]` index, while the current source correctly divides the byte
offset by `KS_PAGE_SIZE` first. v16 fixes this by rebuilding with the working
`src/util.c` fix.

Because the accepted-collision rows were never written, no solver was run and
there is no mm candidate to record. M2 stopped here: no answer backfill, no
punch/fops, and no additional device retry.

Offline v16 artifact (not deployed):

```text
build/alioth/bin/preload_v16.so
size=209688
sha256=E58CA62E1803D2E23280B8C3216AEF801D206DD7D149B045A0213BEF2BD7D749
```

### 2026-09-15 M5 H1A full-chain device round

Result: UNKNOWN.

Device `8c08f79` remained online. PID `24364` ran H1A `id` in the slide
configuration (`pselect`, mm answer `0xffffffc1b1cfb840`) and produced:

```text
preload starting pid=24364
collision threshold approx=9
pile2 anchor id=15796352 addr=0000007d6d110880 align=0 value=0
```

During the observation window it did not reach `F_SETPIPE_SZ accepted`,
`pipe physrw summary`, `uid_after=0`, or a final `rc=0`. PID `24364` was
still alive at the last check (`19:41:47 CST`). No cleanup, reboot, or
second full-chain attempt was performed.

Full log saved to:

```text
build/ks_log_fullchain_m5_20260915T114127Z.txt
```

### 2026-09-15 M5 H1A full-chain completion

Result: FAIL.

The earlier UNKNOWN round later continued normally through pile2 and the
answer-file path:

```text
found 16 accepted collisions (pile1=111 pile2=111)
KS_PC answer accepted mm=ffffffc1b1cfb840 (skipping device scan)
KS_DELTA delta=0 default
KSLAB_BASE_CANDIDATE mm=ffffffc1b1cfb840 base=ffffffc1b1cf8000 idx=0
main route requeue issued
waiter futex returned ret=-1 errno=110
```

PID `24364` then exited. It never reached `F_SETPIPE_SZ accepted`,
`pipe physrw summary`, or `uid_after=0`, so this round is a punch-stage
failure rather than a KernelSnitch or mm-answer failure.

Preserved evidence:

```text
build/ks_log_fullchain_m5_20260915T115200Z.txt
build/ks_collisions_fullchain_m5_20260915T115200Z.txt
```

### 2026-09-15 M5 fd-protect offline audit

Root cause candidate for the missing punch logs: `open_slide_selected_fds()`
reuses fds `0..319`; `g_exlog_fd` and the punch-watchdog parent pipe can live
below that boundary, so both are overwritten before punch completion.

Offline fix and audit:

- `g_exlog_fd` and both punch-watchdog parent write fds now relocate above
  `PSELECT_ROUTE_NFDS + 64` before the fd sweep.
- No change to the v12 pselect fd-bitmap encoding, consumer timing, CFI
  write path, or KernelSnitch collision semantics.
- New offline artifact:
  `build/alioth/bin/preload_slide_h1a.so`, `217456` bytes, SHA256
  `5551CA81683AD28DE7932FA9308C5AC75E9D71E50B3BF3EF832F9BED0949AD00`.
- Artifact string checks pass for `consumer punch enter`, `pselect returned`,
  `main route done`, and `H1A punch returned`.
- Device validation is still pending.
