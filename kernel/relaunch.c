#include "relaunch.h"

#include <string.h>

_Static_assert(sizeof(struct book8_relaunch_plan) == 104u
    + BOOK8_RELAUNCH_SEGMENTS * sizeof(struct book8_relaunch_segment), "plan size");
_Static_assert(__builtin_offsetof(struct book8_relaunch_plan, gdt) == 56u, "plan gdt");
_Static_assert(__builtin_offsetof(struct book8_relaunch_plan, gdtr_limit) == 94u, "plan gdtr");
_Static_assert(__builtin_offsetof(struct book8_relaunch_plan, gdtr_base) == 96u, "plan gdtr base");
_Static_assert(__builtin_offsetof(struct book8_relaunch_plan, segments) == 104u, "plan segs");
_Static_assert(BOOK8_RELAUNCH_PLAN_OFFSET + sizeof(struct book8_relaunch_plan)
    <= BOOK8_RELAUNCH_STUB_OFFSET, "plan overlaps stub");

#define ELF_PT_LOAD 1u
#define MB2_MAGIC 0xe85250d6u
#define MB2_SEARCH 32768u
#define MB2_TAG_END 0u
#define MB2_TAG_CMDLINE 1u
#define MB2_TAG_MODULE 3u
#define MB2_INPUT_MAX 0x100000u
#define IDENTITY_LIMIT 0x40000000ull

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8u));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)le16(p) | ((uint32_t)le16(p + 2u) << 16u);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4u) << 32u);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4u, (uint32_t)(v >> 32u));
}

static bool overlaps(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    return a < b + b_bytes && b < a + a_bytes;
}

static bool token_at(const char *text, size_t bytes, size_t i)
{
    size_t length = sizeof(BOOK8_RELAUNCH_TOKEN) - 1u;
    return (i == 0u || text[i - 1u] == ' ') && i + length <= bytes
        && memcmp(text + i, BOOK8_RELAUNCH_TOKEN, length) == 0;
}

uint32_t book8_relaunch_count(const char *cmdline, size_t bytes)
{
    const size_t length = sizeof(BOOK8_RELAUNCH_TOKEN) - 1u;
    uint32_t count = 0u;
    if (cmdline == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < bytes && cmdline[i] != '\0'; ++i) {
        if (!token_at(cmdline, bytes, i)) {
            continue;
        }
        uint32_t value = 0u;
        size_t j = i + length, digits = 0u;
        while (j < bytes && cmdline[j] >= '0' && cmdline[j] <= '9' && digits < 9u) {
            value = value * 10u + (uint32_t)(cmdline[j] - '0');
            ++j;
            ++digits;
        }
        if (digits != 0u && (j == bytes || cmdline[j] == ' ' || cmdline[j] == '\0')) {
            count = value;
        }
    }
    return count;
}

size_t book8_relaunch_cmdline(const char *in, size_t in_bytes, uint32_t count,
    char *out, size_t capacity)
{
    size_t used = 0u, i = 0u;
    char digits[10];
    size_t digit_count = 0u;
    if (out == NULL || capacity == 0u || (in == NULL && in_bytes != 0u)) {
        return 0u;
    }
    out[0] = '\0';
    while (i < in_bytes && in[i] != '\0') {
        if (in[i] == ' ') {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < in_bytes && in[i] != '\0' && in[i] != ' ') {
            ++i;
        }
        if (token_at(in, in_bytes, start)) {
            continue;
        }
        size_t length = i - start;
        if (used + (used != 0u) + length >= capacity) {
            out[0] = '\0';
            return 0u;
        }
        if (used != 0u) {
            out[used++] = ' ';
        }
        memcpy(out + used, in + start, length);
        used += length;
    }
    do {
        digits[digit_count++] = (char)('0' + count % 10u);
        count /= 10u;
    } while (count != 0u);
    size_t token = sizeof(BOOK8_RELAUNCH_TOKEN) - 1u;
    if (used + (used != 0u) + token + digit_count >= capacity) {
        out[0] = '\0';
        return 0u;
    }
    if (used != 0u) {
        out[used++] = ' ';
    }
    memcpy(out + used, BOOK8_RELAUNCH_TOKEN, token);
    used += token;
    while (digit_count != 0u) {
        out[used++] = digits[--digit_count];
    }
    out[used] = '\0';
    return used + 1u;
}

