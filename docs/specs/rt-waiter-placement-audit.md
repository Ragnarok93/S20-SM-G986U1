# rt_waiter placement audit (alioth 4.19.157 H1A)

Date: 2026-09-15
Scope: offline source/config audit only. No adb/device command and no
device build was run. `src/` and build scripts were not modified.

Citation policy:

- In-repo verifiable kernel evidence is limited to `build/linux-4.19-futex.c`
  and `build/.config`; these may be cited by exact file/line.
- All other kernel source paths are **upstream 4.19, not verifiable in-repo**.
  They may be used as context or reasoning, but their line numbers must be
  treated as external references, not local proof.

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
3. `futex_wait_requeue_pi()` declares the stack-local waiter at
   `build/linux-4.19-futex.c:3111`, initializes it at `:3139`, and publishes
   `q.rt_waiter = &rt_waiter` at `:3147`.
4. The requeue/proxy path enters the waiter through
   `rt_mutex_start_proxy_lock()` (`build/linux-4.19-futex.c:2074-2091`).
   The rtmutex-side details are upstream 4.19, not verifiable in-repo.
5. On the timed path, `futex_wait_requeue_pi()` calls
   `rt_mutex_wait_proxy_lock(pi_mutex, to, &rt_waiter)` and then
   `rt_mutex_cleanup_proxy_lock(pi_mutex, &rt_waiter)`
   (`build/linux-4.19-futex.c:3216-3219`). The user-visible result in run9 was
   `ret=-1 errno=110` (`build/ks_log_h1a_leakprobe_run9_diff.txt:9-13`).
6. `pselect6` does not allocate an rt_waiter. Its `do_pselect() ->
   core_sys_select()` path is upstream 4.19, not verifiable in-repo; its
   timeout only controls the readiness wait.

## Waiter size and cache

For arm64 with the checked configuration:

- `struct rb_node` is one `unsigned long` plus two pointers, so it is 24 bytes
  on arm64. This follows upstream 4.19 `include/linux/rbtree.h:36-40`;
  that file is not verifiable in-repo.
- `struct rt_mutex_waiter` contains two rb nodes, `task`, `lock`, an `int
  prio`, and a `u64 deadline` (`kernel/locking/rtmutex_common.h:27-39`).
  That definition is upstream 4.19, not verifiable in-repo. With 8-byte
  alignment this is 80 bytes, i.e. `0x50`.
- The relevant debug fields are absent: `CONFIG_DEBUG_RT_MUTEXES` and
  `CONFIG_DEBUG_LOCK_ALLOC` are disabled (`build/.config:6354`,
  `build/.config:6359`).
- If this 80-byte object were kmalloc'ed, `kmalloc_index(80)` would select the
  65-96-byte index and use a `kmalloc-96` cache. Those mapping details follow
  upstream 4.19 `include/linux/slab.h:345-354` and
  `mm/slab_common.c:1194-1201`, and are not verifiable in-repo. But no such
  allocation exists on this path: the object is a stack local.

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

There is, however, a high-probability stride mismatch that must be confirmed
from device output before any `src/` change. The mm cache object may not use
the raw `sizeof(struct mm_struct)` value:

- Upstream 4.19 `kernel/fork.c:2601` computes
  `mm_size = sizeof(struct mm_struct) + cpumask_size()`; this source file is
  not verifiable in-repo.
- The target has `CONFIG_NR_CPUS=8` (`build/.config:408`), so for 8-byte
  `long`, upstream 4.19 `include/linux/cpumask.h:672-676` yields an 8-byte
  `cpumask_size()`; that header is not verifiable in-repo.
- With `sizeof(struct mm_struct)=0x388`, the cache request is therefore
  `0x390`.
- Upstream 4.19 `kernel/fork.c:2603-2608` creates the cache with
  `SLAB_HWCACHE_ALIGN`; that citation is not verifiable in-repo. SLUB may
  align object stride to the cache's computed alignment
  (`mm/slub.c:3608-3666`, also not verifiable in-repo). Under the repo's
  existing 0x3c0 stride model, one order-3 slab holds
  `0x8000 / 0x3c0 = 34` objects, not the current `0x8000 / 0x388 = 36`.

Thus the actionable offline finding is only a mismatch between the requested
object size (`0x390`) and the assumed allocation stride (`0x388`, with an
alternative 0x3c0 model). The actual stride must be confirmed via one safe
`/proc/slabinfo`-style or allocation-count probe before any spray-count patch.

This does not by itself prove that the chosen page is not an SKB page, but it
skews pre/post/spray counts. It also reinforces that `MM_STRUCT_SZ=0x388`
means the logical `mm_struct` size, not the SLUB object stride.

## Relation to H1A diffs=0

The pselect encoder is intended to reshape the stale stack waiter through the
readiness output bitmap. For `PSELECT_ROUTE_NFDS=320`
(`src/common.h:141`), each of six fd sets is 40 bytes. `core_sys_select()`
may use `stack_fds` when six sets fit in its 256-byte stack array and then lay
out `in|out|ex|res_in|res_out|res_ex`; these details follow upstream 4.19
`fs/select.c:606`, `fs/select.c:625-626`, and `fs/select.c:638-642`, and are
not verifiable in-repo. The code comments place the stale waiter in the
`res_in/res_out` window (`src/slide.c:107-124`), and the encoder writes
terminal rb nodes, task, fake lock, priority, and deadline through that window
(`src/slide.c:138-161`).

Thus `diffs=0` means no write landed in the fake page. The leading actionable
failure is that the stale stack waiter was not shaped or not consumed as
expected on this run. The cache classification above removes the opposite
hypothesis: no SLUB allocation ever put the rt_waiter in the mm order-3 page.

## Minimal patch proposal (documentation only)

Do not apply this patch until H1A-W step 0 confirms the actual mm cache stride
from device output. If confirmed, use the mm cache stride, not the raw logical
mm size, for spray counts. Keep `MM_STRUCT_SZ=0x388` for mm field/address
semantics.

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

0. First collect read-only evidence of the actual mm cache stride from device
   output: a `/proc/slabinfo`-style entry, allocation-count probe, or
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
bitmap. H1A `diffs=0` says the intended fake-page walk did not happen. The
remaining `0x388` versus `0x390`/`0x3c0` issue is a high-probability stride
mismatch requiring device confirmation, not a definite layout bug.
