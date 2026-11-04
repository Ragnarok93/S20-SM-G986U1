/* identity.c — per-object slab-page identity, as far as this surface allows.
 *
 * Sources, and what each line says they are:
 *
 *   measured      the mm_struct geometry from /proc/slabinfo (objsize,
 *                 objperslab, pagesperslab), the one object address the
 *                 KernelSnitch leak recovers, each slot's task state at the
 *                 freeze and immediately before its own free, and the
 *                 cache-wide counter change that free made.
 *
 *   derived       page base, in-page offset, slot index: arithmetic on the
 *                 measured address with the measured geometry.  The effect
 *                 label is also derived from the measured delta.
 *
 *   unavailable   an identity this surface cannot supply, printed with the
 *                 reason and never replaced by an aggregate counter or zero.
 *
 * Two limits are structural, not temporary:
 *
 * 1. One address per run.  The leak walks candidate objects inside one
 *    process's mm and returns the first whose futex hashes agree with that
 *    process's own collision set (src/kernelsnitch/kernelsnitch.h:283-300),
 *    so one scan yields the address of the scanning child and nothing else.
 *    Addressing the other burst slots would need one scan each.
 *
 * 2. No per-page occupancy.  /proc/slabinfo aggregates over the whole cache
 *    and every per-cache attribute under /sys/kernel/slab is root-only on
 *    this device (probed in kp_identity_surface()).  A free on a page that is
 *    not on n->partial moves no counter at all, so a zero delta means "this
 *    free was not visible", never "this object was elsewhere".
 *
 * A derived page address is never printed without the address it came from,
 * the geometry revision used, and its provenance; when the measured geometry
 * disagrees with itself or with the burst's own constants the row is
 * invalid instead of producing a page from the half that parses.
 */
#include "common.h"
#include "identity.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KP_ID_MAX_SLOTS 64

struct kp_id_row {
  int used;
  size_t slot;
  int freeze_live;   /* 1 yes, 0 no, -1 unknown */
  int sample_live;   /* 1 yes, 0 no, -1 unavailable */
  int free_seen;     /* 1 observed, 0 not-in-window, -1 unavailable */
  int sampled;       /* slabinfo delta valid */
  long d_active;
  long d_num;
  long d_slabs;
  uintptr_t addr;
  int has_addr;
  int addr_unique;   /* 1 unique, 0 repeated, -1 none */
  uintptr_t page;
  int page_exact;
  int slot_match;
  const char *page_reason;
  size_t offset;
  long slot_in_page;
  long span;
  char geom[40];
};

/* Append to a bounded line buffer.  Returns the new offset; the line is never
 * left unterminated, and a truncated row is visibly truncated rather than
 * spilling into the logger's next write. */
static size_t append(char *buf, size_t cap, size_t off, const char *fmt, ...) {
  if (off >= cap) {
    return cap;
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + off, cap - off, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return off;
  }
  return off + (size_t)n < cap ? off + (size_t)n : cap - 1;
}

static struct kp_id_row g_rows[KP_ID_MAX_SLOTS];
static struct kp_identity g_obj[KP_ID_MAX_SLOTS];
static unsigned char g_obj_set[KP_ID_MAX_SLOTS];
static size_t g_nrows;
static size_t g_slots;
static char g_window[48] = "unopened";
static struct kp_geometry g_geom;
static int g_geom_known;
static signed char g_freeze[KP_ID_MAX_SLOTS];
static unsigned char g_freeze_set[KP_ID_MAX_SLOTS];

static void boot_prefix(char out[9]) {
  memcpy(out, "noboot", 7);
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  char buf[37];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 8) {
    return;
  }
  for (int i = 0; i < 8; i++) {
    char c = buf[i];
    int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F');
    out[i] = hex ? c : 'x';
  }
  out[8] = 0;
}

