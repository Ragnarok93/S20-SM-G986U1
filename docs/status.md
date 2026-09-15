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

### 2026-09-15 H1A run4 hash-model fit

离线模型鉴别完成，未上机、未部署、未回填 answer。

- 数据集：`build/ks_collisions_h1a_run4_pass.txt`，
  16 个 accepted collision 地址。
- 工具：新增 `tools/pc_model_fit.c`，
  可执行文件 `build/pc_model_fit.exe`。
- 扫描域：`0xffffffc000000000-0xffffffc200000000`，`step=8`，
  `mask=0x7ff`，阈值 `>=14/16`。
- 模型枚举：`jhash2`/`jhash`/`jhash3` × 4/5/6 words ×
  `addr_mm`/`mm_addr` × `page`/`raw` ×
  `offset`/`0`/`0xdeadbeef`/固定 initval，共 144 个变体。
- 结果：16 地址全集、去掉 pile1 target 的 15 地址子集、
  仅双 pile 交集的 14 地址子集，全变体均为 0 命中。
- `0xffffffc1b1cfb840` 在上述模型下对 run4 地址不再形成集中桶；
  精确模型 `jhash2-4w-mm_addr-page-init_offset` 也是
  16 个独立桶。
- 反汇编复核修正了 ADR-0001：private futex key 实际是
  `mm@0/address@8`，不是 `address@0/mm@8`。

证据目录：`build/model_fit_20260915/`。
关键文件：`scan_summary_16.txt`、
`scan_summary_15_excl_target.txt`、
`scan_summary_14_intersection.txt`、
`historical_mm_16.tsv`、
`kernel_futex_wake.dis`、
`kernel_get_futex_key.dis`。

### 2026-09-15 H1A fd-protect device round

Result: FAIL (single attempt only).

The fd-protect artifact was rebuilt, rebooted, pushed, and hash-verified as
`5551CA81683AD28DE7932FA9308C5AC75E9D71E50B3BF3EF832F9BED0949AD00`.
`KS_H1A_TEST=1` was triggered once and stopped after the observation window;
the process exited with `rc=137` after being killed.

The log reached startup and the H1A route, then stayed in the collision
search. It did not record `H1A punch enter`, `H1A punch returned`, or
`H1A success calls=1 success=1`.

Preserved evidence:

```text
build/ks_log_h1a_fdprotect_run1.txt
```

### 2026-09-15 H1A diff probe (runs 5-9): chain never enters the fake page

NDK r26b (API 34) builds; device verified by sha256 each round.

- run5: discovered `KS_H1A_TEST` did not skip the mm phase; the run sat in the
  full 8 GB kernelsnitch scan. One incident: an earlier run left ~1400 `id`
  clone children; all were killed, device stayed online. Root causes fixed:
  `prepare_kernel_page` now preloads a plausible `ks_mm_answer.txt` value as
  `cached_leaked_mm` (after `detect_memory()`, which previously rejected the
  answer because `g_ram_bytes` was still 0), and the skipped-spray memfds are
  set to `-1` before the close loops (calloc zeros hit closed stdio fds and
  tripped `SYSCHK(close)` EBADF abort).
- run9 (`build/ks_log_h1a_leakprobe_run9_diff.txt`, pid 19375): end-to-end in
  ~8 s at load ~1.5. `KS_PC answer preloaded mm=ffffffc1b1cfb840`, page
  prepared at `ffffffc1b1cf8000` (lock ...9350, w0 ...a220, fops ...9000),
  `H1A punch enter -> first ret=0 -> done -> returned ret=86 calls=1
  success=1`, then `ks_leak_diff_dump()` compared every qword of all 4 SKB
  readbacks (32448 bytes each) against `skb_buf`: `diffs=0` on all four.

Conclusion (replaces the run3 "window/timing" hypothesis): the PI chain does
not write anywhere in the controlled page on alioth. `walk_touched=0` is not a
readback-window artifact. Leading hypothesis: the stale pselect rt_waiter is
not landing in the groomed page on this kernel (SLUB cache/order or
`SKB_DATA_DELTA` layout mismatch), so the walk dereferences real heap memory.
Do NOT run the full chain until the stale-waiter placement is verified offline
or with a safe probe (the full chain rebooted on this exact symptom).

Preserved evidence:

```text
build/ks_log_h1a_leakprobe_run5_diff_only.txt
build/ks_log_h1a_leakprobe_run6_preload_pc.txt
build/ks_log_h1a_leakprobe_run7_pc_fixed.txt
build/ks_log_h1a_leakprobe_run8_fdguard.txt
build/ks_log_h1a_leakprobe_run9_diff.txt
```

### 2026-09-15 fd-protect validation and full-chain reboot

Result: fd-protect PASS; full-chain FAIL (device reboot).

