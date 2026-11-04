/* identity.h — per-object slab-page identity, and what this surface allows.
 *
 * Every output line names the source of each value it prints:
 *
 *   measured     /proc/slabinfo geometry fields, the KernelSnitch object
 *                address, each slot's task state at the freeze and at its own
 *                free, and the cache-wide counter change that free made.
 *   derived      page base, in-page offset, slot index: arithmetic on the
 *                measured address with the measured geometry.  The effect
 *                label is also derived from the measured delta.
 *   unavailable  a value this surface cannot supply, printed with its reason
 *                instead of an aggregate counter or a zero.
 *
 * A derived page address keeps its source address and the geometry revision
 * it was computed with on the same row, and it never survives a failed
 * geometry check: measured fields that disagree make the row invalid instead
 * of silently producing a page from one of them.
 */
#ifndef KP_IDENTITY_H
#define KP_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/* One mm_struct slab's geometry.  /proc/slabinfo reports
 * <objsize> <objperslab> <pagesperslab> for every cache, so the span of one
 * slab is measured here rather than assumed from the payload's constants. */
struct kp_geometry {
  long objsize;        /* measured: slabinfo field 3 */
  long objs_per_slab;  /* measured: slabinfo field 4 */
  long pages_per_slab; /* measured: slabinfo field 5 */
  long span;           /* derived: pages_per_slab * page size */
  long assumed_objs_per_slab; /* the burst's own constant, for comparison */
  long assumed_span;          /* the burst's own order size, for comparison */
  int measured;        /* 1 when all three fields parsed */
  int span_agrees;     /* objsize * objs_per_slab == span */
  int assumes_agree;   /* measured fields equal the construction's constants */
};

/* One object resolved to its slab page.  addr is measured; page, offset and
 * slot are arithmetic on addr and the measured geometry.  page_exact is 0
 * when the geometry check failed, and page_reason says which check failed. */
struct kp_identity {
  uintptr_t addr;
  uintptr_t page;
  size_t offset;
  long slot;
  int constructed_slot;
  int slot_match;
  int page_exact;
  long span;          /* measured span this page was derived with */
  char geom[40];      /* the geometry revision, e.g. 1024/32/8/32768 */
  const char *page_reason;
};

enum kp_co_residency {
  KP_CO_RESIDENT = 0,
  KP_CO_SPLIT = 1,
  KP_CO_UNAVAILABLE = 2
};

/* What the table says, computable without parsing the log.  distinct_pages
 * and objects_per_page are only filled when every slot got a page; the
 * partial view is pages_observed. */
struct kp_identity_stats {
  size_t slots;
  size_t rows;
  size_t addressed;
  size_t page_known;
  size_t unresolved;
  size_t invalid_geometry;
  size_t duplicate_addresses; /* two rows ended up with one address */
  size_t duplicate_reports;   /* reports refused: address already claimed */
  size_t pages_observed;
  size_t distinct_pages;
  size_t objects_per_page;
  size_t same_page_pairs; /* unordered pairs of distinct identities (one per
                             unique address) on one page; a partial
                             observation, never a co-residency verdict */
  int complete;      /* every slot has an exact page: only then are the
                        grouping numbers a statement about the whole table */
  enum kp_co_residency co_residency;
  const char *co_reason;
};

/* Open the table for one run: records the slot count and the observation
 * window id (boot id + monotonic open time) every row carries.  Printed once
 * before the burst. */
void kp_identity_window(size_t slots);

/* Read the geometry.  On failure every field stays -1 and measured stays 0,
 * which is distinguishable from a parsed zero. */
void kp_identity_read(struct kp_geometry *g);

/* One line: the measured geometry, where it came from, whether the payload's
 * own constants agree with it, and which surface attribute is denied.
 * Printed once per run, before the burst.  Stores the measured values and
 * the construction's assumptions for the geometry check. */
void kp_identity_surface(struct kp_geometry *g, long assumed_objs_per_slab,
                         long assumed_span);

/* Resolve the one object address this run has to its slab page.  Returns 1
 * when the derived slot equals the slot the burst built.  page_exact stays 0
 * and page_reason names the failed check when the geometry cannot support a
 * page identity. */
