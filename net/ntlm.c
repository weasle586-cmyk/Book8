#include "ntlm.h"
#include "../include/string.h"

static uint32_t get32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8u)); }
static void put32(uint8_t *p, uint32_t v)
{ for (uint32_t i = 0; i < 4u; ++i) { p[i] = (uint8_t)(v >> (8u * i)); } }
static uint32_t rotate(uint32_t v, uint32_t n) { return (v << n) | (v >> (32u - n)); }
void book8_ntlm_clear(void *data, uint32_t length)
{ volatile uint8_t *p = data; while (length--) { *p++ = 0; } }
static void block(uint32_t h[4], const uint8_t *p, bool md5)
{
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const uint8_t shifts[16] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    static const uint8_t m4shift[12] = {3,7,11,19,3,5,9,13,3,9,11,15};
    static const uint8_t order[16] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
    uint32_t x[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (uint32_t i = 0; i < 16u; ++i) { x[i] = get32(p + i * 4u); }
    for (uint32_t i = 0; i < (md5 ? 64u : 48u); ++i) {
        uint32_t f, g, add, shift, round = i / 16u;
        if (!round) { f = (b & c) | (~b & d); }
        else if (round == 1u) { f = md5 ? ((d & b) | (~d & c)) : ((b & c) | (b & d) | (c & d)); }
        else if (round == 2u) { f = b ^ c ^ d; }
        else { f = c ^ (b | ~d); }
        if (md5) {
            g = !round ? i : round == 1u ? (5u * i + 1u) % 16u : round == 2u ? (3u * i + 5u) % 16u : (7u * i) % 16u;
            add = k[i]; shift = shifts[round * 4u + i % 4u];
        } else {
            g = !round ? i : round == 1u ? (i % 4u) * 4u + (i % 16u) / 4u : order[i % 16u];
            add = !round ? 0u : round == 1u ? 0x5a827999u : 0x6ed9eba1u;
            shift = m4shift[round * 4u + i % 4u];
        }
        uint32_t next = rotate(a + f + x[g] + add, shift) + (md5 ? b : 0u);
        a = d; d = c; c = b; b = next;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    book8_ntlm_clear(x, sizeof(x));
}
static void digest(const void *data, uint32_t length, uint8_t out[16], bool md5)
{
    uint32_t h[4] = {0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u};
    const uint8_t *p = data; uint32_t n = length;
    while (n >= 64u) { block(h, p, md5); p += 64u; n -= 64u; }
    uint8_t tail[128] = {0}; memcpy(tail, p, n); tail[n] = 0x80u;
    uint32_t total = n < 56u ? 64u : 128u;
    uint64_t bits = (uint64_t)length * 8u;
    for (uint32_t i = 0; i < 8u; ++i) { tail[total - 8u + i] = (uint8_t)(bits >> (8u * i)); }
    block(h, tail, md5); if (total == 128u) { block(h, tail + 64u, md5); }
    for (uint32_t i = 0; i < 4u; ++i) { put32(out + i * 4u, h[i]); }
    book8_ntlm_clear(tail, sizeof(tail)); book8_ntlm_clear(h, sizeof(h));
}
void book8_ntlm_md4(const void *data, uint32_t length, uint8_t out[16]) { digest(data, length, out, false); }
void book8_ntlm_md5(const void *data, uint32_t length, uint8_t out[16]) { digest(data, length, out, true); }
static void hmac(const uint8_t key[16], const void *data, uint32_t length, uint8_t out[16])
{
    uint8_t buffer[704], inner[16];
    for (uint32_t i = 0; i < 64u; ++i) { buffer[i] = (i < 16u ? key[i] : 0u) ^ 0x36u; }
    memcpy(buffer + 64u, data, length); book8_ntlm_md5(buffer, length + 64u, inner);
    for (uint32_t i = 0; i < 64u; ++i) { buffer[i] = (i < 16u ? key[i] : 0u) ^ 0x5cu; }
    memcpy(buffer + 64u, inner, 16u); book8_ntlm_md5(buffer, 80u, out);
    book8_ntlm_clear(buffer, sizeof(buffer)); book8_ntlm_clear(inner, sizeof(inner));
}
static uint32_t wide(uint8_t *out, const char *text, bool upper)
{
    uint32_t n = 0;
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (upper && c >= 'a' && c <= 'z') { c -= 'a' - 'A'; }
        out[n++] = c; out[n++] = 0;
    }
    return n;
}
static bool ascii(const char *text, uint32_t limit)
{
    if (!text || strlen(text) >= limit) { return false; }
    while (*text) { if ((uint8_t)*text < 32u || (uint8_t)*text >= 127u) { return false; } ++text; }
    return true;
}
static void security_buffer(uint8_t *token, uint32_t field, uint32_t offset, uint32_t size)
{
    token[field] = token[field + 2u] = (uint8_t)size;
    token[field + 1u] = token[field + 3u] = (uint8_t)(size >> 8u);
    put32(token + field + 4u, offset);
}
bool book8_ntlm_auth(const char *user, const char *domain, const char *password,
    const uint8_t *challenge, uint32_t length, uint64_t nonce,
    uint8_t token[BOOK8_NTLM_TOKEN], uint32_t *size, uint8_t key[16])
{
    if (!ascii(user, 64u) || !user[0] || !ascii(domain, 64u) || !ascii(password, 128u)
        || length < 48u || memcmp(challenge, "NTLMSSP\0", 8u) || get32(challenge + 8u) != 2u) { return false; }
    uint32_t flags = get32(challenge + 20u), info = get16(challenge + 40u), offset = get32(challenge + 44u);
    if (!(flags & 1u) || !(flags & 0x800000u) || info < 4u || info > 512u
        || offset < 48u || offset > length || info > length - offset) { return false; }
    uint8_t timestamp[8] = {0}; bool ended = false, has_time = false;
    for (uint32_t i = 0; i + 4u <= info;) {
        uint32_t id = get16(challenge + offset + i), n = get16(challenge + offset + i + 2u);
        i += 4u;
        if (n > info - i) { return false; }
        if (!id) { if (n || i != info) { return false; } ended = true; break; }
        /* MIC/channel-binding policies require capabilities not offered by this client. */
        if ((id == 6u && (n != 4u || (get32(challenge + offset + i) & 2u))) || id == 10u) { return false; }
        if (id == 7u) { if (n != 8u) { return false; } memcpy(timestamp, challenge + offset + i, 8u); has_time = true; }
        i += n;
    }
    if (!ended) { return false; }
    uint8_t unicode[256], hash[16], v2[16], proof[16], seed[24], client[16], response[564] = {0};
    uint32_t n = wide(unicode, password, false); book8_ntlm_md4(unicode, n, hash);
    n = wide(unicode, user, true); n += wide(unicode + n, domain, false); hmac(hash, unicode, n, v2);
    memcpy(seed, challenge + 24u, 8u);
    for (uint32_t i = 0; i < 8u; ++i) { seed[8u + i] = (uint8_t)(nonce >> (i * 8u)); }
    memcpy(seed + 16u, timestamp, 8u); hmac(v2, seed, sizeof(seed), client);
    memcpy(response, challenge + 24u, 8u); response[8] = response[9] = 1u;
    memcpy(response + 16u, timestamp, 8u); memcpy(response + 24u, client, 8u);
    memcpy(response + 36u, challenge + offset, info);
    uint32_t blob = 32u + info; hmac(v2, response, 8u + blob, proof); hmac(v2, proof, 16u, key);
    memset(token, 0, BOOK8_NTLM_TOKEN); memcpy(token, "NTLMSSP", 7u); put32(token + 8u, 3u);
    uint32_t pos = 64u;
    security_buffer(token, 12u, pos, 24u);
    if (!has_time) {
        uint8_t lm[16]; memcpy(lm, challenge + 24u, 8u); memcpy(lm + 8u, client, 8u);
        hmac(v2, lm, sizeof(lm), token + pos); memcpy(token + pos + 16u, client, 8u);
        book8_ntlm_clear(lm, sizeof(lm));
    }
    pos += 24u;
    security_buffer(token, 20u, pos, 16u + blob); memcpy(token + pos, proof, 16u);
    memcpy(token + pos + 16u, response + 8u, blob); pos += 16u + blob;
    n = wide(token + pos, domain, false); security_buffer(token, 28u, pos, n); pos += n;
    n = wide(token + pos, user, false); security_buffer(token, 36u, pos, n); pos += n;
    security_buffer(token, 44u, pos, 0u); security_buffer(token, 52u, pos, 0u);
    put32(token + 60u, flags & 0x00888215u); *size = pos;
    book8_ntlm_clear(unicode, sizeof(unicode)); book8_ntlm_clear(hash, sizeof(hash));
    book8_ntlm_clear(v2, sizeof(v2)); book8_ntlm_clear(proof, sizeof(proof));
    book8_ntlm_clear(seed, sizeof(seed)); book8_ntlm_clear(client, sizeof(client));
    book8_ntlm_clear(response, sizeof(response));
    return true;
}
