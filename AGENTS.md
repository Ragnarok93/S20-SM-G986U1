# GhostLock K40 (alioth) — 当前状态 (2026-09-13)

## 目标

Redmi K40 (alioth, SM8250, 8GB, 4.19.157-perf-g92c089fc2d37, MIUI V816 Android 13, BL 锁定)。
Firefox JIT 链路加载 preload, 内核提权 root。硬约束: 不弄坏手机, 拒收 BL 解锁。

## 路线 (已定型)

slide pselect (futex PI) 在本内核是死路, **全流程跳过 slide**, 走:

```
KernelSnitch mm 泄露 (PC-hybrid) -> prepare FOPS 页 -> punch (PI walk 在假 waiter 上)
-> fops 劫持 -> configfs 任意读写原语 -> scan_kaslr_base (configfs 读扫 8GB/2MB 步进)
-> 线性映射校准 (memstart_addr) -> pipe physrw -> root
```

KS_SLIDE_TEST=1 仍可单独测 slide, 全流程入口不再调它。

## 工作配置 (固化)

- target.h: `src/targets/alioth-4.19.157-perf/target.h` (KIMAGE_TEXT_BASE=0xffffff8008080000,
  PAGE_OFFSET=0xffffffc000000000, 39 位 VA, 线性映射随机化已证实, 全 256GB 扫描窗口)。
- 符号偏移全部 vmlinux 反汇编实锤: cred 0x7e0/0x7e8, task pi_* 0x8b4-0x8d8,
  mm_struct=0x388, task_struct=0x1140; alioth 专属: ashmem 系列 fops +
  anon_pipe_buf_ops 0x018e8640 + memstart_addr 0x0267adc8。
- 构建: 项目根目录下
  `ghostlock-app\ndk\android-ndk-r27c\...\bin\aarch64-linux-android35-clang.cmd @build_w1_test.rsp`
  (rsp 含相对路径)。产物 `build/alioth/bin/preload_w1_test.so`。
- PC answer 严格 22 字节 LF `mm=0x<16hex>`; 推送必须 tmp+sync+mv 原子协议;
  每轮跑前清空 `ks_mm_answer.txt` 和 `ghostlock_ks_cache.txt`, 预创建 666 文件。
- untrusted_app (u0_a2) 限制: /data/local/tmp 不能新建文件 (可写已有 666 文件);
  /dev/full 写被 SELinux 拒 (fd-bitmap 已改 pipe 写端); so 必须经 Firefox 上传落位 app 目录。

## 当前构建

`preload_w1_test.so` 199064 bytes, SHA256 `4921D41A...FA46B1095`
(跳过 slide + 页内 PI 链 + pipe 写端 fd + 忙等 32B/64B 上限), 已在 /sdcard/Download/。

## 当前阻塞 (2026-09-13 14:50, 下一步从这里继续)

4921D41A 重跑结果: mm 泄露成功后系统被拖死 (keystore2 watchdog pending 283s+,
logcat 14:23-14:41), 进程 14:41 被杀, 主路由零输出。两个硬阻塞:

### 阻塞 1 (根本): fops 劫持写目标地址错误

`data_addr(ASHMEM_MISC_FOPS)` 依赖 `g_linear_map_delta` (当前 0), 但线性映射
已证实随机化 (mm_va - PAGE_OFFSET ~65GB >> 8GB RAM)。delta=0 时 write_right
指向错误地址, **walk 再完美也劫持不到真 misc_fops**。必须先校准 delta。

校准方法 (确定性):
```
memstart = mm_phys - (mm_va - PAGE_OFFSET)
delta    = memstart - P0_PHYS_OFFSET
```
mm_va 每轮已知; mm_phys 未知但受 RAM [0x80000000, 0x280000000) 约束。
若厂商随机化按 arm64 标准 SECTION_SIZE (1GB) 对齐, 每轮 mm_va 只对应
~7 个候选 delta。验证 = 用候选 delta 重跑, try_cfi_stage step=4
(读回 misc_fops == fake_fops) 通过即命中。每轮 ~4 分钟, 最多 7 轮。
下一步: 写 delta 候选生成器 + 自动化循环 (跑 -> 判 step=4 -> 换候选)。

### 阻塞 2: PI walk 行为不确定

上轮 walk 20s 完成 (ret=0), 本轮 26min 未完成拖死系统。"walk 真的执行了
rb 写"从未被直接观测 (ret=0 只说明 syscall 成功)。对策: punch 后用
recvmsg 回读 SKB 页 (ks_leak_readback 已有), 内核改了我们的页 = walk 碰过
假链; 没改 = walk 没碰, 需换方向。与阻塞 1 的每轮测试合并埋点, 零额外成本。
若最终确认 walk 不可控, 备选方向: io_uring / setxattr UAF 等其他 CVE
(见 2026-09-13 03:40 章节的备选清单)。

