#include "system_config.h"

#define CONFIG_DEFAULTS \
    48u, 1u, 25u, 1u, 100u, 400u, 0u, 1u, 0u, 0u, 0u, 1u, 75u, 0u, 880u

static struct book8_system_config current = {{CONFIG_DEFAULTS}};

const struct book8_config_range book8_config_ranges[BOOK8_CFG_COUNT] = {
    {0u, 104u, 1u, 48u}, {0u, 1u, 1u, 1u},
    {1u, 30u, 1u, 25u}, {0u, 3u, 1u, 1u},
    {25u, 300u, 5u, 100u}, {100u, 1000u, 50u, 400u},
    {0u, 1u, 1u, 0u}, {0u, 2u, 1u, 1u},
    {0u, 1u, 1u, 0u}, {0u, 7u, 1u, 0u}, {0u, 1u, 1u, 0u},
    {0u, 1u, 1u, 1u}, {0u, 100u, 5u, 75u}, {0u, 1u, 1u, 0u},
    {100u, 2000u, 20u, 880u}
};

const uint32_t book8_accent_colors[8] = {
    0x0078d7u, 0x0099bcu, 0x00b294u, 0x107c10u,
    0xff8c00u, 0xe81123u, 0xb4009eu, 0x744da9u
};

const struct book8_system_config *book8_config_get(void)
{
    return &current;
}

void book8_config_defaults(struct book8_system_config *config)
{
    for (uint32_t i = 0u; i < BOOK8_CFG_COUNT; ++i) {
        config->values[i] = book8_config_ranges[i].initial;
    }
}

bool book8_config_valid(const struct book8_system_config *config)
{
    if (config == 0) {
        return false;
    }
    for (uint32_t i = 0u; i < BOOK8_CFG_COUNT; ++i) {
        if (config->values[i] < book8_config_ranges[i].min
            || config->values[i] > book8_config_ranges[i].max) {
            return false;
        }
    }
    return true;
}

bool book8_config_commit(const struct book8_system_config *config)
{
    if (!book8_config_valid(config)) {
        return false;
    }
    current = *config;
    return true;
}

static void put32(uint8_t *p, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(value >> (i * 8u));
    }
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t value = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        value |= (uint32_t)p[i] << (i * 8u);
    }
    return value;
}

static uint32_t checksum(const uint8_t *p, uint32_t size)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0u; i < size; ++i) {
        hash = (hash ^ p[i]) * 16777619u;
    }
    return hash;
}

bool book8_config_encode(const struct book8_system_config *config,
    uint8_t bytes[BOOK8_CONFIG_BYTES])
{
    if (bytes == 0 || !book8_config_valid(config)) {
        return false;
    }
    put32(bytes, 0x384b4f42u);
    put32(bytes + 4u, 1u);
    put32(bytes + 8u, BOOK8_CFG_COUNT);
    for (uint32_t i = 0u; i < BOOK8_CFG_COUNT; ++i) {
        put32(bytes + 16u + i * 4u, config->values[i]);
    }
    put32(bytes + 12u, checksum(bytes + 16u, BOOK8_CFG_COUNT * 4u));
    return true;
}

bool book8_config_decode(struct book8_system_config *config,
    const uint8_t *bytes, uint32_t size)
{
    if (config == 0 || bytes == 0 || size != BOOK8_CONFIG_BYTES
        || get32(bytes) != 0x384b4f42u || get32(bytes + 4u) != 1u
        || get32(bytes + 8u) != BOOK8_CFG_COUNT
        || get32(bytes + 12u)
            != checksum(bytes + 16u, BOOK8_CFG_COUNT * 4u)) {
        return false;
    }
    struct book8_system_config decoded;
    for (uint32_t i = 0u; i < BOOK8_CFG_COUNT; ++i) {
        decoded.values[i] = get32(bytes + 16u + i * 4u);
    }
    if (!book8_config_valid(&decoded)) {
        return false;
    }
    *config = decoded;
    return true;
}

uint32_t book8_config_value(enum book8_config_key key)
{
    return current.values[key];
}

uint8_t book8_pointer_buttons(uint8_t buttons)
{
    return book8_config_value(BOOK8_CFG_SWAP_BUTTONS) != 0u
        ? (uint8_t)((buttons & ~3u) | ((buttons & 1u) << 1u)
            | ((buttons & 2u) >> 1u)) : buttons;
}

int32_t book8_pointer_delta(int32_t delta, int32_t *remainder)
{
    int64_t scaled = (int64_t)delta
        * book8_config_value(BOOK8_CFG_POINTER_SPEED) + *remainder;
    if (scaled / 100 > INT32_MAX) { *remainder = 0; return INT32_MAX; }
    if (scaled / 100 < INT32_MIN) { *remainder = 0; return INT32_MIN; }
    *remainder = (int32_t)(scaled % 100);
    return (int32_t)(scaled / 100);
}
