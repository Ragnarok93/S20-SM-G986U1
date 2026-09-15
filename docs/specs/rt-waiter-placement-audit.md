# rt_waiter placement audit (alioth 4.19.157 H1A)

Date: 2026-09-15
Scope: offline source/config audit only. No adb/device command and no
device build was run. `src/` and build scripts were not modified.

Citation policy:

- The registered kernel tree is
  `materials/kernelsrc/sm8250-a13-stable` (see ADR-0004). It is the Xiaomi
  SM8250 Android 13 / Linux 4.19.157 reference tree and may be cited by exact
  file/line.
- `build/.config` and `build/linux-4.19-futex.c` remain useful device-build
  evidence.

## Verdict

The stale object used by the pselect route is **not an SLUB/kmalloc
allocation**. The in-repo kernel source proves this: it is the stack-local
`struct rt_mutex_waiter rt_waiter` in `futex_wait_requeue_pi()`
(`build/linux-4.19-futex.c:3105-3111`). The source explicitly says the waiter
is allocated on the stack and assigns its address to `q.rt_waiter`
(`build/linux-4.19-futex.c:3137-3147`). Therefore the order-3 `mm_struct`
page at `ffffffc1b1cf8000` cannot be the cache/page that "serves" the
rt_waiter allocation.

The H1A `diffs=0` result (`docs/status.md:304-326`) is consistent with the PI
chain not being entered from the shaped stack waiter. It is not explained by a
hypothetical `mm_struct` versus `kmalloc-96` allocation-cache mismatch,
because neither cache is involved for the stale waiter. The fake page can
plausibly be an SKB-data page after reclaim, but it is not the rt_waiter
allocation page.

## Exact 4.19 path

1. The H1A waiter calls `FUTEX_WAIT_REQUEUE_PI`
   (`src/slide.c:687-691`), then calls `pselect()` after that syscall returns
   (`src/slide.c:692`; the pselect syscall is issued at `src/slide.c:598`).
2. `do_futex()` dispatches `FUTEX_WAIT_REQUEUE_PI` to
   `futex_wait_requeue_pi()` (`build/linux-4.19-futex.c:3517-3548`).
3. The registered tree's `futex_wait_requeue_pi()` declares the stack-local
   waiter at
   `materials/kernelsrc/sm8250-a13-stable/kernel/futex.c:3316`, initializes it
   at `:3349`, and publishes `q.rt_waiter = &rt_waiter` at `:3352`.
4. The requeue/proxy path enters the waiter through
   `rt_mutex_start_proxy_lock()` in
   `kernel/locking/rtmutex.c:1798-1805`; the caller site is
   `kernel/futex.c:2241`.
5. On the timed path, `futex_wait_requeue_pi()` calls
   `rt_mutex_wait_proxy_lock(pi_mutex, to, &rt_waiter)` and then
   `rt_mutex_cleanup_proxy_lock(pi_mutex, &rt_waiter)`
   (`materials/kernelsrc/sm8250-a13-stable/kernel/futex.c:3422-3425`). The
   user-visible result in run9 was `ret=-1 errno=110`
   (`build/ks_log_h1a_leakprobe_run9_diff.txt:9-13`).
6. `pselect6` does not allocate an rt_waiter. Its `core_sys_select()` declares
   the 256-byte `stack_fds` array in
   `materials/kernelsrc/sm8250-a13-stable/fs/select.c:606`
   (`include/linux/poll.h:20-21`), and assigns the six bitmaps as
   `in|out|ex|res_in|res_out|res_ex` at `fs/select.c:625-637`. The timeout only
   controls the readiness wait.

## Waiter size and cache

For arm64 with the checked configuration, verified in
`materials/kernelsrc/sm8250-a13-stable`:

- `struct rb_node` is one `unsigned long` plus two pointers, aligned to
  `sizeof(long)` (`include/linux/rbtree.h:36-40`), so it is 24 bytes on arm64.
- `struct rt_mutex_waiter` contains two rb nodes, `task`, `lock`, an `int
  prio`, and a `u64 deadline` (`kernel/locking/rtmutex_common.h:27-40`).
  With 8-byte alignment this is 80 bytes, i.e. `0x50`.
- The relevant debug fields are absent: `CONFIG_DEBUG_RT_MUTEXES` and
  `CONFIG_DEBUG_LOCK_ALLOC` are disabled (`build/.config:6354`,
  `build/.config:6359`).
- If this 80-byte object were kmalloc'ed, the 65-96-byte index would be the
  natural `kmalloc-96` cache. But no such allocation exists on this path: the
  object is a stack local.

## Groom cross-check

The current H1A page is derived from a leaked `mm_struct`:

