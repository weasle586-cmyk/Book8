#include "tcp.h"
#include <string.h>

#define FIN 1u
#define SYN 2u
#define RST 4u
#define PSH 8u
#define ACK 16u
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8u) | p[1]); }
static uint32_t get32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) | (p[2] << 8u) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8u); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{ put16(p, (uint16_t)(v >> 16u)); put16(p + 2u, (uint16_t)v); }
static uint16_t checksum(uint32_t source, uint32_t destination, const uint8_t *p, size_t size)
{
    uint32_t sum = (source >> 16u) + (source & 65535u)
        + (destination >> 16u) + (destination & 65535u) + 6u + (uint32_t)size;
    while (size >= 2u) { sum += get16(p); p += 2u; size -= 2u; }
    if (size) { sum += (uint32_t)*p << 8u; }
    while (sum >> 16u) { sum = (sum & 65535u) + (sum >> 16u); }
    return (uint16_t)~sum;
}
static enum book8_net_result emit(struct book8_tcp *t, uint8_t flags, const void *data, uint32_t size)
{
    uint32_t header = flags & SYN ? 24u : 20u;
    memset(t->packet, 0u, header);
    put16(t->packet, t->local_port); put16(t->packet + 2u, t->peer_port);
    put32(t->packet + 4u, t->sequence + (t->sent && !size && !(flags & SYN) ? t->flight : 0u));
    put32(t->packet + 8u, t->receive_next);
    t->packet[12] = (uint8_t)(header << 2u); t->packet[13] = flags;
    put16(t->packet + 14u, (uint16_t)(BOOK8_TCP_RX - t->rx_length));
    if (flags & SYN) { t->packet[20] = 2u; t->packet[21] = 4u; put16(t->packet + 22u, BOOK8_TCP_MSS); }
    if (size) { memcpy(t->packet + header, data, size); }
    put16(t->packet + 16u, checksum(t->local_ip, t->peer, t->packet, size + header));
    return book8_net_send_ipv4(t->net, t->peer, 6u, t->packet, size + header);
}
static void fail(struct book8_tcp *t, enum book8_net_result error)
{ t->state = TCP_FAILED; t->error = error; }
static enum book8_net_result input(void *context, uint32_t peer, const uint8_t *p, size_t size)
{
    struct book8_tcp *t = context;
    if (size < 20u) { return BOOK8_NET_INVALID; }
    if (peer != t->peer || get16(p) != t->peer_port || get16(p + 2u) != t->local_port
        || (t->state != TCP_CONNECTED && t->state != TCP_CONNECTING)) { return BOOK8_NET_IGNORED; }
    uint32_t header = (p[12] >> 4u) * 4u;
    if (header < 20u || header > size || checksum(peer, t->local_ip, p, size) != 0u) {
        return BOOK8_NET_INVALID;
    }
    uint32_t seq = get32(p + 4u), ack = get32(p + 8u);
    uint8_t flags = p[13];
    if (t->state == TCP_CONNECTING) {
        if (flags & RST) {
            if ((flags & ACK) && t->sent && ack == t->sequence + 1u) { fail(t, BOOK8_NET_REMOTE_ERROR); }
            return BOOK8_NET_OK;
        }
        if (!t->sent || (flags & (SYN | ACK)) != (SYN | ACK)
            || ack != t->sequence + 1u || size != header) { return BOOK8_NET_IGNORED; }
        uint16_t peer_mss = t->peer_mss;
        for (uint32_t i = 20u; i < header;) {
            uint8_t kind = p[i];
            if (!kind) { break; }
            if (kind == 1u) { ++i; continue; }
            if (i + 2u > header || p[i + 1u] < 2u || p[i + 1u] > header - i) { return BOOK8_NET_INVALID; }
            if (kind == 2u && p[i + 1u] == 4u) {
                uint16_t mss = get16(p + i + 2u);
                if (mss < 64u) { fail(t, BOOK8_NET_INVALID); return BOOK8_NET_INVALID; }
                peer_mss = mss < BOOK8_TCP_MSS ? mss : BOOK8_TCP_MSS;
            }
            i += p[i + 1u];
        }
        t->sequence = t->acknowledged = ack; t->receive_next = seq + 1u;
        t->peer_mss = peer_mss; t->peer_window = get16(p + 14u);
        t->sent = false; t->flags = 0u;
        t->state = TCP_CONNECTED; t->ack_pending = true;
        return BOOK8_NET_OK;
    }
    if (flags & RST) {
        if (seq == t->receive_next) { fail(t, BOOK8_NET_REMOTE_ERROR); }
        return BOOK8_NET_OK;
    }
    if (!(flags & ACK) || (flags & SYN)) { return BOOK8_NET_IGNORED; }
    uint32_t end = t->sequence + (t->sent ? t->flight : 0u);
    if ((int32_t)(ack - t->sequence) < 0 || (int32_t)(ack - end) > 0) {
        return BOOK8_NET_IGNORED;
    }
    if (seq == t->receive_next) { t->peer_window = get16(p + 14u); }
    if (t->sent && ack == end) {
        t->sequence = t->acknowledged = ack; t->sent = false;
        t->tx_length -= t->flight;
        memmove(t->tx, t->tx + t->flight, t->tx_length);
        t->flight = 0u; t->flags = t->tx_length ? PSH | ACK : 0u;
        t->retry_at = 0u; t->retries = 0u;
    }
    size_t bytes = size - header;
    if (bytes || (flags & FIN)) {
        const uint8_t *data = p + header;
        if ((int32_t)(seq - t->receive_next) < 0) {
            uint32_t skipped = t->receive_next - seq;
            if (skipped <= bytes) { data += skipped; bytes -= skipped; seq += skipped; }
        }
        if (seq == t->receive_next && bytes <= BOOK8_TCP_RX - t->rx_length) {
            memcpy(t->rx + t->rx_length, data, bytes);
            t->rx_length += (uint32_t)bytes; t->receive_next += (uint32_t)bytes;
            if (flags & FIN) { ++t->receive_next; fail(t, BOOK8_NET_REMOTE_ERROR); }
        }
        t->ack_pending = true;
    }
    return BOOK8_NET_OK;
}
enum book8_net_result book8_tcp_connect(struct book8_tcp *t, struct book8_net *net,
    uint32_t peer, uint16_t port, uint64_t now)
{
    if (!net || !net->ip) { return BOOK8_NET_UNCONFIGURED; }
    if (!port || peer == net->ip || (peer >> 24u) == 0u || (peer >> 24u) >= 224u
        || (peer >> 24u) == 127u) { return BOOK8_NET_INVALID; }
    if (net->tcp_context && net->tcp_context != t) { return BOOK8_NET_BUSY; }
    static uint16_t next_port = 49152u;
    memset(t, 0, sizeof(*t));
    t->net = net; t->peer = peer; t->peer_port = port; t->local_ip = net->ip;
    t->local_port = next_port++;
    t->peer_mss = 536u;
    if (next_port < 49152u) { next_port = 49152u; }
    t->sequence = (uint32_t)now * 64001u + (uint32_t)t->local_port * 65537u;
    t->acknowledged = t->sequence; t->state = TCP_CONNECTING; t->flags = SYN;
    t->started = t->retry_at = now;
    net->tcp_context = t; net->tcp_input = input;
    return BOOK8_NET_OK;
}
void book8_tcp_poll(struct book8_tcp *t, uint64_t now)
{
    if (!t->net || t->state == TCP_CLOSED) { return; }
    if (t->net->ip != t->local_ip) { fail(t, BOOK8_NET_UNCONFIGURED); return; }
    if (t->ack_pending) {
        if (emit(t, ACK, NULL, 0u) == BOOK8_NET_OK) { t->ack_pending = false; }
    }
    if (t->state == TCP_FAILED || (t->state == TCP_CONNECTED && !t->flags)) { return; }
    if (now - t->started >= 10000u) { fail(t, BOOK8_NET_TIMEOUT); return; }
    if (now < t->retry_at) { return; }
    if (t->state == TCP_CONNECTED && !t->sent) {
        t->flight = t->tx_length;
        if (t->flight > t->peer_mss) { t->flight = t->peer_mss; }
        if (t->flight > t->peer_window) { t->flight = t->peer_window; }
        if (!t->flight) { t->retry_at = now + 200u; return; }
    }
    enum book8_net_result r = emit(t, t->flags, t->tx, t->flight);
    if (r == BOOK8_NET_OK) {
        t->sent = true;
        uint32_t delay = 500u << (t->retries < 3u ? t->retries : 3u);
        ++t->retries; t->retry_at = now + delay;
    } else if (r == BOOK8_NET_ARP_PENDING || r == BOOK8_NET_BUSY) { t->retry_at = now + 200u; }
    else { fail(t, r); }
}
enum book8_net_result book8_tcp_send(struct book8_tcp *t, const void *data, uint32_t size, uint64_t now)
{
    if (t->state != TCP_CONNECTED) { return BOOK8_NET_NOT_BOUND; }
    if (t->flags || t->tx_length) { return BOOK8_NET_BUSY; }
    if (!data || !size || size > sizeof(t->tx)) { return BOOK8_NET_TOO_LARGE; }
    memcpy(t->tx, data, size); t->tx_length = size; t->flags = PSH | ACK;
    t->started = t->retry_at = now; t->retries = 0u; t->sent = false;
    return BOOK8_NET_OK;
}
void book8_tcp_consume(struct book8_tcp *t, uint32_t size)
{
    if (size > t->rx_length) { return; }
    memmove(t->rx, t->rx + size, t->rx_length - size);
    t->rx_length -= size; t->ack_pending = true;
}
void book8_tcp_close(struct book8_tcp *t)
{
    if (t->net && t->net->tcp_context == t) {
        if (t->state == TCP_CONNECTED) { (void)emit(t, RST | ACK, NULL, 0u); }
        t->net->tcp_context = NULL; t->net->tcp_input = NULL;
    }
    t->net = NULL; t->state = TCP_CLOSED;
}
