    .section .ap_trampoline,"ax",@progbits
    .balign 16
    .global book8_ap_trampoline_start
book8_ap_trampoline_start:
    .code16
    cli
    cld
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov $0x8000 + (ap_gdt_pointer - book8_ap_trampoline_start), %bx
    lgdt (%bx)
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x18, $(0x8000 + (ap_protected_entry - book8_ap_trampoline_start))

    .code32
ap_protected_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %cr4, %eax
    or $0x30, %eax
    mov %eax, %cr4
    mov $0xc0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov 0x8000 + (book8_ap_trampoline_data - book8_ap_trampoline_start), %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    ljmp $0x08, $(0x8000 + (ap_long_mode_entry - book8_ap_trampoline_start))

    .code64
ap_long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov book8_ap_trampoline_data + 8(%rip), %rsp
    mov book8_ap_trampoline_data + 16(%rip), %rdi
    mov book8_ap_trampoline_data + 24(%rip), %rax
    jmp *%rax

    .balign 8
    .global book8_ap_trampoline_data
book8_ap_trampoline_data:
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .balign 8
ap_gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00cf9a000000ffff
ap_gdt_end:
ap_gdt_pointer:
    .short ap_gdt_end - ap_gdt - 1
    .long 0x8000 + (ap_gdt - book8_ap_trampoline_start)

    /* Relaunch parking loop. This stays in the low page because the kernel
     * image above 1 MiB is overwritten while these APs are halted. */
    .balign 16
    .global book8_ap_relaunch_halt
book8_ap_relaunch_halt:
    cli
1:
    hlt
    jmp 1b

    .global book8_ap_trampoline_end
book8_ap_trampoline_end:

    .section .note.GNU-stack,"",@progbits
