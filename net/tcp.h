#ifndef BOOK8_TCP_H
#define BOOK8_TCP_H
#include "net.h"
#define BOOK8_TCP_RX 32768u
#define BOOK8_TCP_MSS 1400u
enum book8_tcp_state { TCP_CLOSED, TCP_CONNECTING, TCP_CONNECTED, TCP_FAILED };
struct book8_tcp {
    struct book8_net *net;
    enum book8_tcp_state state;
    enum book8_net_result error;
    uint32_t peer, local_ip, sequence, acknowledged, receive_next;
    uint16_t local_port, peer_port, peer_window, peer_mss;
    uint64_t started, retry_at;
    uint32_t retries, rx_length, tx_length, flight;
    uint8_t flags;
    bool sent, ack_pending;
    uint8_t rx[BOOK8_TCP_RX], tx[BOOK8_TCP_MSS], packet[BOOK8_TCP_MSS + 24u];
};
enum book8_net_result book8_tcp_connect(struct book8_tcp *, struct book8_net *,
    uint32_t peer, uint16_t port, uint64_t now);
void book8_tcp_poll(struct book8_tcp *, uint64_t now);
enum book8_net_result book8_tcp_send(struct book8_tcp *, const void *, uint32_t, uint64_t);
void book8_tcp_consume(struct book8_tcp *, uint32_t);
void book8_tcp_close(struct book8_tcp *);
#endif
