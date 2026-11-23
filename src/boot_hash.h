#ifndef BOOK8_BOOT_HASH_H
#define BOOK8_BOOT_HASH_H

/* Integrity manifest loaded as Multiboot2 module "book8-hashes".
 *
 * This is build-time integrity, not secure boot: GRUB loads the manifest
 * alongside the kernel and the kernel refuses trusted payloads whose bytes no
 * longer match the manifest generated for this build. */

#include <stdbool.h>
#include <stdint.h>
#include "../fs/fs.h"

#define BOOK8_BOOT_HASH_MAGIC 0x31483842u /* "B8H1" little-endian */
#define BOOK8_BOOT_HASH_VERSION 1u
#define BOOK8_BOOT_HASH_KEY_SIZE 32u
#define BOOK8_BOOT_HASH_DIGEST_SIZE 32u
#define BOOK8_BOOT_HASH_MAX_ENTRIES 128u
#define BOOK8_BOOT_HASH_HEADER_SIZE 16u
#define BOOK8_BOOT_HASH_ENTRY_SIZE 64u

bool boot_hash_manifest_load(const uint8_t *data, uint64_t size);
bool boot_hash_manifest_valid(void);
uint32_t boot_hash_manifest_count(void);
bool boot_hash_key_for_path(const char *path, char *key, uint32_t capacity);
bool boot_hash_verify_memory(const char *key, const void *data, uint64_t size);
bool boot_hash_verify_path_memory(const char *path, const void *data,
    uint64_t size);
bool boot_hash_verify_file(const char *path, file_handle_t *file);

#endif