- `MM_STRUCT_SZ` is `0x388` for alioth
  (`src/targets/alioth-4.19.157-perf/target.h:28`) and the mm spray uses order 3
  (`src/common.h:57-62`).
- `prepare_kernel_page()` computes `mm_objs_per_slab = ORDER3_SIZE / 0x388`
  (`src/util.c:1202`) and then uses `select_slab_base()` to truncate the leaked
  mm address to a 32 KiB page (`src/util.c:1322`; `src/util.c:538-546`).
- Run9 logged exactly that page: mm `ffffffc1b1cfb840`, base
  `ffffffc1b1cf8000`, fake lock `ffffffc1b1cf9350`, fake w0
  `ffffffc1b1cfa220` (`build/ks_log_h1a_leakprobe_run9_diff.txt:7-9`).

That page may be reused as SKB data by the socket reclaim spray. The relevant
SKB constants are also self-consistent: `SKB_DATA_DELTA` is zero
(`src/common.h:46`), `SKB_SEND_SIZE` is order-3 size minus aligned shared-info
(`src/common.h:65-66`), and run9 read back 32448 bytes for each of four SKBs
(`src/util.c:873-908`; `build/ks_log_h1a_leakprobe_run9_diff.txt:14-17`).
Delta zero is correct for the fixed 4.19 arm64 linear map; it is not the cause
of a waiter-placement mismatch.

### Verified mm-cache stride

The materials tree now closes the stride math. The registered source computes
`mm_size = sizeof(struct mm_struct) + cpumask_size()` in
`materials/kernelsrc/sm8250-a13-stable/kernel/fork.c:2595`, then creates the
`mm_struct` cache with `ARCH_MIN_MMSTRUCT_ALIGN` and
`SLAB_HWCACHE_ALIGN` at `kernel/fork.c:2597-2602`. For arm64,
`ARCH_MIN_MMSTRUCT_ALIGN` defaults to zero (`kernel/fork.c:2557-2558`).

`cpumask_size()` is `bitmap_size(nr_cpumask_bits)`
(`include/linux/cpumask.h:670-676`); `bitmap_size(nbits)` is
`ALIGN(nbits, BITS_PER_LONG) / BITS_PER_BYTE`
(`include/linux/bitmap.h:215`). The build is not
`CONFIG_CPUMASK_OFFSTACK`, so `nr_cpumask_bits` is `NR_CPUS`
(`include/linux/cpumask.h:43-47`). With `CONFIG_NR_CPUS=8`
(`build/.config:408`), the cpumask tail is one 64-bit word, i.e. 8 bytes.

Therefore the SLUB requested object size is:

```text
sizeof(struct mm_struct)        0x388
cpumask_size()                  0x008
requested object size           0x390
```

4.19 SLUB then aligns that requested size to the cache alignment. The
alignment calculation is in
`materials/kernelsrc/sm8250-a13-stable/mm/slab_common.c:266-290`; for
`SLAB_HWCACHE_ALIGN` it asks `cache_line_size()` and only shrinks that value
when the object is small (`size <= ralign / 2`). SLUB stores the result in
`s->align` (`mm/slab_common.c:383`) and rounds the object stride with
`size = ALIGN(size, s->align)` (`mm/slub.c:3660-3664`).

On this arm64 tree, `cache_line_size()` is not a compile-time
`L1_CACHE_BYTES` value: it returns `4 << CTR.CWG`, or
`ARCH_DMA_MINALIGN=128` if CWG is zero
(`arch/arm64/include/asm/cache.h:42-48` and `:86-90`). SM8250's Cortex-A55/A76
parts report a 64-byte cache writeback granule, so the expected runtime value
is 64. With `0x390 = 912`, the next 64-byte boundary is `0x3c0 = 960`:

```text
ALIGN(0x390, 64) = 0x3c0
```

This also confirms that 4K pages are active (`CONFIG_ARM64_4K_PAGES=y`,
`CONFIG_ARM64_PAGE_SHIFT=12`; `build/.config:242` and `:397`), so the current
`ORDER3_SIZE=0x8000` definition is correct for a kernel order-3 page.

Definitive offline verdict:

1. The logical `mm_struct` size is still `0x388`, but the mm-cache object
   stride is not `0x388`.
2. For the expected SM8250 64-byte `cache_line_size()`, the mm cache stride is
   `0x3c0`.
3. One 0x8000 order-3 slab therefore holds
   `floor(0x8000 / 0x3c0) = 34` mm objects, not the current 36.
4. If a future device read disproves the 64-byte CWG assumption, the source
   formula remains `ALIGN(0x390, cache_line_size())`; 128-byte CWG would give
   `0x400` and 32 objects per order-3 slab. A read-only cache/slab probe should
   record this value once before any behavior-affecting patch.

