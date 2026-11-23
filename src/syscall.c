#include "syscall.h"
#include "../kernel/exec.h"
#include "../kernel/hwdiag.h"
#include "../kernel/sched.h"

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

/* Privilege state is per CPU. Each CPU owns its GDT, TSS and syscall scratch
 * block; the TSS is re-pointed at the incoming task's kernel stack on every
 * context switch, which is the x86-64 way to do TSS-based privilege handling.
 * Hardware task gates are deliberately not used: SYSCALL/SYSRET bypasses the
 * task backlink, so mixing the two would corrupt task state. */
static struct syscall_percpu percpu[BOOK8_SMP_MAX_CPUS];
static volatile struct tss64 tss[BOOK8_SMP_MAX_CPUS];
static uint64_t gdt[BOOK8_SMP_MAX_CPUS][8] __attribute__((aligned(16)));
static uint8_t bootstrap_kernel_stack[BOOK8_SMP_MAX_CPUS][EXEC_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));

/* Weak fallbacks keep the syscall unit linkable in hosted boundary tests that
 * do not include the SMP or exec modules. The kernel supplies strong versions. */
__attribute__((weak)) uint32_t book8_smp_current_cpu_index(void) { return 0u; }
__attribute__((weak)) void book8_sched_request_reschedule(uint32_t cpu_id)
{ (void)cpu_id; }
__attribute__((weak)) long exec_spawn_user(const char *path, uint64_t argv)
{ (void)path; (void)argv; return -BOOK8_ENOSYS; }

/* Bounded IPC queues. Port 1 is the kernel event port and is readable by any
 * task; other ports are owned by the pid that created them. */
struct ipc_message {
    uint32_t from_pid;
    uint32_t size;
    uint8_t data[BOOK8_IPC_MESSAGE_MAX];
};

struct ipc_port {
    uint32_t owner_pid;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    struct ipc_message messages[BOOK8_IPC_QUEUE_SIZE];
};

/* The kernel event port is permanently owned by the kernel and readable by
 * every task; no user syscalls may claim or close it. */
static struct ipc_port ipc_ports[BOOK8_IPC_PORT_COUNT] = {
    [BOOK8_IPC_KERNEL_PORT - 1u] = { .owner_pid = BOOK8_IPC_KERNEL_OWNER }
};
static volatile uint32_t ipc_lock;

_Static_assert(sizeof(struct tss64) == 104u, "64-bit TSS layout");
_Static_assert(GDT_TSS + 16u <= sizeof(gdt[0]), "TSS descriptor spans two GDT slots");
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
/* Frozen user/kernel ABI layouts. Assembly (syscall_entry.S) and user ELFs
 * depend on these sizes; a mismatch must fail the build, not boot silently. */
_Static_assert(sizeof(struct syscall_frame) == 16u * sizeof(uint64_t),
               "syscall frame register layout");
_Static_assert(sizeof(struct book8_fb_info) == 20u,
               "framebuffer info ABI layout");
_Static_assert(sizeof(struct book8_gui_utility_request) == 260u,
               "GUI utility request ABI layout");

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

static uint32_t syscall_current_cpu(void);

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