static bool has_multiboot2(const uint8_t *elf, size_t bytes)
{
    size_t limit = bytes < MB2_SEARCH ? bytes : MB2_SEARCH;
    for (size_t i = 0u; i + 16u <= limit; i += 8u) {
        uint32_t magic = le32(elf + i), arch = le32(elf + i + 4u);
        uint32_t length = le32(elf + i + 8u), checksum = le32(elf + i + 12u);
        if (magic == MB2_MAGIC && arch == 0u && length >= 16u
            && (uint32_t)(magic + arch + length + checksum) == 0u) {
            return true;
        }
    }
    return false;
}

/* Copies MBI tags except cmdline into out, cmdline first. Module tags are
 * retained so GRUB-provided payloads such as book8-hashes survive relaunch. */
static enum book8_relaunch_result rewrite_mbi(const uint8_t *mbi, uint8_t *out,
    uint32_t *out_bytes, uint32_t *new_count)
{
    uint32_t total = le32(mbi);
    const char *cmdline = NULL;
    size_t cmdline_bytes = 0u;
    if (total < 16u || total > MB2_INPUT_MAX || (total & 7u) != 0u) {
        return BOOK8_RELAUNCH_BAD_MBI;
    }
    bool ended = false;
    for (uint32_t offset = 8u; offset + 8u <= total;) {
        uint32_t type = le32(mbi + offset), size = le32(mbi + offset + 4u);
        if (size < 8u || size > total - offset) {
            return BOOK8_RELAUNCH_BAD_MBI;
        }
        if (type == MB2_TAG_END) {
            ended = true;
            break;
        }
        if (type == MB2_TAG_CMDLINE) {
            cmdline = (const char *)mbi + offset + 8u;
            cmdline_bytes = size - 8u;
        }
        offset += (size + 7u) & ~7u;
    }
    if (!ended) {
        return BOOK8_RELAUNCH_BAD_MBI;
    }
    char line[BOOK8_RELAUNCH_CMDLINE_MAX];
    *new_count = book8_relaunch_count(cmdline, cmdline_bytes) + 1u;
    size_t line_bytes = book8_relaunch_cmdline(cmdline, cmdline_bytes, *new_count,
        line, sizeof(line));
    if (line_bytes == 0u) {
        return BOOK8_RELAUNCH_MBI_TOO_LARGE;
    }
    uint32_t used = 8u;
    uint32_t tag = (uint32_t)(8u + line_bytes);
    if (used + ((tag + 7u) & ~7u) > BOOK8_RELAUNCH_MBI_MAX) {
        return BOOK8_RELAUNCH_MBI_TOO_LARGE;
    }
    memset(out, 0, BOOK8_RELAUNCH_MBI_MAX);
    put32(out + used, MB2_TAG_CMDLINE);
    put32(out + used + 4u, tag);
    memcpy(out + used + 8u, line, line_bytes);
    used += (tag + 7u) & ~7u;
    for (uint32_t offset = 8u;;) {
        uint32_t type = le32(mbi + offset), size = le32(mbi + offset + 4u);
        if (type == MB2_TAG_END) {
            break;
        }
        uint32_t padded = (size + 7u) & ~7u;
        if (type != MB2_TAG_CMDLINE) {
            if (used + padded + 8u > BOOK8_RELAUNCH_MBI_MAX) {
                return BOOK8_RELAUNCH_MBI_TOO_LARGE;
            }
            memcpy(out + used, mbi + offset, size);
            used += padded;
        }
        offset += padded;
    }
    if (used + 8u > BOOK8_RELAUNCH_MBI_MAX) {
        return BOOK8_RELAUNCH_MBI_TOO_LARGE;
    }
    put32(out + used, MB2_TAG_END);
    put32(out + used + 4u, 8u);
    used += 8u;
    put32(out, used);
    put32(out + 4u, 0u);
    *out_bytes = used;
    return BOOK8_RELAUNCH_OK;
}

