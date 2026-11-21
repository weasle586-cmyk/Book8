#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/push.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define LOCAL BOOK8_NET_IP(192, 168, 50, 208)
#define PEER BOOK8_NET_IP(192, 168, 50, 17)
#define OTHER BOOK8_NET_IP(192, 168, 50, 99)
#define MASK BOOK8_NET_IP(255, 255, 255, 0)
#define CLIENT_PORT 50000u

static const uint8_t local_mac[6] = {2u, 0u, 0u, 0u, 0u, 1u};
static const uint8_t peer_mac[6] = {2u, 0u, 0u, 0u, 0u, 2u};
static struct book8_net net;
static uint8_t frame[BOOK8_NET_FRAME_MAX];
static uint8_t sent[BOOK8_NET_FRAME_MAX];
static size_t sent_length;
static unsigned sends;
static uint64_t clock_ms;
static uint8_t key[BOOK8_PUSH_KEY_BYTES];
static bool fail_send;

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] * 256u + p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16u) | get16(p + 2u); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8u); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16u)); put16(p + 2u, (uint16_t)v); }

static enum book8_net_result fake_send(void *ctx, const void *data, size_t length)
{
    (void)ctx;
    if (fail_send) {
        return BOOK8_NET_IO_ERROR;
    }
    memcpy(sent, data, length);
    sent_length = length;
    ++sends;
    return BOOK8_NET_OK;
}

static enum book8_net_result fake_receive(void *ctx, void *data, size_t cap, size_t *len)
{
    (void)ctx; (void)data; (void)cap; (void)len;
    return BOOK8_NET_EMPTY;
}

static const struct book8_net_transport transport = {
    .ctx = NULL, .send = fake_send, .receive = fake_receive
};

static uint64_t fake_ms(void *ctx) { (void)ctx; return clock_ms; }
static void fake_entropy(void *ctx, uint8_t out[32])
{
    (void)ctx;
    for (unsigned i = 0u; i < 32u; ++i) {
        out[i] = (uint8_t)(i * 7u + clock_ms);
    }
}

static const struct book8_push_platform platform = {
    .ctx = NULL, .milliseconds = fake_ms, .entropy = fake_entropy
};

static void deliver(uint32_t source, uint16_t sport, const uint8_t *payload, size_t length)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac, 6u);
    memcpy(frame + 6u, peer_mac, 6u);
    put16(frame + 12u, 0x0800u);
    frame[14] = 0x45u;
    put16(frame + 16u, (uint16_t)(28u + length));
    frame[22] = 64u;
    frame[23] = 17u;
    put32(frame + 26u, source);
    put32(frame + 30u, LOCAL);
    put16(frame + 24u, book8_net_checksum(frame + 14u, 20u));
    put16(frame + 34u, sport);
    put16(frame + 36u, BOOK8_PUSH_PORT);
    put16(frame + 38u, (uint16_t)(8u + length));
    memcpy(frame + 42u, payload, length);
    sent_length = 0u;
    CHECK(book8_net_input(&net, frame, 42u + length) == BOOK8_NET_OK);
}

static void learn_arp(uint32_t ip)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac, 6u);
    memcpy(frame + 6u, peer_mac, 6u);
    put16(frame + 12u, 0x0806u);
    put16(frame + 14u, 1u);
    put16(frame + 16u, 0x0800u);
    frame[18] = 6u;
    frame[19] = 4u;
    put16(frame + 20u, 2u);
    memcpy(frame + 22u, peer_mac, 6u);
    put32(frame + 28u, ip);
    memcpy(frame + 32u, local_mac, 6u);
    put32(frame + 38u, LOCAL);
    CHECK(book8_net_input(&net, frame, 42u) == BOOK8_NET_OK);
}

static const uint8_t *reply(size_t expected)
{
    CHECK(sent_length == 42u + expected);
    CHECK(get16(sent + 34u) == BOOK8_PUSH_PORT && get16(sent + 36u) == CLIENT_PORT);
    return sent + 42u;
}

static void header(uint8_t *p, uint8_t type, uint16_t length, uint32_t id, const uint8_t *nonce)
{
    put32(p, BOOK8_PUSH_MAGIC);
    p[4] = 1u;
    p[5] = type;
    put16(p + 6u, length);
    put32(p + 8u, id);
    memcpy(p + 12u, nonce, 16u);
}

