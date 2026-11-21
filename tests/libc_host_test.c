/*
 * Host-side build check for lib/: links the freestanding libc sources into a
 * static, startfile-less Linux x86-64 program (raw syscalls only) so it cannot
 * collide with or silently use the host C library.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "libc_selftest.h"

static long sys_write(const char *text, size_t length)
{
    long result;
    __asm__ volatile ("syscall"
                      : "=a"(result)
                      : "a"(1L), "D"(1L), "S"(text), "d"(length)
                      : "rcx", "r11", "memory");
    return result;
}

static void out(const char *text)
{
    sys_write(text, strlen(text));
}

static void out_char(char value)
{
    sys_write(&value, 1);
}

__attribute__((noreturn)) static void sys_exit(int code)
{
    for (;;) {
        __asm__ volatile ("syscall" : : "a"(60L), "D"((long)code) : "rcx", "r11", "memory");
    }
}

__attribute__((noreturn, used)) void libc_host_main(void)
{
    int failed;

    stdio_set_output(out_char);
    failed = libc_selftest(out);
    printf("libc self-test: %d checks, %d failed\n", libc_selftest_checks(), failed);
    sys_exit(failed == 0 ? 0 : 1);
}

__attribute__((naked, used)) void _start(void)
{
    __asm__ volatile ("and $-16, %rsp\n\tcall libc_host_main");
}
