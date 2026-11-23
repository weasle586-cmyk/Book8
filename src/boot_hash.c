#include "boot_hash.h"

#include "../net/sha256.h"

/* Build-time integrity manifest. The binary layout is intentionally tiny and
 * fixed: a 16-byte header followed by 64-byte entries. The first 32 bytes of
 * an entry are the NUL-terminated key; the last 32 are the SHA-256 digest. */

/* Host tests link this module without the kernel console. */
__attribute__((weak)) void serial_write(const char *text) { (void)text; }

static void boot_hash_report_failure(const char *key)
{
    serial_write("BOOK8-HASH: verification failed for ");
    serial_write(key == NULL ? "?" : key);
    serial_write("\r\n");
}

struct boot_hash_entry {
    char key[BOOK8_BOOT_HASH_KEY_SIZE];
    uint8_t digest[BOOK8_BOOT_HASH_DIGEST_SIZE];
};

_Static_assert(sizeof(struct boot_hash_entry) == BOOK8_BOOT_HASH_ENTRY_SIZE,
    "boot hash entry layout must stay at 64 bytes");

static struct boot_hash_entry boot_hash_entries[BOOK8_BOOT_HASH_MAX_ENTRIES];
static uint32_t boot_hash_entry_count;
static bool boot_hash_loaded;

static uint32_t boot_hash_read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0u]
        | ((uint32_t)bytes[1u] << 8u)
        | ((uint32_t)bytes[2u] << 16u)
        | ((uint32_t)bytes[3u] << 24u);
}

