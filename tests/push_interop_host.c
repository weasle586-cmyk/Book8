/* Interop harness: runs the real push agent (net/push.c) behind stdin/stdout so
 * tests/push_client_test.py can check book8_push.py against it.
 *   argv[1] = 64 hex key digits
 *   stdin   = one line per datagram: "<source-port> <hex payload>", or
 *             "tick <ms>" to advance the clock
 *   stdout  = "REQ <cmd> <flags> <ip> <mask> <gw> <tel> <size> <sha-hex> <remote> <local>"
 *             for every request the agent accepted (completed OK with a message
 *             echoing the command), then "OUT <dest-port> <hex>" for each UDP
 *             reply, then "END" per input line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/push.h"

#define LOCAL BOOK8_NET_IP(192, 168, 50, 208)
#define PEER BOOK8_NET_IP(192, 168, 50, 17)

static const uint8_t local_mac[6] = {2u, 0u, 0u, 0u, 0u, 1u};
static const uint8_t peer_mac[6] = {2u, 0u, 0u, 0u, 0u, 2u};
static struct book8_net net;
static uint8_t frame[BOOK8_NET_FRAME_MAX];
static uint64_t clock_ms = 1000u;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8u); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16u)); put16(p + 2u, (uint16_t)v); }

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0u; i < n; ++i) {
        printf("%02x", p[i]);
    }
}

static enum book8_net_result on_send(void *ctx, const void *data, size_t length)
{
    const uint8_t *f = data;
    (void)ctx;
    if (length >= 42u && f[12] == 0x08u && f[13] == 0x00u && f[23] == 17u) {
        printf("OUT %u ", (unsigned)(f[36] * 256u + f[37]));
        hex(f + 42u, length - 42u);
        printf("\n");
    }
    return BOOK8_NET_OK;
}

static enum book8_net_result on_receive(void *ctx, void *data, size_t cap, size_t *len)
{
    (void)ctx; (void)data; (void)cap; (void)len;
    return BOOK8_NET_EMPTY;
}

static const struct book8_net_transport transport = {
    .ctx = NULL, .send = on_send, .receive = on_receive
};
static uint64_t ms(void *ctx) { (void)ctx; return clock_ms; }
static void entropy(void *ctx, uint8_t out[32])
{
    (void)ctx;
    for (unsigned i = 0u; i < 32u; ++i) {
        out[i] = (uint8_t)(rand() ^ (int)clock_ms);
    }
}
static const struct book8_push_platform platform = {
    .ctx = NULL, .milliseconds = ms, .entropy = entropy
};

static int unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s);
    if (n % 2u != 0u || n / 2u > cap) {
        return -1;
    }
    for (size_t i = 0u; i < n / 2u; ++i) {
        unsigned v;
        if (sscanf(s + 2u * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2u);
}

static void deliver(uint16_t sport, const uint8_t *payload, size_t length)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac, 6u);
    memcpy(frame + 6u, peer_mac, 6u);
    put16(frame + 12u, 0x0800u);
    frame[14] = 0x45u;
    put16(frame + 16u, (uint16_t)(28u + length));
    frame[22] = 64u;
    frame[23] = 17u;
    put32(frame + 26u, PEER);
    put32(frame + 30u, LOCAL);
    put16(frame + 24u, book8_net_checksum(frame + 14u, 20u));
    put16(frame + 34u, sport);
    put16(frame + 36u, BOOK8_PUSH_PORT);
    put16(frame + 38u, (uint16_t)(8u + length));
    memcpy(frame + 42u, payload, length);
    (void)book8_net_input(&net, frame, 42u + length);
}

static void learn_peer(void)
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
    put32(frame + 28u, PEER);
    memcpy(frame + 32u, local_mac, 6u);
    put32(frame + 38u, LOCAL);
    (void)book8_net_input(&net, frame, 42u);
}

int main(int argc, char **argv)
{
    static char line[4096];
    static uint8_t payload[2048];
    uint8_t key[BOOK8_PUSH_KEY_BYTES];
    if (argc != 2 || unhex(argv[1], key, sizeof(key)) != (int)sizeof(key)) {
        fprintf(stderr, "usage: %s <64 hex key digits>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (book8_net_init(&net, &transport, local_mac) != BOOK8_NET_OK
        || book8_net_configure(&net, LOCAL, BOOK8_NET_IP(255, 255, 255, 0), 0u) != BOOK8_NET_OK) {
        return 1;
    }
    learn_peer();
    if (book8_push_init(&net, key, 0x20260501u, &platform) != BOOK8_NET_OK) {
        return 1;
    }
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char text[sizeof(line)];
        unsigned port;
        unsigned long long tick;
        line[strcspn(line, "\r\n")] = '\0';
        if (sscanf(line, "tick %llu", &tick) == 1) {
            clock_ms += tick;
        } else if (sscanf(line, "%u %4095s", &port, text) == 2) {
            int n = unhex(text, payload, sizeof(payload));
            if (n > 0) {
                deliver((uint16_t)port, payload, (size_t)n);
            }
        }
        struct book8_push_request r;
        while (book8_push_take(&r)) {
            char message[96];
            printf("REQ %u %u %u %u %u %u %u ", r.command, r.flags, r.ip, r.mask,
                r.gateway, r.telemetry, r.size);
            hex(r.sha256, sizeof(r.sha256));
            printf(" %s %s\n", r.remote[0] ? r.remote : "-", r.local[0] ? r.local : "-");
            snprintf(message, sizeof(message), "echo %s", book8_push_command_name(r.command));
            (void)book8_push_complete(BOOK8_PUSH_OK, message);
        }
        printf("END\n");
    }
    return 0;
}
