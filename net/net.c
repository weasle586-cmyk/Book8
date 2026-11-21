#include "net.h"
#include <string.h>

#define ETH_IPV4 0x0800u
#define ETH_ARP 0x0806u
#define ETH_SIZE sizeof(struct book8_eth_header)
#define IP_SIZE sizeof(struct book8_ipv4_header)
#define UDP_SIZE sizeof(struct book8_udp_header)

_Static_assert(ETH_SIZE == 14u, "Ethernet header size");
_Static_assert(sizeof(struct book8_arp_header) == 28u, "ARP header size");
_Static_assert(IP_SIZE == 20u, "IPv4 header size");
_Static_assert(sizeof(struct book8_icmp_header) == 8u, "ICMP echo header size");
_Static_assert(UDP_SIZE == 8u, "UDP header size");

static const uint8_t broadcast_mac[6] = {255u, 255u, 255u, 255u, 255u, 255u};

static bool same_mac(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6u) == 0;
}

static bool unicast_mac(const uint8_t mac[6])
{
    uint8_t any = 0u;
    for (unsigned i = 0u; i < 6u; ++i) {
        any |= mac[i];
    }
    return any != 0u && (mac[0] & 1u) == 0u;
}

static bool unicast_ip(uint32_t ip)
{
    uint32_t first = ip >> 24u;
    return first != 0u && first != 127u && first < 224u;
}