void kp_identity_window(size_t slots) {
  memset(g_rows, 0, sizeof(g_rows));
  memset(g_obj, 0, sizeof(g_obj));
  memset(g_obj_set, 0, sizeof(g_obj_set));
  g_nrows = 0;
  g_slots = slots;
  memset(g_freeze_set, 0, sizeof(g_freeze_set));
  memset(g_freeze, 0, sizeof(g_freeze));
  struct timespec ts;
  unsigned long long ms = 0;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    ms = (unsigned long long)ts.tv_sec * 1000ULL +
         (unsigned long long)(ts.tv_nsec / 1000000L);
  }
  char boot[9];
  boot_prefix(boot);
  snprintf(g_window, sizeof(g_window), "%s-%llu", boot, ms);
  pr_info("KS_ID_WINDOW window=%s boot=%s opened_mono_ms=%llu slots=%zu "
          "scope=cycle window_src=measured(boot_id,clock_monotonic)\n",
          g_window, boot, ms, slots);
}

void kp_identity_read(struct kp_geometry *g) {
  g->objsize = -1;
  g->objs_per_slab = -1;
  g->pages_per_slab = -1;
  g->span = -1;
  g->assumed_objs_per_slab = -1;
  g->assumed_span = -1;
  g->measured = 0;
  g->span_agrees = 0;
  g->assumes_agree = 0;
  FILE *f = fopen("/proc/slabinfo", "r");
  if (f == NULL) {
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "mm_struct ", 10) != 0) {
      continue;
    }
    char name[64];
    long a = 0, o = 0, osz = -1, ops = -1, pps = -1;
    long t1 = 0, t2 = 0, t3 = 0, as = 0, ns = 0, sh = 0;
    long pg = (long)getpagesize();
    /* Both products below are computed in long: bound them at parse time so a
     * crafted slabinfo line cannot overflow the span or the agreement test. */
    if (sscanf(line,
               "%63s %ld %ld %ld %ld %ld : tunables %ld %ld %ld "
               ": slabdata %ld %ld %ld",
               name, &a, &o, &osz, &ops, &pps, &t1, &t2, &t3, &as, &ns,
               &sh) >= 11 && osz > 0 && ops > 0 && pps > 0 &&
        pg > 0 && pps <= LONG_MAX / pg && ops <= LONG_MAX / osz) {
      g->objsize = osz;
      g->objs_per_slab = ops;
      g->pages_per_slab = pps;
      g->span = pg * pps;
      g->measured = 1;
      g->span_agrees = (osz * ops == g->span);
    }
    break;
  }
  fclose(f);
}

/* One probe is enough for the class: every per-cache attribute under
 * /sys/kernel/slab/mm_struct is gated the same way on this device. */
static int sysfs_probe(const char *name, int *err) {
  char path[128];
  snprintf(path, sizeof(path), "/sys/kernel/slab/mm_struct/%s", name);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    close(fd);
    return 0;
  }
  if (err) {
    *err = errno;
  }
  return 1;
}

static void geom_desc(const struct kp_geometry *g, char *buf, size_t n) {
  if (g == NULL || !g->measured) {
    snprintf(buf, n, "unavailable");
    return;
  }
  snprintf(buf, n, "%ld/%ld/%ld/%ld", g->objsize, g->objs_per_slab,
           g->pages_per_slab, g->span);
}

void kp_identity_surface(struct kp_geometry *g, long assumed_objs_per_slab,
                         long assumed_span) {
  g->assumed_objs_per_slab = assumed_objs_per_slab;
  g->assumed_span = assumed_span;
  g->assumes_agree = (g->measured &&
                      g->objs_per_slab == assumed_objs_per_slab &&
                      g->span == assumed_span)
                         ? 1
                         : 0;
  g_geom = *g;
  g_geom_known = 1;
  char geom[64];
  geom_desc(g, geom, sizeof(geom));
  int err = 0;
  int denied = sysfs_probe("object_size", &err);
  pr_info("KS_SURFACE geometry=%s objsize=%ld objs_per_slab=%ld "
          "pages_per_slab=%ld span=%ld span_src=%s span_agrees=%d "
          "assumed_objs_per_slab=%ld assumed_span=%ld assumes_agree=%d "
          "sysfs=%s%s errno=%d per_object_address=unavailable "
          "reason=one-leak-one-address geometry=%s\n",
          g->measured ? "measured" : "unavailable", g->objsize,
          g->objs_per_slab, g->pages_per_slab, g->span,
          g->measured ? "derived(measured-pagesperslab*page)" : "none",
          g->span_agrees, assumed_objs_per_slab, assumed_span,
          g->assumes_agree, denied ? "denied" : "readable",
          denied ? "" : "(unexpected)", err, geom);
}