Two clean observations closed the earlier ambiguity:

- The second low-load `KS_H1A_TEST=1` round stayed in the collision search
  longer than the first observation window, then reached the 44 GB device scan
  because no mm answer was supplied. This confirmed that the first `rc=137`
  round was an insufficient observation window rather than a startup failure.
- The third low-load punch-only round with the M5 answer accepted
  `mm=ffffffc1b1cfb840`, completed both piles, and reached the previously
  missing chain:
  `H1A punch enter -> H1A punch done -> H1A punch returned -> H1A success
  calls=1 success=1`. This validates the fd-protect fix.

The first full-chain attempt with the same artifact also accepted the mm
answer and entered `consumer punch`. It then showed:

```text
punch completion wait timeout calls=1 inflight=1
consumer punch first ret=0 errno=0
consumer punch done tid=24770 ret=0 errno=0
KS_LEAK skb[*] prio=0x00000082 orig=0x00000082 same
pselect returned attempt=1 ret=86 calls=1 success=1 walk_touched=0 leak=00000000
pselect walk probe miss attempt=1/24; refusing cfi probe
```

The device rebooted immediately after this point (`uptime 0 min`). Therefore
fd protection is no longer the cause of the missing punch logs, but the
full-chain walk/CFI stage remains unsafe or semantically wrong. No further
device retry should be made until the `walk_touched=0` reboot is audited
offline.

Offline next step: `src/slide.c` now has a test-only H1A readback probe after
the punch watchdog finishes. It calls `ks_leak_readback()` and logs
`H1A leak=... walk_touched=...`, so the next clean H1A round can distinguish
"punch returned" from "the PI chain actually touched the fake page". The local
host lacks `make` and an Android NDK toolchain, so this probe is source-only
and has not been rebuilt or deployed.

Preserved evidence:

```text
build/ks_log_h1a_fdprotect_run2.txt
build/ks_log_h1a_fdprotect_run3.txt
build/ks_log_fullchain_m5_fdprotect_run1.txt
build/ks_collisions_fullchain_m5_fdprotect_run1.txt
build/ks_cache_fullchain_m5_fdprotect_run1.txt
build/ks_mm_answer_fullchain_m5_fdprotect_run1.txt
```

### 2026-09-15 (later) H1A-W placement probe + stride fix: still diffs=0

Materials convention: ADR-0004. Kernel source for citation checks is now
`materials/kernelsrc/sm8250-a13-stable` (full 4.19.157 Xiaomi SM8250 tree).

Offline verdict (Linnaeus, verified against the materials tree):
- stale rt_waiter is the stack-local in `futex_wait_requeue_pi()`
  (`materials/.../kernel/futex.c`; also `build/linux-4.19-futex.c:3105-3147`);
  NOT an SLUB object. kmalloc-96/mm-page hypotheses are dead.
- mm cache: request 0x388+8=0x390 (`fork.c:2595`, CONFIG_NR_CPUS=8), HWCACHE_ALIGN
  to 64B -> stride 0x3c0 -> 34 objs/slab (not 36). 4K pages confirmed
  (CONFIG_ARM64_PAGE_SHIFT=12), so ORDER3_SIZE=0x8000 stands.
- device coherency_line_size sysfs is not exposed; 0x3c0 remains the standard
  SM8250 expectation (A77/A55 64B L1).

Device runs:
- H1A-W run1 (`build/ks_log_h1a_w_run1.txt`): owner thread never set
  `h1a_w_owner_chain_done` -> deadlock -> watchdog kill. Fixed; also the
  pselect-enter wait now outlasts SLIDE_WAIT_SECONDS.
- H1A-W run2 (`build/ks_log_h1a_w_run2.txt`): clean, no crash. Evidence:
  kstkesp=0, /proc/stack EACCES, ptrace EPERM. Userspace cannot observe the
  kernel-stack SP on this kernel -> verdict PLACEMENT-UNKNOWN by design.
- H1A run10 with stride 34 (`build/ks_log_h1a_leakprobe_run10_stride34.txt`):
  punch still calls=1 success=1, all 4 SKB diffs=0. Spray count is NOT the
  blocker; consistent with the chain never being entered.

Decisive open question (next): does THIS kernel's `futex_wait_requeue_pi()`
timed path still leave the dangling rt_waiter / pi_blocked_on that the CVE
premise needs, or was the cleanup fix backported (android13-4.19 base)? If the
fix is present, the slide pselect route is a confirmed dead end on alioth.

### 2026-09-15 (night) H1A-D: EDEADLK signature CONFIRMED on device; punch reaches the PI walk

H1A-D (deadlock-shaped requeue, Pauli; audit passed) run1
(`build/ks_log_h1a_d_run1.txt`, pid 25728, binary sha256 2e6815aa...):

