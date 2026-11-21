#ifndef BOOK8_NETWORK_SETTINGS_H
#define BOOK8_NETWORK_SETTINGS_H

#include "net.h"

#define BOOK8_NETWORK_SETTINGS_PATH "A:\\BOOK8DHCP.CFG"
#define BOOK8_NETWORK_SETTINGS_BYTES 40u

enum book8_network_mode { BOOK8_NETWORK_DHCP, BOOK8_NETWORK_STATIC };

struct book8_network_settings {
    enum book8_network_mode mode;
    uint32_t ip, mask, gateway, dns, telemetry;
    uint32_t retry_ms;
};

struct book8_network_status {
    struct book8_network_settings settings;
    uint32_t assigned_ip, assigned_mask, assigned_gateway, dns, lease_seconds;
    const char *state;
    bool ready;
};

bool book8_network_settings_valid(const struct book8_network_settings *);
bool book8_network_settings_encode(const struct book8_network_settings *, uint8_t *);
bool book8_network_settings_decode(struct book8_network_settings *, const uint8_t *, size_t);
bool book8_network_settings_get(struct book8_network_status *);
bool book8_network_settings_apply(const struct book8_network_settings *, bool persist);
bool book8_network_settings_renew(void);

#endif
