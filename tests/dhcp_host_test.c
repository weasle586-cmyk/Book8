#include "../net/dhcp.h"
#include "../net/network_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct book8_net net;
static struct book8_dhcp dhcp;
static uint8_t tx[1514], frame[1514];
static size_t tx_size;
static unsigned sends, logs;
static enum book8_net_result send_result;
static void bind_lease(void);
static const uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
static const uint32_t server = 0xc0a83201u, address = 0xc0a832d0u;

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24u); p[1] = (uint8_t)(value >> 16u);
    p[2] = (uint8_t)(value >> 8u); p[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u)
        | ((uint32_t)p[2] << 8u) | p[3];
}

static enum book8_net_result send(void *ctx, const void *p, size_t length)
{
    (void)ctx;
    ++sends; tx_size = length;
    memcpy(tx, p, length);
    return send_result;
}

static enum book8_net_result receive_empty(void *ctx, void *p, size_t cap, size_t *size)
{
    (void)ctx; (void)p; (void)cap; *size = 0u;
    return BOOK8_NET_EMPTY;
}

static void log_event(const char *p)
{
    assert(p != NULL); ++logs;
}

static void reset(void)
{
    memset(&dhcp, 0, sizeof(dhcp));
    sends = logs = 0u;
    send_result = BOOK8_NET_OK;
    struct book8_net_transport transport = {.send = send, .receive = receive_empty};
    assert(book8_net_init(&net, &transport, mac) == BOOK8_NET_OK);
    assert(book8_dhcp_start(&dhcp, &net, 0u, 0x12345678u, 1000u, log_event) == BOOK8_NET_OK);
}

static size_t opt32(uint8_t *p, uint8_t type, uint32_t value)
{
    p[0] = type; p[1] = 4u; put32(p + 2u, value); return 6u;
}

static size_t reply(uint8_t type, bool unicast)
{
    memset(frame, 0, sizeof(frame));
    memset(frame, 255, 6u);
    if (unicast) memcpy(frame, mac, 6u);
    frame[6] = 2u; frame[7] = 8u; frame[12] = 8u;
    uint8_t *ip = frame + 14u, *udp = ip + 20u, *p = udp + 8u;
    ip[0] = 0x45u; ip[8] = 64u; ip[9] = 17u;
    put32(ip + 12u, server);
    put32(ip + 16u, unicast ? address : UINT32_MAX);
    udp[1] = 67u; udp[3] = 68u;
    p[0] = 2u; p[1] = 1u; p[2] = 6u;
    put32(p + 4u, dhcp.xid);
    put32(p + 16u, address);
    memcpy(p + 28u, mac, 6u);
    put32(p + 236u, 0x63825363u);
    size_t pos = 240u;
    p[pos++] = 53u; p[pos++] = 1u; p[pos++] = type;
    pos += opt32(p + pos, 54u, server);
    if (type == 5u) {
        pos += opt32(p + pos, 1u, 0xffffff00u);
        pos += opt32(p + pos, 3u, server);
        pos += opt32(p + pos, 6u, 0x08080808u);
        pos += opt32(p + pos, 51u, 8u);
    }
    p[pos++] = 255u;
    uint16_t length = book8_htons((uint16_t)(pos + 8u));
    memcpy(udp + 4u, &length, 2u);
    length = book8_htons((uint16_t)(pos + 28u));
    memcpy(ip + 2u, &length, 2u);
    uint16_t checksum = book8_htons(book8_net_checksum(ip, 20u));
    memcpy(ip + 10u, &checksum, 2u);
    uint8_t pseudo[12 + 400] = {0};
    memcpy(pseudo, ip + 12u, 8u);
    pseudo[9] = 17u;
    memcpy(pseudo + 10u, udp + 4u, 2u);
    memcpy(pseudo + 12u, udp, pos + 8u);
    checksum = book8_net_checksum(pseudo, pos + 20u);
    checksum = book8_htons(checksum == 0u ? 0xffffu : checksum);
    memcpy(udp + 6u, &checksum, 2u);
    return pos + 42u;
}

