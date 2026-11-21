#include <stdlib.h>
#include <string.h>
#include "../kernel/sched.h"

/*
 * Boundary-tag allocator.
 *
 * Block: [8-byte header][payload ... ][8-byte footer]. Header and footer both
 * hold (size | allocated); size covers header+payload+footer and is a
 * multiple of 16. A pointer to the payload ("bp") is 16-byte aligned because
 * blocks start at addresses congruent to 8 (mod 16). Free blocks keep
 * next/prev free-list links in the first 16 payload bytes, so the minimum
 * block is 32 bytes.
 *
 * Heap layout (base is page-aligned):
 *   base+0   8-byte pad
 *   base+8   prologue block header/footer (size 16, allocated)
 *   base+24  first real block ... up to the epilogue header (size 0,
 *            allocated) in the last 8 bytes of the heap.
 */

#define TAG_ALLOC ((size_t)1)
#define TAG_MASK (~(size_t)0xF)
#define MIN_BLOCK ((size_t)32)
#define TAG_OVERHEAD ((size_t)16)

typedef struct free_links {
    struct free_links *next;
    struct free_links *prev;
} free_links_t;

static uint8_t heap_arena[HEAP_DEFAULT_SIZE] __attribute__((aligned(HEAP_PAGE_SIZE)));
static uintptr_t heap_base;
static uintptr_t heap_limit;
static free_links_t *free_head;
static bool heap_ready;

static inline size_t read_tag(const void *address)
{
    return *(const size_t *)address;
}

static inline void write_tag(void *address, size_t value)
{
    *(size_t *)address = value;
}

static inline size_t block_size(uint8_t *bp)
{
    return read_tag(bp - 8) & TAG_MASK;
}

static inline bool block_allocated(uint8_t *bp)
{
    return (read_tag(bp - 8) & TAG_ALLOC) != 0;
}

static inline void set_block(uint8_t *bp, size_t size, bool allocated)
{
    size_t tag = size | (allocated ? TAG_ALLOC : 0);
    write_tag(bp - 8, tag);
    write_tag(bp + size - TAG_OVERHEAD, tag);
}

static inline uint8_t *next_block(uint8_t *bp)
{
    return bp + block_size(bp);
}

