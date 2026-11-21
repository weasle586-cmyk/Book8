#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../net/smb.h"
#include "../net/sha256.h"
static struct book8_share share;
static struct book8_net net;
static uint64_t now;
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8u)); }
static uint32_t g32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u); }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u); }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2u, (uint16_t)(v >> 16u)); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4u, (uint32_t)(v >> 32u)); }
enum book8_net_result book8_tcp_connect(struct book8_tcp *t, struct book8_net *n, uint32_t ip, uint16_t port, uint64_t time)
{ (void)time; assert(port == 445u); memset(t, 0, sizeof(*t)); t->net = n; t->peer = ip; t->state = TCP_CONNECTED; return BOOK8_NET_OK; }
void book8_tcp_poll(struct book8_tcp *t, uint64_t time) { (void)t; (void)time; }
void book8_tcp_close(struct book8_tcp *t) { t->net = NULL; t->state = TCP_CLOSED; }
void book8_tcp_consume(struct book8_tcp *t, uint32_t size)
{ assert(size <= t->rx_length); memmove(t->rx, t->rx + size, t->rx_length - size); t->rx_length -= size; }
enum book8_net_result book8_tcp_send(struct book8_tcp *t, const void *p, uint32_t size, uint64_t time)
{ (void)t; (void)time; assert(size <= 1400u && !memcmp((const uint8_t *)p + 4u, "\xfeSMB", 4u)); return BOOK8_NET_OK; }
static uint8_t *reply(uint32_t body, uint32_t status)
{
    uint32_t size = body + 64u;
    memset(share.tcp.rx, 0u, sizeof(share.tcp.rx));
    share.tcp.rx[2] = (uint8_t)(size >> 8u); share.tcp.rx[3] = (uint8_t)size;
    share.tcp.rx_length = size + 4u;
    uint8_t *p = share.tcp.rx + 4u;
    memcpy(p, "\xfeSMB", 4u); p16(p + 4u, 64u); p32(p + 8u, status);
    p16(p + 12u, share.command); p32(p + 16u, 1u); p64(p + 24u, share.pending_id);
    p32(p + 36u, share.tree); p64(p + 40u, share.session);
    return p + 64u;
}
static void poll(void) { book8_share_poll(&share, ++now); }
static void sign_reply(void)
{
    uint8_t *p = share.tcp.rx + 4u, mac[32];
    p32(p + 16u, 9u);
    book8_hmac_sha256(share.session_key, 16u, p, share.tcp.rx_length - 4u, mac);
    memcpy(p + 48u, mac, 16u);
}
static void start(void)
{
    assert(book8_share_connect(&share, &net, BOOK8_NET_IP(192,168,7,20), "public", ++now));
    poll(); assert(share.command == 0u && share.waiting);
    uint8_t *b = reply(64u, 0u); p16(b, 65u); p16(b + 4u, 0x0210u);
    p32(b + 28u, 65536u); p32(b + 32u, 65536u); p32(b + 36u, 65536u);
    poll(); assert(share.command == 1u);
    b = reply(24u, 0xc0000016u); p16(b, 9u); p16(b + 4u, 72u); p16(b + 6u, 16u);
    memcpy(b + 8u, "NTLMSSP\0", 8u); p32(b + 16u, 2u); p64(b - 24u, 42u);
    poll(); assert(share.command == 1u);
    b = reply(8u, 0u); p16(b, 9u); p16(b + 2u, 2u); p64(b - 24u, 42u);
    poll(); assert(share.command == 3u);
    assert(g16(share.request + 72u) == 72u);
    b = reply(16u, 0u); p16(b, 16u); b[2] = 1u; p32(b - 28u, 7u);
    poll(); assert(share.command == 5u);
    b = reply(88u, 0u); p16(b, 89u); memset(b + 64u, 0xaau, 16u);
    poll(); assert(share.command == 14u);
}
static void entry(uint8_t *p, const char *name, bool dir, uint32_t size)
{
    p64(p + 40u, size); p32(p + 56u, dir ? 16u : 0u); p32(p + 60u, (uint32_t)strlen(name) * 2u);
    for (uint32_t i = 0u; name[i]; ++i) { p[i * 2u + 64u] = (uint8_t)name[i]; }
}
int main(void)
{
    start();
    uint8_t *b = reply(168u, 0u); p16(b, 9u); p16(b + 2u, 72u); p32(b + 4u, 160u);
    p32(b + 8u, 80u); entry(b + 8u, "folder", true, 0u); entry(b + 88u, "test.c", false, 5u);
    poll(); assert(share.count == 2u && share.entries[0].directory && share.entries[1].size == 5u);
    reply(8u, 0x80000006u); poll(); assert(share.command == 6u);
    b = reply(60u, 0u); p16(b, 60u); poll(); assert(share.state == SHARE_READY);
    assert(!book8_share_download(&share, 0u, now));
    assert(book8_share_download(&share, 1u, now)); poll(); assert(share.command == 5u);
    b = reply(88u, 0u); p16(b, 89u); p64(b + 48u, 5u); memset(b + 64u, 1u, 16u);
    poll(); assert(share.command == 8u && g32(share.request + 72u) == 5u);
    b = reply(21u, 0u); p16(b, 17u); b[2] = 80u; p32(b + 4u, 5u); memcpy(b + 16u, "hello", 5u);
    poll(); assert(share.command == 6u);
    b = reply(60u, 0u); p16(b, 60u); poll();
    assert(share.state == SHARE_DOWNLOADED && share.downloaded == 5u && !memcmp(share.data, "hello", 5u));
    assert(book8_share_upload(&share, "upload.txt", "hello", 5u, now)); poll();
    assert(share.command == 5u && g32(share.request + 104u) == 2u);
    b = reply(88u, 0u); p16(b, 89u); memset(b + 64u, 1u, 16u); poll();
    assert(share.command == 9u && g16(share.request + 70u) == 112u && !memcmp(share.request + 116u, "hello", 5u));
    b = reply(16u, 0u); p16(b, 17u); p32(b + 4u, 2u); poll();
    assert(share.command == 9u && share.downloaded == 2u && g32(share.request + 72u) == 3u);
    b = reply(16u, 0u); p16(b, 17u); p32(b + 4u, 3u); poll(); assert(share.command == 6u);
    b = reply(60u, 0u); p16(b, 60u); poll(); assert(share.state == SHARE_UPLOADED);
    assert(!book8_share_upload(&share, "../unsafe", "", 0u, now));
    assert(!book8_share_upload(&share, "large.bin", "", BOOK8_SHARE_DOWNLOAD + 1u, now));
    assert(book8_share_upload(&share, "empty.txt", "", 0u, now)); poll();
    b = reply(88u, 0u); p16(b, 89u); poll(); assert(share.command == 6u);
    b = reply(60u, 0u); p16(b, 60u); poll(); assert(share.state == SHARE_UPLOADED);
    assert(!book8_share_directory(&share, "..\\secret", now) && share.state == SHARE_FAILED);
    start(); b = reply(8u, 0u); p16(b, 9u); p16(b + 2u, 65535u); p32(b + 4u, 16u);
    poll(); assert(share.state == SHARE_FAILED);
    start(); b = reply(88u, 0u); p16(b, 9u); p16(b + 2u, 72u); p32(b + 4u, 80u);
    entry(b + 8u, "test.c", false, 1u); b[73] = 1u;
    poll(); assert(share.state == SHARE_FAILED);
    start(); share.tcp.rx[0] = 1u; share.tcp.rx_length = 4u;
    poll(); assert(share.state == SHARE_FAILED);
    start(); reply(8u, 0xc0000022u); poll(); assert(share.state == SHARE_FAILED && strstr(share.error, "Access denied"));
    start(); book8_share_poll(&share, now + 15001u); assert(share.state == SHARE_FAILED && strstr(share.error, "timeout"));
    book8_share_close(&share);
    assert(!book8_share_connect(&share, &net, 1u, "../bad", now));
    assert(book8_share_connect_auth(&share, &net, 1u, "public", "User", "Domain", "Password", now));
    poll(); b = reply(64u, 0u); p16(b, 65u); p16(b + 2u, 3u); p16(b + 4u, 0x0210u);
    p32(b + 28u, 65536u); p32(b + 32u, 65536u); poll(); assert(share.signing);
    b = reply(72u, 0xc0000016u); p16(b, 9u); p16(b + 4u, 72u); p16(b + 6u, 64u);
    uint8_t *token = b + 8u; memcpy(token, "NTLMSSP", 7u); p32(token + 8u, 2u);
    p32(token + 20u, 0x00888215u); p16(token + 40u, 16u); p32(token + 44u, 48u);
    p16(token + 48u, 7u); p16(token + 50u, 8u); p64(token + 52u, 123u); poll();
    assert(share.command == 1u && !share.password[0] && share.auth_size > 64u);
    b = reply(8u, 0u); p16(b, 9u); poll(); assert(share.command == 3u && share.authenticated);
    assert(g32(share.request + 20u) == 8u);
    uint8_t mac[32], request[1400]; memcpy(request, share.request, share.request_length); memset(request + 52u, 0u, 16u);
    book8_hmac_sha256(share.session_key, 16u, request + 4u, share.request_length - 4u, mac);
    assert(!memcmp(mac, share.request + 52u, 16u));
    b = reply(16u, 0u); p16(b, 16u); b[2] = 1u; sign_reply(); poll(); assert(share.command == 5u);
    b = reply(88u, 0u); p16(b, 89u); sign_reply(); share.tcp.rx[84] ^= 1u;
    poll(); assert(share.state == SHARE_FAILED && strstr(share.error, "signature"));
    assert(book8_share_connect_auth(&share, &net, 1u, "public", "User", "", "bad", now));
    poll(); b = reply(64u, 0u); p16(b, 65u); p16(b + 4u, 0x0210u); p32(b + 28u, 8192u); p32(b + 32u, 8192u);
    poll(); reply(8u, 0xc000006du); poll(); assert(share.auth_failed && strstr(share.error, "Login failed") && !share.password[0]);
    puts("PASS: SMB2 guest/NTLMv2 sessions, signing/tamper rejection, metadata, download/upload, partial writes, bounds and errors");
    return 0;
}
