#include <limits.h>
#include <stdlib.h>

static bool is_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

/* Parses sign, prefix and digits. Returns the magnitude; sets overflow. */
static unsigned long parse_magnitude(const char *text, char **end, int base,
                                     bool *negative, bool *overflow)
{
    const char *cursor = text;
    unsigned long value = 0;
    bool any = false;

    *negative = false;
    *overflow = false;
    while (is_space(*cursor)) {
        cursor++;
    }
    if (*cursor == '-' || *cursor == '+') {
        *negative = *cursor == '-';
        cursor++;
    }
    if (base != 0 && (base < 2 || base > 36)) {
        if (end != NULL) {
            *end = (char *)text;
        }
        return 0;
    }
    if ((base == 0 || base == 16) && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X') && digit_value(cursor[2]) < 16) {
        cursor += 2;
        base = 16;
    } else if (base == 0) {
        base = cursor[0] == '0' ? 8 : 10;
    }
    for (;; cursor++) {
        int digit = digit_value(*cursor);
        if (digit >= base) {
            break;
        }
        any = true;
        if (value > (ULONG_MAX - (unsigned long)digit) / (unsigned long)base) {
            *overflow = true;
            value = ULONG_MAX;
        } else if (!*overflow) {
            value = value * (unsigned long)base + (unsigned long)digit;
        }
    }
    if (end != NULL) {
        *end = (char *)(any ? cursor : text);
    }
    return value;
}

long strtol(const char *text, char **end, int base)
{
    bool negative;
    bool overflow;
    unsigned long magnitude = parse_magnitude(text, end, base, &negative, &overflow);

    if (negative) {
        if (overflow || magnitude > (unsigned long)LONG_MAX + 1ul) {
            return LONG_MIN;
        }
        return (long)(0ul - magnitude);
    }
    if (overflow || magnitude > (unsigned long)LONG_MAX) {
        return LONG_MAX;
    }
    return (long)magnitude;
}

unsigned long strtoul(const char *text, char **end, int base)
{
    bool negative;
    bool overflow;
    unsigned long magnitude = parse_magnitude(text, end, base, &negative, &overflow);

    if (overflow) {
        return ULONG_MAX;
    }
    return negative ? 0ul - magnitude : magnitude;
}

int atoi(const char *text)
{
    return (int)strtol(text, NULL, 10);
}

long atol(const char *text)
{
    return strtol(text, NULL, 10);
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

char *utoa(unsigned long long value, char *buf, int base)
{
    char scratch[65];
    size_t count = 0;
    size_t out = 0;

    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }
    do {
        unsigned digit = (unsigned)(value % (unsigned)base);
        scratch[count++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value /= (unsigned)base;
    } while (value != 0);
    while (count != 0) {
        buf[out++] = scratch[--count];
    }
    buf[out] = '\0';
    return buf;
}

char *itoa(int value, char *buf, int base)
{
    if (base == 10 && value < 0) {
        buf[0] = '-';
        utoa(0ull - (unsigned long long)(long long)value, buf + 1, base);
        return buf;
    }
    return utoa((unsigned)value, buf, base);
}

char *hex_to_str(uint64_t value, char *buf, size_t size)
{
    char scratch[17];
    size_t count = 0;

    do {
        unsigned digit = (unsigned)(value & 0xFu);
        scratch[count++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value >>= 4;
    } while (value != 0);
    if (size < count + 1) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        buf[i] = scratch[count - 1 - i];
    }
    buf[count] = '\0';
    return buf;
}
