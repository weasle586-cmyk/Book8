#include "block.h"

#define CACHE_SLOTS 64u
#define SWAP_SECTORS_PER_PAGE 8u
#define SWAP_MAX_SLOTS (1u << 22)
#define SWAP_BITMAP_BYTES (SWAP_MAX_SLOTS / 8u)
#define MBR_PARTITION_OFFSET 446u
#define MBR_SIGNATURE_OFFSET 510u

struct cache_slot {
    block_device_t *dev;
    uint32_t lba;
    bool valid;
    uint8_t data[BLOCK_SECTOR_SIZE] __attribute__((aligned(16)));
};

static block_device_t devices[BLOCK_MAX_DEVICES];
static uint32_t device_total;
static struct cache_slot cache[CACHE_SLOTS];
static uint32_t cache_hits;
static uint32_t cache_misses;
static uint8_t swap_bitmap[SWAP_BITMAP_BYTES];
static block_device_t *swap_device;
static uint32_t swap_base_lba;
static uint32_t swap_slots;
static uint32_t swap_used;
static uint32_t swap_hint;
static bool swap_ready;

static uint64_t block_irq_save(void)
{
#ifdef BOOK8_HOST_TEST
    return 0u;
#else
    uint64_t flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
#endif
}

static void block_irq_restore(uint64_t flags)
{
#ifndef BOOK8_HOST_TEST
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
#else
    (void)flags;
#endif
}

static void copy_bytes(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
}

static struct cache_slot *slot_for(block_device_t *dev, uint32_t lba)
{
    return &cache[(lba + dev->id * 7u) % CACHE_SLOTS];
}

block_device_t *block_register(const char *name, uint64_t sector_count,
    block_read_fn read_blocks, block_write_fn write_blocks, void *priv)
{
    block_device_t *dev;
    uint32_t i;

    if (device_total >= BLOCK_MAX_DEVICES || read_blocks == 0) {
        return 0;
    }
    dev = &devices[device_total];
    dev->id = device_total++;
    for (i = 0; i + 1u < sizeof(dev->name) && name[i] != '\0'; ++i) {
        dev->name[i] = name[i];
    }
    dev->name[i] = '\0';
    dev->sector_size = BLOCK_SECTOR_SIZE;
    dev->sector_count = sector_count;
    dev->read_blocks = read_blocks;
    dev->write_blocks = write_blocks;
    dev->priv = priv;
    return dev;
}

uint32_t block_count(void)
{
    return device_total;
}

block_device_t *block_get(uint32_t index)
{
    return index < device_total ? &devices[index] : 0;
}

static bool range_ok(const block_device_t *dev, uint32_t lba, uint32_t count)
{
    return dev != 0 && count != 0u
        && (uint64_t)lba + count <= dev->sector_count;
}

bool block_read(block_device_t *dev, uint32_t start_lba, uint32_t sector_count,
    void *buf)
{
    return range_ok(dev, start_lba, sector_count)
        && dev->read_blocks(dev, start_lba, sector_count, buf);
}

bool block_write(block_device_t *dev, uint32_t start_lba, uint32_t sector_count,
    const void *buf)
{
    uint32_t i;

    if (!range_ok(dev, start_lba, sector_count) || dev->write_blocks == 0) {
        return false;
    }
    for (i = 0; i < sector_count; ++i) {
        struct cache_slot *slot = slot_for(dev, start_lba + i);
        if (slot->valid && slot->dev == dev && slot->lba == start_lba + i) {
            slot->valid = false;
        }
    }
    return dev->write_blocks(dev, start_lba, sector_count, buf);
}

bool block_cache_read(block_device_t *dev, uint32_t start_lba,
    uint32_t sector_count, void *buf)
{
    uint32_t i;

    if (!range_ok(dev, start_lba, sector_count)) {
        return false;
    }
    for (i = 0; i < sector_count; ++i) {
        uint32_t lba = start_lba + i;
        struct cache_slot *slot = slot_for(dev, lba);

        if (slot->valid && slot->dev == dev && slot->lba == lba) {
            ++cache_hits;
        } else {
            ++cache_misses;
            slot->valid = false;
            if (!dev->read_blocks(dev, lba, 1u, slot->data)) {
                return false;
            }
            slot->dev = dev;
            slot->lba = lba;
            slot->valid = true;
        }
        copy_bytes((uint8_t *)buf + (uint64_t)i * BLOCK_SECTOR_SIZE,
            slot->data, BLOCK_SECTOR_SIZE);
    }
    return true;
}

void block_cache_invalidate(block_device_t *dev)
{
    uint32_t i;

    for (i = 0; i < CACHE_SLOTS; ++i) {
        if (dev == 0 || cache[i].dev == dev) {
            cache[i].valid = false;
        }
    }
}

