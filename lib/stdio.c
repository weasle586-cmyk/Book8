#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} sink_t;

static void emit(sink_t *sink, char value)
{
    if (sink->size != 0 && sink->pos < sink->size - 1u) {
        sink->buf[sink->pos] = value;
    }
    sink->pos++;
}

static void emit_repeat(sink_t *sink, char value, size_t count)
{
    while (count-- != 0) {
        emit(sink, value);
    }
}

static void emit_bytes(sink_t *sink, const char *text, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        emit(sink, text[i]);
    }
}

enum {
    FLAG_LEFT = 1,
    FLAG_PLUS = 2,
    FLAG_SPACE = 4,
    FLAG_ALT = 8,
    FLAG_ZERO = 16
};

typedef struct {
    unsigned flags;
    int width;
    int precision; /* -1 when absent */
} spec_t;

static void emit_padded(sink_t *sink, const spec_t *spec, const char *prefix,
                        size_t prefix_len, const char *body, size_t body_len,
                        size_t zeros)
{
    size_t total = prefix_len + zeros + body_len;
    size_t pad = (spec->width > 0 && (size_t)spec->width > total)
                     ? (size_t)spec->width - total : 0;

    if (!(spec->flags & FLAG_LEFT)) {
        emit_repeat(sink, ' ', pad);
    }
    emit_bytes(sink, prefix, prefix_len);
    emit_repeat(sink, '0', zeros);
    emit_bytes(sink, body, body_len);
    if (spec->flags & FLAG_LEFT) {
        emit_repeat(sink, ' ', pad);
    }
}

static void format_integer(sink_t *sink, const spec_t *spec, unsigned long long value,
                           bool is_signed, bool negative, unsigned base, bool upper)
{
    char digits[72];
    char prefix[3];
    size_t prefix_len = 0;
    size_t count = 0;
    size_t zeros = 0;
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (!(value == 0 && spec->precision == 0)) {
        do {
            digits[count++] = alphabet[value % base];
            value /= base;
        } while (value != 0);
    }
    /* Digits were produced least-significant first; reverse in place. */
    for (size_t i = 0; i < count / 2; i++) {
        char swap = digits[i];
        digits[i] = digits[count - 1 - i];
        digits[count - 1 - i] = swap;
    }

    if (is_signed) {
        if (negative) {
            prefix[prefix_len++] = '-';
        } else if (spec->flags & FLAG_PLUS) {
            prefix[prefix_len++] = '+';
        } else if (spec->flags & FLAG_SPACE) {
            prefix[prefix_len++] = ' ';
        }
    } else if ((spec->flags & FLAG_ALT) && base == 16 && count != 0 &&
               !(count == 1 && digits[0] == '0')) {
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = upper ? 'X' : 'x';
    }

    if (spec->precision >= 0 && (size_t)spec->precision > count) {
        zeros = (size_t)spec->precision - count;
    }
    if ((spec->flags & FLAG_ALT) && base == 8 && zeros == 0 &&
        (count == 0 || digits[0] != '0')) {
        zeros = 1;
    }
    if ((spec->flags & FLAG_ZERO) && !(spec->flags & FLAG_LEFT) &&
        spec->precision < 0 && spec->width > 0 &&
        (size_t)spec->width > prefix_len + count) {
        zeros = (size_t)spec->width - prefix_len - count;
    }
    emit_padded(sink, spec, prefix, prefix_len, digits, count, zeros);
}

