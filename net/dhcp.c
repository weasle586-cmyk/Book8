#include "dhcp.h"
#include <string.h>

int snprintf(char *, size_t, const char *, ...);

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u)
        | ((uint32_t)p[2] << 8u) | p[3];
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24u);
    p[1] = (uint8_t)(value >> 16u);
    p[2] = (uint8_t)(value >> 8u);
    p[3] = (uint8_t)value;
}

static bool host_ip(uint32_t ip)
{
    uint32_t first = ip >> 24u;
    return first != 0u && first != 127u && first < 224u && ip != UINT32_MAX;
}

const char *book8_dhcp_state_string(enum book8_dhcp_state state)
{
    switch (state) {
    case BOOK8_DHCP_OFF: return "off";
    case BOOK8_DHCP_SELECTING: return "discover";
    case BOOK8_DHCP_REQUESTING: return "request";
    case BOOK8_DHCP_BOUND: return "bound";
    case BOOK8_DHCP_RENEWING: return "renew";
    case BOOK8_DHCP_REBINDING: return "rebind";
    }
    return "invalid";
}

static void state_set(struct book8_dhcp *d, enum book8_dhcp_state state)
{
    d->state = state;
    d->attempts = 0u;
    d->next_send = d->now;
    char text[96];
    snprintf(text, sizeof(text), "DHCP state=%s xid=%08x",
        book8_dhcp_state_string(state), d->xid);
    d->log(text);
}

struct options {
    uint32_t server, mask, gateway, dns, lease, t1, t2;
    uint16_t seen;
    uint8_t type, overload;
};

static bool parse_options(const uint8_t *p, size_t length, struct options *o,
    bool main)
{
    size_t offset = 0u;
    while (offset < length) {
        uint8_t code = p[offset++];
        if (code == 255u)
            return true;
        if (code == 0u)
            continue;
        if (offset == length)
            return false;
        size_t size = p[offset++];
        if (size > length - offset)
            return false;
        unsigned bit = 16u;
        switch (code) {
        case 1u: bit = 0u; break;
        case 3u: bit = 1u; break;
        case 6u: bit = 2u; break;
        case 51u: bit = 3u; break;
        case 52u: bit = 4u; break;
        case 53u: bit = 5u; break;
        case 54u: bit = 6u; break;
        case 58u: bit = 7u; break;
        case 59u: bit = 8u; break;
        default: break;
        }
        if (bit != 16u) {
            if ((o->seen & (1u << bit)) != 0u)
                return false;
            o->seen |= (uint16_t)(1u << bit);
            if (code == 52u || code == 53u) {
                if (size != 1u)
                    return false;
            } else if (code == 3u || code == 6u) {
                if (size < 4u || (size & 3u) != 0u)
                    return false;
            } else if (size != 4u) {
                return false;
            }
            switch (code) {
            case 1u: o->mask = get32(p + offset); break;
            case 3u: o->gateway = get32(p + offset); break;
            case 6u: o->dns = get32(p + offset); break;
            case 51u: o->lease = get32(p + offset); break;
            case 52u:
                if (!main || p[offset] == 0u || p[offset] > 3u)
                    return false;
                o->overload = p[offset];
                break;
            case 53u: o->type = p[offset]; break;
            case 54u: o->server = get32(p + offset); break;
            case 58u: o->t1 = get32(p + offset); break;
            case 59u: o->t2 = get32(p + offset); break;
            default: break;
            }
        }
        offset += size;
    }
    return false;
}

static void reject_packet(struct book8_dhcp *d)
{
    ++d->rejected;
    if (d->rejected == 1u || (d->rejected & (d->rejected - 1u)) == 0u)
        d->log("DHCP rejected malformed or mismatched reply");
}

