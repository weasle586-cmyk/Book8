#include "../drivers/block.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DISK_SECTORS 2056u
#define SWAP_START 8u
#define SWAP_SECTORS 2048u

static uint8_t disk[DISK_SECTORS * BLOCK_SECTOR_SIZE];

static bool memory_read(block_device_t *dev, uint32_t lba, uint32_t count,
    void *buffer)
{
    (void)dev;
    memcpy(buffer, disk + (uint64_t)lba * BLOCK_SECTOR_SIZE,
        (size_t)count * BLOCK_SECTOR_SIZE);
    return true;
}

static bool memory_write(block_device_t *dev, uint32_t lba, uint32_t count,
    const void *buffer)
{
    (void)dev;
    memcpy(disk + (uint64_t)lba * BLOCK_SECTOR_SIZE, buffer,
        (size_t)count * BLOCK_SECTOR_SIZE);
    return true;
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

int main(void)
{
    uint8_t page[4096];
    uint8_t restored[4096];
    uint32_t first;
    uint32_t second;
    block_device_t *dev = block_register("mock", DISK_SECTORS,
        memory_read, memory_write, NULL);

    assert(dev != NULL);
    swap_init(1024u * 1024u);
    assert(!swap_available() && swap_total_slots() == 0u);

    disk[510] = 0x55u;
    disk[511] = 0xaau;
    disk[446u + 4u] = 0x82u;
    put_le32(disk + 446u + 8u, SWAP_START);
    put_le32(disk + 446u + 12u, SWAP_SECTORS);
    swap_init(2u * 1024u * 1024u);
    assert(!swap_available());
    swap_init(512u * 1024u);
    assert(swap_available());
    assert(swap_total_slots() == SWAP_SECTORS / 8u);
    swap_init(1024u * 1024u);
    assert(swap_available());
    assert(swap_total_slots() == SWAP_SECTORS / 8u);
    assert(swap_used_slots() == 0u);

    for (uint32_t i = 0u; i < sizeof(page); ++i)
        page[i] = (uint8_t)(i * 13u + 7u);
    assert(swap_alloc_slot(&first));
    assert(swap_alloc_slot(&second) && first != second);
    assert(swap_used_slots() == 2u);
    assert(swap_write_page(first, page));
    memset(restored, 0, sizeof(restored));
    assert(swap_read_page(first, restored));
    assert(memcmp(page, restored, sizeof(page)) == 0);
    assert(memcmp(page, disk + (uint64_t)SWAP_START * BLOCK_SECTOR_SIZE,
        sizeof(page)) == 0);
    swap_free_slot(first);
    swap_free_slot(first);
    assert(swap_used_slots() == 1u);
    assert(!swap_read_page(first, restored));
    swap_free_slot(second);
    assert(swap_used_slots() == 0u);
    puts("PASS: raw MBR swap discovery, bitmap allocation and 8-sector I/O");
    return 0;
}
