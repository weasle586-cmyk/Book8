#include "exec.h"
#include "../src/syscall.h"
#include "../src/session.h"
#include "../include/b8_shlib.h"
#include "../include/b8pkg.h"
#include "vmm.h"
#include <stddef.h>

#define POOL_PAGES 512u
#define PAGE       4096ull
#define EXEC_MAX_RELOCATIONS 65536u
#define PTE_P      0x1ull
#define PTE_RW     0x2ull
#define PTE_US     0x4ull
#define PTE_A      0x20ull
#define PTE_SWAP   0x200ull
#define PTE_MASK   0x000ffffffffff000ull
#define MAX_SEG_BYTES (16ull * 1024u * 1024u)

extern uint64_t boot_pml4[];
extern uint64_t book8_read_cr3(void);
extern void book8_write_cr3(uint64_t value);

static uint8_t page_pool[EXEC_MAX_TASKS][POOL_PAGES][4096]
    __attribute__((aligned(4096)));
static uint8_t page_pool_state[EXEC_MAX_TASKS][POOL_PAGES];
static uint64_t page_pool_vaddr[EXEC_MAX_TASKS][POOL_PAGES];
static uint32_t page_clock_hand[EXEC_MAX_TASKS];
static bool page_pool_reserved[EXEC_MAX_TASKS];
static uint64_t test_pml4[2][512] __attribute__((aligned(4096)));

static struct exec_task tasks[EXEC_MAX_TASKS];
static struct exec_task *current;
static uint32_t next_pid = 1u;
static bool scheduler_test_active;
static uint32_t scheduler_quantum;
static volatile uint64_t scheduler_ticks;
static uint32_t scheduler_user_switches;
static uint32_t scheduler_user_stack_errors;
static volatile uint64_t scheduler_work[2];
static volatile uint64_t scheduler_error[2];

#define SCHEDULER_QUANTUM_TICKS 2u

static void invalidate_user_page(uint64_t address)
{
#ifndef BOOK8_HOST_TEST
    __asm__ volatile ("invlpg (%0)" :: "r"(address) : "memory");
#else
    (void)address;
#endif
}

_Static_assert(offsetof(struct timer_frame, rip) == 15u * sizeof(uint64_t),
               "timer register frame layout");
_Static_assert(sizeof(struct timer_frame) == 20u * sizeof(uint64_t),
               "timer iret frame layout");

extern void book8_scheduler_test_worker(void);

static void scheduler_test_prepare(struct exec_task *task, unsigned index)
{
    uint64_t top = (uint64_t)&task->kernel_stack[EXEC_KERNEL_STACK_SIZE];
    struct timer_frame *frame = (struct timer_frame *)(top - 8u - sizeof(*frame));

    *frame = (struct timer_frame){0};
    frame->rip = (uint64_t)book8_scheduler_test_worker;
    frame->rdi = (uint64_t)&scheduler_work[index];
    frame->rsi = (uint64_t)&scheduler_error[index];
    frame->cs = 0x08u;
    frame->rflags = 0x202u;
    frame->rsp = top - 8u;
    frame->ss = 0x10u;
    task->rsp = (uint64_t)frame;
    task->state = TASK_READY;
}

bool exec_scheduler_test_start(void)
{
    if (scheduler_test_active || current != 0)
        return false;
    for (unsigned i = 0u; i < EXEC_MAX_TASKS; ++i) {
        if (tasks[i].state != TASK_FREE)
            return false;
    }
    scheduler_work[0] = scheduler_work[1] = 0u;
    scheduler_error[0] = scheduler_error[1] = 0u;
    scheduler_ticks = 0u;
    scheduler_user_switches = 0u;
    scheduler_user_stack_errors = 0u;
    scheduler_quantum = SCHEDULER_QUANTUM_TICKS;
    tasks[0].cr3 = book8_read_cr3();
    for (unsigned i = 1u; i <= 2u; ++i) {
        for (unsigned j = 0u; j < 512u; ++j)
            test_pml4[i - 1u][j] = boot_pml4[j];
        test_pml4[i - 1u][0] &= ~PTE_US;
        tasks[i].cr3 = (uint64_t)test_pml4[i - 1u];
    }
    tasks[0].pid = 1u;
    tasks[0].state = TASK_RUNNING;
    current = &tasks[0];
    scheduler_test_prepare(&tasks[1], 0u);
    scheduler_test_prepare(&tasks[2], 1u);
    scheduler_test_active = true;
    return true;
}

uint64_t exec_scheduler_test_ticks(void)
{
    return scheduler_ticks;
}

uint32_t exec_scheduler_test_user_switches(void)
{
    return scheduler_user_switches;
}

struct exec_task *exec_scheduler_test_current(void)
{
    return scheduler_test_active && current == &tasks[0] ? current : 0;
}

bool exec_scheduler_test_active(void)
{
    return scheduler_test_active;
}

bool exec_scheduler_test_finish(void)
{
    if (!scheduler_test_active || current != &tasks[0])
        return false;
    bool passed = scheduler_ticks >= 12u
        && scheduler_work[0] != 0u && scheduler_work[1] != 0u
        && scheduler_error[0] == 0u && scheduler_error[1] == 0u
        && scheduler_user_stack_errors == 0u;
    scheduler_test_active = false;
    for (unsigned i = 0u; i < 3u; ++i) {
        tasks[i].state = TASK_FREE;
        tasks[i].rsp = 0u;
        tasks[i].cr3 = 0u;
    }
    current = 0;
    return passed;
}

struct timer_frame *exec_timer_tick(struct timer_frame *frame)
{
    if (current == 0 || current->state != TASK_RUNNING)
        return frame;
    ++scheduler_ticks;
    if (--scheduler_quantum != 0u)
        return frame;
    scheduler_quantum = SCHEDULER_QUANTUM_TICKS;
    unsigned index = (unsigned)(current - tasks);
    for (unsigned offset = 1u; offset < EXEC_MAX_TASKS; ++offset) {
        struct exec_task *next = &tasks[(index + offset) % EXEC_MAX_TASKS];
        if (next->state == TASK_READY) {
            if (scheduler_test_active && (frame->cs & 3u) == 3u)
                ++scheduler_user_switches;
            if (scheduler_test_active && (frame->cs & 3u) == 3u) {
                uintptr_t frame_address = (uintptr_t)frame;
                uintptr_t stack_end = (uintptr_t)current->kernel_stack
                    + EXEC_KERNEL_STACK_SIZE;
                if (frame_address != stack_end - sizeof(*frame))
                    ++scheduler_user_stack_errors;
            }
            current->rsp = (uint64_t)frame;
            current->state = TASK_READY;
            next->state = TASK_RUNNING;
            current = next;
            syscall_select_task(next);
            if (book8_read_cr3() != next->cr3)
                book8_write_cr3(next->cr3);
            return (struct timer_frame *)next->rsp;
        }
    }
    return frame;
}

/* Kernel-side copies of path/argv: execve frees the old image, so nothing may
 * point into it afterwards. */
static char arg_store[EXEC_ARG_BYTES];
static char *arg_ptrs[EXEC_MAX_ARGS];
static char path_store[B8PKG_MAX_PATH_SIZE + 1u];
static uint8_t bounce[4096] __attribute__((aligned(16)));
static uint32_t export_name_hashes[B8_SHLIB_MAX_SYMBOLS];
static char manifest_data[B8PKG_MAX_FILE_SIZE];
static struct b8pkg_manifest manifest_workspace;
static struct elf_image module_image_workspace;

static bool task_page_fault(struct exec_task *task, uint64_t address,
    uint64_t error_code);

static void zero_bytes(void *p, uint64_t n)
{
    uint8_t *b = p;

    while (n--) {
        *b++ = 0;
    }
}