## myroot 操作 (Firefox 链路)

入口 https://ccp-p.github.io/myroot/ 。页面大按钮执行; 终端打印 PID=/LOG=。
自动档: exploit 就绪且 manifest 加载后, 页面自动 getprop/uname 识别设备并匹配
manifest (K40 靠 4.19.157 内核前缀), 自动 fetch 对应 .so 走 uploadSoAndRun;
终端在流程结束后仍可手动输入命令。取消勾选"自动识别设备并上传 .so"可回到手动模式。
跑流程: 上传 so 到 Firefox -> 页面点跑 -> collision 导出后 adb pull ->
PC 端 `build\pc_mm_search_wirefix.exe <collisions> <answer> 16` -> tmp+sync+mv 回填
-> 设备 1GB 边界截胡采纳。判定新导出以新 log 文件出现 `KS_PC export` 为准。

### adb+截图自动操作 (Codex 免人工跟进)
```powershell
# 前置检查 (device 在线/目标机/页面 URL)
adb devices; adb shell "getprop ro.product.device; uname -r"
# 拉起页面 (加 ?v=N 穿透缓存), 点亮解锁可省略(页面已在后台也会跑)
adb shell am start -a android.intent.action.VIEW -d "https://ccp-p.github.io/myroot/?v=2" org.mozilla.firefox
# 截图盯流程 (存 build/, 每 60-90s 一次直到 Root/失败)
adb exec-out screencap -p > build\k40_now.png
```
判定: 页面自动档会自己匹配 K40 并上传 .so; 终端出现 brute forcing/KS_PC export
即链路在跑; 流程进度"等待 result.done"未结束前不要动设备。完毕后截图确认
Root/KernelSU 状态, 失败先 `adb pull` /data/local/tmp/full_run.log 与 /sdcard/Download/log_*.txt。

## 2026-09-13 17:2x 全自动实跑记录 (未人工介入)

- myroot 页面自动档实跑成功: 自动 getprop/uname 识别 alioth -> codename 匹配
  manifest 新条目 `Redmi K40 (alioth) GhostLock ...` (54 设备) -> 自动上传
  `so/ghostlock_alioth.so` (= build/alioth/bin/preload.so 495C4B9A) -> LD_PRELOAD 启动。
- 注意: GitHub Pages 会忽略 query 参数做 CDN 缓存, manifest/index 更新后最长
  10 分钟才对手机生效; 刷新前先 `curl.exe -s https://ccp-p.github.io/myroot/manifest.json`
  验证含 ghostlock_alioth。旧通用条目 redmi_k40_4.19.so 会走 slide, 实测直接
  panic 重启, 勿再匹配到它。
- 全链路跑通到 PC 回填: KS_PC export 8 addrs -> adb pull -> pc_mm_search_wirefix
  命中 `mm=0xffffffc1b1cfb840` -> tmp+sync+mv 回填成功, 设备采纳。
- 采纳后 walk 阶段把系统拖死 (load 120->140, screencap/adbd 均阻塞), 即阻塞 2
  旧现象重现; `adb shell reboot` 循环重试在第 1 次补发后落地 (僵死期 reboot 请求
  排不进调度, adb 通道基本废掉)。实际是用户长按电源键手动重启成功, 17:45 恢复,
  load 0.71。教训: 拖死期 adb 重启基本无效, 直接提示用户长按电源键最快。
- 结论: 识别/上传/回填全自动化已验证; 卡点仍是 walk 拖死系统, 重跑同二进制只会
  重复, 下轮必须带 ks_leak_readback 埋点判断 walk 是否碰假链 (见阻塞 2 对策)。

## 2026-09-13 18:0x delta 候选实测

- 已加 `src/util.c:select_linear_map_delta()` + `tools/pc_delta_candidates.py`:
  以泄漏 mm 按 `memstart = mm_phys - (mm_va - PAGE_OFFSET)` 生成 1GB 对齐候选,
  并用 `/data/local/tmp/ks_delta_index.txt` 选择候选序号 (缺省 0)。
- 18:00 实跑: mm=0xffffffcec3a9b480, diff=0x0000000ec3a9b480, 候选 8 个,
  候选 0 的 memstart=0xfffffff1c0000000 / delta=0xfffffff140000000 / mm_phys=0x83a9b480。
  设备端 `KS_DELTA` 日志确认选择器工作正常。
