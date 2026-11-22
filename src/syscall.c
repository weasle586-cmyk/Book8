#include "syscall.h"
#include "../kernel/exec.h"
#include "../kernel/hwdiag.h"

#define USER_TOP      0x00007fffffffffffULL
#define RFLAGS_MASK   (0x200u | 0x400u | 0x100u | 0x4000u | 0x40000u) /* IF DF TF NT AC */
#define SYSCALL_IO_MAX_BYTES 65536u

/* Provided by kernel.c. */
long book8_console_write(const char *buf, unsigned long len);
/* Optional hooks; defaults used when the kernel does not define them. */
__attribute__((weak)) long book8_console_read(char *buf, unsigned long len);
__attribute__((weak)) void book8_task_exit(long status);
__attribute__((weak)) long book8_gui_utility_request(
    const struct book8_gui_utility_request *request);

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct syscall_percpu percpu;
static volatile struct tss64 tss;
static uint64_t gdt[8] __attribute__((aligned(16)));
static uint8_t bootstrap_kernel_stack[EXEC_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));
static volatile int ring3_active;

_Static_assert(sizeof(struct tss64) == 104u, "64-bit TSS layout");
_Static_assert(GDT_TSS + 16u <= sizeof(gdt), "TSS descriptor spans two GDT slots");
_Static_assert(__builtin_offsetof(struct syscall_percpu, kernel_rsp) == 0,
               "syscall kernel stack offset");
_Static_assert(__builtin_offsetof(struct syscall_percpu, user_rsp) == 8,
               "syscall user stack offset");
_Static_assert(__builtin_offsetof(struct syscall_percpu, caller_rsp) == 16,
               "syscall caller stack offset");
_Static_assert(__builtin_offsetof(struct exec_task, kernel_stack) % 16 == 0,
               "task kernel stack alignment");
_Static_assert(EXEC_KERNEL_STACK_SIZE % 16 == 0,
               "task kernel stack top alignment");

/* User stack page, placed by linker.ld in the U/S=1 .user_stack pages. */
static uint8_t user_stack[4096] __attribute__((aligned(4096), section(".user.stack"), used));
/* Window page table: replaces one boot 2 MiB supervisor page so individual
 * 4K pages can carry different U/S bits. */
static uint64_t user_pt[512] __attribute__((aligned(4096)));
static int user_paging_done;

extern uint64_t boot_pml4[], boot_pdpt[], boot_pd0[];
extern char __user_text_start[], __user_text_end[];
extern char __user_data_start[], __user_data_end[];
extern char __user_stack_start[], __user_stack_end[];
void user_smoke_main(void);
extern uint64_t user_smoke_delay;

#define PTE_P   0x1ull
#define PTE_RW  0x2ull
#define PTE_US  0x4ull
#define PTE_PS  0x80ull
#define PTE_ADDR_MASK 0x000ffffffffff000ull

typedef long (*syscall_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t);

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Accepts a user buffer only if it is fully inside memory the user may touch:
 * the loaded-program window (page-table checked) or the smoke-test pages. */
static int user_range_ok(uint64_t ptr, uint64_t len, int write)
{
    uint64_t lo = write ? (uint64_t)__user_data_start : (uint64_t)__user_text_start;
    uint64_t hi = (uint64_t)__user_stack_end;

    if (exec_user_range_ok(ptr, len, write != 0))
        return 1;
    if (exec_has_user_space())
        return 0;
    if (ptr < lo || ptr >= hi || len > hi - ptr)
        return 0;
#ifndef BOOK8_HOST_TEST
    if (!user_paging_done)
        return 0;
    uint64_t window = (uint64_t)__user_text_start & ~0x1fffffull;
    for (uint64_t page = ptr & ~0xfffull; page < ptr + len;
         page += 0x1000u) {
        uint64_t index = (page - window) >> 12u;
        if (page < window || index >= 512u
            || (user_pt[index] & (PTE_P | PTE_US))
                != (PTE_P | PTE_US)
            || (write && (user_pt[index] & PTE_RW) == 0u))
            return 0;
    }
#endif
    return 1;
}

