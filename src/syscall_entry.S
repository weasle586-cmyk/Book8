/* SYSCALL entry/exit for x86_64. Interrupts stay masked (SFMASK clears IF)
 * for the whole syscall; the kernel stack comes from the per-CPU block that
 * KERNEL_GS_BASE points to. */
    .text
    .code64

    .global book8_timer_irq_entry
    .type book8_timer_irq_entry, @function
book8_timer_irq_entry:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    mov %rsp, %rdi
    mov $0x20, %al
    out %al, $0x20
    and $-16, %rsp
    call book8_timer_tick
    mov %rax, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    iretq
    .size book8_timer_irq_entry, . - book8_timer_irq_entry

    .global book8_apic_timer_entry
    .type book8_apic_timer_entry, @function
book8_apic_timer_entry:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    mov %rsp, %rdi
    and $-16, %rsp
    call book8_apic_timer_tick
    mov %rax, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    iretq
    .size book8_apic_timer_entry, . - book8_apic_timer_entry

    .global book8_sched_ipi_entry
    .type book8_sched_ipi_entry, @function
book8_sched_ipi_entry:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    mov %rsp, %rdi
    and $-16, %rsp
    call book8_sched_ipi_tick
    mov %rax, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    iretq
    .size book8_sched_ipi_entry, . - book8_sched_ipi_entry

    .global book8_scheduler_test_worker
    .type book8_scheduler_test_worker, @function
book8_scheduler_test_worker:
    mov $1, %rax
    mov $2, %rbx
    mov $3, %rcx
    mov $4, %rdx
    mov $5, %rbp
    mov $8, %r8
    mov $9, %r9
    mov $10, %r10
    mov $11, %r11
    mov $12, %r12
    mov $13, %r13
    mov $14, %r14
    mov $15, %r15
    std
1:
    cmp $1, %rax
    jne 2f
    cmp $2, %rbx
    jne 2f
    cmp $3, %rcx
    jne 2f
    cmp $4, %rdx
    jne 2f
    cmp $5, %rbp
    jne 2f
    cmp $8, %r8
    jne 2f
    cmp $9, %r9
    jne 2f
    cmp $10, %r10
    jne 2f
    cmp $11, %r11
    jne 2f
    cmp $12, %r12
    jne 2f
    cmp $13, %r13
    jne 2f
    cmp $14, %r14
    jne 2f
    cmp $15, %r15
    jne 2f
    pushfq
    pop %rax
    test $0x400, %rax
    jz 2f
    mov $1, %rax
    incq (%rdi)
    jmp 1b
2:
    movq $1, (%rsi)
3:
    jmp 3b
    .size book8_scheduler_test_worker, . - book8_scheduler_test_worker

    .global syscall_entry
    .type syscall_entry, @function
syscall_entry:
    swapgs
    mov %rsp, %gs:8          /* save user RSP */
    mov %gs:0, %rsp          /* switch to kernel stack */

    push %gs:8               /* frame.rsp */
    push %rcx                /* frame.rcx = user RIP */
    push %r11                /* frame.r11 = user RFLAGS */
    push %rax                /* frame.rax = number */
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    cld
    mov %rsp, %rdi
    call syscall_dispatch

    /* Reject non-user canonical return state before SYSRET can fault in ring 0. */
    mov 112(%rsp), %rcx      /* frame.rcx */
    mov %rcx, %rdx
    shl $16, %rdx
    sar $16, %rdx
    cmp %rdx, %rcx
    jne syscall_bad_return
    movabs $0x00007fffffffffff, %rax
    cmp %rax, %rcx
    ja syscall_bad_return
    mov 120(%rsp), %rdx      /* frame.rsp */
    test %rdx, %rdx
    jz syscall_bad_return
    mov %rdx, %rcx
    shl $16, %rcx
    sar $16, %rcx
    cmp %rcx, %rdx
    jne syscall_bad_return
    cmp %rax, %rdx
    ja syscall_bad_return

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax                 /* return value */
    pop %r11                 /* user RFLAGS */
    pop %rcx                 /* user RIP */
    pop %rsp                 /* user RSP */
    swapgs
    sysretq
    .size syscall_entry, . - syscall_entry

syscall_bad_return:
    swapgs                   /* back to kernel GS_BASE state */
    cli
1:  hlt
    jmp 1b

/* long syscall_enter_user(uint64_t rip [rdi], uint64_t rsp [rsi],
 *                         uint64_t *caller_rsp [rdx], uint64_t rflags [rcx])
 * Enters Ring 3 via IRETQ. GS_BASE stays 0 for user mode and KERNEL_GS_BASE
 * already holds the per-CPU block. The kernel context is saved so that
 * SYS_exit can resume here through syscall_return_to_kernel(); the user
 * task's exit status becomes the return value. */
    .global syscall_enter_user
    .type syscall_enter_user, @function
syscall_enter_user:
    pushfq
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdx)
    cli
    mov $0x23, %eax          /* USER_SS */
    mov %ax, %ds
    mov %ax, %es
    push $0x23               /* SS */
    push %rsi                /* RSP */
    push %rcx
    push $0x2b               /* CS (USER_CS) */
    push %rdi                /* RIP */
    iretq
    .size syscall_enter_user, . - syscall_enter_user

/* void syscall_return_to_kernel(long status [rdi])
 * Called from the syscall path (kernel stack, GS swapped in). Abandons the
 * user task and returns status from the pending syscall_enter_user(). */
    .global syscall_return_to_kernel
    .type syscall_return_to_kernel, @function
syscall_return_to_kernel:
    cli
    mov %gs:16, %rdx
    mov (%rdx), %rsp
    swapgs                   /* back to GS_BASE=0, KERNEL_GS=percpu */
    mov %rdi, %rax
    mov $0x10, %ecx          /* KERNEL_DS */
    mov %cx, %ds
    mov %cx, %es
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    popfq
    ret
    .size syscall_return_to_kernel, . - syscall_return_to_kernel