static void hello(uint32_t source, const uint8_t client_nonce[16], uint32_t id)
{
    uint8_t p[28];
    header(p, BOOK8_PUSH_HELLO, 0u, id, client_nonce);
    deliver(source, CLIENT_PORT, p, sizeof(p));
}

/* Returns the server nonce after authenticating the CHALLENGE. */
static void challenge(const uint8_t client_nonce[16], uint8_t server_nonce[16])
{
    hello(PEER, client_nonce, 7u);
    const uint8_t *p = reply(BOOK8_PUSH_CHALLENGE_BYTES + 32u);
    CHECK(get32(p) == BOOK8_PUSH_MAGIC && p[5] == BOOK8_PUSH_CHALLENGE);
    CHECK(get16(p + 6u) == 8u && get32(p + 8u) == 7u);
    CHECK(get32(p + 28u) == 0x20260501u && get32(p + 32u) == BOOK8_PUSH_CHALLENGE_MS);
    uint8_t buffer[36 + 16], mac[32];
    memcpy(buffer, p, 36u);
    memcpy(buffer + 36u, client_nonce, 16u);
    book8_hmac_sha256(key, sizeof(key), buffer, sizeof(buffer), mac);
    CHECK(memcmp(mac, p + 36u, 32u) == 0);
    memcpy(server_nonce, p + 12u, 16u);
}

static void command(uint8_t *p, const uint8_t nonce[16], uint32_t id, uint8_t code,
    const char *remote, const char *local, uint32_t size)
{
    memset(p, 0, BOOK8_PUSH_COMMAND_BYTES);
    header(p, BOOK8_PUSH_COMMAND, 136u, id, nonce);
    uint8_t *b = p + 28u;
    b[0] = code;
    b[1] = BOOK8_PUSH_FLAG_PERSIST;
    put32(b + 4u, LOCAL);
    put32(b + 8u, MASK);
    put32(b + 12u, BOOK8_NET_IP(192, 168, 50, 1));
    put32(b + 16u, PEER);
    put32(b + 20u, size);
    for (unsigned i = 0u; i < 32u; ++i) {
        b[24u + i] = (uint8_t)(0xa0u + i);
    }
    if (remote != NULL) {
        memcpy(b + 56u, remote, strlen(remote));
    }
    if (local != NULL) {
        memcpy(b + 120u, local, strlen(local));
    }
    book8_hmac_sha256(key, sizeof(key), p, 164u, p + 164u);
}

static void check_result(const uint8_t *p, const uint8_t nonce[16], uint32_t id,
    uint8_t code, uint8_t status, const char *message)
{
    uint8_t mac[32];
    CHECK(get32(p) == BOOK8_PUSH_MAGIC && p[5] == BOOK8_PUSH_RESULT);
    CHECK(get16(p + 6u) == 104u && get32(p + 8u) == id);
    CHECK(memcmp(p + 12u, nonce, 16u) == 0);
    CHECK(p[28] == code && p[29] == status && get32(p + 32u) == 0x20260501u);
    CHECK(strncmp((const char *)p + 36u, message, 96u) == 0);
    book8_hmac_sha256(key, sizeof(key), p, 132u, mac);
    CHECK(memcmp(mac, p + 132u, 32u) == 0);
}

