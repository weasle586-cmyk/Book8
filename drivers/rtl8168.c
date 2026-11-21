#include "rtl8168.h"
#include "../kernel/sched.h"

#define PCI_COMMAND 0x04u
#define FOUR_GIB 0x100000000ull
#define RTL8168_IMR 0x3cu
#define RTL8168_ISR 0x3eu
#define RTL8168_TDSAR 0x20u
#define RTL8168_TX_POLL 0x38u
#define RTL8168_TX_CONFIG 0x40u
#define RTL8168_RX_CONFIG 0x44u
#define RTL8168_RX_MAX 0xdau
#define RTL8168_CPLUS 0xe0u
#define RTL8168_RDSAR 0xe4u
#define RTL8168_TX_MAX 0xecu
#define RTL8168_TX_RX_ENABLE 0x0cu

_Static_assert(sizeof(struct rtl8168_descriptor) == 16u, "RTL descriptor size");
_Static_assert(offsetof(struct rtl8168_descriptor, address) == 8u,
    "RTL descriptor address offset");
_Static_assert(offsetof(struct rtl8168_dma, tx) % RTL8168_DMA_ALIGNMENT == 0u,
    "TX ring alignment");

static void dma_barrier(void)
{
    __sync_synchronize();
}

static void write32(volatile uint8_t *regs, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(regs + offset) = value;
}

static void write_address(volatile uint8_t *regs, uint32_t offset, uint64_t phys)
{
    write32(regs, offset, (uint32_t)phys);
    write32(regs, offset + 4u, (uint32_t)(phys >> 32u));
}

static uint32_t config_read(const struct rtl8168_platform *p,
    const struct rtl8168_device *d, uint8_t offset)
{
    return p->pci_read32(p->ctx, d->bus, d->slot, d->function, offset);
}

static void config_write(const struct rtl8168_platform *p,
    const struct rtl8168_device *d, uint8_t offset, uint32_t value)
{
    p->pci_write32(p->ctx, d->bus, d->slot, d->function, offset, value);
}

static enum rtl8168_status read_bars(const struct rtl8168_platform *p,
    struct rtl8168_device *d)
{
    uint32_t command = config_read(p, d, PCI_COMMAND) & 0xffffu;
    enum rtl8168_status result = RTL8168_OK;

    /* Disable decoding and DMA while sizing; never write PCI status W1C bits. */
    config_write(p, d, PCI_COMMAND, command & ~7u);
    if ((config_read(p, d, PCI_COMMAND) & 7u) != 0u) {
        config_write(p, d, PCI_COMMAND, command);
        return RTL8168_PCI_DISABLED;
    }
    for (uint8_t i = 0u; i < 6u; ++i) {
        uint8_t offset = (uint8_t)(0x10u + i * 4u);
        uint32_t low = config_read(p, d, offset);
        bool io = (low & 1u) != 0u;
        uint32_t type = (low >> 1u) & 3u;
        bool wide = !io && type == 2u;
        if (!io && (type == 1u || type == 3u || (wide && i == 5u))) {
            result = RTL8168_BAD_BAR;
            break;
        }
        uint32_t high = wide ? config_read(p, d, offset + 4u) : 0u;
        config_write(p, d, offset, 0xffffffffu);
        if (wide) {
            config_write(p, d, offset + 4u, 0xffffffffu);
        }
        uint32_t mask_low = config_read(p, d, offset);
        uint32_t mask_high = wide ? config_read(p, d, offset + 4u) : 0u;
        config_write(p, d, offset, low);
        if (wide) {
            config_write(p, d, offset + 4u, high);
        }
        uint32_t address_mask = io ? ~3u : ~15u;
        uint64_t mask = ((uint64_t)mask_high << 32u)
            | (mask_low & address_mask);
        if (mask != 0u) {
            uint64_t size = io
                ? (uint64_t)(uint16_t)(~(uint16_t)mask + 1u)
                : wide ? ~mask + 1u
                : (uint64_t)(uint32_t)(~(uint32_t)mask + 1u);
            uint64_t phys = ((uint64_t)high << 32u) | (low & address_mask);
            if (phys == 0u || size == 0u || (size & (size - 1u)) != 0u
                || (phys & (size - 1u)) != 0u
                || (io && (size > 0x10000u || phys > 0x10000u - size))) {
                result = RTL8168_BAD_BAR;
                break;
            }
            d->bars[i] = (struct rtl8168_bar){
                .phys = phys, .size = size, .io = io
            };
            if (io && d->io_base == 0u) {
                d->io_base = (uint16_t)phys;
            }
        }
        if (wide) {
            ++i;
        }
    }
    config_write(p, d, PCI_COMMAND, command);
    return result;
}