static uint32_t str_length(const char *s)
{
    uint32_t n = 0u;

    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static uint64_t *lookup_pte_ptr(struct exec_task *task, uint64_t vaddr)
{
    uint64_t pde;
    uint64_t *pt;

    if (task == 0 || task->user_pd == 0
        || vaddr < USER_BASE || vaddr >= USER_LIMIT)
        return 0;
    pde = task->user_pd[(vaddr - USER_BASE) >> 21];
    if ((pde & PTE_P) == 0u)
        return 0;
    pt = (uint64_t *)(pde & PTE_MASK);
    return &pt[(vaddr >> 12) & 511u];
}

static bool evict_user_page(struct exec_task *task, bool force)
{
    unsigned task_index = task->address_space_slot;
    for (unsigned scanned = 0u; scanned < POOL_PAGES; ++scanned) {
        unsigned pool_index = page_clock_hand[task_index]++;
        uint64_t *pte;
        uint64_t entry;
        uint32_t slot;

        if (page_clock_hand[task_index] == POOL_PAGES)
            page_clock_hand[task_index] = 0u;
        if (page_pool_state[task_index][pool_index] != 2u)
            continue;
        pte = lookup_pte_ptr(task, page_pool_vaddr[task_index][pool_index]);
        if (pte == 0 || (*pte & PTE_P) == 0u)
            continue;
        entry = *pte;
        if (!force && (entry & PTE_A) != 0u) {
            *pte = entry & ~PTE_A;
            invalidate_user_page(page_pool_vaddr[task_index][pool_index]);
            continue;
        }
        if (!swap_alloc_slot(&slot))
            return false;
        if (!swap_write_page(slot,
            (const void *)(uintptr_t)(entry & PTE_MASK))) {
            swap_free_slot(slot);
            return false;
        }
        *pte = ((uint64_t)slot << 12u) | PTE_SWAP
            | (entry & (PTE_RW | PTE_US));
        invalidate_user_page(page_pool_vaddr[task_index][pool_index]);
        page_pool_state[task_index][pool_index] = 0u;
        page_pool_vaddr[task_index][pool_index] = 0u;
        --task->page_pool_used;
        return true;
    }
    if (!force) {
        for (unsigned scanned = 0u; scanned < POOL_PAGES; ++scanned) {
            unsigned pool_index = page_clock_hand[task_index]++;

            if (page_clock_hand[task_index] == POOL_PAGES)
                page_clock_hand[task_index] = 0u;
            if (page_pool_state[task_index][pool_index] == 2u
                && evict_user_page(task, true))
                return true;
        }
    }
    return false;
}

static void *pool_alloc(struct exec_task *task)
{
    unsigned task_index;

    if (task == 0 || task->address_space_slot >= EXEC_MAX_TASKS)
        return 0;
    task_index = task->address_space_slot;
    for (unsigned pass = 0u; pass < 2u; ++pass) {
        for (unsigned i = 0u; i < POOL_PAGES; ++i) {
            if (page_pool_state[task_index][i] == 0u) {
                uint8_t *page = page_pool[task_index][i];

                page_pool_state[task_index][i] = 1u;
                ++task->page_pool_used;
                zero_bytes(page, PAGE);
                return page;
            }
        }
        if (pass == 0u && swap_available()
            && evict_user_page(task, false))
            continue;
        break;
    }
    return 0;
}

static void *pool_alloc_data(struct exec_task *task, uint64_t vaddr)
{
    unsigned task_index;
    uint8_t *page = pool_alloc(task);
    uintptr_t base;
    unsigned pool_index;

    if (page == 0)
        return 0;
    task_index = task->address_space_slot;
    base = (uintptr_t)&page_pool[task_index][0][0];
    pool_index = (unsigned)(((uintptr_t)page - base) / PAGE);
    page_pool_state[task_index][pool_index] = 2u;
    page_pool_vaddr[task_index][pool_index] = vaddr & ~(PAGE - 1u);
    return page;
}

static void pool_release(struct exec_task *task, void *page)
{
    unsigned task_index;
    uintptr_t base;
    uintptr_t address = (uintptr_t)page;

    if (task == 0 || task->address_space_slot >= EXEC_MAX_TASKS || page == 0)
        return;
    task_index = task->address_space_slot;
    base = (uintptr_t)&page_pool[task_index][0][0];
    if (address < base || address >= base + sizeof(page_pool[task_index])
        || ((address - base) & (PAGE - 1u)) != 0u)
        return;
    unsigned pool_index = (unsigned)((address - base) / PAGE);
    if (page_pool_state[task_index][pool_index] != 0u) {
        page_pool_state[task_index][pool_index] = 0u;
        page_pool_vaddr[task_index][pool_index] = 0u;
        --task->page_pool_used;
    }
}

static bool space_create(struct exec_task *task)
{
    uint64_t *pml4 = pool_alloc(task);
    uint64_t *pdpt = pool_alloc(task);
    uint64_t *pd = pool_alloc(task);
    if (pml4 == 0 || pdpt == 0 || pd == 0) {
        return false;
    }
    for (unsigned i = 0u; i < 512u; ++i)
        pml4[i] = boot_pml4[i];
    pml4[0] &= ~PTE_US;
    pdpt[0] = (uint64_t)pd | PTE_P | PTE_RW | PTE_US;
    pml4[1] = (uint64_t)pdpt | PTE_P | PTE_RW | PTE_US;
    task->cr3 = (uint64_t)pml4;
    task->user_pd = pd;
    return true;
}

static void space_destroy(struct exec_task *task)
{
    unsigned task_index;

    if (task == 0 || task->address_space_slot >= EXEC_MAX_TASKS)
        return;
    task_index = task->address_space_slot;
    if (task->user_pd != 0) {
        for (unsigned pde_index = 0u; pde_index < 512u; ++pde_index) {
            uint64_t pde = task->user_pd[pde_index];

            if ((pde & PTE_P) == 0u)
                continue;
            uint64_t *pt = (uint64_t *)(pde & PTE_MASK);
            for (unsigned pte_index = 0u; pte_index < 512u; ++pte_index) {
                uint64_t entry = pt[pte_index];

                if ((entry & PTE_SWAP) != 0u)
                    swap_free_slot((uint32_t)((entry & PTE_MASK) >> 12u));
            }
        }
    }
    if (task->cr3 != 0u && book8_read_cr3() == task->cr3)
        book8_write_cr3((uint64_t)boot_pml4);
    for (unsigned i = 0u; i < POOL_PAGES; ++i) {
        page_pool_state[task_index][i] = 0u;
        page_pool_vaddr[task_index][i] = 0u;
    }
    page_clock_hand[task_index] = 0u;
    task->cr3 = 0u;
    task->user_pd = 0;
    task->page_pool_used = 0u;
    page_pool_reserved[task_index] = false;
    task->address_space_slot = UINT32_MAX;
}

/* Returns the kernel (identity-mapped) address of the frame backing vaddr's
 * page, mapping it U/S=1 if needed. A page shared by a read-only and a
 * writable segment becomes writable. */
static uint8_t *map_page(struct exec_task *task, uint64_t vaddr, bool write)
{
    uint64_t *pde = &task->user_pd[(vaddr - USER_BASE) >> 21];
    uint64_t *pt, *pte;
    uint8_t *frame;

    if ((*pde & PTE_P) == 0u) {
        pt = pool_alloc(task);
        if (pt == 0) {
            return 0;
        }
        *pde = (uint64_t)pt | PTE_P | PTE_RW | PTE_US;
    } else {
        pt = (uint64_t *)(*pde & PTE_MASK);
    }
    pte = &pt[(vaddr >> 12) & 511u];
    if ((*pte & PTE_SWAP) != 0u) {
        if (!task_page_fault(task, vaddr, 0u))
            return 0;
        pde = &task->user_pd[(vaddr - USER_BASE) >> 21];
        pt = (uint64_t *)(*pde & PTE_MASK);
        pte = &pt[(vaddr >> 12) & 511u];
    }
    if ((*pte & PTE_P) != 0u) {
        if (write)
            *pte |= PTE_RW;
        return (uint8_t *)(*pte & PTE_MASK);
    }
    frame = pool_alloc_data(task, vaddr);
    if (frame == 0) {
        return 0;
    }
    *pte = (uint64_t)frame | PTE_P | PTE_US | (write ? PTE_RW : 0u);
    return frame;
}

static uint64_t lookup_pte(struct exec_task *task, uint64_t vaddr)
{
    uint64_t *pte = lookup_pte_ptr(task, vaddr);
    return pte == 0 ? 0u : *pte;
}

bool exec_has_user_space(void)
{
    return current != 0 && current->user_pd != 0;
}

static bool task_page_fault(struct exec_task *task, uint64_t address,
    uint64_t error_code)
{
    uint64_t page_address = address & ~(PAGE - 1u);
    uint64_t *pte;
    uint64_t entry;
    uint32_t slot;
    uint8_t *frame;
    unsigned task_index;
    uintptr_t base;
    unsigned pool_index;

    if (task == 0 || task->user_pd == 0
        || (error_code & 1u) != 0u || address < USER_BASE
        || address >= USER_LIMIT)
        return false;
    pte = lookup_pte_ptr(task, page_address);
    if (pte == 0 || (*pte & PTE_SWAP) == 0u)
        return false;
    entry = *pte;
    if ((error_code & 4u) != 0u && (entry & PTE_US) == 0u)
        return false;
    slot = (uint32_t)((entry & PTE_MASK) >> 12u);
    frame = pool_alloc(task);
    if (frame == 0)
        return false;
    if (!swap_read_page(slot, frame)) {
        pool_release(task, frame);
        return false;
    }

    task_index = task->address_space_slot;
    base = (uintptr_t)&page_pool[task_index][0][0];
    pool_index = (unsigned)(((uintptr_t)frame - base) / PAGE);
    page_pool_state[task_index][pool_index] = 2u;
    page_pool_vaddr[task_index][pool_index] = page_address;
    *pte = (uint64_t)(uintptr_t)frame | PTE_P
        | (entry & (PTE_RW | PTE_US));
    invalidate_user_page(page_address);
    swap_free_slot(slot);
    return true;
}

bool exec_page_fault(uint64_t address, uint64_t error_code)
{
    return task_page_fault(current, address, error_code);
}

bool exec_address_space_test(void)
{
    uint64_t saved_cr3;
    uint64_t *first, *second;
    uint8_t *first_page, *second_page;
    bool passed = false;

    if (current != 0 || tasks[0].state != TASK_FREE
        || tasks[1].state != TASK_FREE || page_pool_reserved[0]
        || page_pool_reserved[1])
        return false;
    tasks[0].address_space_slot = UINT32_MAX;
    tasks[1].address_space_slot = UINT32_MAX;
    saved_cr3 = book8_read_cr3();
    current = &tasks[0];
    current->address_space_slot = 0u;
    page_pool_reserved[0] = true;
    if (!space_create(current))
        goto cleanup;
    first = (uint64_t *)current->cr3;
    first_page = map_page(current, USER_BASE, true);
    if (first_page == 0 || map_page(current, USER_BASE + PAGE, false) == 0
        || !exec_user_range_ok(USER_BASE, 1u, true)
        || !exec_user_range_ok(USER_BASE + PAGE, 1u, false)
        || exec_user_range_ok(USER_BASE + PAGE, 1u, true))
        goto cleanup;
    current = &tasks[1];
    current->address_space_slot = 1u;
    page_pool_reserved[1] = true;
    if (!space_create(current))
        goto cleanup;
    second = (uint64_t *)current->cr3;
    second_page = map_page(current, USER_BASE, true);
    if (second_page == 0 || first == second || first_page == second_page
        || (first[0] & PTE_US) != 0 || (second[0] & PTE_US) != 0
        || first[0] != (boot_pml4[0] & ~PTE_US) || first[0] != second[0]
        || first[1] == second[1]
        || !exec_user_range_ok(USER_BASE, 1u, true)
        || exec_user_range_ok(USER_BASE + PAGE, 1u, false))
        goto cleanup;
    for (unsigned i = 2u; i < 512u; ++i) {
        if (first[i] != boot_pml4[i] || second[i] != boot_pml4[i])
            goto cleanup;
    }
    book8_write_cr3((uint64_t)first);
    *(volatile uint8_t *)USER_BASE = 0x35u;
    book8_write_cr3((uint64_t)second);
    *(volatile uint8_t *)USER_BASE = 0xcau;
    if (*(volatile uint8_t *)USER_BASE != 0xcau || *first_page != 0x35u)
        goto cleanup;
    book8_write_cr3((uint64_t)first);
    passed = *(volatile uint8_t *)USER_BASE == 0x35u && *second_page == 0xcau;

cleanup:
    if (book8_read_cr3() != saved_cr3)
        book8_write_cr3(saved_cr3);
    if (tasks[0].address_space_slot < EXEC_MAX_TASKS)
        space_destroy(&tasks[0]);
    if (tasks[1].address_space_slot < EXEC_MAX_TASKS)
        space_destroy(&tasks[1]);
    current = 0;
    return passed;
}

bool exec_user_range_ok(uint64_t ptr, uint64_t len, bool write)
{
    uint64_t page;

    if (ptr < USER_BASE || ptr >= USER_LIMIT || len > USER_LIMIT - ptr) {
        return false;
    }
    if (len == 0u) {
        return true;
    }
    for (page = ptr & ~(PAGE - 1u); page < ptr + len; page += PAGE) {
        uint64_t pte = lookup_pte(current, page);

        if ((pte & PTE_SWAP) != 0u) {
            if (!exec_page_fault(page, 0u))
                return false;
            pte = lookup_pte(current, page);
        }
        if ((pte & (PTE_P | PTE_US)) != (PTE_P | PTE_US)
            || (write && (pte & PTE_RW) == 0u)) {
            return false;
        }
    }
    return true;
}

static bool user_read_bytes(void *dst, uint64_t src, uint64_t len)
{
    uint8_t *d = dst;
    const uint8_t *s = (const uint8_t *)src;

    if (!exec_user_range_ok(src, len, false)) {
        return false;
    }
    while (len--) {
        *d++ = *s++;
    }
    return true;
}

/* Copies a NUL-terminated user string; returns length or a negative errno. */
static long user_read_string(char *dst, uint32_t cap, uint64_t src)
{
    uint32_t i;

    for (i = 0; i < cap; ++i) {
        if (!user_read_bytes(&dst[i], src + i, 1u)) {
            return -BOOK8_EFAULT;
        }
        if (dst[i] == '\0') {
            return (long)i;
        }
    }
    return -BOOK8_EINVAL;
}

static bool elf_read_at(file_handle_t *file, uint64_t offset, void *buffer,
    uint64_t size)
{
    if (file == 0 || buffer == 0 || offset > UINT32_MAX || size > file->size
        || offset > (uint64_t)file->size - size
        || size > INT32_MAX) {
        return false;
    }
    return fs_seek(file, (uint32_t)offset)
        && fs_read(file, buffer, (uint32_t)size) == (int32_t)size;
}

static bool elf_string_equals(const char *string, const char *expected)
{
    uint32_t i = 0u;

    while (string[i] != '\0' && expected[i] != '\0') {
        if (string[i] != expected[i]) { return false; }
        ++i;
    }
    return string[i] == '\0' && expected[i] == '\0';
}

static bool elf_shlib_magic_valid(const char magic[B8_SHLIB_MAGIC_SIZE])
{
    for (uint32_t i = 0u; i < B8_SHLIB_MAGIC_SIZE; ++i) {
        if (magic[i] != B8_SHLIB_MAGIC[i]) { return false; }
    }
    return true;
}

static bool elf_read_section_name(file_handle_t *file,
    const Elf64_Shdr *strings, uint32_t name_offset, char *name,
    uint32_t capacity)
{
    uint32_t maximum;

    if (name_offset >= strings->sh_size || capacity < 2u) { return false; }
    maximum = (uint32_t)(strings->sh_size - name_offset);
    if (maximum >= capacity) { maximum = capacity - 1u; }
    for (uint32_t i = 0u; i < maximum; ++i) {
        uint8_t character;
        uint64_t offset = strings->sh_offset + name_offset + i;
        if (!elf_read_at(file, offset, &character, sizeof(character))) {
            return false;
        }
        name[i] = (char)character;
        if (character == 0u) { return true; }
    }
    return false;
}

static uint32_t elf_name_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    while (*name != '\0') {
        hash ^= (uint8_t)*name++;
        hash *= 16777619u;
    }
    return hash;
}