static bool subnet_host(uint32_t ip, uint32_t mask)
{
    uint32_t host = ip & ~mask;
    return unicast_ip(ip) && host != 0u && host != ~mask;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length >= 2u) {
        sum += ((uint32_t)data[0] << 8u) | data[1];
        data += 2u;
        length -= 2u;
    }
    if (length != 0u) {
        sum += (uint32_t)data[0] << 8u;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16u) {
        sum = (sum & 0xffffu) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

uint16_t book8_net_checksum(const void *data, size_t length)
{
    return checksum_finish(checksum_add(0u, data, length));
}

static uint16_t udp_checksum(uint32_t source, uint32_t destination,
    const uint8_t *udp, size_t length)
{
    uint32_t sum = (source >> 16u) + (source & 0xffffu)
        + (destination >> 16u) + (destination & 0xffffu) + 17u + length;
    return checksum_finish(checksum_add(sum, udp, length));
}

bool book8_net_parse_ip(const char *text, uint32_t *ip)
{
    if (text == NULL || ip == NULL) {
        return false;
    }
    uint32_t value = 0u;
    for (unsigned part = 0u; part < 4u; ++part) {
        uint32_t octet = 0u;
        unsigned digits = 0u;
        while (*text >= '0' && *text <= '9') {
            octet = octet * 10u + (uint32_t)(*text++ - '0');
            if (++digits > 3u || octet > 255u) {
                return false;
            }
        }
        if (digits == 0u || (part < 3u ? *text != '.' : *text != '\0')) {
            return false;
        }
        value = (value << 8u) | octet;
        if (part < 3u) {
            ++text;
        }
    }
    *ip = value;
    return true;
}

enum book8_net_result book8_net_init(struct book8_net *n,
    const struct book8_net_transport *transport, const uint8_t mac[6])
{
    if (n == NULL || transport == NULL || transport->send == NULL
        || transport->receive == NULL || mac == NULL || !unicast_mac(mac)) {
        return BOOK8_NET_INVALID;
    }
    memset(n, 0, sizeof(*n));
    n->transport = *transport;
    memcpy(n->mac, mac, 6u);
    return BOOK8_NET_OK;
}

enum book8_net_result book8_net_configure(struct book8_net *n,
    uint32_t ip, uint32_t mask, uint32_t gateway)
{
    if (n->receiving || n->sending) {
        return BOOK8_NET_BUSY;
    }
    uint32_t host_mask = ~mask;
    if (ip != 0u || mask != 0u || gateway != 0u) {
        if (mask == 0u || host_mask < 3u
            || (host_mask & (host_mask + 1u)) != 0u || !subnet_host(ip, mask)
            || (gateway != 0u && (!subnet_host(gateway, mask)
                || (gateway & mask) != (ip & mask) || gateway == ip))) {
            return BOOK8_NET_INVALID;
        }
    }
    n->ip = ip;
    n->mask = mask;
    n->gateway = gateway;
    memset(n->arp, 0, sizeof(n->arp));
    n->arp_next = 0u;
    return BOOK8_NET_OK;
}

static void ethernet(struct book8_net *n, const uint8_t destination[6],
    uint16_t type)
{
    struct book8_eth_header header;
    memcpy(header.destination, destination, 6u);
    memcpy(header.source, n->mac, 6u);
    header.type = book8_htons(type);
    memcpy(n->tx, &header, sizeof(header));
}

static enum book8_net_result emit(struct book8_net *n, size_t length)
{
    /* Transport consumes/copies bytes synchronously, as rtl8168_transmit does. */
    n->sending = true;
    enum book8_net_result result = n->transport.send(n->transport.ctx, n->tx, length);
    n->sending = false;
    if (result == BOOK8_NET_OK) {
        ++n->tx_frames;
    } else {
        ++n->tx_errors;
    }
    return result;
}

static void arp_learn(struct book8_net *n, uint32_t ip, const uint8_t mac[6])
{
    if (!subnet_host(ip, n->mask) || (ip & n->mask) != (n->ip & n->mask)
        || ip == n->ip || !unicast_mac(mac) || same_mac(mac, n->mac)) {
        return;
    }
    unsigned index;
    for (index = 0u; index < BOOK8_NET_ARP_COUNT; ++index) {
        if (n->arp[index].ip == ip) {
            break;
        }
    }
    if (index == BOOK8_NET_ARP_COUNT) {
        index = n->arp_next;
        n->arp_next = (uint8_t)((index + 1u) % BOOK8_NET_ARP_COUNT);
    }
    n->arp[index].ip = ip;
    memcpy(n->arp[index].mac, mac, 6u);
}

static enum book8_net_result arp_send(struct book8_net *n, uint16_t operation,
    const uint8_t target_mac[6], uint32_t target_ip)
{
    struct book8_arp_header arp = {
        .hardware_type = book8_htons(1u), .protocol_type = book8_htons(ETH_IPV4),
        .hardware_length = 6u, .protocol_length = 4u,
        .operation = book8_htons(operation),
        .sender_ip = book8_htonl(n->ip), .target_ip = book8_htonl(target_ip)
    };
    memcpy(arp.sender_mac, n->mac, 6u);
    if (operation == 2u) {
        memcpy(arp.target_mac, target_mac, 6u);
    }
    ethernet(n, target_mac, ETH_ARP);
    memcpy(n->tx + ETH_SIZE, &arp, sizeof(arp));
    return emit(n, ETH_SIZE + sizeof(arp));
}

static enum book8_net_result arp_input(struct book8_net *n,
    const struct book8_eth_header *eth, const uint8_t *data, size_t length)
{
    if (length < sizeof(struct book8_arp_header)) {
        return BOOK8_NET_INVALID;
    }
    struct book8_arp_header arp;
    memcpy(&arp, data, sizeof(arp));
    uint16_t operation = book8_ntohs(arp.operation);
    uint32_t sender = book8_ntohl(arp.sender_ip);
    if (book8_ntohs(arp.hardware_type) != 1u
        || book8_ntohs(arp.protocol_type) != ETH_IPV4
        || arp.hardware_length != 6u || arp.protocol_length != 4u
        || (operation != 1u && operation != 2u)
        || !same_mac(eth->source, arp.sender_mac)
        || !unicast_mac(arp.sender_mac)
        || (sender != 0u && !unicast_ip(sender))) {
        return BOOK8_NET_INVALID;
    }
    if (book8_ntohl(arp.target_ip) != n->ip) {
        return BOOK8_NET_IGNORED;
    }
    if (operation == 2u && (!same_mac(arp.target_mac, n->mac)
        || !same_mac(eth->destination, n->mac) || sender == 0u)) {
        return BOOK8_NET_INVALID;
    }
    arp_learn(n, sender, arp.sender_mac);
    if (operation == 1u) {
        enum book8_net_result result = arp_send(n, 2u, arp.sender_mac, sender);
        if (result == BOOK8_NET_OK) {
            ++n->arp_replies;
        }
        return result;
    }
    return BOOK8_NET_OK;
}

static void ipv4_output(struct book8_net *n, const uint8_t destination_mac[6],
    uint32_t destination, uint8_t protocol, size_t payload_length)
{
    ethernet(n, destination_mac, ETH_IPV4);
    struct book8_ipv4_header ip = {
        .version_ihl = 0x45u, .total_length = book8_htons(IP_SIZE + payload_length),
        .identification = book8_htons(n->next_id++), .fragment = book8_htons(0x4000u),
        .ttl = 64u, .protocol = protocol,
        .source = book8_htonl(n->ip), .destination = book8_htonl(destination)
    };
    ip.checksum = book8_htons(book8_net_checksum(&ip, sizeof(ip)));
    memcpy(n->tx + ETH_SIZE, &ip, sizeof(ip));
}

static enum book8_net_result ipv4_input(struct book8_net *n,
    const struct book8_eth_header *eth, const uint8_t *data, size_t length)
{
    if (length < IP_SIZE) {
        return BOOK8_NET_INVALID;
    }
    struct book8_ipv4_header ip;
    memcpy(&ip, data, sizeof(ip));
    size_t ihl = (ip.version_ihl & 15u) * 4u;
    size_t total = book8_ntohs(ip.total_length);
    uint32_t source = book8_ntohl(ip.source);
    if ((ip.version_ihl >> 4u) != 4u || ihl < IP_SIZE || ihl > length
        || total < ihl || total > length || ip.ttl == 0u
        || (book8_ntohs(ip.fragment) & 0xbfffu) != 0u
        || book8_net_checksum(data, ihl) != 0u || !unicast_ip(source)) {
        return BOOK8_NET_INVALID;
    }
    uint32_t destination = book8_ntohl(ip.destination);
    const uint8_t *payload = data + ihl;
    size_t payload_length = total - ihl;
    bool dhcp = ip.protocol == 17u && payload_length >= UDP_SIZE
        && payload[0] == 0u && payload[1] == 67u
        && payload[2] == 0u && payload[3] == 68u;
    bool dhcp_address = dhcp && (destination == UINT32_MAX
        || destination == n->ip || same_mac(eth->destination, n->mac));
    if (!dhcp_address && (n->ip == 0u || destination != n->ip
        || !same_mac(eth->destination, n->mac))) {
        return BOOK8_NET_IGNORED;
    }
    /* Options are unsupported (including source routing); never echo them. */
    if (ihl != IP_SIZE) {
        return BOOK8_NET_IGNORED;
    }
    if (ip.protocol == 1u) {
        if (payload_length < sizeof(struct book8_icmp_header)
            || book8_net_checksum(payload, payload_length) != 0u) {
            return BOOK8_NET_INVALID;
        }
        if (payload[0] != 8u || payload[1] != 0u) {
            return BOOK8_NET_IGNORED;
        }
        ipv4_output(n, eth->source, source, 1u, payload_length);
        uint8_t *reply = n->tx + ETH_SIZE + IP_SIZE;
        memcpy(reply, payload, payload_length);
        reply[0] = reply[2] = reply[3] = 0u;
        uint16_t check = book8_htons(book8_net_checksum(reply, payload_length));
        memcpy(reply + 2u, &check, sizeof(check));
        enum book8_net_result result = emit(n, ETH_SIZE + IP_SIZE + payload_length);
        if (result == BOOK8_NET_OK) {
            ++n->echo_replies;
        }
        return result;
    }
    if (ip.protocol == 6u && n->tcp_input != NULL) {
        return n->tcp_input(n->tcp_context, source, payload, payload_length);
    }
    if (ip.protocol != 17u) {
        return BOOK8_NET_IGNORED;
    }
    if (payload_length < UDP_SIZE) {
        return BOOK8_NET_INVALID;
    }
    struct book8_udp_header udp;
    memcpy(&udp, payload, sizeof(udp));
    uint16_t port = book8_ntohs(udp.destination);
    if (book8_ntohs(udp.length) != payload_length || port == 0u
        || book8_ntohs(udp.source) == 0u
        || (udp.checksum != 0u && udp_checksum(source, destination, payload,
            payload_length) != 0u)) {
        return BOOK8_NET_INVALID;
    }
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        if (n->sockets[i].port == port) {
            struct book8_udp_binding binding = n->sockets[i];
            ++n->udp_received;
            binding.callback(binding.ctx, source, book8_ntohs(udp.source),
                payload + UDP_SIZE, payload_length - UDP_SIZE);
            return BOOK8_NET_OK;
        }
    }
    return BOOK8_NET_IGNORED;
}

