#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/net.h"
#include "../drivers/rtl8168.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define LOCAL BOOK8_NET_IP(192, 168, 7, 10)
#define PEER BOOK8_NET_IP(192, 168, 7, 20)
#define GATEWAY BOOK8_NET_IP(192, 168, 7, 1)
#define MASK BOOK8_NET_IP(255, 255, 255, 0)

static const uint8_t local_mac[6] = {2u, 0u, 0u, 0u, 0u, 1u};
static const uint8_t peer_mac[6] = {2u, 0u, 0u, 0u, 0u, 2u};
static const uint8_t broadcast[6] = {255u, 255u, 255u, 255u, 255u, 255u};
static struct book8_net net;
static uint8_t sent[BOOK8_NET_FRAME_MAX], frame[BOOK8_NET_FRAME_MAX + 1u];
static size_t sent_length, frame_length;
static unsigned sends, callbacks, queued, received;
static bool reply_callback;
static enum book8_net_result send_result, receive_result;
static bool telemetry_reentry;

/* Independent byte-oriented wire oracle (no packed struct/endian helpers). */
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] * 256u + p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)get16(p) << 16u) | get16(p + 2u);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8u);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v >> 16u));
    put16(p + 2u, (uint16_t)v);
}

static uint16_t wire_checksum(const uint8_t *p, size_t size, uint32_t sum)
{
    for (size_t i = 0u; i < size; ++i) {
        sum += (uint32_t)p[i] << (i % 2u == 0u ? 8u : 0u);
    }
    sum = (sum & 65535u) + (sum >> 16u);
    sum = (sum & 65535u) + (sum >> 16u);
    return (uint16_t)(sum ^ 65535u);
}

static uint16_t wire_udp_checksum(const uint8_t *f)
{
    uint32_t source = get32(f + 26u), destination = get32(f + 30u);
    size_t length = get16(f + 38u);
    return wire_checksum(f + 34u, length, (source >> 16u) + (source & 65535u)
        + (destination >> 16u) + (destination & 65535u) + 17u + length);
}

static enum book8_net_result fake_send(void *ctx, const void *data, size_t length)
{
    CHECK(ctx == &net);
    CHECK(length >= 14u && length <= sizeof(sent));
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, NULL, 0u) == BOOK8_NET_BUSY);
    if (telemetry_reentry) {
        CHECK(net_telemetry_send(0u, NULL, NULL) == BOOK8_NET_BUSY);
        CHECK(net_telemetry_init(&net, 0u) == BOOK8_NET_BUSY);
    }
    if (send_result != BOOK8_NET_OK) {
        return send_result;
    }
    memcpy(sent, data, length);
    sent_length = length;
    ++sends;
    return BOOK8_NET_OK;
}

static enum book8_net_result fake_receive(void *ctx, void *data,
    size_t capacity, size_t *length)
{
    CHECK(ctx == &net && capacity == BOOK8_NET_FRAME_MAX);
    ++received;
    if (receive_result != BOOK8_NET_OK) {
        enum book8_net_result result = receive_result;
        receive_result = BOOK8_NET_OK;
        return result;
    }
    if (queued == 0u) {
        return BOOK8_NET_EMPTY;
    }
    --queued;
    CHECK(frame_length <= capacity);
    memcpy(data, frame, frame_length);
    *length = frame_length;
    return BOOK8_NET_OK;
}

static const struct book8_net_transport transport = {
    .ctx = &net, .send = fake_send, .receive = fake_receive
};

static void udp_callback(void *ctx, uint32_t ip, uint16_t port,
    const uint8_t *payload, size_t length)
{
    CHECK(ctx == &callbacks && ip == PEER && port == 4321u);
    CHECK(length == 5u && memcmp(payload, "hello", length) == 0);
    ++callbacks;
    CHECK(book8_net_poll(&net) == BOOK8_NET_BUSY);
    CHECK(book8_net_configure(&net, LOCAL, MASK, GATEWAY) == BOOK8_NET_BUSY);
    if (reply_callback) {
        CHECK(book8_udp_send(&net, 1234u, ip, port, payload, length) == BOOK8_NET_OK);
    }
}

static void setup(void)
{
    CHECK(book8_net_init(&net, &transport, local_mac) == BOOK8_NET_OK);
    CHECK(book8_net_configure(&net, LOCAL, MASK, GATEWAY) == BOOK8_NET_OK);
    memset(sent, 0, sizeof(sent));
    memset(frame, 0, sizeof(frame));
    sends = callbacks = queued = received = 0u;
    sent_length = frame_length = 0u;
    send_result = receive_result = BOOK8_NET_OK;
    reply_callback = false;
}

static void ethernet(uint16_t type, const uint8_t destination[6])
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, destination, 6u);
    memcpy(frame + 6u, peer_mac, 6u);
    put16(frame + 12u, type);
}

static void arp_frame(uint16_t operation, uint32_t source, uint32_t target)
{
    ethernet(0x0806u, operation == 1u ? broadcast : local_mac);
    put16(frame + 14u, 1u);
    put16(frame + 16u, 0x0800u);
    frame[18u] = 6u;
    frame[19u] = 4u;
    put16(frame + 20u, operation);
    memcpy(frame + 22u, peer_mac, 6u);
    put32(frame + 28u, source);
    if (operation == 2u) {
        memcpy(frame + 32u, local_mac, 6u);
    }
    put32(frame + 38u, target);
    frame_length = 42u;
}

static void ip_checksum(void)
{
    put16(frame + 24u, 0u);
    put16(frame + 24u, wire_checksum(frame + 14u, (frame[14u] & 15u) * 4u, 0u));
}

