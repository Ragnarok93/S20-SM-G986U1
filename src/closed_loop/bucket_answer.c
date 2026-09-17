#include "bucket_answer.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUCKET_ANSWER_MAX 8192
#define SHA256_DIGEST_SIZE 32

static uintptr_t g_bucket_answer_mm;

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t used;
} Sha256;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void set_reason(char *reason, size_t size, const char *text)
{
    if (reason && size) {
        snprintf(reason, size, "%s", text);
    }
}

static int valid_uuid(const char *s);
static int line_is(const char *line, const char *key, const char *value);

static void sha256_init(Sha256 *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->used = 0;
}

static uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(Sha256 *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (size_t i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (size_t i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_update(Sha256 *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len) {
        size_t n = 64 - ctx->used;
        if (n > len) n = len;
        memcpy(ctx->buffer + ctx->used, p, n);
        ctx->used += n;
        p += n;
        len -= n;
        ctx->bitlen += (uint64_t)n * 8;
        if (ctx->used == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->used = 0;
        }
    }
}

static void sha256_final(Sha256 *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t pad = 0x80;
    uint8_t zero = 0;
    uint8_t length[8];
    uint64_t total_bits = ctx->bitlen;

    for (size_t i = 0; i < 8; ++i) {
        length[i] = (uint8_t)(total_bits >> (56 - i * 8));
    }
    sha256_update(ctx, &pad, 1);
    while (ctx->used != 56) {
        sha256_update(ctx, &zero, 1);
    }
    sha256_update(ctx, length, sizeof(length));
    for (size_t i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

static int sha256_file(const char *path, uint8_t out[SHA256_DIGEST_SIZE])
{
    FILE *f = fopen(path, "rb");
    Sha256 ctx;
    unsigned char buf[4096];
    size_t n;

    if (!f) {
        return -1;
    }
    sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&ctx, buf, n);
    }
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    sha256_final(&ctx, out);
    return 0;
}

static void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                       char out[SHA256_DIGEST_SIZE * 2 + 1])
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[SHA256_DIGEST_SIZE * 2] = '\0';
}

static int read_first_line(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t used = 0;
    int ch;

    if (!f) {
        return -1;
    }
    while ((ch = fgetc(f)) != EOF && ch != '\n') {
        if (used + 2 >= size) {
            fclose(f);
            return -1;
        }
        out[used++] = (char)ch;
    }
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (used && out[used - 1] == '\r') {
        --used;
    }
    out[used] = '\0';
    return 0;
}

static int read_boot_id(char *out, size_t size)
{
    return read_first_line("/proc/sys/kernel/random/boot_id", out, size);
}

static int validate_bucket_file(const char *path,
                                const char *boot_id,
                                char *reason,
                                size_t reason_size)
{
    FILE *f;
    char line[512];
    size_t total = 0;
    size_t boot_seen = 0;
    size_t model_seen = 0;
    size_t hashsize_seen = 0;
    size_t verdict_seen = 0;

    set_reason(reason, reason_size, "invalid");
    f = fopen(path, "rb");
    if (!f) {
        set_reason(reason, reason_size, "bucket_file_unreadable");
        return 0;
    }
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        set_reason(reason, reason_size, "bucket_file_empty");
        return 0;
    }
    total += strlen(line);
    if (total && line[total - 1] != '\n') {
        int last = fgetc(f);

        if (last != EOF) {
            fclose(f);
            set_reason(reason, reason_size, "bucket_file_line-too-long");
            return 0;
        }
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "BUCKET_COLLECT_V1") != 0) {
        fclose(f);
        set_reason(reason, reason_size, "bucket_file_header");
        return 0;
    }

    while (fgets(line, sizeof(line), f)) {
        size_t length = strlen(line);

        total += length;
        if (total > 1024 * 1024) {
            fclose(f);
            set_reason(reason, reason_size, "bucket_file_too-large");
            return 0;
        }
        if (length && line[length - 1] != '\n') {
            int last = fgetc(f);

            if (last != EOF) {
                fclose(f);
                set_reason(reason, reason_size, "bucket_file_line-too-long");
                return 0;
            }
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "boot_id=", 8) == 0) {
            const char *value = line + 8;

            if (boot_seen ||
                !valid_uuid(value) || strcmp(value, boot_id) != 0) {
                fclose(f);
                set_reason(reason, reason_size, "bucket_file_boot_id");
                return 0;
            }
            boot_seen = 1;
        }
        else if (strncmp(line, "model=", 6) == 0) {
            if (model_seen || !line_is(line, "model", "raw-private")) {
                fclose(f);
                set_reason(reason, reason_size, "bucket_file_model");
                return 0;
            }
            model_seen = 1;
        }
        else if (strncmp(line, "futex_hashsize=", 15) == 0) {
            if (hashsize_seen || !line_is(line, "futex_hashsize", "0x800")) {
                fclose(f);
                set_reason(reason, reason_size, "bucket_file_hashsize");
                return 0;
            }
            hashsize_seen = 1;
        }
        else if (strncmp(line, "collector_verdict=", 18) == 0) {
            if (verdict_seen || !line_is(line, "collector_verdict", "PASS")) {
                fclose(f);
                set_reason(reason, reason_size, "bucket_file_verdict");
                return 0;
            }
            verdict_seen = 1;
        }
    }
    if (ferror(f)) {
        fclose(f);
        set_reason(reason, reason_size, "bucket_file_read");
        return 0;
    }
    fclose(f);
    if (!boot_seen || !model_seen || !hashsize_seen || !verdict_seen) {
        set_reason(reason, reason_size, "bucket_file_missing-field");
        return 0;
    }
    return 1;
}

