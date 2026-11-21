/* long __syscall(long num, a1, a2, a3, a4, a5, a6)
 *
 * C ABI in : RDI=num RSI=a1 RDX=a2 RCX=a3 R8=a4 R9=a5 [RSP+8]=a6
 * Trap  ABI: RAX=num RDI=a1 RSI=a2 RDX=a3 R10=a4 R8=a5 R9=a6
 * The shifts below are ordered so no source register is overwritten early.
 * Returns the raw kernel value in RAX (negative errno on failure).
 */
    .section .user.text,"ax"   /* linked into the U/S=1 code pages */
    .global __syscall
    .type __syscall, @function
__syscall:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    mov 8(%rsp), %r9
    syscall
    ret
    .size __syscall, . - __syscall
