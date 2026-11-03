# Staged validation

The full cycle costs 8-10 minutes (the address scan dominates) and exercises
everything at once, so a failure says little. These stages isolate the parts.
"runs now" means it is implemented and was executed in this tree on
2026-11-03.

| # | stage | command | what it establishes | status |
| --- | --- | --- | --- | --- |
| 1 | counter model | `tests/kernel_page/run.sh` | the verdict arithmetic agrees with `mm/slub.c` for every instant a cycle can produce | runs now (9 cases, seconds) |
| 2 | regression smoke | `BUDGET=78 FRESH=0 bash ~/runs/run_fast.sh` | startup, leak fork, both 32-slot bursts, `span_ok=1`, device survives | runs now (78 s) |
| 3 | anchor construction | same smoke, `KS_PRE`/`KS_SLOT`/`KS_BURST` lines | one fresh slab per burst, per-slot provenance | runs now |
| 4 | control slab | same smoke, `KS_CONTROL` line | second independent burst with the same signature | runs now |
| 5 | full cycle | `BUDGET=1500 bash ~/runs/run_fast.sh` | drain, quiesce, emptying free, gate, send, punch | runs, 8-10 min |
| 6 | address recovery | `ENVV="KS_MM_TEST=1 KS_FOPS_ROUTE=0"` full cycle | recovered `mm` accepted only if it matches this run's collision set | runs, needs a full scan |
| 7 | state machine | not implemented | stage transitions, cleanup on refusal | **gap** |

## Notes

- Stage 1 is the only one that can falsify the gate's logic without a device;
  every gate change should extend it before a cycle is spent.
- Stage 2 cannot reach the gate: the scan takes longer than the budget. A
  passing smoke says nothing about the verdict.
- A controlled test kernel would be the real next step for allocator research:
  the vendor tree is present at `~/kernel_samsung_sm8250`, but booting a
  modified kernel on this device needs an unlock and Odin, which this project
  deliberately does not do.
