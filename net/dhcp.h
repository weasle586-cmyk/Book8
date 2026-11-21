#ifndef BOOK8_DHCP_H
#define BOOK8_DHCP_H

#include "net.h"
#define BOOK8_DHCP_FALLBACK_MS 30000u

enum book8_dhcp_state {
    BOOK8_DHCP_OFF, BOOK8_DHCP_SELECTING, BOOK8_DHCP_REQUESTING,
    BOOK8_DHCP_BOUND, BOOK8_DHCP_RENEWING, BOOK8_DHCP_REBINDING
};

struct book8_dhcp {
    struct book8_net *net;
    void (*log)(const char *);
    enum book8_dhcp_state state;
    uint32_t xid, server, offered_ip, dns;
    uint32_t lease_seconds, retry_ms, attempts, rejected, generation;
    uint64_t now, next_send, expires, renew_at, rebind_at;
    uint32_t pending_ip, pending_mask, pending_gateway, pending_dns;
    uint32_t pending_lease, pending_t1, pending_t2, pending_server;
    uint8_t pending;
    bool bound_port;
    uint32_t fallback_ip, fallback_mask, fallback_gateway;
    uint64_t fallback_at;
    bool fallback_active;
};

enum book8_net_result book8_dhcp_start(struct book8_dhcp *, struct book8_net *,
    uint64_t now_ms, uint32_t xid, uint32_t retry_ms, void (*log)(const char *));
enum book8_net_result book8_dhcp_stop(struct book8_dhcp *);
enum book8_net_result book8_dhcp_set_fallback(struct book8_dhcp *,
    uint32_t ip, uint32_t mask, uint32_t gateway);
enum book8_net_result book8_dhcp_poll(struct book8_dhcp *, uint64_t now_ms);
enum book8_net_result book8_dhcp_renew(struct book8_dhcp *, uint64_t now_ms);
const char *book8_dhcp_state_string(enum book8_dhcp_state);

#endif
