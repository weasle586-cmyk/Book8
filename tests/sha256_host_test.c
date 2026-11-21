#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/sha256.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

static void hex(const char *text, uint8_t *out, size_t length)
{
    for (size_t i = 0u; i < length; ++i) {
        unsigned value;
        CHECK(sscanf(text + i * 2u, "%2x", &value) == 1);
        out[i] = (uint8_t)value;
    }
}

static void check_digest(const void *data, size_t length, const char *expected)
{
    uint8_t want[32], got[32];
    hex(expected, want, sizeof(want));
    book8_sha256(data, length, got);
    CHECK(memcmp(want, got, sizeof(got)) == 0);
}

static void check_hmac(const uint8_t *key, size_t key_length, const void *data,
    size_t length, const char *expected)
{
    uint8_t want[32], got[32];
    hex(expected, want, sizeof(want));
    book8_hmac_sha256(key, key_length, data, length, got);
    CHECK(memcmp(want, got, sizeof(got)) == 0);
}

int main(void)
{
    /* NIST FIPS 180-4 examples. */
    check_digest("", 0u,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_digest("abc", 3u,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check_digest(two_block, strlen(two_block),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    static uint8_t million[1000000];
    memset(million, 'a', sizeof(million));
    check_digest(million, sizeof(million),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    /* Streaming in odd-sized pieces equals one-shot hashing. */
    struct book8_sha256 ctx;
    uint8_t streamed[32], oneshot[32];
    book8_sha256_init(&ctx);
    for (size_t offset = 0u; offset < 1000u;) {
        size_t take = (offset % 7u) + 1u;
        if (take > 1000u - offset) {
            take = 1000u - offset;
        }
        book8_sha256_update(&ctx, million + offset, take);
        offset += take;
    }
    book8_sha256_final(&ctx, streamed);
    book8_sha256(million, 1000u, oneshot);
    CHECK(memcmp(streamed, oneshot, 32u) == 0);

    /* RFC 4231 HMAC-SHA256 test cases 1, 2, 4, 6 and 7. */
    uint8_t key[131];
    memset(key, 0x0b, 20u);
    check_hmac(key, 20u, "Hi There", 8u,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    check_hmac((const uint8_t *)"Jefe", 4u, "what do ya want for nothing?", 28u,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    for (unsigned i = 0u; i < 25u; ++i) {
        key[i] = (uint8_t)(i + 1u);
    }
    uint8_t data[50];
    memset(data, 0xcd, sizeof(data));
    check_hmac(key, 25u, data, sizeof(data),
        "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    memset(key, 0xaa, sizeof(key));
    const char *large = "Test Using Larger Than Block-Size Key - Hash Key First";
    check_hmac(key, 131u, large, strlen(large),
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    const char *large_data = "This is a test using a larger than block-size key and a "
        "larger than block-size data. The key needs to be hashed before being used "
        "by the HMAC algorithm.";
    check_hmac(key, 131u, large_data, strlen(large_data),
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");

    uint8_t a[32] = {1u}, b[32] = {1u};
    CHECK(book8_digest_equal(a, b, 32u));
    b[31] = 2u;
    CHECK(!book8_digest_equal(a, b, 32u));
    printf("PASS: SHA-256/HMAC-SHA256 host test\n");
    return 0;
}
