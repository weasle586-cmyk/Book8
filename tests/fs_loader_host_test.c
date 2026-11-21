#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/filesystem.h"
#include "../fs/fs.h"

static uint8_t *image;
static uint64_t image_bytes;
static uint8_t expected[65536], actual[65536];
static bool fail_write;
static bool second_disk;
static uint32_t root_fallbacks;
static char binary_paths[16][256];
static uint32_t binaries;
static uint32_t directory_entries;

static bool count_entries(const char *name, bool directory, uint32_t size, void *context)
{
    (void)name; (void)directory; (void)size; (void)context;
    ++directory_entries;
    return true;
}

static bool collect_binary(const char *name, bool directory, uint32_t size, void *context)
{
    (void)size; (void)context;
    size_t length = strlen(name);
    if (!directory && length >= 4u && !strcmp(name + length - 4u, ".ELF")) {
        assert(binaries < 16u);
        int n = snprintf(binary_paths[binaries++], 256u, "A:\\BIN\\%s", name);
        assert(n > 0 && n < 256);
    }
    return true;
}

static bool read_sector(uint32_t disk, uint64_t lba, uint8_t data[512])
{
    if ((disk != 0u && !(disk == 1u && second_disk)) || lba >= image_bytes / 512u)
        return false;
    memcpy(data, image + lba * 512u, 512u);
    return true;
}

static bool write_sector(uint32_t disk, uint64_t lba, const uint8_t data[512])
{
    if (disk != 0u || lba >= image_bytes / 512u || fail_write)
        return false;
    memcpy(image + lba * 512u, data, 512u);
    block_cache_invalidate(block_get(0));
    return true;
}

static bool read_blocks(block_device_t *dev, uint32_t lba, uint32_t count, void *data)
{
    (void)dev;
    for (uint32_t i = 0u; i < count; ++i) {
        if (!read_sector(0u, (uint64_t)lba + i, (uint8_t *)data + i * 512u))
            return false;
    }
    return true;
}

static uint64_t disk_size(uint32_t disk)
{
    return disk == 0u || (disk == 1u && second_disk) ? image_bytes / 512u : 0u;
}

static void output(const char *text)
{
    if (strstr(text, "Directory root resolved to first readable filesystem volume.") != NULL) {
        ++root_fallbacks;
    }
}
static bool confirm(const char *text) { (void)text; return false; }