```text
H1A-D requeue ret=-1 errno=35 (expected ret=-1 errno=EDEADLK)   <- gate PASSED
H1A-D page prepared base=ffffffc1b1cf8000 ...
H1A-D waiter futex_requeue_pi ret=-1 errno=110
H1A punch setup nfds=320 timeout=0.200000
H1A punch enter tid=25730 nice=1
H1A punch watchdog timeout parent=25728 status=stuck            <- kernel walk spin
```

Milestones:
- The dangling-waiter precondition is REPRODUCED on alioth: FUTEX_CMP_REQUEUE_PI
  returns -EDEADLK exactly as the materials-tree analysis predicted; the
  victim keeps the stale pi_blocked_on and the punch no longer returns 0
  instantly (run9/10) - the PI chain is actually being walked now.
- The walk then SPINS instead of terminating: the stack-shaped stale waiter /
  fake-page linkage is still wrong on alioth. Prime suspects: the pselect
  core_sys_select stack-frame offset assumptions (stack_fds=T-0x210,
  stale_waiter=T-0x198) and/or the v12 ready-bitmap encoder offsets.
- Device recovered via `adb reboot` (uptime 0 min, load 0.08). The 90s
  watchdog killed the parent as designed; the kernel spin still required a
  reboot, consistent with AGENTS "kernel-mode spin is unkillable".
- Also found and killed a run5-era orphan (pid 20734) that had been running a
  kernelsnitch scan for ~1h and was polluting loadavg; verify `ps` residue
  after every killed run.

Next: offline core_sys_select frame-layout math against
materials/kernelsrc/sm8250-a13-stable (nfds=320 stack_fds path), then one
more single-shot H1A-D with corrected offsets.

### 2026-09-15 slide/H1A independent evidence audit

Completed an independent evidence-only audit (no device action, no exploit
code change, no new conclusions beyond cited logs/docs). Produced
`docs/specs/slide-evidence-matrix.md` with CONFIRMED / CONTRADICTED /
INSUFFICIENT verdicts covering current reproducible failure modes, success
markers, fd-protect effect, residual process hygiene, placement/model
findings, and the current blocking question. Key audit confirmations:
fd-protect fix works; mm answer and collision stages are not the blocker;
stale-waiter/fake-page linkage remains the open blocker; slide pselect
route is not disproven (EDEADLK gate passed and punch reached PI walk).

### 2026-09-15 sendmsg-family replacement audit: NO-GO

Offline ELF/source audit of native blocking `sendmsg`, `sendmmsg`, and
`recvmmsg` is complete in `docs/specs/sendmsg-frame-audit.md`; no device
action and no exploit-code change.

- `sendmsg.address` is at `T-0x168` and covers only the upper `0x20`
  bytes of the `T-0x198..T-0x148` waiter window.
- `sendmmsg.address` is at `T-0x238`, entirely below the window;
  `used_address` is above it.
- `recvmmsg.addr` is at `T-0x2c8` and is written by the protocol, not raw
  copied from user.
- The maximum continuous user-controlled coverage is `0x20`, not the
  required `0x50`; control bytes are not contiguous.

**Verdict: NO-GO. Do not implement a sendmsg-family replacement encoder.**

### 2026-09-15 pselect word-0 fallback audit

- Verdict: **NO-GO**; crash risk **HIGH**. See
  `docs/specs/pselect-word0-fallback-audit.md`.
- In the alioth frame, `res_in` word 0 lands at waiter offset `+0x00`
  (`tree_entry.__rb_parent_color`), not `waiter->lock` at `+0x38`
  (`res_out` word 2). The ELF confirms both mappings.
- A word-0-only shape leaves `waiter->lock=NULL`; `sched_setattr` ->
  `rt_mutex_adjust_pi()` -> `rt_mutex_adjust_prio_chain()` would dereference
  NULL in `raw_spin_trylock(&lock->wait_lock)`.
- This is offline-only. No device run and no exploit-code change were made.

### 2026-09-15 safe placement probe design (SP-1)

Offline design only: no device command, no exploit-code change, and no build.
New spec: `docs/specs/safe-placement-probe-design.md`.

SP-1 is designed to distinguish, in one controlled shot:

- `NO-LIVE-DANGLING`: `sched_setattr` returns 0 and the probe lock is empty.
- `WALKED-WRONG-PLACEMENT`: `sched_setattr` returns `EDEADLK` and the probe
  lock is empty.
- `PLACEMENT-CONFIRMED`: `sched_setattr` returns 0 and the SKB readback shows
  a nonzero `probe_lock.waiters.rb_root`.