static void ip_frame(uint8_t protocol, size_t length)
{
    ethernet(0x0800u, local_mac);
    frame[14u] = 0x45u;
    put16(frame + 16u, (uint16_t)(20u + length));
    put16(frame + 18u, 0x1234u);
    put16(frame + 20u, 0x4000u);
    frame[22u] = 32u;
    frame[23u] = protocol;
    put32(frame + 26u, PEER);
    put32(frame + 30u, LOCAL);
    ip_checksum();
    frame_length = 34u + length;
}

static void udp_frame(bool checksum)
{
    ip_frame(17u, 13u);
    put16(frame + 34u, 4321u);
    put16(frame + 36u, 1234u);
    put16(frame + 38u, 13u);
    memcpy(frame + 42u, "hello", 5u);
    if (checksum) {
        put16(frame + 40u, wire_udp_checksum(frame));
    }
}

static void echo_frame(size_t length)
{
    ip_frame(1u, length);
    frame[34u] = 8u;
    put16(frame + 38u, 0x1234u);
    put16(frame + 40u, 0x5678u);
    for (size_t i = 8u; i < length; ++i) {
        frame[34u + i] = (uint8_t)i;
    }
    put16(frame + 36u, wire_checksum(frame + 34u, length, 0u));
}

static void check_output_ip(uint32_t destination, uint8_t protocol)
{
    CHECK(get16(sent + 12u) == 0x0800u);
    CHECK(memcmp(sent, peer_mac, 6u) == 0);
    CHECK(memcmp(sent + 6u, local_mac, 6u) == 0);
    CHECK(sent[14u] == 0x45u && sent[22u] == 64u && sent[23u] == protocol);
    CHECK(get16(sent + 16u) == sent_length - 14u);
    CHECK(get32(sent + 26u) == LOCAL && get32(sent + 30u) == destination);
    CHECK(wire_checksum(sent + 14u, 20u, 0u) == 0u);
}

static void configuration_tests(void)
{
    setup();
    CHECK(book8_ntohs(0x3412u) == 0x1234u);
    CHECK(book8_ntohl(0x04030201u) == 0x01020304u);
    uint8_t check[] = {0x00u, 0x01u, 0xf2u, 0x03u, 0xf4u, 0xf5u, 0xf6u, 0xf7u};
    CHECK(book8_net_checksum(check, sizeof(check)) == 0x220du);
    CHECK(book8_net_checksum("abc", 3u) == 0x3b9du);
    uint32_t ip = 99u;
    CHECK(book8_net_parse_ip("192.168.7.10", &ip) && ip == LOCAL);
    CHECK(book8_net_parse_ip("0.0.0.0", &ip) && ip == 0u);
    const char *bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.2.3", "-1.2.3.4",
        "1..2.3", "1.2.3.4 ", " 1.2.3.4", "0000.1.2.3", "a.b.c.d"};
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        ip = 99u;
        CHECK(!book8_net_parse_ip(bad[i], &ip) && ip == 99u);
    }
    CHECK(!book8_net_parse_ip(NULL, &ip));
    CHECK(book8_net_configure(&net, LOCAL, 0u, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, LOCAL, 0xff00ff00u, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, LOCAL, 0xfffffffeu, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, LOCAL & MASK, MASK, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, LOCAL | ~MASK, MASK, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, LOCAL, MASK, BOOK8_NET_IP(10, 0, 0, 1))
        == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, LOCAL, MASK, LOCAL) == BOOK8_NET_INVALID);
    CHECK(net.ip == LOCAL && net.mask == MASK && net.gateway == GATEWAY);
    CHECK(book8_net_init(NULL, &transport, local_mac) == BOOK8_NET_INVALID);
    CHECK(book8_net_init(&net, NULL, local_mac) == BOOK8_NET_INVALID);
    CHECK(book8_net_init(&net, &transport, broadcast) == BOOK8_NET_INVALID);
    struct book8_net_transport broken = transport;
    broken.send = NULL;
    CHECK(book8_net_init(&net, &broken, local_mac) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, 0u, 0u, 0u) == BOOK8_NET_OK);
    arp_frame(1u, PEER, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_UNCONFIGURED);
    CHECK(sends == 0u);
}

static void arp_tests(void)
{
    setup();
    arp_frame(1u, PEER, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(sends == 1u && sent_length == 42u && net.arp_replies == 1u);
    CHECK(get16(sent + 12u) == 0x0806u && get16(sent + 20u) == 2u);
    CHECK(memcmp(sent, peer_mac, 6u) == 0 && memcmp(sent + 6u, local_mac, 6u) == 0);
    CHECK(get16(sent + 14u) == 1u && get16(sent + 16u) == 0x0800u);
    CHECK(sent[18u] == 6u && sent[19u] == 4u);
    CHECK(memcmp(sent + 22u, local_mac, 6u) == 0 && get32(sent + 28u) == LOCAL);
    CHECK(memcmp(sent + 32u, peer_mac, 6u) == 0 && get32(sent + 38u) == PEER);
    CHECK(net.arp[0].ip == PEER && memcmp(net.arp[0].mac, peer_mac, 6u) == 0);
    arp_frame(1u, PEER, LOCAL + 1u);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IGNORED);
    arp_frame(1u, PEER, LOCAL);
    frame[22u] ^= 2u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    arp_frame(1u, PEER, LOCAL);
    frame[18u] = 5u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    arp_frame(2u, PEER, LOCAL);
    frame[32u] ^= 2u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    for (unsigned i = 1u; i <= BOOK8_NET_ARP_COUNT; ++i) {
        arp_frame(2u, PEER + i, LOCAL);
        CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    }
    CHECK(net.arp[0].ip == PEER + BOOK8_NET_ARP_COUNT);
    arp_frame(2u, PEER + 1u, LOCAL);
    frame[11u] = frame[27u] = 3u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(net.arp[1].mac[5u] == 3u);
    CHECK(book8_net_configure(&net, LOCAL, MASK, GATEWAY) == BOOK8_NET_OK);
    for (unsigned i = 0u; i < BOOK8_NET_ARP_COUNT; ++i) {
        CHECK(net.arp[i].ip == 0u);
    }
    arp_frame(1u, 0u, LOCAL); /* Address-conflict probe: respond, do not cache 0. */
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(get32(sent + 38u) == 0u && net.arp[0].ip == 0u);
    send_result = BOOK8_NET_IO_ERROR;
    arp_frame(1u, PEER, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IO_ERROR);
    CHECK(net.tx_errors == 1u);
}