- CFI 未到达; punch 后仍长时间无 `pselect returned` 日志, load 升到 ~16 后主动 reboot。
  因此 18:0x 结论: delta=候选0 不是命中, 且 `ks_leak_readback` 埋点尚未在日志中闭合;
  下轮必须先把 `/data/local/tmp/ks_delta_index.txt` 原子改成 1 再试 (已预置为 1)。
- `myroot/so/ghostlock_alioth.so` 已更新并推 Pages, manifest 已 bump 到
  `?v=3`; 新包 201248 字节, SHA256 `F8675E78...EC38113`。

## 设备安全备忘

- 持续满载会热保护关机; 扫描后冷却再继续。
- 内核态自旋不可杀 (kill -9 无效), 只能重启; adb reboot 偶尔无效用 adb shell reboot。
- 每轮跑前清 answer/cache 残留, 否则进程采纳旧 mm 会写错内核位置。
- 后台 nohup 会被神秘杀死, 用前台 PTY 跑。

## 2026-09-13 下午: 布局实证闭合 + slide 字段值改为上游语义 (R1-R4 完成)

方法: 用子代理研究 + 主会话执行。上游 Linuxoid-cn/CVE-2026-43499-Poc-Analysis 已克隆到 upstream-linuxoid/ (分支 secret)。其 generate_target.py 无法用于 alioth (内核无 BTF/无 ikconfig, 已解包核实), 但其"反汇编推导栈帧重叠"思路被移植为 tools/derive_pselect_alioth.py (含 ELF 包装器, llvm-objdump 用 NDK r27c)。

反汇编实证 (alioth_kernel_raw.bin, 从 boot.img v3 offset 4096 解出, 0x303c810 字节; 注意 build/alioth_Image.bin 是 ramdisk 不是内核):
- __arm64_sys_pselect6 @file 0x25fb78: sub sp,#0xa0; 直接 bl core_sys_select @0x25fd7c
- core_sys_select @0x25ef20: sub sp,#0x1c0; stack_fds = add x19,sp,#0x50 (@0x25efc8) => word0 = T-0x210
- nfds=320 走栈内路径 (@0x25f008 cmp #0x180, nfds=320 => 0x17f < 0x180)
- __arm64_sys_futex @0x116c88: sub sp,#0x70; 直接 bl do_futex @0x116dcc
- do_futex @0x1132e0: stp x29,[sp,#-0x60]! + sub sp,#0x1c0 (总 0x220)
- stale rt_waiter: @0x114564 sub x2,x29,#0xc8 -> @0x114570 bl rt_mutex_wait_proxy_lock (@0xd14f8, 其内部 mov x20,x2 转传); x29 = T-0x70-0x60, 无中间重设 => waiter = T-0x198
- 结论: 本地硬编码 T-0x210/T-0x198/nfds=320 全部正确; slide panic 根因不在布局
- 注意: 子代理曾把 futex_wait (0x114c68 起) 的栈变量误当 requeue_pi 的 waiter (T-0x288), 已用函数边界 + x2 参数流证据否决

根因 (Pascal 假设1, 已按上游语义修复): 旧 fd-bitmap 编码把 tree/pi_tree 的 rb 指针全填 0 ("detached"), PI walk 的 rb 操作以 NULL 父指针破坏真实 PI 树 => SIGABRT/panic 交替。上游 word-shift 值 = 用真实内核对象当假节点 (loggers[0][1] 做父, random_table bootid data 做 leak 通道)。

已修改 (唯一改动): src/slide.c prepare_slide_select_fdsets
- in word0=SLIDE_LOGGERS_0_1 (tree.pc), word2=SLIDE_RANDOM_BOOT_ID_DATA (tree.left), word3=SLIDE_LOGGERS_0_1 (pi_tree.pc)
- out word0=SLIDE_RANDOM_BOOT_ID_DATA (pi_tree.left), word1=SLIDE_INIT_TASK (task, 上游 parity, 不再用 fake_task), word2=fake_lock, word3=FAKE_WAITER_PRIO
- word1/4 与 res_ex 保持 0; fd 类型机制 (/dev/zero in-only, pipe wonly out-only, /dev/null both) 不变
- 未改: consumer 10 次重试 (失败才重试, 不放大)、waiter 线程退出时机 (崩溃时序在 route_done 前)

构建: pwsh cmd /c 内层引号用 \`" (见 git log); su_daemon 先编到 build/embed, preload.so -> build/alioth/bin/preload.so (199176 字节, 2026-09-13 16:11)
下次运行: adb push build/alioth/bin/preload.so /data/local/tmp/preload_alioth.so 后按原流程 KS_MM_TEST=1 先验证 mm 泄露, 再全链路; 每次 panic 后先读 /data/local/tmp/full_run.log 与 /sdcard/Download/log_*.txt