enum book8_relaunch_result book8_relaunch_prepare(const uint8_t *elf, size_t elf_bytes,
    uint64_t elf_physical, const uint8_t *mbi, uint8_t *plan_area, uint64_t plan_physical,
    struct book8_relaunch_info *info)
{
    struct book8_relaunch_plan plan;
    if (elf == NULL || mbi == NULL || plan_area == NULL || info == NULL
        || (plan_physical & 0xfffu) != 0u
        || plan_physical + BOOK8_RELAUNCH_PLAN_BYTES > IDENTITY_LIMIT
        || elf_physical + elf_bytes > IDENTITY_LIMIT
        || overlaps(elf_physical, elf_bytes, plan_physical, BOOK8_RELAUNCH_PLAN_BYTES)) {
        return BOOK8_RELAUNCH_BAD_ARGUMENT;
    }
    memset(info, 0, sizeof(*info));
    memset(&plan, 0, sizeof(plan));
    if (elf_bytes < 64u || memcmp(elf, "\177ELF", 4u) != 0 || elf[4] != 2u
        || elf[5] != 1u || elf[6] != 1u || le16(elf + 16u) != 2u
        || le16(elf + 18u) != 62u || le16(elf + 54u) != 56u) {
        return BOOK8_RELAUNCH_NOT_ELF64;
    }
    uint64_t entry = le64(elf + 24u), phoff = le64(elf + 32u);
    uint16_t phnum = le16(elf + 56u);
    if (phnum == 0u || phoff > elf_bytes || (uint64_t)phnum * 56u > elf_bytes - phoff) {
        return BOOK8_RELAUNCH_NOT_ELF64;
    }
    uint64_t low = UINT64_MAX, high = 0u;
    bool entry_ok = false;
    for (uint16_t i = 0u; i < phnum; ++i) {
        const uint8_t *ph = elf + phoff + (size_t)i * 56u;
        if (le32(ph) != ELF_PT_LOAD) {
            continue;
        }
        uint64_t offset = le64(ph + 8u), vaddr = le64(ph + 16u), paddr = le64(ph + 24u);
        uint64_t file_bytes = le64(ph + 32u), memory_bytes = le64(ph + 40u);
        if (memory_bytes == 0u) {
            continue;
        }
        if (plan.segment_count == BOOK8_RELAUNCH_SEGMENTS || vaddr != paddr
            || file_bytes > memory_bytes || offset > elf_bytes
            || file_bytes > elf_bytes - offset || paddr < BOOK8_RELAUNCH_LOW
            || paddr >= BOOK8_RELAUNCH_HIGH
            || memory_bytes > BOOK8_RELAUNCH_HIGH - paddr) {
            return BOOK8_RELAUNCH_BAD_SEGMENT;
        }
        for (uint64_t j = 0u; j < plan.segment_count; ++j) {
            const struct book8_relaunch_segment *s = &plan.segments[j];
            if (overlaps(paddr, memory_bytes, s->destination, s->memory_bytes)) {
                return BOOK8_RELAUNCH_BAD_SEGMENT;
            }
        }
        struct book8_relaunch_segment *segment = &plan.segments[plan.segment_count++];
        segment->destination = paddr;
        segment->source = elf_physical + offset;
        segment->file_bytes = file_bytes;
        segment->memory_bytes = memory_bytes;
        low = paddr < low ? paddr : low;
        high = paddr + memory_bytes > high ? paddr + memory_bytes : high;
        if (entry >= vaddr && entry - vaddr < memory_bytes) {
            entry_ok = true;
        }
    }
    if (plan.segment_count == 0u) {
        return BOOK8_RELAUNCH_BAD_SEGMENT;
    }
    if (!has_multiboot2(elf, elf_bytes)) {
        return BOOK8_RELAUNCH_NO_MULTIBOOT;
    }
    if (!entry_ok || entry > UINT32_MAX) {
        return BOOK8_RELAUNCH_BAD_ENTRY;
    }
    uint8_t *mbi_out = plan_area + BOOK8_RELAUNCH_MBI_OFFSET;
    uint32_t mbi_bytes = 0u, count = 0u;
    enum book8_relaunch_result result = rewrite_mbi(mbi, mbi_out, &mbi_bytes, &count);
    if (result != BOOK8_RELAUNCH_OK) {
        return result;
    }
    uint64_t mbi_destination = (high + 0xfffu) & ~0xfffull;
    if (mbi_destination + mbi_bytes > BOOK8_RELAUNCH_HIGH) {
        return BOOK8_RELAUNCH_MBI_TOO_LARGE;
    }
    /* The stub reads the staged ELF and plan while overwriting low memory. */
    if (overlaps(BOOK8_RELAUNCH_LOW, mbi_destination + mbi_bytes - BOOK8_RELAUNCH_LOW,
            elf_physical, elf_bytes)
        || overlaps(BOOK8_RELAUNCH_LOW, mbi_destination + mbi_bytes - BOOK8_RELAUNCH_LOW,
            plan_physical, BOOK8_RELAUNCH_PLAN_BYTES)) {
        return BOOK8_RELAUNCH_BAD_ARGUMENT;
    }
    memset(plan_area, 0, BOOK8_RELAUNCH_PLAN_OFFSET);
    put64(plan_area, (plan_physical + 0x1000u) | 3u);
    put64(plan_area + 0x1000u, (plan_physical + 0x2000u) | 3u);
    for (uint32_t i = 0u; i < 512u; ++i) {
        put64(plan_area + 0x2000u + i * 8u, ((uint64_t)i << 21u) | 0x83u);
    }
    plan.cr3 = plan_physical;
    plan.entry = entry;
    plan.mbi_source = plan_physical + BOOK8_RELAUNCH_MBI_OFFSET;
    plan.mbi_destination = mbi_destination;
    plan.mbi_bytes = mbi_bytes;
    plan.stack_top = plan_physical + BOOK8_RELAUNCH_STACK_TOP;
    plan.gdt[1] = 0x00af9a000000ffffull;
    plan.gdt[2] = 0x00cf92000000ffffull;
    plan.gdt[3] = 0x00cf9a000000ffffull;
    plan.gdtr_limit = (uint16_t)(sizeof(plan.gdt) - 1u);
    plan.gdtr_base = plan_physical + BOOK8_RELAUNCH_PLAN_OFFSET
        + __builtin_offsetof(struct book8_relaunch_plan, gdt);
    memset(plan_area + BOOK8_RELAUNCH_PLAN_OFFSET, 0,
        BOOK8_RELAUNCH_MBI_OFFSET - BOOK8_RELAUNCH_PLAN_OFFSET);
    memcpy(plan_area + BOOK8_RELAUNCH_PLAN_OFFSET, &plan, sizeof(plan));
    info->entry = entry;
    info->image_low = low;
    info->image_high = high;
    info->mbi_destination = mbi_destination;
    info->segments = (uint32_t)plan.segment_count;
    info->mbi_bytes = mbi_bytes;
    info->relaunch_count = count;
    return BOOK8_RELAUNCH_OK;
}

const char *book8_relaunch_result_string(enum book8_relaunch_result result)
{
    switch (result) {
    case BOOK8_RELAUNCH_OK: return "ok";
    case BOOK8_RELAUNCH_BAD_ARGUMENT: return "bad staging addresses";
    case BOOK8_RELAUNCH_NOT_ELF64: return "not an x86-64 ELF64 executable";
    case BOOK8_RELAUNCH_BAD_SEGMENT: return "PT_LOAD outside 1-72 MiB or malformed";
    case BOOK8_RELAUNCH_NO_MULTIBOOT: return "no Multiboot2 header";
    case BOOK8_RELAUNCH_BAD_ENTRY: return "entry not inside a loaded segment";
    case BOOK8_RELAUNCH_BAD_MBI: return "boot information malformed";
    case BOOK8_RELAUNCH_MBI_TOO_LARGE: return "boot information too large";
    default: return "unknown";
    }
}