int kp_identity_anchor(const struct kp_geometry *g, uintptr_t addr,
                       int constructed_slot, struct kp_identity *out);

/* One dump child's report: the address its own scan found, the collision count
 * that scan accepted, and the terminal state it ended in.  The report is the
 * unit the parent validates, because the collision set that verified the
 * address lives in the child and cannot be re-checked from the parent. */
struct kp_dump_report {
  uintptr_t addr;
  size_t accepted;
  int state;
};

#define KP_DUMP_REPORT_MAX 64
/* Mirrors KERNELSNITCH_MM_FOUND; util.c static-asserts the two are equal. */
#define KP_DUMP_STATE_MM_FOUND 4
/* Same bar the single-scan gate applies to its own collision set: fewer than
 * eight accepted collisions cannot tell an mm from a coincidence. */
#define KP_DUMP_MIN_ACCEPTED 8

/* Parse one report line, exactly "mm=<16 lowercase hex> accepted=<1..4 digits>
 * state=<1 digit>\n" and nothing else.  Returns 1 when every field parsed; 0
 * for a short, long, multi-line, NUL-containing or otherwise malformed line. */
int kp_dump_report_parse(const char *buf, size_t len,
                         struct kp_dump_report *out);

/* Admit one slot's report.  The row carries a page only when the child's own
 * report ended in MM_FOUND, names a non-zero address, accepted at least
 * KP_DUMP_MIN_ACCEPTED collisions, and the measured geometry resolves that
 * address.  Returns 1 and stores the address when a page was derived; 0
 * otherwise, with a static reason token in *reason_out ("no-report",
 * "truncated-report", "malformed-report", "scan-state-not-found",
 * "zero-address", "collision-set-too-small", "duplicate-address",
 * "geometry-check-failed").  A refused report never becomes a page identity;
 * only the geometry failure keeps its measured address, as a row reported but
 * excluded from every page conclusion (schema rule 6). */
int kp_dump_admit(const struct kp_geometry *g, size_t slot, const char *buf,
                  size_t len, uintptr_t *addr_out, const char **reason_out);

/* Read one slot's report from a pipe: up to cap-1 bytes, stopping at the first
 * newline, at EOF, or when timeout_ms has elapsed.  The descriptor is always
 * closed before returning, including on timeout and on a read error, so a
 * child that never answers cannot leave a pipe behind.  Returns the bytes
 * accumulated in buf (0 for no report) or -1 for a bad descriptor/argument;
 * buf is not NUL-terminated.  A partial line is returned as-is and is refused
 * by kp_dump_admit() as a truncated report. */
ssize_t kp_dump_read_report(int fd, char *buf, size_t cap, int timeout_ms);

/* Record one measured address for one slot.  This is the Cycle B route: one
 * leak dump per object, each in the child whose mm is that object.  The same
 * geometry checks as kp_identity_anchor apply, per address, and the row for
 * this slot then carries its own page.  Returns 1 when an exact page was
 * derived. */
int kp_identity_object(const struct kp_geometry *g, size_t slot,
                       uintptr_t addr);

/* Record one slot's task state at the freeze sample, before any free.  This
 * is `allocated_at_freeze`, a different temporal claim from the state read
 * immediately before that slot's own free.  st is the /proc stat state char;
 * 0 means the task was already gone. */
void kp_identity_freeze(size_t slot, char st);

/* One table row per burst slot.  state_at_sample is the task state measured
 * immediately before this row's free ("yes", "zombie", "gone", or
 * "unavailable" when no sample was taken).  free_seen is 1 when this row's
 * kill is known to have reaped the task, 0 when the task was already gone
 * before the sample, -1 when no such observation exists.  With sampled set,
 * d_* are the cache-wide counter changes that free produced; they are never
 * page identity. */
void kp_identity_slot(size_t slot, const char *state_at_sample, int free_seen,
                      int sampled, long d_active, long d_num, long d_slabs,
                      const struct kp_identity *anchor);

/* The same numbers the summary line prints. */
void kp_identity_stats(struct kp_identity_stats *out);

/* Close the table: rows against slots, pages only when complete, and the
 * reason co-residency cannot be stated otherwise. */
void kp_identity_summary(void);

#endif /* KP_IDENTITY_H */