static enum book8_net_result frame_input(struct book8_net *n,
    const void *frame, size_t length)
{
    if (frame == NULL || length < ETH_SIZE || length > BOOK8_NET_FRAME_MAX) {
        return BOOK8_NET_INVALID;
    }
    struct book8_eth_header eth;
    memcpy(&eth, frame, sizeof(eth));
    if (!unicast_mac(eth.source)) {
        return BOOK8_NET_INVALID;
    }
    if ((!same_mac(eth.destination, n->mac)
            && !same_mac(eth.destination, broadcast_mac))
        || same_mac(eth.source, n->mac)) {
        return BOOK8_NET_IGNORED;
    }
    const uint8_t *payload = (const uint8_t *)frame + ETH_SIZE;
    if (book8_ntohs(eth.type) == ETH_ARP) {
        if (n->ip == 0u)
            return BOOK8_NET_UNCONFIGURED;
        return arp_input(n, &eth, payload, length - ETH_SIZE);
    }
    if (book8_ntohs(eth.type) == ETH_IPV4) {
        return ipv4_input(n, &eth, payload, length - ETH_SIZE);
    }
    return BOOK8_NET_IGNORED;
}

enum book8_net_result book8_net_input(struct book8_net *n,
    const void *frame, size_t length)
{
    if (n->receiving || n->sending) {
        return BOOK8_NET_BUSY;
    }
    n->receiving = true;
    ++n->rx_frames;
    enum book8_net_result result = frame_input(n, frame, length);
    n->receiving = false;
    if (result == BOOK8_NET_INVALID) {
        ++n->invalid_frames;
    } else if (result == BOOK8_NET_IGNORED || result == BOOK8_NET_UNCONFIGURED) {
        ++n->ignored_frames;
    }
    return result;
}

enum book8_net_result book8_net_poll(struct book8_net *n)
{
    if (n->receiving || n->sending) {
        return BOOK8_NET_BUSY;
    }
    for (unsigned i = 0u; i < BOOK8_NET_POLL_BUDGET; ++i) {
        size_t length = 0u;
        enum book8_net_result result = n->transport.receive(n->transport.ctx,
            n->rx, sizeof(n->rx), &length);
        if (result == BOOK8_NET_EMPTY) {
            return BOOK8_NET_OK;
        }
        if (result != BOOK8_NET_OK) {
            ++n->rx_errors;
            if (result == BOOK8_NET_RX_DROPPED) {
                continue;
            }
            return result;
        }
        result = book8_net_input(n, n->rx, length);
        if (result != BOOK8_NET_OK && result != BOOK8_NET_INVALID
            && result != BOOK8_NET_IGNORED && result != BOOK8_NET_UNCONFIGURED) {
            return result;
        }
    }
    return BOOK8_NET_OK;
}

enum book8_net_result book8_udp_bind(struct book8_net *n, uint16_t port,
    book8_udp_callback callback, void *ctx)
{
    if (port == 0u || callback == NULL) {
        return BOOK8_NET_INVALID;
    }
    unsigned available = BOOK8_NET_SOCKET_COUNT;
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        if (n->sockets[i].port == port) {
            return BOOK8_NET_PORT_IN_USE;
        }
        if (n->sockets[i].port == 0u && available == BOOK8_NET_SOCKET_COUNT) {
            available = i;
        }
    }
    if (available == BOOK8_NET_SOCKET_COUNT) {
        return BOOK8_NET_NO_SLOTS;
    }
    n->sockets[available] = (struct book8_udp_binding){port, callback, ctx};
    return BOOK8_NET_OK;
}

enum book8_net_result book8_udp_unbind(struct book8_net *n, uint16_t port)
{
    if (port == 0u) {
        return BOOK8_NET_INVALID;
    }
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        if (n->sockets[i].port == port) {
            n->sockets[i] = (struct book8_udp_binding){0};
            return BOOK8_NET_OK;
        }
    }
    return BOOK8_NET_NOT_BOUND;
}