static long sys_read_handler(uint64_t fd, uint64_t buf, uint64_t len,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (fd >= EXEC_FD_BASE)
        return exec_sys_read(fd, buf, len);
    if (fd != 0)
        return -BOOK8_EBADF;
    if (len > SYSCALL_IO_MAX_BYTES)
        return -BOOK8_EINVAL;
    if (!user_range_ok(buf, len, 1))
        return -BOOK8_EFAULT;
    if (book8_console_read == 0)
        return -BOOK8_ENOSYS;
    return book8_console_read((char *)buf, len);
}

static long sys_write_handler(uint64_t fd, uint64_t buf, uint64_t len,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (fd != 1 && fd != 2)
        return -BOOK8_EBADF;
    if (len > SYSCALL_IO_MAX_BYTES)
        return -BOOK8_EINVAL;
    if (!user_range_ok(buf, len, 0))
        return -BOOK8_EFAULT;
    return book8_console_write((const char *)buf, len);
}

static long sys_open_handler(uint64_t path, uint64_t flags, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    return exec_sys_open(path, flags);
}

static long sys_close_handler(uint64_t fd, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return exec_sys_close(fd);
}

static long sys_getpid_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return exec_current_pid();
}

static long sys_fb_info_handler(uint64_t info, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!user_range_ok(info, sizeof(struct book8_fb_info), 1))
        return -BOOK8_EFAULT;
    return book8_framebuffer_info((struct book8_fb_info *)info);
}

static long sys_draw_pixel_handler(uint64_t x, uint64_t y, uint64_t rgb,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (rgb > UINT32_MAX)
        return -BOOK8_EINVAL;
    return book8_framebuffer_draw_pixel(x, y, (uint32_t)rgb);
}

void syscall_kill_current(long status)
{
    if (ring3_active) {
        ring3_active = 0;
        syscall_return_to_kernel(status);
    }
    for (;;)
        __asm__ volatile ("cli; hlt");
}

static long sys_exit_handler(uint64_t status, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!ring3_active && book8_task_exit)
        book8_task_exit((long)status);
    syscall_kill_current((long)status);
    __builtin_unreachable();
}

static long sys_hwdiag_handler(uint64_t operation, uint64_t data, uint64_t size,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    uint64_t expected;
    switch (operation) {
    case BOOK8_HWDIAG_INFO: expected = sizeof(struct book8_hwdiag_info); break;
    case BOOK8_HWDIAG_READ32: expected = sizeof(struct book8_hwdiag_read32); break;
    case BOOK8_HWDIAG_REPORT: expected = sizeof(struct book8_hwdiag_report); break;
    default: return -BOOK8_EINVAL;
    }
    if (size != expected)
        return -BOOK8_EINVAL;
    if (!user_range_ok(data, size, operation != BOOK8_HWDIAG_REPORT))
        return -BOOK8_EFAULT;
    /* Copy through aligned kernel locals; the ABI permits unaligned buffers. */
    uint8_t *bytes = (uint8_t *)data;
    if (operation == BOOK8_HWDIAG_INFO) {
        struct book8_hwdiag_info info;
        long result = hwdiag_info(&info);
        for (unsigned i = 0; i < sizeof(info); ++i)
            bytes[i] = ((const uint8_t *)&info)[i];
        return result;
    }
    if (operation == BOOK8_HWDIAG_READ32) {
        struct book8_hwdiag_read32 request;
        for (unsigned i = 0; i < sizeof(request); ++i)
            ((uint8_t *)&request)[i] = bytes[i];
        long result = hwdiag_read32(&request);
        if (result == 0) {
            for (unsigned i = 0; i < sizeof(request); ++i)
                bytes[i] = ((const uint8_t *)&request)[i];
        }
        return result;
    }
    struct book8_hwdiag_report report;
    for (unsigned i = 0; i < sizeof(report); ++i)
        ((uint8_t *)&report)[i] = bytes[i];
    return hwdiag_report(&report);
}