static inline uint8_t *prev_block(uint8_t *bp)
{
    return bp - (read_tag(bp - TAG_OVERHEAD) & TAG_MASK);
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void list_insert(uint8_t *bp)
{
    free_links_t *node = (free_links_t *)bp;
    node->prev = NULL;
    node->next = free_head;
    if (free_head != NULL) {
        free_head->prev = node;
    }
    free_head = node;
}

static void list_remove(uint8_t *bp)
{
    free_links_t *node = (free_links_t *)bp;
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        free_head = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
}

static uint8_t *first_block(void)
{
    return (uint8_t *)(heap_base + 32u);
}

static bool in_heap(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return address >= heap_base + 32u && address < heap_limit - 8u;
}

static bool heap_init_unlocked(void *base, size_t size)
{
    uintptr_t start = align_up((uintptr_t)base, HEAP_PAGE_SIZE);
    uintptr_t end;

    if (base == NULL || size < HEAP_PAGE_SIZE ||
        (uintptr_t)base + size < (uintptr_t)base) {
        return false;
    }
    end = ((uintptr_t)base + size) & ~(uintptr_t)(HEAP_PAGE_SIZE - 1u);
    if (end <= start || end - start < HEAP_PAGE_SIZE) {
        return false;
    }

    heap_base = start;
    heap_limit = end;
    write_tag((void *)start, 0);
    set_block((uint8_t *)(start + 16u), TAG_OVERHEAD, true);
    write_tag((void *)(end - 8u), TAG_ALLOC);
    free_head = NULL;
    set_block(first_block(), (size_t)(end - start) - 32u, false);
    list_insert(first_block());
    heap_ready = true;
    return true;
}

static bool ensure_heap(void)
{
    return heap_ready || heap_init(heap_arena, sizeof(heap_arena));
}

static uint8_t *coalesce(uint8_t *bp)
{
    bool prev_alloc = (read_tag(bp - TAG_OVERHEAD) & TAG_ALLOC) != 0;
    uint8_t *next = next_block(bp);
    bool next_alloc = block_allocated(next);
    size_t size = block_size(bp);

    if (!prev_alloc) {
        uint8_t *prev = prev_block(bp);
        list_remove(prev);
        size += block_size(prev);
        bp = prev;
    }
    if (!next_alloc) {
        list_remove(next);
        size += block_size(next);
    }
    set_block(bp, size, false);
    list_insert(bp);
    return bp;
}

/* Allocates asize bytes from the free block bp, splitting off any remainder. */
static void place(uint8_t *bp, size_t asize)
{
    size_t size = block_size(bp);

    list_remove(bp);
    if (size - asize >= MIN_BLOCK) {
        set_block(bp, asize, true);
        set_block(bp + asize, size - asize, false);
        list_insert(bp + asize);
    } else {
        set_block(bp, size, true);
    }
}

static size_t adjusted_size(size_t size)
{
    if (size > SIZE_MAX - 64u) {
        return 0;
    }
    size = align_up(size + TAG_OVERHEAD, HEAP_ALIGNMENT);
    return size < MIN_BLOCK ? MIN_BLOCK : size;
}

static void *malloc_unlocked(size_t size)
{
    size_t asize;

    if (size == 0 || !ensure_heap()) {
        return NULL;
    }
    asize = adjusted_size(size);
    if (asize == 0) {
        return NULL;
    }
    for (free_links_t *node = free_head; node != NULL; node = node->next) {
        if (block_size((uint8_t *)node) >= asize) {
            place((uint8_t *)node, asize);
            return node;
        }
    }
    return NULL;
}

static void free_unlocked(void *pointer)
{
    uint8_t *bp = pointer;

    if (pointer == NULL || !heap_ready || !in_heap(pointer) ||
        ((uintptr_t)pointer & (HEAP_ALIGNMENT - 1u)) != 0 ||
        !block_allocated(bp)) {
        return;
    }
    set_block(bp, block_size(bp), false);
    coalesce(bp);
}

static void *calloc_unlocked(size_t count, size_t size)
{
    void *pointer;

    if (count != 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    pointer = malloc(count * size);
    if (pointer != NULL) {
        memset(pointer, 0, count * size);
    }
    return pointer;
}

static void *realloc_unlocked(void *pointer, size_t size)
{
    uint8_t *bp = pointer;
    size_t asize;
    size_t old_size;
    void *replacement;

    if (pointer == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    if (!heap_ready || !in_heap(pointer) ||
        ((uintptr_t)pointer & (HEAP_ALIGNMENT - 1u)) != 0 ||
        !block_allocated(bp)) {
        return NULL;
    }
    asize = adjusted_size(size);
    if (asize == 0) {
        return NULL;
    }

    old_size = block_size(bp);
    if (old_size < asize) {
        uint8_t *next = next_block(bp);
        if (!block_allocated(next) && old_size + block_size(next) >= asize) {
            list_remove(next);
            old_size += block_size(next);
            set_block(bp, old_size, true);
        }
    }
    if (old_size >= asize) {
        if (old_size - asize >= MIN_BLOCK) {
            set_block(bp, asize, true);
            set_block(bp + asize, old_size - asize, false);
            coalesce(bp + asize);
        } else {
            set_block(bp, old_size, true);
        }
        return pointer;
    }

    replacement = malloc(size);
    if (replacement == NULL) {
        return NULL;
    }
    memcpy(replacement, pointer, old_size - TAG_OVERHEAD);
    free(pointer);
    return replacement;
}

static void heap_get_stats_unlocked(heap_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    if (!ensure_heap()) {
        return;
    }
    stats->start = heap_base;
    stats->end = heap_limit;
    stats->total = (size_t)(heap_limit - heap_base);
    for (uint8_t *bp = first_block(); block_size(bp) != 0; bp = next_block(bp)) {
        size_t size = block_size(bp);
        if (block_allocated(bp)) {
            stats->used += size;
            stats->used_blocks++;
        } else {
            stats->free_bytes += size;
            stats->free_blocks++;
            if (size > stats->largest_free) {
                stats->largest_free = size;
            }
        }
    }
}

static bool heap_check_unlocked(void)
{
    size_t walked_free = 0;
    size_t listed_free = 0;
    bool previous_free = false;
    uint8_t *bp;

    if (!ensure_heap()) {
        return false;
    }
    if (read_tag((void *)(heap_base + 8u)) != (TAG_OVERHEAD | TAG_ALLOC) ||
        read_tag((void *)(heap_limit - 8u)) != TAG_ALLOC) {
        return false;
    }
    for (bp = first_block(); block_size(bp) != 0; bp = next_block(bp)) {
        size_t size = block_size(bp);
        bool allocated = block_allocated(bp);
        if (((uintptr_t)bp & (HEAP_ALIGNMENT - 1u)) != 0 || size < MIN_BLOCK ||
            (size & (HEAP_ALIGNMENT - 1u)) != 0 ||
            (uintptr_t)bp + size > heap_limit ||
            read_tag(bp - 8) != read_tag(bp + size - TAG_OVERHEAD)) {
            return false;
        }
        if (!allocated) {
            if (previous_free) {
                return false;
            }
            walked_free++;
        }
        previous_free = !allocated;
    }
    if ((uintptr_t)bp - 8u != heap_limit - 8u) {
        return false;
    }
    for (free_links_t *node = free_head; node != NULL; node = node->next) {
        if (!in_heap(node) || block_allocated((uint8_t *)node) ||
            (node->next != NULL && node->next->prev != node) ||
            ++listed_free > walked_free) {
            return false;
        }
    }
    return listed_free == walked_free && (free_head == NULL || free_head->prev == NULL);
}

bool heap_init(void *base, size_t size)
{
    uint64_t flags = sched_irq_save();
    bool result = heap_init_unlocked(base, size);
    sched_irq_restore(flags);
    return result;
}

void *malloc(size_t size)
{
    uint64_t flags = sched_irq_save();
    void *result = malloc_unlocked(size);
    sched_irq_restore(flags);
    return result;
}

void free(void *pointer)
{
    uint64_t flags = sched_irq_save();
    free_unlocked(pointer);
    sched_irq_restore(flags);
}

void *calloc(size_t count, size_t size)
{
    uint64_t flags = sched_irq_save();
    void *result = calloc_unlocked(count, size);
    sched_irq_restore(flags);
    return result;
}

void *realloc(void *pointer, size_t size)
{
    uint64_t flags = sched_irq_save();
    void *result = realloc_unlocked(pointer, size);
    sched_irq_restore(flags);
    return result;
}

void heap_get_stats(heap_stats_t *stats)
{
    uint64_t flags = sched_irq_save();
    heap_get_stats_unlocked(stats);
    sched_irq_restore(flags);
}

bool heap_check(void)
{
    uint64_t flags = sched_irq_save();
    bool result = heap_check_unlocked();
    sched_irq_restore(flags);
    return result;
}