static void icmp_tests(void)
{
    setup();
    echo_frame(11u);
    uint8_t original[11u];
    memcpy(original, frame + 34u, sizeof(original));
    CHECK(book8_net_input(&net, frame, 60u) == BOOK8_NET_OK); /* Ethernet padding. */
    CHECK(sent_length == 45u && sent[34u] == 0u && sent[35u] == 0u);
    check_output_ip(PEER, 1u);
    CHECK(memcmp(sent + 38u, original + 4u, 7u) == 0);
    CHECK(wire_checksum(sent + 34u, 11u, 0u) == 0u && net.echo_replies == 1u);
    echo_frame(1480u);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(sent_length == BOOK8_NET_FRAME_MAX);
    CHECK(wire_checksum(sent + 34u, 1480u, 0u) == 0u);
    frame[36u] ^= 1u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    echo_frame(8u);
    frame[34u] = 0u;
    put16(frame + 36u, 0u);
    put16(frame + 36u, wire_checksum(frame + 34u, 8u, 0u));
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IGNORED);
    echo_frame(8u);
    for (size_t size = 0u; size < frame_length; ++size) {
        CHECK(book8_net_input(&net, frame, size) == BOOK8_NET_INVALID);
    }
    CHECK(book8_net_input(&net, NULL, 60u) == BOOK8_NET_INVALID);
    CHECK(book8_net_input(&net, frame, sizeof(frame)) == BOOK8_NET_INVALID);
}

static void udp_tests(void)
{
    setup();
    CHECK(book8_udp_bind(&net, 0u, udp_callback, &callbacks) == BOOK8_NET_INVALID);
    CHECK(book8_udp_bind(&net, 1234u, NULL, NULL) == BOOK8_NET_INVALID);
    CHECK(book8_udp_bind(&net, 1234u, udp_callback, &callbacks) == BOOK8_NET_OK);
    CHECK(book8_udp_bind(&net, 1234u, udp_callback, &callbacks) == BOOK8_NET_PORT_IN_USE);
    for (unsigned i = 1u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        CHECK(book8_udp_bind(&net, (uint16_t)(1234u + i), udp_callback, NULL) == BOOK8_NET_OK);
    }
    CHECK(book8_udp_bind(&net, 9000u, udp_callback, NULL) == BOOK8_NET_NO_SLOTS);
    CHECK(book8_udp_unbind(&net, 1235u) == BOOK8_NET_OK);
    CHECK(book8_udp_unbind(&net, 1235u) == BOOK8_NET_NOT_BOUND);
    CHECK(book8_udp_unbind(&net, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_udp_send(&net, 9000u, PEER, 4321u, NULL, 0u) == BOOK8_NET_NOT_BOUND);
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, "hello", 5u) == BOOK8_NET_ARP_PENDING);
    CHECK(sent_length == 42u && get16(sent + 20u) == 1u);
    CHECK(memcmp(sent, broadcast, 6u) == 0 && get32(sent + 38u) == PEER);
    const uint8_t zero[6] = {0};
    CHECK(memcmp(sent + 32u, zero, 6u) == 0);
    arp_frame(2u, PEER, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, "hello", 5u) == BOOK8_NET_OK);
    CHECK(sent_length == 47u && get16(sent + 34u) == 1234u);
    CHECK(get16(sent + 36u) == 4321u && get16(sent + 38u) == 13u);
    CHECK(get16(sent + 40u) != 0u && wire_udp_checksum(sent) == 0u);
    CHECK(memcmp(sent + 42u, "hello", 5u) == 0);
    check_output_ip(PEER, 17u);
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, NULL, 0u) == BOOK8_NET_OK);
    CHECK(sent_length == 42u && wire_udp_checksum(sent) == 0u);
    uint8_t payload[BOOK8_NET_UDP_MAX];
    memset(payload, 0x7b, sizeof(payload));
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, payload, sizeof(payload)) == BOOK8_NET_OK);
    CHECK(sent_length == BOOK8_NET_FRAME_MAX && wire_udp_checksum(sent) == 0u);
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, payload, sizeof(payload) + 1u)
        == BOOK8_NET_INVALID);
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, NULL, 1u) == BOOK8_NET_INVALID);
    CHECK(book8_udp_send(&net, 1234u, PEER, 0u, NULL, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_udp_send(&net, 1234u, LOCAL | ~MASK, 4321u, NULL, 0u) == BOOK8_NET_INVALID);
    CHECK(book8_udp_send(&net, 1234u, BOOK8_NET_IP(224, 0, 0, 1), 4321u, NULL, 0u)
        == BOOK8_NET_INVALID);

    uint8_t special[2] = {0};
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, special, 2u) == BOOK8_NET_OK);
    put16(sent + 40u, 0u);
    put16(special, wire_udp_checksum(sent));
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, special, 2u) == BOOK8_NET_OK);
    CHECK(get16(sent + 40u) == 0xffffu && wire_udp_checksum(sent) == 0u);

    udp_frame(true);
    reply_callback = true;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(callbacks == 1u && net.udp_received == 1u);
    CHECK(memcmp(sent + 42u, "hello", 5u) == 0 && wire_udp_checksum(sent) == 0u);
    reply_callback = false;
    udp_frame(false); /* IPv4 permits absent UDP checksums. */
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(callbacks == 2u);
    udp_frame(true);
    frame[46u] ^= 1u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    put16(frame + 38u, 12u);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    put16(frame + 36u, 9000u);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IGNORED);
    CHECK(callbacks == 2u);

    uint32_t remote = BOOK8_NET_IP(10, 0, 0, 20);
    CHECK(book8_udp_send(&net, 1234u, remote, 4321u, NULL, 0u) == BOOK8_NET_ARP_PENDING);
    CHECK(get32(sent + 38u) == GATEWAY);
    arp_frame(2u, GATEWAY, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(book8_udp_send(&net, 1234u, remote, 4321u, NULL, 0u) == BOOK8_NET_OK);
    check_output_ip(remote, 17u);
    CHECK(wire_udp_checksum(sent) == 0u);
    send_result = BOOK8_NET_IO_ERROR;
    CHECK(book8_udp_send(&net, 1234u, remote, 4321u, NULL, 0u) == BOOK8_NET_IO_ERROR);
    CHECK(net.tx_errors == 1u);
    CHECK(book8_net_configure(&net, LOCAL, MASK, 0u) == BOOK8_NET_OK);
    CHECK(book8_udp_send(&net, 1234u, remote, 4321u, NULL, 0u) == BOOK8_NET_NO_ROUTE);
    CHECK(book8_net_configure(&net, 0u, 0u, 0u) == BOOK8_NET_OK);
    CHECK(book8_udp_send(&net, 1234u, PEER, 4321u, NULL, 0u) == BOOK8_NET_UNCONFIGURED);
}