static void input_reply(uint8_t type)
{
    size_t size = reply(type, false);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
}

static void repair_checksums(size_t size)
{
    uint8_t *ip = frame + 14u, *udp = frame + 34u;
    ip[10] = ip[11] = udp[6] = udp[7] = 0u;
    uint16_t check = book8_htons(book8_net_checksum(ip, 20u));
    memcpy(ip + 10u, &check, 2u);
    uint8_t pseudo[12 + 400] = {0};
    memcpy(pseudo, ip + 12u, 8u);
    pseudo[9] = 17u; memcpy(pseudo + 10u, udp + 4u, 2u);
    memcpy(pseudo + 12u, udp, size - 34u);
    check = book8_net_checksum(pseudo, size - 22u);
    check = book8_htons(check == 0u ? 0xffffu : check);
    memcpy(udp + 6u, &check, 2u);
}

static void test_static_timeout_fallback(void)
{
    reset();
    assert(book8_dhcp_set_fallback(&dhcp, address, 0xffffff00u, server) == BOOK8_NET_OK);
    assert(net.ip == 0u); /* Old static settings are never applied at start. */
    assert(book8_dhcp_poll(&dhcp, 29999u) == BOOK8_NET_OK && net.ip == 0u);
    assert(book8_dhcp_poll(&dhcp, 30000u) == BOOK8_NET_OK);
    assert(dhcp.fallback_active && net.ip == address && net.mask == 0xffffff00u
        && net.gateway == server && dhcp.generation == 1u);
    assert(book8_dhcp_poll(&dhcp, dhcp.next_send) == BOOK8_NET_OK);
    assert(get32(tx + 26u) == 0u); /* Discovery remains 0.0.0.0 despite fallback. */
    const uint32_t assigned = 0xc0a833d0u, mask = 0xfffffe00u, gateway = server + 1u;
    size_t size = reply(2u, true);
    put32(frame + 30u, assigned); put32(frame + 42u + 16u, assigned);
    repair_checksums(size);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
    assert(book8_dhcp_poll(&dhcp, 32000u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_REQUESTING && dhcp.fallback_active);
    assert(net.ip == address && get32(tx + 26u) == 0u);
    size = reply(5u, true);
    put32(frame + 30u, assigned); put32(frame + 42u + 16u, assigned);
    put32(frame + 42u + 251u, mask); put32(frame + 42u + 257u, gateway);
    repair_checksums(size);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
    assert(net.ip == address); /* ACK applies outside RX. */
    assert(book8_dhcp_poll(&dhcp, 32001u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_BOUND && !dhcp.fallback_active);
    assert(net.ip == assigned && net.mask == mask && net.gateway == gateway);
    assert(dhcp.generation == 2u);
    reset();
    assert(book8_dhcp_set_fallback(&dhcp, address, 0xffffff00u, server) == BOOK8_NET_OK);
    bind_lease();
    assert(!dhcp.fallback_active && dhcp.state == BOOK8_DHCP_BOUND);
}

static void test_fallback_deadline_survives_discovery_restart(void)
{
    reset();
    assert(book8_dhcp_set_fallback(&dhcp, address, 0xffffff00u, server) == BOOK8_NET_OK);
    assert(book8_dhcp_poll(&dhcp, 0u) == BOOK8_NET_OK);
    input_reply(2u);
    assert(book8_dhcp_poll(&dhcp, 100u) == BOOK8_NET_OK);
    input_reply(6u);
    assert(book8_dhcp_poll(&dhcp, 200u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_SELECTING && net.ip == 0u);
    assert(book8_dhcp_poll(&dhcp, 29999u) == BOOK8_NET_OK && net.ip == 0u);
    assert(book8_dhcp_poll(&dhcp, 30000u) == BOOK8_NET_OK);
    assert(dhcp.fallback_active && net.ip == address);

    reset();
    assert(book8_dhcp_set_fallback(&dhcp, address, 0xffffff00u, server) == BOOK8_NET_OK);
    bind_lease();
    assert(book8_dhcp_poll(&dhcp, 9001u) == BOOK8_NET_OK && net.ip == 0u);
    input_reply(2u);
    assert(book8_dhcp_poll(&dhcp, 9100u) == BOOK8_NET_OK);
    input_reply(6u);
    assert(book8_dhcp_poll(&dhcp, 9200u) == BOOK8_NET_OK && net.ip == 0u);
    assert(book8_dhcp_poll(&dhcp, 39000u) == BOOK8_NET_OK && net.ip == 0u);
    assert(book8_dhcp_poll(&dhcp, 39001u) == BOOK8_NET_OK);
    assert(dhcp.fallback_active && net.ip == address);
}

static void test_lease_precedes_fallback(void)
{
    const uint32_t assigned = 0xc0a833d0u;
    const uint32_t mask = 0xfffffe00u;
    const uint32_t gateway = server + 1u;
    size_t size;

    reset();
    assert(book8_dhcp_set_fallback(&dhcp, address, 0xffffff00u, server) == BOOK8_NET_OK);
    assert(book8_dhcp_poll(&dhcp, 0u) == BOOK8_NET_OK);
    assert(net.ip == 0u && get32(tx + 26u) == 0u);
    size = reply(2u, false);
    put32(frame + 42u + 16u, assigned);
    repair_checksums(size);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
    assert(book8_dhcp_poll(&dhcp, 100u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_REQUESTING && net.ip == 0u);
    size = reply(5u, false);
    put32(frame + 42u + 16u, assigned);
    put32(frame + 42u + 251u, mask);
    put32(frame + 42u + 257u, gateway);
    put32(frame + 42u + 269u, UINT32_MAX);
    repair_checksums(size);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
    assert(book8_dhcp_poll(&dhcp, 101u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_BOUND && !dhcp.fallback_active);
    assert(net.ip == assigned && net.mask == mask && net.gateway == gateway);
    assert(book8_dhcp_poll(&dhcp, 30000u) == BOOK8_NET_OK);
    assert(!dhcp.fallback_active && net.ip == assigned
        && net.mask == mask && net.gateway == gateway);
}

static void bind_lease(void)
{
    assert(book8_dhcp_poll(&dhcp, 0u) == BOOK8_NET_OK);
    assert(sends == 1u && tx_size >= 342u);
    assert(get32(tx + 14u + 12u) == 0u && get32(tx + 14u + 16u) == UINT32_MAX);
    assert(book8_net_checksum(tx + 14u, 20u) == 0u);
    uint8_t pseudo[12 + 308] = {0};
    memcpy(pseudo, tx + 26u, 8u);
    pseudo[9] = 17u; memcpy(pseudo + 10u, tx + 38u, 2u);
    memcpy(pseudo + 12u, tx + 34u, tx_size - 34u);
    assert(book8_net_checksum(pseudo, tx_size - 22u) == 0u);
    assert(tx[42u + 242u] == 1u);
    assert(book8_dhcp_poll(&dhcp, 999u) == BOOK8_NET_OK && sends == 1u);
    input_reply(2u);
    assert(net.ip == 0u && dhcp.pending == 2u);
    assert(book8_dhcp_poll(&dhcp, 1000u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_REQUESTING && sends == 2u);
    assert(tx[42u + 242u] == 3u);
    input_reply(5u);
    assert(net.ip == 0u); /* RX callback never configures/re-enters transport. */
    assert(book8_dhcp_poll(&dhcp, 1001u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_BOUND && net.ip == address && net.mask == 0xffffff00u);
    assert(net.gateway == server && dhcp.dns == 0x08080808u);
    assert(dhcp.generation == 1u && dhcp.lease_seconds == 8u);
}

static void test_options_and_timeouts(void)
{
    reset();
    assert(book8_dhcp_poll(&dhcp, 0u) == BOOK8_NET_OK);
    input_reply(2u);
    assert(book8_dhcp_poll(&dhcp, 1u) == BOOK8_NET_OK);
    size_t size = reply(5u, false);
    frame[40u] = frame[41u] = 0u;
    frame[42u + 267u] = 99u; /* Missing lease option. */
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    size = reply(5u, false); frame[40u] = frame[41u] = 0u;
    frame[42u + 249u] = 54u; /* Duplicate server option. */
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    size = reply(5u, false); frame[40u] = frame[41u] = 0u;
    put32(frame + 42u + 245u, server + 1u);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    size = reply(5u, false); frame[40u] = frame[41u] = 0u;
    put32(frame + 42u + 269u, 3u); /* Too-short lease. */
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    size = reply(5u, false); frame[40u] = frame[41u] = 0u;
    uint8_t *p = frame + 42u;
    memset(p + 249u, 0, 6u); p[249u] = 52u; p[250u] = 1u; p[251u] = 1u;
    p[267u] = 99u;
    size_t pos = opt32(p + 108u, 1u, 0xffffff00u);
    pos += opt32(p + 108u + pos, 51u, UINT32_MAX);
    p[108u + pos] = 255u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 5u);
    assert(book8_dhcp_poll(&dhcp, 2u) == BOOK8_NET_OK);
    assert(dhcp.expires == UINT64_MAX && dhcp.renew_at == UINT64_MAX);
    assert(book8_dhcp_poll(&dhcp, 999999999u) == BOOK8_NET_OK && net.ip == address);
    net.receiving = true;
    assert(book8_dhcp_stop(&dhcp) == BOOK8_NET_BUSY);
    assert(book8_dhcp_poll(&dhcp, 999999999u) == BOOK8_NET_BUSY);
    net.receiving = false;
    reset();
    assert(book8_dhcp_poll(&dhcp, 0u) == BOOK8_NET_OK);
    input_reply(2u);
    assert(book8_dhcp_poll(&dhcp, 1u) == BOOK8_NET_OK);
    for (unsigned attempt = 1u; attempt < 5u; ++attempt)
        assert(book8_dhcp_poll(&dhcp, dhcp.next_send) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_REQUESTING && dhcp.attempts == 5u);
    input_reply(5u); /* Fifth request still accepts its ACK during response window. */
    assert(book8_dhcp_poll(&dhcp, dhcp.next_send - 1u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_BOUND);
    reset(); bind_lease();
    assert(book8_dhcp_renew(&dhcp, 8000u) == BOOK8_NET_OK);
    input_reply(5u);
    assert(book8_dhcp_poll(&dhcp, 9001u) == BOOK8_NET_OK);
    assert(net.ip == 0u && dhcp.state == BOOK8_DHCP_SELECTING);
}

static void test_bootstrap_filter(void)
{
    reset();
    size_t size = reply(2u, false);
    frame[35u] = 66u; frame[40u] = frame[41u] = 0u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_IGNORED);
    size = reply(2u, false);
    frame[37u] = 69u; frame[40u] = frame[41u] = 0u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_IGNORED);
    size = reply(2u, false);
    assert(book8_net_input(&net, frame, size - 1u) == BOOK8_NET_INVALID);
    size = reply(2u, false);
    frame[20u] = 0x20u; frame[24u] = frame[25u] = 0u;
    uint16_t check = book8_htons(book8_net_checksum(frame + 14u, 20u));
    memcpy(frame + 24u, &check, 2u);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_INVALID);
    assert(dhcp.pending == 0u);
}

static void test_lifecycle(void)
{
    reset(); bind_lease();
    assert(book8_dhcp_poll(&dhcp, 5000u) == BOOK8_NET_OK && sends == 2u);
    assert(book8_dhcp_poll(&dhcp, 5001u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_RENEWING && net.ip == address);
    assert(book8_dhcp_poll(&dhcp, 8001u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_REBINDING && net.ip == address);
    size_t size = reply(5u, true);
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
    assert(book8_dhcp_poll(&dhcp, 8002u) == BOOK8_NET_OK);
    assert(dhcp.state == BOOK8_DHCP_BOUND && dhcp.generation == 2u);
    assert(book8_dhcp_renew(&dhcp, 8100u) == BOOK8_NET_OK);
    input_reply(6u);
    assert(book8_dhcp_poll(&dhcp, 8101u) == BOOK8_NET_OK);
    assert(net.ip == 0u && dhcp.state == BOOK8_DHCP_SELECTING);
    assert(book8_dhcp_stop(&dhcp) == BOOK8_NET_OK);
    unsigned before = sends;
    assert(book8_dhcp_poll(&dhcp, 99999u) == BOOK8_NET_OK && sends == before);
    reset(); bind_lease();
    assert(book8_dhcp_poll(&dhcp, 9001u) == BOOK8_NET_OK);
    assert(net.ip == 0u && dhcp.generation == 2u);
}

static void test_invalid(void)
{
    reset();
    size_t size = reply(2u, false);
    frame[24u] ^= 1u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_INVALID);
    size = reply(2u, false); frame[42u + 28u] ^= 1u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_INVALID); /* UDP checksum. */
    size = reply(2u, false); frame[40u] = frame[41u] = 0u;
    frame[42u + 28u] ^= 1u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK);
    assert(dhcp.rejected == 1u && dhcp.pending == 0u);
    size = reply(2u, false); frame[40u] = frame[41u] = 0u;
    frame[42u + 4u] ^= 1u;
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    size = reply(2u, false); frame[40u] = frame[41u] = 0u;
    frame[size - 1u] = 0u; /* Missing END. */
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    size = reply(2u, false); frame[40u] = frame[41u] = 0u;
    frame[42u + 244u] = 255u; /* Oversized server option. */
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    input_reply(2u);
    assert(book8_dhcp_poll(&dhcp, 1u) == BOOK8_NET_OK);
    size = reply(5u, false); frame[40u] = frame[41u] = 0u;
    put32(frame + 42u + 251u, 0xff00ff00u); /* Invalid subnet mask. */
    assert(book8_net_input(&net, frame, size) == BOOK8_NET_OK && dhcp.pending == 0u);
    assert(net.ip == 0u);
    send_result = BOOK8_NET_BUSY;
    unsigned before = sends;
    assert(book8_dhcp_poll(&dhcp, 1001u) == BOOK8_NET_BUSY);
    assert(sends == before + 1u);
    assert(book8_dhcp_poll(&dhcp, 1002u) == BOOK8_NET_OK && sends == before + 1u);
}

static void test_settings(void)
{
    struct book8_network_settings settings = {.mode = BOOK8_NETWORK_DHCP,
        .ip = address, .mask = 0xffffff00u, .gateway = server,
        .telemetry = 0xc0a83211u, .retry_ms = 1000u}, restored;
    uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
    assert(book8_network_settings_encode(&settings, bytes));
    assert(book8_network_settings_decode(&restored, bytes, sizeof(bytes)));
    assert(restored.mode == settings.mode && restored.ip == address);
    bytes[12] ^= 1u;
    assert(!book8_network_settings_decode(&restored, bytes, sizeof(bytes)));
    settings.mode = BOOK8_NETWORK_STATIC; settings.ip = 0u;
    assert(!book8_network_settings_valid(&settings));
    settings.mode = BOOK8_NETWORK_DHCP; settings.mask = settings.gateway = 0u;
    assert(book8_network_settings_valid(&settings));
}

int main(void)
{
    test_lifecycle();
    test_invalid();
    test_settings();
    test_options_and_timeouts();
    test_bootstrap_filter();
    test_static_timeout_fallback();
    test_fallback_deadline_survives_discovery_restart();
    test_lease_precedes_fallback();
    puts("PASS: DHCP DORA, lease priority, retries, renewal, persistence and 30s fallback");
    return 0;
}
