#ifndef BOOK8_BLOCK_H
#define BOOK8_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512u
#define BLOCK_MAX_DEVICES 16u

struct block_device;

/* Hardware-neutral sector I/O. The device pointer is passed first so one
 * implementation can serve many disks (IDE and AHCI share the kernel adapter). */
typedef bool (*block_read_fn)(struct block_device *dev, uint32_t start_lba,
    uint32_t sector_count, void *buf);
typedef bool (*block_write_fn)(struct block_device *dev, uint32_t start_lba,
    uint32_t sector_count, const void *buf);

typedef struct block_device {
    char name[8];
    uint32_t id;
    uint32_t sector_size;      /* always BLOCK_SECTOR_SIZE today */
    uint64_t sector_count;
    block_read_fn read_blocks;
    block_write_fn write_blocks; /* NULL for read-only devices */
    void *priv;
} block_device_t;

block_device_t *block_register(const char *name, uint64_t sector_count,
    block_read_fn read_blocks, block_write_fn write_blocks, void *priv);
uint32_t block_count(void);
block_device_t *block_get(uint32_t index);

/* Uncached transfers with range checking (bulk data). */
bool block_read(block_device_t *dev, uint32_t start_lba, uint32_t sector_count,
    void *buf);
bool block_write(block_device_t *dev, uint32_t start_lba, uint32_t sector_count,
    const void *buf);

/* Sector cache for metadata (FAT, directories). Write-through; block_write()
 * keeps it coherent. */
bool block_cache_read(block_device_t *dev, uint32_t start_lba,
    uint32_t sector_count, void *buf);
void block_cache_invalidate(block_device_t *dev);
void block_cache_stats(uint32_t *hits, uint32_t *misses);

/* Raw MBR swap partition (type 0x82), accessed without filesystem caching. */
void swap_init(uint64_t physical_memory_bytes);
bool swap_available(void);
uint32_t swap_total_slots(void);
uint32_t swap_used_slots(void);
bool swap_alloc_slot(uint32_t *slot);
void swap_free_slot(uint32_t slot);
bool swap_write_page(uint32_t slot, const void *page);
bool swap_read_page(uint32_t slot, void *page);

#endif