static uint32_t boot_hash_ascii_length(const char *text, uint32_t maximum)
{
    uint32_t length = 0u;
    while (length < maximum && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool boot_hash_key_equal(const char *left, const char *right)
{
    for (uint32_t i = 0u; i < BOOK8_BOOT_HASH_KEY_SIZE; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
        if (left[i] == '\0') {
            return true;
        }
    }
    return true;
}

static bool boot_hash_key_valid(const char *key)
{
    uint32_t length = boot_hash_ascii_length(key, BOOK8_BOOT_HASH_KEY_SIZE);
    if (length == 0u || length >= BOOK8_BOOT_HASH_KEY_SIZE) {
        return false;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        char value = key[i];
        if (!(value >= 'A' && value <= 'Z')
            && !(value >= '0' && value <= '9')
            && value != '/' && value != '_' && value != '-'
            && value != '.') {
            return false;
        }
    }
    return true;
}

bool boot_hash_manifest_load(const uint8_t *data, uint64_t size)
{
    boot_hash_loaded = false;
    boot_hash_entry_count = 0u;
    if (data == NULL || size < BOOK8_BOOT_HASH_HEADER_SIZE) {
        return false;
    }
    uint32_t magic = boot_hash_read_u32_le(data);
    uint32_t version = boot_hash_read_u32_le(data + 4u);
    uint32_t count = boot_hash_read_u32_le(data + 8u);
    uint32_t reserved = boot_hash_read_u32_le(data + 12u);
    if (magic != BOOK8_BOOT_HASH_MAGIC || version != BOOK8_BOOT_HASH_VERSION
        || reserved != 0u || count == 0u
        || count > BOOK8_BOOT_HASH_MAX_ENTRIES) {
        return false;
    }
    uint64_t required = BOOK8_BOOT_HASH_HEADER_SIZE
        + (uint64_t)count * BOOK8_BOOT_HASH_ENTRY_SIZE;
    if (size != required) {
        return false;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const uint8_t *source = data + BOOK8_BOOT_HASH_HEADER_SIZE
            + (uint64_t)i * BOOK8_BOOT_HASH_ENTRY_SIZE;
        struct boot_hash_entry *entry = &boot_hash_entries[i];
        for (uint32_t j = 0u; j < BOOK8_BOOT_HASH_KEY_SIZE; ++j) {
            entry->key[j] = (char)source[j];
        }
        if (!boot_hash_key_valid(entry->key)) {
            return false;
        }
        for (uint32_t j = 0u; j < i; ++j) {
            if (boot_hash_key_equal(entry->key, boot_hash_entries[j].key)) {
                return false;
            }
        }
        for (uint32_t j = 0u; j < BOOK8_BOOT_HASH_DIGEST_SIZE; ++j) {
            entry->digest[j] = source[BOOK8_BOOT_HASH_KEY_SIZE + j];
        }
    }
    boot_hash_entry_count = count;
    boot_hash_loaded = true;
    return true;
}

bool boot_hash_manifest_valid(void) { return boot_hash_loaded; }

uint32_t boot_hash_manifest_count(void)
{
    return boot_hash_loaded ? boot_hash_entry_count : 0u;
}

bool boot_hash_key_for_path(const char *path, char *key, uint32_t capacity)
{
    if (path == NULL || key == NULL || capacity == 0u) {
        return false;
    }
    uint32_t index = 0u;
    if (path[0] != '\0' && path[1] == ':') {
        index = 2u;
    }
    while (path[index] == '/' || path[index] == '\\') {
        ++index;
    }
    uint32_t length = 0u;
    for (; path[index] != '\0'; ++index) {
        char value = path[index];
        if (value == '\\') {
            value = '/';
        }
        if (value >= 'a' && value <= 'z') {
            value = (char)(value - ('a' - 'A'));
        }
        if (value == '/' && length != 0u && key[length - 1u] == '/') {
            continue;
        }
        if (length + 1u >= capacity) {
            return false;
        }
        key[length++] = value;
    }
    while (length > 0u && key[length - 1u] == '/') {
        --length;
    }
    key[length] = '\0';
    return length != 0u && length < BOOK8_BOOT_HASH_KEY_SIZE;
}

const struct boot_hash_entry *boot_hash_find(const char *key)
{
    if (!boot_hash_loaded || key == NULL) {
        return NULL;
    }
    for (uint32_t i = 0u; i < boot_hash_entry_count; ++i) {
        if (boot_hash_key_equal(boot_hash_entries[i].key, key)) {
            return &boot_hash_entries[i];
        }
    }
    return NULL;
}

bool boot_hash_verify_memory(const char *key, const void *data, uint64_t size)
{
    const struct boot_hash_entry *entry = boot_hash_find(key);
    if (entry == NULL || data == NULL) {
        boot_hash_report_failure(key);
        return false;
    }
    uint8_t digest[BOOK8_BOOT_HASH_DIGEST_SIZE];
    book8_sha256(data, (size_t)size, digest);
    if (__builtin_memcmp(digest, entry->digest, sizeof(digest)) != 0) {
        boot_hash_report_failure(key);
        return false;
    }
    return true;
}

bool boot_hash_verify_path_memory(const char *path, const void *data,
    uint64_t size)
{
    char key[BOOK8_BOOT_HASH_KEY_SIZE];
    if (!boot_hash_key_for_path(path, key, sizeof(key))) {
        return false;
    }
    return boot_hash_verify_memory(key, data, size);
}

bool boot_hash_verify_file(const char *path, file_handle_t *file)
{
    char key[BOOK8_BOOT_HASH_KEY_SIZE];
    if (!boot_hash_key_for_path(path, key, sizeof(key)) || file == NULL
        || boot_hash_find(key) == NULL) {
        return false;
    }
    uint32_t saved_offset = file->offset;
    uint8_t buffer[4096u];
    struct book8_sha256 context;
    book8_sha256_init(&context);
    bool ok = fs_seek(file, 0u);
    uint32_t remaining = file->size;
    while (ok && remaining != 0u) {
        uint32_t chunk = remaining > sizeof(buffer)
            ? (uint32_t)sizeof(buffer) : remaining;
        int32_t read = fs_read(file, buffer, chunk);
        if (read <= 0 || (uint32_t)read != chunk) {
            ok = false;
            break;
        }
        book8_sha256_update(&context, buffer, chunk);
        remaining -= chunk;
    }
    uint8_t digest[BOOK8_BOOT_HASH_DIGEST_SIZE];
    book8_sha256_final(&context, digest);
    const struct boot_hash_entry *entry = boot_hash_find(key);
    if (ok && entry != NULL
        && __builtin_memcmp(digest, entry->digest, sizeof(digest)) != 0) {
        ok = false;
    }
    (void)fs_seek(file, saved_offset);
    if (!ok) {
        boot_hash_report_failure(key);
    }
    return ok;
}
