#ifndef BUCKET_ANSWER_H
#define BUCKET_ANSWER_H

#include <stddef.h>
#include <stdint.h>

int bucket_answer_load(uintptr_t *out, char *reason, size_t reason_size);
int bucket_answer_sha256_file(const char *path, uint8_t out[32]);
uintptr_t bucket_answer_pending_mm(void);
int bucket_answer_mm_plausible(uintptr_t mm);
int bucket_answer_parse_sample(const char *text,
                               const char *boot_id,
                               const char *bucket_sha256,
                               uintptr_t *out,
                               char *reason,
                               size_t reason_size);

#endif
