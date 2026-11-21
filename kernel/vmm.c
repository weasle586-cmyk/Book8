#include "vmm.h"
#include "../drivers/block.h"
#include "exec.h"

#define PAGE_SIZE 4096u
#define PAGE_MASK 0x000ffffffffff000ull
#define PTE_PRESENT 0x001ull
#define PTE_WRITABLE 0x002ull
#define PTE_ACCESSED 0x020ull
#define PTE_SWAP_MARKER 0x200ull
#define PTE_FLAGS_MASK 0x8000000000000fffull
#define MULTIBOOT_TAG_END 0u
#define MULTIBOOT_TAG_MODULE 3u
#define MULTIBOOT_TAG_MMAP 6u
#define MULTIBOOT_MAX_FRAMES (1u << 20)
#define FRAME_BITMAP_BYTES (MULTIBOOT_MAX_FRAMES / 8u)
#define VMM_PAGE_COUNT 32768u
#define VMM_PT_COUNT (VMM_PAGE_COUNT / 512u)
#define VMM_BASE 0x0000010000000000ull
#define VMM_END (VMM_BASE + (uint64_t)VMM_PAGE_COUNT * PAGE_SIZE)

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct vmm_page {
    uint64_t frame;
    uint64_t flags;
    uint32_t swap_slot;
    uint8_t state;
};

static uint8_t frame_bitmap[FRAME_BITMAP_BYTES];
static struct vmm_page pages[VMM_PAGE_COUNT];
static uint64_t vmm_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t vmm_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t vmm_pts[VMM_PT_COUNT][512]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t frame_search_hint = 256u;
static uint32_t page_search_hint;
static uint32_t page_clock_hand;
static uint32_t mapped_pages;
static uint32_t swapped_pages;
static bool initialized;

extern uint64_t boot_pml4[];
extern char __kernel_end[];

static uint64_t vmm_irq_save(void)
{
#ifdef BOOK8_HOST_TEST
    return 0u;
#else
    uint64_t flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
#endif
}

static void vmm_irq_restore(uint64_t flags)
{
#ifndef BOOK8_HOST_TEST
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
#else
    (void)flags;
#endif
}

static void invalidate_page(uint64_t address)
{
#ifndef BOOK8_HOST_TEST
    __asm__ volatile ("invlpg (%0)" :: "r"(address) : "memory");
#else
    (void)address;
#endif
}

static void set_frame_range(uint64_t start, uint64_t end, bool used)
{
    uint64_t first;
    uint64_t last;

    if (start >= 0x100000000ull)
        return;
    if (end > 0x100000000ull)
        end = 0x100000000ull;
    if (start >= end)
        return;
    if (used) {
        first = start / PAGE_SIZE;
        last = (end + PAGE_SIZE - 1u) / PAGE_SIZE;
    } else {
        first = (start + PAGE_SIZE - 1u) / PAGE_SIZE;
        last = end / PAGE_SIZE;
    }
    if (last > MULTIBOOT_MAX_FRAMES)
        last = MULTIBOOT_MAX_FRAMES;
    for (uint64_t frame = first; frame < last; ++frame) {
        uint8_t mask = (uint8_t)(1u << (frame & 7u));

        if (used)
            frame_bitmap[frame >> 3u] |= mask;
        else
            frame_bitmap[frame >> 3u] &= (uint8_t)~mask;
    }
}

static bool frame_is_used(uint32_t frame)
{
    return (frame_bitmap[frame >> 3u]
        & (uint8_t)(1u << (frame & 7u))) != 0u;
}

static void frame_set_used(uint32_t frame, bool used)
{
    uint8_t mask = (uint8_t)(1u << (frame & 7u));

    if (used)
        frame_bitmap[frame >> 3u] |= mask;
    else
        frame_bitmap[frame >> 3u] &= (uint8_t)~mask;
}

static void *frame_allocate(void);

static uint64_t *pte_for_index(uint32_t index)
{
    return &vmm_pts[index / 512u][index % 512u];
}

