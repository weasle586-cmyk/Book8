#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/relaunch.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

static uint8_t elf[0x6000];
static uint8_t mbi[4096];
static uint8_t plan[BOOK8_RELAUNCH_PLAN_BYTES];
/* Mirrors BOOK8_NETBOOT_ADDRESS and BOOK8_TFTP_CAPACITY in net/net.h. */
static const uint64_t NETBOOT_PHYS = 0x05000000u;
static const uint64_t NETBOOT_CAPACITY = 8u * 1024u * 1024u;
static const uint64_t ELF_PHYS = NETBOOT_PHYS;
static const uint64_t PLAN_PHYS = NETBOOT_PHYS + NETBOOT_CAPACITY
    - BOOK8_RELAUNCH_PLAN_BYTES;

static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2, (uint16_t)(v >> 16)); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t g32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t g64(const uint8_t *p) { return g32(p) | ((uint64_t)g32(p + 4) << 32); }

static void phdr(unsigned i, uint32_t type, uint64_t off, uint64_t addr,
    uint64_t filesz, uint64_t memsz)
{
    uint8_t *p = elf + 64u + i * 56u;
    p32(p, type);
    p64(p + 8, off);
    p64(p + 16, addr);
    p64(p + 24, addr);
    p64(p + 32, filesz);
    p64(p + 40, memsz);
}

static void make_elf(void)
{
    memset(elf, 0, sizeof(elf));
    memcpy(elf, "\177ELF", 4);
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    p16(elf + 16, 2); p16(elf + 18, 62);
    p64(elf + 24, 0x101000u);
    p64(elf + 32, 64u);
    p16(elf + 54, 56u);
    p16(elf + 56, 3u);
    phdr(0, 1u, 0x1000u, 0x100000u, 0x2000u, 0x2000u);
    phdr(1, 4u, 0, 0, 0, 0); /* PT_NOTE ignored */
    phdr(2, 1u, 0x3000u, 0x103000u, 0x1000u, 0x200000u);
    uint8_t *mb = elf + 0x1000u;
    p32(mb, 0xe85250d6u);
    p32(mb + 4, 0u);
    p32(mb + 8, 24u);
    p32(mb + 12, (uint32_t)-(0xe85250d6u + 24u));
}

static size_t tag(size_t off, uint32_t type, const void *data, uint32_t length)
{
    p32(mbi + off, type);
    p32(mbi + off + 4, 8u + length);
    memcpy(mbi + off + 8, data, length);
    return off + ((8u + length + 7u) & ~7u);
}

