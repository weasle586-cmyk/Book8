#ifndef BOOK8_KERNEL_SYSCALL_H
#define BOOK8_KERNEL_SYSCALL_H

#include <stdint.h>
#include "../include/syscall.h"
#include "../kernel/smp.h"

/* GDT selectors. Layout is dictated by STAR: SYSCALL loads CS=STAR[47:32],
 * SS=+8; SYSRET (64-bit) loads CS=STAR[63:48]+16, SS=+8, RPL forced to 3. */
#define GDT_KCODE   0x08
#define GDT_KDATA   0x10
#define GDT_UCODE32 0x18 /* placeholder; SYSRET base */
#define GDT_UDATA   0x20
#define GDT_UCODE64 0x28
#define GDT_TSS     0x30
#define USER_CS     (GDT_UCODE64 | 3)
#define USER_SS     (GDT_UDATA | 3)

#define MSR_EFER        0xC0000080u
#define MSR_STAR        0xC0000081u
#define MSR_LSTAR       0xC0000082u
#define MSR_SFMASK      0xC0000084u
#define MSR_KERNEL_GS   0xC0000102u

/* Saved state at syscall entry. Field order mirrors the push order in
 * src/syscall_entry.S (last push = lowest address = first field). */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r9, r8, r10, rdx, rsi, rdi; /* args 6..1 */
    uint64_t rax;                        /* in: number, out: return value */
    uint64_t r11;                        /* user RFLAGS */
    uint64_t rcx;                        /* user RIP */
    uint64_t rsp;                        /* user RSP */
};

/* Per-CPU block reached through GS after SWAPGS. Offsets are used by asm. */
struct syscall_percpu {
    uint64_t kernel_rsp; /* 0: stack loaded on entry */
    uint64_t user_rsp;   /* 8: scratch for the user RSP */
    uint64_t *caller_rsp; /* 16: active task's saved kernel caller */
};

void syscall_init(void);
/* Install the full GDT/TSS and syscall MSRs on a CPU. The BSP calls this from
 * syscall_init(); APs call it before enabling their local timers. */
void syscall_cpu_init(uint32_t cpu_id);
/* Point the active TSS at a kernel privilege stack for the current task. */
void syscall_select_kernel_stack(uint32_t cpu_id, uint64_t rsp);
/* Read the privilege stack currently published in the CPU's TSS. */
uint64_t syscall_current_kernel_stack(uint32_t cpu_id);
void syscall_entry(void); /* LSTAR target (asm) */
void syscall_dispatch(struct syscall_frame *frame);
long book8_framebuffer_info(struct book8_fb_info *info);
long book8_framebuffer_draw_pixel(uint64_t x, uint64_t y, uint32_t rgb);

/* Enter Ring 3 at rip with the given stack. Returns the task's SYS_exit
 * status once it exits (see syscall_return_to_kernel). */
long syscall_enter_user(uint64_t rip, uint64_t rsp, uint64_t *caller_rsp,
                        uint64_t user_rflags);
void syscall_return_to_kernel(long status);

/* Enter Ring 3 and wait for SYS_exit; returns the exit status. */
struct exec_task;
void syscall_select_task(struct exec_task *task);
long syscall_run_user(struct exec_task *task);
/* End the running user task from inside a syscall (does not return). */
void syscall_kill_current(long status);

/* Map the user pages (U/S=1) and run the Ring 3 smoke task. Returns 0 on pass. */
long syscall_ring3_smoke(void);
long syscall_scheduler_smoke(struct exec_task *task, uint64_t delay_ticks);

/* Kernel-side IPC injection. Used by kernel services to publish events to a
 * user task through the same bounded queues as SYS_ipc_send/recv. */
long book8_ipc_post(uint32_t port, uint32_t from_pid, const void *data,
    uint32_t size);
/* Kernel-side receive from any port, including messages sent by user tasks. */
long book8_ipc_get(uint32_t port, void *data, uint32_t capacity);
void book8_ipc_release_owner(uint32_t pid);

#endif