static bool elf_export_value_valid(const struct elf_image *img,
    uint64_t value, uint32_t type)
{
    for (uint32_t i = 0u; i < img->phnum; ++i) {
        const Elf64_Phdr *ph = &img->phdr[i];
        uint64_t end;
        bool permission;

        if (ph->p_type != PT_LOAD || ph->p_vaddr > UINT64_MAX - ph->p_memsz) {
            continue;
        }
        end = ph->p_vaddr + ph->p_memsz;
        permission = type == B8_SYM_FUNC
            ? (ph->p_flags & PF_X) != 0u
            : (ph->p_flags & (PF_R | PF_W)) == (PF_R | PF_W);
        if (permission && value >= ph->p_vaddr && value < end) {
            return true;
        }
    }
    return false;
}

static bool elf_local_symbol_value_valid(const struct elf_image *img,
    const Elf64_Sym *symbol)
{
    uint64_t size = symbol->st_size == 0u ? 1u : symbol->st_size;
    uint32_t type = symbol->st_info & 0x0fu;
    if (symbol->st_value > UINT64_MAX - size)
        return false;
    for (uint32_t i = 0u; i < img->phnum; ++i) {
        const Elf64_Phdr *ph = &img->phdr[i];
        uint64_t end;
        if (ph->p_type != PT_LOAD || ph->p_vaddr > UINT64_MAX - ph->p_memsz)
            continue;
        end = ph->p_vaddr + ph->p_memsz;
        if (symbol->st_value < ph->p_vaddr
            || symbol->st_value + size > end)
            continue;
        if (type == STT_FUNC && (ph->p_flags & PF_X) == 0u)
            continue;
        if (type == STT_OBJECT && (ph->p_flags & PF_R) == 0u)
            continue;
        if (type != STT_NOTYPE && type != STT_FUNC && type != STT_OBJECT
            && type != STT_SECTION)
            return false;
        return true;
    }
    return false;
}

static bool elf_symbol_name_valid(const char name[B8_SYM_NAME_SIZE])
{
    for (uint32_t i = 0u; i < B8_SYM_NAME_SIZE; ++i) {
        if (name[i] == '\0') { return i != 0u; }
    }
    return false;
}

static bool elf_read_section_header(file_handle_t *file,
    const Elf64_Ehdr *eh, uint32_t index, Elf64_Shdr *sh)
{
    uint64_t offset = eh->e_shoff
        + (uint64_t)index * sizeof(Elf64_Shdr);
    return elf_read_at(file, offset, sh, sizeof(*sh));
}

static uint32_t elf_relocation_width(uint32_t type)
{
    switch (type) {
    case R_X86_64_NONE:
        return 0u;
    case R_X86_64_64:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
    case R_X86_64_RELATIVE:
        return 8u;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        return 4u;
    default:
        return UINT32_MAX;
    }
}

static bool elf_relocation_target_valid(const Elf64_Phdr *phdr,
    uint32_t phnum, uint64_t target, uint32_t width)
{
    if (width == 0u)
        return true;
    if (target > UINT64_MAX - width)
        return false;
    for (uint32_t i = 0u; i < phnum; ++i) {
        const Elf64_Phdr *ph = &phdr[i];
        uint64_t segment_end;
        if (ph->p_type != PT_LOAD || ph->p_vaddr > UINT64_MAX - ph->p_memsz)
            continue;
        segment_end = ph->p_vaddr + ph->p_memsz;
        if (target >= ph->p_vaddr && target + width <= segment_end)
            return true;
    }
    return false;
}