/* Resolve one measured address against one geometry.  Returns NULL on
 * success, or the token of the check that failed.  Prints nothing: the
 * caller decides whether this is the anchor's line or a per-slot line. */
static const char *resolve(const struct kp_geometry *g, uintptr_t addr,
                           int constructed_slot, struct kp_identity *out) {
  char geom[64];
  memset(out, 0, sizeof(*out));
  out->addr = addr;
  out->constructed_slot = constructed_slot;
  geom_desc(g, geom, sizeof(geom));
  snprintf(out->geom, sizeof(out->geom), "%.*s",
           (int)(sizeof(out->geom) - 1), geom);
  const char *reason = NULL;
  if (!g->measured || g->span <= 0 || g->objsize <= 0 ||
      (unsigned long)g->span > (unsigned long)UINTPTR_MAX) {
    reason = "no-slab-geometry";
  } else if ((g->span & (g->span - 1)) != 0) {
    /* page = addr & ~(span - 1) is exact only when span is a power of two:
     * a slab of, say, 3 pages has no single mask that yields its base.  The
     * device reports 8 pages (order 3), whose compound page is naturally
     * aligned to 32768, but the table must not assume that of another
     * geometry it did not measure. */
    reason = "non-power-of-two-span";
  } else if (!g->span_agrees) {
    reason = "geometry-self-inconsistent";
  } else if (!g->assumes_agree) {
    reason = "construction-geometry-mismatch";
  }
  if (reason != NULL) {
    out->page_reason = reason;
    return reason;
  }
  out->page = addr & ~((uintptr_t)g->span - 1);
  out->offset = (size_t)(addr & ((uintptr_t)g->span - 1));
  out->slot = (long)(out->offset / (size_t)g->objsize);
  out->slot_match = (out->slot == constructed_slot);
  out->page_exact = 1;
  out->span = g->span;
  return NULL;
}

int kp_identity_anchor(const struct kp_geometry *g, uintptr_t addr,
                       int constructed_slot, struct kp_identity *out) {
  char geom[64];
  const char *reason = resolve(g, addr, constructed_slot, out);
  geom_desc(g, geom, sizeof(geom));
  if (reason != NULL) {
    pr_info("KS_OBJ_ID window=%s addr=%016zx addr_src=measured(leak) "
            "addr_unique=unique geom=%s geom_src=%s page=invalid(%s) "
            "page_src=none constructed_slot=%d match=unavailable "
            "page_identity=invalid(%s)\n",
            g_window, (size_t)addr, geom,
            g->measured ? "measured(slabinfo)" : "none", reason,
            constructed_slot, reason);
    return 0;
  }
  pr_info("KS_OBJ_ID window=%s addr=%016zx addr_src=measured(leak) "
          "addr_unique=unique geom=%s geom_src=measured(slabinfo) "
          "page=%016zx page_src=derived(measured-span=%ld) offset=%zu "
          "slot=%ld slot_src=derived constructed_slot=%d match=%d "
          "page_identity=exact\n",
          g_window, (size_t)addr, geom, (size_t)out->page, g->span,
          out->offset, out->slot, constructed_slot, out->slot_match);
  return out->slot_match;
}

