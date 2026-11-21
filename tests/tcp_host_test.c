#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../net/tcp.h"
#define LOCAL BOOK8_NET_IP(192,168,7,10)
#define PEER BOOK8_NET_IP(192,168,7,20)
static struct book8_net net;
static struct book8_tcp tcp, other;
static uint8_t sent[1514], packet[1514];
static uint32_t sends;
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] * 256u + p[1]); }
static uint32_t g32(const uint8_t *p) { return ((uint32_t)g16(p) << 16u) | g16(p + 2u); }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8u); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)(v >> 16u)); p16(p + 2u, (uint16_t)v); }
static uint16_t check(const uint8_t *p, uint32_t n, uint32_t sum)
{
    for (uint32_t i = 0u; i < n; ++i) { sum += (uint32_t)p[i] << (i % 2u ? 0u : 8u); }
    while (sum >> 16u) { sum = (sum & 65535u) + (sum >> 16u); }
    return (uint16_t)~sum;
}
static enum book8_net_result send(void *ctx, const void *p, size_t n)
{ (void)ctx; assert(n <= sizeof(sent)); memcpy(sent, p, n); ++sends; return BOOK8_NET_OK; }
static enum book8_net_result receive(void *ctx, void *p, size_t n, size_t *size)
{ (void)ctx; (void)p; (void)n; *size = 0u; return BOOK8_NET_EMPTY; }
static void setup(void)
{
    const struct book8_net_transport transport = {.send = send, .receive = receive};
    uint8_t mac[6] = {2,0,0,0,0,1};
    assert(book8_net_init(&net, &transport, mac) == BOOK8_NET_OK);
    assert(book8_net_configure(&net, LOCAL, 0xffffff00u, BOOK8_NET_IP(192,168,7,1)) == BOOK8_NET_OK);
    net.arp[0].ip = PEER; net.arp[0].mac[0] = 2u; net.arp[0].mac[5] = 2u;
}
static enum book8_net_result inject(uint32_t seq, uint32_t ack, uint8_t flags, const char *data, uint32_t n, bool corrupt)
{
    memset(packet, 0u, sizeof(packet));
    memcpy(packet, net.mac, 6u); memcpy(packet + 6u, net.arp[0].mac, 6u); p16(packet + 12u, 0x0800u);
    uint8_t *ip = packet + 14u; ip[0] = 0x45u; p16(ip + 2u, (uint16_t)(40u + n));
    ip[8] = 64u; ip[9] = 6u; p32(ip + 12u, PEER); p32(ip + 16u, LOCAL);
    p16(ip + 10u, check(ip, 20u, 0u));
    uint8_t *p = ip + 20u; p16(p, 445u); p16(p + 2u, tcp.local_port);
    p32(p + 4u, seq); p32(p + 8u, ack); p[12] = 0x50u; p[13] = flags;
    p16(p + 14u, 32768u);
    if (n) { memcpy(p + 20u, data, n); }
    p16(p + 16u, check(p, n + 20u, (PEER >> 16u) + (PEER & 65535u)
        + (LOCAL >> 16u) + (LOCAL & 65535u) + 6u + 20u + n));
    if (corrupt) { p[16] ^= 1u; }
    return book8_net_input(&net, packet, 54u + n);
}
int main(void)
{
    setup();
    assert(book8_tcp_connect(&tcp, NULL, PEER, 445u, 0u) == BOOK8_NET_UNCONFIGURED);
    assert(book8_tcp_connect(&tcp, &net, LOCAL, 445u, 0u) == BOOK8_NET_INVALID);
    assert(book8_tcp_connect(&tcp, &net, PEER, 445u, 0u) == BOOK8_NET_OK);
    assert(book8_tcp_connect(&other, &net, PEER, 445u, 0u) == BOOK8_NET_BUSY);
    book8_tcp_poll(&tcp, 0u);
    assert(sends == 1u && sent[47] == 2u && g16(sent + 36u) == 445u);
    uint32_t initial = g32(sent + 38u);
    book8_tcp_poll(&tcp, 499u); assert(sends == 1u);
    book8_tcp_poll(&tcp, 500u); assert(sends == 2u && g32(sent + 38u) == initial);
    assert(inject(100u, initial + 2u, 18u, NULL, 0u, false) == BOOK8_NET_IGNORED);
    assert(inject(100u, initial + 1u, 18u, NULL, 0u, true) == BOOK8_NET_INVALID);
    assert(tcp.state == TCP_CONNECTING);
    assert(inject(100u, initial + 1u, 18u, "\002\010\000\000", 4u, false) == BOOK8_NET_IGNORED);
    uint8_t *synack = packet + 34u;
    synack[12] = 0x60u; p16(synack + 16u, 0u);
    p16(synack + 16u, check(synack, 24u, (PEER >> 16u) + (PEER & 65535u)
        + (LOCAL >> 16u) + (LOCAL & 65535u) + 6u + 24u));
    assert(book8_net_input(&net, packet, 58u) == BOOK8_NET_INVALID);
    assert(tcp.state == TCP_CONNECTING && tcp.sequence == initial && !tcp.receive_next);
    assert(inject(100u, initial + 1u, 18u, NULL, 0u, false) == BOOK8_NET_OK);
    book8_tcp_poll(&tcp, 501u);
    assert(tcp.state == TCP_CONNECTED && g32(sent + 42u) == 101u);
    assert(book8_tcp_send(&tcp, "hello", 5u, 502u) == BOOK8_NET_OK);
    assert(book8_tcp_send(&tcp, "x", 1u, 502u) == BOOK8_NET_BUSY);
    book8_tcp_poll(&tcp, 502u);
    assert(!memcmp(sent + 54u, "hello", 5u));
    assert(inject(101u, initial + 6u, 24u, "abc", 3u, false) == BOOK8_NET_OK);
    assert(tcp.rx_length == 3u && tcp.tx_length == 0u);
    assert(inject(101u, initial + 6u, 24u, "abc", 3u, false) == BOOK8_NET_OK);
    assert(tcp.rx_length == 3u);
    assert(inject(110u, initial + 6u, 24u, "bad", 3u, false) == BOOK8_NET_OK);
    assert(tcp.rx_length == 3u);
    book8_tcp_poll(&tcp, 503u); assert(g32(sent + 42u) == 104u);
    book8_tcp_consume(&tcp, 2u); assert(tcp.rx_length == 1u && tcp.rx[0] == 'c');
    assert(inject(104u, initial + 6u, 24u, "def", 3u, false) == BOOK8_NET_OK);
    assert(tcp.rx_length == 4u && !memcmp(tcp.rx, "cdef", 4u));
    assert(inject(105u, initial + 6u, 24u, "efghi", 5u, false) == BOOK8_NET_OK);
    assert(tcp.rx_length == 7u && !memcmp(tcp.rx, "cdefghi", 7u));
    char request[1200]; memset(request, 'x', sizeof(request));
    assert(book8_tcp_send(&tcp, request, sizeof(request), 504u) == BOOK8_NET_OK);
    book8_tcp_poll(&tcp, 504u); assert(tcp.flight == 536u);
    assert(inject(110u, tcp.sequence + 536u, 16u, NULL, 0u, false) == BOOK8_NET_OK);
    book8_tcp_poll(&tcp, 505u); assert(tcp.flight == 536u && tcp.tx_length == 664u);
    assert(inject(110u, tcp.sequence + 536u, 16u, NULL, 0u, false) == BOOK8_NET_OK);
    book8_tcp_poll(&tcp, 506u); assert(tcp.flight == 128u && tcp.tx_length == 128u);
    assert(inject(110u, tcp.sequence + 128u, 16u, NULL, 0u, false) == BOOK8_NET_OK);
    assert(!tcp.tx_length && !tcp.flags);
    tcp.rx_length = BOOK8_TCP_RX;
    assert(inject(110u, tcp.sequence, 24u, "x", 1u, false) == BOOK8_NET_OK);
    assert(tcp.receive_next == 110u);
    tcp.rx_length = 0u;
    assert(inject(1u, tcp.sequence, 20u, NULL, 0u, false) == BOOK8_NET_OK);
    assert(tcp.state == TCP_CONNECTED);
    assert(inject(110u, tcp.sequence, 20u, NULL, 0u, false) == BOOK8_NET_OK);
    assert(tcp.state == TCP_FAILED);
    book8_tcp_close(&tcp); assert(!net.tcp_context && !net.tcp_input);
    assert(book8_tcp_connect(&tcp, &net, PEER, 445u, 1000u) == BOOK8_NET_OK);
    book8_tcp_poll(&tcp, 11000u); assert(tcp.state == TCP_FAILED && tcp.error == BOOK8_NET_TIMEOUT);
    book8_tcp_close(&tcp);
    assert(book8_tcp_connect(&tcp, &net, PEER, 445u, 0u) == BOOK8_NET_OK);
    net.ip = 0u; book8_tcp_poll(&tcp, 1u);
    assert(tcp.state == TCP_FAILED && tcp.error == BOOK8_NET_UNCONFIGURED);
    book8_tcp_close(&tcp);
    puts("PASS: TCP handshake/checksums, retries, stream/duplicate/ordering, reset, timeout and offline");
    return 0;
}