static long elf_check_relocations(file_handle_t *file, const Elf64_Ehdr *eh,
    const Elf64_Phdr *phdr, uint32_t phnum, struct elf_image *img)
{
    uint64_t section_bytes;
    uint64_t dynamic_rela_size = 0u, dynamic_plt_size = 0u;
    bool dynamic_rela_entry_seen = false, dynamic_plt_type_seen = false;

    if (eh->e_shnum != 0u) {
        section_bytes = (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr);
        if (eh->e_shentsize != sizeof(Elf64_Shdr)
            || eh->e_shoff > file->size
            || section_bytes > file->size - eh->e_shoff) {
            return -BOOK8_ENOEXEC;
        }
        img->section_headers_offset = eh->e_shoff;
        img->section_count = eh->e_shnum;
        for (uint32_t i = 0u; i < eh->e_shnum; ++i) {
            Elf64_Shdr section;
            if (!elf_read_section_header(file, eh, i, &section))
                return -BOOK8_ENOEXEC;
            if (section.sh_type != SHT_REL && section.sh_type != SHT_RELA
                && section.sh_type != SHT_RELR)
                continue;
            if (section.sh_offset > file->size
                || section.sh_size > file->size - section.sh_offset)
                return -BOOK8_ENOEXEC;
            if (section.sh_size == 0u)
                continue;
            if ((section.sh_flags & SHF_ALLOC) == 0u)
                continue;
            if (section.sh_type != SHT_RELA)
                return -BOOK8_ENOTSUP;
            if (section.sh_entsize != sizeof(Elf64_Rela)
                || section.sh_size % sizeof(Elf64_Rela) != 0u
                || section.sh_link >= eh->e_shnum
                || (section.sh_info != 0u && section.sh_info >= eh->e_shnum))
                return -BOOK8_ENOEXEC;
            Elf64_Shdr symbols, strings;
            if (!elf_read_section_header(file, eh, section.sh_link, &symbols)
                || (symbols.sh_type != SHT_DYNSYM
                    && symbols.sh_type != SHT_SYMTAB)
                || symbols.sh_entsize != sizeof(Elf64_Sym)
                || symbols.sh_size % sizeof(Elf64_Sym) != 0u
                || symbols.sh_offset > file->size
                || symbols.sh_size > file->size - symbols.sh_offset
                || symbols.sh_link >= eh->e_shnum
                || !elf_read_section_header(file, eh, symbols.sh_link,
                    &strings)
                || strings.sh_type != SHT_STRTAB
                || strings.sh_offset > file->size
                || strings.sh_size > file->size - strings.sh_offset) {
                return -BOOK8_ENOEXEC;
            }
            uint64_t symbol_count = symbols.sh_size / sizeof(Elf64_Sym);
            uint64_t entry_count = section.sh_size / sizeof(Elf64_Rela);
            if (entry_count > EXEC_MAX_RELOCATIONS - img->relocation_count)
                return -BOOK8_ENOEXEC;
            for (uint64_t j = 0u; j < entry_count; ++j) {
                Elf64_Rela rela;
                uint32_t type, symbol_index, width;
                if (!elf_read_at(file, section.sh_offset
                        + j * sizeof(rela), &rela, sizeof(rela))) {
                    return -BOOK8_ENOEXEC;
                }
                type = ELF64_R_TYPE(rela.r_info);
                symbol_index = ELF64_R_SYM(rela.r_info);
                width = elf_relocation_width(type);
                if (width == UINT32_MAX)
                    return -BOOK8_ENOTSUP;
                if ((type == R_X86_64_RELATIVE && symbol_index != 0u)
                    || (type != R_X86_64_NONE
                        && type != R_X86_64_RELATIVE
                        && (symbol_index == 0u
                            || symbol_index >= symbol_count))
                    || !elf_relocation_target_valid(phdr, phnum,
                        rela.r_offset, width)) {
                    return -BOOK8_ENOEXEC;
                }
            }
            img->relocation_count += (uint32_t)entry_count;
        }
    }

    for (uint32_t i = 0u; i < phnum; ++i) {
        const Elf64_Phdr *ph = &phdr[i];
        if (ph->p_type != PT_DYNAMIC)
            continue;
        if (ph->p_filesz % sizeof(Elf64_Dyn) != 0u
            || ph->p_filesz > ph->p_memsz
            || ph->p_offset > file->size
            || ph->p_filesz > file->size - ph->p_offset) {
            return -BOOK8_ENOEXEC;
        }
        bool terminated = false;
        for (uint64_t offset = 0u; offset < ph->p_filesz;
                offset += sizeof(Elf64_Dyn)) {
            Elf64_Dyn dynamic;
            if (!elf_read_at(file, ph->p_offset + offset, &dynamic,
                    sizeof(dynamic))) {
                return -BOOK8_ENOEXEC;
            }
            if (dynamic.d_tag == DT_NULL) {
                terminated = true;
                break;
            }
            if (dynamic.d_tag == DT_RELSZ || dynamic.d_tag == DT_RELENT
                || dynamic.d_tag == DT_REL || dynamic.d_tag == DT_RELR
                || (dynamic.d_tag == DT_RELRSZ
                    && dynamic.d_un.d_val != 0u)) {
                return -BOOK8_ENOTSUP;
            }
            if ((dynamic.d_tag == DT_RELASZ
                    || dynamic.d_tag == DT_PLTRELSZ)
                && dynamic.d_un.d_val % sizeof(Elf64_Rela) != 0u) {
                return -BOOK8_ENOEXEC;
            }
            if (dynamic.d_tag == DT_RELASZ)
                dynamic_rela_size = dynamic.d_un.d_val;
            if (dynamic.d_tag == DT_PLTRELSZ)
                dynamic_plt_size = dynamic.d_un.d_val;
            if (dynamic.d_tag == DT_RELAENT) {
                if (dynamic.d_un.d_val != sizeof(Elf64_Rela))
                    return -BOOK8_ENOEXEC;
                dynamic_rela_entry_seen = true;
            }
            if (dynamic.d_tag == DT_PLTREL) {
                if (dynamic.d_un.d_val != DT_RELA)
                    return -BOOK8_ENOTSUP;
                dynamic_plt_type_seen = true;
            }
        }
        if (!terminated)
            return -BOOK8_ENOEXEC;
    }
    if (dynamic_rela_size > UINT64_MAX - dynamic_plt_size)
        return -BOOK8_ENOEXEC;
    if ((dynamic_rela_size != 0u || dynamic_plt_size != 0u)
        && (!dynamic_rela_entry_seen
            || (dynamic_plt_size != 0u && !dynamic_plt_type_seen))) {
        return -BOOK8_ENOEXEC;
    }
    if (dynamic_rela_size + dynamic_plt_size != 0u
        && dynamic_rela_size + dynamic_plt_size
            != (uint64_t)img->relocation_count * sizeof(Elf64_Rela)) {
        return img->relocation_count == 0u
            ? -BOOK8_ENOTSUP : -BOOK8_ENOEXEC;
    }
    return 0;
}

static long elf_validate_symbol_section(file_handle_t *file,
    const struct elf_image *img, const Elf64_Shdr *section, bool exports,
    uint32_t *count_out, uint64_t *records_offset_out)
{
    b8_sym_table_header_t header;
    uint64_t record_size = exports ? sizeof(b8_sym_export_record_t)
        : sizeof(b8_sym_import_t);
    uint64_t expected_size;

    if (section->sh_type != SHT_PROGBITS
        || section->sh_size < sizeof(header)
        || !elf_read_at(file, section->sh_offset, &header, sizeof(header))
        || !elf_shlib_magic_valid(header.magic)
        || header.abi_version != B8_SHLIB_ABI_VERSION
        || header.count > B8_SHLIB_MAX_SYMBOLS) {
        return -BOOK8_ENOEXEC;
    }
    expected_size = sizeof(header) + (uint64_t)header.count * record_size;
    if (expected_size != section->sh_size) {
        return -BOOK8_ENOEXEC;
    }
    *count_out = header.count;
    *records_offset_out = section->sh_offset + sizeof(header);
    for (uint32_t i = 0u; i < header.count; ++i) {
        uint64_t offset = *records_offset_out + (uint64_t)i * record_size;
        if (exports) {
            b8_sym_export_record_t record;
            uint32_t hash;

            if (!elf_read_at(file, offset, &record, sizeof(record))
                || !elf_symbol_name_valid(record.name)
                || (record.type != B8_SYM_FUNC && record.type != B8_SYM_DATA)
                || !elf_export_value_valid(img, record.value, record.type)) {
                return -BOOK8_ENOEXEC;
            }
            hash = elf_name_hash(record.name);
            for (uint32_t previous = 0u; previous < i; ++previous) {
                if (export_name_hashes[previous] == hash) {
                    b8_sym_export_record_t earlier;
                    uint64_t earlier_offset = *records_offset_out
                        + (uint64_t)previous * record_size;
                    if (!elf_read_at(file, earlier_offset, &earlier,
                            sizeof(earlier))) {
                        return -BOOK8_ENOEXEC;
                    }
                    if (elf_string_equals(record.name, earlier.name)) {
                        return -BOOK8_ENOEXEC;
                    }
                }
            }
            export_name_hashes[i] = hash;
        } else {
            b8_sym_import_t record;
            if (!elf_read_at(file, offset, &record, sizeof(record))
                || !elf_symbol_name_valid(record.name)) {
                return -BOOK8_ENOEXEC;
            }
        }
    }
    return 0;
}

static long elf_inspect_shlib_sections(file_handle_t *file,
    const Elf64_Ehdr *eh, struct elf_image *img, bool require_exports)
{
    Elf64_Shdr strings, exports = {0}, imports = {0};
    char name[64];
    bool exports_seen = false, imports_seen = false;
    uint64_t section_bytes = (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr);
    long status;

    if (eh->e_shnum == 0u || eh->e_shentsize != sizeof(Elf64_Shdr)
        || eh->e_shoff > file->size || section_bytes > file->size - eh->e_shoff
        || eh->e_shstrndx >= eh->e_shnum
        || !elf_read_section_header(file, eh, eh->e_shstrndx, &strings)
        || strings.sh_type != SHT_STRTAB || strings.sh_size == 0u
        || strings.sh_offset > file->size
        || strings.sh_size > file->size - strings.sh_offset) {
        return -BOOK8_ENOEXEC;
    }

    for (uint32_t i = 0u; i < eh->e_shnum; ++i) {
        Elf64_Shdr section;
        if (!elf_read_section_header(file, eh, i, &section)
            || section.sh_name >= strings.sh_size
            || !elf_read_section_name(file, &strings, section.sh_name, name,
                sizeof(name))) {
            return -BOOK8_ENOEXEC;
        }
        if (section.sh_type != SHT_NOBITS
            && (section.sh_offset > file->size
                || section.sh_size > file->size - section.sh_offset)) {
            return -BOOK8_ENOEXEC;
        }
        if (elf_string_equals(name, B8_SHLIB_EXPORT_SECTION)) {
            if (exports_seen) { return -BOOK8_ENOEXEC; }
            exports_seen = true;
            exports = section;
        } else if (elf_string_equals(name, B8_SHLIB_IMPORT_SECTION)) {
            if (imports_seen) { return -BOOK8_ENOEXEC; }
            imports_seen = true;
            imports = section;
        }
    }
    if (require_exports && !exports_seen) { return -BOOK8_ENOEXEC; }
    if (exports_seen) {
        status = elf_validate_symbol_section(file, img, &exports, true,
            &img->exports_count, &img->exports_offset);
        if (status != 0) { return status; }
    }
    if (imports_seen) {
        status = elf_validate_symbol_section(file, img, &imports, false,
            &img->imports_count, &img->imports_offset);
        if (status != 0) { return status; }
    }
    return 0;
}

long elf_inspect(file_handle_t *file, struct elf_image *img)
{
    Elf64_Ehdr eh;
    struct elf_image parsed = {0};
    uint64_t total_pages = 0u;
    bool entry_ok = false;
    uint32_t i;

    if (file == 0 || img == 0
        || !elf_read_at(file, 0u, &eh, sizeof(eh))) {
        return -BOOK8_ENOEXEC;
    }
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L'
        || eh.e_ident[3] != 'F' || eh.e_ident[4] != ELFCLASS64
        || eh.e_ident[5] != ELFDATA2LSB || eh.e_ident[6] != EV_CURRENT
        || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
        || eh.e_version != EV_CURRENT
        || eh.e_machine != EM_X86_64 || eh.e_phentsize != sizeof(Elf64_Phdr)
        || eh.e_ehsize != sizeof(Elf64_Ehdr)
        || eh.e_phnum == 0u || eh.e_phnum > ELF_MAX_PHDRS
        || eh.e_phoff > file->size
        || (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr)
            > file->size - eh.e_phoff) {
        return -BOOK8_ENOEXEC;
    }
    if (!elf_read_at(file, eh.e_phoff, parsed.phdr,
            (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr))) {
        return -BOOK8_ENOEXEC;
    }
    parsed.phnum = eh.e_phnum;
    parsed.entry = eh.e_entry;
    parsed.type = eh.e_type;
    long relocation_status = elf_check_relocations(file, &eh, parsed.phdr,
        eh.e_phnum, &parsed);
    if (relocation_status != 0)
        return relocation_status;
    for (i = 0; i < parsed.phnum; ++i) {
        const Elf64_Phdr *ph = &parsed.phdr[i];
        uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE;

        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_memsz > MAX_SEG_BYTES
            || ph->p_filesz > ph->p_memsz
            || ph->p_offset > file->size
            || ph->p_filesz > file->size - ph->p_offset
            || ph->p_vaddr > UINT64_MAX - ph->p_memsz) {
            return -BOOK8_ENOEXEC;
        }
        if (eh.e_type == ET_EXEC
            && (ph->p_vaddr < USER_BASE || ph->p_vaddr >= stack_base - PAGE
                || ph->p_memsz > stack_base - PAGE - ph->p_vaddr)) {
            return -BOOK8_ENOEXEC;
        }
        total_pages += (ph->p_memsz + 2u * PAGE - 1u) / PAGE;
        if (eh.e_entry >= ph->p_vaddr && eh.e_entry < ph->p_vaddr + ph->p_filesz
            && (ph->p_flags & PF_X) != 0u) {
            entry_ok = true;
        }
    }
    if ((eh.e_type == ET_EXEC
            && total_pages + USER_STACK_PAGES + 24u > POOL_PAGES)
        || (eh.e_type == ET_DYN && total_pages > POOL_PAGES)
        || (eh.e_type == ET_EXEC && !entry_ok)) {
        return -BOOK8_ENOEXEC;
    }
    if (eh.e_type == ET_DYN) {
        long status = elf_inspect_shlib_sections(file, &eh, &parsed, true);
        if (status != 0) { return status; }
    } else if (eh.e_shnum != 0u) {
        long status = elf_inspect_shlib_sections(file, &eh, &parsed, false);
        if (status != 0) { return status; }
    }
    *img = parsed;
    return 0;
}

