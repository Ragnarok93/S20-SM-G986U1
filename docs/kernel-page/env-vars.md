# `KS_*` environment variables

Classification of the variables the page experiment reads. "Verified" means the
effect was read in the source at the cited site; nothing here is removed yet.

## Supported configuration (useful, documented)

| var | effect |
| --- | --- |
| `KS_FOPS_ROUTE` | enables the post-page stage. Without it (and without `KS_PAGE_PROBE` / `KS_PLANT_PROBE`) `main` refuses on purpose: placement is unvalidated |
| `KS_PLANT_ALL` | build every planted structure (lock, waiter, task, fops table) |
| `KS_IMAGE_ZERO` | zero the whole image after building it — the identity differential |
| `KS_TRIGGER_SLABS` | trigger population size (default 24) |
| `KS_RECLAIM_SENDS`, `KS_RECLAIM_SNDBUF`, `KS_RECLAIM_WAIT_MS`, `KS_RECLAIM_DELAY_US` | image-send count and socket buffering |
| `KS_SPRAY_SOCKETS` | multi-socket variant of the image send |

## Diagnostic instrumentation (read-only reporting)

| var | effect |
| --- | --- |
| `KS_STOP_AFTER_SPRAY` | stop after preparation, leaving the device up so `/proc/slabinfo` can be read |
| `KS_TIMINGS` | export timing tables |
| `KS_PAGE_PROBE` | probe the planted page instead of the fops stage |
| `KS_MM_TEST` | stop after address recovery and report `mm`/`base` |

## Reproducibility controls (change the search, not the chain)

| var | effect |
| --- | --- |
| `KS_MM_START`, `KS_MM_END` | override the scan window |
| `KS_LINEAR_DELTA` | override the direct-map delta |
| `KS_PLANT_SKIP`, `KS_PLANT_PAD`, `KS_PLANT_SHALLOW` | plant shaping used while bisecting the fault site |
| `KS_W1_ONLY_FIRST` | stop the planted walk after the first lock |
| `KSLAB_BASE` | override `base` (debug only) |

## Other stages of the same binary (not part of the page experiment)

`KS_H1A_*`, `KS_SLIDE_TEST`, `KS_W1_TEST`, `KS_WRITE_*`, `KS_SELECT_*`,
`KS_PSELECT_FILL`, `KS_SENTINEL_LOCK` — these belong to the slide/write/root
stages that consume the page, or to older variants. They should be inventoried
with their own modules before any are removed.

## Not yet inventoried

The binary reads roughly fifty `KS_*` names in total. The list above covers the
page experiment. Removing anything not listed here requires checking the caller
first; `grep -o 'getenv("[A-Z_0-9]*")' src/*.c` is the authoritative inventory.