int vsnprintf(char *buf, size_t size, const char *format, va_list args)
{
    sink_t sink = { buf, size, 0 };

    while (*format != '\0') {
        spec_t spec = { 0, 0, -1 };
        int length = 0; /* -2 hh, -1 h, 0 int, 1 long, 2 long long, 3 size/ptrdiff/intmax */
        char conv;

        if (*format != '%') {
            emit(&sink, *format++);
            continue;
        }
        format++;

        for (;; format++) {
            if (*format == '-') {
                spec.flags |= FLAG_LEFT;
            } else if (*format == '+') {
                spec.flags |= FLAG_PLUS;
            } else if (*format == ' ') {
                spec.flags |= FLAG_SPACE;
            } else if (*format == '#') {
                spec.flags |= FLAG_ALT;
            } else if (*format == '0') {
                spec.flags |= FLAG_ZERO;
            } else {
                break;
            }
        }
        if (*format == '*') {
            spec.width = va_arg(args, int);
            if (spec.width < 0) {
                spec.flags |= FLAG_LEFT;
                spec.width = -spec.width;
            }
            format++;
        } else {
            while (*format >= '0' && *format <= '9') {
                spec.width = spec.width * 10 + (*format++ - '0');
            }
        }
        if (*format == '.') {
            format++;
            spec.precision = 0;
            if (*format == '*') {
                spec.precision = va_arg(args, int);
                if (spec.precision < 0) {
                    spec.precision = -1;
                }
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    spec.precision = spec.precision * 10 + (*format++ - '0');
                }
            }
        }
        if (*format == 'h') {
            format++;
            length = -1;
            if (*format == 'h') {
                format++;
                length = -2;
            }
        } else if (*format == 'l') {
            format++;
            length = 1;
            if (*format == 'l') {
                format++;
                length = 2;
            }
        } else if (*format == 'z' || *format == 't' || *format == 'j') {
            format++;
            length = 3;
        }

        conv = *format;
        if (conv == '\0') {
            emit(&sink, '%');
            break;
        }
        format++;

        switch (conv) {
        case '%':
            emit(&sink, '%');
            break;
        case 'c': {
            char value = (char)va_arg(args, int);
            emit_padded(&sink, &spec, "", 0, &value, 1, 0);
            break;
        }
        case 's': {
            const char *text = va_arg(args, const char *);
            size_t count;
            if (text == NULL) {
                text = "(null)";
            }
            count = spec.precision >= 0 ? strnlen(text, (size_t)spec.precision)
                                        : strlen(text);
            emit_padded(&sink, &spec, "", 0, text, count, 0);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(args, void *);
            if (value == 0) {
                emit_padded(&sink, &spec, "", 0, "(nil)", 5, 0);
            } else {
                spec.flags |= FLAG_ALT;
                format_integer(&sink, &spec, value, false, false, 16, false);
            }
            break;
        }
        case 'd':
        case 'i': {
            long long value;
            if (length == 2) {
                value = va_arg(args, long long);
            } else if (length == 1 || length == 3) {
                value = va_arg(args, long);
            } else {
                value = va_arg(args, int);
                if (length == -1) {
                    value = (short)value;
                } else if (length == -2) {
                    value = (signed char)value;
                }
            }
            format_integer(&sink, &spec,
                           value < 0 ? 0ull - (unsigned long long)value
                                     : (unsigned long long)value,
                           true, value < 0, 10, false);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long value;
            if (length == 2) {
                value = va_arg(args, unsigned long long);
            } else if (length == 1 || length == 3) {
                value = va_arg(args, unsigned long);
            } else {
                value = va_arg(args, unsigned int);
                if (length == -1) {
                    value = (unsigned short)value;
                } else if (length == -2) {
                    value = (unsigned char)value;
                }
            }
            format_integer(&sink, &spec, value, false, false,
                           conv == 'u' ? 10u : conv == 'o' ? 8u : 16u, conv == 'X');
            break;
        }
        default:
            emit(&sink, '%');
            emit(&sink, conv);
            break;
        }
    }

    if (size != 0) {
        buf[sink.pos < size ? sink.pos : size - 1u] = '\0';
    }
    return sink.pos > (size_t)INT32_MAX ? INT32_MAX : (int)sink.pos;
}

int snprintf(char *buf, size_t size, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(buf, size, format, args);
    va_end(args);
    return result;
}

int vsprintf(char *buf, const char *format, va_list args)
{
    return vsnprintf(buf, SIZE_MAX, format, args);
}

int sprintf(char *buf, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(buf, SIZE_MAX, format, args);
    va_end(args);
    return result;
}

static stdio_putc_fn console_output;

void stdio_set_output(stdio_putc_fn output)
{
    console_output = output;
}

int putchar(int value)
{
    if (console_output != NULL) {
        console_output((char)value);
    }
    return (unsigned char)value;
}

int puts(const char *text)
{
    while (*text != '\0') {
        putchar(*text++);
    }
    putchar('\n');
    return 1;
}

int vprintf(const char *format, va_list args)
{
    char line[256];
    int total = vsnprintf(line, sizeof(line), format, args);
    size_t shown = (size_t)total < sizeof(line) ? (size_t)total : sizeof(line) - 1u;

    for (size_t i = 0; i < shown; i++) {
        putchar(line[i]);
    }
    return total;
}

int printf(const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vprintf(format, args);
    va_end(args);
    return result;
}