static enum rtl8168_status rtl8168_probe_unlocked(const struct rtl8168_platform *p,
    struct rtl8168_device *d)
{
    *d = (struct rtl8168_device){0};
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint8_t slot = 0u; slot < 32u; ++slot) {
            uint8_t count = 1u;
            for (uint8_t fn = 0u; fn < count; ++fn) {
                uint32_t id = p->pci_read32(p->ctx, (uint8_t)bus, slot, fn, 0u);
                if ((uint16_t)id == 0xffffu || (uint16_t)id == 0u) {
                    continue;
                }
                uint32_t header = p->pci_read32(p->ctx, (uint8_t)bus, slot,
                    fn, 0x0cu);
                if (fn == 0u && (header & 0x800000u) != 0u) {
                    count = 8u;
                }
                if ((uint16_t)id != RTL8168_VENDOR_ID
                    || (uint16_t)(id >> 16u) != RTL8168_DEVICE_ID
                    || (header & 0x7f0000u) != 0u
                    || (p->pci_read32(p->ctx, (uint8_t)bus, slot, fn, 8u)
                        >> 16u) != 0x0200u) {
                    continue;
                }
                d->bus = (uint8_t)bus;
                d->slot = slot;
                d->function = fn;
                d->present = true;
                return read_bars(p, d);
            }
        }
    }
    return RTL8168_ABSENT;
}

static enum rtl8168_status rtl8168_probe_device_unlocked(
    const struct rtl8168_platform *p, struct rtl8168_device *d,
    uint8_t bus, uint8_t slot, uint8_t function)
{
    *d = (struct rtl8168_device){0};
    uint32_t id = p->pci_read32(p->ctx, bus, slot, function, 0u);
    uint32_t header = p->pci_read32(p->ctx, bus, slot, function, 0x0cu);
    uint32_t class_revision = p->pci_read32(p->ctx, bus, slot, function, 8u);
    if ((uint16_t)id != RTL8168_VENDOR_ID
        || (uint16_t)(id >> 16u) != RTL8168_DEVICE_ID
        || (header & 0x7f0000u) != 0u
        || (class_revision >> 16u) != 0x0200u)
        return RTL8168_ABSENT;
    d->bus = bus;
    d->slot = slot;
    d->function = function;
    d->present = true;
    return read_bars(p, d);
}

static enum rtl8168_status rtl8168_init_unlocked(const struct rtl8168_platform *p,
    struct rtl8168_device *d)
{
    if (!d->present) {
        return RTL8168_ABSENT;
    }
    if (d->running) {
        return RTL8168_BAD_STATE;
    }
    d->initialized = false;
    d->regs = 0;
    for (uint8_t i = 0u; i < 6u; ++i) {
        struct rtl8168_bar *bar = &d->bars[i];
        if (bar->size == 0u || bar->io) {
            continue;
        }
        if (bar->phys >= FOUR_GIB || bar->size > FOUR_GIB - bar->phys) {
            return RTL8168_MAP_FAILED;
        }
        bar->mapped = p->map_mmio(p->ctx, bar->phys, bar->size, true);
        if (bar->mapped == 0) {
            return RTL8168_MAP_FAILED;
        }
        if (d->regs == 0 && bar->size >= 0x100u) {
            d->regs = bar->mapped;
        }
    }
    if (d->regs == 0) {
        return RTL8168_BAD_BAR;
    }
    /* Keep bus mastering off until rings are installed; use masked INTx. */
    uint32_t command = config_read(p, d, PCI_COMMAND) & 0xffffu;
    uint32_t enabled = (command | 2u | 0x400u | (d->io_base ? 1u : 0u)) & ~4u;
    config_write(p, d, PCI_COMMAND, enabled);
    if ((config_read(p, d, PCI_COMMAND) & 0x407u) != (enabled & 0x407u)) {
        return RTL8168_PCI_DISABLED;
    }
    *(volatile uint16_t *)(d->regs + RTL8168_IMR) = 0u;
    d->regs[RTL8168_CR] = RTL8168_RESET;
    uint32_t polls = 0u;
    while ((d->regs[RTL8168_CR] & RTL8168_RESET) != 0u
        && polls < RTL8168_RESET_POLLS) {
        ++polls;
        p->relax(p->ctx);
    }
    if ((d->regs[RTL8168_CR] & RTL8168_RESET) != 0u) {
        return RTL8168_RESET_TIMEOUT;
    }
    *(volatile uint16_t *)(d->regs + RTL8168_IMR) = 0u;
    *(volatile uint16_t *)(d->regs + RTL8168_ISR) = 0xffffu;
    d->regs[RTL8168_CFG9346] = 0xc0u;
    (void)d->regs[RTL8168_CFG9346]; /* Flush the posted unlock before MAC reads. */
    uint8_t any = 0u, all = 0xffu;
    for (uint8_t i = 0u; i < 6u; ++i) {
        d->mac[i] = d->regs[i];
        any |= d->mac[i];
        all &= d->mac[i];
    }
    /* No configuration changes follow; do not leave protected registers open. */
    d->regs[RTL8168_CFG9346] = 0u;
    (void)d->regs[RTL8168_CFG9346];
    if (any == 0u || all == 0xffu || (d->mac[0] & 1u) != 0u) {
        return RTL8168_BAD_MAC;
    }
    d->initialized = true;
    return RTL8168_OK;
}

