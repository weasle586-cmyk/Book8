#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/filesystem.h"

static uint8_t disk[8192][512];
static uint8_t second_disk[8192][512];
static bool second_present;
static uint32_t reads, writes;
static bool fail_read;
static bool fail_write;
static uint64_t fail_write_lba;

static bool read_sector(uint32_t index, uint64_t lba, uint8_t data[512])
{
    ++reads;
    if (index > 1u || (index == 1u && !second_present)
        || lba >= 8192u || (fail_read && lba == 2u)) { return false; }
    memcpy(data, index == 0u ? disk[lba] : second_disk[lba], 512u);
    return true;
}

static bool write_sector(uint32_t index, uint64_t lba, const uint8_t data[512])
{
    ++writes;
    if (index > 1u || (index == 1u && !second_present) || lba >= 8192u
        || (fail_write && lba == fail_write_lba)) { return false; }
    memcpy(index == 0u ? disk[lba] : second_disk[lba], data, 512u);
    return true;
}

static uint64_t size(uint32_t index)
{
    return index == 0u || (index == 1u && second_present) ? 8192u : 0u;
}
static void output(const char *text) { (void)text; }
static bool confirm(const char *text) { (void)text; return false; }
static void put16(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8u);
}

static void initialize(void)
{
    memset(disk, 0, sizeof(disk));
    second_present = false;
    put16(disk[0] + 11u, 512u);
    disk[0][13] = 1u;
    put16(disk[0] + 14u, 1u);
    disk[0][16] = 1u;
    put16(disk[0] + 17u, 16u);
    put16(disk[0] + 19u, 8192u);
    put16(disk[0] + 22u, 32u);
    put16(disk[0] + 510u, 0xaa55u);
    put16(disk[1], 0xfff8u);
    put16(disk[1] + 2u, 0xffffu);
    put16(disk[1] + 4u, 0xffffu);
    fs_initialize(read_sector, write_sector, size, output, confirm);
    assert(fs_drive_count() == 1u);
}

