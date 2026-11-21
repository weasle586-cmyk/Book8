#ifndef BOOK8_RTL8168_MODULE_H
#define BOOK8_RTL8168_MODULE_H

#include "rtl8168.h"

#define B8_RTL8168_IOCTL_START 1u
#define B8_RTL8168_IOCTL_PHY_STATUS 2u
#define B8_RTL8168_IOCTL_RING_STATE 3u
#define B8_RTL8168_IOCTL_STATUS_TEXT 4u

struct b8_rtl8168_ring_state {
    uint32_t tx_pending;
    uint32_t rx_ready;
};

struct b8_rtl8168_status_text {
    uint32_t status;
    const char *text;
};

#endif
