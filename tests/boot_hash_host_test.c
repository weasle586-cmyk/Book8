#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/boot_hash.h"
#include "../net/sha256.h"

static uint8_t host_file_bytes[1024u * 1024u];
static uint32_t host_file_size;

bool fs_seek(file_handle_t *file, uint32_t offset)
{
    if (file == NULL || offset > file->size || offset > host_file_size) {
        return false;
    }
    file->offset = offset;
    return true;
}

int32_t fs_read(file_handle_t *file, void *buffer, uint32_t count)
{
    if (file == NULL || file->offset > file->size
        || count > file->size - file->offset
        || file->offset + count > host_file_size) {
        return -1;
    }
    memcpy(buffer, host_file_bytes + file->offset, count);
    file->offset += count;
    return (int32_t)count;
}

static void put_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0u] = (uint8_t)value;
    bytes[1u] = (uint8_t)(value >> 8u);
    bytes[2u] = (uint8_t)(value >> 16u);
    bytes[3u] = (uint8_t)(value >> 24u);
}

static uint32_t add_entry(uint8_t *manifest, uint32_t offset,
    const char *key, const void *data, uint32_t size)
{
    memset(manifest + offset, 0, BOOK8_BOOT_HASH_ENTRY_SIZE);
    memcpy(manifest + offset, key, strlen(key));
    book8_sha256(data, size, manifest + offset + BOOK8_BOOT_HASH_KEY_SIZE);
    return offset + BOOK8_BOOT_HASH_ENTRY_SIZE;
}

static bool read_whole_file(const char *path, uint8_t *buffer,
    uint32_t capacity, uint32_t *size)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    size_t read = fread(buffer, 1u, capacity, stream);
    bool ok = !ferror(stream) && feof(stream);
    fclose(stream);
    if (!ok) {
        return false;
    }
    *size = (uint32_t)read;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,
            "usage: boot_hash_host_test <manifest.bin> <book8.elf>\n");
        return 1;
    }

    static const uint8_t kernel_data[] = "kernel image bytes for hashing";
    static const uint8_t app_data[] = "BIN/HELLO.ELF payload";
    static const uint8_t splash_data[] = "splash payload";
    static uint8_t manifest[4096u];

    put_u32_le(manifest, BOOK8_BOOT_HASH_MAGIC);
    put_u32_le(manifest + 4u, BOOK8_BOOT_HASH_VERSION);
    put_u32_le(manifest + 8u, 4u);
    put_u32_le(manifest + 12u, 0u);
    uint32_t offset = BOOK8_BOOT_HASH_HEADER_SIZE;
    offset = add_entry(manifest, offset, "BOOK8.ELF", kernel_data,
        sizeof(kernel_data));
    offset = add_entry(manifest, offset, "KERNEL", kernel_data,
        sizeof(kernel_data));
    offset = add_entry(manifest, offset, "BIN/HELLO.ELF", app_data,
        sizeof(app_data));
    offset = add_entry(manifest, offset, "SPLASH", splash_data,
        sizeof(splash_data));

    if (!boot_hash_manifest_load(manifest, offset)
        || !boot_hash_manifest_valid()
        || boot_hash_manifest_count() != 4u) {
        fprintf(stderr, "FAIL: valid manifest was rejected\n");
        return 1;
    }
    if (!boot_hash_verify_memory("BOOK8.ELF", kernel_data,
            sizeof(kernel_data))
        || boot_hash_verify_memory("BOOK8.ELF", app_data, sizeof(app_data))
        || !boot_hash_verify_memory("SPLASH", splash_data,
            sizeof(splash_data))
        || boot_hash_verify_memory("MISSING", splash_data,
            sizeof(splash_data))) {
        fprintf(stderr, "FAIL: memory digest verification\n");
        return 1;
    }

    char key[BOOK8_BOOT_HASH_KEY_SIZE];
    if (!boot_hash_key_for_path("A:\\bin\\hello.elf", key, sizeof(key))
        || strcmp(key, "BIN/HELLO.ELF") != 0
        || !boot_hash_key_for_path("a:/BOOK8.elf", key, sizeof(key))
        || strcmp(key, "BOOK8.ELF") != 0) {
        fprintf(stderr, "FAIL: path normalization\n");
        return 1;
    }
    if (!boot_hash_verify_path_memory("A:\\BIN\\HELLO.ELF", app_data,
            sizeof(app_data))
        || boot_hash_verify_path_memory("A:\\BIN\\HELLO.ELF", kernel_data,
            sizeof(kernel_data))) {
        fprintf(stderr, "FAIL: path digest verification\n");
        return 1;
    }

    memcpy(host_file_bytes, kernel_data, sizeof(kernel_data));
    host_file_size = sizeof(kernel_data);
    file_handle_t file = {
        .used = true, .size = host_file_size, .offset = 7u
    };
    if (!boot_hash_verify_file("A:\\BOOK8.ELF", &file)
        || file.offset != 7u) {
        fprintf(stderr, "FAIL: file digest verification or offset restore\n");
        return 1;
    }
    host_file_bytes[0u] ^= 0xffu;
    if (boot_hash_verify_file("A:\\BOOK8.ELF", &file)) {
        fprintf(stderr, "FAIL: corrupted file passed verification\n");
        return 1;
    }

    put_u32_le(manifest, BOOK8_BOOT_HASH_MAGIC ^ 1u);
    if (boot_hash_manifest_load(manifest, offset)
        || boot_hash_manifest_valid()) {
        fprintf(stderr, "FAIL: bad magic accepted\n");
        return 1;
    }
    put_u32_le(manifest, BOOK8_BOOT_HASH_MAGIC);
    put_u32_le(manifest + 8u, 3u);
    if (boot_hash_manifest_load(manifest,
            offset - BOOK8_BOOT_HASH_ENTRY_SIZE - 1u)) {
        fprintf(stderr, "FAIL: truncated manifest accepted\n");
        return 1;
    }
    put_u32_le(manifest + 8u, 4u);
    memcpy(manifest + BOOK8_BOOT_HASH_HEADER_SIZE + BOOK8_BOOT_HASH_ENTRY_SIZE,
        manifest + BOOK8_BOOT_HASH_HEADER_SIZE, BOOK8_BOOT_HASH_KEY_SIZE);
    if (boot_hash_manifest_load(manifest, offset)) {
        fprintf(stderr, "FAIL: duplicate key accepted\n");
        return 1;
    }

    uint32_t manifest_size;
    if (!read_whole_file(argv[1], manifest, sizeof(manifest), &manifest_size)
        || !boot_hash_manifest_load(manifest, manifest_size)) {
        fprintf(stderr, "FAIL: generated manifest is not loadable\n");
        return 1;
    }
    uint32_t kernel_size;
    if (!read_whole_file(argv[2], host_file_bytes, sizeof(host_file_bytes),
            &kernel_size)
        || !boot_hash_verify_memory("BOOT/BOOK8.ELF", host_file_bytes,
            kernel_size)) {
        fprintf(stderr, "FAIL: generated manifest does not match kernel ELF\n");
        return 1;
    }
    printf("boot hash host test: PASS (%u generated entries)\n",
        boot_hash_manifest_count());
    return 0;
}