int kp_identity_object(const struct kp_geometry *g, size_t slot,
                       uintptr_t addr) {
  if (slot >= KP_ID_MAX_SLOTS) {
    return 0;
  }
  struct kp_identity id;
  const char *reason = resolve(g, addr, (int)slot, &id);
  g_obj[slot] = id;
  g_obj_set[slot] = 1;
  if (reason != NULL) {
    pr_info("KS_OBJ_ADDR window=%s slot=%zu addr=%016zx "
            "addr_src=measured(leak) geom=%s page=invalid(%s) page_src=none "
            "page_identity=invalid(%s)\n",
            g_window, slot, (size_t)addr, id.geom, reason, reason);
    return 0;
  }
  pr_info("KS_OBJ_ADDR window=%s slot=%zu addr=%016zx "
          "addr_src=measured(leak) geom=%s page=%016zx "
          "page_src=derived(measured-span=%ld) offset=%zu slot_in_page=%ld "
          "match=%d page_identity=exact\n",
          g_window, slot, (size_t)addr, id.geom, (size_t)id.page, id.span,
          id.offset, id.slot, id.slot_match);
  return 1;
}

void kp_identity_freeze(size_t slot, char st) {
  if (slot >= KP_ID_MAX_SLOTS) {
    return;
  }
  g_freeze_set[slot] = 1;
  g_freeze[slot] = (st == 0 || st == 'Z' || st == 'X') ? 0 : 1;
}

static struct kp_id_row *row_for(size_t slot) {
  for (size_t i = 0; i < g_nrows; i++) {
    if (g_rows[i].slot == slot) {
      return &g_rows[i];
    }
  }
  if (g_nrows >= KP_ID_MAX_SLOTS) {
    return NULL;
  }
  struct kp_id_row *r = &g_rows[g_nrows++];
  memset(r, 0, sizeof(*r));
  r->used = 1;
  r->slot = slot;
  r->addr_unique = -1;
  r->slot_in_page = -1;
  return r;
}