static void make_mbi(const char *cmdline)
{
    memset(mbi, 0, sizeof(mbi));
    size_t off = 8u;
    uint8_t meminfo[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    off = tag(off, 4u, meminfo, sizeof(meminfo));
    if (cmdline != NULL) {
        off = tag(off, 1u, cmdline, (uint32_t)strlen(cmdline) + 1u);
    }
    uint8_t module[16] = {0};
    off = tag(off, 3u, module, sizeof(module));
    uint8_t fb[13] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    off = tag(off, 8u, fb, sizeof(fb));
    p32(mbi + off, 0u);
    p32(mbi + off + 4, 8u);
    off += 8u;
    p32(mbi, (uint32_t)off);
}

static const uint8_t *find_tag(const uint8_t *m, uint32_t type)
{
    for (uint32_t off = 8u; off < g32(m);) {
        uint32_t t = g32(m + off), size = g32(m + off + 4);
        if (t == type) {
            return m + off;
        }
        if (t == 0u) {
            return NULL;
        }
        off += (size + 7u) & ~7u;
    }
    return NULL;
}

static void cmdline_tests(void)
{
    char out[64];
    CHECK(book8_relaunch_count("a book8.relaunch=7 b", 20u) == 7u);
    CHECK(book8_relaunch_count("book8.relaunch=12", 17u) == 12u);
    CHECK(book8_relaunch_count("xbook8.relaunch=5", 17u) == 0u);
    CHECK(book8_relaunch_count("book8.relaunch=5x", 17u) == 0u);
    CHECK(book8_relaunch_count("book8.relaunch=", 15u) == 0u);
    CHECK(book8_relaunch_count("book8.relaunch=12", 16u) == 1u);
    CHECK(book8_relaunch_count(NULL, 0u) == 0u);
    CHECK(book8_relaunch_cmdline("book8.autostart=0 book8.relaunch=3  quiet", 41u, 4u,
        out, sizeof(out)) == 41u);
    CHECK(strcmp(out, "book8.autostart=0 quiet book8.relaunch=4") == 0);
    CHECK(book8_relaunch_cmdline(NULL, 0u, 1u, out, sizeof(out)) == 17u);
    CHECK(strcmp(out, "book8.relaunch=1") == 0);
    CHECK(book8_relaunch_cmdline("abc", 3u, 10u, out, 18u) == 0u && out[0] == '\0');
    CHECK(book8_relaunch_cmdline("abc", 3u, 10u, out, 22u) == 22u);
    CHECK(book8_relaunch_cmdline("abc", 3u, 10u, out, 21u) == 0u);
}

static void plan_tests(void)
{
    struct book8_relaunch_info info;
    make_elf();
    make_mbi("/boot/book8.elf book8.autostart=0 book8.relaunch=2");
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_OK);
    CHECK(info.entry == 0x101000u && info.segments == 2u && info.relaunch_count == 3u);
    CHECK(info.image_low == 0x100000u && info.image_high == 0x303000u);
    CHECK(info.mbi_destination == 0x303000u);
    CHECK(g64(plan) == ((PLAN_PHYS + 0x1000u) | 3u));
    CHECK(g64(plan + 0x1000u) == ((PLAN_PHYS + 0x2000u) | 3u));
    CHECK(g64(plan + 0x2000u) == 0x83u && g64(plan + 0x2000u + 511u * 8u) == 0x3fe00083u);
    const uint8_t *pl = plan + BOOK8_RELAUNCH_PLAN_OFFSET;
    CHECK(g64(pl + 0) == PLAN_PHYS && g64(pl + 8) == 0x101000u);
    CHECK(g64(pl + 16) == PLAN_PHYS + BOOK8_RELAUNCH_MBI_OFFSET);
    CHECK(g64(pl + 24) == 0x303000u && g64(pl + 32) == info.mbi_bytes);
    CHECK(g64(pl + 40) == 2u && g64(pl + 48) == PLAN_PHYS + BOOK8_RELAUNCH_STACK_TOP);
    CHECK(g64(pl + 64) == 0x00af9a000000ffffull && g64(pl + 80) == 0x00cf9a000000ffffull);
    /* lgdt operand as the stub reads it: 2-byte limit immediately followed by base. */
    CHECK((pl[94] | (pl[95] << 8)) == 31u);
    CHECK(g64(pl + 96) == PLAN_PHYS + BOOK8_RELAUNCH_PLAN_OFFSET + 56u);
    CHECK(g64(pl + 104) == 0x100000u && g64(pl + 112) == ELF_PHYS + 0x1000u);
    CHECK(g64(pl + 120) == 0x2000u && g64(pl + 128) == 0x2000u);
    CHECK(g64(pl + 136) == 0x103000u && g64(pl + 144) == ELF_PHYS + 0x3000u);
    CHECK(g64(pl + 152) == 0x1000u && g64(pl + 160) == 0x200000u);
    const uint8_t *m = plan + BOOK8_RELAUNCH_MBI_OFFSET;
    CHECK(g32(m) == info.mbi_bytes && (info.mbi_bytes & 7u) == 0u && g32(m + 4) == 0u);
    const uint8_t *cmd = find_tag(m, 1u);
    CHECK(cmd != NULL && cmd == m + 8u);
    CHECK(strcmp((const char *)cmd + 8,
        "/boot/book8.elf book8.autostart=0 book8.relaunch=3") == 0);
    /* GRUB modules (including the boot hash manifest) survive the restart. */
    const uint8_t *module = find_tag(m, 3u);
    CHECK(module != NULL && g32(module + 4) == 24u);
    for (uint32_t i = 0u; i < 16u; ++i) {
        CHECK(module[8u + i] == 0u);
    }
    const uint8_t *meminfo = find_tag(m, 4u), *fb = find_tag(m, 8u);
    CHECK(meminfo != NULL && meminfo[8] == 1u && meminfo[15] == 8u);
    CHECK(fb != NULL && g32(fb + 4) == 21u && fb[20] == 9u);
    CHECK(book8_relaunch_count((const char *)cmd + 8, g32(cmd + 4) - 8u) == 3u);

    /* No command line in the original MBI: one is synthesized. */
    make_mbi(NULL);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_OK && info.relaunch_count == 1u);
    CHECK(strcmp((const char *)plan + BOOK8_RELAUNCH_MBI_OFFSET + 16, "book8.relaunch=1") == 0);

    /* Rejections. */
    make_mbi("x");
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS + 8u, &info)
        == BOOK8_RELAUNCH_BAD_ARGUMENT);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, ELF_PHYS + 0x1000u, &info)
        == BOOK8_RELAUNCH_BAD_ARGUMENT);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), 0x200000u, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_ARGUMENT);
    CHECK(book8_relaunch_prepare(elf, 63u, ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_NOT_ELF64);
    elf[18] = 3u;
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_NOT_ELF64);
    make_elf();
    phdr(2, 1u, 0x3000u, 0x80000u, 0x1000u, 0x1000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_SEGMENT);
    phdr(2, 1u, 0x3000u, BOOK8_RELAUNCH_HIGH - 0x1000u, 0x1000u, 0x2000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_SEGMENT);
    phdr(2, 1u, 0x3000u, 0x101000u, 0x1000u, 0x1000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_SEGMENT);
    phdr(2, 1u, 0x5800u, 0x103000u, 0x1000u, 0x1000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_SEGMENT);
    phdr(2, 1u, 0x3000u, 0x103000u, 0x2000u, 0x1000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_SEGMENT);
    make_elf();
    p32(elf + 0x100cu, 0u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_NO_MULTIBOOT);
    make_elf();
    p64(elf + 24, 0x500000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_ENTRY);
    make_elf();
    p32(mbi, 12u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_MBI);
    make_mbi("x");
    p32(mbi + 12, 4000u);
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_MBI);
    make_mbi("x");
    p32(mbi + g32(mbi) - 8u, 77u); /* END tag removed */
    CHECK(book8_relaunch_prepare(elf, sizeof(elf), ELF_PHYS, mbi, plan, PLAN_PHYS, &info)
        == BOOK8_RELAUNCH_BAD_MBI);
    CHECK(strcmp(book8_relaunch_result_string(BOOK8_RELAUNCH_NO_MULTIBOOT),
        "no Multiboot2 header") == 0);
}