static enum book8_net_result route(struct book8_net *n, uint32_t destination,
    const uint8_t **mac)
{
    if (n->ip == 0u) { return BOOK8_NET_UNCONFIGURED; }
    if (!unicast_ip(destination) || destination == n->ip) { return BOOK8_NET_INVALID; }
    uint32_t hop = destination;
    if ((destination & n->mask) != (n->ip & n->mask)) {
        hop = n->gateway;
        if (hop == 0u) { return BOOK8_NET_NO_ROUTE; }
    } else if (!subnet_host(destination, n->mask)) { return BOOK8_NET_INVALID; }
    for (unsigned i = 0u; i < BOOK8_NET_ARP_COUNT; ++i) {
        if (n->arp[i].ip == hop) { *mac = n->arp[i].mac; return BOOK8_NET_OK; }
    }
    enum book8_net_result result = arp_send(n, 1u, broadcast_mac, hop);
    return result == BOOK8_NET_OK ? BOOK8_NET_ARP_PENDING : result;
}

enum book8_net_result book8_net_send_ipv4(struct book8_net *n, uint32_t destination,
    uint8_t protocol, const void *payload, size_t length)
{
    if (n == NULL || payload == NULL || length > 1480u) { return BOOK8_NET_INVALID; }
    if (n->sending) { return BOOK8_NET_BUSY; }
    const uint8_t *mac = NULL;
    enum book8_net_result result = route(n, destination, &mac);
    if (result != BOOK8_NET_OK) { return result; }
    ipv4_output(n, mac, destination, protocol, length);
    memcpy(n->tx + ETH_SIZE + IP_SIZE, payload, length);
    return emit(n, ETH_SIZE + IP_SIZE + length);
}

enum book8_net_result book8_udp_send(struct book8_net *n, uint16_t local_port,
    uint32_t destination, uint16_t destination_port, const void *payload, size_t length)
{
    if (n->sending) {
        return BOOK8_NET_BUSY;
    }
    if (destination_port == 0u || local_port == 0u
        || !unicast_ip(destination) || destination == n->ip
        || length > BOOK8_NET_UDP_MAX || (length != 0u && payload == NULL)) {
        return BOOK8_NET_INVALID;
    }
    bool bound = false;
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        bound |= n->sockets[i].port == local_port;
    }
    if (!bound) {
        return BOOK8_NET_NOT_BOUND;
    }
    const uint8_t *mac = NULL;
    enum book8_net_result result = route(n, destination, &mac);
    if (result != BOOK8_NET_OK) { return result; }
    ipv4_output(n, mac, destination, 17u, UDP_SIZE + length);
    uint8_t *data = n->tx + ETH_SIZE + IP_SIZE;
    struct book8_udp_header udp = {
        .source = book8_htons(local_port), .destination = book8_htons(destination_port),
        .length = book8_htons(UDP_SIZE + length)
    };
    if (length != 0u) {
        memcpy(data + UDP_SIZE, payload, length);
    }
    memcpy(data, &udp, sizeof(udp));
    uint16_t check = udp_checksum(n->ip, destination, data, UDP_SIZE + length);
    udp.checksum = book8_htons(check == 0u ? 0xffffu : check);
    memcpy(data, &udp, sizeof(udp));
    return emit(n, ETH_SIZE + IP_SIZE + UDP_SIZE + length);
}

enum book8_net_result book8_udp_dhcp_broadcast(struct book8_net *n,
    uint32_t source_ip, const void *payload, size_t length)
{
    if (n->sending || n->receiving)
        return BOOK8_NET_BUSY;
    if (payload == NULL || length > BOOK8_NET_UDP_MAX
        || (source_ip != 0u && source_ip != n->ip))
        return BOOK8_NET_INVALID;
    bool bound = false;
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i)
        bound |= n->sockets[i].port == 68u;
    if (!bound)
        return BOOK8_NET_NOT_BOUND;
    ipv4_output(n, broadcast_mac, UINT32_MAX, 17u, UDP_SIZE + length);
    struct book8_ipv4_header ip;
    memcpy(&ip, n->tx + ETH_SIZE, sizeof(ip));
    ip.source = book8_htonl(source_ip);
    ip.checksum = 0u;
    ip.checksum = book8_htons(book8_net_checksum(&ip, sizeof(ip)));
    memcpy(n->tx + ETH_SIZE, &ip, sizeof(ip));
    struct book8_udp_header udp = {.source = book8_htons(68u),
        .destination = book8_htons(67u), .length = book8_htons(UDP_SIZE + length)};
    uint8_t *data = n->tx + ETH_SIZE + IP_SIZE;
    memcpy(data, &udp, sizeof(udp));
    memcpy(data + UDP_SIZE, payload, length);
    uint16_t check = udp_checksum(source_ip, UINT32_MAX, data, UDP_SIZE + length);
    udp.checksum = book8_htons(check == 0u ? 0xffffu : check);
    memcpy(data, &udp, sizeof(udp));
    return emit(n, ETH_SIZE + IP_SIZE + UDP_SIZE + length);
}

