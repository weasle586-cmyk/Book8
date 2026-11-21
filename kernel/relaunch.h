#ifndef BOOK8_RELAUNCH_H
#define BOOK8_RELAUNCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* In-RAM kernel relaunch: a Multiboot2 ELF64 kernel image is staged in a
 * scratch window, a self-contained plan (page tables, GDT, copy list, rewritten
 * Multiboot2 info and a position-independent stub) is built at the end of the
 * window, and the stub replaces the running kernel image before entering the
 * new one exactly as GRUB would: 32-bit protected mode, paging off,
 * EAX=0x36d76289, EBX=physical MBI address.
 *
 * Plan area layout (BOOK8_RELAUNCH_PLAN_BYTES, 4 KiB aligned):
 *   0x0000 PML4   0x1000 PDPT   0x2000 PD (identity 0..1 GiB, 2 MiB pages)
 *   0x3000 struct book8_relaunch_plan   0x3400 stub code
 *   0x4000 stack (top 0x5000)           0x5000 rewritten MBI */
#define BOOK8_RELAUNCH_PLAN_BYTES 0x10000u
#define BOOK8_RELAUNCH_PLAN_OFFSET 0x3000u
#define BOOK8_RELAUNCH_STUB_OFFSET 0x3400u
#define BOOK8_RELAUNCH_STUB_MAX 0x0c00u
#define BOOK8_RELAUNCH_STACK_TOP 0x5000u
#define BOOK8_RELAUNCH_MBI_OFFSET 0x5000u
#define BOOK8_RELAUNCH_MBI_MAX (BOOK8_RELAUNCH_PLAN_BYTES - BOOK8_RELAUNCH_MBI_OFFSET)
#define BOOK8_RELAUNCH_SEGMENTS 8u
#define BOOK8_RELAUNCH_LOW 0x00100000ull
#define BOOK8_RELAUNCH_HIGH 0x04800000ull
#define BOOK8_RELAUNCH_TOKEN "book8.relaunch="
#define BOOK8_RELAUNCH_CMDLINE_MAX 512u

enum book8_relaunch_result {
    BOOK8_RELAUNCH_OK,
    BOOK8_RELAUNCH_BAD_ARGUMENT,
    BOOK8_RELAUNCH_NOT_ELF64,
    BOOK8_RELAUNCH_BAD_SEGMENT,
    BOOK8_RELAUNCH_NO_MULTIBOOT,
    BOOK8_RELAUNCH_BAD_ENTRY,
    BOOK8_RELAUNCH_BAD_MBI,
    BOOK8_RELAUNCH_MBI_TOO_LARGE
};

struct book8_relaunch_segment {
    uint64_t destination, source, file_bytes, memory_bytes;
};

/* Consumed by src/relaunch_stub.S at fixed offsets; keep in sync. */
struct book8_relaunch_plan {
    uint64_t cr3;            /* 0 */
    uint64_t entry;          /* 8 */
    uint64_t mbi_source;     /* 16 */
    uint64_t mbi_destination;/* 24 */
    uint64_t mbi_bytes;      /* 32 */
    uint64_t segment_count;  /* 40 */
    uint64_t stack_top;      /* 48 */
    uint64_t gdt[4];         /* 56: null, code64 0x08, data 0x10, code32 0x18 */
    uint16_t gdtr_pad[3];    /* 88 */
    uint16_t gdtr_limit;     /* 94: lgdt operand is limit + base at 94..103 */
    uint64_t gdtr_base;      /* 96 */
    struct book8_relaunch_segment segments[BOOK8_RELAUNCH_SEGMENTS]; /* 104 */
};

struct book8_relaunch_info {
    uint64_t entry, image_low, image_high, mbi_destination;
    uint32_t segments, mbi_bytes, relaunch_count;
};

/* Validates the staged ELF (ELF64 x86-64 EXEC, PT_LOAD segments inside
 * [1 MiB, 72 MiB), Multiboot2 header in the first 32 KiB, entry < 4 GiB inside
 * a loaded segment) and builds the plan. `plan` is the plan area as seen by
 * the caller; `plan_physical`/`elf_physical` are the addresses the stub uses.
 * Module tags are dropped and the command line's book8.relaunch=N token is
 * replaced by the new count. */
enum book8_relaunch_result book8_relaunch_prepare(const uint8_t *elf, size_t elf_bytes,
    uint64_t elf_physical, const uint8_t *mbi, uint8_t *plan, uint64_t plan_physical,
    struct book8_relaunch_info *info);
/* Rewrites a command line: drops every book8.relaunch= token, appends the new
 * one. Returns the NUL-inclusive length or 0 when capacity is insufficient. */
size_t book8_relaunch_cmdline(const char *in, size_t in_bytes, uint32_t count,
    char *out, size_t capacity);
/* Parses book8.relaunch=N from a (not necessarily NUL-terminated) command line. */
uint32_t book8_relaunch_count(const char *cmdline, size_t bytes);
const char *book8_relaunch_result_string(enum book8_relaunch_result result);

/* Freestanding only: copies the stub into the plan area and enters it. */
void book8_relaunch_enter(uint8_t *plan, uint64_t plan_physical) __attribute__((noreturn));
extern const uint8_t book8_relaunch_stub_start[], book8_relaunch_stub_end[];

#endif
