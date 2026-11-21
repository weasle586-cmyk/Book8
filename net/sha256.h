#ifndef BOOK8_SHA256_H
#define BOOK8_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK8_SHA256_BYTES 32u
#define BOOK8_SHA256_BLOCK 64u

/* FIPS 180-4 SHA-256 and RFC 2104 HMAC-SHA256; zero allocation, freestanding. */
struct book8_sha256 {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[BOOK8_SHA256_BLOCK];
    size_t used;
};

void book8_sha256_init(struct book8_sha256 *ctx);
void book8_sha256_update(struct book8_sha256 *ctx, const void *data, size_t length);
void book8_sha256_final(struct book8_sha256 *ctx, uint8_t digest[BOOK8_SHA256_BYTES]);
void book8_sha256(const void *data, size_t length, uint8_t digest[BOOK8_SHA256_BYTES]);
void book8_hmac_sha256(const uint8_t *key, size_t key_length, const void *data,
    size_t length, uint8_t mac[BOOK8_SHA256_BYTES]);
/* Constant-time comparison for MACs/digests. */
bool book8_digest_equal(const uint8_t *a, const uint8_t *b, size_t length);

#endif
