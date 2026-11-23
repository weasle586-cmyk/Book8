#ifndef BOOK8_EXEC_H
#define BOOK8_EXEC_H

#include <stdbool.h>
#include <stdint.h>
#include "elf.h"
#include "../fs/fs.h"
#include "../include/b8_shlib.h"

/* User address space: one 1 GiB window at PML4[1], backed by a kernel page
 * pool. Executables must be linked inside it (see user/user.ld). */
#define USER_BASE        0x0000008000000000ull
#define USER_SIZE        0x0000000040000000ull
#define USER_LIMIT       (USER_BASE + USER_SIZE)
#define USER_STACK_PAGES 1u
#define USER_STACK_TOP   USER_LIMIT

#define EXEC_MAX_TASKS 4u
#define EXEC_MAX_FDS   8u
#define EXEC_MAX_ARGS  16u
#define EXEC_MAX_MODULES 16u
#define EXEC_ARG_BYTES 1024u
#define EXEC_FD_BASE   3
#define EXEC_KERNEL_STACK_SIZE 16384u

#define ELF_MAX_PHDRS 8u

struct elf_image {
    uint64_t entry;
    uint64_t exports_offset;
    uint64_t imports_offset;
    uint32_t exports_count;
    uint32_t imports_count;
    uint32_t relocation_count;
    uint64_t section_headers_offset;
    uint16_t section_count;
    uint16_t type;
    uint16_t phnum;
    Elf64_Phdr phdr[ELF_MAX_PHDRS];
};

struct exec_module {
    char path[256];
    uint64_t load_bias;
    b8_sym_export_t *exports;
    uint32_t export_count;
    uint32_t export_pages;
};

enum task_state {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
};

/* One user task runs at a time. rsp is reserved for an interrupt context. */
struct exec_task {
    uint32_t pid;
    uint32_t uid;
    uint64_t session_token;
    enum task_state state;
    char name[32];
    uint64_t entry;
    uint64_t user_rsp;
    uint64_t rsp;
    uint64_t cr3;
    uint64_t *user_pd;
    uint32_t page_pool_used;
    uint64_t image_high_water;
    uint32_t address_space_slot;
    uint32_t module_count;
    struct exec_module modules[EXEC_MAX_MODULES];
    uint64_t caller_rsp;
    long exit_status;
    file_handle_t *fds[EXEC_MAX_FDS];
    uint8_t kernel_stack[EXEC_KERNEL_STACK_SIZE] __attribute__((aligned(16)));
};

/* Timer entry stack: 15 GPRs followed by the x86-64 IRET frame. */
struct timer_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

struct timer_frame *exec_timer_tick(struct timer_frame *frame);
/* True when the BSP timer must route through the user-task scheduler. */
bool exec_user_scheduler_active(void);
/* Start a background user task under the unified task/privilege model. */
long exec_spawn_user(const char *path, uint64_t argv);
/* Mark a user task terminated from its exit trampoline. */
void exec_task_finish(struct exec_task *task, long status);
/* Start and finish require local interrupts disabled. Test threads use no FPU. */
bool exec_scheduler_test_start(void);
bool exec_scheduler_test_finish(void);
uint64_t exec_scheduler_test_ticks(void);
uint32_t exec_scheduler_test_user_switches(void);
struct exec_task *exec_scheduler_test_current(void);
bool exec_scheduler_test_active(void);

/* Validate an ELF64 x86-64 ET_EXEC file and read its program headers.
 * Returns 0 or -BOOK8_ENOEXEC. */
long elf_inspect(file_handle_t *file, struct elf_image *img);
/* Map PT_LOAD pages into a task's address space. */
long elf_load(file_handle_t *file, const struct elf_image *img);
/* Resolve and validate imports against the task's loaded module set. */
long elf_resolve_imports(file_handle_t *file, const struct elf_image *img);
/* Shared execution guard used by exec_run and execve. */
long elf_require_executable(const struct elf_image *img);

/* Load path, run it in Ring 3 until SYS_exit, tear down. Returns the exit
 * status, or a negative errno if it could not be started. */
long exec_run(const char *path, int argc, char **argv);

struct syscall_frame;
/* Kernel side of SYS_open/read/close/execve (user pointers are validated). */
long exec_sys_open(uint64_t path, uint64_t flags);
long exec_sys_read(uint64_t fd, uint64_t buf, uint64_t len);
long exec_sys_close(uint64_t fd);
long exec_sys_execve(struct syscall_frame *frame, uint64_t path, uint64_t argv);

bool exec_user_range_ok(uint64_t ptr, uint64_t len, bool write);
bool exec_has_user_space(void);
bool exec_page_fault(uint64_t address, uint64_t error_code);
/* Requires local interrupts disabled and no active task. */
bool exec_address_space_test(void);
uint32_t exec_current_pid(void);

#endif