static enum rtl8168_status rtl8168_start_unlocked(const struct rtl8168_platform *p,
    struct rtl8168_device *d, struct rtl8168_dma *dma)
{
    if (!d->initialized || d->regs == 0 || d->running) {
        return RTL8168_BAD_STATE;
    }
    uint64_t phys;
    if (dma == 0 || (uintptr_t)dma % RTL8168_DMA_ALIGNMENT != 0u
        || p->dma_address == 0
        || !p->dma_address(p->ctx, dma, sizeof(*dma), &phys)
        || phys == 0u || phys % RTL8168_DMA_ALIGNMENT != 0u
        || phys > UINT64_MAX - (sizeof(*dma) - 1u)) {
        return RTL8168_BAD_DMA;
    }
    if ((d->regs[RTL8168_CR] & (RTL8168_TX_RX_ENABLE | RTL8168_RESET)) != 0u) {
        return RTL8168_BAD_STATE;
    }
    uint32_t command = config_read(p, d, PCI_COMMAND) & 0xffffu;
    if ((command & 0x406u) != 0x402u) {
        return RTL8168_PCI_DISABLED;
    }

    uint8_t *bytes = (uint8_t *)dma;
    for (size_t i = 0u; i < sizeof(*dma); ++i) {
        bytes[i] = 0u;
    }
    for (uint32_t i = 0u; i < RTL8168_RING_COUNT; ++i) {
        uint32_t end = i == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u;
        dma->rx[i].address = phys + offsetof(struct rtl8168_dma, rx_buffers)
            + i * RTL8168_BUFFER_SIZE;
        dma->tx[i].address = phys + offsetof(struct rtl8168_dma, tx_buffers)
            + i * RTL8168_BUFFER_SIZE;
        dma->tx[i].opts1 = end;
        dma->rx[i].opts1 = RTL8168_OWN | end | RTL8168_BUFFER_SIZE;
    }
    dma_barrier();
    volatile uint8_t *regs = d->regs;
    regs[RTL8168_CFG9346] = 0xc0u;
    (void)regs[RTL8168_CFG9346];
    /* Legacy 16-byte descriptors, no checksum/VLAN offloads or interrupts. */
    *(volatile uint16_t *)(regs + RTL8168_CPLUS) = 0u;
    *(volatile uint16_t *)(regs + RTL8168_IMR) = 0u;
    *(volatile uint16_t *)(regs + RTL8168_ISR) = 0xffffu;
    write_address(regs, RTL8168_RDSAR, phys + offsetof(struct rtl8168_dma, rx));
    write_address(regs, RTL8168_TDSAR, phys + offsetof(struct rtl8168_dma, tx));
    *(volatile uint16_t *)(regs + RTL8168_RX_MAX) = RTL8168_BUFFER_SIZE;
    regs[RTL8168_TX_MAX] = 0x3fu;
    write32(regs, RTL8168_TX_CONFIG, (3u << 24u) | (7u << 8u));
    /* Unlimited DMA burst/FIFO threshold; accept own unicast and broadcasts. */
    write32(regs, RTL8168_RX_CONFIG, (7u << 13u) | (7u << 8u) | 0x0au);
    regs[RTL8168_CFG9346] = 0u;
    (void)regs[RTL8168_CFG9346];