static bool evict_index(uint32_t index, bool force)
{
    struct vmm_page *page = &pages[index];
    uint64_t *pte = pte_for_index(index);
    uint64_t entry = *pte;
    uint32_t slot;

    if (page->state != 1u || (entry & PTE_PRESENT) == 0u)
        return false;
    if (!force && (entry & PTE_ACCESSED) != 0u) {
        *pte = entry & ~PTE_ACCESSED;
        invalidate_page(VMM_BASE + (uint64_t)index * PAGE_SIZE);
        return false;
    }
    if (!swap_alloc_slot(&slot))
        return false;
    if (!swap_write_page(slot, (const void *)(uintptr_t)(entry & PAGE_MASK))) {
        swap_free_slot(slot);
        return false;
    }

    page->frame = 0u;
    page->flags = entry & PTE_FLAGS_MASK & ~PTE_PRESENT;
    page->swap_slot = slot;
    page->state = 2u;
    *pte = ((uint64_t)slot << 12u) | PTE_SWAP_MARKER
        | page->flags;
    invalidate_page(VMM_BASE + (uint64_t)index * PAGE_SIZE);
    frame_set_used((uint32_t)((entry & PAGE_MASK) / PAGE_SIZE), false);
    --mapped_pages;
    ++swapped_pages;
    return true;
}

static bool evict_one(void)
{
    if (!swap_available())
        return false;
    for (uint32_t scanned = 0u; scanned < VMM_PAGE_COUNT; ++scanned) {
        uint32_t index = page_clock_hand++;

        if (page_clock_hand == VMM_PAGE_COUNT)
            page_clock_hand = 0u;
        if (evict_index(index, false))
            return true;
    }
    for (uint32_t scanned = 0u; scanned < VMM_PAGE_COUNT; ++scanned) {
        uint32_t index = page_clock_hand++;

        if (page_clock_hand == VMM_PAGE_COUNT)
            page_clock_hand = 0u;
        if (pages[index].state == 1u
            && evict_index(index, true))
            return true;
    }
    return false;
}

static void *frame_allocate(void)
{
    uint32_t frame_count = MULTIBOOT_MAX_FRAMES;

    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        for (uint32_t scanned = 0u; scanned < frame_count; ++scanned) {
            uint32_t frame = frame_search_hint + scanned;

            if (frame >= frame_count)
                frame -= frame_count;
            if (!frame_is_used(frame)) {
                frame_set_used(frame, true);
                frame_search_hint = frame + 1u == frame_count ? 256u : frame + 1u;
                return (void *)(uintptr_t)((uint64_t)frame * PAGE_SIZE);
            }
        }
        if (pass == 0u && evict_one())
            continue;
        break;
    }
    return 0;
}

static void *frame_allocate_contiguous(uint32_t count)
{
    uint32_t frame_count = MULTIBOOT_MAX_FRAMES;
    if (count == 0u || count > frame_count)
        return 0;

    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        uint32_t start = pass == 0u ? frame_search_hint : 256u;
        uint32_t end = pass == 0u ? frame_count : frame_search_hint;
        if (end > frame_count - count)
            end = frame_count - count;
        for (uint32_t frame = start; frame <= end; ++frame) {
            uint32_t offset = 0u;
            while (offset < count && !frame_is_used(frame + offset))
                ++offset;
            if (offset == count) {
                for (offset = 0u; offset < count; ++offset)
                    frame_set_used(frame + offset, true);
                frame_search_hint = frame + count;
                if (frame_search_hint >= frame_count)
                    frame_search_hint = 256u;
                return (void *)(uintptr_t)((uint64_t)frame * PAGE_SIZE);
            }
            frame += offset;
        }
    }
    return 0;
}

static void frame_release(uint64_t frame)
{
    uint64_t index = frame / PAGE_SIZE;

    if ((frame & (PAGE_SIZE - 1u)) == 0u
        && index < MULTIBOOT_MAX_FRAMES)
        frame_set_used((uint32_t)index, false);
}

