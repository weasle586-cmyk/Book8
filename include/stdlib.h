#ifndef BOOK8_STDLIB_H
#define BOOK8_STDLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Heap: boundary-tag, explicit free list, first-fit, immediate coalescing.
 * Every returned pointer is 16-byte aligned. The first malloc() lazily
 * initializes a built-in page-aligned arena in .bss unless heap_init() was
 * called first with a caller-supplied region.
 */
#define HEAP_PAGE_SIZE 4096u
#define HEAP_DEFAULT_SIZE (1024u * 1024u)
#define HEAP_ALIGNMENT 16u

typedef struct {
    uintptr_t start;      /* page-aligned heap base */
    uintptr_t end;        /* one past the last usable byte */
    size_t total;         /* bytes managed (end - start) */
    size_t used;          /* bytes in allocated blocks (including tags) */
    size_t free_bytes;    /* bytes in free blocks */
    size_t free_blocks;
    size_t used_blocks;
    size_t largest_free;  /* largest block, tags included */
} heap_stats_t;

/* Aligns base up and size down to HEAP_PAGE_SIZE; returns false if too small. */
bool heap_init(void *base, size_t size);
void heap_get_stats(heap_stats_t *stats);
/* Walks every block validating tags and the free list; true when consistent. */
bool heap_check(void);

void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);

int abs(int value);
long labs(long value);
int atoi(const char *text);
long atol(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);

/* Writes value in the given base (2..36; base 10 is signed) and returns buf. */
char *itoa(int value, char *buf, int base);
char *utoa(unsigned long long value, char *buf, int base);
/* Lower-case hex without prefix. Returns buf, or NULL if size is too small. */
char *hex_to_str(uint64_t value, char *buf, size_t size);

#endif