static long sys_gui_utility_handler(uint64_t data, uint64_t size, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (size != sizeof(struct book8_gui_utility_request)) { return -BOOK8_EINVAL; }
    if (!user_range_ok(data, size, 0)) { return -BOOK8_EFAULT; }
    struct book8_gui_utility_request request;
    for (uint32_t i = 0u; i < sizeof(request); ++i) {
        ((uint8_t *)&request)[i] = ((const uint8_t *)data)[i];
    }
    if (request.kind != BOOK8_GUI_HEXVIEW && request.kind != BOOK8_GUI_CODEEDIT
        && request.kind != BOOK8_GUI_NETSHARE
        && request.kind != BOOK8_GUI_PAINTBOOK
        && request.kind != BOOK8_GUI_BROWSER
        && request.kind != BOOK8_GUI_NVDIAG
        && request.kind != BOOK8_GUI_PLAYER) {
        return -BOOK8_EINVAL;
    }
    uint32_t n = 0u;
    while (n < sizeof(request.path) && request.path[n] != '\0') { ++n; }
    if (n == sizeof(request.path)) { return -BOOK8_EINVAL; }
    if (book8_gui_utility_request == NULL) { return -BOOK8_ENOSYS; }
    return book8_gui_utility_request(&request);
}

static const syscall_fn syscall_table[SYS_MAX] = {
    [SYS_read]   = sys_read_handler,
    [SYS_write]  = sys_write_handler,
    [SYS_open]   = sys_open_handler,
    [SYS_close]  = sys_close_handler,
    [SYS_getpid] = sys_getpid_handler,
    [SYS_fb_info] = sys_fb_info_handler,
    [SYS_draw_pixel] = sys_draw_pixel_handler,
    [SYS_hwdiag] = sys_hwdiag_handler,
    [SYS_gui_utility] = sys_gui_utility_handler,
    [SYS_exit]   = sys_exit_handler,
};

void syscall_dispatch(struct syscall_frame *f)
{
    uint64_t num = f->rax;
    long ret = -BOOK8_ENOSYS;

    if (num == SYS_execve)
        ret = exec_sys_execve(f, f->rdi, f->rsi); /* rewrites the frame on success */
    else if (num < SYS_MAX && syscall_table[num])
        ret = syscall_table[num](f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    f->rax = (uint64_t)ret;
}

void syscall_select_task(struct exec_task *task)
{
    percpu.kernel_rsp = (uint64_t)&task->kernel_stack[EXEC_KERNEL_STACK_SIZE];
    percpu.caller_rsp = &task->caller_rsp;
    tss.rsp[0] = percpu.kernel_rsp;
}

static long syscall_run_user_flags(struct exec_task *task, uint64_t user_rflags)
{
    long status;

    if (task == NULL)
        return -BOOK8_EINVAL;
    syscall_select_task(task);
    ring3_active = 1;
    status = syscall_enter_user(task->entry, task->user_rsp, &task->caller_rsp,
                                user_rflags);
    task->caller_rsp = 0u;
    percpu.kernel_rsp = (uint64_t)&bootstrap_kernel_stack[sizeof(bootstrap_kernel_stack)];
    percpu.caller_rsp = 0;
    tss.rsp[0] = percpu.kernel_rsp;
    return status;
}

long syscall_run_user(struct exec_task *task)
{
    return syscall_run_user_flags(task, 0x202u);
}

static void gdt_setup(void)
{
    uint64_t tss_base = (uint64_t)&tss;
    uint64_t tss_limit = sizeof(tss) - 1u;
    struct gdt_ptr ptr;

    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL; /* 0x08 kernel code64 */
    gdt[2] = 0x00cf92000000ffffULL; /* 0x10 kernel data */
    gdt[3] = 0x00cffa000000ffffULL; /* 0x18 user code32 (SYSRET base) */
    gdt[4] = 0x00cff2000000ffffULL; /* 0x20 user data */
    gdt[5] = 0x00affa000000ffffULL; /* 0x28 user code64 */
    gdt[6] = (tss_limit & 0xffffu) | ((tss_base & 0xffffffu) << 16) |
             (0x89ULL << 40) | (((tss_limit >> 16) & 0xfu) << 48) |
             (((tss_base >> 24) & 0xffu) << 56); /* 0x30 available TSS */
    gdt[7] = tss_base >> 32;

    tss.iomap_base = sizeof(tss);

    ptr.limit = sizeof(gdt) - 1u;
    ptr.base = (uint64_t)gdt;
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "ltr %3"
        :: "m"(ptr), "i"(GDT_KCODE), "i"(GDT_KDATA), "r"((uint16_t)GDT_TSS)
        : "rax", "memory");
}