static bool resolve(uint32_t drive, struct fs_extent *extent)
{
    uint32_t disk;
    uint64_t first, count;
    if (drive == UINT32_MAX)
        drive = 0u;
    if (!fs_drive_extent(drive, &disk, &first, &count)
        || first > UINT32_MAX || count > UINT32_MAX)
        return false;
    extent->dev = block_get(disk);
    extent->first_lba = (uint32_t)first;
    extent->sector_count = (uint32_t)count;
    return extent->dev != NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *disk = fopen(argv[1], "rb");
    assert(disk != NULL && fseek(disk, 0, SEEK_END) == 0);
    long length = ftell(disk);
    assert(length > 0 && length % 512 == 0 && fseek(disk, 0, SEEK_SET) == 0);
    image_bytes = (uint64_t)length;
    image = malloc((size_t)length);
    assert(image != NULL && fread(image, 1, (size_t)length, disk) == (size_t)length);
    assert(fclose(disk) == 0);
    assert(block_register("test", image_bytes / 512u, read_blocks, NULL, NULL));
    fs_initialize(read_sector, write_sector, disk_size, output, confirm);
    fs_set_drive_resolver(resolve);
    static const char *const roots[] = {"A:\\", "a:", "/", "\\", "", "a:/"};
    uint32_t root_entries = 0u;
    for (uint32_t i = 0u; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        directory_entries = 0u;
        assert(fs_list_directory(roots[i], count_entries, NULL));
        assert(fs_last_error() == BOOK8_FS_OK);
        if (i == 0u) { root_entries = directory_entries; }
        assert(root_entries != 0u && directory_entries == root_entries);
    }
    directory_entries = 0u;
    assert(fs_list_directory("/BIN/", count_entries, NULL) && directory_entries == 6u);
    second_disk = true;
    fs_initialize(read_sector, write_sector, disk_size, output, confirm);
    assert(fs_handle_command("B:", NULL) && !strcmp(fs_current_path(), "B:\\"));
    second_disk = false;
    fs_initialize(read_sector, write_sector, disk_size, output, confirm);
    directory_entries = 0u;
    assert(fs_list_directory("/", count_entries, NULL));
    assert(fs_last_error() == BOOK8_FS_OK && directory_entries == root_entries);
    assert(root_fallbacks == 1u && !strcmp(fs_current_path(), "B:\\"));
    assert(!fs_list_directory("B:", count_entries, NULL));
    assert(fs_last_error() == BOOK8_FS_ERR_NO_DRIVE && root_fallbacks == 1u);
    assert(fs_handle_command("A:", NULL));
    assert(!fs_list_directory(NULL, count_entries, NULL));
    assert(!fs_list_directory("A", count_entries, NULL));
    assert(!fs_list_directory("A:\\MISSING\\", count_entries, NULL));
    uint32_t count;
    assert(fs_read_file("A:\\NVMAP.ELF", expected, sizeof(expected), &count));
    assert(count > 4u && memcmp(expected, "\177ELF", 4) == 0);
    file_handle_t *file = fs_open_any("A:\\NVMAP.ELF");
    assert(file != NULL && file->size == count);
    assert(fs_open_any("B:\\NVMAP.ELF") == NULL);
    assert(fs_read(file, actual, sizeof(actual)) == (int32_t)count);
    assert(memcmp(expected, actual, count) == 0);
    assert(fs_seek(file, 1u));
    assert(fs_read(file, actual, 3u) == 3 && memcmp(actual, "ELF", 3u) == 0);
    fs_close(file);
    /* Exercise the push writer and native startup reader on the same volume. */
    assert(fs_write_file("A:\\NVMAP.CFG", "1\r\n", 3u));
    file = fs_open_any("a:/nvmap.cfg");
    assert(file != NULL);
    assert(fs_read(file, actual, sizeof(actual)) == 3);
    assert(memcmp(actual, "1\r\n", 3u) == 0);
    fs_close(file);
    file = fs_open_any("NVMAP.ELF");
    assert(file != NULL);
    fs_close(file);
    /* GUI Save As and overwrite use existing FAT subdirectories on both formats. */
    for (uint32_t i = 0u; i < 2000u; ++i) { expected[i] = (uint8_t)(i * 7u); }
    assert(fs_write_file("A:\\DATA\\TEST.C", expected, 2000u));
    assert(fs_read_file("A:\\DATA\\TEST.C", actual, sizeof(actual), &count));
    assert(count == 2000u && memcmp(expected, actual, count) == 0);
    struct book8_file_info metadata;
    assert(fs_file_info("A:\\DATA\\TEST.C", &metadata));
    assert(metadata.allocation_valid && metadata.allocated_bytes >= 2000u);
    fail_write = true;
    assert(!fs_write_file("A:\\DATA\\TEST.C", "replacement", 11u));
    fail_write = false;
    assert(fs_read_file("A:\\DATA\\TEST.C", actual, sizeof(actual), &count));
    assert(count == 2000u && memcmp(expected, actual, count) == 0);
    assert(fs_write_file("A:\\DATA\\TEST.C", "replacement", 11u));
    assert(fs_read_file("A:\\DATA\\TEST.C", actual, sizeof(actual), &count));
    assert(count == 11u && memcmp(actual, "replacement", count) == 0);
    assert(fs_write_file("A:\\DATA\\TEST.C", NULL, 0u));
    assert(fs_read_file("A:\\DATA\\TEST.C", actual, sizeof(actual), &count) && count == 0u);
    assert(!fs_write_file("A:\\MISSING\\TEST.C", "x", 1u));
    assert(!fs_write_file("A:\\DATA", "x", 1u));
    assert(!fs_read_cluster('A', 1u, actual, sizeof(actual), &count));
    assert(!fs_read_cluster('A', UINT32_MAX, actual, sizeof(actual), &count));
    assert(!fs_read_cluster('A', 2u, actual, 1u, &count));
    assert(fs_read_cluster('A', 2u, actual, sizeof(actual), &count) && count >= 512u);
    assert(fs_make_directory("A:\\DOWNLOADS\\"));
    assert(fs_make_directory("A:\\DOWNLOADS"));
    assert(fs_create_file("A:\\DOWNLOADS\\TEST.C", "download", 8u));
    assert(!fs_create_file("A:\\DOWNLOADS\\TEST.C", "overwrite", 9u));
    assert(fs_read_file("A:\\DOWNLOADS\\TEST.C", actual, sizeof(actual), &count));
    assert(count == 8u && !memcmp(actual, "download", count));
    assert(!fs_make_directory("A:\\DOWNLOADS\\TEST.C"));
    assert(!fs_make_directory("A:\\MISSING\\SUB"));
    assert(fs_make_directory("A:\\DOWNLOADS\\SUB"));
    assert(fs_create_file("A:\\DOWNLOADS\\SUB\\EMPTY.TXT", NULL, 0u));
    assert(fs_create_file("A:\\BIN\\PAD.ELF", "ELF", 3u));
    assert(fs_create_file("A:\\BIN\\EMPTY.ELF", NULL, 0u));
    bool patched = false;
    for (uint64_t offset = 0u; offset + 32u <= image_bytes; offset += 32u) {
        uint8_t *raw = image + offset;
        if (!memcmp(raw, "PAD     ELF", 11u)) {
            memset(raw + 3u, 0u, 5u);
            memset(raw + 14u, 0u, 4u);
            patched = true;
            break;
        }
    }
    assert(patched);
    block_cache_invalidate(block_get(0));
    assert(fs_list_directory("A:\\BIN\\", collect_binary, NULL));
    assert(binaries == 8u);
    for (uint32_t i = 0u; i < binaries; ++i) {
        assert(fs_file_info(binary_paths[i], &metadata));
        assert(metadata.allocation_valid && !metadata.directory);
    }
    assert(fs_file_info("A:\\BIN\\PAD.ELF", &metadata));
    assert(metadata.size == 3u && !metadata.creation_valid);
    assert(fs_file_info("A:\\BIN\\EMPTY.ELF", &metadata) && metadata.allocated_bytes == 0u);
    free(image);
    puts("PASS: shared ELF volume, FAT subdirectory saves, replacement failures, raw clusters, BIN metadata and null-padded FAT names");
    return 0;
}