const char *book8_net_result_string(enum book8_net_result result)
{
    switch (result) {
    case BOOK8_NET_OK: return "ok";
    case BOOK8_NET_EMPTY: return "receive queue empty";
    case BOOK8_NET_IGNORED: return "packet ignored";
    case BOOK8_NET_INVALID: return "invalid packet or configuration";
    case BOOK8_NET_UNCONFIGURED: return "IPv4 not configured";
    case BOOK8_NET_NO_ROUTE: return "no route to destination";
    case BOOK8_NET_ARP_PENDING: return "ARP pending; poll and retry datagram";
    case BOOK8_NET_PORT_IN_USE: return "UDP port already bound";
    case BOOK8_NET_NO_SLOTS: return "UDP binding table full";
    case BOOK8_NET_NOT_BOUND: return "UDP port not bound";
    case BOOK8_NET_IO_ERROR: return "NIC packet I/O failed";
    case BOOK8_NET_BUSY: return "network service busy";
    case BOOK8_NET_RX_DROPPED: return "NIC dropped frame";
    case BOOK8_NET_TIMEOUT: return "TFTP transfer timed out";
    case BOOK8_NET_REMOTE_ERROR: return "TFTP server returned ERROR";
    case BOOK8_NET_TOO_LARGE: return "TFTP image exceeds load buffer";
    case BOOK8_NET_BAD_IMAGE: return "invalid warm kernel signature, layout or CRC32";
    }
    return "unknown network result";
}

_Static_assert(sizeof(struct net_telemetry_header) == 16u, "telemetry header");
_Static_assert(sizeof(struct net_telemetry_metrics) == 60u, "telemetry metrics");
_Static_assert(sizeof(struct net_telemetry_packet) == 204u, "telemetry packet");
_Static_assert(_Alignof(struct net_telemetry_packet) == 1u, "telemetry packing");
_Static_assert(sizeof(struct net_telemetry_extended) == 88u, "telemetry v2 extension");
_Static_assert(sizeof(struct net_telemetry_packet_v2) == 292u, "telemetry v2 packet");
_Static_assert(offsetof(struct net_telemetry_packet_v2, payload.extended) == 76u,
    "telemetry v2 extension offset");
_Static_assert(_Alignof(struct net_telemetry_packet_v2) == 1u, "telemetry v2 packing");

static struct {
    struct book8_net *net;
    uint32_t target, sequence;
    bool busy;
    union {
        struct net_telemetry_packet v1;
        struct net_telemetry_packet_v2 v2;
    } packet;
} telemetry;

static void telemetry_receive(void *ctx, uint32_t ip, uint16_t port,
    const uint8_t *data, size_t length)
{
    /* Transmit-only telemetry binding; incoming datagrams have no commands. */
    (void)ctx;
    (void)ip;
    (void)port;
    (void)data;
    (void)length;
}

static bool telemetry_owns_port(struct book8_net *n)
{
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        if (n->sockets[i].port == BOOK8_TELEMETRY_PORT
            && n->sockets[i].callback == telemetry_receive
            && n->sockets[i].ctx == &telemetry) {
            return true;
        }
    }
    return false;
}

enum book8_net_result net_telemetry_init(struct book8_net *n, uint32_t target)
{
    if (telemetry.busy || (n != NULL && (n->receiving || n->sending))
        || (telemetry.net != NULL
            && (telemetry.net->receiving || telemetry.net->sending))) {
        return BOOK8_NET_BUSY;
    }
    if (target != 0u) {
        if (n == NULL || n->transport.send == NULL || !unicast_ip(target)
            || target == n->ip) {
            return BOOK8_NET_INVALID;
        }
        if (n->ip == 0u) {
            return BOOK8_NET_UNCONFIGURED;
        }
        if ((target & n->mask) == (n->ip & n->mask)) {
            if (!subnet_host(target, n->mask)) {
                return BOOK8_NET_INVALID;
            }
        } else if (n->gateway == 0u) {
            return BOOK8_NET_NO_ROUTE;
        }
        if (!telemetry_owns_port(n)) {
            enum book8_net_result result = book8_udp_bind(n, BOOK8_TELEMETRY_PORT,
                telemetry_receive, &telemetry);
            if (result != BOOK8_NET_OK) {
                return result;
            }

        }
    }
    if (telemetry.net != NULL && (target == 0u || telemetry.net != n)
        && telemetry_owns_port(telemetry.net)) {
        enum book8_net_result result = book8_udp_unbind(telemetry.net, BOOK8_TELEMETRY_PORT);
        if (result != BOOK8_NET_OK) {
            return result;
        }
    }
    telemetry.net = target != 0u ? n : NULL;
    telemetry.target = target;
    telemetry.sequence = 0u;
    return BOOK8_NET_OK;
}

_Static_assert(sizeof(struct book8_tftp_rrq) == 2u, "TFTP RRQ header");
_Static_assert(sizeof(struct book8_tftp_data) == 4u, "TFTP DATA header");
_Static_assert(sizeof(struct book8_tftp_ack) == 4u, "TFTP ACK header");
_Static_assert(sizeof(struct book8_tftp_error) == 4u, "TFTP ERROR header");
_Static_assert(sizeof(struct book8_warm_header) == 32u, "warm header");

static struct {
    struct book8_net *net;
    struct book8_tftp_clock clock;
    uint32_t server;
    size_t capacity, size;
    uint8_t *load;
    uint16_t local_port, server_port, block, remote_error;
    uint16_t next_port;
    uint8_t request[2u + BOOK8_TFTP_FILENAME_MAX + 1u + 6u];
    size_t request_size;
    bool active, complete, need_ack, progressed;
    enum book8_net_result error;
} tftp;

