#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/net.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define LOCAL BOOK8_NET_IP(192, 168, 7, 10)
#define PEER  BOOK8_NET_IP(192, 168, 7, 20)
#define GATEWAY BOOK8_NET_IP(192, 168, 7, 1)
#define MASK BOOK8_NET_IP(255, 255, 255, 0)

static const uint8_t local_mac[6] = {2u, 0u, 0u, 0u, 0u, 1u};
static const uint8_t peer_mac[6]  = {2u, 0u, 0u, 0u, 0u, 2u};
static const uint8_t broadcast_mac[6] = {255u, 255u, 255u, 255u, 255u, 255u};
static struct book8_net net;
static uint8_t frame[BOOK8_NET_FRAME_MAX];
static uint8_t sent[BOOK8_NET_FRAME_MAX];
static size_t sent_length;

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

static uint16_t checksum(const uint8_t *p, size_t length)
{
    uint32_t sum = 0u;
    while (length >= 2u) {
        sum += ((uint32_t)p[0] << 8u) | p[1];
        p += 2u;
        length -= 2u;
    }
    if (length != 0u) sum += (uint32_t)p[0] << 8u;
    while (sum >> 16u) sum = (sum & 0xffffu) + (sum >> 16u);
    return (uint16_t)~sum;
}

static enum book8_net_result fake_send(void *ctx, const void *data, size_t length)
{
    CHECK(ctx == &net);
    memcpy(sent, data, length);
    sent_length = length;
    return BOOK8_NET_OK;
}

static enum book8_net_result fake_receive(void *ctx, void *data,
    size_t capacity, size_t *length)
{
    (void)ctx; (void)data; (void)capacity; (void)length;
    return BOOK8_NET_EMPTY;
}

static const struct book8_net_transport transport = {
    .ctx = &net, .send = fake_send, .receive = fake_receive
};

static void setup(void)
{
    CHECK(book8_net_init(&net, &transport, local_mac) == BOOK8_NET_OK);
    CHECK(book8_net_configure(&net, LOCAL, MASK, GATEWAY) == BOOK8_NET_OK);
    memset(frame, 0, sizeof(frame));
    memset(sent, 0, sizeof(sent));
    sent_length = 0u;
}

static void icmp_from_peer(void)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac, 6u);
    memcpy(frame + 6u, peer_mac, 6u);
    put16(frame + 12u, 0x0800u);
    frame[14u] = 0x45u;
    put16(frame + 16u, 48u);           /* 20 IP + 8 ICMP + 20 payload */
    put16(frame + 18u, 0x1234u);
    put16(frame + 20u, 0x4000u);
    frame[22u] = 32u;
    frame[23u] = 1u;                   /* ICMP */
    put32(frame + 26u, PEER);
    put32(frame + 30u, LOCAL);
    put16(frame + 24u, checksum(frame + 14u, 20u));
    frame[34u] = 8u;                   /* echo request */
    put16(frame + 38u, 1u);            /* id */
    put16(frame + 40u, 1u);            /* sequence */
    put16(frame + 36u, checksum(frame + 34u, 28u));
}

static void arp_reply_from_peer(void)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, local_mac, 6u);
    memcpy(frame + 6u, peer_mac, 6u);
    put16(frame + 12u, 0x0806u);
    put16(frame + 14u, 1u);            /* hardware type */
    put16(frame + 16u, 0x0800u);       /* protocol type */
    frame[18u] = 6u; frame[19u] = 4u;
    put16(frame + 20u, 2u);            /* ARP reply */
    memcpy(frame + 22u, peer_mac, 6u);
    put32(frame + 28u, PEER);
    memcpy(frame + 32u, local_mac, 6u);
    put32(frame + 38u, LOCAL);
}

static void test_ipv6_and_tls_stubs(void)
{
#if BOOK8_NET_IPV6
    CHECK(book8_net_ipv6_supported() == true);
#else
    CHECK(book8_net_ipv6_supported() == false);
#endif
    CHECK(brook8_tls_available() == false);
}