static int valid_uuid(const char *s)
{
    if (strlen(s) != 36 || s[8] != '-' || s[13] != '-' ||
        s[18] != '-' || s[23] != '-') {
        return 0;
    }
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }
        if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

static int valid_lower_hex(const char *s, size_t length)
{
    if (strlen(s) != length) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

static int parse_answer(const char *text, uintptr_t *out)
{
    const char *prefix = "0x";

    if (strlen(text) != 18 || strncmp(text, prefix, strlen(prefix)) != 0) {
        return 0;
    }
    if (!valid_lower_hex(text + 2, 16)) {
        return 0;
    }
    *out = (uintptr_t)strtoull(text + 2, NULL, 16);
    return 1;
}

static int line_is(const char *line, const char *key, const char *value)
{
    size_t key_len = strlen(key);

    return strncmp(line, key, key_len) == 0 &&
           line[key_len] == '=' &&
           strcmp(line + key_len + 1, value) == 0;
}

int bucket_answer_parse_sample(const char *text,
                               const char *boot_id,
                               const char *bucket_sha256,
                               uintptr_t *out,
                               char *reason,
                               size_t reason_size)
{
    char copy[BUCKET_ANSWER_MAX];
    char *line;
    char *save = NULL;
    size_t answer_seen = 0;
    size_t boot_seen = 0;
    size_t model_seen = 0;
    size_t hashsize_seen = 0;
    size_t bucket_sha_seen = 0;
    size_t candidates_seen = 0;
    size_t ambiguous_seen = 0;
    size_t verdict_seen = 0;
    uintptr_t mm = 0;

    set_reason(reason, reason_size, "invalid");
    if (!text || !boot_id || !bucket_sha256 || !out) {
        set_reason(reason, reason_size, "input");
        return 0;
    }
    if (strlen(text) >= sizeof(copy)) {
        set_reason(reason, reason_size, "too-large");
        return 0;
    }
    strcpy(copy, text);
    line = copy;
    {
        char *next = strchr(line, '\n');

        if (next) {
            *next = '\0';
            save = next + 1;
        }
    }
    if (!line || strcmp(line, "BUCKET_ANSWER_V1") != 0) {
        set_reason(reason, reason_size, "header");
        return 0;
    }
    line = save;
    while (line != NULL && *line) {
        char *next = strchr(line, '\n');

        if (next) {
            *next = '\0';
            next = next + 1;
        }
        size_t n = strlen(line);

        if (n && line[n - 1] == '\r') {
            line[n - 1] = '\0';
            n = strlen(line);
        }
        if (!*line) {
            set_reason(reason, reason_size, "blank-line");
            return 0;
        }
        if (!answer_seen && strncmp(line, "answer=", 7) == 0) {
            if (!parse_answer(line + 7, &mm)) {
                set_reason(reason, reason_size, "answer");
                return 0;
            }
            answer_seen = 1;
        }
        else if (!boot_seen && strncmp(line, "boot_id=", 8) == 0) {
            const char *value = line + 8;

            if (!valid_uuid(value) || strcmp(value, boot_id) != 0) {
                set_reason(reason, reason_size, "boot_id");
                return 0;
            }
            boot_seen = 1;
        }
        else if (!model_seen && line_is(line, "model", "raw-private")) {
            model_seen = 1;
        }
        else if (!hashsize_seen && line_is(line, "futex_hashsize", "0x800")) {
            hashsize_seen = 1;
        }
        else if (!bucket_sha_seen &&
                 strncmp(line, "bucket_file_sha256=", 19) == 0) {
            const char *value = line + 19;

            if (!valid_lower_hex(value, 64) ||
                strcmp(value, bucket_sha256) != 0) {
                set_reason(reason, reason_size, "bucket_file_sha256");
                return 0;
            }
            bucket_sha_seen = 1;
        }
        else if (!candidates_seen && line_is(line, "candidates", "1")) {
            candidates_seen = 1;
        }
        else if (!ambiguous_seen && line_is(line, "ambiguous", "0")) {
            ambiguous_seen = 1;
        }
        else if (!verdict_seen && line_is(line, "answer_verdict", "UNIQUE")) {
            verdict_seen = 1;
        }
        else {
            set_reason(reason, reason_size, "unknown-or-duplicate");
            return 0;
        }
        line = next;
    }
    if (!answer_seen || !boot_seen || !model_seen || !hashsize_seen ||
        !bucket_sha_seen || !candidates_seen || !ambiguous_seen ||
        !verdict_seen) {
        set_reason(reason, reason_size, "missing-field");
        return 0;
    }
    if (mm == 0 || (mm & 0x3fULL) != 0) {
        set_reason(reason, reason_size, "alignment");
        return 0;
    }
    if (!bucket_answer_mm_plausible(mm)) {
        set_reason(reason, reason_size, "plausibility");
        return 0;
    }
    *out = mm;
    set_reason(reason, reason_size, "ok");
    return 1;
}

int bucket_answer_sha256_file(const char *path, uint8_t out[32])
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    char hex[SHA256_DIGEST_SIZE * 2 + 1];

    if (!path || !out || sha256_file(path, digest) != 0) {
        return -1;
    }
    sha256_hex(digest, hex);
    memcpy(out, digest, SHA256_DIGEST_SIZE);
    return 0;
}