/* Optional: validate the real kernel image produced by the build. */
static void real_kernel(const char *path)
{
    static uint8_t image[8u << 20u];
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    size_t bytes = fread(image, 1u, sizeof(image), file);
    fclose(file);
    make_mbi("/boot/book8.elf");
    struct book8_relaunch_info info;
    enum book8_relaunch_result result = book8_relaunch_prepare(image, bytes, ELF_PHYS, mbi,
        plan, PLAN_PHYS, &info);
    if (result != BOOK8_RELAUNCH_OK) {
        printf("FAIL: %s relaunch: %s\n", path,
            book8_relaunch_result_string(result));
    }
    CHECK(result == BOOK8_RELAUNCH_OK);
    CHECK(info.entry >= 0x100000u && info.image_high < BOOK8_RELAUNCH_HIGH);
    printf("PASS: %s relaunch plan: entry=0x%llx image=0x%llx-0x%llx mbi=0x%llx\n", path,
        (unsigned long long)info.entry, (unsigned long long)info.image_low,
        (unsigned long long)info.image_high, (unsigned long long)info.mbi_destination);
}

int main(int argc, char **argv)
{
    cmdline_tests();
    plan_tests();
    if (argc > 1) {
        real_kernel(argv[1]);
    }
    puts("PASS: relaunch ELF validation, Multiboot2 rewrite and plan layout");
    return 0;
}
