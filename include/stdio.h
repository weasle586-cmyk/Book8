#ifndef BOOK8_STDIO_H
#define BOOK8_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Supported: %d %i %u %x %X %o %c %s %p %% with flags '-' '+' ' ' '#' '0',
 * width and precision (number or *), and length modifiers hh h l ll z t j.
 * Floating point is not supported. snprintf-family functions always
 * NUL-terminate when size > 0 and return the length that would have been
 * written (C99 semantics).
 */
int vsnprintf(char *buf, size_t size, const char *format, va_list args);
int snprintf(char *buf, size_t size, const char *format, ...);
int vsprintf(char *buf, const char *format, va_list args);
int sprintf(char *buf, const char *format, ...);

/* Console output goes through a hook; printf is a no-op until one is set. */
typedef void (*stdio_putc_fn)(char value);
void stdio_set_output(stdio_putc_fn output);
int putchar(int value);
int puts(const char *text);
int vprintf(const char *format, va_list args);
int printf(const char *format, ...);

#endif
