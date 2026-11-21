/* Minimal Ring 3 smoke task. Everything it touches lives in .user.* sections
 * (U/S=1 pages): this code, __syscall (lib/syscall_stub.S), the message data
 * and the stack. It also checks syscall pointer rejection. Exit status 0 =
 * pass, otherwise the failing check number.
 * Entered by syscall_enter_user via IRETQ with RSP = top of user stack - 8. */
    .section .user.data,"aw"
    .balign 8
user_saved_rsp:
    .quad 0
    .global user_smoke_delay
user_smoke_delay:
    .quad 0
msg_ok:
    .ascii "RING3: hello from CPL3 via SYSCALL\n"
msg_ok_end:
msg_fail:
    .ascii "RING3: check failed\n"
msg_fail_end:
msg_bad_pointer:
    .ascii "RING3: invalid syscall pointers rejected\n"
msg_bad_pointer_end:

    .section .user.text,"ax"
    .code64

.macro CHK reg, val, code
    movabs $\val, %rcx
    cmp %rcx, \reg
    jne fail_\code
.endm

    .global user_smoke_main
    .type user_smoke_main, @function
user_smoke_main:
    pushfq
    pop %rax
    test $0x200, %rax
    jz fail_19
    mov user_smoke_delay(%rip), %rsi
    test %rsi, %rsi
    jz 2f
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rbp
    movabs $0x3333333333333333, %r12
    movabs $0x4444444444444444, %r13
    movabs $0x5555555555555555, %r14
    movabs $0x6666666666666666, %r15
    rdtsc
    shl $32, %rdx
    or %rdx, %rax
    mov %rax, %rdi
1:
    pause
    rdtsc
    shl $32, %rdx
    or %rdx, %rax
    sub %rdi, %rax
    cmp %rsi, %rax
    jb 1b
    CHK %rbx, 0x1111111111111111, 3
    CHK %rbp, 0x2222222222222222, 4
    CHK %r12, 0x3333333333333333, 5
    CHK %r13, 0x4444444444444444, 6
    CHK %r14, 0x5555555555555555, 7
    CHK %r15, 0x6666666666666666, 8
2:
    /* 1: really running at CPL 3 */
    mov %cs, %ax
    and $3, %ax
    cmp $3, %ax
    jne fail_1

    /* 2: raw SYSCALL getpid; every register except RAX/RCX/R11 survives */
    lea user_saved_rsp(%rip), %rax
    mov %rsp, (%rax)
    movabs $0x1111111111111111, %rbx
    movabs $0x2222222222222222, %rbp
    movabs $0x3333333333333333, %r12
    movabs $0x4444444444444444, %r13
    movabs $0x5555555555555555, %r14
    movabs $0x6666666666666666, %r15
    movabs $0x7777777777777777, %rdi
    movabs $0x8888888888888888, %rsi
    movabs $0x9999999999999999, %rdx
    movabs $0xaaaaaaaaaaaaaaaa, %r8
    movabs $0xbbbbbbbbbbbbbbbb, %r9
    movabs $0xcccccccccccccccc, %r10
    mov $39, %eax
    syscall
    cmp $1, %rax
    jne fail_2
    CHK %rbx, 0x1111111111111111, 3
    CHK %rbp, 0x2222222222222222, 4
    CHK %r12, 0x3333333333333333, 5
    CHK %r13, 0x4444444444444444, 6
    CHK %r14, 0x5555555555555555, 7
    CHK %r15, 0x6666666666666666, 8
    CHK %rdi, 0x7777777777777777, 9
    CHK %rsi, 0x8888888888888888, 10
    CHK %rdx, 0x9999999999999999, 11
    CHK %r8,  0xaaaaaaaaaaaaaaaa, 12
    CHK %r9,  0xbbbbbbbbbbbbbbbb, 13
    CHK %r10, 0xcccccccccccccccc, 14
    lea user_saved_rsp(%rip), %rax
    cmp (%rax), %rsp
    jne fail_15

    /* Reject null, kernel, and range-crossing buffers before kernel access. */
    mov $1, %edi
    xor %esi, %esi
    mov $1, %edx
    mov $1, %eax
    syscall
    cmp $-14, %rax
    jne fail_20

    mov $1, %edi
    movabs $0xffff800000000000, %rsi
    mov $1, %edx
    mov $1, %eax
    syscall
    cmp $-14, %rax
    jne fail_21

    mov $1, %edi
    movabs $__user_stack_end-1, %rsi
    mov $2, %edx
    mov $1, %eax
    syscall
    cmp $-14, %rax
    jne fail_22

    /* The hardware-diagnostic handler must reject before touching its input. */
    mov $63, %eax
    mov $1, %edi
    movabs $0xffff800000000000, %rsi
    mov $8, %edx
    syscall
    cmp $-14, %rax
    jne fail_23

    mov $1, %edi
    mov $2, %esi
    lea msg_bad_pointer(%rip), %rdx
    mov $(msg_bad_pointer_end - msg_bad_pointer), %ecx
    call __syscall
    cmp $(msg_bad_pointer_end - msg_bad_pointer), %rax
    jne fail_24

    /* 3: libc wrapper: __syscall(SYS_getpid) */
    mov $39, %edi
    call __syscall
    cmp $1, %rax
    jne fail_16

    /* 4: __syscall(SYS_write, 1, msg, len) */
    mov $1, %edi
    mov $1, %esi
    lea msg_ok(%rip), %rdx
    mov $(msg_ok_end - msg_ok), %ecx
    call __syscall
    cmp $(msg_ok_end - msg_ok), %rax
    jne fail_17

    /* 5: __syscall(SYS_exit, 0) */
    mov $60, %edi
    xor %esi, %esi
    call __syscall
    jmp fail_18             /* SYS_exit must not return */

.irp code,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24
fail_\code:
    mov $\code, %ebx
    jmp fail_common
.endr

fail_common:
    mov $1, %edi
    mov $2, %esi
    lea msg_fail(%rip), %rdx
    mov $(msg_fail_end - msg_fail), %ecx
    call __syscall
    mov $60, %edi
    mov %rbx, %rsi
    call __syscall
1:  jmp 1b
    .size user_smoke_main, . - user_smoke_main
