/* experiment_state.c — what this run actually established.
 *
 * The experiment produces several different propositions, and they are not
 * interchangeable:
 *
 *   the address was re-checked        ADDRESS_VERIFIED
 *   one fresh 32-slot slab was built  ANCHOR_VERIFIED
 *   the emptying free registered      ALLOCATOR_TRANSITION_VERIFIED
 *   the slab left the cache whole     RECLAMATION_VERIFIED
 *   the image occupies that page      OBJECT_IDENTITY_VERIFIED
 *   the kernel read those bytes       KERNEL_CONSUMPTION_VERIFIED
 *
 * The preparation path can establish the first four.  It cannot establish
 * the last two from inside the process, so they stay unverified and the
 * report says so: a successful allocator cycle must never be reported as a
 * successful placement.
 */
#include "common.h"
#include "kernel_page.h"

static unsigned kp_verified;

void kp_result_reset(void) { kp_verified = 0; }

void kp_result_mark(enum kp_status status) { kp_verified |= (unsigned)status; }

int kp_result_has(enum kp_status status) {
  return (kp_verified & (unsigned)status) != 0;
}

const char *kp_status_name(enum kp_status status) {
  switch (status) {
    case KP_ADDRESS_VERIFIED:
      return "address";
    case KP_ANCHOR_VERIFIED:
      return "anchor";
    case KP_ALLOCATOR_TRANSITION_VERIFIED:
      return "transition";
    case KP_RECLAMATION_VERIFIED:
      return "reclamation";
    case KP_OBJECT_IDENTITY_VERIFIED:
      return "identity";
    case KP_KERNEL_CONSUMPTION_VERIFIED:
      return "consumption";
  }
  return "?";
}

void kp_result_report(uintptr_t base) {
  static const enum kp_status all[] = {
      KP_ADDRESS_VERIFIED,
      KP_ANCHOR_VERIFIED,
      KP_ALLOCATOR_TRANSITION_VERIFIED,
      KP_RECLAMATION_VERIFIED,
      KP_OBJECT_IDENTITY_VERIFIED,
      KP_KERNEL_CONSUMPTION_VERIFIED,
  };
  char verified[128];
  char unverified[128];
  size_t vn = 0;
  size_t un = 0;

  verified[0] = 0;
  unverified[0] = 0;
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    const char *name = kp_status_name(all[i]);
    int has = kp_result_has(all[i]);
    char *dst = has ? verified : unverified;
    size_t *len = has ? &vn : &un;
    int wrote = snprintf(dst + *len, 128 - *len, "%s%s", *len ? "," : "",
                         name);
    if (wrote > 0) {
      *len += (size_t)wrote;
    }
  }
  pr_info("KS_RESULT verified=%s unverified=%s base=%016zx\n",
          verified[0] ? verified : "none",
          unverified[0] ? unverified : "none", (size_t)base);
}