void block_cache_stats(uint32_t *hits, uint32_t *misses)
{
    *hits = cache_hits;
    *misses = cache_misses;
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8u
        | (uint32_t)bytes[2] << 16u | (uint32_t)bytes[3] << 24u;
}

static bool swap_bit(uint32_t slot)
{
    return (swap_bitmap[slot >> 3u] & (uint8_t)(1u << (slot & 7u))) != 0u;
}

static void swap_set_bit(uint32_t slot, bool used)
{
    uint8_t mask = (uint8_t)(1u << (slot & 7u));

    if (used)
        swap_bitmap[slot >> 3u] |= mask;
    else
        swap_bitmap[slot >> 3u] &= (uint8_t)~mask;
}

void swap_init(uint64_t physical_memory_bytes)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint64_t flags;

    swap_device = 0;
    swap_base_lba = 0u;
    swap_slots = 0u;
    swap_used = 0u;
    swap_hint = 0u;
    swap_ready = false;
    for (uint32_t i = 0u; i < SWAP_BITMAP_BYTES; ++i)
        swap_bitmap[i] = 0u;
    if (physical_memory_bytes == 0u)
        return;

    for (uint32_t device_index = 0u; device_index < block_count();
        ++device_index) {
        block_device_t *dev = block_get(device_index);

        if (dev == 0 || dev->write_blocks == 0
            || !block_read(dev, 0u, 1u, sector)
            || sector[MBR_SIGNATURE_OFFSET] != 0x55u
            || sector[MBR_SIGNATURE_OFFSET + 1u] != 0xaau) {
            continue;
        }
        for (uint32_t part = 0u; part < 4u; ++part) {
            const uint8_t *entry = sector + MBR_PARTITION_OFFSET + part * 16u;
            uint32_t start = read_le32(entry + 8u);
            uint32_t count = read_le32(entry + 12u);
            uint64_t end = (uint64_t)start + count;
            uint64_t bytes = (uint64_t)count * BLOCK_SECTOR_SIZE;
            uint32_t slots = count / SWAP_SECTORS_PER_PAGE;

            if (entry[4] != 0x82u || start == 0u || count < 8u
                || end > dev->sector_count || end > 0x100000000ull
                || bytes < physical_memory_bytes
                || slots == 0u || slots > SWAP_MAX_SLOTS) {
                continue;
            }
            swap_device = dev;
            swap_base_lba = start;
            swap_slots = slots;
            swap_ready = true;
            return;
        }
    }

    flags = block_irq_save();
    swap_device = 0;
    swap_base_lba = 0u;
    swap_slots = 0u;
    swap_used = 0u;
    swap_hint = 0u;
    swap_ready = false;
    block_irq_restore(flags);
}

bool swap_available(void)
{
    return swap_ready;
}

uint32_t swap_total_slots(void)
{
    return swap_ready ? swap_slots : 0u;
}

uint32_t swap_used_slots(void)
{
    uint64_t flags = block_irq_save();
    uint32_t used = swap_used;

    block_irq_restore(flags);
    return used;
}

bool swap_alloc_slot(uint32_t *slot)
{
    uint64_t flags;

    if (slot == 0 || !swap_ready)
        return false;
    flags = block_irq_save();
    for (uint32_t offset = 0u; offset < swap_slots; ++offset) {
        uint32_t candidate = (swap_hint + offset) % swap_slots;

        if (!swap_bit(candidate)) {
            swap_set_bit(candidate, true);
            ++swap_used;
            swap_hint = candidate + 1u == swap_slots ? 0u : candidate + 1u;
            *slot = candidate;
            block_irq_restore(flags);
            return true;
        }
    }
    block_irq_restore(flags);
    return false;
}

void swap_free_slot(uint32_t slot)
{
    uint64_t flags = block_irq_save();

    if (swap_ready && slot < swap_slots && swap_bit(slot)) {
        swap_set_bit(slot, false);
        --swap_used;
        if (slot < swap_hint)
            swap_hint = slot;
    }
    block_irq_restore(flags);
}

static bool swap_transfer(uint32_t slot, void *page, bool write)
{
    uint64_t flags;
    uint32_t lba;
    bool result;

    if (page == 0 || !swap_ready || slot >= swap_slots)
        return false;
    flags = block_irq_save();
    if (!swap_bit(slot)) {
        block_irq_restore(flags);
        return false;
    }
    lba = swap_base_lba + slot * SWAP_SECTORS_PER_PAGE;
    result = write
        ? block_write(swap_device, lba, SWAP_SECTORS_PER_PAGE, page)
        : block_read(swap_device, lba, SWAP_SECTORS_PER_PAGE, page);
    block_irq_restore(flags);
    return result;
}

bool swap_write_page(uint32_t slot, const void *page)
{
    return swap_transfer(slot, (void *)page, true);
}

bool swap_read_page(uint32_t slot, void *page)
{
    return swap_transfer(slot, page, false);
}
