#ifndef BOOK8_STRING_H
#define BOOK8_STRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memchr(const void *block, int value, size_t count);

size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t count);

#endif