static void receive(void *ctx, uint32_t source, uint16_t port,
    const uint8_t *p, size_t length)
{
    struct book8_dhcp *d = ctx;
    if (d->state == BOOK8_DHCP_OFF || d->state == BOOK8_DHCP_BOUND || d->pending != 0u)
        return;
    struct options o = {0};
    if (port != 67u || length < 240u || p[0] != 2u || p[1] != 1u || p[2] != 6u
        || get32(p + 4u) != d->xid || memcmp(p + 28u, d->net->mac, 6u) != 0
        || get32(p + 236u) != 0x63825363u
        || !parse_options(p + 240u, length - 240u, &o, true)
        || ((o.overload & 1u) != 0u && !parse_options(p + 108u, 128u, &o, false))
        || ((o.overload & 2u) != 0u && !parse_options(p + 44u, 64u, &o, false))
        || !host_ip(o.server)
        || (source != o.server && (get32(p + 24u) == 0u || source != get32(p + 24u)))) {
        reject_packet(d);
        return;
    }
    bool renewing = d->state == BOOK8_DHCP_RENEWING || d->state == BOOK8_DHCP_REBINDING;
    if (d->state != BOOK8_DHCP_SELECTING && d->state != BOOK8_DHCP_REBINDING
        && o.server != d->server) {
        reject_packet(d);
        return;
    }
    if (o.type == 6u && d->state != BOOK8_DHCP_SELECTING) {
        d->pending = 6u;
        return;
    }
    if ((d->state == BOOK8_DHCP_SELECTING && o.type != 2u)
        || (d->state != BOOK8_DHCP_SELECTING && o.type != 5u)) {
        reject_packet(d);
        return;
    }
    uint32_t ip = get32(p + 16u);
    if (renewing && ip == 0u)
        ip = d->net->ip;
    if (!host_ip(ip) || (d->state == BOOK8_DHCP_REQUESTING && ip != d->offered_ip)
        || (renewing && ip != d->net->ip)) {
        reject_packet(d);
        return;
    }
    if (o.type == 2u) {
        d->pending_ip = ip;
        d->pending_server = o.server;
        d->pending = 2u;
        return;
    }
    struct book8_net validation = {0};
    if ((o.seen & ((1u << 0u) | (1u << 3u))) != ((1u << 0u) | (1u << 3u))
        || o.lease < 4u || book8_net_configure(&validation, ip, o.mask, o.gateway) != BOOK8_NET_OK
        || (o.dns != 0u && !host_ip(o.dns))) {
        reject_packet(d);
        return;
    }
    if (o.lease != UINT32_MAX) {
        if ((o.seen & (1u << 7u)) == 0u)
            o.t1 = o.lease / 2u;
        if ((o.seen & (1u << 8u)) == 0u)
            o.t2 = (uint32_t)((uint64_t)o.lease * 7u / 8u);
        if (o.t1 == 0u || o.t1 >= o.t2 || o.t2 >= o.lease) {
            reject_packet(d);
            return;
        }
    }
    d->pending_ip = ip;
    d->pending_mask = o.mask;
    d->pending_gateway = o.gateway;
    d->pending_dns = o.dns;
    d->pending_lease = o.lease;
    d->pending_t1 = o.t1;
    d->pending_t2 = o.t2;
    d->pending_server = o.server;
    d->pending = 5u;
}

enum book8_net_result book8_dhcp_stop(struct book8_dhcp *d)
{
    if (d == NULL)
        return BOOK8_NET_INVALID;
    if (d->net != NULL && (d->net->receiving || d->net->sending))
        return BOOK8_NET_BUSY;
    if (d->bound_port) {
        enum book8_net_result result = book8_udp_unbind(d->net, 68u);
        if (result != BOOK8_NET_OK)
            return result;
    }
    d->bound_port = false;
    d->state = BOOK8_DHCP_OFF;
    d->pending = 0u;
    d->dns = d->lease_seconds = 0u;
    d->fallback_active = false;
    return BOOK8_NET_OK;
}

enum book8_net_result book8_dhcp_set_fallback(struct book8_dhcp *d,
    uint32_t ip, uint32_t mask, uint32_t gateway)
{
    if (d == NULL || !d->bound_port)
        return BOOK8_NET_UNCONFIGURED;
    if (d->net->receiving || d->net->sending)
        return BOOK8_NET_BUSY;
    struct book8_net validation = {0};
    enum book8_net_result result = book8_net_configure(&validation, ip, mask, gateway);
    if (result != BOOK8_NET_OK)
        return result;
    d->fallback_ip = ip;
    d->fallback_mask = mask;
    d->fallback_gateway = gateway;
    d->fallback_at = d->now + BOOK8_DHCP_FALLBACK_MS;
    return BOOK8_NET_OK;
}