    config_write(p, d, PCI_COMMAND, command | 4u);
    if ((config_read(p, d, PCI_COMMAND) & 0x406u) != 0x406u) {
        config_write(p, d, PCI_COMMAND, command);
        return RTL8168_PCI_DISABLED;
    }
    dma_barrier();
    regs[RTL8168_CR] = RTL8168_TX_RX_ENABLE;
    if ((regs[RTL8168_CR] & RTL8168_TX_RX_ENABLE) != RTL8168_TX_RX_ENABLE) {
        regs[RTL8168_CR] = 0u;
        config_write(p, d, PCI_COMMAND, command);
        return RTL8168_ENABLE_FAILED;
    }
    d->dma = dma;
    d->tx_next = d->rx_next = 0u;
    d->tx_packets = d->rx_packets = d->tx_timeouts = d->rx_dropped = 0u;
    d->tx_dropped = 0u;
    d->running = true;
    return RTL8168_OK;
}

static enum rtl8168_status rtl8168_transmit_unlocked(const struct rtl8168_platform *p,
    struct rtl8168_device *d, const void *packet, size_t length)
{
    (void)p;
    if (!d->running || d->dma == 0 || d->regs == 0) {
        return RTL8168_BAD_STATE;
    }
    if (packet == 0 || length < 14u || length > RTL8168_FRAME_MAX) {
        ++d->tx_dropped;
        return RTL8168_BAD_PACKET;
    }
    uint8_t index = d->tx_next;
    struct rtl8168_descriptor *desc = &d->dma->tx[index];
    if ((desc->opts1 & RTL8168_OWN) != 0u) {
        ++d->tx_dropped;
        return RTL8168_TX_BUSY;
    }
    dma_barrier();
    const uint8_t *src = packet;
    uint8_t *buffer = d->dma->tx_buffers[index];
    size_t wire_length = length < 60u ? 60u : length;
    for (size_t i = 0u; i < wire_length; ++i) {
        buffer[i] = i < length ? src[i] : 0u;
    }
    desc->opts2 = 0u;
    uint32_t end = index == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u;
    dma_barrier();
    desc->opts1 = RTL8168_OWN | end | RTL8168_FS | RTL8168_LS
        | (uint32_t)wire_length;
    dma_barrier();
    d->tx_next = (uint8_t)((index + 1u) % RTL8168_RING_COUNT);
    d->regs[RTL8168_TX_POLL] = 0x40u; /* Normal-priority queue doorbell. */
    (void)d->regs[RTL8168_CR]; /* Flush posted MMIO without reading TxPoll. */
    ++d->tx_packets;
    return RTL8168_OK;
}

static enum rtl8168_status rtl8168_receive_unlocked(struct rtl8168_device *d,
    void *packet, size_t capacity, size_t *length)
{
    if (length == 0 || packet == 0 || capacity == 0u) {
        return RTL8168_BAD_PACKET;
    }
    *length = 0u;
    if (!d->running || d->dma == 0) {
        return RTL8168_BAD_STATE;
    }
    uint8_t index = d->rx_next;
    struct rtl8168_descriptor *desc = &d->dma->rx[index];
    uint32_t status = desc->opts1;
    if ((status & RTL8168_OWN) != 0u) {
        return RTL8168_RX_EMPTY;
    }
    dma_barrier();
    size_t received = status & RTL8168_LENGTH_MASK;
    enum rtl8168_status result = RTL8168_OK;
    if ((status & (RTL8168_FS | RTL8168_LS | RTL8168_RX_ERROR))
            != (RTL8168_FS | RTL8168_LS)
        || received < 64u || received > RTL8168_FRAME_MAX + 4u) {
        result = RTL8168_RX_DROPPED;
    } else {
        *length = received - 4u; /* The descriptor length includes the FCS. */
        if (capacity < *length) {
            result = RTL8168_BUFFER_SMALL;
        } else {
            uint8_t *dst = packet;
            for (size_t i = 0u; i < *length; ++i) {
                dst[i] = d->dma->rx_buffers[index][i];
            }
            ++d->rx_packets;
        }
    }
    if (result != RTL8168_OK) {
        ++d->rx_dropped;
    }
    desc->opts2 = 0u;
    uint32_t end = index == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u;
    dma_barrier();
    desc->opts1 = RTL8168_OWN | end | RTL8168_BUFFER_SIZE;
    dma_barrier();
    d->rx_next = (uint8_t)((index + 1u) % RTL8168_RING_COUNT);
    return result;
}

static uint8_t rtl8168_phy_status_unlocked(const struct rtl8168_device *d)
{
    return d != 0 && d->initialized && d->regs != 0 ? d->regs[RTL8168_PHY_STATUS] : 0u;
}