static bool reserve_virtual_range(uint32_t *first, uint32_t count)
{
    uint32_t run = 0u;
    uint32_t scan_start = page_search_hint;
    uint32_t run_start = 0u;

    for (uint32_t scanned = 0u; scanned < VMM_PAGE_COUNT; ++scanned) {
        uint32_t linear = scan_start + scanned;
        uint32_t index;

        if (linear >= VMM_PAGE_COUNT) {
            run = 0u;
            linear -= VMM_PAGE_COUNT;
        }
        index = linear;
        if (pages[index].state == 0u) {
            if (run == 0u)
                run_start = index;
            if (++run == count) {
                *first = run_start;
                page_search_hint = run_start + count;
                if (page_search_hint >= VMM_PAGE_COUNT)
                    page_search_hint %= VMM_PAGE_COUNT;
                return true;
            }
        } else {
            run = 0u;
        }
    }
    return false;
}

bool vmm_init(const void *multiboot_info)
{
    const uint8_t *info = multiboot_info;
    uint32_t total_size;
    uint32_t offset;

    if (initialized || info == 0)
        return false;
    total_size = *(const uint32_t *)info;
    if (total_size < 16u || (boot_pml4[2] & PTE_PRESENT) != 0u)
        return false;
    for (uint32_t i = 0u; i < FRAME_BITMAP_BYTES; ++i)
        frame_bitmap[i] = 0xffu;
    for (uint32_t i = 0u; i < VMM_PAGE_COUNT; ++i)
        pages[i] = (struct vmm_page){0};
    for (uint32_t i = 0u; i < VMM_PT_COUNT; ++i) {
        for (uint32_t j = 0u; j < 512u; ++j)
            vmm_pts[i][j] = 0u;
    }
    for (uint32_t i = 0u; i < 512u; ++i) {
        vmm_pdpt[i] = 0u;
        vmm_pd[i] = 0u;
    }

    offset = 8u;
    while (offset <= total_size - sizeof(struct multiboot_tag)) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(info + offset);
        uint32_t next;

        if (tag->size < sizeof(*tag) || tag->size > total_size - offset)
            break;
        if (tag->type == MULTIBOOT_TAG_END)
            break;
        if (tag->type == MULTIBOOT_TAG_MMAP && tag->size >= 16u) {
            const uint32_t *words = (const uint32_t *)tag;
            uint32_t entry_size = words[2];

            for (uint32_t pos = 16u; entry_size >= 24u
                && pos + entry_size <= tag->size; pos += entry_size) {
                const uint8_t *entry = (const uint8_t *)tag + pos;
                const uint64_t *fields = (const uint64_t *)entry;

                if (*(const uint32_t *)(entry + 16u) == 1u
                    && fields[1] <= UINT64_MAX - fields[0])
                    set_frame_range(fields[0], fields[0] + fields[1], false);
            }
        }
        next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset)
            break;
        offset += next;
    }

    set_frame_range(0u, 0x100000u, true);
    set_frame_range(0u, (uint64_t)(uintptr_t)__kernel_end, true);
    set_frame_range((uint64_t)(uintptr_t)info,
        (uint64_t)(uintptr_t)info + total_size, true);

    offset = 8u;
    while (offset <= total_size - sizeof(struct multiboot_tag)) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(info + offset);
        uint32_t next;

        if (tag->size < sizeof(*tag) || tag->size > total_size - offset)
            break;
        if (tag->type == MULTIBOOT_TAG_END)
            break;
        if (tag->type == MULTIBOOT_TAG_MODULE && tag->size >= 16u) {
            const uint32_t *words = (const uint32_t *)tag;

            if (words[3] >= words[2])
                set_frame_range(words[2], words[3], true);
        }
        next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset)
            break;
        offset += next;
    }

    vmm_pdpt[0] = (uint64_t)(uintptr_t)vmm_pd | PTE_PRESENT | PTE_WRITABLE;
    for (uint32_t i = 0u; i < VMM_PT_COUNT; ++i) {
        vmm_pd[i] = (uint64_t)(uintptr_t)vmm_pts[i]
            | PTE_PRESENT | PTE_WRITABLE;
    }
    boot_pml4[2] = (uint64_t)(uintptr_t)vmm_pdpt
        | PTE_PRESENT | PTE_WRITABLE;
    initialized = true;
#ifndef BOOK8_HOST_TEST
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
#endif
    return true;
}