void kp_identity_slot(size_t slot, const char *state_at_sample, int free_seen,
                      int sampled, long d_active, long d_num, long d_slabs,
                      const struct kp_identity *anchor) {
  if (slot >= KP_ID_MAX_SLOTS) {
    return;
  }
  struct kp_id_row *r = row_for(slot);
  if (r == NULL) {
    return;
  }
  int scan = anchor != NULL && (long)slot == anchor->constructed_slot;
  const struct kp_identity *own = NULL;
  if (g_obj_set[slot]) {
    own = &g_obj[slot];
  } else if (scan && anchor != NULL) {
    own = anchor;
  }
  r->freeze_live = g_freeze_set[slot] ? g_freeze[slot] : -1;
  if (state_at_sample == NULL) {
    r->sample_live = -1;
  } else if (strcmp(state_at_sample, "yes") == 0) {
    r->sample_live = 1;
  } else if (strcmp(state_at_sample, "zombie") == 0 ||
             strcmp(state_at_sample, "gone") == 0) {
    r->sample_live = 0;
  } else {
    r->sample_live = -1;
  }
  r->free_seen = free_seen;
  r->sampled = sampled;
  r->d_active = d_active;
  r->d_num = d_num;
  r->d_slabs = d_slabs;
  if (own != NULL && own->addr != 0) {
    r->addr = own->addr;
    r->has_addr = 1;
    r->page = own->page;
    r->page_exact = own->page_exact;
    r->slot_match = own->slot_match;
    r->page_reason = own->page_reason;
    r->offset = own->offset;
    r->slot_in_page = own->slot;
    r->span = own->span;
    snprintf(r->geom, sizeof(r->geom), "%.*s",
             (int)(sizeof(r->geom) - 1), own->geom);
    int uniq = 1;
    /* Per-slot addresses are all known before the first row is written, so a
     * duplicate is marked on both rows, not only on the later one. */
    for (size_t s = 0; s < g_slots && s < KP_ID_MAX_SLOTS; s++) {
      if (s != slot && g_obj_set[s] && g_obj[s].addr == r->addr) {
        uniq = 0;
      }
    }
    for (size_t i = 0; i < g_nrows; i++) {
      if (&g_rows[i] != r && g_rows[i].has_addr &&
          g_rows[i].addr == r->addr) {
        uniq = 0;
        g_rows[i].addr_unique = 0;
      }
    }
    r->addr_unique = uniq;
  }
  const char *role = scan ? "scan"
                          : (slot + 1 == g_slots ? "target" : "filler");
  char geom_buf[64];
  geom_desc(g_geom_known ? &g_geom : NULL, geom_buf, sizeof(geom_buf));
  const char *geom = r->has_addr && r->geom[0] ? r->geom : geom_buf;
  const char *freeze = r->freeze_live == 1   ? "yes"
                       : r->freeze_live == 0 ? "no"
                                             : "unknown";
  const char *sample = r->sample_live == 1   ? "yes"
                       : r->sample_live == 0 ? "no"
                                             : "unavailable";
  const char *free = free_seen == 1   ? "yes"
                     : free_seen == 0 ? "not-in-window"
                                      : "unavailable";
  int constructed = anchor != NULL ? anchor->constructed_slot : -1;

  /* One row is one log line: compose it here and write it once, so no second
   * write can land the logger's prefix in the middle of the fields. */
  char line[1024];
  size_t off = 0;
  off = append(line, sizeof(line), off,
               "KS_ID_ROW window=%s slot=%zu role=%s allocated_at_freeze=%s "
               "allocated_at_sample=%s free_observed=%s",
               g_window, slot, role, freeze, sample, free);
  if (!r->has_addr) {
    off = append(line, sizeof(line), off,
                 " addr=unavailable(no-per-object-address) addr_src=none "
                 "addr_unique=none geom=%s page=unresolved(no-object-address) "
                 "page_src=none offset=unavailable(no-object-address) "
                 "slot_in_page=unavailable(no-object-address) slot_src=none "
                 "constructed_slot=%d match=unavailable "
                 "page_identity=unresolved(no-object-address)",
                 geom, constructed);
  } else if (!r->page_exact) {
    off = append(line, sizeof(line), off,
                 " addr=%016zx addr_src=measured(leak) addr_unique=%s "
                 "geom=%s page=invalid(%s) page_src=none "
                 "offset=unavailable(geometry-check-failed) "
                 "slot_in_page=unavailable(geometry-check-failed) "
                 "slot_src=none constructed_slot=%d match=unavailable "
                 "page_identity=invalid(%s)",
                 (size_t)r->addr, r->addr_unique ? "unique" : "repeated",
                 geom, r->page_reason ? r->page_reason : "geometry-check-failed",
                 constructed,
                 r->page_reason ? r->page_reason : "geometry-check-failed");
  } else {
    off = append(line, sizeof(line), off,
                 " addr=%016zx addr_src=measured(leak) addr_unique=%s "
                 "geom=%s page=%016zx page_src=derived(measured-span=%ld) "
                 "offset=%zu slot_in_page=%ld slot_src=derived "
                 "constructed_slot=%d match=%d page_identity=exact",
                 (size_t)r->addr, r->addr_unique ? "unique" : "repeated",
                 geom, (size_t)r->page, r->span, r->offset, r->slot_in_page,
                 constructed, r->slot_match);
  }
  if (sampled) {
    (void)append(line, sizeof(line), off,
                 " d_active=%ld d_num=%ld d_slabs=%ld d_src=measured "
                 "delta_attribution=consistent-with",
                 d_active, d_num, d_slabs);
  } else {
    (void)append(line, sizeof(line), off,
                 " d_active=unavailable d_num=unavailable d_slabs=unavailable "
                 "d_src=unavailable delta_attribution=unavailable");
  }
  pr_info("%s\n", line);
}

