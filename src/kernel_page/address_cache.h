/* address_cache.h — bookkeeping for recovered addresses.
 *
 * Two files matter before an address can be reused:
 *
 *   ks_cache.txt        boot_id + anchor_mm, kept across runs of one boot
 *   ks_mm_answer.txt    mm=0x…, written by the solver and read once
 *
 * Both are untrusted input: they can be stale (another boot), truncated, or
 * hand-edited.  This module owns the parsing and the validation rules; the
 * caller owns the I/O and keeps its existing messages, so a rejected file
 * still produces the same line it always did.
 *
 * Pure apart from reading /proc and the filesystem.  Host tests in
 * tests/kernel_page/cache_test.c exercise every rejection path.
 */
#ifndef KERNEL_PAGE_ADDRESS_CACHE_H
#define KERNEL_PAGE_ADDRESS_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* --- path selection ---------------------------------------------------- */

/* Same order and same writability test as before: KS_CACHE_PATH, then
 * $GHOSTLOCK_HOME, $TMPDIR, $HOME (each + "/ks_cache.txt"), then the two
 * device locations.  Returns a pointer to a static buffer or NULL. */
const char *kp_cache_select(int write);

/* KS_CACHE_RADIUS_GB, defaulted to 8 and clamped to 128. */
size_t kp_cache_radius_bytes(void);

/* Read /proc/sys/kernel/random/boot_id, stripped of CR/LF.  Returns 1 only
 * when at least 8 characters were read. */
int kp_boot_id_read(char *buf, size_t len);

/* --- saved anchor ------------------------------------------------------ */

enum kp_cache_status {
  KP_CACHE_OK = 0,
  KP_CACHE_NO_ANCHOR,     /* no anchor_mm line in the file */
  KP_CACHE_BOOT_MISMATCH, /* file was written for a different boot */
  KP_CACHE_OUT_OF_RANGE   /* anchor is zero or outside [lo, hi) */
};

/* Parse an open cache file one 128-byte line at a time, exactly as the
 * original loader did.  Later lines override earlier ones.  The anchor is
 * written only on KP_CACHE_OK.  Checks run in the original order: anchor
 * presence, then range, then boot id. */
enum kp_cache_status kp_cache_parse(FILE *f, const char *boot_id,
                                    uintptr_t lo, uintptr_t hi,
                                    uintptr_t *anchor);

/* Format exactly "boot_id=<id>\nanchor_mm=0x<16 hex>\n".  Returns the number
 * of bytes written, or 0 when the buffer is too small or the anchor is
 * outside [lo, hi). */
int kp_cache_format(char *buf, size_t len, const char *boot_id,
                    uintptr_t anchor, uintptr_t lo, uintptr_t hi);

/* --- one-shot answer file ---------------------------------------------- */

enum kp_answer_status {
  KP_ANSWER_OK = 0,
  KP_ANSWER_LEN,        /* not 22 (LF) or 23 (CRLF) bytes */
  KP_ANSWER_FORMAT,     /* missing "mm=0x" prefix or bad line ending */
  KP_ANSWER_HEX,        /* a character outside lowercase hex */
  KP_ANSWER_IMPLAUSIBLE /* unaligned, outside [lo, hi), or beyond RAM */
};

/* Parse the exact bytes read from the answer file.  `out` receives the
 * parsed number whenever the bytes form one, including when it is then
 * rejected as implausible, so the caller can report what it read.  Uppercase
 * hex is rejected on purpose: it has never been produced by the solver, and a
 * file with it is edited or foreign. */
enum kp_answer_status kp_answer_parse(const char *buf, size_t n, uintptr_t lo,
                                      uintptr_t hi, uintptr_t page_offset,
                                      uint64_t ram_bytes, uintptr_t *out);

#endif /* KERNEL_PAGE_ADDRESS_CACHE_H */