enum book8_net_result tftp_client_init(struct book8_net *n, uint32_t server,
    size_t capacity, const struct book8_tftp_clock *clock)
{
    if (tftp.active || (n != NULL && (n->receiving || n->sending))) {
        return BOOK8_NET_BUSY;
    }
    if (n == NULL || n->transport.send == NULL || clock == NULL
        || clock->milliseconds == NULL || clock->idle == NULL
        || capacity == 0u || capacity > BOOK8_TFTP_CAPACITY
        || !unicast_ip(server) || server == n->ip) {
        return BOOK8_NET_INVALID;
    }
    if (n->ip == 0u) {
        return BOOK8_NET_UNCONFIGURED;
    }
    if ((server & n->mask) == (n->ip & n->mask)) {
        if (!subnet_host(server, n->mask)) {
            return BOOK8_NET_INVALID;
        }
    } else if (n->gateway == 0u) {
        return BOOK8_NET_NO_ROUTE;
    }
    tftp.net = n;
    tftp.server = server;
    tftp.capacity = capacity;
    tftp.clock = *clock;
    tftp.size = 0u;
    return BOOK8_NET_OK;
}

static void tftp_input(void *ctx, uint32_t ip, uint16_t port,
    const uint8_t *payload, size_t length)
{
    (void)ctx;
    if (!tftp.active || ip != tftp.server || port == 0u || length < 4u
        || (tftp.server_port != 0u && port != tftp.server_port)
        || tftp.error != BOOK8_NET_OK) {
        return;
    }
    struct book8_tftp_data data;
    memcpy(&data, payload, sizeof(data));
    uint16_t opcode = book8_ntohs(data.opcode);
    uint16_t block = book8_ntohs(data.block);
    if (opcode == 5u) {
        if (length < 5u || memchr(payload + 4u, 0, length - 4u) == NULL) {
            return;
        }
        tftp.remote_error = block;
        tftp.error = BOOK8_NET_REMOTE_ERROR;
        return;
    }
    if (opcode != 3u || length > 4u + BOOK8_TFTP_BLOCK) {
        return;
    }
    if (tftp.server_port == 0u && block != 1u) {
        return;
    }
    if (tftp.server_port == 0u) {
        tftp.server_port = port;
    }
    if (block == tftp.block && tftp.block != 0u) {
        tftp.need_ack = true; /* Lost ACK: re-ACK, never copy the block twice. */
        return;
    }
    if (tftp.complete || block != (uint16_t)(tftp.block + 1u)) {
        return;
    }
    size_t bytes = length - sizeof(data);
    if (bytes > tftp.capacity - tftp.size) {
        tftp.error = BOOK8_NET_TOO_LARGE;
        return;
    }
    memcpy(tftp.load + tftp.size, payload + sizeof(data), bytes);
    tftp.size += bytes;
    tftp.block = block;
    tftp.need_ack = true;
    tftp.progressed = true;
    tftp.complete = bytes < BOOK8_TFTP_BLOCK;
}

static enum book8_net_result tftp_send_current(void)
{
    if (tftp.block == 0u) {
        return book8_udp_send(tftp.net, tftp.local_port, tftp.server, 69u,
            tftp.request, tftp.request_size);
    }
    struct book8_tftp_ack ack = {book8_htons(4u), book8_htons(tftp.block)};
    return book8_udp_send(tftp.net, tftp.local_port, tftp.server,
        tftp.server_port, &ack, sizeof(ack));
}

enum book8_net_result tftp_fetch_kernel(const char *filename, uint8_t *load)
{
    if (tftp.active || (tftp.net != NULL
        && (tftp.net->receiving || tftp.net->sending))) {
        return BOOK8_NET_BUSY;
    }
    tftp.size = 0u;
    tftp.remote_error = 0u;
    if (tftp.net == NULL) {
        return BOOK8_NET_UNCONFIGURED;
    }
    if (filename == NULL || load == NULL
        || (uintptr_t)load > UINTPTR_MAX - (tftp.capacity - 1u)) {
        return BOOK8_NET_INVALID;
    }
    size_t length = 0u;
    while (length <= BOOK8_TFTP_FILENAME_MAX && filename[length] != '\0') {
        if ((uint8_t)filename[length] < 0x20u || (uint8_t)filename[length] > 0x7eu) {
            return BOOK8_NET_INVALID;
        }
        ++length;
    }
    if (length == 0u || length > BOOK8_TFTP_FILENAME_MAX) {
        return BOOK8_NET_INVALID;
    }
    struct book8_tftp_rrq rrq = {book8_htons(1u)};
    memcpy(tftp.request, &rrq, sizeof(rrq));
    memcpy(tftp.request + 2u, filename, length + 1u);
    memcpy(tftp.request + 3u + length, "octet", 6u);
    tftp.request_size = length + 9u;
    enum book8_net_result result = BOOK8_NET_NO_SLOTS;
    for (unsigned i = 0u; i < BOOK8_NET_SOCKET_COUNT; ++i) {
        tftp.local_port = (uint16_t)(49152u + tftp.next_port++ % 16384u);
        result = book8_udp_bind(tftp.net, tftp.local_port, tftp_input, &tftp);
        if (result != BOOK8_NET_PORT_IN_USE) {
            break;
        }
    }
    if (result != BOOK8_NET_OK) {
        return result;
    }
    tftp.load = load;
    tftp.block = tftp.server_port = 0u;
    tftp.complete = tftp.need_ack = tftp.progressed = false;
    tftp.error = BOOK8_NET_OK;
    tftp.active = true;
    result = tftp_send_current();
    uint64_t deadline = tftp.clock.milliseconds(tftp.clock.ctx);
    uint64_t finish = 0u;
    unsigned retries = 0u;
    bool final_acked = false;
    while (result == BOOK8_NET_OK || result == BOOK8_NET_ARP_PENDING) {
        enum book8_net_result polled = book8_net_poll(tftp.net);
        if (polled != BOOK8_NET_OK) {
            result = polled;
            break;
        }
        if (tftp.error != BOOK8_NET_OK) {
            result = tftp.error;
            break;
        }
        uint64_t now = tftp.clock.milliseconds(tftp.clock.ctx);
        if (tftp.need_ack) {
            result = tftp_send_current();
            if (result != BOOK8_NET_OK && result != BOOK8_NET_ARP_PENDING) {
                break;
            }
            tftp.need_ack = false;
            if (tftp.progressed) {
                retries = 0u;
                deadline = now;
                tftp.progressed = false;
            }
            if (tftp.complete && result == BOOK8_NET_OK && !final_acked) {
                final_acked = true;
                finish = now;
            }
        }
        if (final_acked && now - finish >= BOOK8_TFTP_TIMEOUT_MS * 2u) {
            result = BOOK8_NET_OK;
            break;
        }
        if (!final_acked && now - deadline >= BOOK8_TFTP_TIMEOUT_MS) {
            if (retries++ == BOOK8_TFTP_RETRIES) {
                result = BOOK8_NET_TIMEOUT;
                break;
            }
            result = tftp_send_current();
            deadline = now;
            if (tftp.complete && result == BOOK8_NET_OK) {
                final_acked = true;
                finish = now;
            }
        }
        tftp.clock.idle(tftp.clock.ctx);
    }
    enum book8_net_result unbound = book8_udp_unbind(tftp.net, tftp.local_port);
    tftp.active = false;
    if (result == BOOK8_NET_OK && unbound != BOOK8_NET_OK) {
        result = unbound;
    }
    if (result != BOOK8_NET_OK) {
        tftp.size = 0u; /* Partial bytes must never be treated as an executable. */
    }
    return result;
}

