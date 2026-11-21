#include "sha256.h"

static const uint32_t k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static void compress(struct book8_sha256 *ctx, const uint8_t *block)
{
    uint32_t w[64];
    for (unsigned i = 0u; i < 16u; ++i) {
        w[i] = ((uint32_t)block[i * 4u] << 24u) | ((uint32_t)block[i * 4u + 1u] << 16u)
            | ((uint32_t)block[i * 4u + 2u] << 8u) | block[i * 4u + 3u];
    }
    for (unsigned i = 16u; i < 64u; ++i) {
        uint32_t s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
        uint32_t s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (unsigned i = 0u; i < 64u; ++i) {
        uint32_t t1 = h + (rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u))
            + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u))
            + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
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

void book8_sha256_init(struct book8_sha256 *ctx)
{
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    for (unsigned i = 0u; i < 8u; ++i) {
        ctx->state[i] = initial[i];
    }
    ctx->bytes = 0u;
    ctx->used = 0u;
}

void book8_sha256_update(struct book8_sha256 *ctx, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    ctx->bytes += length;
    while (length != 0u) {
        size_t take = BOOK8_SHA256_BLOCK - ctx->used;
        if (take > length) {
            take = length;
        }
        for (size_t i = 0u; i < take; ++i) {
            ctx->block[ctx->used + i] = bytes[i];
        }
        ctx->used += take;
        bytes += take;
        length -= take;
        if (ctx->used == BOOK8_SHA256_BLOCK) {
            compress(ctx, ctx->block);
            ctx->used = 0u;
        }
    }
}

void book8_sha256_final(struct book8_sha256 *ctx, uint8_t digest[BOOK8_SHA256_BYTES])
{
    uint64_t bits = ctx->bytes * 8u;
    uint8_t pad = 0x80u;
    book8_sha256_update(ctx, &pad, 1u);
    pad = 0u;
    while (ctx->used != 56u) {
        book8_sha256_update(ctx, &pad, 1u);
    }
    uint8_t length[8];
    for (unsigned i = 0u; i < 8u; ++i) {
        length[i] = (uint8_t)(bits >> (56u - i * 8u));
    }
    book8_sha256_update(ctx, length, sizeof(length));
    for (unsigned i = 0u; i < 8u; ++i) {
        digest[i * 4u] = (uint8_t)(ctx->state[i] >> 24u);
        digest[i * 4u + 1u] = (uint8_t)(ctx->state[i] >> 16u);
        digest[i * 4u + 2u] = (uint8_t)(ctx->state[i] >> 8u);
        digest[i * 4u + 3u] = (uint8_t)ctx->state[i];
    }
}

void book8_sha256(const void *data, size_t length, uint8_t digest[BOOK8_SHA256_BYTES])
{
    struct book8_sha256 ctx;
    book8_sha256_init(&ctx);
    book8_sha256_update(&ctx, data, length);
    book8_sha256_final(&ctx, digest);
}

void book8_hmac_sha256(const uint8_t *key, size_t key_length, const void *data,
    size_t length, uint8_t mac[BOOK8_SHA256_BYTES])
{
    uint8_t block[BOOK8_SHA256_BLOCK] = {0};
    if (key_length > BOOK8_SHA256_BLOCK) {
        book8_sha256(key, key_length, block);
    } else {
        for (size_t i = 0u; i < key_length; ++i) {
            block[i] = key[i];
        }
    }
    uint8_t pad[BOOK8_SHA256_BLOCK];
    uint8_t inner[BOOK8_SHA256_BYTES];
    struct book8_sha256 ctx;
    for (unsigned i = 0u; i < BOOK8_SHA256_BLOCK; ++i) {
        pad[i] = block[i] ^ 0x36u;
    }
    book8_sha256_init(&ctx);
    book8_sha256_update(&ctx, pad, sizeof(pad));
    book8_sha256_update(&ctx, data, length);
    book8_sha256_final(&ctx, inner);
    for (unsigned i = 0u; i < BOOK8_SHA256_BLOCK; ++i) {
        pad[i] = block[i] ^ 0x5cu;
    }
    book8_sha256_init(&ctx);
    book8_sha256_update(&ctx, pad, sizeof(pad));
    book8_sha256_update(&ctx, inner, sizeof(inner));
    book8_sha256_final(&ctx, mac);
}

bool book8_digest_equal(const uint8_t *a, const uint8_t *b, size_t length)
{
    uint8_t difference = 0u;
    for (size_t i = 0u; i < length; ++i) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0u;
}