static long elf_load_into(struct exec_task *task, file_handle_t *file,
    const struct elf_image *img, uint64_t load_bias)
{
    uint32_t i;

    if (task == 0 || file == 0 || img == 0
        || (img->type != ET_EXEC && img->type != ET_DYN)
        || (img->type == ET_EXEC && load_bias != 0u))
        return -BOOK8_ENOEXEC;
    if (task->user_pd == 0 && !space_create(task)) {
        return -BOOK8_ENOMEM;
    }
    for (i = 0; i < img->phnum; ++i) {
        const Elf64_Phdr *ph = &img->phdr[i];
        bool writable = (ph->p_flags & PF_W) != 0u;
        uint64_t addr, end, done = 0u;

        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_vaddr > UINT64_MAX - load_bias
            || ph->p_vaddr + load_bias > UINT64_MAX - ph->p_memsz) {
            return -BOOK8_ENOEXEC;
        }
        uint64_t segment_address = ph->p_vaddr + load_bias;
        end = segment_address + ph->p_memsz;
        if (ph->p_memsz != 0u
            && (segment_address < USER_BASE
                || end > USER_STACK_TOP - USER_STACK_PAGES * PAGE - PAGE)) {
            return -BOOK8_ENOEXEC;
        }
        for (addr = segment_address & ~(PAGE - 1u); addr < end; addr += PAGE) {
            if (map_page(task, addr, writable) == 0) {
                return -BOOK8_ENOMEM;
            }
        }
        if (ph->p_filesz != 0u && !fs_seek(file, (uint32_t)ph->p_offset)) {
            return -BOOK8_ENOEXEC;
        }
        while (done < ph->p_filesz) {
            uint64_t va = segment_address + done;
            uint64_t in_page = va & (PAGE - 1u);
            uint64_t chunk = PAGE - in_page;
            uint8_t *frame = map_page(task, va & ~(PAGE - 1u), writable);

            if (chunk > ph->p_filesz - done) {
                chunk = ph->p_filesz - done;
            }
            if (frame == 0 || fs_read(file, frame + in_page, (uint32_t)chunk)
                != (int32_t)chunk) {
                return -BOOK8_ENOEXEC;
            }
            done += chunk;
        }
        if (end > task->image_high_water)
            task->image_high_water = end;
    }
    return 0;
}

long elf_load(file_handle_t *file, const struct elf_image *img)
{
    if (current == 0)
        return -BOOK8_EINVAL;
    return elf_load_into(current, file, img, 0u);
}

long elf_require_executable(const struct elf_image *img)
{
    return img != 0 && img->type == ET_EXEC ? 0 : -BOOK8_ENOEXEC;
}

static long resolve_image_imports(struct exec_task *task,
    file_handle_t *file, const struct elf_image *img,
    int32_t source_module, uint16_t *edges, bool *has_imports);

long elf_resolve_imports(file_handle_t *file, const struct elf_image *img)
{
    uint16_t edges[EXEC_MAX_MODULES] = {0};
    bool has_imports = false;
    long status;

    if (current == 0 || file == 0 || img == 0
        || (img->type != ET_DYN && img->type != ET_EXEC)) {
        return -BOOK8_EINVAL;
    }
    status = resolve_image_imports(current, file, img, -1, edges,
        &has_imports);
    if (status != 0)
        return status;
    return 0;
}

/* SysV-style initial stack: argc, argv[], NULL, envp NULL, auxv AT_NULL. */
static long build_stack(struct exec_task *task, int argc, char *const *argv,
    uint64_t *rsp_out)
{
    uint64_t top = USER_STACK_TOP;
    uint64_t base = top - USER_STACK_PAGES * PAGE;
    uint64_t str_va[EXEC_MAX_ARGS];
    uint64_t sp = top, words, rsp, *slots;
    uint32_t total = 0u, p;
    int i;
    uint8_t *frame[USER_STACK_PAGES];

    if (argc < 0 || (uint32_t)argc > EXEC_MAX_ARGS) {
        return -BOOK8_EINVAL;
    }
    for (i = 0; i < argc; ++i) {
        total += str_length(argv[i]) + 1u;
    }
    if (total > EXEC_ARG_BYTES) {
        return -BOOK8_EINVAL;
    }
    for (p = 0; p < USER_STACK_PAGES; ++p) {
        frame[p] = map_page(task, base + p * PAGE, true);
        if (frame[p] == 0) {
            return -BOOK8_ENOMEM;
        }
    }
#define STACK_PTR(va) (frame[((va) - base) / PAGE] + (((va) - base) % PAGE))
    for (i = argc - 1; i >= 0; --i) {
        uint32_t len = str_length(argv[i]) + 1u;
        uint32_t k;

        sp -= len;
        for (k = 0; k < len; ++k) {
            STACK_PTR(sp + k)[0] = (uint8_t)argv[i][k];
        }
        str_va[i] = sp;
    }
    words = (uint64_t)argc + 5u; /* argc, argv..., NULL, NULL(envp), 2x auxv */
    rsp = (sp - words * 8u) & ~15ull;
    if (rsp < base) {
        return -BOOK8_EINVAL;
    }
    slots = (uint64_t *)STACK_PTR(rsp);
    slots[0] = (uint64_t)argc;
    for (i = 0; i < argc; ++i) {
        slots[1 + i] = str_va[i];
    }
    slots[1 + argc] = 0u;
    slots[2 + argc] = 0u;
    slots[3 + argc] = 0u;
    slots[4 + argc] = 0u;
#undef STACK_PTR
    *rsp_out = rsp;
    return 0;
}

static void close_fds(struct exec_task *t)
{
    uint32_t i;

    for (i = 0; i < EXEC_MAX_FDS; ++i) {
        if (t->fds[i] != 0) {
            fs_close(t->fds[i]);
            t->fds[i] = 0;
        }
    }
}

static void set_name(struct exec_task *t, const char *path)
{
    uint32_t i = 0u;

    while (i + 1u < sizeof(t->name) && path[i] != '\0') {
        t->name[i] = path[i];
        ++i;
    }
    t->name[i] = '\0';
}

static bool ascii_case_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        char a = *left++, b = *right++;
        if (a >= 'A' && a <= 'Z') { a = (char)(a + ('a' - 'A')); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b + ('a' - 'A')); }
        if (a != b) { return false; }
    }
    return *left == '\0' && *right == '\0';
}

static bool ascii_case_prefix(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        char a = *text++, b = *prefix++;
        if (a == '\0')
            return false;
        if (a >= 'A' && a <= 'Z') { a = (char)(a + ('a' - 'A')); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b + ('a' - 'A')); }
        if (a != b) { return false; }
    }
    return true;
}

static bool append_text(char *destination, uint32_t capacity,
    uint32_t *length, const char *text)
{
    while (*text != '\0') {
        if (*length + 1u >= capacity)
            return false;
        destination[(*length)++] = *text++;
    }
    destination[*length] = '\0';
    return true;
}

static struct exec_task *task_reserve(void)
{
    uint32_t slot = UINT32_MAX;
    struct exec_task *task = 0;

    for (uint32_t i = 0u; i < EXEC_MAX_TASKS; ++i) {
        if (!page_pool_reserved[i]) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX)
        return 0;
    for (uint32_t i = 0u; i < EXEC_MAX_TASKS; ++i) {
        if (&tasks[i] != current && tasks[i].state == TASK_FREE) {
            task = &tasks[i];
            break;
        }
    }
    if (task == 0)
        return 0;
    zero_bytes(task, sizeof(*task));
    task->address_space_slot = slot;
    task->state = TASK_BLOCKED;
    page_pool_reserved[slot] = true;
    return task;
}

static void modules_destroy(struct exec_task *task)
{
    for (uint32_t i = 0u; i < task->module_count; ++i) {
        struct exec_module *module = &task->modules[i];
        if (module->exports != 0 && module->export_pages != 0u)
            vmm_free_pages(module->exports, module->export_pages);
        zero_bytes(module, sizeof(*module));
    }
    task->module_count = 0u;
}

static void task_release(struct exec_task *task)
{
    if (task == 0)
        return;
    close_fds(task);
    modules_destroy(task);
    if (task->address_space_slot < EXEC_MAX_TASKS)
        space_destroy(task);
    task->pid = 0u;
    task->uid = 0u;
    task->session_token = 0u;
    task->name[0] = '\0';
    task->entry = 0u;
    task->user_rsp = 0u;
    task->rsp = 0u;
    task->caller_rsp = 0u;
    task->exit_status = 0;
    task->address_space_slot = UINT32_MAX;
    task->state = TASK_FREE;
}

static long package_manifest_load(const char *requested_path, char *root,
    struct b8pkg_manifest *manifest)
{
    static const char prefix[] = "A:\\APPS\\";
    char manifest_path[B8PKG_MAX_PATH_SIZE + 1u];
    char expected_path[B8PKG_MAX_PATH_SIZE + 1u];
    uint32_t prefix_length = sizeof(prefix) - 1u;
    uint32_t folder_length = 0u, root_length = 0u, expected_length = 0u;
    const char *cursor;
    file_handle_t *file;
    int32_t parsed;