The mechanism uses a fail-closed one-word pselect marker at
`waiter->lock` (`T-0x160`). The marker points to a new empty-owner probe
lock, not the CFI fake lock. If the PI walk reads it, it leaves the stack
waiter pointer in `probe_lock.waiters.rb_root`; if it misses the marker, the
real H1A-D PI cycle exits with `EDEADLK`. The probe never reaches CFI or the
full chain, and any gate/readback/reboot anomaly is fail-closed.

Do not implement or schedule this until the static checklist in the new spec
is audited. The design explicitly rejects arbitrary sub-qword placement
hypotheses as unsafe and out of scope.

### 2026-09-15 SP-1 adversarial review: UNSAFE

Offline audit verdict: **UNSAFE; do not implement, build, deploy, or
schedule SP-1.** Review:
`docs/specs/safe-placement-probe-review.md`.

Key findings:

- The marker-read path is not safe. The single-qword pselect shape zeroes
  `waiter.task`; when the empty-owner probe lock enqueues that waiter,
  4.19 `rt_mutex_adjust_prio_chain()` reaches
  `wake_up_process(rt_mutex_top_waiter(lock)->task)` and dereferences NULL.
- The marker-miss assumption is not fail-closed. `build/ks_log_h1a_d_run1.txt`
  passed the pre-punch `EDEADLK` requeue gate but then showed
  `punch watchdog timeout ... status=stuck`; a later PI walk is not proven to
  exit with `EDEADLK`.
- A nonzero `probe_lock.waiters.rb_root` is not sufficient proof of waiter
  enqueue. It can be confused with other kernel writes and lacks an exact
  `rb_root == rb_leftmost` / all-copy signature.

The single-qword placement at `res_out[2]` is mechanically correct, but that
does not make the probe safe. SP-1 remains blocked and must not advance to any
device run.

### 2026-09-15 H1A-D spin root-cause audit

Offline audit complete; see `docs/specs/h1a-d-spin-root-cause.md`. The
`-EDEADLK` requeue gate correctly proves the stale victim state, but
`remove_waiter()` clears the requeue thread's `pi_blocked_on`, not the
victim's. The victim's timeout path then skips waiter cleanup.

The subsequent pselect zeroes and only partially rewrites the stale waiter,
so the PI walk cannot reach a coherent `waiter->lock`. The kernel
`rt_mutex_adjust_prio_chain()` trylock-failure path is an unbounded
`cpu_relax(); goto retry;` loop; with the nominal partial shape, `lock == 0`
instead predicts an immediate fault/hard wedge. The log lacks a post-trylock
PC, so it cannot separate those two NO-GO outcomes. No further H1A-D punch or
full-chain device run is approved.

### 2026-09-16 kernel frame placement enumeration: one GO

Offline ELF/source enumeration is complete in
`docs/specs/kernel-frame-placement-enumeration.md`; no device access and no
exploit-code change were made.

- Corrects the prior blanket sendmsg NO-GO for native blocking TCP `sendmsg`:
  the adjacent `iovstack[5..7]` and first 32 bytes of `address` form one
  continuous, user-controlled 0x50 region over `T-0x198..T-0x149`.
- Frame chain: wrapper `0xffffff80091f4268` (`sub sp,#0x90`) ->
  `___sys_sendmsg` `0xffffff80091f3f98` (`sub sp,#0x190`) -> protocol call
  `0xffffff80091f4188`. At the block, `iovstack=C+0x38` and
  `address=C+0xb8`, where `C=T-0x220`.
- For a connected established TCP socket, TCP ignores normal `msg_name` and
  waits in `sk_stream_wait_memory()`, leaving the copied iov/address bytes
  intact.
- All audited other network, fs/poll/select, timer/nanosleep, and
  futex/requeue/wait/timeout entries remain NO-GO. SysV msg is unavailable
  because `CONFIG_SYSVIPC=n` and its native wrappers return `ENOSYS`.

**Placement verdict: GO for the raw 0x50 stack placement only; downstream
fake task/lock safety is not validated by this audit.**

### 2026-09-16 task-pointer acquisition audit

Offline audit complete; see
`docs/specs/task-pointer-acquisition-audit.md`. No device access and no
exploit-code change.

- Verdict: **FAKE-TASK-ONLY**.
- The pre-sendmsg KernelSnitch/mm-answer surface provides mm, slab page base,
  KASLR/delta, and futex collision addresses, but not a valid victim
  `task_struct`.
- `mm->owner` and the existing `init_task.tasks` walk are only usable after
  the post-CFI fops/configfs primitive, so they do not meet the pre-sendmsg
  requirement.
- Native blocking TCP sendmsg can encode the controlled `fake_task` pointer
  into the stale waiter without the victim task pointer.
- This supports only a bounded fake-task PI walk under the full
  waiter/task/lock contract. It does not validate CFI, pipe-physrw, root, or
  scheduler-specific fake-task fields.
