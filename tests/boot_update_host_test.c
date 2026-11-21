#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/filesystem.h"
#include "../fs/fs.h"

static uint8_t *image, *original, *kernel, *old_kernel;
static size_t image_size, kernel_size;
static unsigned writes, fail_at, commit_at;
static bool corrupt_data;
static bool committed;

static bool read_sector(uint32_t disk, uint64_t lba, uint8_t data[512])
{
    if (disk != 0u || lba >= image_size / 512u)
        return false;
    memcpy(data, image + lba * 512u, 512u);
    return true;
}

static bool write_sector(uint32_t disk, uint64_t lba, const uint8_t data[512])
{
    assert(disk == 0u && lba < image_size / 512u);
    ++writes;
    if (writes == fail_at)
        return false;
    /* The directory commit changes the existing BOOK8 ELF short entry. */
    for (unsigned i = 0u; i < 512u; i += 32u) {
        if (memcmp(data + i, "BOOK8   ELF", 11u) == 0
            && memcmp(image + lba * 512u + i, data + i, 32u) != 0) {
            committed = true;
            commit_at = writes;
        }
    }
    memcpy(image + lba * 512u, data, 512u);
    if (corrupt_data && memcmp(data, "\177ELF", 4u) == 0)
        image[lba * 512u + 100u] ^= 1u;
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
{ return disk == 0u ? image_size / 512u : 0u; }
static void output(const char *text) { fputs(text, stderr); }
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

static uint8_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    *size = (size_t)length;
    uint8_t *data = malloc(*size);
    assert(data && fread(data, 1, *size, file) == *size && fclose(file) == 0);
    return data;
}

static void reset(void)
{
    memcpy(image, original, image_size);
    writes = 0u;
    committed = false;
    block_cache_invalidate(NULL);
    fs_initialize(read_sector, write_sector, disk_size, output, confirm);
    fs_set_drive_resolver(resolve);
}

static size_t read_file(const char *path, uint8_t *data, size_t capacity)
{
    file_handle_t *file = fs_open_any(path);
    assert(file && file->size <= capacity);
    uint32_t size = file->size;
    assert(fs_read(file, data, size) == (int32_t)size);
    fs_close(file);
    return size;
}

int main(int argc, char **argv)
{
    assert(argc == 4); /* disposable input image, new ELF, disposable output */
    original = load(argv[1], &image_size);
    assert(image_size % 512u == 0u);
    kernel = load(argv[2], &kernel_size);
    assert(kernel_size <= 1024u * 1024u);
    image = malloc(image_size);
    old_kernel = malloc(1024u * 1024u);
    uint8_t *verify = malloc(1024u * 1024u);
    assert(image && old_kernel && verify);
    assert(block_register("test", image_size / 512u, read_blocks, NULL, NULL));
    reset();
    size_t old_size = read_file("A:\\boot\\book8.elf", old_kernel, 1024u * 1024u);
    uint8_t grub[4096];
    size_t grub_size = read_file("A:\\boot\\grub\\grub.cfg", grub, sizeof(grub));
    assert(!fs_replace_boot_kernel(NULL, (uint32_t)kernel_size));
    assert(!fs_replace_boot_kernel(kernel, 0u));
    assert(!fs_replace_boot_kernel(kernel, 1024u * 1024u + 1u));
    assert(writes == 0u);
    assert(fs_replace_boot_kernel(kernel, (uint32_t)kernel_size));
    unsigned commit_write = commit_at;
    assert(committed && commit_write > 1u);
    block_cache_invalidate(NULL);
    assert(read_file("A:\\boot\\book8.elf", verify, 1024u * 1024u) == kernel_size);
    assert(memcmp(verify, kernel, kernel_size) == 0);
    assert(read_file("A:\\boot\\grub\\grub.cfg", verify, 1024u * 1024u) == grub_size);
    assert(memcmp(verify, grub, grub_size) == 0);
    /* Include pre-allocation, data, and commit failures. An uncertain commit
     * must return failure; no candidate is freed after commit is attempted. */
    const unsigned failures[] = {1u, 4u, commit_write - 1u, commit_write};
    for (unsigned i = 0u; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset();
        fail_at = failures[i];
        assert(!fs_replace_boot_kernel(kernel, (uint32_t)kernel_size));
        block_cache_invalidate(NULL);
        assert(read_file("A:\\boot\\book8.elf", verify, 1024u * 1024u) == old_size);
        assert(memcmp(verify, old_kernel, old_size) == 0);
        assert(!committed);
    }
    reset();
    fail_at = 0u;
    corrupt_data = true;
    assert(!fs_replace_boot_kernel(kernel, (uint32_t)kernel_size));
    assert(!committed);
    corrupt_data = false;
    block_cache_invalidate(NULL);
    assert(read_file("A:\\boot\\book8.elf", verify, 1024u * 1024u) == old_size);
    assert(memcmp(verify, old_kernel, old_size) == 0);
    reset();
    assert(fs_replace_boot_kernel(kernel, (uint32_t)kernel_size));
    FILE *file = fopen(argv[3], "wbx");
    assert(file && fwrite(image, 1, image_size, file) == image_size && fclose(file) == 0);
    free(verify); free(old_kernel); free(image); free(kernel); free(original);
    puts("PASS: FAT boot kernel copy-on-write, readback, failure retention and unchanged GRUB");
    return 0;
}