enum book8_net_result book8_dhcp_start(struct book8_dhcp *d, struct book8_net *net,
    uint64_t now, uint32_t xid, uint32_t retry_ms, void (*log)(const char *))
{
    if (d == NULL || net == NULL || log == NULL || retry_ms < 1000u || retry_ms > 16000u || xid == 0u)
        return BOOK8_NET_INVALID;
    if (d->bound_port || net->receiving || net->sending)
        return BOOK8_NET_BUSY;
    enum book8_net_result result = book8_udp_bind(net, 68u, receive, d);
    if (result != BOOK8_NET_OK)
        return result;
    result = book8_net_configure(net, 0u, 0u, 0u);
    if (result != BOOK8_NET_OK) {
        enum book8_net_result unbound = book8_udp_unbind(net, 68u);
        return unbound != BOOK8_NET_OK ? unbound : result;
    }
    *d = (struct book8_dhcp){.net = net, .log = log, .xid = xid,
        .retry_ms = retry_ms, .now = now, .bound_port = true};
    state_set(d, BOOK8_DHCP_SELECTING);
    return BOOK8_NET_OK;
}

static size_t option32(uint8_t *p, uint8_t code, uint32_t value)
{
    p[0] = code;
    p[1] = 4u;
    put32(p + 2u, value);
    return 6u;
}

static enum book8_net_result transmit(struct book8_dhcp *d)
{
    uint8_t packet[300] = {0};
    packet[0] = 1u; packet[1] = 1u; packet[2] = 6u;
    put32(packet + 4u, d->xid);
    bool renewing = d->state == BOOK8_DHCP_RENEWING || d->state == BOOK8_DHCP_REBINDING;
    if (renewing)
        put32(packet + 12u, d->net->ip);
    if (d->state != BOOK8_DHCP_RENEWING)
        packet[10] = 0x80u;
    memcpy(packet + 28u, d->net->mac, 6u);
    put32(packet + 236u, 0x63825363u);
    size_t pos = 240u;
    packet[pos++] = 53u; packet[pos++] = 1u;
    packet[pos++] = d->state == BOOK8_DHCP_SELECTING ? 1u : 3u;
    packet[pos++] = 61u; packet[pos++] = 7u; packet[pos++] = 1u;
    memcpy(packet + pos, d->net->mac, 6u); pos += 6u;
    if (d->state == BOOK8_DHCP_REQUESTING) {
        pos += option32(packet + pos, 50u, d->offered_ip);
        pos += option32(packet + pos, 54u, d->server);
    }
    packet[pos++] = 55u; packet[pos++] = 6u;
    packet[pos++] = 1u; packet[pos++] = 3u; packet[pos++] = 6u;
    packet[pos++] = 51u; packet[pos++] = 58u; packet[pos++] = 59u;
    packet[pos] = 255u;
    if (d->state == BOOK8_DHCP_RENEWING)
        return book8_udp_send(d->net, 68u, d->server, 67u, packet, sizeof(packet));
    return book8_udp_dhcp_broadcast(d->net, renewing ? d->net->ip : 0u,
        packet, sizeof(packet));
}

static enum book8_net_result restart(struct book8_dhcp *d)
{
    bool had_lease = d->state == BOOK8_DHCP_BOUND
        || d->state == BOOK8_DHCP_RENEWING || d->state == BOOK8_DHCP_REBINDING;
    enum book8_net_result result = d->fallback_active ? BOOK8_NET_OK
        : book8_net_configure(d->net, 0u, 0u, 0u);
    if (result != BOOK8_NET_OK)
        return result;
    ++d->generation;
    d->dns = d->lease_seconds = 0u;
    d->server = d->offered_ip = 0u;
    d->pending = 0u;
    if (had_lease)
        d->fallback_at = d->now + BOOK8_DHCP_FALLBACK_MS;
    if (++d->xid == 0u)
        ++d->xid;
    state_set(d, BOOK8_DHCP_SELECTING);
    return BOOK8_NET_OK;
}

enum book8_net_result book8_dhcp_renew(struct book8_dhcp *d, uint64_t now)
{
    if (d == NULL)
        return BOOK8_NET_INVALID;
    if (!d->bound_port)
        return BOOK8_NET_UNCONFIGURED;
    if (d->net->receiving || d->net->sending)
        return BOOK8_NET_BUSY;
    d->now = now;
    if (d->state == BOOK8_DHCP_BOUND) {
        if (++d->xid == 0u)
            ++d->xid;
        state_set(d, BOOK8_DHCP_RENEWING);
        return BOOK8_NET_OK;
    }
    return restart(d);
}

