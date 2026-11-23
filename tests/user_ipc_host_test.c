/* Host coverage for the user IPC ABI and the kernel event port.
 *
 * The real syscall_dispatch() is linked against small stubs so port ownership,
 * bounded copies, queue overflow and kernel -> user injection can be checked
 * without booting QEMU. */
#include "../src/syscall.h"
#include "../kernel/hwdiag.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_USER_BYTES 4096u

static uint8_t user_buffer[TEST_USER_BYTES] __attribute__((aligned(4096)));
static uint32_t test_pid;
static unsigned exit_calls;
static long exit_status;
static unsigned kernel_exit_calls;
static long kernel_exit_status;

/* user_range_ok() consults these labels in the kernel build. */
char smoke_pages[64];
__asm__(".globl __user_text_start\n.set __user_text_start, smoke_pages\n"
        ".globl __user_text_end\n.set __user_text_end, smoke_pages + 16\n"
        ".globl __user_data_start\n.set __user_data_start, smoke_pages + 16\n"
        ".globl __user_stack_end\n.set __user_stack_end, smoke_pages + 64\n");

bool exec_user_range_ok(uint64_t ptr, uint64_t len, bool write)
{
    uint64_t base = (uint64_t)(uintptr_t)user_buffer;

    (void)write;
    return ptr >= base && ptr - base <= TEST_USER_BYTES
        && len <= TEST_USER_BYTES - (ptr - base);
}
bool exec_has_user_space(void) { return true; }
uint32_t exec_current_pid(void) { return test_pid; }
long exec_sys_open(uint64_t path, uint64_t flags)
{ (void)path; (void)flags; return -BOOK8_ENOENT; }
long exec_sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{ (void)fd; (void)buf; (void)len; return -BOOK8_EBADF; }
long exec_sys_close(uint64_t fd) { (void)fd; return -BOOK8_EBADF; }
long exec_sys_execve(struct syscall_frame *f, uint64_t a, uint64_t b)
{ (void)f; (void)a; (void)b; return -BOOK8_ENOEXEC; }
long book8_console_write(const char *buf, unsigned long len)
{ (void)buf; return (long)len; }
long book8_console_read(char *buf, unsigned long len)
{ (void)buf; return (long)len; }
long book8_framebuffer_info(struct book8_fb_info *info)
{ (void)info; return -BOOK8_ENODEV; }
long book8_framebuffer_draw_pixel(uint64_t x, uint64_t y, uint32_t rgb)
{ (void)x; (void)y; (void)rgb; return -BOOK8_ENODEV; }
long book8_gui_utility_request(const struct book8_gui_utility_request *request)
{ (void)request; return 0; }
void syscall_return_to_kernel(long status)
{
    ++exit_calls;
    exit_status = status;
}
void book8_task_exit(long status)
{
    ++kernel_exit_calls;
    kernel_exit_status = status;
}
long hwdiag_info(struct book8_hwdiag_info *info)
{ (void)info; return -BOOK8_ENOTSUP; }
long hwdiag_read32(struct book8_hwdiag_read32 *request)
{ (void)request; return -BOOK8_ENOTSUP; }
long hwdiag_report(const struct book8_hwdiag_report *report)
{ (void)report; return 0; }

static long call_syscall(uint64_t number, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
    struct syscall_frame frame = {0};

    frame.rax = number;
    frame.rdi = a1;
    frame.rsi = a2;
    frame.rdx = a3;
    frame.r10 = a4;
    syscall_dispatch(&frame);
    return (long)frame.rax;
}

static uint64_t user_address(uint64_t offset)
{
    return (uint64_t)(uintptr_t)&user_buffer[offset];
}

