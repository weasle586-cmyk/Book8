#include <string.h>

void *memcpy(void *dest, const void *src, size_t count)
{
    void *result = dest;
    __asm__ volatile ("rep movsb"
                      : "+D"(dest), "+S"(src), "+c"(count)
                      :
                      : "memory");
    return result;
}

void *memmove(void *dest, const void *src, size_t count)
{
    void *result = dest;
    if ((const unsigned char *)dest <= (const unsigned char *)src ||
        (const unsigned char *)dest >= (const unsigned char *)src + count) {
        __asm__ volatile ("rep movsb"
                          : "+D"(dest), "+S"(src), "+c"(count)
                          :
                          : "memory");
    } else {
        /* Overlap with dest above src: copy backwards. */
        dest = (unsigned char *)dest + count - 1;
        src = (const unsigned char *)src + count - 1;
        __asm__ volatile ("std\n\trep movsb\n\tcld"
                          : "+D"(dest), "+S"(src), "+c"(count)
                          :
                          : "memory", "cc");
    }
    return result;
}

void *memset(void *dest, int value, size_t count)
{
    void *result = dest;
    __asm__ volatile ("rep stosb"
                      : "+D"(dest), "+c"(count)
                      : "a"((unsigned char)value)
                      : "memory");
    return result;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}

void *memchr(const void *block, int value, size_t count)
{
    const unsigned char *bytes = block;
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] == (unsigned char)value) {
            return (void *)(bytes + i);
        }
    }
    return NULL;
}

size_t strlen(const char *text)
{
    const char *cursor = text;
    while (*cursor != '\0') {
        cursor++;
    }
    return (size_t)(cursor - text);
}

size_t strnlen(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        length++;
    }
    return length;
}

char *strcpy(char *dest, const char *src)
{
    char *result = dest;
    while ((*dest++ = *src++) != '\0') {
    }
    return result;
}

char *strncpy(char *dest, const char *src, size_t count)
{
    size_t i = 0;
    for (; i < count && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < count; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a != b) {
            return (int)a - (int)b;
        }
        if (a == '\0') {
            break;
        }
    }
    return 0;
}

char *strchr(const char *text, int value)
{
    char wanted = (char)value;
    for (;; text++) {
        if (*text == wanted) {
            return (char *)text;
        }
        if (*text == '\0') {
            return NULL;
        }
    }
}

char *strrchr(const char *text, int value)
{
    char wanted = (char)value;
    const char *found = NULL;
    for (;; text++) {
        if (*text == wanted) {
            found = text;
        }
        if (*text == '\0') {
            return (char *)found;
        }
    }
}

char *strcat(char *dest, const char *src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

char *strncat(char *dest, const char *src, size_t count)
{
    char *end = dest + strlen(dest);
    size_t i = 0;
    for (; i < count && src[i] != '\0'; i++) {
        end[i] = src[i];
    }
    end[i] = '\0';
    return dest;
}