static void test_dns_cache(void)
{
#if BOOK8_NET_DNS_CACHE
    book8_dns_cache_flush();
    CHECK(book8_dns_cache_entries() == 0u);

    uint32_t ip = 0u;
    CHECK(book8_dns_cache_lookup("Example.COM", 1000u, &ip) == false);

    book8_dns_cache_insert("Example.COM", 0x01020304u, 1000u);
    CHECK(book8_dns_cache_entries() == 1u);
    CHECK(book8_dns_cache_lookup("example.com", 2000u, &ip) == true);
    CHECK(ip == 0x01020304u);
    CHECK(book8_dns_cache_lookup("EXAMPLE.com", 59999u, &ip) == true);
    CHECK(book8_dns_cache_lookup("example.com", 61000u, &ip) == false);

    /* Re-insert refreshes TTL. */
    book8_dns_cache_insert("example.com", 0x05060708u, 61000u);
    CHECK(book8_dns_cache_lookup("example.com", 70000u, &ip) == true);
    CHECK(ip == 0x05060708u);

    /* Bounded capacity: 8 entries max, round-robin replacement. */
    for (uint32_t i = 0u; i < BOOK8_NET_DNS_CACHE_ENTRIES + 3u; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "host%u.test", i);
        book8_dns_cache_insert(name, 0x0a000001u + i, 70000u);
    }
    CHECK(book8_dns_cache_entries() <= BOOK8_NET_DNS_CACHE_ENTRIES);

    book8_dns_cache_flush();
    CHECK(book8_dns_cache_entries() == 0u);
    CHECK(book8_dns_cache_lookup("example.com", 80000u, &ip) == false);
#endif
}

static void test_firewall(void)
{
    (void)broadcast_mac;
    (void)setup;
    (void)icmp_from_peer;
    (void)arp_reply_from_peer;
#if BOOK8_NET_FIREWALL
    setup();
    CHECK(book8_net_firewall_enabled(&net) == false);

    /* Unsolicited ICMP is accepted when firewall is disabled. */
    icmp_from_peer();
    CHECK(book8_net_input(&net, frame, 62u) == BOOK8_NET_OK);

    setup();
    book8_net_firewall_set(&net, true);
    CHECK(book8_net_firewall_enabled(&net) == true);

    /* Unsolicited ICMP dropped while firewall is active. */
    uint32_t baseline = book8_net_firewall_dropped(&net);
    icmp_from_peer();
    CHECK(book8_net_input(&net, frame, 62u) == BOOK8_NET_IGNORED);
    CHECK(book8_net_firewall_dropped(&net) == baseline + 1u);

    /* Learn the peer as established by learning ARP + sending IPv4. */
    arp_reply_from_peer();
    CHECK(book8_net_input(&net, frame, 42u) == BOOK8_NET_OK);
    uint8_t payload[4] = {1u, 2u, 3u, 4u};
    CHECK(book8_net_send_ipv4(&net, PEER, 17u, payload, sizeof(payload)) == BOOK8_NET_OK);

    /* Reply from established peer is allowed. */
    icmp_from_peer();
    CHECK(book8_net_input(&net, frame, 62u) == BOOK8_NET_OK);
    CHECK(book8_net_firewall_dropped(&net) == baseline + 1u);

    /* Clearing established peers restores the drop. */
    book8_net_firewall_peers_clear(&net);
    icmp_from_peer();
    CHECK(book8_net_input(&net, frame, 62u) == BOOK8_NET_IGNORED);

    book8_net_firewall_set(&net, false);
#endif
}

int main(void)
{
    test_ipv6_and_tls_stubs();
    test_dns_cache();
    test_firewall();
    puts("PASS: IPv6/TLS stubs, in-memory DNS cache, and established-peer firewall");
    return 0;
}