static int user_paging_init(void)
{
    uint64_t first = (uint64_t)__user_text_start;
    uint64_t last = (uint64_t)__user_stack_end;
    uint64_t window = first & ~0x1fffffull;
    uint64_t gb = first >> 30;
    uint64_t *pde;
    uint64_t addr, cr3;
    unsigned i;

    if (user_paging_done)
        return 0;
    /* All user pages must share one boot 2 MiB page inside the first 4 GiB. */
    if (gb > 3 || ((last - 1u) & ~0x1fffffull) != window)
        return -1;
    pde = &boot_pd0[gb * 512u + ((first >> 21) & 511u)];
    if (!(*pde & PTE_PS) || (*pde & PTE_ADDR_MASK) != window)
        return -1;

    for (i = 0; i < 512u; ++i)
        user_pt[i] = (window + (uint64_t)i * 4096u) | PTE_P | PTE_RW;
    /* Only the user pages get U/S=1; code is read-only, data and stack RW. */
    for (addr = (uint64_t)__user_text_start; addr < (uint64_t)__user_text_end; addr += 4096u)
        user_pt[(addr - window) >> 12] = addr | PTE_P | PTE_US;
    for (addr = (uint64_t)__user_data_start; addr < (uint64_t)__user_data_end; addr += 4096u)
        user_pt[(addr - window) >> 12] = addr | PTE_P | PTE_RW | PTE_US;
    for (addr = (uint64_t)__user_stack_start; addr < (uint64_t)__user_stack_end; addr += 4096u)
        user_pt[(addr - window) >> 12] = addr | PTE_P | PTE_RW | PTE_US;

    /* U/S is the AND across levels; intermediate entries must allow user,
     * the kernel's remaining 2 MiB leaf pages stay U/S=0. */
    boot_pml4[0] |= PTE_US;
    boot_pdpt[gb] |= PTE_US;
    *pde = (uint64_t)user_pt | PTE_P | PTE_RW | PTE_US;

    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    user_paging_done = 1;
    return 0;
}

long syscall_ring3_smoke(void)
{
    static struct exec_task smoke_task;

    if (user_paging_init() != 0)
        return -1;
    smoke_task.entry = (uint64_t)user_smoke_main;
    smoke_task.user_rsp = (uint64_t)&user_stack[sizeof(user_stack)] - 8u;
    return syscall_run_user(&smoke_task);
}

long syscall_scheduler_smoke(struct exec_task *task, uint64_t delay_ticks)
{
    long status;

    if (task == 0 || delay_ticks == 0u || user_paging_init() != 0)
        return -1;
    task->entry = (uint64_t)user_smoke_main;
    task->user_rsp = (uint64_t)&user_stack[sizeof(user_stack)] - 8u;
    user_smoke_delay = delay_ticks;
    status = syscall_run_user_flags(task, 0x202u);
    user_smoke_delay = 0u;
    return status;
}

void syscall_init(void)
{
    percpu.kernel_rsp =
        (uint64_t)&bootstrap_kernel_stack[sizeof(bootstrap_kernel_stack)];
    tss.rsp[0] = percpu.kernel_rsp;
    gdt_setup();

    /* GS_BASE stays 0; SWAPGS on entry swaps in &percpu. */
    wrmsr(MSR_KERNEL_GS, (uint64_t)&percpu);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u); /* SCE */
    wrmsr(MSR_STAR, ((uint64_t)GDT_UCODE32 << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_MASK);
}
