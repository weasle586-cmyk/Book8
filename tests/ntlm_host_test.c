#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../net/ntlm.h"
static void check(const char *text, const char *md4, const char *md5)
{
    uint8_t out[16]; char hex[33];
    book8_ntlm_md4(text, (uint32_t)strlen(text), out);
    for (uint32_t i = 0; i < 16u; ++i) { snprintf(hex + i * 2u, 3u, "%02x", out[i]); }
    assert(!strcmp(hex, md4));
    book8_ntlm_md5(text, (uint32_t)strlen(text), out);
    for (uint32_t i = 0; i < 16u; ++i) { snprintf(hex + i * 2u, 3u, "%02x", out[i]); }
    assert(!strcmp(hex, md5));
}
int main(void)
{
    check("", "31d6cfe0d16ae931b73c59d7e0c089c0", "d41d8cd98f00b204e9800998ecf8427e");
    check("abc", "a448017aaf21d8525fc10ae87aa6729d", "900150983cd24fb0d6963f7d28e17f72");
    check("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "043f8582f241db351ce627e153e7f0e4", "d174ab98d277d9f5a5611c2c9f419d9f");
    uint8_t challenge[64] = {0}, token[BOOK8_NTLM_TOKEN], key[16]; uint32_t size;
    memcpy(challenge, "NTLMSSP", 7u); challenge[8] = 2u;
    challenge[20] = 1u; challenge[22] = 0x80u; challenge[40] = 16u; challenge[44] = 48u;
    challenge[48] = 7u; challenge[50] = 8u; challenge[52] = 1u;
    assert(book8_ntlm_auth("User", "Domain", "Password", challenge, 64u, 1u, token, &size, key));
    assert(size <= BOOK8_NTLM_TOKEN && !memcmp(token, "NTLMSSP", 7u) && token[8] == 3u);
    uint8_t previous[BOOK8_NTLM_TOKEN]; memcpy(previous, token, size);
    assert(book8_ntlm_auth("User", "Domain", "Password", challenge, 64u, 2u, token, &size, key));
    assert(memcmp(previous, token, size));
    assert(!book8_ntlm_auth("User", "Domain", "Password", challenge, 47u, 1u, token, &size, key));
    challenge[44] = 255u;
    assert(!book8_ntlm_auth("User", "Domain", "Password", challenge, 64u, 1u, token, &size, key));
    challenge[44] = 48u; challenge[48] = 6u; challenge[50] = 4u; challenge[52] = 2u;
    assert(!book8_ntlm_auth("User", "Domain", "Password", challenge, 64u, 1u, token, &size, key));
    book8_ntlm_clear(key, 16u);
    for (uint32_t i = 0; i < 16u; ++i) { assert(key[i] == 0u); }
    puts("PASS: MD4/MD5 RFC vectors, NTLMv2 bounds, nonce uniqueness, policy rejection and secret clearing");
    return 0;
}
