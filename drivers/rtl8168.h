#ifndef BOOK8_RTL8168_H
#define BOOK8_RTL8168_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTL8168_VENDOR_ID 0x10ecu
#define RTL8168_DEVICE_ID 0x8168u /* RTL8111 uses the same PCI device ID. */
#define RTL8168_CR 0x37u
#define RTL8168_CFG9346 0x50u
#define RTL8168_RESET 0x10u
#define RTL8168_RESET_POLLS 1000000u
#define RTL8168_RING_COUNT 8u
#define RTL8168_DMA_ALIGNMENT 256u
#define RTL8168_BUFFER_SIZE 1536u
#define RTL8168_FRAME_MAX 1514u
#define RTL8168_OWN (1u << 31u)
#define RTL8168_EOR (1u << 30u)
#define RTL8168_FS (1u << 29u)
#define RTL8168_LS (1u << 28u)
#define RTL8168_RX_ERROR (1u << 21u)
#define RTL8168_LENGTH_MASK 0x3fffu
#define RTL8168_PHY_STATUS 0x6cu
#define RTL8168_PHY_FULL_DUPLEX 0x01u
#define RTL8168_PHY_LINK 0x02u
#define RTL8168_PHY_10M 0x04u
#define RTL8168_PHY_100M 0x08u
#define RTL8168_PHY_1000M 0x10u

struct rtl8168_descriptor {
    volatile uint32_t opts1;
    uint32_t opts2;
    uint64_t address;
};

struct rtl8168_dma {
    struct rtl8168_descriptor rx[RTL8168_RING_COUNT]
        __attribute__((aligned(RTL8168_DMA_ALIGNMENT)));
    struct rtl8168_descriptor tx[RTL8168_RING_COUNT]
        __attribute__((aligned(RTL8168_DMA_ALIGNMENT)));
    uint8_t rx_buffers[RTL8168_RING_COUNT][RTL8168_BUFFER_SIZE]
        __attribute__((aligned(RTL8168_DMA_ALIGNMENT)));
    uint8_t tx_buffers[RTL8168_RING_COUNT][RTL8168_BUFFER_SIZE]
        __attribute__((aligned(RTL8168_DMA_ALIGNMENT)));
};

struct rtl8168_platform {
    void *ctx;
    uint32_t (*pci_read32)(void *, uint8_t, uint8_t, uint8_t, uint8_t);
    void (*pci_write32)(void *, uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
    volatile uint8_t *(*map_mmio)(void *, uint64_t, uint64_t, bool);
    void (*relax)(void *);
    /* Translate one persistent, physically contiguous, coherent DMA block. */
    bool (*dma_address)(void *, const void *, size_t, uint64_t *);
};

struct rtl8168_bar {
    uint64_t phys;
    uint64_t size;
    volatile uint8_t *mapped;
    bool io;
};

enum rtl8168_status {
    RTL8168_OK,
    RTL8168_ABSENT,
    RTL8168_BAD_BAR,
    RTL8168_MAP_FAILED,
    RTL8168_PCI_DISABLED,
    RTL8168_RESET_TIMEOUT,
    RTL8168_BAD_MAC,
    RTL8168_BAD_STATE,
    RTL8168_BAD_DMA,
    RTL8168_BAD_PACKET,
    RTL8168_TX_BUSY,
    RTL8168_TX_TIMEOUT,
    RTL8168_RX_EMPTY,
    RTL8168_RX_DROPPED,
    RTL8168_BUFFER_SMALL,
    RTL8168_ENABLE_FAILED
};

struct rtl8168_device {
    uint8_t bus, slot, function;
    struct rtl8168_bar bars[6];
    volatile uint8_t *regs;
    uint16_t io_base;
    uint8_t mac[6];
    bool present;
    bool initialized;
    bool running;
    struct rtl8168_dma *dma;
    uint8_t tx_next, rx_next;
    uint32_t tx_packets, rx_packets, tx_timeouts, rx_dropped, tx_dropped;
};

enum rtl8168_status rtl8168_probe(const struct rtl8168_platform *platform,
    struct rtl8168_device *device);
enum rtl8168_status rtl8168_probe_device(
    const struct rtl8168_platform *platform, struct rtl8168_device *device,
    uint8_t bus, uint8_t slot, uint8_t function);
enum rtl8168_status rtl8168_init(const struct rtl8168_platform *platform,
    struct rtl8168_device *device);
enum rtl8168_status rtl8168_start(const struct rtl8168_platform *platform,
    struct rtl8168_device *device, struct rtl8168_dma *dma);
/* Serialized polling APIs; callers supply Ethernet frames without the FCS. */
/* TX returns after enqueue, not completion/delivery; a full ring returns BUSY.
 * tx_packets counts enqueued frames. Hardware-owned buffers are never reused. */
enum rtl8168_status rtl8168_transmit(const struct rtl8168_platform *platform,
    struct rtl8168_device *device, const void *packet, size_t length);
/* RX_EMPTY is normal. BUFFER_SMALL drops the frame and reports its needed size. */
enum rtl8168_status rtl8168_receive(struct rtl8168_device *device,
    void *packet, size_t capacity, size_t *length);
const char *rtl8168_status_string(enum rtl8168_status status);
/* Diagnostics: raw PHYStatus (0x6C) byte, 0 when the device is not mapped. */
uint8_t rtl8168_phy_status(const struct rtl8168_device *device);
/* TX descriptors still hardware-owned and RX descriptors holding unread frames. */
void rtl8168_ring_state(const struct rtl8168_device *device,
    uint32_t *tx_pending, uint32_t *rx_ready);

#endif
