#ifndef BOOK8_SYSTEM_CONFIG_H
#define BOOK8_SYSTEM_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define BOOK8_CONFIG_PATH "A:\\BOOK8.CFG"
#define BOOK8_GUI_VERSION "BOOK8 GUI 0.3"

enum book8_config_key {
    BOOK8_CFG_TIMEZONE,
    BOOK8_CFG_RTC_SYNC,
    BOOK8_CFG_REPEAT_RATE,
    BOOK8_CFG_KEY_DELAY,
    BOOK8_CFG_POINTER_SPEED,
    BOOK8_CFG_DOUBLE_CLICK,
    BOOK8_CFG_SWAP_BUTTONS,
    BOOK8_CFG_RESOLUTION,
    BOOK8_CFG_PRESENT_MODE,
    BOOK8_CFG_ACCENT,
    BOOK8_CFG_DARK,
    BOOK8_CFG_NETWORK,
    BOOK8_CFG_VOLUME,
    BOOK8_CFG_MUTE,
    BOOK8_CFG_TONE,
    BOOK8_CFG_COUNT
};

struct book8_system_config {
    uint32_t values[BOOK8_CFG_COUNT];
};

struct book8_config_range {
    uint32_t min, max, step, initial;
};

#define BOOK8_CONFIG_BYTES (16u + BOOK8_CFG_COUNT * 4u)

extern const struct book8_config_range book8_config_ranges[BOOK8_CFG_COUNT];
extern const uint32_t book8_accent_colors[8];

const struct book8_system_config *book8_config_get(void);
void book8_config_defaults(struct book8_system_config *config);
bool book8_config_valid(const struct book8_system_config *config);
bool book8_config_commit(const struct book8_system_config *config);
bool book8_config_encode(const struct book8_system_config *config,
    uint8_t bytes[BOOK8_CONFIG_BYTES]);
bool book8_config_decode(struct book8_system_config *config,
    const uint8_t *bytes, uint32_t size);
uint32_t book8_config_value(enum book8_config_key key);
uint8_t book8_pointer_buttons(uint8_t buttons);
int32_t book8_pointer_delta(int32_t delta, int32_t *remainder);

#endif
