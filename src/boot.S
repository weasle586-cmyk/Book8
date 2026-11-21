.section .multiboot,"a"
.balign 8
header_start:
    .long 0xe85250d6
    .long 0
    .long header_end - header_start
    .long -(0xe85250d6 + (header_end - header_start))
    .short 5
    .short 0
    .long 20
    .long 1024
    .long 768
    .long 32
.balign 8
    .short 0
    .short 0
    .long 8
header_end:

.section .bss
.balign 4096
.global boot_pml4
boot_pml4:
    .skip 4096
.balign 4096
.global boot_pdpt
boot_pdpt:
    .skip 4096
.balign 4096
.global boot_pd0
boot_pd0:
    .skip 4096
.balign 4096
.global boot_pd1
boot_pd1:
    .skip 4096
.balign 4096
.global boot_pd2
boot_pd2:
    .skip 4096
.balign 4096
.global boot_pd3
boot_pd3:
    .skip 4096
.balign 16
boot_stack_bottom:
    .skip 32768
boot_stack_top:
.balign 4
boot_magic:
    .long 0
boot_info:
    .long 0

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    cld
    mov %eax, boot_magic
    mov %ebx, boot_info
    mov $boot_stack_top, %esp
    xor %ebp, %ebp

    mov $boot_pml4, %edi
    xor %eax, %eax
    mov $(4096 * 6 / 4), %ecx
    rep stosl

    movl $(boot_pdpt + 3), boot_pml4
    movl $0, boot_pml4 + 4
    movl $(boot_pd0 + 3), boot_pdpt
    movl $0, boot_pdpt + 4
    movl $(boot_pd1 + 3), boot_pdpt + 8
    movl $0, boot_pdpt + 12
    movl $(boot_pd2 + 3), boot_pdpt + 16
    movl $0, boot_pdpt + 20
    movl $(boot_pd3 + 3), boot_pdpt + 24
    movl $0, boot_pdpt + 28

    mov $boot_pd0, %edi
    xor %ebx, %ebx
    mov $2048, %ecx
1:
    mov %ebx, %eax
    or $0x83, %eax
    stosl
    xor %eax, %eax
    stosl
    add $0x200000, %ebx
    loop 1b

    mov %cr4, %eax
    or $0x30, %eax
    mov %eax, %cr4
    mov $boot_pml4, %eax
    mov %eax, %cr3

    mov $0xc0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    lgdt gdt64_pointer
    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    movabs $boot_stack_top, %rsp
    and $-16, %rsp
    mov boot_magic(%rip), %edi
    mov boot_info(%rip), %esi
    call kernel_main
2:
    cli
    hlt
    jmp 2b

.balign 8
gdt64:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
gdt64_end:
gdt64_pointer:
    .short gdt64_end - gdt64 - 1
    .long gdt64

.size _start, . - _start

.code64
.global book8_netboot_jump
.type book8_netboot_jump, @function
book8_netboot_jump:
    cli
    cld
    mov %rdi, %rax
    mov %rsi, %rsp
    and $-16, %rsp
    xor %ebp, %ebp
    mov %rdx, %rsi
    movabs $0x314d524157384b42, %rdi
    lea netboot_returned(%rip), %rcx
    push %rcx
    jmp *%rax
netboot_returned:
    cli
    hlt
    jmp netboot_returned
.size book8_netboot_jump, . - book8_netboot_jump
