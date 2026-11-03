/* identity.c — per-object slab-page identity, as far as this surface allows.
 *
 * Sources, and what each line says they are:
 *
 *   measured      the mm_struct geometry from /proc/slabinfo (objsize,
 *                 objperslab, pagesperslab), the one object address the
 *                 KernelSnitch leak recovers, each slot's task state before
 *                 its free, and the cache-wide counter change that free made.
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
 */
#include "common.h"
#include "identity.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Probe one per-cache sysfs attribute.  They are all gated the same way, so
 * one probe is enough to report the class; returning the errno keeps the line
 * checkable instead of asserting "denied". */
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

void kp_identity_read(struct kp_geometry *g) {
  g->objsize = -1;
  g->objs_per_slab = -1;
  g->pages_per_slab = -1;
  g->span = -1;
  g->measured = 0;
  g->span_agrees = 0;
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
    if (sscanf(line,
               "%63s %ld %ld %ld %ld %ld : tunables %ld %ld %ld "
               ": slabdata %ld %ld %ld",
               name, &a, &o, &osz, &ops, &pps, &t1, &t2, &t3, &as, &ns,
               &sh) >= 11 && osz > 0 && ops > 0 && pps > 0) {
      g->objsize = osz;
      g->objs_per_slab = ops;
      g->pages_per_slab = pps;
      g->span = (long)getpagesize() * pps;
      g->measured = 1;
      g->span_agrees = (osz * ops == g->span);
    }
    break;
  }
  fclose(f);
}

void kp_identity_surface(const struct kp_geometry *g, long assumed_objs_per_slab,
                         long assumed_span) {
  int err = 0;
  int denied = sysfs_probe("object_size", &err);
  pr_info("KS_SURFACE geometry=%s objsize=%ld objs_per_slab=%ld "
          "pages_per_slab=%ld span=%ld span_src=%s span_agrees=%d "
          "assumed_objs_per_slab=%ld assumed_span=%ld assumes_agree=%d "
          "sysfs=%s%s errno=%d per_object_address=unavailable "
          "reason=one-leak-one-address\n",
          g->measured ? "measured" : "unavailable", g->objsize,
          g->objs_per_slab, g->pages_per_slab, g->span,
          g->measured ? "derived(measured-pagesperslab*page)" : "none",
          g->span_agrees, assumed_objs_per_slab, assumed_span,
          (g->measured && g->objs_per_slab == assumed_objs_per_slab &&
           g->span == assumed_span)
              ? 1
              : 0,
          denied ? "denied" : "readable",
          denied ? "" : "(unexpected)", err);
}

int kp_identity_anchor(const struct kp_geometry *g, uintptr_t addr,
                       int constructed_slot, struct kp_identity *out) {
  memset(out, 0, sizeof(*out));
  out->addr = addr;
  out->constructed_slot = constructed_slot;
  if (!g->measured || g->span <= 0 || g->objsize <= 0) {
    pr_info("KS_OBJ_ID addr=%016zx addr_src=measured(leak) page=unavailable "
            "page_src=none reason=no-slab-geometry constructed_slot=%d\n",
            (size_t)addr, constructed_slot);
    return 0;
  }
  out->page = addr & ~((uintptr_t)g->span - 1);
  out->offset = (size_t)(addr & ((uintptr_t)g->span - 1));
  out->slot = (long)(out->offset / (size_t)g->objsize);
  out->slot_match = (out->slot == constructed_slot);
  pr_info("KS_OBJ_ID addr=%016zx addr_src=measured(leak) page=%016zx "
          "page_src=derived(measured-span=%ld) offset=%zu slot=%ld "
          "slot_src=derived constructed_slot=%d match=%d\n",
          (size_t)addr, (size_t)out->page, g->span, out->offset, out->slot,
          constructed_slot, out->slot_match);
  return out->slot_match;
}

void kp_identity_slot(size_t slot, const char *alive, int sampled,
                      long d_active, long d_num, long d_slabs,
                      const struct kp_identity *anchor) {
  int addressed =
      anchor != NULL && anchor->page != 0 && (long)slot == anchor->constructed_slot;
  const char *effect = "unavailable";
  if (sampled) {
    if (d_slabs < 0 || d_num < 0) {
      effect = "page-left-cache";
    } else if (d_active < 0) {
      effect = "listed-page-freed";
    } else {
      effect = "no-counter-moved";
    }
  }
  if (addressed) {
    pr_info("KS_ID_SLOT slot=%zu alive=%s d_src=%s d_active=%ld "
            "d_num=%ld d_slabs=%ld effect=%s effect_src=derived "
            "addr=%016zx addr_src=measured(leak) page=%016zx "
            "page_src=derived(measured-geometry)\n",
            slot, alive, sampled ? "measured" : "unavailable", d_active,
            d_num, d_slabs, effect, (size_t)anchor->addr,
            (size_t)anchor->page);
  } else {
    pr_info("KS_ID_SLOT slot=%zu alive=%s d_src=%s d_active=%ld "
            "d_num=%ld d_slabs=%ld effect=%s effect_src=derived "
            "page=unavailable page_src=none reason=no-per-object-address\n",
            slot, alive, sampled ? "measured" : "unavailable", d_active,
            d_num, d_slabs, effect);
  }
}

void kp_identity_summary(size_t slots, size_t addressed) {
  pr_info("KS_ID_SUMMARY slots=%zu addressed=%zu page_known=%zu "
          "unavailable=%zu reason=one-leak-one-address "
          "effect_scope=cache-wide\n",
          slots, addressed, addressed,
          slots >= addressed ? slots - addressed : 0);
}
