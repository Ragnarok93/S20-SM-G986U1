# H1A-S2 final audit

Date: 2026-09-16

Scope: offline final source and artifact audit only. No device command,
deployment, execution, or exploit run was performed. The audited sources
were `src/slide_s2.c`, `src/util.c`, `src/common.h`, and the isolated
`src/s2_main.c` entry. The requirements are
`docs/specs/sendmsg-h1a-s2-implementation-design.md` and
`docs/specs/h1a-s2-proxy-root-audit.md`.

## Verdict

**PASS as an offline S2-boundary build** after fixing four blocking audit
defects. This remains GO only for the isolated `KS_H1A_S2_TEST=1` page/PI
probe. It is not approval for device use, CFI, fops mutation, pipe-physrw,
root, the full chain, retries, or any follow-on stage.

Final artifact:

```text
build/alioth/bin/preload_slide_h1a_s2.so
size=201648
sha256=1CB81B113442415246F34C0D6106F0A844117B55364944103857AD74FF043605
```

Built with NDK r26b `aarch64-linux-android34-clang.cmd`, API 34, alioth
target. The only build warning is the pre-existing unused `ks` in
`src/kernelsnitch/kernelsnitch.h`.

## Blocking defects and fixes

1. **Vacuous page contract.** `ORDER3_SIZE=0x8000`, but the controlled SKB
   data is `SKB_SEND_SIZE=0x7ec0`. Both payload preparation and
   `s2_payload_contract_ok()` used a loop requiring a complete 0x8000
   chunk, so neither wrote nor checked any S2 fields; the old contract
   returned true without testing anything.

   Fix: S2 now explicitly prepares and checks one controlled prefix.
   With `SKB_FRAG_BIAS=0`, the mandatory fields end at `FT+0x8d8+8 =
   payload_base+0x3ae0`, well inside 0x7ec0. The code first rejects any
   bias/offset configuration that puts W0 or FT fields outside the
   controlled send data. It then checks the exact FL/W0/FT shapes,
   `FT+0x420=0`, PI roots, `pi_blocked_on=0`, and the zeroed fops range.

2. **Page retry.** The default `prepare_good_kernel_page()` path allowed
   two attempts. That violated the S2 no-refresh/no-second-attempt rule.
   `PAGE_PAYLOAD_S2` now has exactly one attempt and fails closed.

3. **Invalid TCP gate.** `socketpair(AF_INET, SOCK_STREAM)` does not
   create a connected IPv4 TCP pair on Linux. The gate now creates a real
   loopback listener, connects, accepts, then applies `TCP_NODELAY`, the
   nonblocking fill/EAGAIN sequence, blocking restore, and the exact
   `SIOCOUTQ` threshold before arming.

4. **Missing readback could pass.** Readback evidence was logged but did
   not affect the verdict. `s2_readback_evidence()` now returns success
   only when a qualifying packet is read. Missing evidence produces
   `H1A-S2 verdict=FAIL reason=evidence-missing`.

## Contract review

- The sendmsg carrier requires `msg_name` of exactly 32 bytes,
  `msg_iovlen=8`, `msg_control=NULL`, `msg_controllen=0`, and
  `msg_flags=0`. `iov[0]` is 0x10000, `iov[1..4]` are one byte each, and
  `iov[5..7]` are exactly `{NULL,0}`. The checked total is the correct
  arithmetic sum `0x10004`.
- The page initializes `FL` at `+0x1350`, `W0` at `+0x2220`, and `FT` at
  `+0x3200`. `FL.wait_lock=0`, `FL.owner=FT|1`, both FL roots are `W0`,
  W0 is terminal in both rb entries with priority 140, and `FT` has
  priority/normal_prio 120, `pi_top_task=FT`, and `pi_blocked_on=NULL`.
- `FT+0x420` is explicitly initialized to zero and included in the static
  contract, as required by the proxy-root audit.
- The consumer is still one-shot and gated by `s2_go`, a 150 ms delay,
  `s2_sendmsg_armed`, `s2_punch_allowed`, and a pending sendmsg result.
  It requires exactly one successful `sched_setattr` punch.
- The stale-waiter verdict still requires requeue `-EDEADLK` and victim
  `FUTEX_WAIT_REQUEUE_PI` `-ETIMEDOUT`. Page, TCP, carrier, and watchdog
  failures return before the futex gate. Gate or punch failures do not
  unlock the possibly bad PI state.
- The watchdog remains 45 s, starts after page and TCP preparation but
  before the futex gate, relocates its read fd above the protected-fd
  minimum, and treats timeout as reboot-only.

## Binary evidence

`llvm-readelf` reports an ELF64 AArch64 shared object. Dynamic imports
include `sendmsg`. `llvm-nm` reports the following relevant symbols:

```text
slide_h1a_s2_test      0x22c54
s2_waiter_thread       0x23834
s2_carrier_contract_ok 0x2407c
s2_payload_contract_ok 0x1e99c
sendmsg@plt            0x2a440
```

In `s2_waiter_thread`, r26b emits the carrier contract call at
`0x23a78`, the two release stores at `0x23af8` and `0x23afc`, and the
victim `sendmsg` call at `0x23b08`. This proves the sendmsg path was not
constant-folded or dead-stripped.

The required markers are present in the binary: header, page, tcp, gate,
armed, punch enter/first/done, sendmsg returned, readback, evidence,
watchdog, and verdict. The binary has `cfi=disabled`; it has no
`active-CFI=1`, `uid_after=0=1`, `fops hijack`, `full-chain`, or
`KS_H1A_TEST=1` marker.

Residual pselect and CFI/full-chain helper code is linked from shared
sources, but the isolated `s2_main.c` gate does not call `run_exploit`.
Disassembly of `main`, the S2 test, all S2 thread bodies, and the carrier
and payload contracts contains no `pselect@plt` call and no CFI/fops/root
call. The S2 path stays at the page/PI/readback boundary.

## Result

The final artifact satisfies the sendmsg timing, page contract, pselect
exclusion, CFI/full-chain exclusion, readback evidence, watchdog, and
fail-closed requirements for the offline S2 test. Any future device
attempt must use the hash above and the updated single-shot plan; this
document does not itself authorize that attempt.