static void rtl8168_ring_state_unlocked(const struct rtl8168_device *d,
    uint32_t *tx_pending, uint32_t *rx_ready)
{
    uint32_t tx = 0u, rx = 0u;
    if (d != 0 && d->running && d->dma != 0) {
        for (unsigned i = 0u; i < RTL8168_RING_COUNT; ++i) {
            tx += (d->dma->tx[i].opts1 & RTL8168_OWN) != 0u ? 1u : 0u;
            rx += (d->dma->rx[i].opts1 & RTL8168_OWN) == 0u ? 1u : 0u;
        }
    }
    if (tx_pending != 0) {
        *tx_pending = tx;
    }
    if (rx_ready != 0) {
        *rx_ready = rx;
    }
}

enum rtl8168_status rtl8168_probe(const struct rtl8168_platform *p,
    struct rtl8168_device *d)
{
    uint64_t flags = sched_irq_save();
    enum rtl8168_status status = rtl8168_probe_unlocked(p, d);
    sched_irq_restore(flags);
    return status;
}

enum rtl8168_status rtl8168_probe_device(
    const struct rtl8168_platform *p, struct rtl8168_device *d,
    uint8_t bus, uint8_t slot, uint8_t function)
{
    uint64_t flags = sched_irq_save();
    enum rtl8168_status status = rtl8168_probe_device_unlocked(p, d,
        bus, slot, function);
    sched_irq_restore(flags);
    return status;
}

enum rtl8168_status rtl8168_init(const struct rtl8168_platform *p,
    struct rtl8168_device *d)
{
    uint64_t flags = sched_irq_save();
    enum rtl8168_status status = rtl8168_init_unlocked(p, d);
    sched_irq_restore(flags);
    return status;
}

enum rtl8168_status rtl8168_start(const struct rtl8168_platform *p,
    struct rtl8168_device *d, struct rtl8168_dma *dma)
{
    uint64_t flags = sched_irq_save();
    enum rtl8168_status status = rtl8168_start_unlocked(p, d, dma);
    sched_irq_restore(flags);
    return status;
}

enum rtl8168_status rtl8168_transmit(const struct rtl8168_platform *p,
    struct rtl8168_device *d, const void *packet, size_t length)
{
    uint64_t flags = sched_irq_save();
    enum rtl8168_status status = rtl8168_transmit_unlocked(p, d, packet, length);
    sched_irq_restore(flags);
    return status;
}

enum rtl8168_status rtl8168_receive(struct rtl8168_device *d,
    void *packet, size_t capacity, size_t *length)
{
    uint64_t flags = sched_irq_save();
    enum rtl8168_status status = rtl8168_receive_unlocked(d, packet, capacity,
        length);
    sched_irq_restore(flags);
    return status;
}

uint8_t rtl8168_phy_status(const struct rtl8168_device *d)
{
    uint64_t flags = sched_irq_save();
    uint8_t status = rtl8168_phy_status_unlocked(d);
    sched_irq_restore(flags);
    return status;
}

void rtl8168_ring_state(const struct rtl8168_device *d,
    uint32_t *tx_pending, uint32_t *rx_ready)
{
    uint64_t flags = sched_irq_save();
    rtl8168_ring_state_unlocked(d, tx_pending, rx_ready);
    sched_irq_restore(flags);
}

const char *rtl8168_status_string(enum rtl8168_status status)
{
    switch (status) {
    case RTL8168_OK: return "reset complete";
    case RTL8168_ABSENT: return "no RTL8168/RTL8111 detected";
    case RTL8168_BAD_BAR: return "invalid or missing register BAR";
    case RTL8168_MAP_FAILED: return "MMIO mapping failed (requires low 4 GiB)";
    case RTL8168_PCI_DISABLED: return "PCI command readback failed";
    case RTL8168_RESET_TIMEOUT: return "software reset timed out";
    case RTL8168_BAD_MAC: return "invalid MAC ID registers";
    case RTL8168_BAD_STATE: return "packet engine not ready or already running";
    case RTL8168_BAD_DMA: return "invalid DMA allocation/address/alignment";
    case RTL8168_BAD_PACKET: return "invalid packet argument or frame length";
    case RTL8168_TX_BUSY: return "TX ring descriptor still owned by hardware";
    case RTL8168_TX_TIMEOUT: return "TX completion timed out (DMA still pending)";
    case RTL8168_RX_EMPTY: return "no received packet";
    case RTL8168_RX_DROPPED: return "RX error, fragmented or oversized frame dropped";
    case RTL8168_BUFFER_SMALL: return "RX output buffer too small (frame dropped)";
    case RTL8168_ENABLE_FAILED: return "TX/RX command readback failed";
    }
    return "unknown driver status";
}
