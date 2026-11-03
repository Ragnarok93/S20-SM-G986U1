# `KS_*` environment variables

Inventory produced by reading every `getenv("KS_…")` in `src/` (64 names) and
comparing it with what was documented. No documented name is unread, so nothing
was removed; the 39 undocumented names are listed here for the first time. No
variable was deleted, because static reachability of the stage selectors below
has not been established — and a control that looks dead is still evidence
about how a past experiment was run.

## Page experiment: supported configuration

| var | effect |
| --- | --- |
| `KS_FOPS_ROUTE` | enables the post-page stage; without it `main` refuses on purpose |
| `KS_PLANT_ALL` | build every planted structure |
| `KS_IMAGE_ZERO` | zero the whole image — the placement differential |
| `KS_TRIGGER_SLABS` | trigger population size (default 24) |
| `KS_SPRAY_SOCKETS` | multi-socket variant of the image send |
| `KS_RECLAIM_SENDS`, `KS_RECLAIM_SNDBUF`, `KS_RECLAIM_WAIT_MS`, `KS_RECLAIM_DELAY_US`, `KS_RECLAIM_WAIT_FREES` | image-send count and socket buffering |

## Page experiment: diagnostics

| var | effect |
| --- | --- |
| `KS_STOP_AFTER_SPRAY` | stop after preparation, leave the device up for `/proc/slabinfo` |
| `KS_TIMINGS` | export timing tables |
| `KS_PAGE_PROBE`, `KS_PLANT_PROBE` | probe the planted page instead of the fops stage |
| `KS_MM_TEST` | stop after address recovery and report `mm`/`base` |
| `KS_LOG_POST_DRAIN` | extra logging around the drain |
| `KS_PLANT_SKIP`, `KS_PLANT_PAD`, `KS_PLANT_SHALLOW` | plant shaping used while bisecting the fault site |

## Address recovery (KernelSnitch)

| var | effect |
| --- | --- |
| `KS_MM_START`, `KS_MM_END` | override the scan window |
| `KS_LINEAR_DELTA` | override the direct-map delta |
| `KS_CACHE_PATH`, `KS_CACHE_RADIUS_GB` | saved-answer file and its validity radius |
| `KS_PC_WAIT`, `KS_PC_POLL_MS` | how long to wait for an answer file and how often to poll |
| `KS_MEASURE_REPS`, `KS_MMLOOP_MODE`, `KS_MM_COLLECT_ONLY`, `KS_PILE2_BASELINE`, `KS_TOTAL_MULT`, `KS_TRUNCATE_PARTIAL` | measurement shape and reduction used by the scan |
| `KS_KASLR_BASE` | override the image base |
| `KS_RUN33_VALIDATION` | validation-mode selector for the recovery run |

## Other stages of the same binary (not the page experiment)

`KS_H1A_TEST`, `KS_H1A_S2_TEST`, `KS_H1A_D_TEST`, `KS_H1A_W_TEST`,
`KS_SLIDE_TEST`, `KS_SELECT_INLINE`, `KS_SELECT_CARPET`,
`KS_SELECT_RAW_FILL`, `KS_SELECT_RAW_TEXT`, `KS_PSELECT_FILL`,
`KS_SENTINEL_LOCK`, `KS_W1_TEST`, `KS_W1_ONLY_FIRST`, `KS_WRITE_VIA`,
`KS_WRITE_OFF`, `KS_WRITE_VAL`, `KS_WRITE_VERIFY`, `KS_BUCKET_ANSWER`,
`KS_BUCKET_FILE` — slide/select/write/closed-loop selectors. They are read by
the same payload but are not part of the page experiment and were not audited
for reachability here.

## Plant shaping (diagnostic, shares names with the page stage)

`KS_PLANT_FILL`, `KS_PLANT_CLEAN`, `KS_PLANT_ZERO`, `KS_PLANT_CARPET`,
`KS_PLANT_PAGE_BASE`, `KS_PLANT_PRIO`, `KS_DUMP_WAITER`, `KS_HOLD_POST`,
`KS_FREE_BURST`, `KS_BUDDY_HOLD` — alter how the planted page or the burst is
built. Changing any of them changes the experiment, so they are not "safe"
knobs even though they are diagnostic in intent.

## What the audit did not establish

1. Which of the 39 undocumented controls are still reachable in a current run.
2. Whether any control changes an observation boundary rather than shaping
   data. Those must not be touched during a validation-only change.