size_t tftp_fetched_size(void) { return tftp.size; }
uint16_t tftp_remote_error(void) { return tftp.remote_error; }

uint32_t book8_netboot_crc32(const void *image, size_t length)
{
    const uint8_t *bytes = image;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0u; i < length; ++i) {
        uint8_t byte = i >= offsetof(struct book8_warm_header, crc32)
            && i < sizeof(struct book8_warm_header) ? 0u : bytes[i];
        crc ^= byte;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return ~crc;
}

enum book8_net_result book8_netboot_validate(const void *image, size_t length,
    size_t capacity, struct book8_warm_header *out)
{
    if (image == NULL || out == NULL || length < sizeof(*out)
        || length > capacity || capacity > BOOK8_TFTP_CAPACITY) {
        return BOOK8_NET_BAD_IMAGE;
    }
    struct book8_warm_header h;
    memcpy(&h, image, sizeof(h));
    if (h.magic != BOOK8_WARM_MAGIC || h.version != 1u || h.header_bytes != sizeof(h)
        || h.image_bytes != length || h.memory_bytes < length || h.memory_bytes > capacity
        || h.entry_offset < sizeof(h) || h.entry_offset >= length
        || h.crc32 != book8_netboot_crc32(image, length)) {
        return BOOK8_NET_BAD_IMAGE;
    }
    *out = h;
    return BOOK8_NET_OK;
}

static struct net_telemetry_metrics telemetry_metrics_wire(
    const struct net_telemetry_metrics *m)
{
    return (struct net_telemetry_metrics){
        .cpu_flags = book8_htonl(m->cpu_flags), .cpu_mhz = book8_htonl(m->cpu_mhz),
        .memory_total_bytes = book8_htonll(m->memory_total_bytes),
        .kernel_reserved_bytes = book8_htonll(m->kernel_reserved_bytes),
        .heap_used_bytes = book8_htonll(m->heap_used_bytes),
        .heap_total_bytes = book8_htonll(m->heap_total_bytes),
        .tx_packets = book8_htonl(m->tx_packets), .rx_packets = book8_htonl(m->rx_packets),
        .tx_dropped = book8_htonl(m->tx_dropped), .rx_dropped = book8_htonl(m->rx_dropped),
        .tx_timeouts = book8_htonl(m->tx_timeouts)
    };
}

static enum book8_net_result telemetry_emit(uint64_t timestamp_ms,
    const struct net_telemetry_metrics *m, const struct net_telemetry_extended *x,
    const char *log)
{
    if (telemetry.busy) {
        return BOOK8_NET_BUSY;
    }
    if (telemetry.net == NULL) {
        return BOOK8_NET_UNCONFIGURED;
    }
    if (m == NULL) {
        return BOOK8_NET_INVALID;
    }
    if (!telemetry_owns_port(telemetry.net)) {
        return BOOK8_NET_NOT_BOUND;
    }
    telemetry.busy = true;
    memset(&telemetry.packet, 0, sizeof(telemetry.packet));
    struct net_telemetry_header header = {
        book8_htonl(x != NULL ? BOOK8_TELEMETRY_MAGIC_V2 : BOOK8_TELEMETRY_MAGIC),
        book8_htonl(telemetry.sequence), book8_htonll(timestamp_ms)
    };
    char *text;
    size_t size;
    if (x != NULL) {
        struct net_telemetry_extended wire;
        const uint32_t *source = (const uint32_t *)(const void *)x;
        uint32_t *destination = (uint32_t *)(void *)&wire;
        uint32_t value;
        for (size_t i = 0u; i < sizeof(wire) / sizeof(uint32_t); ++i) {
            memcpy(&value, (const uint8_t *)source + i * sizeof(value), sizeof(value));
            value = book8_htonl(value);
            memcpy((uint8_t *)destination + i * sizeof(value), &value, sizeof(value));
        }
        telemetry.packet.v2.header = header;
        telemetry.packet.v2.payload.metrics = telemetry_metrics_wire(m);
        telemetry.packet.v2.payload.extended = wire;
        text = telemetry.packet.v2.payload.log;
        size = sizeof(telemetry.packet.v2);
    } else {
        telemetry.packet.v1.header = header;
        telemetry.packet.v1.payload.metrics = telemetry_metrics_wire(m);
        text = telemetry.packet.v1.payload.log;
        size = sizeof(telemetry.packet.v1);
    }
    if (log != NULL) {
        for (size_t i = 0u; i < BOOK8_TELEMETRY_LOG_SIZE - 1u && log[i] != '\0'; ++i) {
            text[i] = log[i];
        }
    }
    enum book8_net_result result = book8_udp_send(telemetry.net, BOOK8_TELEMETRY_PORT,
        telemetry.target, BOOK8_TELEMETRY_PORT, &telemetry.packet, size);
    if (result == BOOK8_NET_OK) {
        ++telemetry.sequence;
    }
    telemetry.busy = false;
    return result;
}