static void malformed_ip_tests(void)
{
    setup();
    CHECK(book8_udp_bind(&net, 1234u, udp_callback, &callbacks) == BOOK8_NET_OK);
    const uint16_t fragments[] = {0x2000u, 1u, 0x8000u};
    for (unsigned i = 0u; i < sizeof(fragments) / sizeof(fragments[0]); ++i) {
        udp_frame(true);
        put16(frame + 20u, fragments[i]);
        ip_checksum();
        CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    }
    udp_frame(true);
    frame[24u] ^= 1u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    frame[14u] = 0x65u;
    ip_checksum();
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    frame[14u] = 0x44u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    frame[22u] = 0u;
    ip_checksum();
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    put16(frame + 16u, 19u);
    ip_checksum();
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    put16(frame + 16u, 34u);
    ip_checksum();
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    put32(frame + 30u, LOCAL + 1u);
    ip_checksum();
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IGNORED);
    udp_frame(false);
    memcpy(frame, broadcast, 6u);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IGNORED);
    udp_frame(false);
    frame[6u] = 1u;
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_INVALID);
    udp_frame(false);
    frame[14u] = 0x46u; /* Validly bounded IPv4 options are unsupported. */
    put16(frame + 16u, 24u);
    ip_checksum();
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_IGNORED);
    CHECK(callbacks == 0u);

    /* Unaligned wire input and every truncation boundary under ASan/UBSan. */
    udp_frame(true);
    uint8_t unaligned[BOOK8_NET_FRAME_MAX + 1u];
    memcpy(unaligned + 1u, frame, frame_length);
    CHECK(book8_net_input(&net, unaligned + 1u, frame_length) == BOOK8_NET_OK);
    for (size_t size = 0u; size < frame_length; ++size) {
        CHECK(book8_net_input(&net, unaligned + 1u, size) == BOOK8_NET_INVALID);
    }
}

static void poll_tests(void)
{
    setup();
    CHECK(book8_udp_bind(&net, 1234u, udp_callback, &callbacks) == BOOK8_NET_OK);
    udp_frame(true);
    queued = BOOK8_NET_POLL_BUDGET + 2u;
    CHECK(book8_net_poll(&net) == BOOK8_NET_OK);
    CHECK(callbacks == BOOK8_NET_POLL_BUDGET && queued == 2u);
    CHECK(received == BOOK8_NET_POLL_BUDGET);
    CHECK(book8_net_poll(&net) == BOOK8_NET_OK);
    CHECK(callbacks == BOOK8_NET_POLL_BUDGET + 2u && queued == 0u);
    receive_result = BOOK8_NET_RX_DROPPED;
    CHECK(book8_net_poll(&net) == BOOK8_NET_OK && net.rx_errors == 1u);
    receive_result = BOOK8_NET_IO_ERROR;
    CHECK(book8_net_poll(&net) == BOOK8_NET_IO_ERROR && net.rx_errors == 2u);
    frame[24u] ^= 1u;
    queued = 2u;
    CHECK(book8_net_poll(&net) == BOOK8_NET_OK && net.invalid_frames == 2u);
    CHECK(callbacks == BOOK8_NET_POLL_BUDGET + 2u);
}

static struct rtl8168_device nic;
static struct rtl8168_dma nic_dma;
static uint8_t nic_regs[256u] __attribute__((aligned(8)));
static unsigned nic_tx_waits;

