#include "ata_dma.h"

#define ATA_DMA_TIMEOUT 4000000u

#define ATA_SR_ERR 0x01u
#define ATA_SR_DRQ 0x08u
#define ATA_SR_DF  0x20u
#define ATA_SR_BSY 0x80u

#define BM_CMD    0u
#define BM_STATUS 2u
#define BM_PRD    4u
#define BM_CMD_START 0x01u
#define BM_CMD_READ  0x08u /* device -> memory */
#define BM_ST_ACTIVE 0x01u
#define BM_ST_ERROR  0x02u
#define BM_ST_IRQ    0x04u

struct prd_entry {
    uint32_t address;
    uint16_t byte_count; /* 0 means 64 KiB */
    uint16_t flags;      /* bit 15 = end of table */
};

static uint8_t bounce[ATA_BOUNCE_BYTES] __attribute__((aligned(65536)));
static struct prd_entry prd_table __attribute__((aligned(16)));

static inline void port_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t port_inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void port_outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static void copy_bytes(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
}

void *ata_bounce_buffer(void)
{
    return bounce;
}

const char *ata_status_name(enum ata_status status)
{
    switch (status) {
    case ATA_OK: return "ok";
    case ATA_ERR_NO_CHANNEL: return "no bus master";
    case ATA_ERR_RANGE: return "LBA out of range";
    case ATA_ERR_BUSY: return "device busy";
    case ATA_ERR_TIMEOUT: return "DMA timeout";
    case ATA_ERR_DEVICE: return "device error";
    case ATA_ERR_BUS_MASTER: return "bus-master error";
    }
    return "unknown";
}

static enum ata_status injected_fault;
static uint32_t injected_count;

void ata_inject_fault(enum ata_status fault, uint32_t count)
{
    injected_fault = fault;
    injected_count = fault == ATA_OK ? 0u : count;
}

enum ata_status ata_fault_take(void)
{
    if (injected_count == 0u) {
        return ATA_OK;
    }
    --injected_count;
    return injected_fault;
}

/* Stops the bus master and clears its latched status so a failed transfer
 * leaves no state behind for the next command. */
static void bus_master_abort(uint16_t bm)
{
    port_outb(bm + BM_CMD, 0u);
    port_outb(bm + BM_STATUS, (uint8_t)(port_inb(bm + BM_STATUS)
        | BM_ST_ERROR | BM_ST_IRQ));
}

enum ata_status ata_dma_build_taskfile(const struct ata_dma_request *request,
    struct ata_taskfile *out)
{
    uint64_t lba = request->lba;
    uint32_t count = request->sector_count;

    if (count == 0u || count > ATA_BOUNCE_SECTORS || request->drive > 1u) {
        return ATA_ERR_NO_CHANNEL;
    }
    out->lba_lo[0] = (uint8_t)lba;
    out->lba_lo[1] = (uint8_t)(lba >> 8u);
    out->lba_lo[2] = (uint8_t)(lba >> 16u);
    out->count_lo = (uint8_t)count;
    out->count_hi = 0u;
    if (request->lba48) {
        if (lba + count > (1ull << 48u)) {
            return ATA_ERR_RANGE;
        }
        out->lba_hi[0] = (uint8_t)(lba >> 24u);
        out->lba_hi[1] = (uint8_t)(lba >> 32u);
        out->lba_hi[2] = (uint8_t)(lba >> 40u);
        out->device = (uint8_t)(0x40u | (request->drive << 4u));
        out->command = request->write ? 0x35u : 0x25u;
    } else {
        if (lba + count > (1ull << 28u)) {
            return ATA_ERR_RANGE;
        }
        out->lba_hi[0] = out->lba_hi[1] = out->lba_hi[2] = 0u;
        out->device = (uint8_t)(0xe0u | (request->drive << 4u)
            | ((lba >> 24u) & 0x0fu));
        out->command = request->write ? 0xcau : 0xc8u;
    }
    return ATA_OK;
}

static void ata_400ns(const struct ata_dma_channel *channel)
{
    for (uint32_t i = 0u; i < 4u; ++i) {
        (void)port_inb(channel->control_base + 2u);
    }
}

static enum ata_status wait_not_busy(const struct ata_dma_channel *channel)
{
    for (uint32_t i = 0u; i < ATA_DMA_TIMEOUT; ++i) {
        uint8_t status = port_inb(channel->command_base + 7u);

        if (status == 0xffu) {
            return ATA_ERR_BUSY;
        }
        if ((status & (ATA_SR_BSY | ATA_SR_DRQ)) == 0u) {
            return ATA_OK;
        }
        __asm__ volatile ("pause");
    }
    return ATA_ERR_BUSY;
}