void *vmm_alloc_pages(uint32_t count)
{
    uint64_t flags;
    uint32_t first;

    if (!initialized || count == 0u || count > VMM_PAGE_COUNT)
        return 0;
    flags = vmm_irq_save();
    if (!reserve_virtual_range(&first, count)) {
        vmm_irq_restore(flags);
        return 0;
    }
    for (uint32_t offset = 0u; offset < count; ++offset) {
        uint32_t index = first + offset;
        struct vmm_page *page = &pages[index];
        uint64_t *pte = pte_for_index(index);
        uint8_t *frame = frame_allocate();

        if (frame == 0) {
            vmm_irq_restore(flags);
            vmm_free_pages((void *)(uintptr_t)(VMM_BASE
                + (uint64_t)first * PAGE_SIZE), offset);
            return 0;
        }
        for (uint32_t i = 0u; i < PAGE_SIZE; ++i)
            frame[i] = 0u;
        page->frame = (uint64_t)(uintptr_t)frame;
        page->flags = PTE_WRITABLE;
        page->state = 1u;
        *pte = page->frame | PTE_PRESENT | PTE_WRITABLE;
        ++mapped_pages;
        invalidate_page(VMM_BASE + (uint64_t)index * PAGE_SIZE);
    }
    vmm_irq_restore(flags);
    return (void *)(uintptr_t)(VMM_BASE + (uint64_t)first * PAGE_SIZE);
}

void *vmm_alloc_contiguous_pages(uint32_t count)
{
    uint64_t flags;
    uint32_t first;

    if (!initialized || count == 0u || count > VMM_PAGE_COUNT)
        return 0;
    flags = vmm_irq_save();
    if (!reserve_virtual_range(&first, count)) {
        vmm_irq_restore(flags);
        return 0;
    }
    uint8_t *frame = frame_allocate_contiguous(count);
    if (frame == 0) {
        vmm_irq_restore(flags);
        return 0;
    }
    uint64_t frame_base = (uint64_t)(uintptr_t)frame;
    for (uint32_t offset = 0u; offset < count; ++offset) {
        uint32_t index = first + offset;
        struct vmm_page *page = &pages[index];
        uint64_t physical = frame_base + (uint64_t)offset * PAGE_SIZE;
        uint8_t *backing = (uint8_t *)(uintptr_t)physical;
        for (uint32_t byte = 0u; byte < PAGE_SIZE; ++byte)
            backing[byte] = 0u;
        page->frame = physical;
        page->flags = PTE_WRITABLE;
        page->state = 1u;
        *pte_for_index(index) = physical | PTE_PRESENT | PTE_WRITABLE;
        ++mapped_pages;
        invalidate_page(VMM_BASE + (uint64_t)index * PAGE_SIZE);
    }
    vmm_irq_restore(flags);
    return (void *)(uintptr_t)(VMM_BASE + (uint64_t)first * PAGE_SIZE);
}

bool vmm_dma_address(const void *pointer, size_t size, uint64_t *physical)
{
    uint64_t address = (uint64_t)(uintptr_t)pointer;
    if (pointer == 0 || physical == 0 || size == 0u
        || size > 0x100000000ull)
        return false;
    if (address < 0x100000000ull) {
        if (size > 0x100000000ull - address)
            return false;
        *physical = address;
        return true;
    }
    if (address < VMM_BASE || address >= VMM_END)
        return false;

    uint64_t offset = address - VMM_BASE;
    uint32_t index = (uint32_t)(offset / PAGE_SIZE);
    uint32_t page_offset = (uint32_t)(offset & (PAGE_SIZE - 1u));
    uint64_t span = (uint64_t)page_offset + size;
    uint32_t page_count = (uint32_t)((span + PAGE_SIZE - 1u) / PAGE_SIZE);
    if (page_count == 0u || page_count > VMM_PAGE_COUNT - index
        || pages[index].state != 1u)
        return false;
    uint64_t base = pages[index].frame + page_offset;
    for (uint32_t page = 1u; page < page_count; ++page) {
        if (pages[index + page].state != 1u
            || pages[index + page].frame
                != pages[index].frame + (uint64_t)page * PAGE_SIZE)
            return false;
    }
    if (base >= 0x100000000ull
        || size > 0x100000000ull - base)
        return false;
    *physical = base;
    return true;
}