    if (!ascii_case_prefix(requested_path, prefix))
        return 0;
    cursor = requested_path + prefix_length;
    while (cursor[folder_length] != '\0'
        && cursor[folder_length] != '\\') {
        ++folder_length;
    }
    if (folder_length == 0u || cursor[folder_length] != '\\'
        || (folder_length == 1u && cursor[0] == '.')
        || (folder_length == 2u && cursor[0] == '.'
            && cursor[1] == '.'))
        return -BOOK8_ENOEXEC;
    for (uint32_t i = 0u; i < prefix_length; ++i)
        root[root_length++] = prefix[i];
    for (uint32_t i = 0u; i < folder_length; ++i) {
        char character = cursor[i];
        if (!((character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-' || character == '.'))
            return -BOOK8_ENOEXEC;
        if (root_length + 1u >= B8PKG_MAX_PATH_SIZE + 1u)
            return -BOOK8_EINVAL;
        root[root_length++] = character;
    }
    root[root_length] = '\0';

    for (uint32_t i = 0u; i < root_length; ++i)
        manifest_path[i] = root[i];
    manifest_path[root_length] = '\0';
    if (!append_text(manifest_path, sizeof(manifest_path), &root_length,
            "\\manifest.ini"))
        return -BOOK8_EINVAL;
    file = fs_open_any(manifest_path);
    if (file == 0)
        return -BOOK8_ENOENT;
    uint32_t file_size = file->size;
    if (file_size == 0u || file_size > B8PKG_MAX_FILE_SIZE) {
        fs_close(file);
        return -BOOK8_EINVAL;
    }
    int32_t got = fs_read(file, manifest_data, file_size);
    fs_close(file);
    if (got < 0 || (uint32_t)got != file_size)
        return -BOOK8_EIO;
    parsed = b8pkg_parse(manifest_data, (size_t)got, manifest);
    if (parsed != 0)
        return parsed;
    if (!ascii_case_equal(root + prefix_length, manifest->name))
        return -BOOK8_ENOEXEC;

    expected_length = root_length - (uint32_t)sizeof("\\manifest.ini") + 1u;
    for (uint32_t i = 0u; i < expected_length; ++i)
        expected_path[i] = root[i];
    expected_path[expected_length] = '\0';
    if (!append_text(expected_path, sizeof(expected_path), &expected_length,
            "\\"))
        return -BOOK8_EINVAL;
    for (uint32_t i = 0u; manifest->entry[i] != '\0'; ++i) {
        char character = manifest->entry[i] == '/' ? '\\' : manifest->entry[i];
        if (expected_length + 1u >= sizeof(expected_path))
            return -BOOK8_EINVAL;
        expected_path[expected_length++] = character;
        expected_path[expected_length] = '\0';
    }
    if (!ascii_case_equal(requested_path, expected_path))
        return -BOOK8_ENOEXEC;
    return 1;
}

static long module_choose_bias(struct exec_task *task,
    const struct elf_image *img, uint64_t *load_bias)
{
    uint64_t low = UINT64_MAX, high = 0u;
    uint64_t start, maximum_end = USER_STACK_TOP
        - USER_STACK_PAGES * PAGE - PAGE;

    for (uint32_t i = 0u; i < img->phnum; ++i) {
        const Elf64_Phdr *ph = &img->phdr[i];
        uint64_t segment_end, aligned_end;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0u)
            continue;
        if (ph->p_vaddr > UINT64_MAX - ph->p_memsz)
            return -BOOK8_ENOEXEC;
        segment_end = ph->p_vaddr + ph->p_memsz;
        if (segment_end > UINT64_MAX - (PAGE - 1u))
            return -BOOK8_ENOEXEC;
        aligned_end = (segment_end + PAGE - 1u) & ~(PAGE - 1u);
        if ((ph->p_vaddr & ~(PAGE - 1u)) < low)
            low = ph->p_vaddr & ~(PAGE - 1u);
        if (aligned_end > high)
            high = aligned_end;
    }
    if (low == UINT64_MAX || high <= low)
        return -BOOK8_ENOEXEC;
    start = task->image_high_water;
    if (start < USER_BASE)
        start = USER_BASE;
    if (start < low)
        start = low;
    if (start > UINT64_MAX - (PAGE - 1u))
        return -BOOK8_EINVAL;
    start = (start + PAGE - 1u) & ~(PAGE - 1u);

    for (uint32_t attempt = 0u; attempt <= POOL_PAGES; ++attempt) {
        uint64_t bias, image_end, conflict = 0u;
        if (start < low || start > UINT64_MAX - (high - low))
            return -BOOK8_EINVAL;
        bias = start - low;
        image_end = start + (high - low);
        if (image_end > maximum_end)
            return -BOOK8_EINVAL;

        for (uint32_t i = 0u; i < img->phnum && conflict == 0u; ++i) {
            const Elf64_Phdr *ph = &img->phdr[i];
            uint64_t segment_end, address;
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0u)
                continue;
            segment_end = ph->p_vaddr + ph->p_memsz;
            for (address = (ph->p_vaddr + bias) & ~(PAGE - 1u);
                    address < segment_end + bias; address += PAGE) {
                if (lookup_pte(task, address) != 0u) {
                    conflict = address;
                    break;
                }
            }
        }
        if (conflict == 0u) {
            *load_bias = bias;
            return 0;
        }
        if (conflict > UINT64_MAX - PAGE)
            return -BOOK8_EINVAL;
        start = conflict + PAGE;
    }
    return -BOOK8_EINVAL;
}

static long module_load_exports(struct exec_task *task,
    struct exec_module *module,
    file_handle_t *file, const struct elf_image *img)
{
    uint64_t bytes = (uint64_t)img->exports_count
        * sizeof(b8_sym_export_t);
    uint32_t pages = (uint32_t)((bytes + PAGE - 1u) / PAGE);

    module->export_count = img->exports_count;
    if (pages == 0u)
        return 0;
    module->exports = vmm_alloc_pages(pages);
    if (module->exports == 0)
        return -BOOK8_ENOMEM;
    module->export_pages = pages;
    zero_bytes(module->exports, (uint64_t)pages * PAGE);
    for (uint32_t i = 0u; i < img->exports_count; ++i) {
        b8_sym_export_record_t record;
        b8_sym_export_t *exported = &module->exports[i];
        uint64_t offset = img->exports_offset
            + (uint64_t)i * sizeof(record);
        if (!elf_read_at(file, offset, &record, sizeof(record))
            || record.value > UINT64_MAX - module->load_bias) {
            return -BOOK8_ENOEXEC;
        }
        for (uint32_t previous = 0u; previous + 1u < task->module_count;
                ++previous) {
            const struct exec_module *provider = &task->modules[previous];
            for (uint32_t j = 0u; j < provider->export_count; ++j) {
                if (elf_string_equals(record.name, provider->exports[j].name))
                    return -BOOK8_EINVAL;
            }
        }
        for (uint32_t j = 0u; j < B8_SYM_NAME_SIZE; ++j)
            exported->name[j] = record.name[j];
        exported->ptr = (void *)(uintptr_t)(module->load_bias + record.value);
        exported->type = record.type;
        exported->version_major = record.version_major;
        exported->version_minor = record.version_minor;
    }
    return 0;
}

static bool copy_path(char *destination, const char *source)
{
    uint32_t i = 0u;
    while (source[i] != '\0') {
        if (i + 1u >= B8PKG_MAX_PATH_SIZE + 1u)
            return false;
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
    return true;
}

static bool make_library_path(char *path, const char *root,
    const char *middle, const char *library)
{
    uint32_t length = 0u;
    path[0] = '\0';
    return append_text(path, B8PKG_MAX_PATH_SIZE + 1u, &length, root)
        && append_text(path, B8PKG_MAX_PATH_SIZE + 1u, &length, middle)
        && append_text(path, B8PKG_MAX_PATH_SIZE + 1u, &length, library);
}

static long load_package_libraries(struct exec_task *task, const char *root,
    const struct b8pkg_manifest *manifest)
{
    char local_path[B8PKG_MAX_PATH_SIZE + 1u];
    char global_path[B8PKG_MAX_PATH_SIZE + 1u];

    for (uint32_t i = 0u; i < manifest->library_count; ++i) {
        file_handle_t *file;
        const char *selected_path;
        struct elf_image *img = &module_image_workspace;
        struct exec_module *module;
        uint64_t bias;
        long status;

        if (task->module_count >= EXEC_MAX_MODULES)
            return -BOOK8_EINVAL;
        bool have_local = make_library_path(local_path, root, "\\lib\\",
            manifest->libraries[i]);
        bool have_global = make_library_path(global_path, "A:\\LIB\\", "",
            manifest->libraries[i]);
        file = have_local ? fs_open_any(local_path) : 0;
        selected_path = local_path;
        if (file == 0) {
            if (!have_global)
                return -BOOK8_EINVAL;
            file = fs_open_any(global_path);
            selected_path = global_path;
        }
        if (file == 0)
            return -BOOK8_ENOENT;
        status = elf_inspect(file, img);
        if (status == 0 && img->type != ET_DYN)
            status = -BOOK8_ENOEXEC;
        if (status == 0) {
            module = &task->modules[task->module_count++];
            zero_bytes(module, sizeof(*module));
            if (!copy_path(module->path, selected_path))
                status = -BOOK8_EINVAL;
        } else {
            module = 0;
        }
        if (status == 0)
            status = module_choose_bias(task, img, &bias);
        if (status == 0) {
            module->load_bias = bias;
            status = elf_load_into(task, file, img, bias);
            if (status == -BOOK8_ENOMEM)
                status = -BOOK8_EINVAL;
        }
        if (status == 0)
            status = module_load_exports(task, module, file, img);
        fs_close(file);
        if (status != 0)
            return status;
    }
    return 0;
}

static long resolve_image_imports(struct exec_task *task, file_handle_t *file,
    const struct elf_image *img, int32_t source_module, uint16_t *edges,
    bool *has_imports)
{
    for (uint32_t i = 0u; i < img->imports_count; ++i) {
        b8_sym_import_t imported;
        const b8_sym_export_t *provider = 0;
        uint32_t provider_index = UINT32_MAX;
        uint64_t offset = img->imports_offset
            + (uint64_t)i * sizeof(imported);
        if (!elf_read_at(file, offset, &imported, sizeof(imported)))
            return -BOOK8_ENOEXEC;
        *has_imports = true;
        for (uint32_t m = 0u; m < task->module_count; ++m) {
            const struct exec_module *module = &task->modules[m];
            for (uint32_t e = 0u; e < module->export_count; ++e) {
                const b8_sym_export_t *candidate = &module->exports[e];
                if (!elf_string_equals(imported.name, candidate->name))
                    continue;
                if (provider != 0)
                    return -BOOK8_EINVAL;
                provider = candidate;
                provider_index = m;
            }
        }
        if (provider == 0)
            return -BOOK8_ENOENT;
        if (provider->version_major != imported.version_major
            || provider->version_minor < imported.version_minor
            || provider->ptr == 0)
            return -BOOK8_EINVAL;
        if (source_module >= 0) {
            if ((uint32_t)source_module >= task->module_count
                || provider_index >= EXEC_MAX_MODULES)
                return -BOOK8_EINVAL;
            edges[source_module] |= (uint16_t)(1u << provider_index);
        }
    }
    return 0;
}

static bool elf_read_string(file_handle_t *file, const Elf64_Shdr *strings,
    uint32_t name_offset, char name[B8_SYM_NAME_SIZE])
{
    if (name_offset >= strings->sh_size)
        return false;
    for (uint32_t i = 0u; i < B8_SYM_NAME_SIZE; ++i) {
        uint8_t character;
        uint64_t offset = (uint64_t)strings->sh_offset + name_offset + i;
        if ((uint64_t)name_offset + i >= strings->sh_size
            || !elf_read_at(file, offset, &character, sizeof(character)))
            return false;
        name[i] = (char)character;
        if (character == 0u)
            return i != 0u;
    }
    return false;
}

static long elf_find_import(file_handle_t *file, const struct elf_image *img,
    const char *name, b8_sym_import_t *imported)
{
    for (uint32_t i = 0u; i < img->imports_count; ++i) {
        b8_sym_import_t candidate;
        uint64_t offset = img->imports_offset
            + (uint64_t)i * sizeof(candidate);
        if (!elf_read_at(file, offset, &candidate, sizeof(candidate)))
            return -BOOK8_ENOEXEC;
        if (!elf_string_equals(candidate.name, name))
            continue;
        if (imported->name[0] != '\0')
            return -BOOK8_EINVAL;
        *imported = candidate;
    }
    return imported->name[0] == '\0' ? -BOOK8_ENOENT : 0;
}

static long elf_find_provider(struct exec_task *task,
    const b8_sym_import_t *imported, const b8_sym_export_t **provider_out)
{
    const b8_sym_export_t *provider = 0;
    for (uint32_t m = 0u; m < task->module_count; ++m) {
        const struct exec_module *module = &task->modules[m];
        for (uint32_t e = 0u; e < module->export_count; ++e) {
            const b8_sym_export_t *candidate = &module->exports[e];
            if (!elf_string_equals(imported->name, candidate->name))
                continue;
            if (provider != 0)
                return -BOOK8_EINVAL;
            provider = candidate;
        }
    }
    if (provider == 0)
        return -BOOK8_ENOENT;
    if (provider->version_major != imported->version_major
        || provider->version_minor < imported->version_minor
        || provider->ptr == 0)
        return -BOOK8_EINVAL;
    *provider_out = provider;
    return 0;
}

static long elf_image_section(file_handle_t *file,
    const struct elf_image *img, uint32_t index, Elf64_Shdr *section)
{
    uint64_t offset;
    if (index >= img->section_count
        || index > (UINT64_MAX - img->section_headers_offset)
            / sizeof(Elf64_Shdr))
        return -BOOK8_ENOEXEC;
    offset = img->section_headers_offset
        + (uint64_t)index * sizeof(Elf64_Shdr);
    return elf_read_at(file, offset, section, sizeof(*section))
        ? 0 : -BOOK8_ENOEXEC;
}

static long elf_resolve_relocation_symbol(struct exec_task *task,
    file_handle_t *file, const struct elf_image *img,
    const Elf64_Shdr *symbols, const Elf64_Shdr *strings,
    uint32_t symbol_index, uint64_t load_bias, uint64_t *value_out)
{
    Elf64_Sym symbol;
    char name[B8_SYM_NAME_SIZE];
    uint64_t offset = symbols->sh_offset
        + (uint64_t)symbol_index * sizeof(symbol);
    if (!elf_read_at(file, offset, &symbol, sizeof(symbol)))
        return -BOOK8_ENOEXEC;
    if (symbol.st_shndx == SHN_ABS) {
        *value_out = symbol.st_value;
        return 0;
    }
    if (symbol.st_shndx == SHN_COMMON)
        return -BOOK8_ENOEXEC;
    if (symbol.st_shndx != SHN_UNDEF) {
        uint64_t value = symbol.st_value;
        if (symbol.st_shndx >= img->section_count
            || value > UINT64_MAX - load_bias
            || !elf_local_symbol_value_valid(img, &symbol)) {
            return -BOOK8_ENOEXEC;
        }
        *value_out = value + load_bias;
        return 0;
    }
    if ((symbol.st_info >> 4u) != STB_GLOBAL
        && (symbol.st_info >> 4u) != STB_WEAK) {
        return -BOOK8_ENOEXEC;
    }
    if (!elf_read_string(file, strings, symbol.st_name, name))
        return -BOOK8_ENOEXEC;
    b8_sym_import_t imported = {0};
    long status = elf_find_import(file, img, name, &imported);
    if (status != 0)
        return status;
    const b8_sym_export_t *provider = 0;
    status = elf_find_provider(task, &imported, &provider);
    if (status != 0)
        return status;
    uint32_t elf_type = symbol.st_info & 0x0fu;
    if ((elf_type == STT_FUNC && provider->type != B8_SYM_FUNC)
        || (elf_type == STT_OBJECT && provider->type != B8_SYM_DATA)
        || (elf_type != STT_NOTYPE && elf_type != STT_FUNC
            && elf_type != STT_OBJECT)) {
        return -BOOK8_EINVAL;
    }
    *value_out = (uint64_t)(uintptr_t)provider->ptr;
    return 0;
}

static bool elf_add_signed(uint64_t base, int64_t addend, uint64_t *value)
{
    if (addend >= 0) {
        uint64_t positive = (uint64_t)addend;
        if (base > UINT64_MAX - positive)
            return false;
        *value = base + positive;
    } else {
        uint64_t negative = (uint64_t)(-(addend + 1)) + 1u;
        if (base < negative)
            return false;
        *value = base - negative;
    }
    return true;
}

static bool elf_write_relocation(struct exec_task *task, uint64_t target,
    uint64_t value, uint32_t width)
{
    for (uint32_t i = 0u; i < width; ++i) {
        uint64_t address = target + i;
        uint8_t *frame = map_page(task, address & ~(PAGE - 1u), false);
        if (frame == 0)
            return false;
        frame[address & (PAGE - 1u)] = (uint8_t)(value >> (i * 8u));
    }
    return true;
}

static long elf_apply_relocation_section(struct exec_task *task,
    file_handle_t *file, const struct elf_image *img,
    const Elf64_Shdr *relocations, uint64_t load_bias)
{
    Elf64_Shdr symbols, strings;
    long status = elf_image_section(file, img, relocations->sh_link, &symbols);
    if (status != 0)
        return status;
    status = elf_image_section(file, img, symbols.sh_link, &strings);
    if (status != 0)
        return status;
    uint64_t count = relocations->sh_size / sizeof(Elf64_Rela);
    for (uint64_t i = 0u; i < count; ++i) {
        Elf64_Rela rela;
        uint32_t type, symbol_index, width;
        uint64_t target, symbol_value = 0u, result = 0u;
        uint64_t offset = relocations->sh_offset + i * sizeof(rela);
        if (!elf_read_at(file, offset, &rela, sizeof(rela)))
            return -BOOK8_ENOEXEC;
        type = ELF64_R_TYPE(rela.r_info);
        symbol_index = ELF64_R_SYM(rela.r_info);
        width = elf_relocation_width(type);
        if (type == R_X86_64_NONE)
            continue;
        if (width == UINT32_MAX || rela.r_offset > UINT64_MAX - load_bias)
            return -BOOK8_ENOTSUP;
        target = rela.r_offset + load_bias;
        if (target < USER_BASE || target > USER_LIMIT - width
            || !elf_relocation_target_valid(img->phdr, img->phnum,
                rela.r_offset, width)) {
            return -BOOK8_ENOEXEC;
        }
        if (type == R_X86_64_RELATIVE) {
            if (!elf_add_signed(load_bias, rela.r_addend, &result))
                return -BOOK8_ENOEXEC;
        } else {
            status = elf_resolve_relocation_symbol(task, file, img,
                &symbols, &strings, symbol_index, load_bias, &symbol_value);
            if (status != 0)
                return status;
            switch (type) {
            case R_X86_64_64:
                if (!elf_add_signed(symbol_value, rela.r_addend, &result))
                    return -BOOK8_ENOEXEC;
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                uint64_t sum, distance;
                if (!elf_add_signed(symbol_value, rela.r_addend, &sum))
                    return -BOOK8_ENOEXEC;
                if (sum >= target) {
                    distance = sum - target;
                    if (distance > INT32_MAX)
                        return -BOOK8_ENOEXEC;
                    result = (uint32_t)distance;
                } else {
                    distance = target - sum;
                    if (distance > (uint64_t)INT32_MAX + 1u)
                        return -BOOK8_ENOEXEC;
                    result = (uint32_t)(-(int64_t)distance);
                }
                break;
            }
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT:
                result = symbol_value;
                break;
            default:
                return -BOOK8_ENOTSUP;
            }
        }
        if (!elf_write_relocation(task, target, result, width))
            return -BOOK8_ENOMEM;
    }
    return 0;
}

static long elf_apply_relocations(struct exec_task *task, file_handle_t *file,
    const struct elf_image *img, uint64_t load_bias)
{
    for (uint32_t i = 0u; i < img->section_count; ++i) {
        Elf64_Shdr section;
        long status = elf_image_section(file, img, i, &section);
        if (status != 0)
            return status;
        if (section.sh_type != SHT_RELA || section.sh_size == 0u
            || (section.sh_flags & SHF_ALLOC) == 0u)
            continue;
        status = elf_apply_relocation_section(task, file, img, &section,
            load_bias);
        if (status != 0)
            return status;
    }
    return 0;
}

static bool module_graph_visit(uint32_t node, uint16_t edges[EXEC_MAX_MODULES],
    uint8_t state[EXEC_MAX_MODULES])
{
    if (state[node] == 1u)
        return false;
    if (state[node] == 2u)
        return true;
    state[node] = 1u;
    for (uint32_t next = 0u; next < EXEC_MAX_MODULES; ++next) {
        if ((edges[node] & (uint16_t)(1u << next)) != 0u
            && !module_graph_visit(next, edges, state))
            return false;
    }
    state[node] = 2u;
    return true;
}

static long resolve_loaded_imports(struct exec_task *task,
    file_handle_t *main_file, const struct elf_image *main_img)
{
    uint16_t edges[EXEC_MAX_MODULES] = {0};
    uint8_t state[EXEC_MAX_MODULES] = {0};
    bool has_imports = false;
    long status = resolve_image_imports(task, main_file, main_img, -1,
        edges, &has_imports);
    if (status != 0)
        return status;

    for (uint32_t i = 0u; i < task->module_count; ++i) {
        file_handle_t *file = fs_open_any(task->modules[i].path);
        if (file == 0)
            return -BOOK8_ENOENT;
        status = elf_inspect(file, &module_image_workspace);
        if (status == 0)
            status = resolve_image_imports(task, file,
                &module_image_workspace, (int32_t)i, edges, &has_imports);
        fs_close(file);
        if (status != 0)
            return status;
    }
    for (uint32_t i = 0u; i < task->module_count; ++i) {
        if (!module_graph_visit(i, edges, state))
            return -BOOK8_EINVAL;
    }
    status = elf_apply_relocations(task, main_file, main_img, 0u);
    if (status != 0)
        return status;
    for (uint32_t i = 0u; i < task->module_count; ++i) {
        file_handle_t *file = fs_open_any(task->modules[i].path);
        if (file == 0)
            return -BOOK8_ENOENT;
        status = elf_inspect(file, &module_image_workspace);
        if (status == 0)
            status = elf_apply_relocations(task, file,
                &module_image_workspace, task->modules[i].load_bias);
        fs_close(file);
        if (status != 0)
            return status;
    }
    return 0;
}

static long prepare_executable_modules(struct exec_task *task,
    const char *path, file_handle_t *main_file,
    const struct elf_image *main_img)
{
    char root[B8PKG_MAX_PATH_SIZE + 1u];
    struct b8pkg_manifest *manifest = &manifest_workspace;
    long status;

    zero_bytes(manifest, sizeof(*manifest));
    status = package_manifest_load(path, root, manifest);
    if (status < 0)
        return status;
    if (status > 0) {
        status = load_package_libraries(task, root, manifest);
        if (status != 0)
            return status;
    }
    return resolve_loaded_imports(task, main_file, main_img);
}

long exec_run(const char *path, int argc, char **argv)
{
    struct elf_image *img;
    static struct elf_image image;
    struct exec_task *task;
    file_handle_t *file;
    uint64_t rsp;
    long status;

    if (current != 0 || path == 0) {
        return -BOOK8_EINVAL;
    }
    task = task_reserve();
    if (task == 0) {
        return -BOOK8_EMFILE;
    }
    file = fs_open_any(path);
    if (file == 0) {
        task_release(task);
        return -BOOK8_ENOENT;
    }
    img = &image;
    status = elf_inspect(file, img);
    if (status == 0) {
        status = elf_require_executable(img);
    }
    if (status != 0) {
        fs_close(file);
        task_release(task);
        return status;
    }
    status = elf_load_into(task, file, img, 0u);
    if (status == 0)
        status = prepare_executable_modules(task, path, file, img);
    fs_close(file);
    if (status == 0) {
        status = build_stack(task, argc, argv, &rsp);
    }
    if (status != 0) {
        task_release(task);
        return status;
    }
    task->pid = next_pid++;
    struct book8_session_info session;
    if (book8_session_current(&session)) {
        task->uid = session.uid;
        task->session_token = session.token;
    } else {
        task->uid = UINT32_MAX;
        task->session_token = 0u;
    }
    task->state = TASK_RUNNING;
    task->entry = img->entry;
    task->user_rsp = rsp;
    set_name(task, path);
    current = task;
    scheduler_quantum = SCHEDULER_QUANTUM_TICKS;
    if (book8_read_cr3() != task->cr3)
        book8_write_cr3(task->cr3);

    status = syscall_run_user(task);

    task->exit_status = status;
    task->state = TASK_TERMINATED;
    task_release(task);
    current = 0;
    return status;
}

long exec_sys_open(uint64_t path, uint64_t flags)
{
    file_handle_t *file;
    long len;
    uint32_t i;

    if (current == 0) {
        return -BOOK8_ENOSYS;
    }
    if (!book8_session_authorize(current->uid, current->session_token,
            BOOK8_SESSION_READ)) {
        return -BOOK8_EPERM;
    }
    if (flags != 0u) {
        return -BOOK8_EINVAL; /* read-only file system */
    }
    len = user_read_string(path_store, sizeof(path_store), path);
    if (len < 0) {
        return len;
    }
    for (i = 0; i < EXEC_MAX_FDS; ++i) {
        if (current->fds[i] == 0) {
            file = fs_open(path_store);
            if (file == 0) {
                return -BOOK8_ENOENT;
            }
            current->fds[i] = file;
            return (long)i + EXEC_FD_BASE;
        }
    }
    return -BOOK8_EMFILE;
}

static file_handle_t *fd_lookup(uint64_t fd)
{
    if (current == 0 || fd < EXEC_FD_BASE || fd >= EXEC_FD_BASE + EXEC_MAX_FDS) {
        return 0;
    }
    return current->fds[fd - EXEC_FD_BASE];
}

long exec_sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    file_handle_t *file = fd_lookup(fd);
    int32_t got;
    uint8_t *dst = (uint8_t *)buf;
    int32_t i;

