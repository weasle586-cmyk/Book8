/* Ring 3 entry: SysV initial stack (argc, argv[], NULL, envp[], NULL, auxv). */
    .section .text
    .global _start
    .type _start, @function
_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi         /* argc */
    lea 8(%rsp), %rsi        /* argv */
    and $-16, %rsp
    call main
    mov %eax, %edi
    call sys_exit
1:  jmp 1b
    .size _start, . - _start
