#ifndef BOOK8_USER_ULIB_H
#define BOOK8_USER_ULIB_H

#include <syscall.h>

static inline unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

static inline void uputs(const char *s)
{
    sys_write(1, s, ustrlen(s));
}

static inline void uputnum(long v)
{
    char buf[24];
    int i = sizeof(buf);
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;

    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        buf[--i] = '-';
    uputs(&buf[i]);
}

#endif
