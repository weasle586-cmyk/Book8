    .text
    .code64

    .global book8_read_cr3
    .type book8_read_cr3, @function
book8_read_cr3:
    mov %cr3, %rax
    ret
    .size book8_read_cr3, . - book8_read_cr3

    .global book8_write_cr3
    .type book8_write_cr3, @function
book8_write_cr3:
    mov %rdi, %cr3
    ret
    .size book8_write_cr3, . - book8_write_cr3

    .global book8_unhandled_interrupt
    .type book8_unhandled_interrupt, @function
book8_unhandled_interrupt:
    cli
1:
    hlt
    jmp 1b
    .size book8_unhandled_interrupt, . - book8_unhandled_interrupt

    /* Only exact CPL0 guarded access instructions have recovery targets. */
    .global book8_gp_entry
    .type book8_gp_entry, @function
book8_gp_entry:
    push %rax
    cmpq $0, 8(%rsp)
    jne .Lgp_memory
    testb $3, 24(%rsp)
    jne .Lgp_fatal
    lea .Lsafe_rdmsr(%rip), %rax
    cmp %rax, 16(%rsp)
    jne .Lgp_memory
    lea .Lmsr_failed(%rip), %rax
    mov %rax, 16(%rsp)
    pop %rax
    add $8, %rsp
    iretq
.Lgp_fatal:
    pop %rax
    jmp book8_unhandled_interrupt
.Lgp_memory:
    mov $13, %eax
    jmp .Lmemory_fixup
    .size book8_gp_entry, . - book8_gp_entry

    .global book8_pf_entry
    .type book8_pf_entry, @function
book8_pf_entry:
    push %rax
    mov $14, %eax
.Lmemory_fixup:
    testb $3, 24(%rsp)
    jz .Lmemory_kernel
    cmp $14, %eax
    jne .Lgp_fatal
    push %rdx
    jmp .Lmemory_fatal
.Lmemory_kernel:
    push %rdx
    lea .Lsafe_read32(%rip), %rdx
    cmp %rdx, 24(%rsp)
    je .Lmemory_recover
    lea .Lsafe_write32(%rip), %rdx
    cmp %rdx, 24(%rsp)
    jne .Lmemory_fatal
.Lmemory_recover:
    lea .Lmemory_failed(%rip), %rdx
    mov %rdx, 24(%rsp)
    pop %rdx
    add $8, %rsp
    add $8, %rsp
    iretq
.Lmemory_fatal:
    pop %rdx
    cmp $14, %eax
    jne .Lgp_fatal
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
    mov %rsp, %rbx
    mov %cr2, %rdi
    mov 120(%rbx), %rsi
    and $-16, %rsp
    call book8_page_fault_dispatch
    test %eax, %eax
    jz .Lpf_unhandled
    mov %rbx, %rsp
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
    add $8, %rsp
    iretq
.Lpf_unhandled:
    jmp book8_unhandled_interrupt
    .size book8_pf_entry, . - book8_pf_entry

    .global book8_read32_safe
    .type book8_read32_safe, @function
book8_read32_safe:
    pushfq
    cli
.Lsafe_read32:
    mov (%rdi), %eax
    mov %eax, (%rsi)
    xor %eax, %eax
    popfq
    ret
    .size book8_read32_safe, . - book8_read32_safe

    .global book8_write32_safe
    .type book8_write32_safe, @function
book8_write32_safe:
    pushfq
    cli
.Lsafe_write32:
    mov %esi, (%rdi)
    xor %eax, %eax
.Lmemory_failed:
    popfq
    ret
    .size book8_write32_safe, . - book8_write32_safe

    .global book8_read_msr_safe
    .type book8_read_msr_safe, @function
book8_read_msr_safe:
    pushfq
    cli
    mov %edi, %ecx
.Lsafe_rdmsr:
    rdmsr
    shl $32, %rdx
    or %rdx, %rax
    mov %rax, (%rsi)
    mov $1, %eax
    popfq
    ret
.Lmsr_failed:
    xor %eax, %eax
    popfq
    ret
    .size book8_read_msr_safe, . - book8_read_msr_safe

    .global book8_ps2_mouse_irq_entry
    .type book8_ps2_mouse_irq_entry, @function
book8_ps2_mouse_irq_entry:
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
    mov %rsp, %rbx
    and $-16, %rsp
    call book8_ps2_mouse_irq_dispatch
    mov %rbx, %rsp
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
    .size book8_ps2_mouse_irq_entry, . - book8_ps2_mouse_irq_entry

    .section .note.GNU-stack,"",@progbits