static void complete_tx(void *ctx)
{
    ++nic_tx_waits;
    CHECK(ctx == &nic && nic_regs[0x38u] == 0x40u);
    unsigned index = (nic.tx_next + RTL8168_RING_COUNT - 1u) % RTL8168_RING_COUNT;
    CHECK((nic_dma.tx[index].opts1 & RTL8168_OWN) != 0u);
    nic_dma.tx[index].opts1 &= ~RTL8168_OWN;
}

static const struct rtl8168_platform nic_platform = {
    .ctx = &nic, .relax = complete_tx
};

static enum book8_net_result nic_send(void *ctx, const void *data, size_t length)
{
    enum rtl8168_status status = rtl8168_transmit(&nic_platform, ctx, data, length);
    return status == RTL8168_OK ? BOOK8_NET_OK
        : status == RTL8168_TX_BUSY ? BOOK8_NET_BUSY : BOOK8_NET_IO_ERROR;
}

static enum book8_net_result nic_receive(void *ctx, void *data,
    size_t capacity, size_t *length)
{
    enum rtl8168_status status = rtl8168_receive(ctx, data, capacity, length);
    if (status == RTL8168_OK) {
        return BOOK8_NET_OK;
    }
    return status == RTL8168_RX_EMPTY ? BOOK8_NET_EMPTY : BOOK8_NET_RX_DROPPED;
}

static void rtl_poll_tests(void)
{
    setup();
    memset(&nic_dma, 0, sizeof(nic_dma));
    memset(nic_regs, 0, sizeof(nic_regs));
    nic_tx_waits = 0u;
    nic = (struct rtl8168_device){.running = true, .dma = &nic_dma, .regs = nic_regs};
    const struct book8_net_transport rtl_transport = {
        .ctx = &nic, .send = nic_send, .receive = nic_receive
    };
    CHECK(book8_net_init(&net, &rtl_transport, local_mac) == BOOK8_NET_OK);
    CHECK(book8_net_configure(&net, LOCAL, MASK, 0u) == BOOK8_NET_OK);
    for (unsigned i = 0u; i < RTL8168_RING_COUNT; ++i) {
        nic_dma.rx[i].opts1 = RTL8168_OWN | RTL8168_BUFFER_SIZE
            | (i == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u);
    }
    arp_frame(1u, PEER, LOCAL);
    memcpy(nic_dma.rx_buffers[0u], frame, frame_length);
    nic_dma.rx[0u].opts1 = RTL8168_FS | RTL8168_LS | 64u;
    echo_frame(11u);
    memcpy(nic_dma.rx_buffers[1u], frame, frame_length);
    nic_dma.rx[1u].opts1 = RTL8168_FS | RTL8168_LS | 64u;
    CHECK(book8_net_poll(&net) == BOOK8_NET_OK);
    CHECK(nic.rx_packets == 2u && nic.tx_packets == 2u);
    CHECK(net.arp_replies == 1u && net.echo_replies == 1u);
    CHECK(nic.rx_next == 2u && nic.tx_next == 2u);
    CHECK(nic_dma.rx[0u].opts1 == (RTL8168_OWN | RTL8168_BUFFER_SIZE));
    CHECK(nic_dma.rx[1u].opts1 == (RTL8168_OWN | RTL8168_BUFFER_SIZE));
    CHECK(get16(nic_dma.tx_buffers[0u] + 20u) == 2u);
    CHECK(nic_dma.tx_buffers[1u][34u] == 0u);
    CHECK(wire_checksum(nic_dma.tx_buffers[1u] + 34u, 11u, 0u) == 0u);
    CHECK((nic_dma.tx[1u].opts1 & RTL8168_LENGTH_MASK) == 60u);
    CHECK(net_telemetry_init(&net, PEER) == BOOK8_NET_OK);
    const struct net_telemetry_metrics metrics = {0};
    const struct net_telemetry_extended extended = {0};
    for (unsigned i = 2u; i < RTL8168_RING_COUNT; ++i) {
        CHECK(net_telemetry_send_v2(i, &metrics, &extended, "queued")
            == BOOK8_NET_OK);
        CHECK((nic_dma.tx[i].opts1 & RTL8168_OWN) != 0u);
    }
    CHECK(nic.tx_packets == RTL8168_RING_COUNT && nic.tx_next == 0u);
    uint8_t saved[RTL8168_BUFFER_SIZE];
    memcpy(saved, nic_dma.tx_buffers[0], sizeof(saved));
    CHECK(net_telemetry_send_v2(10u, &metrics, &extended, "ring full")
        == BOOK8_NET_BUSY);
    CHECK(memcmp(saved, nic_dma.tx_buffers[0], sizeof(saved)) == 0);
    CHECK(nic.tx_packets == RTL8168_RING_COUNT && nic_tx_waits == 0u);
    nic_dma.tx[0].opts1 &= ~RTL8168_OWN;
    CHECK(net_telemetry_send_v2(11u, &metrics, &extended, "retry later")
        == BOOK8_NET_OK);
    CHECK(nic_tx_waits == 0u && nic.tx_packets == RTL8168_RING_COUNT + 1u);
    memset(net.arp, 0, sizeof(net.arp));
    nic_dma.tx[1].opts1 &= ~RTL8168_OWN;
    CHECK(net_telemetry_send_v2(12u, &metrics, &extended, "collector absent")
        == BOOK8_NET_ARP_PENDING);
    CHECK(nic_tx_waits == 0u);
    CHECK(net_telemetry_init(&net, 0u) == BOOK8_NET_OK);
    puts("PASS: RTL8168 RX ring -> network poll -> ARP/ICMP TX descriptors");
    puts("PASS: telemetry through stalled RTL8168 DMA returns without waiting and retries after backpressure");
}

