/* allocator_observation.c — the only reader of /proc/slabinfo in this tree.
 *
 * Moved verbatim out of src/util.c so the counter semantics live in one
 * place, next to the kernel definition they mirror.  No behaviour change:
 * same format string, same fields, same fallbacks.
 *
 * mm/slub.c (4.19.113) defines what these numbers mean:
 *   get_slabinfo()   nr_slabs  = sum(node_nr_slabs)   -> slabdata
 *                    nr_objs   = sum(node_nr_objs)    -> num_objs
 *                    nr_free   = count_partial(free)  -> active_objs
 *                                   = num_objs - nr_free
 * and every one of them counts only pages on n->partial.  Frozen pages on a
 * CPU partial list, the active page, and pages on no list do not appear.
 */
#include "common.h"
#include "kernel_page.h"

#include <errno.h>
#include <string.h>

int mm_slab_scan(FILE *f, long *active, long *num, long *slabs) {
  char line[512];
  if (active) *active = -1;
  if (num) *num = -1;
  if (slabs) *slabs = -1;
  if (f == NULL) return 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "mm_struct ", 10) != 0) continue;
    {
      char name[64];
      long a = 0, o = 0, osz = 0, ops = 0, pps = 0;
      long t1 = 0, t2 = 0, t3 = 0, as = 0, ns = 0, sh = 0;
      if (sscanf(line,
                 "%63s %ld %ld %ld %ld %ld : tunables %ld %ld %ld "
                 ": slabdata %ld %ld %ld",
                 name, &a, &o, &osz, &ops, &pps, &t1, &t2, &t3, &as, &ns,
                 &sh) >= 11) {
        if (active) *active = a;
        if (num) *num = o;
        if (slabs) *slabs = ns;
        return 1;
      }
    }
    break;
  }
  return 0;
}

void mm_slab_counts_full(long *active, long *num, long *slabs) {
  FILE *f = fopen("/proc/slabinfo", "r");
  if (active) *active = -1;
  if (num) *num = -1;
  if (slabs) *slabs = -1;
  if (!f) return;
  (void)mm_slab_scan(f, active, num, slabs);
  fclose(f);
}

void mm_slab_counts(long *active, long *slabs) {
  mm_slab_counts_full(active, NULL, slabs);
}

void mm_slab_print(const char *tag, long active, long num, long slabs) {
  pr_info("MMSLAB %s active_objs=%ld num_objs=%ld slabdata=%ld\n", tag,
          active, num, slabs);
}

/* Diagnostic: report the mm_struct cache line so a run can distinguish
 * "the target slab was never discarded" from "the slab was discarded and the
 * order-3 spray still missed it".  /proc/slabinfo is world readable here;
 * every per-cache sysfs node is root only. */
void mm_slab_log(const char *tag) {
  FILE *f = fopen("/proc/slabinfo", "r");
  if (!f) {
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "mm_struct ", 10) != 0) {
      continue;
    }
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[--n] = 0;
    }
    pr_info("MMSLAB %s %s\n", tag, line);
    break;
  }
  fclose(f);

  /* slabinfo cannot see objects freed onto a per-CPU partial list, so its
   * active_objs is a constant here and says nothing about frees.  Slab: in
   * /proc/meminfo is page-granular and does fall when a slab page is
   * discarded, which is the state the order-3 spray needs.  Log both. */
  if ((f = fopen("/proc/meminfo", "r")) != NULL) {
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "Slab:", 5) != 0) {
        continue;
      }
      size_t m = strlen(line);
      while (m && (line[m - 1] == '\n' || line[m - 1] == '\r')) {
        line[--m] = 0;
      }
      pr_info("SLABPAGES %s %s\n", tag, line);
      break;
    }
    fclose(f);
  }
}

void slab32_log(const char *tag) {
  FILE *f = fopen("/proc/slabinfo", "r");
  char line[256];
  int found = 0;
  if (f == NULL) {
    pr_info("SLAB32 %s unavailable errno=%d\n", tag, errno);
    return;
  }
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "kmalloc-32k ", 12) == 0) {
      pr_info("SLAB32 %s %s", tag, line);
      found = 1;
      break;
    }
  }
  fclose(f);
  if (!found) {
    pr_info("SLAB32 %s absent\n", tag);
  }
}