enum book8_net_result net_telemetry_send(uint64_t timestamp_ms,
    const struct net_telemetry_metrics *m, const char *log)
{
    return telemetry_emit(timestamp_ms, m, NULL, log);
}

enum book8_net_result net_telemetry_send_v2(uint64_t timestamp_ms,
    const struct net_telemetry_metrics *m, const struct net_telemetry_extended *x,
    const char *log)
{
    if (x == NULL) {
        return BOOK8_NET_INVALID;
    }
    return telemetry_emit(timestamp_ms, m, x, log);
}

uint32_t book8_net_arp_entries(const struct book8_net *n)
{
    uint32_t count = 0u;
    for (unsigned i = 0u; n != NULL && i < BOOK8_NET_ARP_COUNT; ++i) {
        count += n->arp[i].ip != 0u ? 1u : 0u;
    }
    return count;
}

static const char *const netcfg_keys[4] = {"IP", "MASK", "GATEWAY", "TELEMETRY"};

static bool netcfg_mask_valid(uint32_t mask)
{
    return mask != 0u && (~mask & (~mask + 1u)) == 0u;
}

bool book8_netcfg_parse(const char *text, size_t length, struct book8_netcfg *config)
{
    if (text == NULL || config == NULL || length > BOOK8_NETCFG_TEXT_MAX) {
        return false;
    }
    uint32_t values[4] = {0u, 0u, 0u, 0u};
    bool seen[4] = {false, false, false, false};
    bool header = false;
    size_t position = 0u;
    while (position < length) {
        char line[48];
        size_t used = 0u;
        while (position < length && text[position] != '\n') {
            char c = text[position++];
            if (c == '\0') {
                return false;
            }
            if (c == '\r') {
                continue;
            }
            if (used + 1u >= sizeof(line)) {
                return false;
            }
            line[used++] = c;
        }
        ++position;
        while (used != 0u && (line[used - 1u] == ' ' || line[used - 1u] == '\t')) {
            --used;
        }
        line[used] = '\0';
        if (used == 0u || line[0] == '#') {
            continue;
        }
        if (!header) {
            if (strcmp(line, "BOOK8NET 1") != 0) {
                return false;
            }
            header = true;
            continue;
        }
        char *value = line;
        while (*value != '\0' && *value != ' ') {
            ++value;
        }
        if (*value != ' ') {
            return false;
        }
        *value++ = '\0';
        unsigned key = 0u;
        while (key < 4u && strcmp(line, netcfg_keys[key]) != 0) {
            ++key;
        }
        if (key == 4u || seen[key] || !book8_net_parse_ip(value, &values[key])) {
            return false;
        }
        seen[key] = true;
    }
    if (!header || !seen[0] || !seen[1] || values[0] == 0u
        || !netcfg_mask_valid(values[1])) {
        return false;
    }
    *config = (struct book8_netcfg){values[0], values[1], values[2], values[3]};
    return true;
}

static size_t netcfg_append(char *text, size_t used, size_t capacity, const char *add)
{
    while (*add != '\0') {
        if (used + 1u >= capacity) {
            return SIZE_MAX;
        }
        text[used++] = *add++;
    }
    return used;
}

size_t book8_netcfg_format(const struct book8_netcfg *config, char *text, size_t capacity)
{
    if (config == NULL || text == NULL || capacity == 0u) {
        return 0u;
    }
    const uint32_t values[4] = {config->ip, config->mask, config->gateway,
        config->telemetry};
    size_t used = netcfg_append(text, 0u, capacity, "BOOK8NET 1\r\n");
    for (unsigned key = 0u; key < 4u && used != SIZE_MAX; ++key) {
        char address[16];
        size_t length = 0u;
        for (unsigned octet = 0u; octet < 4u; ++octet) {
            unsigned part = (values[key] >> (24u - octet * 8u)) & 255u;
            if (octet != 0u) {
                address[length++] = '.';
            }
            if (part >= 100u) {
                address[length++] = (char)('0' + part / 100u);
            }
            if (part >= 10u) {
                address[length++] = (char)('0' + part / 10u % 10u);
            }
            address[length++] = (char)('0' + part % 10u);
        }
        address[length] = '\0';
        used = netcfg_append(text, used, capacity, netcfg_keys[key]);
        if (used != SIZE_MAX) {
            used = netcfg_append(text, used, capacity, " ");
        }
        if (used != SIZE_MAX) {
            used = netcfg_append(text, used, capacity, address);
        }
        if (used != SIZE_MAX) {
            used = netcfg_append(text, used, capacity, "\r\n");
        }
    }
    if (used == SIZE_MAX) {
        text[0] = '\0';
        return 0u;
    }
    text[used] = '\0';
    return used;
}
