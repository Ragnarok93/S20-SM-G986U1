# Safe placement probe design (SP-1)

Date: 2026-09-15  
Scope: offline design and static audit only. No device command, no build, and
no `src/` change in this task.  
Kernel evidence: `k40rom/boot/kernel.elf`
(`sha256=f73a2163a0a8311ed44c731951e8e5a7a789b1b08e1931c920632a9e3098b662`),
materials tree `materials/kernelsrc/sm8250-a13-stable`, and
`build/.config`.

## Purpose

H1A-W cannot observe the victim's kernel-stack SP on alioth
(`kstkesp=0`, `/proc/<tid>/stack` is `EACCES`, and ptrace `GETREGSET` is
denied), so it cannot distinguish the two live hypotheses:

* `A`: after the H1A-D requeue gate, there is no live dangling
  `pi_blocked_on`, so the `sched_setattr` punch never walks a stale waiter.
* `B`: a live stale waiter is walked, but the pselect output is not at
  `waiter->lock`.

The existing fake-page diff cannot separate these cases because both leave
`diffs=0`. SP-1 adds a third, persistent observable: a one-word pselect marker
at the exact `waiter->lock` slot points to an otherwise empty **probe lock**.
If the PI walk reads that marker, it writes the stack-waiter pointer into
`probe_lock.waiters.rb_root`; the SKB readback can see that value. If the walk
misses the marker, the original H1A-D real PI cycle produces `EDEADLK` and the
probe lock stays empty.

## Admissible model

SP-1 only decides between placement hypotheses whose waiter base is either

```text
R = T - 0x198
```

or is not overlapped by the pselect result window at all. A hypothesis that
shifts `R` by an arbitrary sub-qword amount is **out of scope and must be
rejected offline**: writing a marker through an unknown kernel-stack offset
cannot be made fail-closed.

The known native frame is:

```text
T             = syscall wrapper entry SP
stack_fds     = T - 0x210
res_out       = T - 0x170
R             = T - 0x198
waiter.lock   = R + 0x38 = T - 0x160
```

For `nfds=320`, each fd set is 40 bytes. `res_out` word 2 is the qword at
`T - 0x160`. This is also `struct rt_mutex_waiter::lock` in the
4.19.157 layout.

## Probe lock

The probe uses a new page offset, provisionally `PROBE_LOCK_OFF=0x7000`, not
the existing `LOCK_OFF=0x1350` CFI fake lock. In every repeated SKB payload:

```text
probe_lock + 0x00  wait_lock          = 0
probe_lock + 0x08  waiters.rb_node    = 0
probe_lock + 0x10  waiters.rb_leftmost = 0
probe_lock + 0x18  owner              = 0
```

No fake task and no fops pointers are required for SP-1. `PROBE_LOCK_OFF`
must be at least 0x100 bytes away from every other payload object so the
readback cannot be confused by rb operations in another object.

## One-word marker

Let `P` be the kernel virtual address of `probe_lock` in the prepared SKB
page. The pselect input masks are all zero except:

```text
out global word 2 = P
```

Each set bit `i` is represented by a write-ready fd `128 + i`; each clear bit
is represented by a blocked read end. All 320 fds remain open, and fds 0, 1,
and 2 are **not** restored over the encoded blocked/ready types. This avoids
the stdio-restore failure identified in the earlier pselect audit.

`do_select()` zeroes all result regions, scans words in order, writes
`res_out[2] = P`, and stops after that word. Therefore the only qword written
inside the waiter span is:

```text
*(uint64_t *)(R + 0x38) = P
```

If `R` is the stale-waiter base, this replaces `waiter->lock` with the probe
lock. If `R` is not the stale-waiter base, the write stays in dead pselect
stack scratch and the original `waiter->lock` remains intact.

The pselect return count must equal `popcount64(P)`. A different count is a
shape failure; the consumer must not issue `sched_setattr`.

## Chain algebra

The H1A-D gate establishes a stale `pi_blocked_on` whose original lock is in
the victim/owner real PI cycle. Define:

```text
E  = sched_setattr returns -1 / EDEADLK
S  = sched_setattr returns 0
X  = post-punch readback has probe_lock.waiters.rb_node != 0
O  = post-punch readback has probe_lock.waiters.rb_node == 0
```

The expected truth table is:

| State at punch | Marker read? | Result | Probe root | Verdict |
|---|---:|---|---:|---|
| No live stale `pi_blocked_on` | no | `S` | 0 | `NO-LIVE-DANGLING` |
| Stale waiter, marker missed | no | `E` | 0 | `WALKED-WRONG-PLACEMENT` |
| Stale waiter, marker read | yes | `S` | nonzero | `PLACEMENT-CONFIRMED` |

`X` is not merely "the spinlock was nonzero": `raw_spin_unlock()` may clear
`wait_lock`, but `rt_mutex_enqueue()` leaves the waiter pointer in
`probe_lock.waiters.rb_root`.

For the marker-read path, the probe lock has `owner == 0`. The PI chain:

1. succeeds in `raw_spin_trylock(&probe_lock->wait_lock)`;
2. writes the waiter into the empty rb tree;
3. sees `rt_mutex_owner(probe_lock) == NULL`;
4. releases the wait lock and returns 0.

It never follows a task pointer and never reaches CFI. If the marker is
missed, the untouched real PI cycle is walked and returns `EDEADLK`.

