#ifndef BOOK8_SYSCALL_H
#define BOOK8_SYSCALL_H

/*
 * BOOK8 system call ABI (shared by kernel and libc; uses only builtin types).
 *
 *   RAX            syscall number on entry, return value on exit
 *   RDI RSI RDX R10 R8 R9   arguments 1..6   (R10 replaces RCX: SYSCALL clobbers RCX)
 *   RCX, R11       clobbered by the SYSCALL instruction (user RIP / RFLAGS)
 *   All other registers are preserved.
 *
 * Errors are returned as -errno in RAX (values -4095..-1).
 */

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_getpid  39
#define SYS_execve  59
#define SYS_exit    60
#define SYS_fb_info 61
#define SYS_draw_pixel 62
#define SYS_hwdiag   63
#define SYS_gui_utility 64
#define SYS_spawn    65 /* start a background user task */
#define SYS_yield    66 /* voluntary CPU yield from a user task */
#define SYS_ipc_open 67 /* bind or open a kernel IPC port */
#define SYS_ipc_close 68 /* release an owned IPC port */
#define SYS_ipc_send 69 /* send a bounded message to an IPC port */
#define SYS_ipc_recv 70 /* receive a bounded message from an IPC port */
#define SYS_MAX     71 /* exclusive upper bound of the dispatch table */

/* Kernel IPC ports. Port 1 is reserved for kernel -> user event delivery and
 * may be received by any task; all other ports are owned by the creating pid. */
#define BOOK8_IPC_KERNEL_PORT 1u
#define BOOK8_IPC_KERNEL_OWNER 0xffffffffu /* never assigned to a user pid */
#define BOOK8_IPC_PORT_COUNT  32u
#define BOOK8_IPC_QUEUE_SIZE  8u
#define BOOK8_IPC_MESSAGE_MAX 256u

/* SYS_ipc_open flags. */
#define BOOK8_IPC_CREATE 0x1u
#define BOOK8_IPC_EXCL   0x2u
/* SYS_ipc_recv flags. BOOK8_IPC_NOWAIT is currently the only supported mode;
 * blocking waits are expressed as a user-side poll plus SYS_yield. */
#define BOOK8_IPC_NOWAIT 0x1u

/* ------------------------------------------------------------
 * Architecture stabilization guards.
 * Changes to these constants, struct layouts, or enum values
 * require a full rebuild and test matrix to avoid ABI breakage.
 * ------------------------------------------------------------ */

/* Maximum number of GUI program-registry slots, including system
   components seeded at boot. This guard prevents silent overflow of the
   registered_programs array when new ELFs are added. */
#define BOOK8_MAX_PROGRAMS 512u

/* GUI utility kinds passed to SYS_gui_utility (RAX=64).
   Each kind maps to a specific ELF launcher in A:\BIN\ via
   utility_path(), and the corresponding app_class in src/book8gui.c.
   The kernel seeds the registry in registry_seed_defaults() (kernel.c).
   KIND VALUES MUST NOT BE REORDERING OR REASSIGNED without also
   updating utility_path(), book8gui_program_info(), and the
   programs_menu[] array in src/book8gui.c.
   Registry program indices are frozen at boot; adding new user
   programs beyond BOOK8_MAX_PROGRAMS requires a kernel rebuild. */

/* 1 = Hex Viewer: views binary files; ELF: A:\BIN\HEXVIEW.ELF */
#define BOOK8_GUI_HEXVIEW 1u

/* 2 = Code Editor: plain-text editor; ELF: A:\BIN\CODEEDIT.ELF */
#define BOOK8_GUI_CODEEDIT 2u

/* 3 = Network Share Browser: LAN file browser; ELF: A:\BIN\NETSHARE.ELF */
#define BOOK8_GUI_NETSHARE 3u

/* 4 = PaintBook: bitmap drawing app; ELF: A:\BIN\PAINTBK.ELF */
#define BOOK8_GUI_PAINTBOOK 4u

/* 5 = Web Browser: HTTP/html client; ELF: A:\BIN\BROWSER.ELF */
#define BOOK8_GUI_BROWSER 5u

/* 6 = NV Diagnostic: hardware register dump; ELF: A:\BIN\NVDIAG.ELF */
#define BOOK8_GUI_NVDIAG 6u

/* 7 = BOOK8 Player: VLC-style player; built-in app class, no user ELF */
#define BOOK8_GUI_PLAYER 7u

#define BOOK8_EPERM    1
#define BOOK8_ENOENT   2
#define BOOK8_EIO      5
#define BOOK8_ENOEXEC  8
#define BOOK8_EBADF    9
#define BOOK8_EAGAIN   11
#define BOOK8_ENOMEM   12
#define BOOK8_EFAULT   14
#define BOOK8_ENODEV   19
#define BOOK8_EINVAL   22
#define BOOK8_EMFILE   24
#define BOOK8_EEXIST   17
#define BOOK8_ENOSYS   38
#define BOOK8_ENOTSUP  95

#define BOOK8_MAX_ERRNO 4095UL

#ifndef __ASSEMBLER__

#include <stdint.h>

extern int errno;

struct book8_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bits_per_pixel;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
    uint8_t reserved;
};
struct book8_gui_utility_request {
    uint32_t kind;
    char path[256];
};

/* Raw trap wrappers: return the kernel's value (negative errno on failure). */
static inline long _syscall0(long n)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long _syscall1(long n, long a1)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long _syscall2(long n, long a1, long a2)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long _syscall3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long _syscall4(long n, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall" : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long _syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile ("syscall" : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long _syscall6(long n, long a1, long a2, long a3, long a4,
                             long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile ("syscall" : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
                        "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

/* Assembly stub (lib/syscall_stub.S): long __syscall(long num, ...) with up to
 * six C-ABI arguments after num. Returns the raw kernel value. */
long __syscall(long num, ...);

/* Convert a raw kernel return to the C convention: -1 and errno on error. */
static inline long __syscall_ret(long r)
{
    if ((unsigned long)r > (unsigned long)-1L - BOOK8_MAX_ERRNO) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* libc-level wrappers (lib/syscall.c). */
long sys_read(int fd, void *buf, unsigned long count);
long sys_write(int fd, const void *buf, unsigned long count);
long sys_getpid(void);
/* Framebuffer metadata and a bounds-checked RGB pixel operation. */
long sys_fb_info(struct book8_fb_info *info);
long sys_draw_pixel(uint32_t x, uint32_t y, uint32_t rgb);
/* Dedicated NVIDIA diagnostics; operations/types are in hwdiag.h. */
long sys_hwdiag(unsigned operation, void *data, unsigned long size);
#define O_RDONLY 0
/* Read-only file access; returns an fd >= 3. */
long sys_open(const char *path, int flags);
long sys_close(int fd);
/* Replaces the process image; returns only on failure. argv is NULL-terminated. */
long sys_execve(const char *path, char *const argv[], char *const envp[]);
/* Starts another user task in the unified task/privilege model. */
long sys_spawn(const char *path, char *const argv[]);
long sys_yield(void);
/* Bounded kernel <-> user IPC. Returns 0 or a message length on success. */
long sys_ipc_open(uint32_t port, uint32_t flags);
long sys_ipc_close(uint32_t port);
long sys_ipc_send(uint32_t port, const void *data, uint32_t size);
long sys_ipc_recv(uint32_t port, void *data, uint32_t capacity, uint32_t flags);
__attribute__((noreturn)) void sys_exit(int status);

#endif /* __ASSEMBLER__ */
#endif