int main(void)
{
    struct book8_push_request request;
    struct book8_push_stats stats;
    uint8_t cn[16], sn[16], sn2[16], cmd[BOOK8_PUSH_COMMAND_BYTES];
    for (unsigned i = 0u; i < 32u; ++i) {
        key[i] = (uint8_t)(i * 13u + 1u);
    }
    for (unsigned i = 0u; i < 16u; ++i) {
        cn[i] = (uint8_t)(0x40u + i);
    }
    CHECK(sizeof(struct book8_push_command) == 136u);
    CHECK(BOOK8_PUSH_COMMAND_BYTES == 196u && BOOK8_PUSH_RESULT_BYTES == 164u);
    CHECK(book8_net_init(&net, &transport, local_mac) == BOOK8_NET_OK);
    CHECK(book8_net_configure(&net, LOCAL, MASK, 0u) == BOOK8_NET_OK);
    learn_arp(PEER);
    learn_arp(OTHER);
    CHECK(book8_push_init(&net, NULL, 1u, &platform) == BOOK8_NET_INVALID);
    CHECK(book8_push_init(&net, key, 0x20260501u, &platform) == BOOK8_NET_OK);
    CHECK(book8_udp_bind(&net, BOOK8_PUSH_PORT, NULL, NULL) != BOOK8_NET_OK);
    CHECK(!book8_push_take(&request));
    CHECK(book8_push_complete(BOOK8_PUSH_OK, "x") == BOOK8_NET_INVALID);

    /* Happy path: FILE command is queued, taken, completed, then dedup'ed. */
    clock_ms = 1000u;
    challenge(cn, sn);
    command(cmd, sn, 42u, BOOK8_PUSH_FILE, "tools/APP.ELF", "APP.ELF", 1234u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(sent_length == 0u);
    book8_push_get_stats(&stats);
    CHECK(stats.accepted == 1u && stats.request_pending && stats.challenges == 1u);
    CHECK(book8_push_take(&request) && !book8_push_take(&request));
    CHECK(request.command == BOOK8_PUSH_FILE && request.request_id == 42u);
    CHECK(request.source_ip == PEER && request.source_port == CLIENT_PORT);
    CHECK(request.flags == 1u && request.ip == LOCAL && request.mask == MASK);
    CHECK(request.gateway == BOOK8_NET_IP(192, 168, 50, 1) && request.telemetry == PEER);
    CHECK(request.size == 1234u && request.sha256[31] == 0xbfu);
    CHECK(strcmp(request.remote, "tools/APP.ELF") == 0 && strcmp(request.local, "APP.ELF") == 0);
    /* Retransmission while running: no second execution and no reply. */
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(sent_length == 0u && !book8_push_take(&request));
    /* HELLO while running gets a signed BUSY bound to the client nonce. */
    hello(OTHER, cn, 9u);
    CHECK(sent_length == 42u + BOOK8_PUSH_RESULT_BYTES);
    check_result(sent + 42u, cn, 9u, BOOK8_PUSH_FILE, BOOK8_PUSH_BUSY, "update agent busy");
    CHECK(book8_push_complete(BOOK8_PUSH_OK, "wrote A:\\APP.ELF") == BOOK8_NET_OK);
    check_result(reply(BOOK8_PUSH_RESULT_BYTES), sn, 42u, BOOK8_PUSH_FILE,
        BOOK8_PUSH_OK, "wrote A:\\APP.ELF");
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    check_result(reply(BOOK8_PUSH_RESULT_BYTES), sn, 42u, BOOK8_PUSH_FILE,
        BOOK8_PUSH_OK, "wrote A:\\APP.ELF");
    CHECK(!book8_push_take(&request));
    /* A different request id with the spent nonce is a replay. */
    command(cmd, sn, 43u, BOOK8_PUSH_REBOOT, NULL, NULL, 0u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(sent_length == 0u && !book8_push_take(&request));
    book8_push_get_stats(&stats);
    CHECK(stats.rejected == 1u && stats.busy_replies == 1u);

    /* Fresh nonce each challenge; bad MAC, wrong source and wrong nonce are dropped. */
    challenge(cn, sn2);
    CHECK(memcmp(sn, sn2, 16u) != 0);
    command(cmd, sn2, 50u, BOOK8_PUSH_STATUS, NULL, NULL, 0u);
    cmd[100] ^= 1u;
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    command(cmd, sn2, 50u, BOOK8_PUSH_STATUS, NULL, NULL, 0u);
    deliver(OTHER, CLIENT_PORT, cmd, sizeof(cmd));
    deliver(PEER, CLIENT_PORT + 1u, cmd, sizeof(cmd));
    command(cmd, sn, 50u, BOOK8_PUSH_STATUS, NULL, NULL, 0u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(sent_length == 0u && !book8_push_take(&request));
    book8_push_get_stats(&stats);
    CHECK(stats.rejected == 5u && stats.accepted == 1u);
    /* The challenge survived the forgeries and still works. */
    command(cmd, sn2, 50u, BOOK8_PUSH_STATUS, NULL, NULL, 0u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(book8_push_take(&request) && request.command == BOOK8_PUSH_STATUS);
    /* ARP/IO failure: the RESULT is retried by service until the window ends. */
    fail_send = true;
    CHECK(book8_push_complete(BOOK8_PUSH_OK, "status ok") != BOOK8_NET_OK);
    book8_push_get_stats(&stats);
    CHECK(stats.result_unsent);
    clock_ms += 1000u;
    CHECK(book8_push_service() != BOOK8_NET_OK);
    fail_send = false;
    CHECK(book8_push_service() == BOOK8_NET_OK);
    check_result(reply(BOOK8_PUSH_RESULT_BYTES), sn2, 50u, BOOK8_PUSH_STATUS,
        BOOK8_PUSH_OK, "status ok");
    CHECK(book8_push_service() == BOOK8_NET_EMPTY);
    fail_send = true;
    hello(PEER, cn, 7u);
    fail_send = false;
    CHECK(sent_length == 0u);
    challenge(cn, sn);
    command(cmd, sn, 51u, BOOK8_PUSH_STATUS, NULL, NULL, 0u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(book8_push_take(&request));
    fail_send = true;
    (void)book8_push_complete(BOOK8_PUSH_OK, "late");
    clock_ms += BOOK8_PUSH_RESULT_RETRY_MS + 1u;
    CHECK(book8_push_service() == BOOK8_NET_TIMEOUT);
    CHECK(book8_push_service() == BOOK8_NET_EMPTY);
    fail_send = false;

    /* Expired challenge. */
    challenge(cn, sn);
    clock_ms += BOOK8_PUSH_CHALLENGE_MS + 1u;
    command(cmd, sn, 60u, BOOK8_PUSH_REBOOT, NULL, NULL, 0u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(sent_length == 0u && !book8_push_take(&request));
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    CHECK(!book8_push_take(&request));

    /* Authenticated but malformed bodies are answered BAD_REQUEST, never queued. */
    const struct { uint8_t code; const char *remote, *local; uint32_t size; } bad[] = {
        {9u, NULL, NULL, 0u},
        {BOOK8_PUSH_FILE, "x.bin", NULL, 4u},
        {BOOK8_PUSH_FILE, NULL, "X.BIN", 4u},
        {BOOK8_PUSH_KERNEL, "book8.elf", NULL, 0u},
        {BOOK8_PUSH_KERNEL, "bad name", NULL, 4u}
    };
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        challenge(cn, sn);
        command(cmd, sn, 70u + i, bad[i].code, bad[i].remote, bad[i].local, bad[i].size);
        deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
        check_result(reply(BOOK8_PUSH_RESULT_BYTES), sn, 70u + i, bad[i].code,
            BOOK8_PUSH_BAD_REQUEST, "malformed command body");
        CHECK(!book8_push_take(&request));
    }
    /* Non-zero padding after the string terminator is malformed even with a valid MAC. */
    challenge(cn, sn);
    command(cmd, sn, 80u, BOOK8_PUSH_KERNEL, "book8.elf", NULL, 4u);
    cmd[28u + 56u + 63u] = 'Z';
    book8_hmac_sha256(key, sizeof(key), cmd, 164u, cmd + 164u);
    deliver(PEER, CLIENT_PORT, cmd, sizeof(cmd));
    check_result(reply(BOOK8_PUSH_RESULT_BYTES), sn, 80u, BOOK8_PUSH_KERNEL,
        BOOK8_PUSH_BAD_REQUEST, "malformed command body");

    /* Framing errors. */
    uint8_t junk[28] = {0};
    deliver(PEER, CLIENT_PORT, junk, sizeof(junk));
    header(junk, BOOK8_PUSH_HELLO, 1u, 1u, cn);
    deliver(PEER, CLIENT_PORT, junk, sizeof(junk));
    header(junk, BOOK8_PUSH_RESULT, 0u, 1u, cn);
    deliver(PEER, CLIENT_PORT, junk, sizeof(junk));
    deliver(PEER, CLIENT_PORT, junk, 10u);
    CHECK(sent_length == 0u);
    CHECK(strcmp(book8_push_command_name(BOOK8_PUSH_KERNEL), "KERNEL") == 0);
    CHECK(strcmp(book8_push_command_name(200u), "UNKNOWN") == 0);

    CHECK(book8_push_init(NULL, NULL, 0u, NULL) == BOOK8_NET_OK);
    book8_push_get_stats(&stats);
    CHECK(!stats.listening && stats.accepted == 0u);
    CHECK(book8_udp_bind(&net, BOOK8_PUSH_PORT, NULL, NULL) == BOOK8_NET_INVALID
        || book8_udp_unbind(&net, BOOK8_PUSH_PORT) == BOOK8_NET_NOT_BOUND);
    puts("PASS: authenticated push agent HELLO/CHALLENGE/COMMAND/RESULT, replay and expiry");
    return 0;
}