## Safety argument

* The probe performs exactly one `sched_setattr` per victim. It never enters
  CFI, physical read/write, fd-table rewrite, or the full root chain.
* The probe lock has an empty rb tree and null owner. A one-node
  enqueue/dequeue cannot walk an external parent chain. The tree write stays
  inside the controlled probe lock.
* A missed marker leaves the real H1A-D cycle intact. Its chain exits through
  normal deadlock detection rather than requeueing a partially shaped node.
* A shape failure (`pselect` return mismatch), requeue-gate failure, page
  preparation failure, or pre-readback failure suppresses the punch.
* Every device run is one-shot per boot. The default outcome after a watchdog,
  unexpected reboot, panic, residue process, or unreadable readback is
  `SP-UNSAFE-REJECT`; no positive placement claim may be derived from it.
* A controlled reboot is mandatory at the end of every device execution. The
  dangling-waiter state is not reused for another probe or cleanup experiment.

The only remaining unavoidable failure is a fault caused by an unadmitted
placement model (for example a sub-qword shift that makes the marker land in a
live non-waiter field). Such an event is classified `SP-UNSAFE-REJECT`, not as
evidence for either hypothesis.

## Logging contract

Use stable `SP:` markers. No marker may be emitted before its precondition is
complete. The log lines below are minimum required fields.

```text
SP config kernel_sha256=... elf_probe_offsets=R=-0x198,lock=-0x160
SP gate requeue_ret=-1 requeue_errno=35 verdict=PASS
SP gate requeue_ret=<n> requeue_errno=<n> verdict=FAIL_NO_PUNCH
SP page base=<kernel_va> probe_lock=<kernel_va> payload_sha256=...
SP prefill probe_root=0 wait_lock=0 skb_reads=<n> verdict=PASS
SP shape nfds=320 expected_ret=<popcount(P)> marker_word=<P>
SP punch enter tid=<tid>
SP punch returned ret=<n> errno=<n>
SP readback probe_root=<hex> wait_lock=<hex> diff_count=<n> skb_reads=<n>
SP verdict=<name>
SP finish action=controlled-reboot
```

`marker_word` may be logged after the punch; it must not cause a syscall in
the victim between pselect return and punch completion.

## Verdict decision procedure

1. If the H1A-D requeue gate is not exactly `ret=-1 errno=EDEADLK`, emit
   `SP-BLOCKED-GATE` and do not prepare a punch.
2. If page preparation, prefill, fd shaping, or the pselect return-count gate
   fails, emit `SP-BLOCKED-SHAPE` and do not punch.
3. After exactly one punch:
   * `ret=0`, `probe_root != 0` => `PLACEMENT-CONFIRMED`.
   * `ret=0`, `probe_root == 0` => `NO-LIVE-DANGLING`.
   * `ret=-1/EDEADLK`, `probe_root == 0` => `WALKED-WRONG-PLACEMENT`.
   * any other combination => `SP-AMBIGUOUS-REJECT`.
4. If the device panicked, rebooted unexpectedly, or the watchdog fired,
   emit `SP-UNSAFE-REJECT`. This overrides every other verdict.
5. If fewer than all planned SKB readbacks are received, emit
   `SP-INDETERMINATE-FAIL-CLOSED`. Do not backfill `walk_touched`, mm answers,
   or placement claims.

## Falsification conditions

SP-1 is disproved if any of the following occurs in a clean single-shot run:

* the marker-read path produces `probe_root == 0`;
* the marker-read path panics or spins at the empty-owner probe lock;
* a marker-missed run returns something other than `EDEADLK` without touching
  `probe_lock`;
* a no-live-dangling run writes `probe_lock.waiters.rb_root`;
* the same binary produces two mutually exclusive verdicts without an
  identifiable gate, fd, or kernel-build difference.

Any of these requires a new offline audit before another device run. It never
permits advancing to CFI or the full chain.

## Static audit checklist before implementation

1. Recompute `T`, `stack_fds`, `res_out`, and stale-waiter offsets from the
   exact target ELF; the allowed kernel hash is recorded above.
2. Confirm `struct rt_mutex_waiter::lock` is `+0x38`, `struct rt_mutex` has
   `wait_lock=+0x00`, `waiters=+0x08`, and `owner=+0x18` without
   `CONFIG_DEBUG_RT_MUTEXES`, `CONFIG_DEBUG_LOCK_ALLOC`, or
   `CONFIG_PROVE_LOCKING`.
3. Confirm `do_select()` zeroes the result regions, scans words in increasing
   order, and breaks after the first ready word.
4. Confirm the marker input word and fd classes produce exactly
   `res_out[2] = P`; no stdio fd is restored over fd 0..319.
5. Confirm `raw_spin_trylock()` on a zero spinlock is non-blocking and that
   `rt_mutex_enqueue()` writes `waiters.rb_root` before the owner-null exit.
6. Confirm `sched_setattr` reaches `rt_mutex_adjust_pi()` for the chosen
   policy/nice and that the H1A-D gate still returns `EDEADLK` on this build.
7. Confirm the SKB readback reads all planned copies and records the exact
   `probe_lock + 0x08` qword, not only a whole-buffer diff count.

No implementation is approved by this document. The implementation must be
audited against this checklist before any device schedule is created.