enum book8_net_result book8_dhcp_poll(struct book8_dhcp *d, uint64_t now)
{
    if (d == NULL)
        return BOOK8_NET_INVALID;
    if (d->state == BOOK8_DHCP_OFF)
        return BOOK8_NET_OK;
    if (d->net->receiving || d->net->sending)
        return BOOK8_NET_BUSY;
    d->now = now;
    if ((d->state == BOOK8_DHCP_BOUND || d->state == BOOK8_DHCP_RENEWING
        || d->state == BOOK8_DHCP_REBINDING) && now >= d->expires) {
        d->log("DHCP lease expired; IPv4 removed");
        return restart(d);
    }
    if (d->pending == 6u) {
        d->log("DHCP NAK; lease discarded");
        return restart(d);
    }
    if (d->pending == 2u) {
        d->offered_ip = d->pending_ip;
        d->server = d->pending_server;
        d->pending = 0u;
        state_set(d, BOOK8_DHCP_REQUESTING);
    } else if (d->pending == 5u) {
        /* NETSAVE static IP persists beyond the 30s DHCP timeout: once the
           fallback is active, a late ACK must not overwrite the saved IPv4
           settings. DNS/lease metadata is still recorded for diagnostics. */
        if (!d->fallback_active) {
            enum book8_net_result result = book8_net_configure(d->net,
                d->pending_ip, d->pending_mask, d->pending_gateway);
            if (result != BOOK8_NET_OK)
                return result;
        } else {
            d->log("Late DHCP ACK ignored; NETSAVE static IP retained");
        }
        d->server = d->pending_server;
        d->dns = d->pending_dns;
        d->lease_seconds = d->pending_lease;
        d->expires = d->lease_seconds == UINT32_MAX ? UINT64_MAX
            : now + (uint64_t)d->lease_seconds * 1000u;
        d->renew_at = d->lease_seconds == UINT32_MAX ? UINT64_MAX
            : now + (uint64_t)d->pending_t1 * 1000u;
        d->rebind_at = d->lease_seconds == UINT32_MAX ? UINT64_MAX
            : now + (uint64_t)d->pending_t2 * 1000u;
        d->pending = 0u;
        /* fallback_active is intentionally left set when a static IP is in use. */
        ++d->generation;
        state_set(d, BOOK8_DHCP_BOUND);
        return BOOK8_NET_OK;
    }
    if (d->state == BOOK8_DHCP_BOUND && now >= d->renew_at) {
        if (++d->xid == 0u)
            ++d->xid;
        state_set(d, BOOK8_DHCP_RENEWING);
    }
    if (d->state == BOOK8_DHCP_RENEWING && now >= d->rebind_at)
        state_set(d, BOOK8_DHCP_REBINDING);
    if (!d->fallback_active && d->net->ip == 0u && d->fallback_ip != 0u
        && now >= d->fallback_at) {
        enum book8_net_result result = book8_net_configure(d->net, d->fallback_ip,
            d->fallback_mask, d->fallback_gateway);
        if (result != BOOK8_NET_OK)
            return result;
        d->fallback_active = true;
        ++d->generation;
        d->log("DHCP acquisition timed out; static fallback active, DHCP continues");
    }
    if (d->state == BOOK8_DHCP_BOUND || now < d->next_send)
        return BOOK8_NET_OK;
    if (d->state == BOOK8_DHCP_REQUESTING && d->attempts == 5u) {
        d->log("DHCP REQUEST retries exhausted; restarting discovery");
        return restart(d);
    }
    /* At most one transmission attempt per poll. */
    enum book8_net_result result = transmit(d);
    if (result == BOOK8_NET_ARP_PENDING || result == BOOK8_NET_BUSY) {
        d->next_send = now + (result == BOOK8_NET_ARP_PENDING ? 100u : d->retry_ms);
        return result == BOOK8_NET_ARP_PENDING ? BOOK8_NET_OK : result;
    }
    if (d->attempts < 7u)
        ++d->attempts;
    uint64_t wait = (uint64_t)d->retry_ms << (d->attempts - 1u);
    if (wait > 64000u)
        wait = 64000u;
    d->next_send = now + wait;
    if (result != BOOK8_NET_OK && result != BOOK8_NET_ARP_PENDING)
        d->log("DHCP transmission failed; retry scheduled");
    return result == BOOK8_NET_ARP_PENDING ? BOOK8_NET_OK : result;
}
