#include <syscall.h>

int errno;

long sys_read(int fd, void *buf, unsigned long count)
{
    return __syscall_ret(__syscall(SYS_read, (long)fd, (long)buf, (long)count));
}

long sys_write(int fd, const void *buf, unsigned long count)
{
    return __syscall_ret(_syscall3(SYS_write, (long)fd, (long)buf, (long)count));
}

long sys_open(const char *path, int flags)
{
    return __syscall_ret(_syscall2(SYS_open, (long)path, (long)flags));
}

long sys_close(int fd)
{
    return __syscall_ret(_syscall1(SYS_close, (long)fd));
}

long sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return __syscall_ret(_syscall3(SYS_execve, (long)path, (long)argv, (long)envp));
}

long sys_getpid(void)
{
    return __syscall_ret(_syscall0(SYS_getpid));
}

long sys_spawn(const char *path, char *const argv[])
{
    return __syscall_ret(_syscall2(SYS_spawn, (long)path, (long)argv));
}

long sys_yield(void)
{
    return __syscall_ret(_syscall0(SYS_yield));
}

long sys_ipc_open(uint32_t port, uint32_t flags)
{
    return __syscall_ret(_syscall2(SYS_ipc_open, (long)port, (long)flags));
}

long sys_ipc_close(uint32_t port)
{
    return __syscall_ret(_syscall1(SYS_ipc_close, (long)port));
}

long sys_ipc_send(uint32_t port, const void *data, uint32_t size)
{
    return __syscall_ret(_syscall3(SYS_ipc_send, (long)port, (long)data,
        (long)size));
}

long sys_ipc_recv(uint32_t port, void *data, uint32_t capacity, uint32_t flags)
{
    return __syscall_ret(_syscall4(SYS_ipc_recv, (long)port, (long)data,
        (long)capacity, (long)flags));
}

long sys_fb_info(struct book8_fb_info *info)
{
    return __syscall_ret(_syscall1(SYS_fb_info, (long)info));
}

long sys_draw_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    return __syscall_ret(_syscall3(SYS_draw_pixel, (long)x, (long)y,
        (long)rgb));
}

long sys_hwdiag(unsigned operation, void *data, unsigned long size)
{
    return __syscall_ret(_syscall3(SYS_hwdiag, (long)operation,
        (long)data, (long)size));
}

void sys_exit(int status)
{
    _syscall1(SYS_exit, (long)status);
    for (;;) {
        __asm__ volatile ("" ::: "memory");
    }
}