int main(void)
{
    struct book8_drive_space space;
    initialize();
    assert(fs_drive_space(0u, &space));
    assert(space.state == BOOK8_SPACE_PENDING);
    for (uint32_t i = 0u; i < 32u; ++i) {
        uint32_t before = reads;
        fs_space_poll();
        assert(reads - before == 1u);
    }
    assert(fs_drive_space(0u, &space));
    assert(space.state == BOOK8_SPACE_READY);
    assert(space.total_bytes == (8192u - 34u) * 512u);
    assert(space.free_bytes == space.total_bytes - 512u);
    assert(writes == 0u);
    /* A real filesystem write invalidates accounting, then resamples. */
    assert(fs_write_file("A:\\TEST.TXT", "abc", 3u));
    assert(fs_drive_space(0u, &space));
    assert(space.state == BOOK8_SPACE_PENDING);
    for (uint32_t i = 0u; i < 32u; ++i) { fs_space_poll(); }
    assert(fs_drive_space(0u, &space));
    assert(space.state == BOOK8_SPACE_READY);
    assert(space.free_bytes == space.total_bytes - 1024u);
    initialize();
    assert(fs_drive_space(0u, &space));
    fail_read = true;
    fs_space_poll(); fs_space_poll();
    assert(fs_drive_space(0u, &space));
    assert(space.state == BOOK8_SPACE_ERROR);
    fail_read = false;
    initialize();
    put16(disk[1] + 6u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    memcpy(disk[34], "NOTE    TXT", 11u);
    disk[34][11] = 0x20u;
    put16(disk[34] + 26u, 3u);
    disk[34][28] = 3u;
    disk[34][11] |= 0x05u;
    put16(disk[34] + 14u, (13u << 11u) | (25u << 5u) | 9u);
    put16(disk[34] + 16u, (46u << 9u) | (11u << 5u) | 7u);
    memcpy(disk[35], "abc", 3u);
    char content[4] = {0};
    uint32_t file_size = 0u;
    assert(fs_read_file("A:\\DATA\\NOTE.TXT", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(!fs_read_file("A:\\DATA\\NOTE.TXT", content, 2u, &file_size));
    assert(!fs_read_file("A:\\DATA\\MISSING.TXT", content, 3u, &file_size));
    assert(!fs_read_file("A:\\DATA\\..\\NOTE.TXT", content, 3u, &file_size));
    struct book8_file_info info;
    assert(fs_file_info("A:\\DATA\\NOTE.TXT", &info));
    assert(info.size == 3u && info.allocation_valid && info.allocated_bytes == 512u
        && info.read_only && info.system && !info.directory
        && info.creation_valid && info.year == 2026u && info.month == 11u
        && info.day == 7u && info.hour == 13u && info.minute == 25u
        && info.second == 18u);
    assert(fs_file_info("A:\\DATA", &info) && info.directory
        && info.allocated_bytes == 512u);
    assert(!fs_file_info("A:\\DATA\\MISSING.TXT", &info));
    assert(!fs_rename_entry("A:\\DATA\\NOTE.TXT", "NEW.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_PROTECTED);
    assert(!fs_delete_entry("A:\\DATA\\NOTE.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_PROTECTED);
    assert(!fs_delete_entry("A:\\DATA"));
    assert(fs_last_error() == BOOK8_FS_ERR_PROTECTED);
    disk[34][11] = 0x20u;
    assert(!fs_rename_entry("A:\\DATA\\NOTE.TXT", "invalid/name.txt"));
    assert(fs_last_error() == BOOK8_FS_ERR_BAD_PATH);
    assert(fs_rename_entry("A:\\DATA\\NOTE.TXT", "NEW.TXT"));
    assert(fs_read_file("A:\\DATA\\NEW.TXT", content, 3u, &file_size));
    assert(!fs_read_file("A:\\DATA\\NOTE.TXT", content, 3u, &file_size));
    memcpy(disk[34] + 32u, "EXIST   TXT", 11u);
    disk[34][32u + 11u] = 0x20u;
    assert(!fs_rename_entry("A:\\DATA\\NEW.TXT", "EXIST.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    disk[34][32u] = 0xe5u;
    assert(fs_delete_entry("A:\\DATA\\NEW.TXT"));
    assert(fs_delete_entry("A:\\DATA"));
    assert(!fs_file_info("A:\\DATA", &info));

    initialize();
    disk[33][0] = 0x41u;
    disk[33][11] = 0x0fu;
    memcpy(disk[33] + 32u, "LONGNA~1TXT", 11u);
    disk[33][32u + 11u] = 0x20u;
    memcpy(disk[33] + 64u, "OTHER   TXT", 11u);
    disk[33][64u + 11u] = 0x20u;
    assert(!fs_rename_entry("A:\\LONGNA~1.TXT", "NEW.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_UNSUPPORTED);
    assert(!fs_delete_entry("A:\\LONGNA~1.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_UNSUPPORTED);
    assert(!fs_rename_entry("A:\\OTHER.TXT", "LONGNA~1.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    fail_write = true;
    fail_write_lba = 33u;
    assert(!fs_delete_entry("A:\\OTHER.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_IO && disk[33][64u] == 'O');
    fail_write = false;
    put16(disk[33] + 64u + 26u, 2u);
    put16(disk[1] + 4u, 2u);
    assert(!fs_delete_entry("A:\\OTHER.TXT"));
    assert(fs_last_error() == BOOK8_FS_ERR_IO && disk[33][64u] == 'O');

    initialize();
    assert(fs_write_file("A:\\long-example-file.txt", "abc", 3u));
    assert(fs_read_file("A:\\long-example-file.txt", content, 3u, &file_size));
    assert(fs_delete_entry("A:\\long-example-file.txt"));
    assert(!fs_file_info("A:\\long-example-file.txt", &info));
    assert(disk[33][0] == 0xe5u && disk[33][32] == 0xe5u
        && disk[33][64] == 0xe5u);
    initialize();
    assert(fs_write_file("A:\\long-example-file.txt", "abc", 3u));
    uint8_t long_entries[96];
    memcpy(long_entries, disk[33], sizeof(long_entries));
    initialize();
    put16(disk[1] + 4u, 3u);
    put16(disk[1] + 6u, 0xffffu);
    put16(disk[1] + 8u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    for (unsigned i = 0u; i < 15u; ++i)
        disk[34][i * 32u] = 0xe5u;
    memcpy(disk[34] + 480u, long_entries, 32u);
    memcpy(disk[35], long_entries + 32u, 64u);
    put16(disk[35] + 32u + 26u, 4u);
    memcpy(disk[36], "abc", 3u);
    assert(fs_read_file("A:\\DATA\\long-example-file.txt", content, 3u, &file_size));
    uint8_t before_sector34[512], before_sector35[512];
    memcpy(before_sector34, disk[34], 512u);
    memcpy(before_sector35, disk[35], 512u);
    assert(!fs_rename_entry("A:\\DATA\\long-example-file.txt", "a-new-long-name.txt"));
    assert(fs_last_error() == BOOK8_FS_ERR_UNSUPPORTED);
    assert(memcmp(before_sector34, disk[34], 512u) == 0
        && memcmp(before_sector35, disk[35], 512u) == 0);
    assert(fs_copy_to_directory("A:\\DATA\\long-example-file.txt", "A:\\", false));
    assert(fs_read_file("A:\\long-example-file.txt", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(fs_write_file("A:\\long-example-four.txt", "xyz", 3u));
    assert(fs_read_file("A:\\long-example-four.txt", content, 3u, &file_size));
    assert(memcmp(content, "xyz", 3u) == 0);
    assert(fs_read_file("A:\\long-example-file.txt", content, 3u, &file_size));
    assert(memcmp(content, "abc", 3u) == 0);
    assert(fs_copy_to_directory("A:\\DATA", "A:\\", false) == false);
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    assert(fs_delete_entry("A:\\DATA\\long-example-file.txt"));
    assert(disk[34][480u] == 0xe5u && disk[35][0] == 0xe5u
        && disk[35][32u] == 0xe5u);
    assert(fs_rename_entry("A:\\long-example-file.txt", "RENAMED.TXT"));
    assert(!fs_file_info("A:\\long-example-file.txt", &info));
    assert(fs_read_file("A:\\RENAMED.TXT", content, 3u, &file_size));
    assert(memcmp(content, "abc", 3u) == 0);

    initialize();
    assert(fs_write_file("A:\\ORIGINAL.TXT", "abc", 3u));
    fail_write = true;
    fail_write_lba = 33u;
    assert(!fs_rename_entry("A:\\ORIGINAL.TXT", "renamed-long-file.txt"));
    assert(fs_last_error() == BOOK8_FS_ERR_IO);
    fail_write = false;
    assert(fs_read_file("A:\\ORIGINAL.TXT", content, 3u, &file_size));
    assert(fs_rename_entry("A:\\ORIGINAL.TXT", "renamed-long-file.txt"));
    assert(!fs_file_info("A:\\ORIGINAL.TXT", &info));
    assert(fs_read_file("A:\\renamed-long-file.txt", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(fs_rename_entry("A:\\renamed-long-file.txt", "another-long-file.txt"));
    assert(!fs_file_info("A:\\renamed-long-file.txt", &info));
    assert(fs_read_file("A:\\another-long-file.txt", content, 3u, &file_size));
    assert(memcmp(content, "abc", 3u) == 0);

    initialize();
    put16(disk[0] + 17u, 64u);
    fs_initialize(read_sector, write_sector, size, output, confirm);
    for (unsigned i = 0u; i < 12u; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "A:\\same-prefix-file-%02u.txt", i);
        assert(fs_write_file(name, "abc", 3u));
    }
    for (unsigned i = 0u; i < 12u; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "A:\\same-prefix-file-%02u.txt", i);
        assert(fs_read_file(name, content, 3u, &file_size));
        assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    }
    assert(!fs_copy_to_directory("A:\\same-prefix-file-00.txt", "A:\\", false));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    assert(fs_read_file("A:\\same-prefix-file-00.txt", content, 3u, &file_size));
    assert(memcmp(content, "abc", 3u) == 0);

    initialize();
    for (unsigned i = 0u; i < 16u; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "A:\\F%02u.TXT", i);
        uint32_t before = reads;
        assert(fs_write_file(name, "abc", 3u));
        assert(reads - before < 12u);
    }

    initialize();
    put16(disk[1] + 6u, 0xffffu);
    put16(disk[1] + 8u, 0xffffu);
    memcpy(disk[33], long_entries, sizeof(long_entries));
    disk[33][64u + 11u] = 0x10u;
    put16(disk[33] + 64u + 26u, 2u);
    disk[33][64u + 28u] = 0u;
    memcpy(disk[33] + 96u, "TARGET     ", 11u);
    disk[33][96u + 11u] = 0x10u;
    put16(disk[33] + 96u + 26u, 4u);
    memcpy(disk[34], "NOTE    TXT", 11u);
    disk[34][11] = 0x20u;
    put16(disk[34] + 26u, 3u);
    disk[34][28] = 3u;
    memcpy(disk[35], "abc", 3u);
    assert(fs_copy_to_directory("A:\\long-example-file.txt", "A:\\TARGET\\", true));
    assert(!fs_file_info("A:\\long-example-file.txt", &info));
    assert(fs_read_file("A:\\TARGET\\long-example-file.txt\\NOTE.TXT",
        content, 3u, &file_size));
    assert(memcmp(content, "abc", 3u) == 0);

    initialize();
    put16(disk[1] + 6u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    memcpy(disk[34], "NOTE    TXT", 11u);
    disk[34][11] = 0x20u;
    put16(disk[34] + 26u, 3u);
    disk[34][28] = 3u;
    memcpy(disk[35], "abc", 3u);
    assert(!fs_copy_to_directory("A:\\DATA", "A:\\", false));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    assert(!fs_copy_to_directory("A:\\DATA", "A:\\DATA\\", false));
    assert(fs_last_error() == BOOK8_FS_ERR_BAD_PATH);
    assert(!fs_copy_to_directory("A:\\DATA\\NOTE.TXT", "A:\\DATA\\", false));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    assert(fs_copy_to_directory("A:\\DATA\\NOTE.TXT", "A:\\", false));
    assert(fs_read_file("A:\\NOTE.TXT", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(!fs_copy_to_directory("A:\\DATA\\NOTE.TXT", "A:\\", true));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    assert(fs_read_file("A:\\DATA\\NOTE.TXT", content, 3u, &file_size));
    assert(fs_delete_entry("A:\\NOTE.TXT"));
    assert(fs_copy_to_directory("A:\\DATA\\NOTE.TXT", "A:\\", true));
    assert(!fs_file_info("A:\\DATA\\NOTE.TXT", &info));
    assert(fs_read_file("A:\\NOTE.TXT", content, 3u, &file_size));
    assert(fs_copy_to_directory("A:\\NOTE.TXT", "A:\\DATA\\", false));
    assert(fs_read_file("A:\\DATA\\NOTE.TXT", content, 3u, &file_size));

    initialize();
    put16(disk[1] + 6u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    memcpy(disk[34], "NOTE    TXT", 11u);
    disk[34][11] = 0x20u;
    put16(disk[34] + 26u, 3u);
    disk[34][28] = 3u;
    memcpy(disk[35], "abc", 3u);
    fail_write = true;
    fail_write_lba = 33u;
    assert(!fs_copy_to_directory("A:\\DATA\\NOTE.TXT", "A:\\", true));
    assert(fs_last_error() == BOOK8_FS_ERR_IO);
    fail_write = false;
    assert(fs_read_file("A:\\DATA\\NOTE.TXT", content, 3u, &file_size));
    assert(memcmp(content, "abc", 3u) == 0);

    initialize();
    put16(disk[1] + 6u, 0xffffu);
    put16(disk[1] + 8u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    memcpy(disk[33] + 32u, "TARGET     ", 11u);
    disk[33][43] = 0x10u;
    put16(disk[33] + 58u, 4u);
    memcpy(disk[34], "NOTE    TXT", 11u);
    disk[34][11] = 0x20u;
    put16(disk[34] + 26u, 3u);
    disk[34][28] = 3u;
    memcpy(disk[35], "abc", 3u);
    assert(fs_copy_to_directory("A:\\DATA", "A:\\TARGET\\", false));
    assert(fs_read_file("A:\\TARGET\\DATA\\NOTE.TXT", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(!fs_copy_to_directory("A:\\DATA", "A:\\TARGET\\", true));
    assert(fs_last_error() == BOOK8_FS_ERR_EXISTS);
    assert(fs_read_file("A:\\DATA\\NOTE.TXT", content, 3u, &file_size));
    assert(fs_delete_entry("A:\\TARGET\\DATA\\NOTE.TXT"));
    assert(fs_delete_entry("A:\\TARGET\\DATA"));
    assert(fs_copy_to_directory("A:\\DATA", "A:\\TARGET\\", true));
    assert(!fs_file_info("A:\\DATA", &info));
    assert(fs_read_file("A:\\TARGET\\DATA\\NOTE.TXT", content, 3u, &file_size));

    initialize();
    put16(disk[1] + 6u, 0xffffu);
    put16(disk[1] + 8u, 0xffffu);
    put16(disk[1] + 10u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    memcpy(disk[33] + 32u, "TARGET     ", 11u);
    disk[33][43] = 0x10u;
    put16(disk[33] + 58u, 4u);
    memcpy(disk[34], "SUB        ", 11u);
    disk[34][11] = 0x10u;
    put16(disk[34] + 26u, 5u);
    memcpy(disk[37], "NOTE    TXT", 11u);
    disk[37][11] = 0x21u;
    put16(disk[37] + 26u, 3u);
    disk[37][28] = 3u;
    memcpy(disk[35], "abc", 3u);
    assert(!fs_copy_to_directory("A:\\DATA", "A:\\DATA\\SUB\\", false));
    assert(fs_last_error() == BOOK8_FS_ERR_BAD_PATH);
    assert(!fs_copy_to_directory("A:\\DATA", "A:\\TARGET\\", true));
    assert(fs_last_error() == BOOK8_FS_ERR_PROTECTED);
    assert(fs_read_file("A:\\DATA\\SUB\\NOTE.TXT", content, 3u, &file_size));
    disk[37][11] = 0x20u;
    assert(fs_copy_to_directory("A:\\DATA", "A:\\TARGET\\", true));
    assert(fs_read_file("A:\\TARGET\\DATA\\SUB\\NOTE.TXT", content, 3u, &file_size));
    assert(!fs_file_info("A:\\DATA", &info));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);

    initialize();
    put16(disk[1] + 6u, 0xffffu);
    memcpy(disk[33], "SOURCE  TXT", 11u);
    disk[33][11] = 0x20u;
    put16(disk[33] + 26u, 3u);
    disk[33][28] = 3u;
    memcpy(disk[33] + 32u, "TARGET     ", 11u);
    disk[33][43] = 0x10u;
    put16(disk[33] + 58u, 2u);
    memcpy(disk[35], "abc", 3u);
    for (unsigned i = 0u; i < 16u; ++i) {
        uint8_t *raw = disk[34] + i * 32u;
        memcpy(raw, "ITEM00  TXT", 11u);
        raw[4] = (uint8_t)('0' + i / 10u);
        raw[5] = (uint8_t)('0' + i % 10u);
        raw[11] = 0x20u;
    }
    assert(fs_copy_to_directory("A:\\SOURCE.TXT", "A:\\TARGET\\", false));
    assert(fs_read_file("A:\\TARGET\\SOURCE.TXT", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(disk[1][4] != 0xffu || disk[1][5] != 0xffu);

    initialize();
    memcpy(second_disk, disk, sizeof(disk));
    second_present = true;
    put16(disk[1] + 6u, 0xffffu);
    memcpy(disk[33], "DATA       ", 11u);
    disk[33][11] = 0x10u;
    put16(disk[33] + 26u, 2u);
    memcpy(disk[34], "NOTE    TXT", 11u);
    disk[34][11] = 0x20u;
    put16(disk[34] + 26u, 3u);
    disk[34][28] = 3u;
    memcpy(disk[35], "abc", 3u);
    fs_initialize(read_sector, write_sector, size, output, confirm);
    assert(fs_drive_count() == 2u);
    assert(fs_copy_to_directory("A:\\DATA", "B:\\", true));
    assert(!fs_file_info("A:\\DATA", &info));
    assert(fs_read_file("B:\\DATA\\NOTE.TXT", content, 3u, &file_size));
    assert(file_size == 3u && memcmp(content, "abc", 3u) == 0);
    assert(fs_delete_entry("B:\\DATA"));
    assert(!fs_file_info("B:\\DATA\\NOTE.TXT", &info));
    puts("filesystem space host test: PASS");
    return 0;
}