The remaining device check is no longer whether `0x388` is the allocation
stride; it is only a fail-closed confirmation of the runtime CWG/cache-line
value. `MM_STRUCT_SZ=0x388` remains correct for mm field/address semantics.

## Relation to H1A diffs=0

The pselect encoder is intended to reshape the stale stack waiter through the
readiness output bitmap. For `PSELECT_ROUTE_NFDS=320`
(`src/common.h:141`), each of six fd sets is 40 bytes. `core_sys_select()`
may use `stack_fds` when six sets fit in its 256-byte stack array and then lay
out `in|out|ex|res_in|res_out|res_ex`; the registered tree verifies this at
`materials/kernelsrc/sm8250-a13-stable/fs/select.c:606`,
`fs/select.c:625-637`. The code comments place the stale waiter in the
`res_in/res_out` window (`src/slide.c:107-124`), and the encoder writes
terminal rb nodes, task, fake lock, priority, and deadline through that window
(`src/slide.c:138-161`).

Thus `diffs=0` means no write landed in the fake page. The leading actionable
failure is that the stale stack waiter was not shaped or not consumed as
expected on this run. The cache classification above removes the opposite
hypothesis: no SLUB allocation ever put the rt_waiter in the mm order-3 page.

## Minimal patch proposal (documentation only)

Do not apply this patch until H1A-W step 0 confirms the runtime cache-line/CWG
value from device output. If confirmed as 64 bytes, use `0x3c0`, not the raw
logical mm size, for spray counts. Keep `MM_STRUCT_SZ=0x388` for mm
field/address semantics.

```diff
--- a/src/common.h
++++ b/src/common.h
@@
 #define MM_STRUCT_SZ 0x388
+#define MM_CACHE_OBJ_STRIDE 0x3c0
--- a/src/util.c
++++ b/src/util.c
@@
-  mm_objs_per_slab = ORDER3_SIZE / 0x388; /* spray sizing uses real mm_struct size */
+  mm_objs_per_slab = ORDER3_SIZE / MM_CACHE_OBJ_STRIDE;
```

The same expression exists in the separate pipe path (`src/pipe.c:123`), but
it should not be changed as part of an H1A-only patch.

## Safe next probe: H1A-W

Goal: one run, userland only, H1A only; no CFI, no fops hijack, no full chain.

Design a new `H1A_W=1` gated artifact with no `sched_setattr` punch:

0. First collect read-only evidence of the runtime cache-line/CWG value from
   device output: a `/proc/slabinfo`-style entry, cache sysfs
   `coherency_line_size`/`writeback` evidence, allocation-count probe, or
   equivalent. Record the reported object size, objects-per-slab, and cache
   name. If the evidence is denied or ambiguous, log `STRIDE-UNKNOWN` and
   abort before pselect geometry work; do not infer the stride into a patch.
1. Start the H1A waiter/owner futex sequence, but stop after the waiter enters
   a deliberately long `pselect()` with `nfds=320`. Do not prepare or invoke
   the fake-page chain.
2. From a separate parent thread, collect read-only placement evidence while
   the waiter remains blocked:
   - try `/proc/<waiter_tid>/stat` and record the kernel SP fields if exposed;
   - if allowed, use `PTRACE_SEIZE` plus `PTRACE_GETREGSET` on the waiter
     thread to record `pc` and `sp`; detach immediately;
   - optionally try `/proc/<waiter_tid>/stack` for read-only frame evidence.
3. Compute the expected waiter address from the already disassembly-verified
   frame offsets (`src/slide.c:107-112`): `stack_fds=T-0x210`, stale waiter
   `T-0x198`, where `T` is the pselect/futex syscall stack top. Confirm that
   the recorded SP/frame belongs to the waiter thread's kernel stack, not the
   leaked linear-map/mm page.
4. Log one of three fail-closed outcomes: `STACK-CONFIRMED`,
   `PLACEMENT-UNKNOWN`, or `PROBE-DENIED`. Abort before any chain if the
   result is not `STACK-CONFIRMED`.

The parent's ptrace/proc read is a syscall on a different kernel stack, so it
does not clobber the waiter's stale stack bytes. There is no PI-chain trigger,
fake FOPS target, physical read/write, or full-chain step. Because a failed
attempt can still perturb kernel state, keep the existing one-attempt-per-boot
rule and require explicit review of the log before any later H1A-C test.

## Conclusion

The task's original cache hypothesis is false in its literal form: the stale
rt_waiter does not come from `kmalloc-96` and cannot land in the mm order-3
page. The real mechanism is a stale stack waiter shaped by pselect's readiness
bitmap. H1A `diffs=0` says the intended fake-page walk did not happen. The mm
source now establishes the likely stride mismatch: `0x388` is only the logical
struct size, while `0x3c0` is the expected SLUB stride and yields 34 objects
per order-3 slab.