void vmm_free_pages(void *base, uint32_t count)
{
    uint64_t address = (uint64_t)(uintptr_t)base;
    uint64_t flags;
    uint32_t first;

    if (!initialized || address < VMM_BASE || address >= VMM_END
        || (address & (PAGE_SIZE - 1u)) != 0u)
        return;
    first = (uint32_t)((address - VMM_BASE) / PAGE_SIZE);
    if (count == 0u || count > VMM_PAGE_COUNT - first)
        return;
    flags = vmm_irq_save();
    for (uint32_t offset = 0u; offset < count; ++offset) {
        uint32_t index = first + offset;
        struct vmm_page *page = &pages[index];
        uint64_t *pte = pte_for_index(index);

        if (page->state == 1u) {
            frame_release(page->frame);
            --mapped_pages;
        } else if (page->state == 2u) {
            swap_free_slot(page->swap_slot);
            --swapped_pages;
        } else {
            continue;
        }
        *pte = 0u;
        pages[index] = (struct vmm_page){0};
        invalidate_page(VMM_BASE + (uint64_t)index * PAGE_SIZE);
    }
    if (first < page_search_hint)
        page_search_hint = first;
    vmm_irq_restore(flags);
}

bool vmm_handle_page_fault(uint64_t address, uint64_t error_code)
{
    uint64_t flags;
    uint32_t index;
    struct vmm_page *page;
    uint64_t *pte;
    uint8_t *frame;

    if (!initialized || (error_code & 5u) != 0u
        || address < VMM_BASE || address >= VMM_END)
        return false;
    index = (uint32_t)((address - VMM_BASE) / PAGE_SIZE);
    page = &pages[index];
    pte = pte_for_index(index);
    if (page->state != 2u || (*pte & PTE_SWAP_MARKER) == 0u)
        return false;
    flags = vmm_irq_save();
    frame = frame_allocate();
    if (frame == 0 || !swap_read_page(page->swap_slot, frame)) {
        if (frame != 0)
            frame_release((uint64_t)(uintptr_t)frame);
        vmm_irq_restore(flags);
        return false;
    }
    *pte = (uint64_t)(uintptr_t)frame | page->flags | PTE_PRESENT;
    page->frame = (uint64_t)(uintptr_t)frame;
    page->state = 1u;
    swap_free_slot(page->swap_slot);
    page->swap_slot = 0u;
    --swapped_pages;
    ++mapped_pages;
    invalidate_page(address & ~(uint64_t)(PAGE_SIZE - 1u));
    vmm_irq_restore(flags);
    return true;
}

bool vmm_swap_selftest(void)
{
    volatile uint8_t *page;
    uint8_t *memory;
    uint32_t before;
    bool passed = true;

    if (!swap_available())
        return false;
    memory = vmm_alloc_pages(1u);
    if (memory == 0)
        return false;
    page = memory;
    for (uint32_t i = 0u; i < PAGE_SIZE; ++i)
        page[i] = (uint8_t)(i * 37u + 11u);
    before = swap_used_slots();

    uint64_t flags = vmm_irq_save();
    uint32_t index = (uint32_t)(((uint64_t)(uintptr_t)memory - VMM_BASE)
        / PAGE_SIZE);
    bool evicted = evict_index(index, true);
    vmm_irq_restore(flags);
    if (!evicted || swap_used_slots() != before + 1u) {
        vmm_free_pages(memory, 1u);
        return false;
    }

    for (uint32_t i = 0u; i < PAGE_SIZE; ++i) {
        if (page[i] != (uint8_t)(i * 37u + 11u)) {
            passed = false;
            break;
        }
    }
    if (swap_used_slots() != before)
        passed = false;
    vmm_free_pages(memory, 1u);
    return passed;
}

uint32_t vmm_page_count(void)
{
    return mapped_pages;
}

uint32_t vmm_swapped_page_count(void)
{
    return swapped_pages;
}

bool book8_page_fault_dispatch(uint64_t address, uint64_t error_code)
{
    return exec_page_fault(address, error_code)
        || vmm_handle_page_fault(address, error_code);
}
