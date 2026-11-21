#ifndef BOOK8_ATA_DMA_H
#define BOOK8_ATA_DMA_H

#include <stdbool.h>
#include <stdint.h>

/* Bounce buffer shared by IDE bus-master DMA and AHCI multi-sector transfers.
 * It lives in the identity-mapped kernel image (below 4 GiB) and never crosses
 * a 64 KiB boundary, as the PCI IDE bus-master PRD rules require. */
#define ATA_BOUNCE_SECTORS 128u
#define ATA_BOUNCE_BYTES (ATA_BOUNCE_SECTORS * 512u)

enum ata_status {
    ATA_OK = 0,
    ATA_ERR_NO_CHANNEL,   /* no bus-master base or invalid arguments */
    ATA_ERR_RANGE,        /* LBA does not fit the addressing mode */
    ATA_ERR_BUSY,         /* device never became ready */
    ATA_ERR_TIMEOUT,      /* DMA did not complete */
    ATA_ERR_DEVICE,       /* ERR/DF set in the ATA status register */
    ATA_ERR_BUS_MASTER    /* bus-master error bit set */
};

struct ata_dma_channel {
    uint16_t command_base;
    uint16_t control_base;
    uint16_t bus_master_base; /* 0 when the controller has no bus master */
};

struct ata_dma_request {
    uint8_t drive;        /* 0 = master, 1 = slave */
    bool lba48;           /* use READ/WRITE DMA EXT */
    bool write;
    uint64_t lba;
    uint32_t sector_count; /* 1..ATA_BOUNCE_SECTORS */
    void *buffer;
};

void *ata_bounce_buffer(void);
const char *ata_status_name(enum ata_status status);

/* Programs one bus-master DMA transfer and polls it to completion (IRQs are
 * masked at the device with nIEN). Data is staged through the bounce buffer. */
enum ata_status ata_dma_transfer(const struct ata_dma_channel *channel,
    const struct ata_dma_request *request);

/* Test hook: the next `count` hardware transfers (DMA or AHCI) fail with
 * `fault` before any command is issued, after the normal cleanup path runs. */
void ata_inject_fault(enum ata_status fault, uint32_t count);
enum ata_status ata_fault_take(void);

/* LBA28 / LBA48 command and register encoding, exposed for host unit tests. */
struct ata_taskfile {
    uint8_t command;
    uint8_t device;
    uint8_t count_hi, count_lo;
    uint8_t lba_hi[3];   /* LBA bits 24..47 (only used for LBA48) */
    uint8_t lba_lo[3];   /* LBA bits 0..23 */
};

enum ata_status ata_dma_build_taskfile(const struct ata_dma_request *request,
    struct ata_taskfile *out);

#endif