int bucket_answer_load(uintptr_t *out, char *reason, size_t reason_size)
{
    const char *answer_path = getenv("KS_BUCKET_ANSWER");
    const char *bucket_path = getenv("KS_BUCKET_FILE");
    char boot_id[64];
    char answer[BUCKET_ANSWER_MAX];
    uint8_t digest[SHA256_DIGEST_SIZE];
    char bucket_hex[SHA256_DIGEST_SIZE * 2 + 1];
    FILE *f;
    size_t n;
    int rc;

    set_reason(reason, reason_size, "invalid");
    if (!answer_path || !*answer_path) {
        set_reason(reason, reason_size, "missing");
        return 0;
    }
    if (!bucket_path || !*bucket_path) {
        bucket_path = "/data/local/tmp/ks_bucket_classes_v1.txt";
    }
    if (read_boot_id(boot_id, sizeof(boot_id)) != 0 || !valid_uuid(boot_id)) {
        set_reason(reason, reason_size, "boot_id");
        return 0;
    }
    if (sha256_file(bucket_path, digest) != 0) {
        set_reason(reason, reason_size, "bucket_file_unreadable");
        return 0;
    }
    sha256_hex(digest, bucket_hex);
    if (!validate_bucket_file(bucket_path, boot_id, reason, reason_size)) {
        return 0;
    }
    f = fopen(answer_path, "rb");
    if (!f) {
        set_reason(reason, reason_size, "answer-unreadable");
        return 0;
    }
    n = fread(answer, 1, sizeof(answer) - 1, f);
    if (ferror(f) || n == 0 || n >= sizeof(answer) - 1) {
        fclose(f);
        set_reason(reason, reason_size, "answer-size");
        return 0;
    }
    fclose(f);
    answer[n] = '\0';
    rc = bucket_answer_parse_sample(answer, boot_id, bucket_hex, out,
                                    reason, reason_size);
    if (rc) {
        g_bucket_answer_mm = *out;
    }
    return rc;
}

uintptr_t bucket_answer_pending_mm(void)
{
    return g_bucket_answer_mm;
}