    if (file == 0) {
        return -BOOK8_EBADF;
    }
    if (len > sizeof(bounce)) {
        len = sizeof(bounce);
    }
    if (!exec_user_range_ok(buf, len, true)) {
        return -BOOK8_EFAULT;
    }
    /* Disk DMA needs a physical address, so go through a kernel bounce buffer. */
    got = fs_read(file, bounce, (uint32_t)len);
    if (got < 0) {
        return -BOOK8_EINVAL;
    }
    for (i = 0; i < got; ++i) {
        dst[i] = bounce[i];
    }
    return got;
}

long exec_sys_close(uint64_t fd)
{
    file_handle_t *file = fd_lookup(fd);

    if (file == 0) {
        return -BOOK8_EBADF;
    }
    fs_close(file);
    current->fds[fd - EXEC_FD_BASE] = 0;
    return 0;
}

long exec_sys_execve(struct syscall_frame *frame, uint64_t path, uint64_t argv)
{
    static struct elf_image image;
    file_handle_t *file;
    struct exec_task *staged;
    uint32_t argc = 0u, used = 0u;
    uint64_t rsp;
    long status;

    if (current == 0) {
        return -BOOK8_ENOSYS;
    }
    status = user_read_string(path_store, sizeof(path_store), path);
    if (status < 0) {
        return status;
    }
    while (argv != 0) {
        uint64_t ptr;
        long len;

        if (argc > (UINT64_MAX - argv) / sizeof(uint64_t)) {
            return -BOOK8_EFAULT;
        }
        if (!user_read_bytes(&ptr, argv + argc * 8u, 8u)) {
            return -BOOK8_EFAULT;
        }
        if (ptr == 0u) {
            break;
        }
        if (argc >= EXEC_MAX_ARGS) {
            return -BOOK8_EINVAL;
        }
        len = user_read_string(arg_store + used, EXEC_ARG_BYTES - used, ptr);
        if (len < 0) {
            return len;
        }
        arg_ptrs[argc++] = arg_store + used;
        used += (uint32_t)len + 1u;
    }
    file = fs_open(path_store);
    if (file == 0) {
        file = fs_open_any(path_store);
    }
    if (file == 0) {
        return -BOOK8_ENOENT;
    }
    status = elf_inspect(file, &image);
    if (status != 0) {
        fs_close(file);
        return status; /* old image untouched */
    }
    status = elf_require_executable(&image);
    if (status != 0) {
        fs_close(file);
        return status;
    }

    staged = task_reserve();
    if (staged == 0) {
        fs_close(file);
        return -BOOK8_EMFILE;
    }
    staged->pid = current->pid;
    staged->uid = current->uid;
    staged->session_token = current->session_token;
    status = elf_load_into(staged, file, &image, 0u);
    if (status == 0)
        status = prepare_executable_modules(staged, path_store, file, &image);
    fs_close(file);
    if (status == 0) {
        status = build_stack(staged, (int)argc, arg_ptrs, &rsp);
    }
    if (status != 0) {
        task_release(staged);
        return status;
    }

    close_fds(current);
    modules_destroy(current);
    space_destroy(current);
    current->cr3 = staged->cr3;
    current->user_pd = staged->user_pd;
    current->page_pool_used = staged->page_pool_used;
    current->image_high_water = staged->image_high_water;
    current->address_space_slot = staged->address_space_slot;
    current->module_count = staged->module_count;
    for (uint32_t i = 0u; i < staged->module_count; ++i) {
        current->modules[i] = staged->modules[i];
        zero_bytes(&staged->modules[i], sizeof(staged->modules[i]));
    }
    staged->cr3 = 0u;
    staged->user_pd = 0;
    staged->page_pool_used = 0u;
    staged->image_high_water = 0u;
    staged->address_space_slot = UINT32_MAX;
    staged->module_count = 0u;
    staged->state = TASK_FREE;
    if (book8_read_cr3() != current->cr3)
        book8_write_cr3(current->cr3);
    set_name(current, path_store);
    current->entry = image.entry;
    current->user_rsp = rsp;

    uint64_t user_if = frame->r11 & 0x200u;
    zero_bytes(frame, sizeof(*frame));
    frame->rcx = image.entry;
    frame->r11 = user_if | 0x2u;
    frame->rsp = rsp;
    return 0;
}

uint32_t exec_current_pid(void)
{
    return current != 0 ? current->pid : 1u;
}
