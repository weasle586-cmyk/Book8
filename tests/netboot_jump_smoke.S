.section .text
.code64
.global kernel_main
kernel_main:
    mov $0x3f9, %dx
    xor %eax, %eax
    outb %al, %dx
    mov $0x3fb, %dx
    mov $0x80, %al
    outb %al, %dx
    mov $0x3f8, %dx
    mov $1, %al
    outb %al, %dx
    mov $0x3f9, %dx
    xor %eax, %eax
    outb %al, %dx
    mov $0x3fb, %dx
    mov $3, %al
    outb %al, %dx
    lea payload(%rip), %rsi
    mov $0x05000000, %edi
    mov $(payload_end - payload), %ecx
    rep movsb
    mov %cr3, %rax
    mov %rax, warm_info + 40(%rip)
    mov $0x05000000, %edi
    lea warm_stack_end(%rip), %rsi
    lea warm_info(%rip), %rdx
    call book8_netboot_jump

payload:
    movabs $0x314d524157384b42, %rax
    cmp %rax, %rdi
    jne failure
    cmp %rax, (%rsi)
    jne failure
    cmpq $1, 8(%rsi)
    jne failure
    cmpq $0x05000000, 16(%rsi)
    jne failure
    mov %cr3, %rax
    cmp %rax, 40(%rsi)
    jne failure
    pushfq
    pop %rax
    test $0x600, %rax
    jne failure
    mov %rsp, %rax
    and $15, %eax
    cmp $8, %eax
    jne failure
    lea passed(%rip), %rsi
    jmp print
failure:
    lea failed(%rip), %rsi
print:
    lodsb
    test %al, %al
    jz finish
    mov %al, %bl
    mov $0x3fd, %dx
1:
    inb %dx, %al
    test $0x20, %al
    jz 1b
    mov $0x3f8, %dx
    mov %bl, %al
    outb %al, %dx
    jmp print
finish:
    mov $0xf4, %dx
    xor %eax, %eax
    outl %eax, %dx
2:
    cli
    hlt
    jmp 2b
passed: .asciz "PASS: NETBOOT long-mode warm handoff ABI\r\n"
failed: .asciz "FAIL: NETBOOT warm handoff ABI\r\n"
payload_end:

.section .data
.balign 8
warm_info:
    .quad 0x314d524157384b42, 1, 0x05000000
    .quad payload_end - payload, payload_end - payload, 0
.section .bss
.balign 16
warm_stack: .skip 16384
warm_stack_end:
.section .note.GNU-stack,"",@progbits