enum ata_status ata_dma_transfer(const struct ata_dma_channel *channel,
    const struct ata_dma_request *request)
{
    struct ata_taskfile tf;
    enum ata_status result;
    uint16_t bm;
    uint16_t base;
    uint32_t bytes;
    uint8_t status = 0u;
    uint8_t bm_status = 0u;
    bool done = false;

    if (channel == 0 || request == 0 || request->buffer == 0
        || channel->bus_master_base == 0u || channel->command_base == 0u) {
        return ATA_ERR_NO_CHANNEL;
    }
    result = ata_dma_build_taskfile(request, &tf);
    if (result != ATA_OK) {
        return result;
    }
    bytes = request->sector_count * 512u;
    bm = channel->bus_master_base;
    base = channel->command_base;

    result = ata_fault_take();
    if (result != ATA_OK) {
        bus_master_abort(bm);
        return result;
    }

    if (request->write) {
        copy_bytes(bounce, request->buffer, bytes);
    }
    prd_table.address = (uint32_t)(uintptr_t)bounce;
    prd_table.byte_count = (uint16_t)(bytes == 65536u ? 0u : bytes);
    prd_table.flags = 0x8000u;

    port_outb(channel->control_base, 0x02u); /* nIEN: we poll */
    port_outb(bm + BM_CMD, 0u);
    port_outl(bm + BM_PRD, (uint32_t)(uintptr_t)&prd_table);
    port_outb(bm + BM_STATUS, (uint8_t)(port_inb(bm + BM_STATUS)
        | BM_ST_ERROR | BM_ST_IRQ));
    port_outb(bm + BM_CMD, request->write ? 0u : BM_CMD_READ);

    port_outb(base + 6u, (uint8_t)(0xa0u | (request->drive << 4u)));
    ata_400ns(channel);
    result = wait_not_busy(channel);
    if (result != ATA_OK) {
        return result;
    }
    port_outb(base + 6u, tf.device);
    if (request->lba48) {
        port_outb(base + 2u, tf.count_hi);
        port_outb(base + 3u, tf.lba_hi[0]);
        port_outb(base + 4u, tf.lba_hi[1]);
        port_outb(base + 5u, tf.lba_hi[2]);
    }
    port_outb(base + 2u, tf.count_lo);
    port_outb(base + 3u, tf.lba_lo[0]);
    port_outb(base + 4u, tf.lba_lo[1]);
    port_outb(base + 5u, tf.lba_lo[2]);
    port_outb(base + 7u, tf.command);

    port_outb(bm + BM_CMD,
        (uint8_t)((request->write ? 0u : BM_CMD_READ) | BM_CMD_START));

    result = ATA_ERR_TIMEOUT;
    for (uint32_t i = 0u; i < ATA_DMA_TIMEOUT && !done; ++i) {
        bm_status = port_inb(bm + BM_STATUS);
        status = port_inb(base + 7u);
        if ((bm_status & BM_ST_ERROR) != 0u) {
            result = ATA_ERR_BUS_MASTER;
            done = true;
        } else if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0u
            && (status & ATA_SR_BSY) == 0u) {
            result = ATA_ERR_DEVICE;
            done = true;
        } else if ((bm_status & BM_ST_IRQ) != 0u
            && (bm_status & BM_ST_ACTIVE) == 0u
            && (status & ATA_SR_BSY) == 0u) {
            result = ATA_OK;
            done = true;
        } else {
            __asm__ volatile ("pause");
        }
    }

    bus_master_abort(bm);
    if (result == ATA_OK && (port_inb(base + 7u) & (ATA_SR_ERR | ATA_SR_DF))) {
        result = ATA_ERR_DEVICE;
    }
    if (result != ATA_OK && wait_not_busy(channel) != ATA_OK) {
        /* Device is wedged mid-command: soft reset so PIO fallback can run. */
        port_outb(channel->control_base, 0x06u);
        ata_400ns(channel);
        port_outb(channel->control_base, 0x02u);
        ata_400ns(channel);
        (void)wait_not_busy(channel);
    }
    if (result == ATA_OK && !request->write) {
        copy_bytes(request->buffer, bounce, bytes);
    }
    return result;
}