static uint64_t ipc_lock_enter(void)
{
    uint64_t flags = sched_irq_save();
    while (__atomic_exchange_n(&ipc_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}

static void ipc_lock_leave(uint64_t flags)
{
    __atomic_store_n(&ipc_lock, 0u, __ATOMIC_RELEASE);
    sched_irq_restore(flags);
}

#define IPC_BAD_INDEX 0xffffffffu

static uint32_t ipc_port_index(uint32_t port)
{
    if (port == 0u || port > BOOK8_IPC_PORT_COUNT)
        return IPC_BAD_INDEX;
    return port - 1u;
}

static bool ipc_port_readable(const struct ipc_port *port, uint32_t pid)
{
    return port->owner_pid == pid || port->owner_pid == 0u
        || port->owner_pid == BOOK8_IPC_KERNEL_OWNER;
}

static long ipc_queue_message(struct ipc_port *port, uint32_t from_pid,
    const uint8_t *data, uint32_t size)
{
    struct ipc_message *message;

    if (size > BOOK8_IPC_MESSAGE_MAX)
        return -BOOK8_EINVAL;
    if (port->count >= BOOK8_IPC_QUEUE_SIZE)
        return -BOOK8_EAGAIN;
    message = &port->messages[port->tail];
    message->from_pid = from_pid;
    message->size = size;
    for (uint32_t i = 0u; i < size; ++i)
        message->data[i] = data[i];
    port->tail = (port->tail + 1u) % BOOK8_IPC_QUEUE_SIZE;
    ++port->count;
    return 0;
}

static long ipc_dequeue_message(struct ipc_port *port, uint8_t *data,
    uint32_t capacity)
{
    struct ipc_message *message;
    uint32_t copy;

    if (port->count == 0u)
        return -BOOK8_EAGAIN;
    message = &port->messages[port->head];
    copy = message->size < capacity ? message->size : capacity;
    for (uint32_t i = 0u; i < copy; ++i)
        data[i] = message->data[i];
    port->head = (port->head + 1u) % BOOK8_IPC_QUEUE_SIZE;
    --port->count;
    return (long)message->size;
}

static long copy_user_path(char *dst, uint32_t capacity, uint64_t src)
{
    for (uint32_t i = 0u; i < capacity; ++i) {
        if (!user_range_ok(src + i, 1u, 0))
            return -BOOK8_EFAULT;
        dst[i] = *(const char *)(uintptr_t)(src + i);
        if (dst[i] == '\0')
            return (long)i;
    }
    return -BOOK8_EINVAL;
}

long book8_ipc_post(uint32_t port, uint32_t from_pid, const void *data,
    uint32_t size)
{
    uint32_t index = ipc_port_index(port);
    uint64_t flags;
    long result;

    if (index == IPC_BAD_INDEX || (size > 0u && data == 0))
        return -BOOK8_EINVAL;
    flags = ipc_lock_enter();
    result = ipc_queue_message(&ipc_ports[index], from_pid,
        (const uint8_t *)data, size);
    ipc_lock_leave(flags);
    return result;
}

long book8_ipc_get(uint32_t port, void *data, uint32_t capacity)
{
    uint32_t index = ipc_port_index(port);
    uint64_t flags;
    long result;

    if (index == IPC_BAD_INDEX || capacity > BOOK8_IPC_MESSAGE_MAX
        || (capacity > 0u && data == 0))
        return -BOOK8_EINVAL;
    flags = ipc_lock_enter();
    if (ipc_ports[index].count == 0u)
        result = -BOOK8_EAGAIN;
    else
        result = ipc_dequeue_message(&ipc_ports[index], (uint8_t *)data,
            capacity);
    ipc_lock_leave(flags);
    return result;
}

void book8_ipc_release_owner(uint32_t pid)
{
    uint64_t flags;

    if (pid == 0u || pid == BOOK8_IPC_KERNEL_OWNER)
        return;
    flags = ipc_lock_enter();
    for (uint32_t i = 0u; i < BOOK8_IPC_PORT_COUNT; ++i) {
        if (ipc_ports[i].owner_pid == pid) {
            ipc_ports[i].owner_pid = 0u;
            ipc_ports[i].head = 0u;
            ipc_ports[i].tail = 0u;
            ipc_ports[i].count = 0u;
        }
    }
    ipc_lock_leave(flags);
}

static long sys_ipc_open_handler(uint64_t port, uint64_t flags, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    uint32_t number = (uint32_t)port;
    uint32_t index = ipc_port_index(number);
    uint32_t pid = exec_current_pid();
    uint64_t irq_flags;
    struct ipc_port *entry;
    long result;

    if (index == IPC_BAD_INDEX || (flags & ~(uint64_t)(BOOK8_IPC_CREATE | BOOK8_IPC_EXCL)) != 0u)
        return -BOOK8_EINVAL;
    if (number == BOOK8_IPC_KERNEL_PORT)
        return (flags & BOOK8_IPC_CREATE) != 0u ? -BOOK8_EPERM : 0;

    irq_flags = ipc_lock_enter();
    entry = &ipc_ports[index];
    if ((flags & BOOK8_IPC_CREATE) != 0u) {
        if (entry->owner_pid != 0u && entry->owner_pid != pid)
            result = -BOOK8_EPERM;
        else if (entry->owner_pid == 0u) {
            entry->owner_pid = pid;
            result = 0;
        } else if ((flags & BOOK8_IPC_EXCL) != 0u) {
            result = -BOOK8_EEXIST;
        } else {
            result = 0;
        }
    } else if (entry->owner_pid == 0u) {
        result = -BOOK8_ENOENT;
    } else {
        result = entry->owner_pid == pid ? 0 : -BOOK8_EPERM;
    }
    ipc_lock_leave(irq_flags);
    return result;
}

static long sys_ipc_close_handler(uint64_t port, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    uint32_t number = (uint32_t)port;
    uint32_t index = ipc_port_index(number);
    uint64_t irq_flags;
    struct ipc_port *entry;
    long result;

    if (index == IPC_BAD_INDEX)
        return -BOOK8_EINVAL;
    if (number == BOOK8_IPC_KERNEL_PORT)
        return -BOOK8_EPERM;
    irq_flags = ipc_lock_enter();
    entry = &ipc_ports[index];
    if (entry->owner_pid == 0u || entry->owner_pid != exec_current_pid()) {
        result = -BOOK8_EPERM;
    } else {
        entry->owner_pid = 0u;
        entry->head = 0u;
        entry->tail = 0u;
        entry->count = 0u;
        result = 0;
    }
    ipc_lock_leave(irq_flags);
    return result;
}

static long sys_ipc_send_handler(uint64_t port, uint64_t data, uint64_t size,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    uint32_t index = ipc_port_index((uint32_t)port);
    uint64_t irq_flags;
    struct ipc_port *entry;
    uint8_t message[BOOK8_IPC_MESSAGE_MAX];
    long result;

    if (index == IPC_BAD_INDEX || size > sizeof(message))
        return -BOOK8_EINVAL;
    if (size > 0u) {
        if (!user_range_ok(data, size, 0))
            return -BOOK8_EFAULT;
        for (uint32_t i = 0u; i < (uint32_t)size; ++i)
            message[i] = ((const uint8_t *)(uintptr_t)data)[i];
    }
    irq_flags = ipc_lock_enter();
    entry = &ipc_ports[index];
    if (entry->owner_pid == 0u)
        result = -BOOK8_ENOENT;
    else
        result = ipc_queue_message(entry, exec_current_pid(), message,
            (uint32_t)size);
    ipc_lock_leave(irq_flags);
    return result;
}

static long sys_ipc_recv_handler(uint64_t port, uint64_t data, uint64_t capacity,
    uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    uint32_t index = ipc_port_index((uint32_t)port);
    uint32_t pid = exec_current_pid();
    uint64_t irq_flags;
    struct ipc_port *entry;
    uint8_t message[BOOK8_IPC_MESSAGE_MAX];
    long result;

    if (index == IPC_BAD_INDEX || capacity > sizeof(message)
        || (flags & ~(uint64_t)BOOK8_IPC_NOWAIT) != 0u)
        return -BOOK8_EINVAL;
    if (capacity > 0u && !user_range_ok(data, capacity, 1))
        return -BOOK8_EFAULT;

    irq_flags = ipc_lock_enter();
    entry = &ipc_ports[index];
    if (!ipc_port_readable(entry, pid)) {
        result = -BOOK8_EPERM;
    } else if (entry->count == 0u) {
        result = -BOOK8_EAGAIN;
    } else {
        result = ipc_dequeue_message(entry, message, (uint32_t)capacity);
    }
    ipc_lock_leave(irq_flags);

    if (result >= 0 && capacity > 0u) {
        uint32_t copy = (uint32_t)result < (uint32_t)capacity
            ? (uint32_t)result : (uint32_t)capacity;
        for (uint32_t i = 0u; i < copy; ++i)
            ((uint8_t *)(uintptr_t)data)[i] = message[i];
    }
    return result;
}

static long sys_spawn_handler(uint64_t path, uint64_t argv, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    char path_store[256];

    long len = copy_user_path(path_store, sizeof(path_store), path);
    if (len < 0)
        return len;
    return exec_spawn_user(path_store, argv);
}

static long sys_yield_handler(uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    book8_sched_request_reschedule(book8_smp_current_cpu_index());
    return 0;
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
    /* A valid caller stack means this CPU is running a Ring 3 task, including
     * a task resumed directly from a saved timer frame after preemption. */
    if (percpu[syscall_current_cpu()].caller_rsp != 0)
        syscall_return_to_kernel(status);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

static long sys_exit_handler(uint64_t status, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (percpu[syscall_current_cpu()].caller_rsp == 0 && book8_task_exit)
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
    [SYS_spawn]  = sys_spawn_handler,
    [SYS_yield]  = sys_yield_handler,
    [SYS_ipc_open] = sys_ipc_open_handler,
    [SYS_ipc_close] = sys_ipc_close_handler,
    [SYS_ipc_send] = sys_ipc_send_handler,
    [SYS_ipc_recv] = sys_ipc_recv_handler,
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

static uint32_t syscall_current_cpu(void)
{
    uint32_t cpu = book8_smp_current_cpu_index();
    return cpu < BOOK8_SMP_MAX_CPUS ? cpu : 0u;
}

void syscall_select_kernel_stack(uint32_t cpu_id, uint64_t rsp)
{
    if (cpu_id >= BOOK8_SMP_MAX_CPUS)
        cpu_id = 0u;
    percpu[cpu_id].kernel_rsp = rsp;
    percpu[cpu_id].caller_rsp = 0;
    tss[cpu_id].rsp[0] = rsp;
}

uint64_t syscall_current_kernel_stack(uint32_t cpu_id)
{
    if (cpu_id >= BOOK8_SMP_MAX_CPUS)
        cpu_id = 0u;
    return tss[cpu_id].rsp[0];
}

void syscall_select_task(struct exec_task *task)
{
    uint32_t cpu = syscall_current_cpu();
    uint64_t top = (uint64_t)&task->kernel_stack[EXEC_KERNEL_STACK_SIZE];

    percpu[cpu].kernel_rsp = top;
    percpu[cpu].caller_rsp = &task->caller_rsp;
    tss[cpu].rsp[0] = top;
}

static long syscall_run_user_flags(struct exec_task *task, uint64_t user_rflags)
{
    uint32_t cpu = syscall_current_cpu();
    uint64_t saved_rsp0 = tss[cpu].rsp[0];
    long status;

    if (task == NULL)
        return -BOOK8_EINVAL;
    syscall_select_task(task);
    status = syscall_enter_user(task->entry, task->user_rsp, &task->caller_rsp,
                                user_rflags);
    task->caller_rsp = 0u;
    syscall_select_kernel_stack(cpu, saved_rsp0);
    return status;
}

long syscall_run_user(struct exec_task *task)
{
    return syscall_run_user_flags(task, 0x202u);
}

static void gdt_setup(uint32_t cpu)
{
    uint64_t tss_base = (uint64_t)&tss[cpu];
    uint64_t tss_limit = sizeof(tss[cpu]) - 1u;
    struct gdt_ptr ptr;

    gdt[cpu][0] = 0;
    gdt[cpu][1] = 0x00af9a000000ffffULL; /* 0x08 kernel code64 */
    gdt[cpu][2] = 0x00cf92000000ffffULL; /* 0x10 kernel data */
    gdt[cpu][3] = 0x00cffa000000ffffULL; /* 0x18 user code32 (SYSRET base) */
    gdt[cpu][4] = 0x00cff2000000ffffULL; /* 0x20 user data */
    gdt[cpu][5] = 0x00affa000000ffffULL; /* 0x28 user code64 */
    gdt[cpu][6] = (tss_limit & 0xffffu) | ((tss_base & 0xffffffu) << 16) |
             (0x89ULL << 40) | (((tss_limit >> 16) & 0xfu) << 48) |
             (((tss_base >> 24) & 0xffu) << 56); /* 0x30 available TSS */
    gdt[cpu][7] = tss_base >> 32;

    tss[cpu].iomap_base = sizeof(tss[cpu]);

    ptr.limit = sizeof(gdt[cpu]) - 1u;
    ptr.base = (uint64_t)&gdt[cpu][0];
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

void syscall_cpu_init(uint32_t cpu_id)
{
    uint64_t stack_top;

    if (cpu_id >= BOOK8_SMP_MAX_CPUS)
        return;
    stack_top = (uint64_t)&bootstrap_kernel_stack[cpu_id][EXEC_KERNEL_STACK_SIZE];
    percpu[cpu_id].kernel_rsp = stack_top;
    tss[cpu_id].rsp[0] = stack_top;
    gdt_setup(cpu_id);

    /* The syscall MSRs are per CPU even though GS_BASE remains 0 in user mode.
     * SWAPGS on entry swaps in this CPU's per-task scratch block. */
    wrmsr(MSR_KERNEL_GS, (uint64_t)&percpu[cpu_id]);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u); /* SCE */
    wrmsr(MSR_STAR, ((uint64_t)GDT_UCODE32 << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_MASK);
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
    syscall_cpu_init(0u);
}