static uint64_t get64(const uint8_t *p)
{
    return ((uint64_t)get32(p) << 32u) | get32(p + 4u);
}

static void telemetry_tests(void)
{
    setup();
    CHECK(sizeof(struct net_telemetry_header) == 16u);
    CHECK(sizeof(struct net_telemetry_metrics) == 60u);
    CHECK(sizeof(struct net_telemetry_payload) == 188u);
    CHECK(sizeof(struct net_telemetry_packet) == 204u);
    CHECK(_Alignof(struct net_telemetry_header) == 1u);
    CHECK(_Alignof(struct net_telemetry_metrics) == 1u);
    CHECK(_Alignof(struct net_telemetry_payload) == 1u);
    CHECK(_Alignof(struct net_telemetry_packet) == 1u);
    CHECK(offsetof(struct net_telemetry_packet, payload.log) == 76u);
    CHECK(book8_ntohll(0x0807060504030201ull) == 0x0102030405060708ull);
    CHECK(book8_htonll(book8_ntohll(0x0102030405060708ull)) == 0x0102030405060708ull);
    const struct net_telemetry_metrics metrics = {
        .cpu_flags = 0x01020304u, .cpu_mhz = 0x11223344u,
        .memory_total_bytes = 0x0102030405060708ull,
        .kernel_reserved_bytes = 0x1122334455667788ull,
        .heap_used_bytes = 0x2233445566778899ull,
        .heap_total_bytes = 0x33445566778899aaull,
        .tx_packets = 0x44556677u, .rx_packets = 0x55667788u,
        .tx_dropped = 0x66778899u, .rx_dropped = 0x778899aau,
        .tx_timeouts = 0x8899aabbu
    };
    CHECK(net_telemetry_init(NULL, 0u) == BOOK8_NET_OK);
    CHECK(net_telemetry_send(0u, &metrics, NULL) == BOOK8_NET_UNCONFIGURED);
    CHECK(net_telemetry_init(NULL, PEER) == BOOK8_NET_INVALID);
    CHECK(net_telemetry_init(&net, LOCAL) == BOOK8_NET_INVALID);
    CHECK(net_telemetry_init(&net, LOCAL | ~MASK) == BOOK8_NET_INVALID);
    CHECK(net_telemetry_init(&net, BOOK8_NET_IP(224, 0, 0, 1)) == BOOK8_NET_INVALID);
    CHECK(book8_udp_bind(&net, 8080u, udp_callback, &callbacks) == BOOK8_NET_OK);
    CHECK(net_telemetry_init(&net, PEER) == BOOK8_NET_PORT_IN_USE);
    CHECK(book8_udp_unbind(&net, 8080u) == BOOK8_NET_OK);
    CHECK(book8_net_configure(&net, 0u, 0u, 0u) == BOOK8_NET_OK);
    CHECK(net_telemetry_init(&net, PEER) == BOOK8_NET_UNCONFIGURED);
    CHECK(book8_net_configure(&net, LOCAL, MASK, 0u) == BOOK8_NET_OK);
    CHECK(net_telemetry_init(&net, BOOK8_NET_IP(10, 0, 0, 1)) == BOOK8_NET_NO_ROUTE);
    CHECK(net_telemetry_init(&net, PEER) == BOOK8_NET_OK);
    CHECK(net_telemetry_send(0u, NULL, "log") == BOOK8_NET_INVALID);
    CHECK(net_telemetry_send(0x0102030405060708ull, &metrics, "hello")
        == BOOK8_NET_ARP_PENDING);
    CHECK(get16(sent + 12u) == 0x0806u && get32(sent + 38u) == PEER);
    arp_frame(2u, PEER, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    telemetry_reentry = true;
    CHECK(net_telemetry_send(0x0102030405060708ull, &metrics, "hello") == BOOK8_NET_OK);
    telemetry_reentry = false;
    CHECK(sent_length == 42u + 204u);
    CHECK(get16(sent + 34u) == 8080u && get16(sent + 36u) == 8080u);
    CHECK(get16(sent + 38u) == 212u && wire_udp_checksum(sent) == 0u);
    const uint8_t *p = sent + 42u;
    CHECK(memcmp(p, "\x42\x4b\x38\x01", 4u) == 0);
    CHECK(get32(p + 4u) == 0u && get64(p + 8u) == 0x0102030405060708ull);
    CHECK(get32(p + 16u) == metrics.cpu_flags && get32(p + 20u) == metrics.cpu_mhz);
    CHECK(get64(p + 24u) == metrics.memory_total_bytes);
    CHECK(get64(p + 32u) == metrics.kernel_reserved_bytes);
    CHECK(get64(p + 40u) == metrics.heap_used_bytes);
    CHECK(get64(p + 48u) == metrics.heap_total_bytes);
    CHECK(get32(p + 56u) == metrics.tx_packets && get32(p + 60u) == metrics.rx_packets);
    CHECK(get32(p + 64u) == metrics.tx_dropped && get32(p + 68u) == metrics.rx_dropped);
    CHECK(get32(p + 72u) == metrics.tx_timeouts);
    CHECK(memcmp(p + 76u, "hello", 5u) == 0);
    for (unsigned i = 81u; i < 204u; ++i) {
        CHECK(p[i] == 0u);
    }
    char log[BOOK8_TELEMETRY_LOG_SIZE + 10u];
    memset(log, 'x', sizeof(log));
    CHECK(net_telemetry_send(UINT64_MAX, &metrics, log) == BOOK8_NET_OK);
    CHECK(get32(p + 4u) == 1u && get64(p + 8u) == UINT64_MAX);
    for (unsigned i = 76u; i < 203u; ++i) {
        CHECK(p[i] == 'x');
    }
    CHECK(p[203u] == 0u);
    send_result = BOOK8_NET_IO_ERROR;
    CHECK(net_telemetry_send(2u, &metrics, NULL) == BOOK8_NET_IO_ERROR);
    send_result = BOOK8_NET_OK;
    CHECK(net_telemetry_send(3u, &metrics, NULL) == BOOK8_NET_OK);
    CHECK(get32(p + 4u) == 2u && get64(p + 8u) == 3u);
    for (unsigned i = 76u; i < 204u; ++i) {
        CHECK(p[i] == 0u);
    }
    /* Packed metrics may start at an unaligned address; reads must be safe. */
    uint8_t unaligned[sizeof(metrics) + 1u];
    memcpy(unaligned + 1u, &metrics, sizeof(metrics));
    CHECK(net_telemetry_send(4u, (const struct net_telemetry_metrics *)(unaligned + 1u),
        "") == BOOK8_NET_OK);
    CHECK(get32(p + 4u) == 3u && get64(p + 24u) == metrics.memory_total_bytes);
    CHECK(net_telemetry_init(&net, LOCAL) == BOOK8_NET_INVALID);
    CHECK(net_telemetry_send(5u, &metrics, NULL) == BOOK8_NET_OK);
    CHECK(get32(p + 4u) == 4u);
    CHECK(book8_udp_unbind(&net, 8080u) == BOOK8_NET_OK);
    CHECK(book8_udp_bind(&net, 8080u, udp_callback, &callbacks) == BOOK8_NET_OK);
    CHECK(net_telemetry_send(5u, &metrics, NULL) == BOOK8_NET_NOT_BOUND);
    CHECK(net_telemetry_init(&net, 0u) == BOOK8_NET_OK);
    CHECK(book8_udp_unbind(&net, 8080u) == BOOK8_NET_OK); /* Do not unbind another owner. */
    CHECK(net_telemetry_init(&net, PEER) == BOOK8_NET_OK);
    CHECK(net_telemetry_send(6u, &metrics, NULL) == BOOK8_NET_OK);
    CHECK(get32(p + 4u) == 0u);
    CHECK(net_telemetry_init(&net, 0u) == BOOK8_NET_OK);
    CHECK(book8_udp_unbind(&net, 8080u) == BOOK8_NET_NOT_BOUND);
    CHECK(net_telemetry_send(0u, &metrics, NULL) == BOOK8_NET_UNCONFIGURED);
    puts("PASS: packed zero-allocation UDP telemetry serialization and lifecycle");
}

static void telemetry_v2_tests(void)
{
    setup();
    CHECK(sizeof(struct net_telemetry_extended) == 88u);
    CHECK(sizeof(struct net_telemetry_payload_v2) == 276u);
    CHECK(sizeof(struct net_telemetry_packet_v2) == 292u);
    CHECK(_Alignof(struct net_telemetry_packet_v2) == 1u);
    CHECK(offsetof(struct net_telemetry_packet_v2, payload.metrics) == 16u);
    CHECK(offsetof(struct net_telemetry_packet_v2, payload.extended) == 76u);
    CHECK(offsetof(struct net_telemetry_packet_v2, payload.extended.phy_status) == 88u);
    CHECK(offsetof(struct net_telemetry_packet_v2, payload.extended.push_rejected) == 160u);
    CHECK(offsetof(struct net_telemetry_packet_v2, payload.log) == 164u);
    const struct net_telemetry_metrics metrics = {
        .cpu_flags = 5u, .cpu_mhz = 3895u, .memory_total_bytes = 0x0102030405060708ull,
        .kernel_reserved_bytes = 17u << 20u, .heap_used_bytes = 4096u,
        .heap_total_bytes = 1u << 20u, .tx_packets = 606u, .rx_packets = 81u,
        .tx_dropped = 1u, .rx_dropped = 2u, .tx_timeouts = 3u
    };
    struct net_telemetry_extended extended;
    uint32_t words[22];
    for (unsigned i = 0u; i < 22u; ++i) {
        words[i] = 0x01000000u * (i + 1u) + 0x00010203u * i;
    }
    memcpy(&extended, words, sizeof(extended));
    arp_frame(2u, PEER, LOCAL);
    CHECK(book8_net_input(&net, frame, frame_length) == BOOK8_NET_OK);
    CHECK(book8_net_arp_entries(&net) == 1u && book8_net_arp_entries(NULL) == 0u);
    CHECK(net_telemetry_init(&net, PEER) == BOOK8_NET_OK);
    CHECK(net_telemetry_send_v2(1u, &metrics, NULL, "x") == BOOK8_NET_INVALID);
    CHECK(net_telemetry_send_v2(1u, NULL, &extended, "x") == BOOK8_NET_INVALID);
    CHECK(net_telemetry_send_v2(0x1122334455667788ull, &metrics, &extended,
        "push: kernel relaunch") == BOOK8_NET_OK);
    CHECK(sent_length == 42u + 292u);
    CHECK(get16(sent + 38u) == 300u && wire_udp_checksum(sent) == 0u);
    const uint8_t *p = sent + 42u;
    CHECK(memcmp(p, "\x42\x4b\x38\x02", 4u) == 0);
    CHECK(get32(p + 4u) == 0u && get64(p + 8u) == 0x1122334455667788ull);
    CHECK(get32(p + 16u) == 5u && get32(p + 20u) == 3895u);
    CHECK(get64(p + 24u) == metrics.memory_total_bytes);
    CHECK(get32(p + 56u) == 606u && get32(p + 72u) == 3u);
    for (unsigned i = 0u; i < 22u; ++i) {
        CHECK(get32(p + 76u + i * 4u) == words[i]);
    }
    CHECK(memcmp(p + 164u, "push: kernel relaunch", 21u) == 0);
    for (unsigned i = 164u + 21u; i < 292u; ++i) {
        CHECK(p[i] == 0u);
    }
    /* v1 and v2 share one sequence counter; v1 still emits exactly 204 bytes. */
    CHECK(net_telemetry_send(2u, &metrics, NULL) == BOOK8_NET_OK);
    CHECK(sent_length == 42u + 204u && get32(p + 4u) == 1u);
    CHECK(memcmp(p, "\x42\x4b\x38\x01", 4u) == 0);
    uint8_t unaligned[sizeof(extended) + 1u];
    memcpy(unaligned + 1u, &extended, sizeof(extended));
    CHECK(net_telemetry_send_v2(3u, &metrics,
        (const struct net_telemetry_extended *)(unaligned + 1u), NULL) == BOOK8_NET_OK);
    CHECK(get32(p + 4u) == 2u && get32(p + 76u) == words[0]);
    CHECK(net_telemetry_init(&net, 0u) == BOOK8_NET_OK);
    puts("PASS: telemetry v2 extended diagnostics serialization");
}

static void netcfg_tests(void)
{
    struct book8_netcfg config = {0};
    const char *text = "BOOK8NET 1\r\nIP 192.168.50.208\r\nMASK 255.255.255.0\r\n"
        "GATEWAY 192.168.50.1\r\nTELEMETRY 192.168.50.17\r\n";
    CHECK(book8_netcfg_parse(text, strlen(text), &config));
    CHECK(config.ip == BOOK8_NET_IP(192, 168, 50, 208));
    CHECK(config.mask == BOOK8_NET_IP(255, 255, 255, 0));
    CHECK(config.gateway == BOOK8_NET_IP(192, 168, 50, 1));
    CHECK(config.telemetry == BOOK8_NET_IP(192, 168, 50, 17));
    char out[BOOK8_NETCFG_TEXT_MAX];
    size_t length = book8_netcfg_format(&config, out, sizeof(out));
    CHECK(length == strlen(text) && strcmp(out, text) == 0);
    struct book8_netcfg round = {0};
    CHECK(book8_netcfg_parse(out, length, &round));
    CHECK(memcmp(&round, &config, sizeof(round)) == 0);
    const char *minimal = "# lab\nBOOK8NET 1\nMASK 255.255.0.0\nIP 10.1.2.3  \n\n";
    CHECK(book8_netcfg_parse(minimal, strlen(minimal), &round));
    CHECK(round.ip == BOOK8_NET_IP(10, 1, 2, 3) && round.mask == BOOK8_NET_IP(255, 255, 0, 0)
        && round.gateway == 0u && round.telemetry == 0u);
    const char *bad[] = {
        "", "IP 1.2.3.4\nMASK 255.0.0.0\n", "BOOK8NET 2\nIP 1.2.3.4\nMASK 255.0.0.0\n",
        "BOOK8NET 1\nIP 1.2.3.4\n", "BOOK8NET 1\nMASK 255.0.0.0\n",
        "BOOK8NET 1\nIP 0.0.0.0\nMASK 255.0.0.0\n",
        "BOOK8NET 1\nIP 1.2.3.4\nMASK 255.0.255.0\n",
        "BOOK8NET 1\nIP 1.2.3.4\nMASK 255.0.0.0\nIP 1.2.3.5\n",
        "BOOK8NET 1\nIP 1.2.3.4\nMASK 255.0.0.0\nDNS 1.1.1.1\n",
        "BOOK8NET 1\nIP 1.2.3.400\nMASK 255.0.0.0\n",
        "BOOK8NET 1\nIP\nMASK 255.0.0.0\n",
        "BOOK8NET 1\nIP 1.2.3.4 \nMASK 255.0.0.0\nGATEWAY 1.2.3.4.5\n"
    };
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!book8_netcfg_parse(bad[i], strlen(bad[i]), &round));
    }
    char nul[] = "BOOK8NET 1\nIP 1.2.3.4\0\nMASK 255.0.0.0\n";
    CHECK(!book8_netcfg_parse(nul, sizeof(nul) - 1u, &round));
    char huge[BOOK8_NETCFG_TEXT_MAX + 2u];
    memset(huge, '#', sizeof(huge));
    CHECK(!book8_netcfg_parse(huge, sizeof(huge), &round));
    CHECK(book8_netcfg_format(&config, out, 20u) == 0u && out[0] == '\0');
    CHECK(book8_netcfg_format(&config, out, length) == 0u);
    CHECK(book8_netcfg_format(&config, out, length + 1u) == length);
    puts("PASS: BOOK8NET.CFG parse/format round trip and rejection");
}

int main(void)
{
    configuration_tests();
    arp_tests();
    icmp_tests();
    udp_tests();
    malformed_ip_tests();
    poll_tests();
    rtl_poll_tests();
    telemetry_tests();
    telemetry_v2_tests();
    netcfg_tests();
    puts("PASS: zero-allocation Ethernet/ARP/IPv4/ICMP/UDP stack");
    return 0;
}
