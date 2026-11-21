#include "network_settings.h"
#include <string.h>

bool book8_network_settings_valid(const struct book8_network_settings *s)
{
    if (s == NULL || (s->mode != BOOK8_NETWORK_DHCP && s->mode != BOOK8_NETWORK_STATIC)
        || s->retry_ms < 1000u || s->retry_ms > 16000u)
        return false;
    struct book8_net net = {0};
    if (book8_net_configure(&net, s->ip, s->mask, s->gateway) != BOOK8_NET_OK
        || (s->mode == BOOK8_NETWORK_STATIC && s->ip == 0u))
        return false;
    uint32_t telemetry_first = s->telemetry >> 24u;
    uint32_t dns_first = s->dns >> 24u;
    return (s->telemetry == 0u || (telemetry_first != 0u
        && telemetry_first != 127u && telemetry_first < 224u))
        && (s->dns == 0u || (dns_first != 0u && dns_first != 127u
            && dns_first < 224u));
}

bool book8_network_settings_encode(const struct book8_network_settings *s, uint8_t *bytes)
{
    if (bytes == NULL || !book8_network_settings_valid(s))
        return false;
    uint32_t words[10] = {0x424b4e32u, 2u, (uint32_t)s->mode, s->ip,
        s->mask, s->gateway, s->dns, s->telemetry, s->retry_ms, 0u};
    for (unsigned i = 0u; i < 9u; ++i)
        words[9] ^= words[i];
    for (unsigned i = 0u; i < 10u; ++i) {
        uint32_t value = book8_htonl(words[i]);
        memcpy(bytes + i * 4u, &value, 4u);
    }
    return true;
}

bool book8_network_settings_decode(struct book8_network_settings *s,
    const uint8_t *bytes, size_t size)
{
    if (s == NULL || bytes == NULL || (size != 36u && size != 40u))
        return false;
    bool version_two = size == 40u;
    uint32_t words[10] = {0}, check = 0u;
    uint32_t count = version_two ? 10u : 9u;
    for (unsigned i = 0u; i < count; ++i) {
        memcpy(&words[i], bytes + i * 4u, 4u);
        words[i] = book8_ntohl(words[i]);
        check ^= words[i];
    }
    if (words[0] != 0x424b4e32u
        || words[1] != (version_two ? 2u : 1u) || check != 0u
        || words[2] > 1u)
        return false;
    struct book8_network_settings parsed = {
        .mode = (enum book8_network_mode)words[2],
        .ip = words[3], .mask = words[4], .gateway = words[5],
        .dns = version_two ? words[6] : 0u,
        .telemetry = words[version_two ? 7u : 6u],
        .retry_ms = words[version_two ? 8u : 7u]
    };
    if (!book8_network_settings_valid(&parsed))
        return false;
    *s = parsed;
    return true;
}
