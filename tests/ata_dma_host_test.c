/* Host test for ATA LBA28/LBA48 taskfile encoding (links drivers/ata_dma.c;
 * the hardware path is not exercised). */
#include <stdio.h>
#include "../drivers/ata_dma.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main(void)
{
    struct ata_taskfile tf;
    struct ata_dma_request r = {1, false, false, 0x0abcdef1ull, 16, (void *)&r};

    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_OK);
    CHECK(tf.command == 0xc8 && tf.device == (0xe0 | 0x10 | 0x0a));
    CHECK(tf.lba_lo[0] == 0xf1 && tf.lba_lo[1] == 0xde && tf.lba_lo[2] == 0xbc);
    CHECK(tf.count_lo == 16);

    r.write = true;
    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_OK && tf.command == 0xca);

    r.lba = (1ull << 28) - 8; r.sector_count = 16;
    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_ERR_RANGE);

    r.lba48 = true; r.drive = 0; r.write = false; r.lba = 0x0000123456789abcull;
    r.sector_count = 128;
    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_OK);
    CHECK(tf.command == 0x25 && tf.device == 0x40);
    CHECK(tf.lba_lo[0] == 0xbc && tf.lba_lo[1] == 0x9a && tf.lba_lo[2] == 0x78);
    CHECK(tf.lba_hi[0] == 0x56 && tf.lba_hi[1] == 0x34 && tf.lba_hi[2] == 0x12);
    CHECK(tf.count_lo == 128);
    r.write = true;
    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_OK && tf.command == 0x35);

    r.sector_count = 0;
    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_ERR_NO_CHANNEL);
    r.sector_count = 129;
    CHECK(ata_dma_build_taskfile(&r, &tf) == ATA_ERR_NO_CHANNEL);

    puts(failures ? "ata_dma host test FAILED" : "ata_dma host test PASS");
    return failures != 0;
}