int main(void)
{
    uint8_t kernel_buffer[32];
    uint64_t out = user_address(128u);

    memset(user_buffer, 0, sizeof(user_buffer));
    test_pid = 11u;

    /* Owned port lifecycle. */
    assert(call_syscall(SYS_ipc_open, 2u, BOOK8_IPC_CREATE, 0u, 0u) == 0);
    assert(call_syscall(SYS_ipc_open, 2u,
        BOOK8_IPC_CREATE | BOOK8_IPC_EXCL, 0u, 0u) == -BOOK8_EEXIST);
    test_pid = 12u;
    assert(call_syscall(SYS_ipc_open, 2u, 0u, 0u, 0u) == -BOOK8_EPERM);
    assert(call_syscall(SYS_ipc_close, 2u, 0u, 0u, 0u) == -BOOK8_EPERM);

    /* Any task may post to an owned port; only the owner receives. */
    memcpy(user_buffer, "hello", 5u);
    assert(call_syscall(SYS_ipc_send, 2u, user_address(0u), 5u, 0u) == 0);
    assert(call_syscall(SYS_ipc_recv, 2u, out, 32u, BOOK8_IPC_NOWAIT)
        == -BOOK8_EPERM);
    test_pid = 11u;
    assert(call_syscall(SYS_ipc_recv, 2u, out, 32u, BOOK8_IPC_NOWAIT) == 5);
    assert(memcmp((const void *)out, "hello", 5u) == 0);
    assert(call_syscall(SYS_ipc_recv, 2u, out, 32u, BOOK8_IPC_NOWAIT)
        == -BOOK8_EAGAIN);

    /* Kernel can drain user -> kernel messages through any port. */
    test_pid = 12u;
    memcpy(user_buffer, "to-kernel", 9u);
    assert(call_syscall(SYS_ipc_send, 2u, user_address(0u), 9u, 0u) == 0);
    assert(book8_ipc_get(2u, kernel_buffer, sizeof(kernel_buffer)) == 9);
    assert(memcmp(kernel_buffer, "to-kernel", 9u) == 0);
    assert(book8_ipc_get(2u, kernel_buffer, sizeof(kernel_buffer))
        == -BOOK8_EAGAIN);
    test_pid = 11u;

    /* Queue saturation is reported instead of overwriting older messages. */
    for (unsigned i = 0u; i < BOOK8_IPC_QUEUE_SIZE; ++i)
        assert(call_syscall(SYS_ipc_send, 2u, user_address(0u), 5u, 0u) == 0);
    assert(call_syscall(SYS_ipc_send, 2u, user_address(0u), 5u, 0u)
        == -BOOK8_EAGAIN);
    for (unsigned i = 0u; i < BOOK8_IPC_QUEUE_SIZE; ++i)
        assert(call_syscall(SYS_ipc_recv, 2u, out, 32u, BOOK8_IPC_NOWAIT) == 5);

    /* Kernel event port cannot be claimed, but every pid can receive it. */
    assert(call_syscall(SYS_ipc_open, BOOK8_IPC_KERNEL_PORT,
        BOOK8_IPC_CREATE, 0u, 0u) == -BOOK8_EPERM);
    assert(call_syscall(SYS_ipc_open, BOOK8_IPC_KERNEL_PORT, 0u, 0u, 0u) == 0);
    assert(call_syscall(SYS_ipc_close, BOOK8_IPC_KERNEL_PORT, 0u, 0u, 0u)
        == -BOOK8_EPERM);
    assert(book8_ipc_post(BOOK8_IPC_KERNEL_PORT, 0u, "event", 5u) == 0);
    test_pid = 99u;
    assert(call_syscall(SYS_ipc_recv, BOOK8_IPC_KERNEL_PORT, out, 32u,
        BOOK8_IPC_NOWAIT) == 5);
    assert(memcmp((const void *)out, "event", 5u) == 0);

    /* Bounds and ownership errors. */
    assert(call_syscall(SYS_ipc_send, 0u, user_address(0u), 1u, 0u)
        == -BOOK8_EINVAL);
    assert(call_syscall(SYS_ipc_send, BOOK8_IPC_PORT_COUNT + 1u,
        user_address(0u), 1u, 0u) == -BOOK8_EINVAL);
    assert(call_syscall(SYS_ipc_send, 2u, user_address(0u),
        BOOK8_IPC_MESSAGE_MAX + 1u, 0u) == -BOOK8_EINVAL);
    assert(call_syscall(SYS_ipc_send, 2u, 0u, 1u, 0u) == -BOOK8_EFAULT);
    assert(call_syscall(SYS_ipc_recv, 2u, 0u, 1u, BOOK8_IPC_NOWAIT)
        == -BOOK8_EFAULT);
    assert(call_syscall(SYS_ipc_recv, 2u, out, 8u, 0x80u) == -BOOK8_EINVAL);

    /* Released ports become unbound even for another pid. */
    book8_ipc_release_owner(11u);
    assert(call_syscall(SYS_ipc_open, 2u, 0u, 0u, 0u) == -BOOK8_ENOENT);
    assert(call_syscall(SYS_ipc_send, 2u, user_address(0u), 1u, 0u)
        == -BOOK8_ENOENT);

    /* The lightweight scheduling/spawn ABI remains callable from user code. */
    assert(call_syscall(SYS_yield, 0u, 0u, 0u, 0u) == 0);
    assert(call_syscall(SYS_spawn, 0u, 0u, 0u, 0u) == -BOOK8_EFAULT);
    memcpy(user_buffer, "A:\\BIN\\APP.ELF", 15u);
    assert(call_syscall(SYS_spawn, user_address(0u), 0u, 0u, 0u)
        == -BOOK8_ENOSYS);

    /* Each CPU publishes its own TSS privilege stack. */
    syscall_select_kernel_stack(0u, 0x1234u);
    syscall_select_kernel_stack(1u, 0x5678u);
    assert(syscall_current_kernel_stack(0u) == 0x1234u);
    assert(syscall_current_kernel_stack(1u) == 0x5678u);

    printf("PASS: user IPC ownership, bounded queues, kernel events, TSS stacks and spawn ABI\n");
    return 0;
}