void kp_identity_stats(struct kp_identity_stats *out) {
  memset(out, 0, sizeof(*out));
  out->slots = g_slots;
  out->rows = g_nrows;
  uintptr_t pages[KP_ID_MAX_SLOTS];
  size_t counts[KP_ID_MAX_SLOTS];
  size_t npages = 0;

  for (size_t i = 0; i < g_nrows; i++) {
    const struct kp_id_row *r = &g_rows[i];
    if (r->has_addr) {
      out->addressed++;
      for (size_t j = 0; j < i; j++) {
        if (g_rows[j].has_addr && g_rows[j].addr == r->addr) {
          out->duplicate_addresses++;
          break;
        }
      }
    }
    if (r->page_exact) {
      out->page_known++;
      size_t k = 0;
      for (; k < npages; k++) {
        if (pages[k] == r->page) {
          counts[k]++;
          break;
        }
      }
      if (k == npages && npages < KP_ID_MAX_SLOTS) {
        pages[npages] = r->page;
        counts[npages] = 1;
        npages++;
      }
    } else if (r->has_addr) {
      out->invalid_geometry++;
    }
  }
  out->unresolved = g_slots >= out->page_known ? g_slots - out->page_known : 0;
  out->pages_observed = npages;
  out->complete = (out->slots > 0 && out->rows == out->slots &&
                   out->page_known == out->slots);
  if (out->complete) {
    /* Grouping numbers exist only for the whole table: an interrupted scan
     * must not leave a page count that reads like a result. */
    out->distinct_pages = npages;
    for (size_t k = 0; k < npages; k++) {
      if (counts[k] > out->objects_per_page) {
        out->objects_per_page = counts[k];
      }
    }
  }
  if (out->rows < out->slots) {
    out->co_residency = KP_CO_UNAVAILABLE;
    out->co_reason = "rows-missing";
  } else if (out->page_known < 2) {
    out->co_residency = KP_CO_UNAVAILABLE;
    out->co_reason = "addressed-too-few";
  } else if (out->page_known < out->slots) {
    out->co_residency = KP_CO_UNAVAILABLE;
    out->co_reason = "unresolved-rows";
  } else if (npages == 1) {
    out->co_residency = KP_CO_RESIDENT;
    out->co_reason = "one-page";
  } else {
    out->co_residency = KP_CO_SPLIT;
    out->co_reason = "multiple-pages";
  }
}

void kp_identity_summary(void) {
  struct kp_identity_stats s;
  char geom[64];
  char pages_buf[64];
  char perpage_buf[64];
  char co_buf[80];
  kp_identity_stats(&s);
  geom_desc(g_geom_known ? &g_geom : NULL, geom, sizeof(geom));
  if (s.complete) {
    snprintf(pages_buf, sizeof(pages_buf), "%zu", s.distinct_pages);
    snprintf(perpage_buf, sizeof(perpage_buf), "%zu", s.objects_per_page);
  } else {
    snprintf(pages_buf, sizeof(pages_buf), "unavailable(pages=%zu-of-%zu)",
             s.page_known, s.slots);
    snprintf(perpage_buf, sizeof(perpage_buf), "unavailable(incomplete)");
  }
  if (s.co_residency == KP_CO_RESIDENT) {
    snprintf(co_buf, sizeof(co_buf), "co-resident(%zu-rows-one-page)",
             s.page_known);
  } else if (s.co_residency == KP_CO_SPLIT) {
    snprintf(co_buf, sizeof(co_buf), "split(pages=%zu)", s.pages_observed);
  } else {
    snprintf(co_buf, sizeof(co_buf), "unavailable(reason=%s)",
             s.co_reason ? s.co_reason : "unknown");
  }
  pr_info("KS_ID_SUMMARY window=%s slots=%zu rows=%zu addressed=%zu "
          "page_known=%zu unresolved=%zu invalid_geometry=%zu "
          "duplicate_addresses=%zu pages_observed=%zu distinct_pages=%s "
          "objects_per_page=%s co_residency=%s geom=%s geom_src=%s "
          "occupancy=unavailable(no-page-resolved-source)\n",
          g_window, s.slots, s.rows, s.addressed, s.page_known, s.unresolved,
          s.invalid_geometry, s.duplicate_addresses, s.pages_observed,
          pages_buf, perpage_buf, co_buf, geom,
          g_geom_known && g_geom.measured ? "measured(slabinfo)" : "none");
}
