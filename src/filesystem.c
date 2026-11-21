#include "filesystem.h"

#include <stddef.h>
#include "../kernel/sched.h"

#define SECTOR_SIZE 512u
#define MAX_PARTITIONS 32u
#define MAX_NAME 256u
#define MAX_PATH 512u
#define MAX_OPEN_FILE_SIZE (1024u * 1024u)
#define MAX_NTFS_RUNS 32u
#define MAX_NTFS_RECORD 4096u
#define MBR_ROOT_START_LBA 2048u
#define MBR_SWAP_SECTORS (4ull * 1024u * 1024u * 1024u / SECTOR_SIZE)
#define MBR_MIN_ROOT_SECTORS (2u * 1024u * 1024u * 1024u / SECTOR_SIZE)
#define FAT_ATTR_DIRECTORY 0x10u
#define FAT_ATTR_VOLUME 0x08u
#define FAT_ATTR_LONG_NAME 0x0fu
#define FAT_CLUSTER_EOC 0x0fffffffu

struct partition {
    uint64_t first_lba;
    uint64_t sector_count;
    uint8_t type;
};

struct volume {
    bool mounted;
    bool fat32;
    bool ntfs;
    uint32_t disk;
    uint32_t partition_index;
    uint64_t start_lba;
    uint64_t sector_count;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_entries;
    uint32_t root_cluster;
    uint32_t first_data_sector;
    uint32_t root_directory_sector;
    uint32_t root_directory_sectors;
    uint32_t next_free_cluster;
    uint8_t media;
    char label[12];
    uint32_t ntfs_cluster_size;
    uint32_t ntfs_record_size;
    uint64_t ntfs_mft_size;
};

struct ntfs_run {
    uint64_t first_vcn;
    uint64_t cluster_count;
    int64_t first_lcn;
};

struct fat_slot {
    uint64_t sector;
    uint16_t offset;
};

struct directory_entry {
    char name[MAX_NAME];
    uint8_t raw_name[11];
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
    uint64_t full_size;
    uint64_t sector;
    uint16_t offset;
    uint16_t creation_date, creation_time;
    uint64_t ntfs_creation, ntfs_allocation;
    uint32_t ntfs_attributes;
    bool long_name, long_name_safe;
    uint8_t long_name_count;
    struct fat_slot long_name_slots[20];
};

static book8_read_sector_fn read_sector_callback;
static book8_write_sector_fn write_sector_callback;
static book8_disk_size_fn disk_size_callback;
static book8_print_fn print_callback;
static book8_confirm_fn confirm_callback;
static struct volume mounted_volume;
static char current_path[MAX_PATH] = "A:\\";
static uint8_t io_sector[SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t second_sector[SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t file_buffer[MAX_OPEN_FILE_SIZE] __attribute__((aligned(16)));
static uint8_t copy_verify_buffer[MAX_OPEN_FILE_SIZE] __attribute__((aligned(16)));
static uint8_t ntfs_record[MAX_NTFS_RECORD] __attribute__((aligned(16)));
static uint8_t ntfs_record_work[MAX_NTFS_RECORD] __attribute__((aligned(16)));
static struct ntfs_run ntfs_mft_runs[MAX_NTFS_RUNS];
static uint32_t ntfs_mft_run_count;
static void invalidate_space_samples(void);

static void print(const char *text)
{
    print_callback(text);
}

static void clear_memory(void *memory, size_t size)
{
    uint8_t *bytes = memory;
    for (size_t i = 0u; i < size; ++i) {
        bytes[i] = 0u;
    }
}

static bool equal_case_insensitive(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        char a = *left++;
        char b = *right++;
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (a != b) {
            return false;
        }
    }
    return *left == *right;
}

static char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text;
}

static size_t string_length(const char *text)
{
    size_t length = 0u;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static char *read_argument(char **text, char *output, size_t capacity)
{
    char *cursor = skip_spaces(*text);
    if (*cursor == '\0' || capacity == 0u) {
        return NULL;
    }
    bool quoted = *cursor == '"';
    if (quoted) {
        ++cursor;
    }
    size_t length = 0u;
    while (*cursor != '\0'
        && (quoted ? *cursor != '"' : (*cursor != ' ' && *cursor != '\t'))) {
        if (length + 1u >= capacity) {
            return NULL;
        }
        output[length++] = *cursor++;
    }
    if (quoted) {
        if (*cursor != '"') {
            return NULL;
        }
        ++cursor;
    }
    output[length] = '\0';
    *text = skip_spaces(cursor);
    return output;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u)
        | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4u) << 32u);
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u);
    p[3] = (uint8_t)(value >> 24u);
}

static void write_u64(uint8_t *p, uint64_t value)
{
    write_u32(p, (uint32_t)value);
    write_u32(p + 4u, (uint32_t)(value >> 32u));
}

static uint32_t fat_mask(void)
{
    return mounted_volume.fat32 ? 0x0fffffffu : 0x0000ffffu;
}

static bool volume_read(uint64_t relative_lba, uint8_t data[SECTOR_SIZE])
{
    if (!mounted_volume.mounted || relative_lba >= mounted_volume.sector_count
        || mounted_volume.start_lba + relative_lba >=
            disk_size_callback(mounted_volume.disk)) {
        return false;
    }
    return read_sector_callback(mounted_volume.disk,
        mounted_volume.start_lba + relative_lba, data);
}

static bool volume_write(uint64_t relative_lba,
    const uint8_t data[SECTOR_SIZE])
{
    invalidate_space_samples();
    if (!mounted_volume.mounted || relative_lba >= mounted_volume.sector_count
        || mounted_volume.start_lba + relative_lba >=
            disk_size_callback(mounted_volume.disk)) {
        return false;
    }
    return write_sector_callback(mounted_volume.disk,
        mounted_volume.start_lba + relative_lba, data);
}

static bool fat_read_entry(uint32_t cluster, uint32_t *value)
{
    uint32_t entry_size = mounted_volume.fat32 ? 4u : 2u;
    uint64_t byte_offset = (uint64_t)cluster * entry_size;
    uint64_t sector = mounted_volume.reserved_sectors
        + byte_offset / SECTOR_SIZE;
    uint32_t offset = (uint32_t)(byte_offset % SECTOR_SIZE);
    if (!volume_read(sector, io_sector)) {
        return false;
    }
    *value = mounted_volume.fat32
        ? read_u32(io_sector + offset) & fat_mask()
        : read_u16(io_sector + offset);
    return true;
}

static bool fat_write_entry_copy(uint32_t cluster, uint32_t value,
    uint32_t copy)
{
    uint32_t entry_size = mounted_volume.fat32 ? 4u : 2u;
    uint64_t byte_offset = (uint64_t)cluster * entry_size;
    uint64_t sector = mounted_volume.reserved_sectors
        + (uint64_t)copy * mounted_volume.sectors_per_fat
        + byte_offset / SECTOR_SIZE;
    uint32_t offset = (uint32_t)(byte_offset % SECTOR_SIZE);
    if (!volume_read(sector, io_sector)) {
        return false;
    }
    if (mounted_volume.fat32) {
        uint32_t current = read_u32(io_sector + offset);
        write_u32(io_sector + offset,
            (current & 0xf0000000u) | (value & 0x0fffffffu));
    } else {
        write_u16(io_sector + offset, (uint16_t)value);
    }
    return volume_write(sector, io_sector);
}

static bool fat_write_entry(uint32_t cluster, uint32_t value)
{
    for (uint32_t copy = 0u; copy < mounted_volume.fat_count; ++copy) {
        if (!fat_write_entry_copy(cluster, value, copy)) {
            return false;
        }
    }
    return true;
}

static uint64_t cluster_sector(uint32_t cluster)
{
    return mounted_volume.first_data_sector
        + (uint64_t)(cluster - 2u) * mounted_volume.sectors_per_cluster;
}

static uint32_t cluster_count(void)
{
    uint64_t data_sectors = mounted_volume.sector_count
        - mounted_volume.first_data_sector;
    return (uint32_t)(data_sectors / mounted_volume.sectors_per_cluster);
}

static bool is_end_of_chain(uint32_t cluster)
{
    return mounted_volume.fat32 ? cluster >= 0x0ffffff8u
        : cluster >= 0x0000fff8u;
}

static uint8_t short_name_checksum(const uint8_t name[11])
{
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < 11u; ++i) {
        sum = (uint8_t)(((sum & 1u) != 0u ? 0x80u : 0u)
            + (sum >> 1u) + name[i]);
    }
    return sum;
}

static void decode_long_name(const uint8_t entry[32], char name[MAX_NAME],
    uint32_t *length, uint32_t *expected_ordinal, bool *valid)
{
    static const uint8_t offsets[13] = {
        1,3,5,7,9,14,16,18,20,22,24,28,30
    };
    uint32_t ordinal = entry[0] & 0x1fu;
    if ((entry[0] & 0x40u) != 0u) {
        *length = 0u;
        *valid = ordinal != 0u && ordinal <= 20u;
        *expected_ordinal = ordinal;
        for (uint32_t i = 0u; i < MAX_NAME; ++i) {
            name[i] = '\0';
        }
    }
    if (!*valid || ordinal != *expected_ordinal) {
        *valid = false;
        return;
    }
    uint32_t base = (ordinal - 1u) * 13u;
    for (uint32_t i = 0u; i < 13u && base + i < MAX_NAME - 1u; ++i) {
        uint16_t value = read_u16(entry + offsets[i]);
        if (value == 0u || value == 0xffffu) { break; }
        if (value < 32u || value == '/' || value == '\\' || value == ':') {
            *valid = false;
            return;
        }
        name[base + i] = value < 0x80u ? (char)value : '?';
        if (*length < base + i + 1u) {
            *length = base + i + 1u;
        }
    }
    *expected_ordinal = ordinal - 1u;
}

static void make_short_name(const uint8_t raw[11], char name[MAX_NAME])
{
    uint32_t out = 0u;
    for (uint32_t i = 0u; i < 8u && raw[i] != ' ' && raw[i] != 0u; ++i) {
        char value = (char)raw[i];
        name[out++] = value == 0x05 ? (char)0xe5 : value;
    }
    uint32_t extension_length = 0u;
    while (extension_length < 3u && raw[8u + extension_length] != ' '
        && raw[8u + extension_length] != 0u) {
        ++extension_length;
    }
    if (extension_length != 0u) {
        name[out++] = '.';
        for (uint32_t i = 0u; i < extension_length; ++i) {
            name[out++] = (char)raw[8u + i];
        }
    }
    name[out] = '\0';
}

static bool name_matches(const char *left, const char *right)
{
    if (equal_case_insensitive(left, right)) {
        return true;
    }
    return false;
}

typedef bool (*directory_visitor)(const struct directory_entry *entry,
    void *context);
static bool directory_list_visitor(const struct directory_entry *entry,
    void *context);

static bool visit_directory(uint32_t first_cluster, bool fixed_root,
    directory_visitor visitor, void *context)
{
    uint64_t fixed_sector = mounted_volume.root_directory_sector;
    uint32_t fixed_sectors = mounted_volume.root_directory_sectors;
    uint32_t cluster = first_cluster;
    uint32_t steps = 0u;
    char long_name[MAX_NAME];
    uint32_t long_name_length = 0u;
    uint32_t expected_ordinal = 0u;
    bool long_name_valid = false;
    bool long_name_present = false;
    bool long_name_safe = true;
    uint32_t long_name_count = 0u;
    struct fat_slot long_name_slots[20];
    uint8_t expected_checksum = 0u;

    for (;;) {
        uint32_t sectors = fixed_root ? fixed_sectors
            : mounted_volume.sectors_per_cluster;
        uint64_t base = fixed_root ? fixed_sector : cluster_sector(cluster);
        for (uint32_t sector_index = 0u; sector_index < sectors; ++sector_index) {
            if (!volume_read(base + sector_index, io_sector)) {
                return false;
            }
            for (uint32_t offset = 0u; offset < SECTOR_SIZE; offset += 32u) {
                const uint8_t *raw = io_sector + offset;
                if (raw[0] == 0u) {
                    return true;
                }
                if (raw[0] == 0xe5u) {
                    long_name_valid = false;
                    long_name_length = 0u;
                    long_name_present = false;
                    long_name_count = 0u;
                    long_name_safe = true;
                    continue;
                }
                if (raw[11] == FAT_ATTR_LONG_NAME) {
                    static const uint8_t offsets[13] = {
                        1,3,5,7,9,14,16,18,20,22,24,28,30
                    };
                    long_name_present = true;
                    if (long_name_count >= 20u || raw[12] != 0u
                        || read_u16(raw + 26u) != 0u
                        || (raw[0] & 0xa0u) != 0u
                        || ((raw[0] & 0x40u) != 0u && long_name_count != 0u))
                        long_name_safe = false;
                    for (uint32_t i = 0u; i < 13u; ++i) {
                        uint16_t ch = read_u16(raw + offsets[i]);
                        if (ch != 0xffffu && ch >= 0x80u)
                            long_name_safe = false;
                    }
                    if (long_name_count < 20u) {
                        long_name_slots[long_name_count].sector = base + sector_index;
                        long_name_slots[long_name_count].offset = (uint16_t)offset;
                        ++long_name_count;
                    }
                    if ((raw[0] & 0x40u) != 0u) {
                        expected_checksum = raw[13];
                        long_name_valid = true;
                    }
                    if (long_name_valid && raw[13] == expected_checksum) {
                        decode_long_name(raw, long_name, &long_name_length,
                            &expected_ordinal, &long_name_valid);
                    } else {
                        long_name_valid = false;
                    }
                    continue;
                }
                if ((raw[11] & FAT_ATTR_VOLUME) != 0u) {
                    long_name_valid = false;
                    long_name_present = false;
                    long_name_count = 0u;
                    long_name_safe = true;
                    continue;
                }

                struct directory_entry entry;
                clear_memory(&entry, sizeof(entry));
                entry.long_name = long_name_present;
                entry.long_name_count = (uint8_t)long_name_count;
                for (uint32_t i = 0u; i < long_name_count; ++i)
                    entry.long_name_slots[i] = long_name_slots[i];
                long_name_present = false;
                long_name_count = 0u;
                for (uint32_t i = 0u; i < 11u; ++i) {
                    entry.raw_name[i] = raw[i];
                }
                entry.attributes = raw[11];
                entry.creation_time = read_u16(raw + 14u);
                entry.creation_date = read_u16(raw + 16u);
                entry.first_cluster = (uint32_t)read_u16(raw + 26u)
                    | ((uint32_t)read_u16(raw + 20u) << 16u);
                if (!mounted_volume.fat32) {
                    entry.first_cluster &= 0xffffu;
                }
                entry.size = read_u32(raw + 28u);
                entry.full_size = entry.size;
                entry.sector = base + sector_index;
                entry.offset = (uint16_t)offset;
                if (long_name_valid
                    && expected_checksum == short_name_checksum(entry.raw_name)
                    && long_name_length != 0u && expected_ordinal == 0u) {
                    long_name[long_name_length] = '\0';
                    entry.long_name = true;
                    entry.long_name_safe = long_name_safe;
                    for (uint32_t i = 0u; i < long_name_length; ++i) {
                        entry.name[i] = long_name[i];
                    }
                    entry.name[long_name_length] = '\0';
                } else {
                    make_short_name(entry.raw_name, entry.name);
                }
                long_name_valid = false;
                long_name_length = 0u;
                long_name_safe = true;
                if (!visitor(&entry, context)) {
                    return true;
                }
            }
        }
        if (fixed_root) {
            return true;
        }
        if (++steps > cluster_count() || !fat_read_entry(cluster, &cluster)) {
            return false;
        }
        if (is_end_of_chain(cluster)) {
            return true;
        }
        if (cluster < 2u || cluster >= cluster_count() + 2u)
            return false;
    }
}

struct find_context {
    const char *name;
    struct directory_entry *found;
    bool present;
};

static bool find_visitor(const struct directory_entry *entry, void *context)
{
    struct find_context *find = context;
    if (name_matches(entry->name, find->name)) {
        *find->found = *entry;
        find->present = true;
        return false;
    }
    return true;
}

static bool find_in_directory(uint32_t cluster, bool fixed_root,
    const char *name, struct directory_entry *found)
{
    struct find_context context = {name, found, false};
    if (!visit_directory(cluster, fixed_root, find_visitor, &context)) {
        return false;
    }
    return context.present;
}

static bool mount_fat_partition(uint32_t disk, uint32_t partition_index,
    const struct partition *partition)
{
    if (partition->sector_count < 16u || partition->first_lba
        > disk_size_callback(disk)
        || partition->sector_count > disk_size_callback(disk) - partition->first_lba
        || !read_sector_callback(disk, partition->first_lba, io_sector)) {
        return false;
    }
    uint16_t bytes_per_sector = read_u16(io_sector + 11u);
    uint8_t sectors_per_cluster = io_sector[13];
    uint16_t reserved = read_u16(io_sector + 14u);
    uint8_t fats = io_sector[16];
    uint16_t root_entries = read_u16(io_sector + 17u);
    uint32_t total_sectors = read_u16(io_sector + 19u);
    if (total_sectors == 0u) {
        total_sectors = read_u32(io_sector + 32u);
    }
    uint32_t sectors_per_fat16 = read_u16(io_sector + 22u);
    uint32_t sectors_per_fat32 = read_u32(io_sector + 36u);
    uint32_t fsinfo_sector = read_u16(io_sector + 48u);
    if (bytes_per_sector != SECTOR_SIZE || sectors_per_cluster == 0u
        || (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0u
        || sectors_per_cluster > 128u || reserved == 0u
        || (fats != 1u && fats != 2u) || total_sectors == 0u
        || total_sectors > partition->sector_count) {
        return false;
    }
    uint32_t sectors_per_fat = sectors_per_fat16 != 0u
        ? sectors_per_fat16 : sectors_per_fat32;
    if (sectors_per_fat == 0u) {
        return false;
    }
    uint64_t root_dir_sectors =
        ((uint64_t)root_entries * 32u + SECTOR_SIZE - 1u) / SECTOR_SIZE;
    uint64_t data_start = (uint64_t)reserved
        + (uint64_t)fats * sectors_per_fat + root_dir_sectors;
    if (data_start >= total_sectors) {
        return false;
    }
    uint32_t clusters = (uint32_t)((total_sectors - data_start)
        / sectors_per_cluster);
    bool fat32 = root_entries == 0u && sectors_per_fat16 == 0u
        && sectors_per_fat32 != 0u;
    if ((fat32 && (clusters < 65525u || read_u32(io_sector + 44u) < 2u))
        || (!fat32 && (clusters < 4085u || clusters >= 65525u))) {
        return false;
    }
    clear_memory(&mounted_volume, sizeof(mounted_volume));
    mounted_volume.mounted = true;
    mounted_volume.fat32 = fat32;
    mounted_volume.disk = disk;
    mounted_volume.partition_index = partition_index;
    mounted_volume.start_lba = partition->first_lba;
    mounted_volume.sector_count = total_sectors;
    mounted_volume.sectors_per_cluster = sectors_per_cluster;
    mounted_volume.reserved_sectors = reserved;
    mounted_volume.fat_count = fats;
    mounted_volume.sectors_per_fat = sectors_per_fat;
    mounted_volume.root_entries = root_entries;
    mounted_volume.root_cluster = fat32 ? read_u32(io_sector + 44u) : 0u;
    mounted_volume.first_data_sector = (uint32_t)data_start;
    mounted_volume.root_directory_sector = reserved + fats * sectors_per_fat;
    mounted_volume.root_directory_sectors = (uint32_t)root_dir_sectors;
    mounted_volume.next_free_cluster = 2u;
    if (fat32 && fsinfo_sector != 0u && fsinfo_sector < reserved
        && read_sector_callback(disk, partition->first_lba + fsinfo_sector,
            second_sector)
        && read_u32(second_sector) == 0x41615252u
        && read_u32(second_sector + 484u) == 0x61417272u
        && read_u32(second_sector + 508u) == 0xaa550000u) {
        uint32_t next_free = read_u32(second_sector + 492u);
        if (next_free >= 2u && next_free < clusters + 2u) {
            mounted_volume.next_free_cluster = next_free;
        }
    }
    mounted_volume.media = io_sector[21];
    const uint8_t *label = fat32 ? io_sector + 71u : io_sector + 43u;
    for (uint32_t i = 0u; i < 11u; ++i) {
        mounted_volume.label[i] = label[i] == ' ' ? '\0' : (char)label[i];
    }
    mounted_volume.label[11] = '\0';
    return true;
}

static bool ntfs_apply_fixups(uint8_t *record, uint32_t size)
{
    if (size < 512u) {
        return false;
    }
    uint16_t usa_offset = read_u16(record + 4u);
    uint16_t usa_count = read_u16(record + 6u);
    uint32_t sectors = size / SECTOR_SIZE;
    if (size % SECTOR_SIZE != 0u || usa_count != sectors + 1u
        || usa_offset + (uint32_t)usa_count * 2u > size) {
        return false;
    }
    uint16_t sequence = read_u16(record + usa_offset);
    for (uint32_t i = 1u; i <= sectors; ++i) {
        uint32_t end_offset = i * SECTOR_SIZE - 2u;
        if (read_u16(record + end_offset) != sequence) {
            return false;
        }
        write_u16(record + end_offset,
            read_u16(record + usa_offset + i * 2u));
    }
    return true;
}

static bool ntfs_fixup_record(uint8_t *record, uint32_t size)
{
    return record[0] == 'F' && record[1] == 'I'
        && record[2] == 'L' && record[3] == 'E'
        && ntfs_apply_fixups(record, size);
}

static bool ntfs_parse_runs(const uint8_t *data, uint32_t size,
    struct ntfs_run runs[MAX_NTFS_RUNS], uint32_t *run_count)
{
    *run_count = 0u;
    uint32_t offset = 0u;
    uint64_t vcn = 0u;
    int64_t lcn = 0;
    while (offset < size && data[offset] != 0u) {
        uint8_t header = data[offset++];
        uint32_t length_bytes = header & 0x0fu;
        uint32_t offset_bytes = header >> 4u;
        if (length_bytes == 0u || length_bytes > 8u || offset_bytes > 8u
            || offset + length_bytes + offset_bytes > size
            || *run_count >= MAX_NTFS_RUNS) {
            return false;
        }
        uint64_t length = 0u;
        for (uint32_t i = 0u; i < length_bytes; ++i) {
            length |= (uint64_t)data[offset + i] << (i * 8u);
        }
        offset += length_bytes;
        if (length == 0u) {
            return false;
        }
        if (offset_bytes == 0u) {
            return false;
        }
        uint64_t raw_delta = 0u;
        for (uint32_t i = 0u; i < offset_bytes; ++i) {
            raw_delta |= (uint64_t)data[offset + i] << (i * 8u);
        }
        int64_t delta;
        uint32_t sign_bit = offset_bytes * 8u - 1u;
        if ((raw_delta & (1ull << sign_bit)) == 0u) {
            delta = (int64_t)raw_delta;
        } else {
            uint64_t magnitude = offset_bytes == 8u
                ? ~raw_delta + 1u
                : (1ull << (offset_bytes * 8u)) - raw_delta;
            delta = magnitude == (1ull << 63u)
                ? INT64_MIN : -(int64_t)magnitude;
        }
        offset += offset_bytes;
        if ((delta > 0 && lcn > INT64_MAX - delta)
            || (delta < 0 && lcn < INT64_MIN - delta)) {
            return false;
        }
        lcn += delta;
        if (lcn < 0) {
            return false;
        }
        runs[*run_count].first_vcn = vcn;
        runs[*run_count].cluster_count = length;
        runs[*run_count].first_lcn = lcn;
        ++*run_count;
        if (vcn > UINT64_MAX - length) {
            return false;
        }
        vcn += length;
    }
    return *run_count != 0u;
}

static bool ntfs_read_stream(uint32_t disk, uint64_t volume_start,
    uint32_t sectors_per_cluster, const struct ntfs_run *runs,
    uint32_t run_count, uint64_t stream_size, uint64_t offset,
    uint8_t *destination, uint32_t length)
{
    if (offset > stream_size || length > stream_size - offset) {
        return false;
    }
    uint32_t copied = 0u;
    while (copied < length) {
        uint64_t position = offset + copied;
        uint64_t vcn = position
            / ((uint64_t)sectors_per_cluster * SECTOR_SIZE);
        uint64_t run_index = 0u;
        while (run_index < run_count
            && vcn >= runs[run_index].first_vcn
                + runs[run_index].cluster_count) {
            ++run_index;
        }
        if (run_index >= run_count || vcn < runs[run_index].first_vcn) {
            return false;
        }
        uint64_t total_clusters = mounted_volume.sector_count
            / sectors_per_cluster;
        if (runs[run_index].first_vcn > total_clusters
            || runs[run_index].cluster_count
                > total_clusters - runs[run_index].first_vcn
            || runs[run_index].first_lcn < 0
            || (uint64_t)runs[run_index].first_lcn >= total_clusters
            || runs[run_index].cluster_count > total_clusters
                - (uint64_t)runs[run_index].first_lcn) {
            return false;
        }
        uint64_t run_byte_start = runs[run_index].first_vcn
            * sectors_per_cluster * SECTOR_SIZE;
        uint64_t run_byte_end = (runs[run_index].first_vcn
            + runs[run_index].cluster_count)
            * sectors_per_cluster * SECTOR_SIZE;
        uint64_t in_run = position - run_byte_start;
        uint64_t lba_offset = (uint64_t)runs[run_index].first_lcn
            * sectors_per_cluster + in_run / SECTOR_SIZE;
        if (lba_offset >= mounted_volume.sector_count
            || volume_start > UINT64_MAX - lba_offset) {
            return false;
        }
        uint64_t lba = volume_start + lba_offset;
        if (lba >= volume_start + mounted_volume.sector_count
            || lba >= disk_size_callback(disk)) {
            return false;
        }
        uint32_t in_sector = (uint32_t)(in_run % SECTOR_SIZE);
        uint32_t amount = SECTOR_SIZE - in_sector;
        if (amount > length - copied) {
            amount = length - copied;
        }
        if (position + amount > run_byte_end
            || !read_sector_callback(disk, lba, io_sector)) {
            return false;
        }
        for (uint32_t i = 0u; i < amount; ++i) {
            destination[copied + i] = io_sector[in_sector + i];
        }
        copied += amount;
    }
    return true;
}

static bool ntfs_write_stream(uint32_t disk, uint64_t volume_start,
    uint32_t sectors_per_cluster, const struct ntfs_run *runs,
    uint32_t run_count, uint64_t stream_size, uint64_t offset,
    const uint8_t *source, uint32_t length)
{
    if (offset > stream_size || length > stream_size - offset) {
        return false;
    }
    uint32_t copied = 0u;
    while (copied < length) {
        uint64_t position = offset + copied;
        uint64_t vcn = position
            / ((uint64_t)sectors_per_cluster * SECTOR_SIZE);
        uint32_t run_index = 0u;
        while (run_index < run_count
            && vcn >= runs[run_index].first_vcn
                + runs[run_index].cluster_count) {
            ++run_index;
        }
        if (run_index >= run_count || vcn < runs[run_index].first_vcn) {
            return false;
        }
        uint64_t total_clusters = mounted_volume.sector_count
            / sectors_per_cluster;
        if (runs[run_index].first_vcn > total_clusters
            || runs[run_index].cluster_count
                > total_clusters - runs[run_index].first_vcn
            || runs[run_index].first_lcn < 0
            || (uint64_t)runs[run_index].first_lcn >= total_clusters
            || runs[run_index].cluster_count > total_clusters
                - (uint64_t)runs[run_index].first_lcn) {
            return false;
        }
        uint64_t run_byte_start = runs[run_index].first_vcn
            * sectors_per_cluster * SECTOR_SIZE;
        uint64_t run_byte_end = (runs[run_index].first_vcn
            + runs[run_index].cluster_count)
            * sectors_per_cluster * SECTOR_SIZE;
        uint64_t in_run = position - run_byte_start;
        uint64_t lba_offset = (uint64_t)runs[run_index].first_lcn
            * sectors_per_cluster + in_run / SECTOR_SIZE;
        if (lba_offset >= mounted_volume.sector_count
            || volume_start > UINT64_MAX - lba_offset) {
            return false;
        }
        uint64_t lba = volume_start + lba_offset;
        if (lba >= volume_start + mounted_volume.sector_count
            || lba >= disk_size_callback(disk)) {
            return false;
        }
        uint32_t in_sector = (uint32_t)(in_run % SECTOR_SIZE);
        uint32_t amount = SECTOR_SIZE - in_sector;
        if (amount > length - copied) {
            amount = length - copied;
        }
        if (position + amount > run_byte_end
            || !read_sector_callback(disk, lba, io_sector)) {
            return false;
        }
        for (uint32_t i = 0u; i < amount; ++i) {
            io_sector[in_sector + i] = source[copied + i];
        }
        if (!write_sector_callback(disk, lba, io_sector)) {
            return false;
        }
        copied += amount;
    }
    return true;
}

static bool ntfs_read_mft_record(uint64_t record_number,
    uint8_t record[MAX_NTFS_RECORD])
{
    if (record_number > UINT64_MAX / mounted_volume.ntfs_record_size) {
        return false;
    }
    uint64_t offset = record_number * mounted_volume.ntfs_record_size;
    if (offset > mounted_volume.ntfs_mft_size
        || mounted_volume.ntfs_record_size >
            mounted_volume.ntfs_mft_size - offset
        || !ntfs_read_stream(mounted_volume.disk, mounted_volume.start_lba,
            mounted_volume.ntfs_cluster_size / SECTOR_SIZE,
            ntfs_mft_runs, ntfs_mft_run_count, mounted_volume.ntfs_mft_size,
            offset, record, mounted_volume.ntfs_record_size)) {
        return false;
    }
    return ntfs_fixup_record(record, mounted_volume.ntfs_record_size);
}

static const uint8_t *ntfs_find_attribute(const uint8_t *record,
    uint32_t record_size, uint32_t type, const char *name)
{
    uint32_t offset = read_u16(record + 20u);
    while (offset + 16u <= record_size) {
        uint32_t current_type = read_u32(record + offset);
        if (current_type == 0xffffffffu) {
            break;
        }
        uint32_t length = read_u32(record + offset + 4u);
        if (length < 16u || length > record_size - offset) {
            return NULL;
        }
        uint8_t name_length = record[offset + 9u];
        uint16_t name_offset = read_u16(record + offset + 10u);
        bool matches_name = name == NULL && name_length == 0u;
        if (name != NULL && name_length == string_length(name)
            && name_offset + (uint32_t)name_length * 2u <= length) {
            matches_name = true;
            for (uint32_t i = 0u; i < name_length; ++i) {
                if (read_u16(record + offset + name_offset + i * 2u)
                    != (uint8_t)name[i]) {
                    matches_name = false;
                    break;
                }
            }
        }
        if (current_type == type && matches_name) {
            return record + offset;
        }
        offset += length;
    }
    return NULL;
}

static bool ntfs_mount_partition(uint32_t disk, uint32_t partition_index,
    const struct partition *partition)
{
    uint8_t boot[SECTOR_SIZE];
    if (partition->sector_count < 16u
        || partition->first_lba > disk_size_callback(disk)
        || partition->sector_count >
            disk_size_callback(disk) - partition->first_lba
        || !read_sector_callback(disk, partition->first_lba, boot)
        || boot[3] != 'N' || boot[4] != 'T' || boot[5] != 'F'
        || boot[6] != 'S' || boot[7] != ' '
        || boot[510] != 0x55u || boot[511] != 0xaau) {
        return false;
    }
    uint16_t bytes_per_sector = read_u16(boot + 11u);
    uint32_t sectors_per_cluster = boot[13];
    uint64_t total_sectors = read_u64(boot + 40u);
    uint64_t mft_lcn = read_u64(boot + 48u);
    int8_t record_code = (int8_t)boot[64];
    uint32_t record_size = 0u;
    if (record_code > 0 && record_code <= 16) {
        record_size = sectors_per_cluster * SECTOR_SIZE
            * (uint8_t)record_code;
    } else if (record_code >= -12 && record_code <= -9) {
        record_size = 1u << (uint8_t)(-record_code);
    }
    if (bytes_per_sector != SECTOR_SIZE || sectors_per_cluster == 0u
        || sectors_per_cluster > 128u
        || (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0u
        || total_sectors == 0u || total_sectors > partition->sector_count
        || mft_lcn >= total_sectors / sectors_per_cluster
        || record_size < 512u || record_size > MAX_NTFS_RECORD
        || record_size % SECTOR_SIZE != 0u) {
        return false;
    }

    uint64_t mft_start = partition->first_lba
        + mft_lcn * sectors_per_cluster;
    if (record_size / SECTOR_SIZE > total_sectors
        - mft_lcn * sectors_per_cluster) {
        return false;
    }
    for (uint32_t i = 0u; i < record_size / SECTOR_SIZE; ++i) {
        if (!read_sector_callback(disk, mft_start + i, io_sector)) {
            return false;
        }
        for (uint32_t j = 0u; j < SECTOR_SIZE; ++j) {
            ntfs_record[i * SECTOR_SIZE + j] = io_sector[j];
        }
    }
    if (!ntfs_fixup_record(ntfs_record, record_size)) {
        return false;
    }
    const uint8_t *data = ntfs_find_attribute(ntfs_record, record_size,
        0x80u, NULL);
    if (data == NULL || data[8] == 0u) {
        return false;
    }
    uint16_t runlist_offset = read_u16(data + 32u);
    uint32_t attribute_size = read_u32(data + 4u);
    uint64_t mft_size = read_u64(data + 48u);
    if (mft_size < record_size || runlist_offset >= attribute_size
        || !ntfs_parse_runs(data + runlist_offset,
            attribute_size - runlist_offset, ntfs_mft_runs,
            &ntfs_mft_run_count)) {
        return false;
    }
    clear_memory(&mounted_volume, sizeof(mounted_volume));
    mounted_volume.mounted = true;
    mounted_volume.ntfs = true;
    mounted_volume.disk = disk;
    mounted_volume.partition_index = partition_index;
    mounted_volume.start_lba = partition->first_lba;
    mounted_volume.sector_count = total_sectors;
    mounted_volume.ntfs_cluster_size = sectors_per_cluster * SECTOR_SIZE;
    mounted_volume.ntfs_record_size = record_size;
    mounted_volume.ntfs_mft_size = mft_size;
    return true;
}

static uint64_t ntfs_index_record_number(const uint8_t *entry)
{
    return read_u64(entry) & 0x0000ffffffffffffull;
}

static bool ntfs_visit_index_header(const uint8_t *header,
    uint32_t available, directory_visitor visitor, void *context,
    const char *find_name, struct directory_entry *found,
    bool *found_entry, bool *stop)
{
    uint32_t entries_offset = read_u32(header);
    uint32_t index_size = read_u32(header + 4u);
    if (entries_offset < 16u || entries_offset > index_size
        || index_size > available) {
        return false;
    }
    uint32_t offset = entries_offset;
    while (offset <= index_size && index_size - offset >= 16u) {
        const uint8_t *entry = header + offset;
        uint16_t entry_length = read_u16(entry + 8u);
        uint16_t key_length = read_u16(entry + 10u);
        uint16_t flags = read_u16(entry + 12u);
        if (entry_length < 16u || entry_length > index_size - offset
            || key_length > entry_length - 16u) {
            return false;
        }
        if ((flags & 2u) == 0u && key_length >= 66u) {
            const uint8_t *key = entry + 16u;
            uint8_t name_length = key[64u];
            uint32_t name_bytes = (uint32_t)name_length * 2u;
            if (66u + name_bytes > key_length) {
                return false;
            }
            if (key[65u] != 2u) {
                struct directory_entry item;
                clear_memory(&item, sizeof(item));
                for (uint32_t i = 0u; i < name_length; ++i) {
                    uint16_t character = read_u16(key + 66u + i * 2u);
                    item.name[i] = character < 0x80u
                        ? (char)character : '?';
                }
                item.name[name_length] = '\0';
                item.first_cluster =
                    (uint32_t)ntfs_index_record_number(entry);
                uint64_t file_size = read_u64(key + 48u);
                item.full_size = file_size;
                item.size = file_size > UINT32_MAX
                    ? UINT32_MAX : (uint32_t)file_size;
                item.attributes =
                    (read_u32(key + 56u) & 0x10000000u) != 0u
                        ? FAT_ATTR_DIRECTORY : 0u;
                item.ntfs_creation = read_u64(key + 8u);
                item.ntfs_allocation = read_u64(key + 40u);
                item.ntfs_attributes = read_u32(key + 56u);
                if (find_name != NULL
                    && name_matches(item.name, find_name)) {
                    *found = item;
                    *found_entry = true;
                    *stop = true;
                    return true;
                }
                if (visitor != NULL && !visitor(&item, context)) {
                    *stop = true;
                    return true;
                }
            }
        }
        offset += entry_length;
        if ((flags & 2u) != 0u) {
            return true;
        }
    }
    return false;
}

static bool ntfs_index_visit(uint64_t directory_record,
    directory_visitor visitor, void *context, const char *find_name,
    struct directory_entry *found, bool *found_entry)
{
    if (!ntfs_read_mft_record(directory_record, ntfs_record)) {
        return false;
    }
    const uint8_t *index = ntfs_find_attribute(ntfs_record,
        mounted_volume.ntfs_record_size, 0x90u, "$I30");
    if (index == NULL || index[8] != 0u) {
        return false;
    }
    uint32_t value_length = read_u32(index + 16u);
    uint16_t value_offset = read_u16(index + 20u);
    uint32_t index_attribute_size = read_u32(index + 4u);
    if (value_offset < 24u || value_offset > index_attribute_size
        || value_length > index_attribute_size - value_offset) {
        return false;
    }
    const uint8_t *root = index + value_offset;
    if (value_length < 32u) {
        return false;
    }
    bool stop = false;
    if (!ntfs_visit_index_header(root + 16u, value_length - 16u,
        visitor, context, find_name, found, found_entry, &stop)) {
        return false;
    }
    if (stop || (read_u32(root + 28u) & 1u) == 0u) {
        return true;
    }

    uint32_t index_block_size = read_u32(root + 8u);
    const uint8_t *allocation = ntfs_find_attribute(ntfs_record,
        mounted_volume.ntfs_record_size, 0xa0u, "$I30");
    const uint8_t *bitmap = ntfs_find_attribute(ntfs_record,
        mounted_volume.ntfs_record_size, 0xb0u, "$I30");
    if (index_block_size < SECTOR_SIZE
        || index_block_size > MAX_NTFS_RECORD
        || index_block_size % SECTOR_SIZE != 0u || allocation == NULL
        || allocation[8] == 0u || bitmap == NULL || bitmap[8] != 0u) {
        return false;
    }
    uint32_t allocation_attribute_size = read_u32(allocation + 4u);
    uint16_t runlist_offset = read_u16(allocation + 32u);
    uint64_t allocation_size = read_u64(allocation + 48u);
    uint64_t initialized_size = read_u64(allocation + 56u);
    struct ntfs_run runs[MAX_NTFS_RUNS];
    uint32_t run_count;
    if (runlist_offset >= allocation_attribute_size
        || initialized_size > allocation_size
        || !ntfs_parse_runs(allocation + runlist_offset,
            allocation_attribute_size - runlist_offset, runs, &run_count)) {
        return false;
    }
    uint32_t bitmap_attribute_size = read_u32(bitmap + 4u);
    uint32_t bitmap_length = read_u32(bitmap + 16u);
    uint16_t bitmap_offset = read_u16(bitmap + 20u);
    if (bitmap_offset < 24u || bitmap_offset > bitmap_attribute_size
        || bitmap_length > bitmap_attribute_size - bitmap_offset) {
        return false;
    }
    const uint8_t *bitmap_data = bitmap + bitmap_offset;
    uint64_t index_count = initialized_size / index_block_size;
    if (index_count > (uint64_t)bitmap_length * 8u) {
        return false;
    }
    for (uint64_t i = 0u; i < index_count; ++i) {
        if ((bitmap_data[i / 8u] & (1u << (i % 8u))) == 0u) {
            continue;
        }
        uint64_t offset = i * index_block_size;
        if (!ntfs_read_stream(mounted_volume.disk,
            mounted_volume.start_lba,
            mounted_volume.ntfs_cluster_size / SECTOR_SIZE,
            runs, run_count, initialized_size, offset,
            ntfs_record_work, index_block_size)
            || ntfs_record_work[0] != 'I' || ntfs_record_work[1] != 'N'
            || ntfs_record_work[2] != 'D' || ntfs_record_work[3] != 'X'
            || !ntfs_apply_fixups(ntfs_record_work, index_block_size)
            || !ntfs_visit_index_header(ntfs_record_work + 24u,
                index_block_size - 24u, visitor, context, find_name,
                found, found_entry, &stop)) {
            return false;
        }
        if (stop) {
            return true;
        }
    }
    return true;
}

static bool ntfs_find_root_file_in(uint64_t directory,
    const char *name, struct directory_entry *entry)
{
    bool found = false;
    return ntfs_index_visit(directory, NULL, NULL, name, entry, &found)
        && found;
}

static bool ntfs_find_root_file(const char *name,
    struct directory_entry *entry)
{
    bool found = false;
    return ntfs_index_visit(5u, NULL, NULL, name, entry, &found) && found;
}

static bool ntfs_list_root(void)
{
    bool ignored = false;
    print("NTFS volume mounted (limited, uncompressed files only).\n");
    return ntfs_index_visit(5u, directory_list_visitor, NULL,
        NULL, NULL, &ignored);
}

static bool ntfs_get_file_data(uint64_t record_number, uint8_t *destination,
    uint32_t capacity, uint32_t *file_size, const uint8_t **data_attribute)
{
    if (!ntfs_read_mft_record(record_number, ntfs_record_work)) {
        return false;
    }
    const uint8_t *data = ntfs_find_attribute(ntfs_record_work,
        mounted_volume.ntfs_record_size, 0x80u, NULL);
    if (data == NULL) {
        return false;
    }
    bool nonresident = data[8] != 0u;
    uint64_t size = nonresident ? read_u64(data + 48u)
        : read_u32(data + 16u);
    if (size > capacity) {
        return false;
    }
    *file_size = (uint32_t)size;
    if (data_attribute != NULL) {
        *data_attribute = data;
    }
    if (!nonresident) {
        uint32_t value_length = read_u32(data + 16u);
        uint16_t value_offset = read_u16(data + 20u);
        uint32_t attribute_size = read_u32(data + 4u);
        if (value_offset < 24u || value_length != size
            || value_offset > attribute_size
            || value_length > attribute_size - value_offset) {
            return false;
        }
        for (uint32_t i = 0u; i < size; ++i) {
            destination[i] = data[value_offset + i];
        }
        return true;
    }
    uint32_t attribute_size = read_u32(data + 4u);
    uint16_t runlist_offset = read_u16(data + 32u);
    struct ntfs_run runs[MAX_NTFS_RUNS];
    uint32_t count;
    if (runlist_offset >= attribute_size
        || !ntfs_parse_runs(data + runlist_offset,
            attribute_size - runlist_offset, runs, &count)) {
        return false;
    }
    return ntfs_read_stream(mounted_volume.disk, mounted_volume.start_lba,
        mounted_volume.ntfs_cluster_size / SECTOR_SIZE, runs, count,
        read_u64(data + 48u), 0u, destination, (uint32_t)size);
}

static bool ntfs_read_file_root(const char *name, uint8_t *destination,
    uint32_t capacity, uint32_t *file_size)
{
    struct directory_entry entry;
    if (!ntfs_find_root_file(name, &entry)
        || (entry.attributes & FAT_ATTR_DIRECTORY) != 0u) {
        return false;
    }
    return ntfs_get_file_data(entry.first_cluster, destination, capacity,
        file_size, NULL);
}

static bool ntfs_write_existing_file(const char *name, const uint8_t *data,
    uint32_t size)
{
    struct directory_entry entry;
    if (!ntfs_find_root_file(name, &entry)
        || (entry.attributes & FAT_ATTR_DIRECTORY) != 0u
        || !ntfs_read_mft_record(entry.first_cluster, ntfs_record_work)) {
        print("NTFS file not found or unsupported.\n");
        return false;
    }
    const uint8_t *attribute = ntfs_find_attribute(ntfs_record_work,
        mounted_volume.ntfs_record_size, 0x80u, NULL);
    if (attribute == NULL || attribute[8] == 0u) {
        print("NTFS writes currently require an existing non-resident data stream.\n");
        return false;
    }
    uint64_t stream_size = read_u64(attribute + 48u);
    uint32_t attribute_size = read_u32(attribute + 4u);
    uint16_t runlist_offset = read_u16(attribute + 32u);
    struct ntfs_run runs[MAX_NTFS_RUNS];
    uint32_t run_count;
    if (stream_size != size || runlist_offset >= attribute_size
        || !ntfs_parse_runs(attribute + runlist_offset,
            attribute_size - runlist_offset, runs, &run_count)) {
        print("NTFS subset only overwrites an existing file without changing its size.\n");
        return false;
    }
    if (!confirm_callback("WARNING: overwrite the existing NTFS file in place. Type YES: ")) {
        print("NTFS write cancelled.\n");
        return false;
    }
    if (!ntfs_write_stream(mounted_volume.disk, mounted_volume.start_lba,
        mounted_volume.ntfs_cluster_size / SECTOR_SIZE, runs, run_count,
        stream_size, 0u, data, size)) {
        print("NTFS file write failed; verify the file contents.\n");
        return false;
    }
    print("NTFS file overwritten in place. The volume must be cleanly unmounted before use by another OS.\n");
    return true;
}

static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0u; i < size; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t read_partitions(uint32_t disk,
    struct partition partitions[MAX_PARTITIONS], bool *is_gpt)
{
    *is_gpt = false;
    uint64_t disk_sectors = disk_size_callback(disk);
    if (disk_sectors < 2u || !read_sector_callback(disk, 0u, io_sector)
        || io_sector[510] != 0x55u || io_sector[511] != 0xaau) {
        return 0u;
    }
    if (read_sector_callback(disk, 1u, second_sector)
        && second_sector[0] == 'E' && second_sector[1] == 'F'
        && second_sector[2] == 'I' && second_sector[3] == ' ' ) {
        *is_gpt = true;
        uint32_t header_size = read_u32(second_sector + 12u);
        uint32_t stored_crc = read_u32(second_sector + 16u);
        if (header_size < 92u || header_size > SECTOR_SIZE) {
            return 0u;
        }
        second_sector[16] = 0u;
        second_sector[17] = 0u;
        second_sector[18] = 0u;
        second_sector[19] = 0u;
        if (crc32(second_sector, header_size) != stored_crc) {
            return 0u;
        }
        uint64_t entries_lba = read_u64(second_sector + 72u);
        uint32_t entry_count = read_u32(second_sector + 80u);
        uint32_t entry_size = read_u32(second_sector + 84u);
        uint32_t entries_crc = read_u32(second_sector + 88u);
        if (entry_count > 128u || entry_size < 128u || entry_size > 512u
            || entries_lba >= disk_sectors) {
            return 0u;
        }
        uint64_t byte_count = (uint64_t)entry_count * entry_size;
        uint32_t sector_count = (uint32_t)((byte_count + 511u) / 512u);
        static uint8_t entries[128u * 128u];
        if (byte_count > sizeof(entries) || entries_lba + sector_count > disk_sectors) {
            return 0u;
        }
        for (uint32_t i = 0u; i < sector_count; ++i) {
            if (!read_sector_callback(disk, entries_lba + i, io_sector)) {
                return 0u;
            }
            uint32_t amount = (uint32_t)(byte_count - (uint64_t)i * 512u);
            if (amount > 512u) {
                amount = 512u;
            }
            for (uint32_t j = 0u; j < amount; ++j) {
                entries[i * 512u + j] = io_sector[j];
            }
        }
        if (crc32(entries, (size_t)byte_count) != entries_crc) {
            return 0u;
        }
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < entry_count && count < MAX_PARTITIONS; ++i) {
            const uint8_t *entry = entries + (size_t)i * entry_size;
            bool empty = true;
            for (uint32_t j = 0u; j < 16u; ++j) {
                empty = empty && entry[j] == 0u;
            }
            if (empty) {
                continue;
            }
            uint64_t first = read_u64(entry + 32u);
            uint64_t last = read_u64(entry + 40u);
            if (first > last || last >= disk_sectors) {
                continue;
            }
            partitions[count].first_lba = first;
            partitions[count].sector_count = last - first + 1u;
            partitions[count].type = (entry[0] == 0u && entry[1] == 0u
                && entry[2] == 0u && entry[3] == 0u) ? 0u : 0x0cu;
            ++count;
        }
        return count;
    }

    uint32_t count = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t *entry = io_sector + 446u + i * 16u;
        uint8_t type = entry[4];
        uint64_t first = read_u32(entry + 8u);
        uint64_t sectors = read_u32(entry + 12u);
        if (type == 0u || sectors == 0u || first >= disk_sectors
            || sectors > disk_sectors - first) {
            continue;
        }
        partitions[count].first_lba = first;
        partitions[count].sector_count = sectors;
        partitions[count].type = type;
        ++count;
    }
    return count;
}

static bool mount_partition(uint32_t disk, uint32_t partition_index,
    const struct partition *partition)
{
    if (partition->type == 0xeeu) {
        return false;
    }
    if (partition->type == 0x07u) {
        return ntfs_mount_partition(disk, partition_index, partition);
    }
    if (ntfs_mount_partition(disk, partition_index, partition)) {
        return true;
    }
    return mount_fat_partition(disk, partition_index, partition);
}

#define MAX_DRIVES 26u
#define MAX_DISKS 16u

struct drive_record {
    uint32_t disk;
    uint32_t partition_index;
    struct partition partition;
    bool whole_disk;
    bool readable;
    bool ntfs;
    bool fat32;
    int error;
    char label[12];
};

static struct drive_record drives[MAX_DRIVES];
static uint32_t drive_total;
static int32_t active_drive = -1;
static bool drives_dirty;
static int last_error;
static struct {
    bool initialized;
    struct book8_drive_space space;
    uint64_t fat_lba;
    uint32_t clusters, next_cluster, cluster_bytes, entry_bytes;
} space_samples[MAX_DRIVES];

static void invalidate_space_samples(void)
{
    for (uint32_t i = 0u; i < MAX_DRIVES; ++i) {
        space_samples[i].initialized = false;
    }
}

static void record_drive(uint32_t disk, uint32_t partition_index,
    const struct partition *partition, bool whole_disk, bool readable,
    int error)
{
    if (drive_total >= MAX_DRIVES) {
        return;
    }
    struct drive_record *drive = &drives[drive_total++];
    clear_memory(drive, sizeof(*drive));
    drive->disk = disk;
    drive->partition_index = partition_index;
    drive->partition = *partition;
    drive->whole_disk = whole_disk;
    drive->readable = readable;
    drive->error = error;
    if (readable) {
        drive->ntfs = mounted_volume.ntfs;
        drive->fat32 = mounted_volume.fat32;
        for (uint32_t i = 0u; i < 11u; ++i) {
            drive->label[i] = mounted_volume.label[i];
        }
    }
}

static bool mount_drive_record(const struct drive_record *drive)
{
    if (drive->whole_disk) {
        struct partition whole_disk = drive->partition;
        return ntfs_mount_partition(drive->disk, 0u, &whole_disk)
            || mount_fat_partition(drive->disk, 0u, &whole_disk);
    }
    return mount_partition(drive->disk, drive->partition_index,
        &drive->partition);
}

/* Probe every disk and register each mountable volume; letters follow
   discovery order, so the first volume found (the boot volume) is A:. Disks
   with no readable volume are listed last so selecting them reports why. */
static void scan_drives(void)
{
    invalidate_space_samples();
    struct partition partitions[MAX_PARTITIONS];
    bool disk_has_drive[MAX_DISKS] = {false};
    bool disk_bad_table[MAX_DISKS] = {false};
    drive_total = 0u;
    active_drive = -1;
    drives_dirty = false;
    for (uint32_t disk = 0u; disk < MAX_DISKS; ++disk) {
        if (disk_size_callback(disk) == 0u) {
            continue;
        }
        bool is_gpt;
        uint32_t count = read_partitions(disk, partitions, &is_gpt);
        if (is_gpt && count == 0u) {
            disk_bad_table[disk] = true;
            continue;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            if (mount_partition(disk, i, &partitions[i])) {
                record_drive(disk, i, &partitions[i], false, true,
                    BOOK8_FS_OK);
                disk_has_drive[disk] = true;
            }
        }
        if (count == 0u) {
            struct partition whole_disk = {0u, disk_size_callback(disk), 0u};
            if (ntfs_mount_partition(disk, 0u, &whole_disk)
                || mount_fat_partition(disk, 0u, &whole_disk)) {
                record_drive(disk, 0u, &whole_disk, true, true,
                    BOOK8_FS_OK);
                disk_has_drive[disk] = true;
            }
        }
    }
    for (uint32_t disk = 0u; disk < MAX_DISKS; ++disk) {
        if (disk_size_callback(disk) == 0u || disk_has_drive[disk]) {
            continue;
        }
        struct partition none = {0u, disk_size_callback(disk), 0u};
        record_drive(disk, 0u, &none, true, false,
            disk_bad_table[disk] ? BOOK8_FS_ERR_NO_PARTITION
                                 : BOOK8_FS_ERR_NO_VOLUME);
    }
    clear_memory(&mounted_volume, sizeof(mounted_volume));
}

static bool activate_drive(uint32_t index)
{
    if (drives_dirty) {
        scan_drives();
    }
    if (index >= drive_total) {
        last_error = BOOK8_FS_ERR_NO_DRIVE;
        return false;
    }
    if (!drives[index].readable) {
        last_error = drives[index].error;
        return false;
    }
    if ((int32_t)index == active_drive && mounted_volume.mounted) {
        return true;
    }
    if (!mount_drive_record(&drives[index])) {
        mounted_volume.mounted = false;
        active_drive = -1;
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    active_drive = (int32_t)index;
    return true;
}

static bool fs_drive_extent_unlocked(uint32_t index, uint32_t *disk, uint64_t *first_lba,
    uint64_t *sector_count)
{
    if (drives_dirty) {
        scan_drives();
    }
    if (index >= drive_total || !drives[index].readable || drives[index].ntfs
        || disk == NULL || first_lba == NULL || sector_count == NULL) {
        return false;
    }
    *disk = drives[index].disk;
    *first_lba = drives[index].partition.first_lba;
    *sector_count = drives[index].partition.sector_count;
    return true;
}

static bool auto_mount(void)
{
    scan_drives();
    return activate_drive(0u);
}

static bool fs_mount_disk_as_root_unlocked(uint32_t disk)
{
    scan_drives();
    uint32_t selected = drive_total;
    for (uint32_t i = 0u; i < drive_total; ++i) {
        if (drives[i].disk == disk && drives[i].readable && drives[i].fat32) {
            selected = i;
            break;
        }
    }
    if (selected == drive_total) {
        last_error = BOOK8_FS_ERR_NO_VOLUME;
        return false;
    }

    struct drive_record root = drives[selected];
    for (uint32_t i = selected; i > 0u; --i) {
        drives[i] = drives[i - 1u];
    }
    drives[0] = root;
    active_drive = -1;
    clear_memory(&mounted_volume, sizeof(mounted_volume));
    if (!activate_drive(0u)) {
        return false;
    }
    current_path[0] = 'A';
    current_path[1] = ':';
    current_path[2] = '\\';
    current_path[3] = '\0';
    last_error = BOOK8_FS_OK;
    return true;
}

static uint32_t root_cluster(void)
{
    return mounted_volume.fat32 ? mounted_volume.root_cluster : 0u;
}

static bool root_is_fixed(void)
{
    return !mounted_volume.fat32;
}

static bool directory_list_visitor(const struct directory_entry *entry,
    void *context)
{
    (void)context;
    print(entry->name);
    if ((entry->attributes & FAT_ATTR_DIRECTORY) != 0u) {
        print(" <DIR>");
    } else {
        print("  ");
        char digits[11];
        uint32_t count = 0u;
        uint32_t value = entry->size;
        do {
            digits[count++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        while (count > 0u) {
            char single[2] = {digits[--count], '\0'};
            print(single);
        }
    }
    print("\n");
    return true;
}

static bool list_root(void)
{
    if (!mounted_volume.mounted) {
        print("No FAT volume is mounted. Use MOUNT <disk-number>.\n");
        return false;
    }
    if (mounted_volume.ntfs) {
        return ntfs_list_root();
    }
    print("Volume ");
    print(mounted_volume.label[0] != '\0' ? mounted_volume.label : "(no label)");
    print(mounted_volume.fat32 ? " (FAT32)\n" : " (FAT16)\n");
    return visit_directory(root_cluster(), root_is_fixed(),
        directory_list_visitor, NULL);
}

struct public_directory_context {
    book8_directory_entry_fn visitor;
    void *context;
};

static bool public_directory_visitor(const struct directory_entry *entry,
    void *context)
{
    struct public_directory_context *public_context = context;
    if (entry->name[0] == '.' && (entry->name[1] == '\0'
        || (entry->name[1] == '.' && entry->name[2] == '\0'))) {
        return true;
    }
    return public_context->visitor(entry->name,
        (entry->attributes & FAT_ATTR_DIRECTORY) != 0u,
        entry->size, public_context->context);
}

static bool path_separator(char value)
{
    return value == '\\' || value == '/';
}

static bool valid_drive_path(const char *path)
{
    return path != NULL
        && ((path[0] >= 'A' && path[0] <= 'Z')
            || (path[0] >= 'a' && path[0] <= 'z'))
        && path[1] == ':' && path_separator(path[2]);
}

static bool resolve_fat_directory(const char *path, uint32_t *cluster,
    bool *fixed_root)
{
    if (cluster == NULL || fixed_root == NULL || !valid_drive_path(path)) {
        return false;
    }

    *cluster = root_cluster();
    *fixed_root = root_is_fixed();
    const char *cursor = path + 3u;
    while (*cursor != '\0') {
        char component[MAX_NAME];
        uint32_t length = 0u;
        while (*cursor != '\0' && path_separator(*cursor)) {
            ++cursor;
        }
        while (*cursor != '\0' && !path_separator(*cursor)) {
            if (length + 1u >= sizeof(component)) {
                return false;
            }
            component[length++] = *cursor++;
        }
        if (length == 0u) {
            continue;
        }
        component[length] = '\0';
        if (equal_case_insensitive(component, ".")) {
            continue;
        }
        if (equal_case_insensitive(component, "..")) {
            return false;
        }

        struct directory_entry entry;
        if (!find_in_directory(*cluster, *fixed_root, component, &entry)
            || (entry.attributes & FAT_ATTR_DIRECTORY) == 0u
            || entry.first_cluster < 2u
            || entry.first_cluster >= cluster_count() + 2u) {
            return false;
        }
        *cluster = entry.first_cluster;
        *fixed_root = false;
    }
    return true;
}

static bool resolve_ntfs_directory(const char *path, uint64_t *record)
{
    if (record == NULL || !valid_drive_path(path)) {
        return false;
    }
    *record = 5u;
    const char *cursor = path + 3u;
    while (*cursor != '\0') {
        char component[MAX_NAME];
        uint32_t length = 0u;
        while (*cursor != '\0' && path_separator(*cursor)) {
            ++cursor;
        }
        while (*cursor != '\0' && !path_separator(*cursor)) {
            if (length + 1u >= sizeof(component)) {
                return false;
            }
            component[length++] = *cursor++;
        }
        if (length == 0u) {
            continue;
        }
        component[length] = '\0';
        if (equal_case_insensitive(component, ".")) {
            continue;
        }
        struct directory_entry entry;
        if (equal_case_insensitive(component, "..")
            || !ntfs_find_root_file_in(*record, component, &entry)
            || (entry.attributes & FAT_ATTR_DIRECTORY) == 0u) {
            return false;
        }
        *record = entry.first_cluster;
    }
    return true;
}

static bool select_drive_for_path(const char *path)
{
    if (!valid_drive_path(path)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    char letter = path[0] >= 'a' ? (char)(path[0] - 'a' + 'A') : path[0];
    return activate_drive((uint32_t)(letter - 'A'));
}

static bool resolve_entry_path(const char *path, struct directory_entry *entry)
{
    char directory[MAX_PATH], name[MAX_NAME];
    size_t length = 0u, split = 0u;
    if (!valid_drive_path(path)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    while (path[length] != '\0') {
        if (path_separator(path[length]))
            split = length;
        ++length;
    }
    size_t name_length = length - split - 1u;
    if (split + 1u >= sizeof(directory) || name_length == 0u
        || name_length >= sizeof(name)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    for (size_t i = 0u; i <= split; ++i)
        directory[i] = path[i];
    directory[split + 1u] = '\0';
    for (size_t i = 0u; i <= name_length; ++i)
        name[i] = path[split + i + 1u];
    if (equal_case_insensitive(name, ".") || equal_case_insensitive(name, "..")) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    if (!select_drive_for_path(directory))
        return false;
    bool found;
    if (mounted_volume.ntfs) {
        uint64_t record;
        found = resolve_ntfs_directory(directory, &record)
            && ntfs_find_root_file_in(record, name, entry);
    } else {
        uint32_t cluster;
        bool fixed;
        found = resolve_fat_directory(directory, &cluster, &fixed)
            && find_in_directory(cluster, fixed, name, entry);
    }
    if (!found)
        last_error = BOOK8_FS_ERR_NOT_FOUND;
    return found;
}

static bool leap_year(uint32_t year)
{
    return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

static void ntfs_creation_time(uint64_t timestamp, struct book8_file_info *info)
{
    static const uint8_t month_days[] =
        {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (timestamp == 0u)
        return;
    uint64_t seconds = timestamp / 10000000u;
    uint64_t days = seconds / 86400u;
    uint32_t year = 1601u;
    while (year <= 9999u) {
        uint32_t n = leap_year(year) ? 366u : 365u;
        if (days < n)
            break;
        days -= n;
        ++year;
    }
    if (year > 9999u)
        return;
    uint32_t month = 0u;
    while (month < 12u) {
        uint32_t n = month_days[month] + (month == 1u && leap_year(year) ? 1u : 0u);
        if (days < n)
            break;
        days -= n;
        ++month;
    }
    info->year = (uint16_t)year;
    info->month = (uint8_t)(month + 1u);
    info->day = (uint8_t)(days + 1u);
    info->hour = (uint8_t)(seconds / 3600u % 24u);
    info->minute = (uint8_t)(seconds / 60u % 60u);
    info->second = (uint8_t)(seconds % 60u);
    info->creation_valid = true;
}

static bool fs_file_info_unlocked(const char *path, struct book8_file_info *info)
{
    struct directory_entry entry;
    if (info == NULL) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    clear_memory(info, sizeof(*info));
    if (!resolve_entry_path(path, &entry))
        return false;
    info->size = entry.full_size;
    info->directory = (entry.attributes & FAT_ATTR_DIRECTORY) != 0u;
    if (mounted_volume.ntfs) {
        info->allocated_bytes = entry.ntfs_allocation;
        info->allocation_valid = !info->directory;
        info->read_only = (entry.ntfs_attributes & 1u) != 0u;
        info->system = (entry.ntfs_attributes & 4u) != 0u;
        ntfs_creation_time(entry.ntfs_creation, info);
        info->creation_utc = info->creation_valid;
    } else {
        info->allocation_valid = true;
        info->read_only = (entry.attributes & 1u) != 0u;
        info->system = (entry.attributes & 4u) != 0u;
        uint16_t date = entry.creation_date, time = entry.creation_time;
        info->year = (uint16_t)(1980u + (date >> 9u));
        info->month = (uint8_t)((date >> 5u) & 15u);
        info->day = (uint8_t)(date & 31u);
        info->hour = (uint8_t)(time >> 11u);
        info->minute = (uint8_t)((time >> 5u) & 63u);
        info->second = (uint8_t)((time & 31u) * 2u);
        info->creation_valid = info->month >= 1u && info->month <= 12u
            && info->day >= 1u && info->day <= 31u && info->hour < 24u
            && info->minute < 60u && info->second < 60u;
        uint32_t cluster = entry.first_cluster, steps = 0u;
        while (cluster != 0u) {
            uint32_t next;
            if (cluster < 2u || cluster >= cluster_count() + 2u
                || ++steps > cluster_count() || !fat_read_entry(cluster, &next)) {
                last_error = BOOK8_FS_ERR_IO;
                return false;
            }
            info->allocated_bytes += (uint64_t)mounted_volume.sectors_per_cluster * SECTOR_SIZE;
            if (is_end_of_chain(next))
                break;
            cluster = next;
            if (cluster == 0u) {
                last_error = BOOK8_FS_ERR_IO;
                return false;
            }
        }
    }
    last_error = BOOK8_FS_OK;
    return true;
}

static bool fs_list_directory_unlocked(const char *path, book8_directory_entry_fn visitor,
    void *context)
{
    last_error = BOOK8_FS_OK;
    if (visitor == NULL || path == NULL) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    char normalized[MAX_PATH];
    size_t length = 0u;
    while (length < sizeof(normalized) && path[length] != '\0') { ++length; }
    if (length == sizeof(normalized)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    bool implicit_root = length == 0u || path_separator(path[0]);
    if (implicit_root) {
        uint32_t drive = (uint32_t)(current_path[0] - 'A');
        if (!activate_drive(drive)) {
            bool mounted = false;
            for (uint32_t i = 0u; i < fs_drive_count(); ++i) {
                if (drives[i].readable && activate_drive(i)) {
                    drive = i; mounted = true; break;
                }
            }
            if (!mounted) { return false; }
            last_error = BOOK8_FS_OK;
            print("Directory root resolved to first readable filesystem volume.\n");
        }
        normalized[0] = (char)('A' + drive);
        normalized[1] = ':'; normalized[2] = '\\';
        size_t start = 0u;
        while (start < length && path_separator(path[start])) { ++start; }
        if (length - start + 3u >= sizeof(normalized)) {
            last_error = BOOK8_FS_ERR_BAD_PATH;
            return false;
        }
        for (size_t i = start; i <= length; ++i) { normalized[i - start + 3u] = path[i]; }
    } else if (length == 2u && path[1] == ':'
        && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
        normalized[0] = path[0]; normalized[1] = ':'; normalized[2] = '\\'; normalized[3] = '\0';
    } else {
        for (size_t i = 0u; i <= length; ++i) { normalized[i] = path[i]; }
    }
    path = normalized;
    if (!select_drive_for_path(path) || !mounted_volume.mounted) {
        if (last_error == BOOK8_FS_OK) {
            last_error = BOOK8_FS_ERR_NO_VOLUME;
        }
        return false;
    }
    if (mounted_volume.ntfs) {
        uint64_t directory_record;
        if (!resolve_ntfs_directory(path, &directory_record)) {
            last_error = BOOK8_FS_ERR_NOT_FOUND;
            return false;
        }
        bool ignored = false;
        if (!ntfs_index_visit(directory_record, public_directory_visitor,
            &(struct public_directory_context){visitor, context},
            NULL, NULL, &ignored)) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
        return true;
    }
    uint32_t cluster;
    bool fixed_root;
    if (!resolve_fat_directory(path, &cluster, &fixed_root)) {
        last_error = BOOK8_FS_ERR_NOT_FOUND;
        return false;
    }
    struct public_directory_context public_context = {visitor, context};
    if (!visit_directory(cluster, fixed_root,
        public_directory_visitor, &public_context)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    return true;
}

static bool fs_list_root_unlocked(book8_directory_entry_fn visitor, void *context)
{
    return fs_list_directory("A:\\", visitor, context);
}

static int fs_last_error_unlocked(void)
{
    return last_error;
}

static const char *fs_error_text_unlocked(int error)
{
    switch (error) {
    case BOOK8_FS_OK: return "OK";
    case BOOK8_FS_ERR_BAD_PATH: return "Invalid path.";
    case BOOK8_FS_ERR_NO_DRIVE: return "No such drive.";
    case BOOK8_FS_ERR_NO_VOLUME: return "Drive has no readable FAT/NTFS volume.";
    case BOOK8_FS_ERR_NO_PARTITION: return "Drive has no valid partition table.";
    case BOOK8_FS_ERR_NOT_FOUND: return "Directory not found.";
    case BOOK8_FS_ERR_IO: return "Disk I/O failed; changes may be incomplete.";
    case BOOK8_FS_ERR_UNSUPPORTED: return "Operation not supported for this item.";
    case BOOK8_FS_ERR_PROTECTED: return "Read-only or system item is protected.";
    case BOOK8_FS_ERR_EXISTS: return "Destination already exists.";
    case BOOK8_FS_ERR_NOT_EMPTY: return "Directory is not empty.";
    case BOOK8_FS_ERR_CUT_PARTIAL: return "Copy completed; source removal failed or is incomplete.";
    case BOOK8_FS_ERR_COPY_PARTIAL: return "Copy incomplete; destination may contain partial results.";
    default: return "Filesystem error.";
    }
}

static uint32_t fs_drive_count_unlocked(void)
{
    if (drives_dirty) {
        scan_drives();
    }
    return drive_total;
}

static char fs_drive_letter_unlocked(uint32_t index)
{
    return index < drive_total ? (char)('A' + index) : '\0';
}

static bool fs_drive_info_unlocked(uint32_t index, const char **type, bool *readable)
{
    if (index >= fs_drive_count()) {
        return false;
    }
    const struct drive_record *drive = &drives[index];
    if (type != NULL) {
        *type = !drive->readable ? "NOFS" : drive->ntfs ? "NTFS"
            : drive->fat32 ? "FAT32" : "FAT16";
    }
    if (readable != NULL) {
        *readable = drive->readable;
    }
    return true;
}

static bool fs_drive_space_unlocked(uint32_t index, struct book8_drive_space *space)
{
    if (index >= fs_drive_count() || space == NULL) {
        return false;
    }
    if (!space_samples[index].initialized) {
        const struct drive_record *drive = &drives[index];
        space_samples[index].space = (struct book8_drive_space){
            .total_bytes = drive->partition.sector_count * SECTOR_SIZE,
            .state = BOOK8_SPACE_UNAVAILABLE
        };
        if (drive->readable && !drive->ntfs) {
            struct volume saved = mounted_volume;
            if (!mount_drive_record(drive)) {
                space_samples[index].space.state = BOOK8_SPACE_ERROR;
            } else {
                uint32_t entries = mounted_volume.fat32 ? 4u : 2u;
                uint32_t clusters = cluster_count();
                space_samples[index].space.total_bytes = (uint64_t)clusters
                    * mounted_volume.sectors_per_cluster * SECTOR_SIZE;
                space_samples[index].space.state =
                    (uint64_t)clusters + 2u <=
                        (uint64_t)mounted_volume.sectors_per_fat * SECTOR_SIZE / entries
                    ? BOOK8_SPACE_PENDING : BOOK8_SPACE_ERROR;
                space_samples[index].fat_lba = mounted_volume.start_lba
                    + mounted_volume.reserved_sectors;
                space_samples[index].clusters = clusters;
                space_samples[index].next_cluster = 2u;
                space_samples[index].cluster_bytes =
                    mounted_volume.sectors_per_cluster * SECTOR_SIZE;
                space_samples[index].entry_bytes = entries;
            }
            mounted_volume = saved;
        }
        space_samples[index].initialized = true;
    }
    space_samples[index].space.mounted = active_drive == (int32_t)index
        && mounted_volume.mounted;
    *space = space_samples[index].space;
    return true;
}

static void fs_space_poll_unlocked(void)
{
    for (uint32_t i = 0u; i < drive_total; ++i) {
        if (!space_samples[i].initialized
            || space_samples[i].space.state != BOOK8_SPACE_PENDING) {
            continue;
        }
        uint32_t bytes = space_samples[i].entry_bytes;
        uint32_t cluster = space_samples[i].next_cluster;
        uint64_t sector = space_samples[i].fat_lba
            + (uint64_t)cluster * bytes / SECTOR_SIZE;
        uint8_t sample[SECTOR_SIZE];
        if (!read_sector_callback(drives[i].disk, sector, sample)) {
            space_samples[i].space.state = BOOK8_SPACE_ERROR;
            print("SPACE: FAT accounting read failed.\n");
            return;
        }
        uint32_t offset = cluster * bytes % SECTOR_SIZE;
        for (; offset < SECTOR_SIZE && cluster < space_samples[i].clusters + 2u;
            offset += bytes, ++cluster) {
            uint32_t value = bytes == 4u
                ? read_u32(sample + offset) & 0x0fffffffu : read_u16(sample + offset);
            if (value == 0u) {
                space_samples[i].space.free_bytes += space_samples[i].cluster_bytes;
            }
        }
        space_samples[i].next_cluster = cluster;
        if (cluster == space_samples[i].clusters + 2u) {
            space_samples[i].space.state = BOOK8_SPACE_READY;
        }
        return;
    }
}

static bool read_fat_file_entry(const struct directory_entry *found,
    uint8_t *destination, uint32_t capacity, uint32_t *file_size)
{
    struct directory_entry entry = *found;
    if ((entry.attributes & FAT_ATTR_DIRECTORY) != 0u
        || entry.size > capacity) {
        return false;
    }
    *file_size = entry.size;
    uint32_t remaining = entry.size;
    uint32_t cluster = entry.first_cluster;
    uint32_t steps = 0u;
    while (remaining != 0u) {
        if (cluster < 2u || cluster >= cluster_count() + 2u
            || ++steps > cluster_count()) {
            return false;
        }
        uint64_t first_sector = cluster_sector(cluster);
        for (uint32_t i = 0u; i < mounted_volume.sectors_per_cluster
            && remaining != 0u; ++i) {
            if (!volume_read(first_sector + i, io_sector)) {
                return false;
            }
            uint32_t amount = remaining > SECTOR_SIZE ? SECTOR_SIZE : remaining;
            uint32_t offset = entry.size - remaining;
            for (uint32_t j = 0u; j < amount; ++j) {
                destination[offset + j] = io_sector[j];
            }
            remaining -= amount;
        }
        if (remaining != 0u) {
            uint32_t next;
            if (!fat_read_entry(cluster, &next) || is_end_of_chain(next)) {
                return false;
            }
            cluster = next;
        }
    }
    return true;
}

static bool read_file_root(const char *name, uint8_t *destination,
    uint32_t capacity, uint32_t *file_size)
{
    if (mounted_volume.ntfs)
        return ntfs_read_file_root(name, destination, capacity, file_size);
    struct directory_entry entry;
    return find_in_directory(root_cluster(), root_is_fixed(), name, &entry)
        && read_fat_file_entry(&entry, destination, capacity, file_size);
}

static bool make_short_filename(const char *name, uint8_t short_name[11])
{
    uint32_t base = 0u;
    uint32_t extension = 0u;
    bool after_dot = false;
    for (uint32_t i = 0u; name[i] != '\0'; ++i) {
        char value = name[i];
        if (value == '.') {
            if (after_dot || base == 0u) {
                return false;
            }
            after_dot = true;
            continue;
        }
        if (value < 0x21 || value > 0x7e || value == '"' || value == '*'
            || value == '+' || value == ',' || value == '/' || value == ':'
            || value == ';' || value == '<' || value == '=' || value == '>'
            || value == '?' || value == '[' || value == '\\' || value == ']'
            || value == '|') {
            return false;
        }
        if (value >= 'a' && value <= 'z') {
            value = (char)(value - 'a' + 'A');
        }
        if (after_dot) {
            if (++extension > 3u) {
                return false;
            }
            short_name[7u + extension] = (uint8_t)value;
        } else {
            if (++base > 8u) {
                return false;
            }
            short_name[base - 1u] = (uint8_t)value;
        }
    }
    return base != 0u && (!after_dot || extension != 0u);
}

struct free_slot_context {
    bool found;
    uint32_t count;
    struct {
        uint64_t sector;
        uint16_t offset;
    } slots[21];
};

static uint32_t allocate_cluster(void);

static bool find_free_directory_slots(uint32_t first_cluster, bool fixed_root,
    uint32_t required,
    struct free_slot_context *free_slot)
{
    bool end_of_directory = false;
    if (fixed_root) {
        for (uint32_t s = 0u; s < mounted_volume.root_directory_sectors; ++s) {
            if (!volume_read(mounted_volume.root_directory_sector + s, io_sector)) {
                return false;
            }
            for (uint32_t offset = 0u; offset < SECTOR_SIZE; offset += 32u) {
                bool free_entry = end_of_directory
                    || io_sector[offset] == 0u || io_sector[offset] == 0xe5u;
                end_of_directory = end_of_directory || io_sector[offset] == 0u;
                if (free_entry) {
                    if (free_slot->count < required) {
                        free_slot->slots[free_slot->count].sector =
                            mounted_volume.root_directory_sector + s;
                        free_slot->slots[free_slot->count].offset = (uint16_t)offset;
                        ++free_slot->count;
                    }
                    if (free_slot->count == required) {
                        free_slot->found = true;
                        return true;
                    }
                } else {
                    free_slot->count = 0u;
                }
            }
        }
        return false;
    }

    uint32_t cluster = first_cluster;
    for (uint32_t steps = 0u; steps < cluster_count(); ++steps) {
        if (cluster < 2u || cluster >= cluster_count() + 2u)
            return false;
        uint64_t base = cluster_sector(cluster);
        for (uint32_t s = 0u; s < mounted_volume.sectors_per_cluster; ++s) {
            if (!volume_read(base + s, io_sector)) {
                return false;
            }
            for (uint32_t offset = 0u; offset < SECTOR_SIZE; offset += 32u) {
                bool free_entry = end_of_directory
                    || io_sector[offset] == 0u || io_sector[offset] == 0xe5u;
                end_of_directory = end_of_directory || io_sector[offset] == 0u;
                if (free_entry) {
                    if (free_slot->count < required) {
                        free_slot->slots[free_slot->count].sector = base + s;
                        free_slot->slots[free_slot->count].offset = (uint16_t)offset;
                        ++free_slot->count;
                    }
                    if (free_slot->count == required) {
                        free_slot->found = true;
                        return true;
                    }
                } else {
                    free_slot->count = 0u;
                }
            }
        }
        uint32_t next;
        if (!fat_read_entry(cluster, &next)) {
            return false;
        }
        if (is_end_of_chain(next)) {
            next = allocate_cluster();
            if (next == 0u || !fat_write_entry(cluster, next)) {
                print("Directory extension failed; allocation state may be incomplete.\n");
                return false;
            }
        }
        cluster = next;
    }
    return false;
}

static uint32_t allocate_cluster(void)
{
    uint32_t count = cluster_count();
    uint32_t end_marker = mounted_volume.fat32 ? 0x0fffffffu : 0x0000ffffu;
    uint8_t empty[SECTOR_SIZE];
    clear_memory(empty, sizeof(empty));
    uint32_t start = mounted_volume.next_free_cluster;
    if (start < 2u || start >= count + 2u) {
        start = 2u;
    }
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        uint32_t first = pass == 0u ? start : 2u;
        uint32_t limit = pass == 0u ? count + 2u : start;
        for (uint32_t cluster = first; cluster < limit; ++cluster) {
            uint32_t value;
            if (!fat_read_entry(cluster, &value)) {
                return 0u;
            }
            if (value != 0u) {
                continue;
            }
            if (!fat_write_entry(cluster, end_marker)) {
                return 0u;
            }
            uint64_t first_sector = cluster_sector(cluster);
            for (uint32_t i = 0u;
                i < mounted_volume.sectors_per_cluster; ++i) {
                if (!volume_write(first_sector + i, empty)) {
                    (void)fat_write_entry(cluster, 0u);
                    mounted_volume.next_free_cluster = cluster;
                    return 0u;
                }
            }
            mounted_volume.next_free_cluster = cluster + 1u;
            if (mounted_volume.next_free_cluster >= count + 2u) {
                mounted_volume.next_free_cluster = 2u;
            }
            return cluster;
        }
    }
    return 0u;
}

static bool free_cluster_chain(uint32_t cluster)
{
    for (uint32_t steps = 0u; cluster >= 2u && cluster < cluster_count() + 2u
        && steps < cluster_count(); ++steps) {
        uint32_t next;
        if (!fat_read_entry(cluster, &next) || !fat_write_entry(cluster, 0u)) {
            return false;
        }
        if (mounted_volume.next_free_cluster < 2u
            || mounted_volume.next_free_cluster >= cluster_count() + 2u
            || cluster < mounted_volume.next_free_cluster) {
            mounted_volume.next_free_cluster = cluster;
        }
        if (is_end_of_chain(next)) {
            return true;
        }
        cluster = next;
    }
    return cluster == 0u;
}

static bool directory_empty_visitor(const struct directory_entry *entry, void *context)
{
    bool *empty = context;
    if (!equal_case_insensitive(entry->name, ".")
        && !equal_case_insensitive(entry->name, "..")) {
        *empty = false;
        return false;
    }
    return true;
}

struct name_collision {
    const uint8_t *short_name;
    const char *name;
    bool found;
    struct directory_entry entry;
};

static bool collision_visitor(const struct directory_entry *entry, void *context)
{
    struct name_collision *collision = context;
    bool same = true;
    for (unsigned i = 0u; i < 11u; ++i)
        if (entry->raw_name[i] != collision->short_name[i])
            same = false;
    if (same || name_matches(entry->name, collision->name)) {
        collision->found = true;
        collision->entry = *entry;
        return false;
    }
    return true;
}

static bool writable_entry(const char *path, struct directory_entry *entry)
{
    if (!resolve_entry_path(path, entry))
        return false;
    if (mounted_volume.ntfs || (entry->long_name && !entry->long_name_safe)) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    if ((entry->attributes & 5u) != 0u) {
        last_error = BOOK8_FS_ERR_PROTECTED;
        return false;
    }
    return true;
}

static bool remove_long_name_slots(const struct directory_entry *entry)
{
    for (uint32_t i = 0u; i < entry->long_name_count; ++i) {
        const struct fat_slot *slot = &entry->long_name_slots[i];
        if (!volume_read(slot->sector, io_sector))
            return false;
        io_sector[slot->offset] = 0xe5u;
        if (!volume_write(slot->sector, io_sector))
            return false;
    }
    return true;
}

static bool make_fat_name(const char *name, uint8_t raw_name[11],
    uint32_t *lfn_entries);
static bool choose_fat_name(const char *name, uint32_t directory_cluster,
    bool fixed_root, uint8_t raw_name[11], uint32_t *lfn_entries);
static void format_long_name_slot(uint8_t entry[32], const char *name,
    size_t name_length, uint32_t ordinal, bool first, uint8_t checksum);

static bool fs_rename_entry_unlocked(const char *path, const char *new_name)
{
    struct directory_entry entry;
    uint8_t short_name[11];
    uint32_t lfn_entries;
    char directory[MAX_PATH];
    if (path == NULL || new_name == NULL) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    if (!make_fat_name(new_name, short_name, &lfn_entries)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    if (!writable_entry(path, &entry))
        return false;
    size_t split = 0u;
    for (size_t i = 0u; path[i] != '\0'; ++i)
        if (path_separator(path[i]))
            split = i;
    if (split + 1u >= sizeof(directory)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    for (size_t i = 0u; i <= split; ++i)
        directory[i] = path[i];
    directory[split + 1u] = '\0';
    uint32_t cluster;
    bool fixed;
    if (!resolve_fat_directory(directory, &cluster, &fixed)) {
        last_error = BOOK8_FS_ERR_NOT_FOUND;
        return false;
    }
    if (lfn_entries != 0u) {
        if (name_matches(entry.name, new_name)) {
            last_error = BOOK8_FS_OK;
            return true;
        }
        if (!choose_fat_name(new_name, cluster, fixed, short_name, &lfn_entries))
            return false;
        if (!volume_read(entry.sector, io_sector)) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
        uint32_t first = 0u, count = 0u;
        bool past_end = false, consumes_end = false, found = false;
        for (uint32_t offset = 0u; offset < SECTOR_SIZE; offset += 32u) {
            bool is_end = io_sector[offset] == 0u;
            past_end = past_end || is_end;
            if (past_end || io_sector[offset] == 0xe5u) {
                if (count == 0u) {
                    first = offset;
                    consumes_end = false;
                }
                consumes_end = consumes_end || is_end;
                ++count;
                if (count == lfn_entries + 1u
                    && (!consumes_end || offset + 32u < SECTOR_SIZE)) {
                    found = true;
                    break;
                }
            } else {
                count = 0u;
            }
        }
        if (!found) {
            last_error = BOOK8_FS_ERR_UNSUPPORTED;
            return false;
        }
        for (uint32_t i = 0u; i < entry.long_name_count; ++i) {
            if (entry.long_name_slots[i].sector != entry.sector) {
                last_error = BOOK8_FS_ERR_UNSUPPORTED;
                return false;
            }
        }
        uint8_t original[32];
        for (uint32_t i = 0u; i < 32u; ++i)
            original[i] = io_sector[entry.offset + i];
        size_t length = 0u;
        while (new_name[length] != '\0')
            ++length;
        for (uint32_t i = 0u; i < lfn_entries; ++i)
            format_long_name_slot(io_sector + first + i * 32u,
                new_name, length, lfn_entries - i, i == 0u,
                short_name_checksum(short_name));
        uint8_t *raw = io_sector + first + lfn_entries * 32u;
        for (uint32_t i = 0u; i < 32u; ++i)
            raw[i] = original[i];
        for (uint32_t i = 0u; i < 11u; ++i)
            raw[i] = short_name[i];
        raw[12] = 0u;
        io_sector[entry.offset] = 0xe5u;
        for (uint32_t i = 0u; i < entry.long_name_count; ++i)
            io_sector[entry.long_name_slots[i].offset] = 0xe5u;
        if (consumes_end)
            io_sector[first + (lfn_entries + 1u) * 32u] = 0u;
        if (!volume_write(entry.sector, io_sector)) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
        last_error = BOOK8_FS_OK;
        return true;
    }
    struct name_collision collision = {
        .short_name = short_name, .name = new_name, .found = false
    };
    if (!visit_directory(cluster, fixed, collision_visitor, &collision)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    if (collision.found) {
        if (collision.entry.sector == entry.sector
            && collision.entry.offset == entry.offset && !entry.long_name) {
            last_error = BOOK8_FS_OK;
            return true;
        }
        if (collision.entry.sector != entry.sector
            || collision.entry.offset != entry.offset) {
            last_error = BOOK8_FS_ERR_EXISTS;
            return false;
        }
    }
    if (!volume_read(entry.sector, io_sector)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    for (unsigned i = 0u; i < sizeof(short_name); ++i)
        io_sector[entry.offset + i] = short_name[i];
    io_sector[entry.offset + 12u] = 0u;
    if (!volume_write(entry.sector, io_sector)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    if (!remove_long_name_slots(&entry)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    last_error = BOOK8_FS_OK;
    return true;
}

static bool delete_single_entry(const char *path)
{
    struct directory_entry entry;
    struct book8_file_info info;
    if (!writable_entry(path, &entry))
        return false;
    if (!fs_file_info(path, &info))
        return false;
    if (!info.directory && info.size > info.allocated_bytes) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    if (info.directory) {
        bool empty = true;
        if (entry.first_cluster < 2u
            || !visit_directory(entry.first_cluster, false,
                directory_empty_visitor, &empty)) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
        if (!empty) {
            last_error = BOOK8_FS_ERR_NOT_EMPTY;
            return false;
        }
    }
    if (!volume_read(entry.sector, io_sector)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    io_sector[entry.offset] = 0xe5u;
    if (!volume_write(entry.sector, io_sector)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    if (!remove_long_name_slots(&entry)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    if (entry.first_cluster != 0u && !free_cluster_chain(entry.first_cluster)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    invalidate_space_samples();
    last_error = BOOK8_FS_OK;
    return true;
}

static bool verify_file_chain(uint32_t cluster, const uint8_t *data, uint32_t size)
{
    uint32_t offset = 0u;
    for (uint32_t steps = 0u; steps < cluster_count() && offset < size; ++steps) {
        if (cluster < 2u || cluster >= cluster_count() + 2u)
            return false;
        uint64_t base = cluster_sector(cluster);
        for (uint32_t s = 0u; s < mounted_volume.sectors_per_cluster && offset < size; ++s) {
            if (!volume_read(base + s, io_sector))
                return false;
            uint32_t count = size - offset < SECTOR_SIZE ? size - offset : SECTOR_SIZE;
            for (uint32_t i = 0u; i < count; ++i) {
                if (io_sector[i] != data[offset + i])
                    return false;
            }
            offset += count;
        }
        uint32_t next;
        if (!fat_read_entry(cluster, &next))
            return false;
        if (offset == size)
            return is_end_of_chain(next);
        cluster = next;
    }
    return false;
}

static bool replace_fat_entry(const struct directory_entry *old_entry,
    const uint8_t *data, uint32_t size)
{
    struct directory_entry old = *old_entry;
    uint8_t original[SECTOR_SIZE], committed[SECTOR_SIZE];
    if (!volume_read(old.sector, original))
        return false;
    uint32_t first = 0u, previous = 0u, offset = 0u;
    while (offset < size) {
        uint32_t cluster = allocate_cluster();
        if (cluster == 0u)
            goto candidate_failed;
        if (first == 0u)
            first = cluster;
        if (previous != 0u && !fat_write_entry(previous, cluster)) {
            if (!free_cluster_chain(cluster))
                print("FAT save: could not reclaim unlinked candidate cluster.\n");
            goto candidate_failed;
        }
        previous = cluster;
        uint64_t base = cluster_sector(cluster);
        for (uint32_t s = 0u; s < mounted_volume.sectors_per_cluster && offset < size; ++s) {
            clear_memory(io_sector, sizeof(io_sector));
            uint32_t count = size - offset < SECTOR_SIZE ? size - offset : SECTOR_SIZE;
            for (uint32_t i = 0u; i < count; ++i)
                io_sector[i] = data[offset + i];
            if (!volume_write(base + s, io_sector))
                goto candidate_failed;
            offset += count;
        }
    }
    if (size != 0u && !verify_file_chain(first, data, size))
        goto candidate_failed;
    /* Recheck the directory sector before the single-sector commit. */
    if (!volume_read(old.sector, committed))
        goto candidate_failed;
    for (uint32_t i = 0u; i < SECTOR_SIZE; ++i) {
        if (committed[i] != original[i])
            goto candidate_failed;
    }
    uint8_t *entry = committed + old.offset;
    write_u16(entry + 20u, (uint16_t)(first >> 16u));
    write_u16(entry + 26u, (uint16_t)first);
    write_u32(entry + 28u, size);
    /* Writes use the existing flushed sector callback. After an attempted
     * commit, retain both chains on uncertainty; never free possibly live data. */
    if (!volume_write(old.sector, committed) || !volume_read(old.sector, io_sector)) {
        print("FAT save: directory commit indeterminate; both chains retained.\n");
        return false;
    }
    for (uint32_t i = 0u; i < SECTOR_SIZE; ++i) {
        if (io_sector[i] != committed[i]) {
            print("FAT save: directory readback mismatch; both chains retained.\n");
            return false;
        }
    }
    if (size != 0u && !verify_file_chain(first, data, size)) {
        print("FAT save: committed data readback failed; old chain retained.\n");
        return false;
    }
    if (old.first_cluster >= 2u && !free_cluster_chain(old.first_cluster)) {
        print("FAT save: data committed but old chain cleanup failed.\n");
        return false;
    }
    invalidate_space_samples();
    last_error = BOOK8_FS_OK;
    return true;

candidate_failed:
    if (first != 0u && !free_cluster_chain(first))
        print("FAT save: candidate cleanup failed; check free space.\n");
    print("FAT save: candidate failed; original file is unchanged.\n");
    return false;
}

static bool fs_replace_boot_kernel_unlocked(const void *buffer, uint32_t size)
{
    uint32_t directory;
    bool fixed;
    struct directory_entry old;
    last_error = BOOK8_FS_ERR_IO;
    if (buffer == NULL || size == 0u || size > MAX_OPEN_FILE_SIZE
        || !select_drive_for_path("A:\\") || mounted_volume.ntfs
        || !resolve_fat_directory("A:\\boot", &directory, &fixed)
        || !find_in_directory(directory, fixed, "book8.elf", &old)
        || (old.attributes & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME | 1u)) != 0u
        || old.first_cluster < 2u || old.size == 0u) {
        print("BOOTUPDATE: existing writable FAT A:\\boot\\book8.elf required (max 1 MiB).\n");
        return false;
    }
    return replace_fat_entry(&old, buffer, size);
}

static bool make_fat_name(const char *name, uint8_t raw_name[11],
    uint32_t *lfn_entries)
{
    for (uint32_t i = 0u; i < 11u; ++i) {
        raw_name[i] = ' ';
    }
    size_t name_length = 0u;
    while (name[name_length] != '\0' && name_length < MAX_NAME - 1u) {
        char ch = name[name_length];
        if ((uint8_t)ch > 0x7eu || (uint8_t)ch < 0x20u
            || ch == '"' || ch == '*' || ch == '/' || ch == ':'
            || ch == '<' || ch == '>' || ch == '?' || ch == '\\' || ch == '|') {
            print("Creating long names currently supports printable ASCII only.\n");
            return false;
        }
        ++name_length;
    }
    if (name[name_length] != '\0' || name_length == 0u) {
        print("File name is too long or empty.\n");
        return false;
    }
    if (name[name_length - 1u] == '.' || name[name_length - 1u] == ' ') {
        return false;
    }
    *lfn_entries = (uint32_t)((name_length + 12u) / 13u);
    if (*lfn_entries > 20u) {
        print("VFAT names are limited to 255 characters.\n");
        return false;
    }
    bool has_short_name = make_short_filename(name, raw_name);
    if (!has_short_name) {
        for (uint32_t i = 0u; i < 11u; ++i) {
            raw_name[i] = ' ';
        }
        uint32_t base_index = 0u;
        for (size_t i = 0u; i < name_length && name[i] != '.' && base_index < 6u; ++i) {
            char value = name[i];
            if (value == ' ' || value == '.' || value == '+' || value == ','
                || value == ';' || value == '=' || value == '[' || value == ']') {
                continue;
            }
            if (value >= 'a' && value <= 'z') {
                value = (char)(value - 'a' + 'A');
            }
            raw_name[base_index++] = (uint8_t)value;
        }
        if (base_index == 0u) {
            raw_name[0] = 'O';
            raw_name[1] = 'S';
            base_index = 2u;
        }
        raw_name[base_index++] = '~';
        raw_name[base_index] = '1';
        const char *dot = NULL;
        for (size_t i = 0u; i < name_length; ++i) {
            if (name[i] == '.') {
                dot = name + i;
            }
        }
        if (dot != NULL) {
            for (uint32_t i = 0u; i < 3u && dot[i + 1u] != '\0'; ++i) {
                char value = dot[i + 1u];
                if (value == ' ' || value == '.' || value == '+' || value == ','
                    || value == ';' || value == '=' || value == '[' || value == ']')
                    break;
                raw_name[8u + i] = (uint8_t)(value >= 'a' && value <= 'z'
                    ? value - 'a' + 'A' : value);
            }
        }
    } else {
        *lfn_entries = 0u;
    }
    return true;
}

static bool choose_fat_name(const char *name, uint32_t directory_cluster,
    bool fixed_root, uint8_t raw_name[11], uint32_t *lfn_entries)
{
    if (!make_fat_name(name, raw_name, lfn_entries)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    uint8_t base[11];
    for (uint32_t i = 0u; i < 11u; ++i)
        base[i] = raw_name[i];
    for (uint32_t number = 1u; number <= 999999u; ++number) {
        if (*lfn_entries != 0u) {
            char digits[6];
            uint32_t n = number, count = 0u;
            do {
                digits[count++] = (char)('0' + n % 10u);
                n /= 10u;
            } while (n != 0u);
            uint32_t prefix = 0u;
            while (prefix < 8u - count - 1u
                && base[prefix] != '~' && base[prefix] != ' ')
                ++prefix;
            for (uint32_t i = 0u; i < 8u; ++i)
                raw_name[i] = ' ';
            for (uint32_t i = 0u; i < prefix; ++i)
                raw_name[i] = base[i];
            raw_name[prefix] = '~';
            for (uint32_t i = 0u; i < count; ++i)
                raw_name[prefix + 1u + i] = (uint8_t)digits[count - 1u - i];
        }
        struct name_collision collision = {
            .short_name = raw_name, .name = name, .found = false
        };
        if (!visit_directory(directory_cluster, fixed_root, collision_visitor,
                &collision)) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
        if (!collision.found)
            return true;
        if (*lfn_entries == 0u || name_matches(collision.entry.name, name)) {
            last_error = BOOK8_FS_ERR_EXISTS;
            return false;
        }
    }
    last_error = BOOK8_FS_ERR_EXISTS;
    return false;
}

static void format_long_name_slot(uint8_t entry[32], const char *name,
    size_t name_length, uint32_t ordinal, bool first, uint8_t checksum)
{
    static const uint8_t offsets[13] = {
        1,3,5,7,9,14,16,18,20,22,24,28,30
    };
    for (uint32_t i = 0u; i < 32u; ++i)
        entry[i] = 0xffu;
    entry[0] = (uint8_t)(ordinal | (first ? 0x40u : 0u));
    entry[11] = FAT_ATTR_LONG_NAME;
    entry[12] = 0u;
    entry[13] = checksum;
    write_u16(entry + 26u, 0u);
    for (uint32_t character = 0u; character < 13u; ++character) {
        uint32_t index = (ordinal - 1u) * 13u + character;
        uint16_t value = index < name_length
            ? (uint8_t)name[index] : (index == name_length ? 0u : 0xffffu);
        write_u16(entry + offsets[character], value);
    }
}

static bool write_long_name_slots(const char *name, const uint8_t raw_name[11],
    uint32_t count, const struct free_slot_context *slots)
{
    size_t name_length = 0u;
    while (name[name_length] != '\0')
        ++name_length;
    uint8_t checksum = short_name_checksum(raw_name);
    for (uint32_t slot = 0u; slot < count; ++slot) {
        uint8_t entry[32];
        format_long_name_slot(entry, name, name_length, count - slot,
            slot == 0u, checksum);
        if (!volume_read(slots->slots[slot].sector, io_sector))
            return false;
        for (uint32_t i = 0u; i < sizeof(entry); ++i)
            io_sector[slots->slots[slot].offset + i] = entry[i];
        if (!volume_write(slots->slots[slot].sector, io_sector))
            return false;
    }
    return true;
}

static bool write_file_directory(const char *name, const uint8_t *data,
    uint32_t size, uint32_t directory_cluster, bool fixed_root)
{
    uint8_t raw_name[11];
    uint32_t lfn_entries;
    if (mounted_volume.ntfs || size > MAX_OPEN_FILE_SIZE
        || !choose_fat_name(name, directory_cluster, fixed_root, raw_name,
            &lfn_entries)) {
        print("File name or size is not supported.\n");
        return false;
    }

    struct free_slot_context free_slot = {0};
    uint32_t required_slots = lfn_entries + 1u;
    if (!find_free_directory_slots(directory_cluster, fixed_root, required_slots, &free_slot)) {
        print("Directory is full or could not be read.\n");
        return false;
    }

    uint32_t cluster_bytes =
        mounted_volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t required_clusters = size == 0u ? 0u
        : (size + cluster_bytes - 1u) / cluster_bytes;
    uint32_t first_cluster = 0u;
    uint32_t last_cluster = 0u;
    for (uint32_t i = 0u; i < required_clusters; ++i) {
        uint32_t cluster = allocate_cluster();
        if (cluster == 0u) {
            (void)free_cluster_chain(first_cluster);
            print("Not enough free clusters to create the file.\n");
            return false;
        }
        if (first_cluster == 0u) {
            first_cluster = cluster;
        }
        if (last_cluster != 0u && !fat_write_entry(last_cluster, cluster)) {
            print("FAT link failed; allocation state may be incomplete.\n");
            return false;
        }
        last_cluster = cluster;

        uint64_t sector_start = cluster_sector(cluster);
        for (uint32_t sector_index = 0u;
            sector_index < mounted_volume.sectors_per_cluster; ++sector_index) {
            clear_memory(io_sector, sizeof(io_sector));
            uint32_t offset = i * cluster_bytes + sector_index * SECTOR_SIZE;
            for (uint32_t j = 0u; j < SECTOR_SIZE && offset + j < size; ++j) {
                io_sector[j] = data[offset + j];
            }
            if (!volume_write(sector_start + sector_index, io_sector)) {
                (void)free_cluster_chain(first_cluster);
                print("Failed writing file data; directory was not updated.\n");
                return false;
            }
        }
    }

    if (!write_long_name_slots(name, raw_name, lfn_entries, &free_slot)) {
        (void)free_cluster_chain(first_cluster);
        return false;
    }

    uint32_t short_slot = lfn_entries;
    uint64_t short_sector = free_slot.slots[short_slot].sector;
    if (!volume_read(short_sector, io_sector)) {
        (void)free_cluster_chain(first_cluster);
        return false;
    }
    uint8_t *raw = io_sector + free_slot.slots[short_slot].offset;
    clear_memory(raw, 32u);
    for (uint32_t i = 0u; i < 11u; ++i) {
        raw[i] = raw_name[i];
    }
    raw[11] = 0x20u;
    write_u16(raw + 20u, (uint16_t)(first_cluster >> 16u));
    write_u16(raw + 26u, (uint16_t)first_cluster);
    write_u32(raw + 28u, size);
    if (!volume_write(short_sector, io_sector)) {
        print("Directory write failed; allocation state may be incomplete.\n");
        return false;
    }
    print("File written.\n");
    return true;
}

static bool write_file_root(const char *name, const uint8_t *data, uint32_t size)
{
    if (mounted_volume.ntfs)
        return ntfs_write_existing_file(name, data, size);
    return write_file_directory(name, data, size, root_cluster(), root_is_fixed());
}

#define COPY_TREE_LIMIT 128u
struct copy_tree_item {
    char source[MAX_PATH];
    char parent[MAX_PATH];
    uint32_t cluster;
    bool directory;
};
static struct copy_tree_item copy_tree[COPY_TREE_LIMIT];
static uint32_t copy_tree_count;

static bool append_path(char out[MAX_PATH], const char *parent, const char *name)
{
    size_t i = 0u, n = 0u;
    while (parent[i] != '\0' && i + 1u < MAX_PATH) {
        out[i] = parent[i];
        ++i;
    }
    if (parent[i] != '\0')
        return false;
    if (i != 0u && !path_separator(out[i - 1u])) {
        if (i + 1u >= MAX_PATH)
            return false;
        out[i++] = '\\';
    }
    while (name[n] != '\0' && i + 1u < MAX_PATH)
        out[i++] = name[n++];
    out[i] = '\0';
    return name[n] == '\0';
}

static const char *path_basename(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p != '\0'; ++p)
        if (path_separator(*p))
            name = p + 1u;
    return name;
}

struct tree_collect_context {
    const struct copy_tree_item *parent;
    bool cut, deleting;
    int error;
};

static bool collect_tree_entry(const struct directory_entry *entry, void *context)
{
    struct tree_collect_context *c = context;
    uint8_t short_name[11];
    if (equal_case_insensitive(entry->name, ".")
        || equal_case_insensitive(entry->name, ".."))
        return true;
    for (unsigned i = 0u; i < sizeof(short_name); ++i)
        short_name[i] = ' ';
    if (c->cut && (entry->attributes & 5u) != 0u) {
        c->error = BOOK8_FS_ERR_PROTECTED;
        return false;
    }
    if (copy_tree_count == COPY_TREE_LIMIT
        || (entry->long_name && !entry->long_name_safe)
        || (!entry->long_name && !make_short_filename(entry->name, short_name))
        || (!c->deleting && (entry->attributes & FAT_ATTR_DIRECTORY) == 0u
            && entry->size > MAX_OPEN_FILE_SIZE)) {
        c->error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    struct copy_tree_item *child = &copy_tree[copy_tree_count];
    char target[MAX_PATH];
    if (!append_path(child->source, c->parent->source, entry->name)
        || !append_path(target, c->parent->parent,
            path_basename(c->parent->source))
        || !append_path(child->parent, target, "")) {
        c->error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    child->directory = (entry->attributes & FAT_ATTR_DIRECTORY) != 0u;
    child->cluster = entry->first_cluster;
    if (child->directory) {
        if (child->cluster < 2u || child->cluster >= cluster_count() + 2u) {
            c->error = BOOK8_FS_ERR_IO;
            return false;
        }
        for (uint32_t i = 0u; i < copy_tree_count; ++i)
            if (copy_tree[i].directory && copy_tree[i].cluster == child->cluster) {
                c->error = BOOK8_FS_ERR_IO;
                return false;
            }
    }
    ++copy_tree_count;
    return true;
}

static bool fs_delete_entry_unlocked(const char *path)
{
    struct directory_entry root;
    if (!writable_entry(path, &root))
        return false;
    if ((root.attributes & FAT_ATTR_DIRECTORY) == 0u)
        return delete_single_entry(path);
    if (root.first_cluster < 2u || root.first_cluster >= cluster_count() + 2u
        || !append_path(copy_tree[0].source, "", path)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    copy_tree_count = 1u;
    copy_tree[0].parent[0] = '\0';
    copy_tree[0].directory = true;
    copy_tree[0].cluster = root.first_cluster;
    for (uint32_t i = 0u; i < copy_tree_count; ++i) {
        struct book8_file_info info;
        if (!fs_file_info(copy_tree[i].source, &info))
            return false;
        if (info.read_only || info.system) {
            last_error = BOOK8_FS_ERR_PROTECTED;
            return false;
        }
        if (!info.directory && info.size > info.allocated_bytes) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
        if (copy_tree[i].directory) {
            struct tree_collect_context c = {
                &copy_tree[i], true, true, BOOK8_FS_OK
            };
            if (!visit_directory(copy_tree[i].cluster, false, collect_tree_entry, &c)) {
                last_error = BOOK8_FS_ERR_IO;
                return false;
            }
            if (c.error != BOOK8_FS_OK) {
                last_error = c.error;
                return false;
            }
        }
    }
    for (uint32_t i = copy_tree_count; i != 0u; --i) {
        if (!delete_single_entry(copy_tree[i - 1u].source))
            return false;
    }
    return true;
}

static bool create_copy_directory(const char *parent, const char *name)
{
    uint8_t short_name[11];
    uint32_t lfn_entries;
    uint32_t parent_cluster;
    bool fixed;
    if (!select_drive_for_path(parent) || mounted_volume.ntfs
        || !resolve_fat_directory(parent, &parent_cluster, &fixed)) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    if (!choose_fat_name(name, parent_cluster, fixed, short_name, &lfn_entries))
        return false;
    struct free_slot_context slot = {0};
    if (!find_free_directory_slots(parent_cluster, fixed, lfn_entries + 1u, &slot)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    uint32_t cluster = allocate_cluster();
    if (cluster == 0u) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    uint8_t sector[SECTOR_SIZE] = {0};
    for (uint32_t i = 0u; i < 11u; ++i)
        sector[i] = sector[32u + i] = ' ';
    sector[0] = '.';
    sector[32] = sector[33] = '.';
    sector[11] = sector[43] = FAT_ATTR_DIRECTORY;
    write_u16(sector + 20u, (uint16_t)(cluster >> 16u));
    write_u16(sector + 26u, (uint16_t)cluster);
    uint32_t parent_ref = fixed || parent_cluster == root_cluster()
        ? 0u : parent_cluster;
    write_u16(sector + 52u, (uint16_t)(parent_ref >> 16u));
    write_u16(sector + 58u, (uint16_t)parent_ref);
    if (!volume_write(cluster_sector(cluster), sector)
        || !write_long_name_slots(name, short_name, lfn_entries, &slot)
        || !volume_read(slot.slots[lfn_entries].sector, io_sector)) {
        (void)free_cluster_chain(cluster);
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    uint8_t *raw = io_sector + slot.slots[lfn_entries].offset;
    clear_memory(raw, 32u);
    for (unsigned i = 0u; i < sizeof(short_name); ++i)
        raw[i] = short_name[i];
    raw[11] = FAT_ATTR_DIRECTORY;
    write_u16(raw + 20u, (uint16_t)(cluster >> 16u));
    write_u16(raw + 26u, (uint16_t)cluster);
    if (!volume_write(slot.slots[lfn_entries].sector, io_sector)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    invalidate_space_samples();
    return true;
}

static bool copy_directory_tree(const char *source, const char *directory,
    const struct directory_entry *root, bool cut)
{
    uint64_t source_start = mounted_volume.start_lba;
    uint32_t source_disk = mounted_volume.disk;
    if (mounted_volume.ntfs || (root->long_name && !root->long_name_safe)
        || root->first_cluster < 2u
        || root->first_cluster >= cluster_count() + 2u) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    copy_tree_count = 1u;
    if (!append_path(copy_tree[0].source, "", source)
        || !append_path(copy_tree[0].parent, "", directory)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    copy_tree[0].directory = true;
    copy_tree[0].cluster = root->first_cluster;
    for (uint32_t i = 0u; i < copy_tree_count; ++i) {
        struct book8_file_info info;
        if (!fs_file_info(copy_tree[i].source, &info))
            return false;
        if (cut && (info.read_only || info.system)) {
            last_error = BOOK8_FS_ERR_PROTECTED;
            return false;
        }
        if (copy_tree[i].directory) {
            struct tree_collect_context c = {&copy_tree[i], cut, false, BOOK8_FS_OK};
            if (!visit_directory(copy_tree[i].cluster, false, collect_tree_entry, &c)) {
                last_error = BOOK8_FS_ERR_IO;
                return false;
            }
            if (c.error != BOOK8_FS_OK) {
                last_error = c.error;
                return false;
            }
        } else if (info.size > info.allocated_bytes) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
    }
    uint32_t target;
    bool fixed;
    if (!select_drive_for_path(directory) || mounted_volume.ntfs
        || !resolve_fat_directory(directory, &target, &fixed)) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    if (mounted_volume.disk == source_disk && mounted_volume.start_lba == source_start)
        for (uint32_t i = 0u; i < copy_tree_count; ++i)
            if (copy_tree[i].directory && copy_tree[i].cluster == target) {
                last_error = BOOK8_FS_ERR_BAD_PATH;
                return false;
            }
    for (uint32_t i = 0u; i < copy_tree_count; ++i) {
        bool ok = copy_tree[i].directory
            ? create_copy_directory(copy_tree[i].parent,
                path_basename(copy_tree[i].source))
            : fs_copy_to_directory(copy_tree[i].source, copy_tree[i].parent, false);
        if (!ok) {
            if (i != 0u)
                last_error = BOOK8_FS_ERR_COPY_PARTIAL;
            return false;
        }
    }
    if (cut) {
        for (uint32_t i = copy_tree_count; i != 0u; --i) {
            if (!delete_single_entry(copy_tree[i - 1u].source)) {
                last_error = BOOK8_FS_ERR_CUT_PARTIAL;
                return false;
            }
        }
    }
    last_error = BOOK8_FS_OK;
    return true;
}

static bool fs_copy_to_directory_unlocked(const char *source, const char *directory, bool cut)
{
    struct directory_entry entry;
    struct book8_file_info info;
    uint8_t short_name[11];
    uint32_t lfn_entries;
    const char *name = source;
    uint32_t size;
    if (source == NULL || !valid_drive_path(directory)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    if (cut ? !writable_entry(source, &entry)
            : !resolve_entry_path(source, &entry))
        return false;
    if (!fs_file_info(source, &info))
        return false;
    if (info.directory)
        return copy_directory_tree(source, directory, &entry, cut);
    if (info.size > MAX_OPEN_FILE_SIZE) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    for (const char *p = source; *p != '\0'; ++p)
        if (path_separator(*p))
            name = p + 1;
    if ((entry.long_name && !entry.long_name_safe)
        || !make_fat_name(name, short_name, &lfn_entries)) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    if (!fs_read_file(source, file_buffer, sizeof(file_buffer), &size))
        return false;
    if (size != info.size) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    if (!select_drive_for_path(directory))
        return false;
    if (mounted_volume.ntfs) {
        last_error = BOOK8_FS_ERR_UNSUPPORTED;
        return false;
    }
    uint32_t target_cluster;
    bool target_fixed;
    if (!resolve_fat_directory(directory, &target_cluster, &target_fixed)) {
        last_error = BOOK8_FS_ERR_NOT_FOUND;
        return false;
    }
    if (!choose_fat_name(name, target_cluster, target_fixed, short_name,
            &lfn_entries)) {
        return false;
    }
    char destination[MAX_PATH];
    if (!append_path(destination, directory, name)) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    if (!write_file_directory(name, file_buffer, size, target_cluster, target_fixed)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    invalidate_space_samples();
    uint32_t verify_size;
    if (!fs_read_file(destination, copy_verify_buffer,
            sizeof(copy_verify_buffer), &verify_size)
        || verify_size != size) {
        last_error = BOOK8_FS_ERR_COPY_PARTIAL;
        return false;
    }
    for (uint32_t i = 0u; i < size; ++i) {
        if (copy_verify_buffer[i] != file_buffer[i]) {
            last_error = BOOK8_FS_ERR_COPY_PARTIAL;
            return false;
        }
    }
    if (cut && !fs_delete_entry(source)) {
        last_error = BOOK8_FS_ERR_CUT_PARTIAL;
        return false;
    }
    last_error = BOOK8_FS_OK;
    return true;
}

static bool delete_file_root(const char *name)
{
    struct directory_entry entry;
    if (!find_in_directory(root_cluster(), root_is_fixed(), name, &entry)
        || (entry.attributes & FAT_ATTR_DIRECTORY) != 0u) {
        print("File not found or is a directory.\n");
        return false;
    }
    if (!volume_read(entry.sector, io_sector)) {
        print("Could not read the file directory entry.\n");
        return false;
    }
    io_sector[entry.offset] = 0xe5u;
    if (!volume_write(entry.sector, io_sector)) {
        print("Could not update the directory entry.\n");
        return false;
    }
    if (entry.first_cluster != 0u && !free_cluster_chain(entry.first_cluster)) {
        print("Directory entry was removed, but the file's cluster chain could not be freed.\n");
        return false;
    }
    return true;
}

/* Accepts "NAME" (shell drive) or "X:NAME" / "X:\NAME"; root directory only. */
static bool resolve_root_file(const char *path, char *name, size_t capacity)
{
    last_error = BOOK8_FS_OK;
    uint32_t drive = (uint32_t)(current_path[0] - 'A');
    if (path[0] != '\0' && path[1] == ':') {
        if (!((path[0] >= 'A' && path[0] <= 'Z')
            || (path[0] >= 'a' && path[0] <= 'z'))) {
            last_error = BOOK8_FS_ERR_BAD_PATH;
            return false;
        }
        drive = (uint32_t)((path[0] >= 'a' ? path[0] - 'a' + 'A' : path[0])
            - 'A');
        path += 2;
    }
    while (path_separator(*path)) {
        ++path;
    }
    size_t length = 0u;
    for (; path[length] != '\0'; ++length) {
        if (path_separator(path[length]) || length + 1u >= capacity) {
            last_error = BOOK8_FS_ERR_BAD_PATH;
            return false;
        }
        name[length] = path[length];
    }
    name[length] = '\0';
    if (length == 0u) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    return activate_drive(drive);
}

static bool fs_read_file_unlocked(const char *path, void *destination, uint32_t capacity,
    uint32_t *size)
{
    char name[MAX_NAME];
    if (path == NULL || destination == NULL || size == NULL) {
        last_error = BOOK8_FS_ERR_BAD_PATH;
        return false;
    }
    if (valid_drive_path(path)) {
        size_t length = 0u, split = 0u;
        while (path[length] != '\0') {
            if (path_separator(path[length]))
                split = length;
            ++length;
        }
        if (split > 2u) {
            char directory[MAX_PATH];
            size_t name_length = length - split - 1u;
            if (split + 1u >= sizeof(directory) || name_length == 0u
                || name_length >= sizeof(name)) {
                last_error = BOOK8_FS_ERR_BAD_PATH;
                return false;
            }
            for (size_t i = 0u; i <= split; ++i)
                directory[i] = path[i];
            directory[split + 1u] = '\0';
            for (size_t i = 0u; i <= name_length; ++i)
                name[i] = path[split + 1u + i];
            if (!select_drive_for_path(directory))
                return false;
            struct directory_entry entry;
            bool found;
            if (mounted_volume.ntfs) {
                uint64_t record;
                found = resolve_ntfs_directory(directory, &record)
                    && ntfs_find_root_file_in(record, name, &entry)
                    && (entry.attributes & FAT_ATTR_DIRECTORY) == 0u
                    && ntfs_get_file_data(entry.first_cluster, destination,
                        capacity, size, NULL);
            } else {
                uint32_t cluster;
                bool fixed_root;
                found = resolve_fat_directory(directory, &cluster, &fixed_root)
                    && find_in_directory(cluster, fixed_root, name, &entry)
                    && read_fat_file_entry(&entry, destination, capacity, size);
            }
            if (!found)
                last_error = BOOK8_FS_ERR_NOT_FOUND;
            else
                last_error = BOOK8_FS_OK;
            return found;
        }
    }
    if (!resolve_root_file(path, name, sizeof(name)))
        return false;
    if (!read_file_root(name, destination, capacity, size)) {
        last_error = BOOK8_FS_ERR_NOT_FOUND;
        return false;
    }
    return true;
}

static bool write_file(const char *path, const void *data, uint32_t size, bool replace)
{
    char name[MAX_NAME];
    last_error = BOOK8_FS_ERR_BAD_PATH;
    if (size > MAX_OPEN_FILE_SIZE) { return false; }
    if (path != NULL && valid_drive_path(path)) {
        char directory[MAX_PATH];
        size_t length = 0u, split = 0u;
        while (path[length] != '\0') {
            if (path_separator(path[length])) { split = length; }
            ++length;
        }
        size_t name_length = length - split - 1u;
        if (split + 1u >= sizeof(directory) || name_length == 0u
            || name_length >= sizeof(name) || (data == NULL && size != 0u)) {
            return false;
        }
        for (size_t i = 0u; i <= split; ++i) { directory[i] = path[i]; }
        directory[split + 1u] = '\0';
        for (size_t i = 0u; i <= name_length; ++i) { name[i] = path[split + 1u + i]; }
        if (!select_drive_for_path(directory)) { return false; }
        if (!mounted_volume.ntfs) {
            uint32_t cluster;
            bool fixed;
            if (!resolve_fat_directory(directory, &cluster, &fixed)) {
                last_error = BOOK8_FS_ERR_NOT_FOUND;
                return false;
            }
            struct directory_entry old;
            last_error = BOOK8_FS_ERR_IO;
            if (find_in_directory(cluster, fixed, name, &old)) {
                if (!replace) { last_error = BOOK8_FS_ERR_EXISTS; return false; }
                if ((old.attributes & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME | 5u)) != 0u
                    || (old.long_name && !old.long_name_safe)) {
                    last_error = BOOK8_FS_ERR_PROTECTED;
                    return false;
                }
                return replace_fat_entry(&old, data, size);
            }
            if (!write_file_directory(name, data, size, cluster, fixed)) { return false; }
            invalidate_space_samples();
            last_error = BOOK8_FS_OK;
            return true;
        }
    }
    if (path == NULL || (data == NULL && size != 0u)
        || !resolve_root_file(path, name, sizeof(name))) {
        return false;
    }
    if (!replace) { last_error = BOOK8_FS_ERR_UNSUPPORTED; return false; }
    if (!mounted_volume.ntfs) {
        struct directory_entry existing;
        if (find_in_directory(root_cluster(), root_is_fixed(), name,
            &existing)) {
            if ((existing.attributes & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME | 5u)) != 0u
                || (existing.long_name && !existing.long_name_safe)) {
                last_error = BOOK8_FS_ERR_PROTECTED;
                return false;
            }
            last_error = BOOK8_FS_ERR_IO;
            return replace_fat_entry(&existing, data, size);
        }
    }
    if (!write_file_root(name, data, size)) {
        last_error = BOOK8_FS_ERR_IO;
        return false;
    }
    last_error = BOOK8_FS_OK;
    return true;
}

static bool fs_write_file_unlocked(const char *path, const void *data, uint32_t size)
{ return write_file(path, data, size, true); }
static bool fs_create_file_unlocked(const char *path, const void *data, uint32_t size)
{ return write_file(path, data, size, false); }
static bool fs_make_directory_unlocked(const char *path)
{
    last_error = BOOK8_FS_ERR_BAD_PATH;
    if (!path || !valid_drive_path(path)) { return false; }
    char directory[MAX_PATH], name[MAX_NAME], target[MAX_PATH];
    size_t length = 0u;
    while (path[length] && length < sizeof(target)) { ++length; }
    if (length >= sizeof(target)) { return false; }
    while (length > 3u && path_separator(path[length - 1u])) { --length; }
    for (size_t i = 0u; i < length; ++i) { target[i] = path[i]; }
    target[length] = '\0';
    size_t split = 0u;
    for (size_t i = 0u; i < length; ++i) { if (path_separator(target[i])) { split = i; } }
    if (length <= split + 1u || length - split >= sizeof(name)) { return false; }
    for (size_t i = 0u; i <= split; ++i) { directory[i] = target[i]; }
    directory[split + 1u] = '\0';
    for (size_t i = split + 1u; i <= length; ++i) { name[i - split - 1u] = target[i]; }
    if (!select_drive_for_path(directory)) { return false; }
    if (mounted_volume.ntfs) { last_error = BOOK8_FS_ERR_UNSUPPORTED; return false; }
    uint32_t cluster; bool fixed;
    if (!resolve_fat_directory(directory, &cluster, &fixed)) {
        last_error = BOOK8_FS_ERR_NOT_FOUND; return false;
    }
    struct directory_entry existing;
    if (find_in_directory(cluster, fixed, name, &existing)) {
        if (!(existing.attributes & FAT_ATTR_DIRECTORY)) { last_error = BOOK8_FS_ERR_EXISTS; return false; }
        last_error = BOOK8_FS_OK; return true;
    }
    if (!create_copy_directory(directory, name)) { return false; }
    last_error = BOOK8_FS_OK; return true;
}

static bool fs_read_cluster_unlocked(char drive, uint32_t cluster, void *data,
    uint32_t capacity, uint32_t *size)
{
    char path[] = "A:\\";
    path[0] = drive;
    last_error = BOOK8_FS_ERR_BAD_PATH;
    if (data == NULL || size == NULL || !select_drive_for_path(path)) { return false; }
    if (mounted_volume.ntfs) { last_error = BOOK8_FS_ERR_UNSUPPORTED; return false; }
    uint32_t bytes = mounted_volume.sectors_per_cluster * SECTOR_SIZE;
    if (cluster < 2u || cluster >= cluster_count() + 2u || capacity < bytes) {
        return false;
    }
    uint64_t sector = cluster_sector(cluster);
    for (uint32_t i = 0u; i < mounted_volume.sectors_per_cluster; ++i) {
        if (!volume_read(sector + i, (uint8_t *)data + i * SECTOR_SIZE)) {
            last_error = BOOK8_FS_ERR_IO;
            return false;
        }
    }
    *size = bytes;
    last_error = BOOK8_FS_OK;
    return true;
}

static void print_volume_status(void)
{
    if (!mounted_volume.mounted) {
        print("No FAT volume is mounted.\n");
        return;
    }
    if (mounted_volume.ntfs) {
        print("NTFS volume mounted; limited root-directory file access.\n");
        return;
    }
    print(mounted_volume.fat32 ? "FAT32 volume mounted.\n" : "FAT16 volume mounted.\n");
}

static bool parse_disk_index(char **text, uint32_t *disk)
{
    char *start = skip_spaces(*text);
    if (*start < '0' || *start > '9') {
        return false;
    }
    uint32_t value = 0u;
    while (*start >= '0' && *start <= '9') {
        uint32_t digit = (uint32_t)(*start - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        ++start;
    }
    if (*start != '\0' && *start != ' ' && *start != '\t') {
        return false;
    }
    *disk = value;
    *text = skip_spaces(start);
    return true;
}

static void mount_command(char *arguments)
{
    uint32_t disk;
    if (!parse_disk_index(&arguments, &disk)) {
        print("Usage: MOUNT <disk-number>\n");
        return;
    }
    scan_drives();
    for (uint32_t i = 0u; i < drive_total; ++i) {
        if (drives[i].disk == disk && drives[i].readable
            && activate_drive(i)) {
            char message[48] = "FAT volume mounted as X:\\\n";
            if (mounted_volume.ntfs) {
                char ntfs[] = "NTFS volume mounted as X:\\ (limited file access).\n";
                ntfs[sizeof("NTFS volume mounted as ") - 1u] = (char)('A' + i);
                print(ntfs);
            } else {
                message[sizeof("FAT volume mounted as ") - 1u] = (char)('A' + i);
                print(message);
            }
            current_path[0] = (char)('A' + i);
            return;
        }
    }
    print("No supported FAT16/FAT32 or limited NTFS volume found.\n");
}

static bool format_fat(uint32_t disk, uint64_t start, uint64_t sectors,
    bool fat32)
{
    if (sectors < 8192u || start > disk_size_callback(disk)
        || sectors > disk_size_callback(disk) - start
        || sectors > UINT32_MAX) {
        print("Partition is too small or outside the disk for FAT formatting.\n");
        return false;
    }
    uint32_t spc = 1u;
    if (fat32 && sectors < 131072u) {
        print("FAT32 formatting requires at least 131072 sectors in this build.\n");
        return false;
    }
    if (!fat32) {
        while (sectors / spc > 65524u && spc < 128u) {
            spc *= 2u;
        }
        while (sectors / spc < 4085u && spc > 1u) {
            spc /= 2u;
        }
        if (sectors / spc < 4085u || sectors / spc >= 65525u) {
            print("FAT16 volume cluster count is outside the supported range.\n");
            return false;
        }
    }

    uint32_t reserved = fat32 ? 32u : 1u;
    uint32_t root_entries = fat32 ? 0u : 512u;
    uint32_t root_sectors = (root_entries * 32u + 511u) / 512u;
    uint32_t fat_sectors = 1u;
    uint32_t clusters = 0u;
    for (uint32_t pass = 0u; pass < 16u; ++pass) {
        uint64_t data_sectors = sectors - reserved - 2u * fat_sectors
            - root_sectors;
        clusters = (uint32_t)(data_sectors / spc);
        uint64_t required_bytes = (uint64_t)(clusters + 2u)
            * (fat32 ? 4u : 2u);
        uint32_t needed = (uint32_t)((required_bytes + 511u) / 512u);
        if (needed == fat_sectors) {
            break;
        }
        fat_sectors = needed;
    }
    if (fat32 && clusters < 65525u) {
        print("FAT32 cluster count is too small.\n");
        return false;
    }

    uint8_t sector[SECTOR_SIZE];
    clear_memory(sector, sizeof(sector));
    sector[0] = 0xebu;
    sector[1] = 0x58u;
    sector[2] = 0x90u;
    sector[3] = 'O'; sector[4] = 'S'; sector[5] = '4';
    sector[6] = 'F'; sector[7] = 'A'; sector[8] = 'T';
    write_u16(sector + 11u, SECTOR_SIZE);
    sector[13] = (uint8_t)spc;
    write_u16(sector + 14u, (uint16_t)reserved);
    sector[16] = 2u;
    write_u16(sector + 17u, (uint16_t)root_entries);
    write_u16(sector + 24u, 63u);
    write_u16(sector + 26u, 255u);
    if (sectors < 65536u) {
        write_u16(sector + 19u, (uint16_t)sectors);
    } else {
        write_u32(sector + 32u, (uint32_t)sectors);
    }
    sector[21] = 0xf8u;
    if (!fat32) {
        write_u16(sector + 22u, (uint16_t)fat_sectors);
    } else {
        write_u32(sector + 36u, fat_sectors);
        write_u16(sector + 40u, 0u);
        write_u16(sector + 42u, 0u);
        write_u32(sector + 44u, 2u);
        write_u16(sector + 48u, 1u);
        write_u16(sector + 50u, 6u);
        sector[64] = 0x80u;
        sector[66] = 0x29u;
        sector[71] = 'O'; sector[72] = 'S'; sector[73] = '4';
        sector[74] = ' '; sector[75] = 'V'; sector[76] = 'O';
        sector[77] = 'L'; sector[78] = ' ';
        sector[79] = ' '; sector[80] = ' ';
        sector[81] = 'F'; sector[82] = 'A'; sector[83] = 'T';
        sector[84] = '3'; sector[85] = '2'; sector[86] = ' ';
    }
    if (!fat32) {
        sector[36] = 0x80u;
        sector[38] = 0x29u;
        sector[43] = 'O'; sector[44] = 'S'; sector[45] = '4';
        sector[46] = ' '; sector[47] = 'V'; sector[48] = 'O';
        sector[49] = 'L'; sector[50] = ' ';
        sector[51] = ' '; sector[52] = ' ';
        sector[54] = 'F'; sector[55] = 'A'; sector[56] = 'T';
        sector[57] = '1'; sector[58] = '6'; sector[59] = ' ';
    }
    sector[510] = 0x55u;
    sector[511] = 0xaau;
    if (!write_sector_callback(disk, start, sector)) {
        return false;
    }
    for (uint32_t i = 0u; i < SECTOR_SIZE; ++i) {
        second_sector[i] = sector[i];
    }

    clear_memory(sector, sizeof(sector));
    for (uint32_t copy = 0u; copy < 2u; ++copy) {
        uint64_t fat_start = start + reserved
            + (uint64_t)copy * fat_sectors;
        clear_memory(sector, sizeof(sector));
        if (fat32) {
            write_u32(sector, 0x0ffffff8u);
            write_u32(sector + 4u, 0x0fffffffu);
            write_u32(sector + 8u, 0x0fffffffu);
        } else {
            write_u16(sector, 0xfff8u);
            write_u16(sector + 2u, 0xffffu);
        }
        if (!write_sector_callback(disk, fat_start, sector)) {
            return false;
        }
        clear_memory(sector, sizeof(sector));
        for (uint32_t i = 1u; i < fat_sectors; ++i) {
            if (!write_sector_callback(disk, fat_start + i, sector)) {
                return false;
            }
        }
    }

    uint64_t first_root = start + reserved + (uint64_t)2u * fat_sectors;
    if (fat32) {
        clear_memory(sector, sizeof(sector));
        sector[0] = 'O'; sector[1] = 'S'; sector[2] = '4';
        sector[3] = ' '; sector[4] = 'V'; sector[5] = 'O';
        sector[6] = 'L'; sector[7] = ' ';
        sector[8] = ' '; sector[9] = ' ';
        sector[10] = ' '; sector[11] = FAT_ATTR_VOLUME;
        if (!write_sector_callback(disk,
            start + reserved + 2u * fat_sectors, sector)) {
            return false;
        }
        clear_memory(sector, sizeof(sector));
        write_u32(sector, 0x41615252u);
        write_u32(sector + 484u, 0x61417272u);
        write_u32(sector + 488u, clusters - 1u);
        write_u32(sector + 492u, 3u);
        write_u32(sector + 508u, 0xaa550000u);
        if (!write_sector_callback(disk, start + 1u, sector)
            || !write_sector_callback(disk, start + 7u, sector)
            || !write_sector_callback(disk, start + 6u, second_sector)) {
            return false;
        }
    } else {
        clear_memory(sector, sizeof(sector));
        for (uint32_t i = 0u; i < root_sectors; ++i) {
            if (!write_sector_callback(disk, first_root + i, sector)) {
                return false;
            }
        }
    }
    if (!mount_fat_partition(disk, 0u,
        &(struct partition){start, sectors, fat32 ? 0x0cu : 0x06u})) {
        print("Formatted volume could not be remounted.\n");
        return false;
    }
    return true;
}

static void format_command(char *arguments)
{
    uint32_t disk;
    if (!parse_disk_index(&arguments, &disk)) {
        print("Usage: FORMAT <disk-number> [partition-number] FAT16|FAT32\n");
        return;
    }
    uint32_t partition_index = 0u;
    char *format = arguments;
    char *separator = format;
    while (*separator != '\0' && *separator != ' ' && *separator != '\t') {
        ++separator;
    }
    if (*separator != '\0') {
        *separator++ = '\0';
        uint32_t parsed;
        char *partition_text = format;
        if (!parse_disk_index(&partition_text, &parsed) || *partition_text != '\0') {
            print("Usage: FORMAT <disk-number> [partition-number] FAT16|FAT32\n");
            return;
        }
        partition_index = parsed;
        format = skip_spaces(separator);
    }
    bool fat32 = equal_case_insensitive(format, "FAT32");
    if (!fat32 && !equal_case_insensitive(format, "FAT16")) {
        print("Usage: FORMAT <disk-number> [partition-number] FAT16|FAT32 (NTFS format deferred).\n");
        return;
    }
    uint64_t disk_sectors = disk_size_callback(disk);
    if (disk_sectors == 0u) {
        print("Disk number is not present.\n");
        return;
    }
    uint64_t start = 0u;
    uint64_t sectors = disk_sectors;
    if (partition_index != 0u) {
        struct partition partitions[MAX_PARTITIONS];
        bool is_gpt;
        uint32_t count = read_partitions(disk, partitions, &is_gpt);
        (void)is_gpt;
        if (partition_index > count || partitions[partition_index - 1u].type == 0u) {
            print("Partition number is not present.\n");
            return;
        }
        start = partitions[partition_index - 1u].first_lba;
        sectors = partitions[partition_index - 1u].sector_count;
        partition_index--;
    }
    print("FORMAT will erase the selected volume. This build formats FAT only.\n");
    if (!confirm_callback("Type YES to format this disk: ")) {
        print("Format cancelled; disk was not modified.\n");
        return;
    }
    if (format_fat(disk, start, sectors, fat32)) {
        mounted_volume.partition_index = partition_index;
        drives_dirty = true;
        print("FAT volume formatted; use DRIVES to see its drive letter.\n");
    } else {
        print("FAT formatting failed.\n");
    }
}

static void fdisk_command(char *arguments)
{
    uint32_t disk;
    if (!parse_disk_index(&arguments, &disk)) {
        print("Usage: FDISK <disk-number> [CREATE MBR|GPT]\n");
        return;
    }
    if (*arguments != '\0') {
        if (!equal_case_insensitive(arguments, "CREATE MBR")
            && !equal_case_insensitive(arguments, "CREATE GPT")) {
            print("Usage: FDISK <disk-number> [CREATE MBR|GPT]\n");
            return;
        }
        if (disk_size_callback(disk) < 8192u) {
            print("Disk is too small for a supported partition table.\n");
            return;
        }
        bool make_gpt = equal_case_insensitive(arguments, "CREATE GPT");
        print(make_gpt
            ? "FDISK will replace the table and create one GPT data partition.\n"
            : "FDISK will replace the table and create a FAT32 root plus 4 GiB raw swap.\n");
        if (!confirm_callback("Type YES to repartition this disk: ")) {
            print("Partitioning cancelled; disk was not modified.\n");
            return;
        }
        uint64_t sectors = disk_size_callback(disk);
        if (!read_sector_callback(disk, 0u, io_sector)) {
            print("Could not read the disk before partitioning.\n");
            return;
        }
        if (!make_gpt) {
            if (sectors > 0x100000000ull) {
                print("MBR is limited to 2 TiB; use GPT for this disk.\n");
                return;
            }
            if (sectors < (uint64_t)MBR_ROOT_START_LBA
                    + MBR_MIN_ROOT_SECTORS + MBR_SWAP_SECTORS) {
                print("MBR requires at least a 2 GiB root and 4 GiB raw swap partition.\n");
                return;
            }
            uint32_t root_sectors = (uint32_t)(sectors
                - MBR_ROOT_START_LBA - MBR_SWAP_SECTORS);
            uint32_t swap_start = MBR_ROOT_START_LBA + root_sectors;
            clear_memory(io_sector, sizeof(io_sector));
            io_sector[446u] = 0x80u;
            io_sector[447u] = 0xffu;
            io_sector[448u] = 0xffu;
            io_sector[449u] = 0xffu;
            io_sector[450u] = 0x0cu;
            io_sector[451u] = 0xffu;
            io_sector[452u] = 0xffu;
            io_sector[453u] = 0xffu;
            write_u32(io_sector + 454u, MBR_ROOT_START_LBA);
            write_u32(io_sector + 458u, root_sectors);
            io_sector[462u] = 0u;
            io_sector[463u] = 0xffu;
            io_sector[464u] = 0xffu;
            io_sector[465u] = 0xffu;
            io_sector[466u] = 0x82u;
            io_sector[467u] = 0xffu;
            io_sector[468u] = 0xffu;
            io_sector[469u] = 0xffu;
            write_u32(io_sector + 470u, swap_start);
            write_u32(io_sector + 474u, MBR_SWAP_SECTORS);
            io_sector[510] = 0x55u;
            io_sector[511] = 0xaau;
            if (!write_sector_callback(disk, 0u, io_sector)) {
                print("MBR write failed.\n");
                return;
            }
        } else {
            if (sectors < 4096u) {
                print("Disk is too small for GPT metadata and a data partition.\n");
                return;
            }
            static uint8_t entries[128u * 128u];
            clear_memory(entries, sizeof(entries));
            static const uint8_t basic_data_guid[16] = {
                0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,
                0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7
            };
            static const uint8_t disk_guid[16] = {
                0x4f,0x53,0x34,0x47,0x50,0x54,0x41,0x42,
                0x80,0x00,0x4f,0x53,0x34,0x44,0x49,0x53
            };
            for (uint32_t i = 0u; i < 16u; ++i) {
                entries[i] = basic_data_guid[i];
                entries[16u + i] = (uint8_t)(disk_guid[i] ^ (uint8_t)disk);
            }
            write_u64(entries + 32u, 2048u);
            write_u64(entries + 40u, sectors - 34u);
            static const char name[] = "BOOK8 DATA";
            for (uint32_t i = 0u; i < sizeof(name) - 1u; ++i) {
                write_u16(entries + 56u + i * 2u, (uint16_t)name[i]);
            }
            uint32_t entries_crc = crc32(entries, sizeof(entries));
            uint64_t backup_entries_lba = sectors - 33u;
            for (uint32_t i = 0u; i < 32u; ++i) {
                if (!write_sector_callback(disk, 2u + i,
                    entries + i * SECTOR_SIZE)) {
                    print("GPT primary partition-entry write failed.\n");
                    return;
                }
                if (!write_sector_callback(disk, backup_entries_lba + i,
                    entries + i * SECTOR_SIZE)) {
                    print("GPT backup partition-entry write failed.\n");
                    return;
                }
            }
            uint8_t header[SECTOR_SIZE];
            for (uint32_t copy = 0u; copy < 2u; ++copy) {
                clear_memory(header, sizeof(header));
                header[0] = 'E'; header[1] = 'F';
                header[2] = 'I'; header[3] = ' ';
                header[4] = 'P'; header[5] = 'A';
                header[6] = 'R'; header[7] = 'T';
                write_u32(header + 8u, 0x00010000u);
                write_u32(header + 12u, 92u);
                write_u64(header + 24u, copy == 0u ? 1u : sectors - 1u);
                write_u64(header + 32u, copy == 0u ? sectors - 1u : 1u);
                write_u64(header + 40u, 34u);
                write_u64(header + 48u, sectors - 34u);
                for (uint32_t i = 0u; i < 16u; ++i) {
                    header[56u + i] = disk_guid[i];
                }
                write_u64(header + 72u, copy == 0u ? 2u : backup_entries_lba);
                write_u32(header + 80u, 128u);
                write_u32(header + 84u, 128u);
                write_u32(header + 88u, entries_crc);
                write_u32(header + 16u, crc32(header, 92u));
                uint64_t header_lba = copy == 0u ? 1u : sectors - 1u;
                if (!write_sector_callback(disk, header_lba, header)) {
                    print("GPT header write failed.\n");
                    return;
                }
            }
            clear_memory(io_sector, sizeof(io_sector));
            io_sector[446u + 4u] = 0xeeu;
            write_u32(io_sector + 446u + 8u, 1u);
            write_u32(io_sector + 446u + 12u,
                sectors - 1u > 0xffffffffu ? 0xffffffffu
                    : (uint32_t)(sectors - 1u));
            io_sector[510] = 0x55u;
            io_sector[511] = 0xaau;
            if (!write_sector_callback(disk, 0u, io_sector)) {
                print("Protective MBR write failed.\n");
                return;
            }
        }
        print(make_gpt ? "GPT partition table created.\n"
            : "MBR partition table created.\n");
        mounted_volume.mounted = false;
        drives_dirty = true;
        return;
    }
    struct partition partitions[MAX_PARTITIONS];
    bool is_gpt;
    uint32_t count = read_partitions(disk, partitions, &is_gpt);
    if (disk_size_callback(disk) == 0u) {
        print("Disk number is not present.\n");
        return;
    }
    print(is_gpt ? "GPT partition table:\n" : "MBR partition table:\n");
    if (count == 0u) {
        print("No valid partitions were found.\n");
        return;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        print("Partition ");
        char number[2] = {(char)('1' + i), '\0'};
        print(number);
        print(": start LBA ");
        char digits[21];
        uint32_t length = 0u;
        uint64_t value = partitions[i].first_lba;
        do {
            digits[length++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        while (length > 0u) {
            char digit[2] = {digits[--length], '\0'};
            print(digit);
        }
        print(", sectors ");
        length = 0u;
        value = partitions[i].sector_count;
        do {
            digits[length++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        while (length > 0u) {
            char digit[2] = {digits[--length], '\0'};
            print(digit);
        }
        static const char hex[] = "0123456789ABCDEF";
        print(", type 0x");
        char type_digits[3] = {hex[partitions[i].type >> 4u],
            hex[partitions[i].type & 0x0fu], '\0'};
        print(type_digits);
        print("\n");
    }
    print("CREATE MBR makes a FAT32 root and raw swap partition; CREATE GPT makes one data partition.\n");
}

static void fs_initialize_unlocked(book8_read_sector_fn read_sector,
    book8_write_sector_fn write_sector, book8_disk_size_fn disk_size,
    book8_print_fn print, book8_confirm_fn confirm)
{
    read_sector_callback = read_sector;
    write_sector_callback = write_sector;
    disk_size_callback = disk_size;
    print_callback = print;
    confirm_callback = confirm;
    clear_memory(&mounted_volume, sizeof(mounted_volume));
    (void)auto_mount();
}

static const char *fs_current_path_unlocked(void)
{
    return current_path;
}

static bool fs_handle_command_unlocked(const char *command, char *arguments)
{
    if (command[0] != '\0' && command[1] == ':' && command[2] == '\0'
        && ((command[0] >= 'A' && command[0] <= 'Z')
            || (command[0] >= 'a' && command[0] <= 'z'))) {
        char letter = command[0] >= 'a' ? (char)(command[0] - 'a' + 'A')
            : command[0];
        if (activate_drive((uint32_t)(letter - 'A'))) {
            current_path[0] = letter;
            current_path[1] = ':';
            current_path[2] = '\\';
            current_path[3] = '\0';
            print(current_path);
            print("\n");
        } else {
            print(fs_error_text(last_error));
            print("\n");
        }
        return true;
    }

    if (equal_case_insensitive(command, "DRIVES")) {
        for (uint32_t i = 0u; i < fs_drive_count(); ++i) {
            const char *type = "";
            bool readable = false;
            (void)fs_drive_info(i, &type, &readable);
            char line[4] = {fs_drive_letter(i), ':', ' ', '\0'};
            print(line);
            print(type);
            print(readable ? "\n" : " (unreadable)\n");
        }
        return true;
    }
    if (!equal_case_insensitive(command, "MOUNT")
        && !equal_case_insensitive(command, "FORMAT")
        && !equal_case_insensitive(command, "FDISK")) {
        /* The explorer may have switched volumes; commands use the shell drive. */
        (void)activate_drive((uint32_t)(current_path[0] - 'A'));
    }
    if (equal_case_insensitive(command, "MOUNT")) {
        mount_command(arguments);
    } else if (equal_case_insensitive(command, "FORMAT")) {
        format_command(arguments);
    } else if (equal_case_insensitive(command, "FDISK")) {
        fdisk_command(arguments);
    } else if (equal_case_insensitive(command, "VOL")) {
        print_volume_status();
    } else if (equal_case_insensitive(command, "DIR")) {
        (void)list_root();
    } else if (equal_case_insensitive(command, "TYPE")) {
        char name[MAX_NAME];
        if (read_argument(&arguments, name, sizeof(name)) == NULL
            || *arguments != '\0') {
            print("Usage: TYPE <name> (quote long names).\n");
            return true;
        }
        uint32_t size = 0u;
        if (name[0] == '\0' || !read_file_root(name, file_buffer,
            sizeof(file_buffer), &size)) {
            print("File not found, unsupported, or too large.\n");
        } else {
            for (uint32_t i = 0u; i < size; ++i) {
                char character[2] = {(char)file_buffer[i], '\0'};
                print(character);
            }
            print("\n");
        }
    } else if (equal_case_insensitive(command, "CD")
        || equal_case_insensitive(command, "CHDIR")) {
        char *path = skip_spaces(arguments);
        if (*path == '\0' || equal_case_insensitive(path, "\\")
            || equal_case_insensitive(path, "/")) {
            current_path[2] = '\\';
            current_path[3] = '\0';
            print(current_path);
            print("\n");
        } else {
            print("Directory changes require a mounted directory-entry path.\n");
        }
    } else if (equal_case_insensitive(command, "COPY")) {
        char *source = skip_spaces(arguments);
        char source_name[MAX_NAME];
        char destination_name[MAX_NAME];
        char *destination = read_argument(&source, source_name, sizeof(source_name));
        if (destination != NULL) {
            destination = read_argument(&source, destination_name,
                sizeof(destination_name));
        }
        uint32_t size = 0u;
        if (destination == NULL || *source != '\0'
            || !read_file_root(source_name, file_buffer, sizeof(file_buffer), &size)) {
            print("Usage: COPY <source> <destination> (quote names containing spaces).\n");
        } else {
            (void)write_file_root(destination_name, file_buffer, size);
        }
    } else if (equal_case_insensitive(command, "WRITEFILE")) {
        char name[MAX_NAME];
        char *text = read_argument(&arguments, name, sizeof(name));
        if (text == NULL || *arguments == '\0') {
            print("Usage: WRITEFILE <name> <text> (quote long names).\n");
        } else {
            size_t length = 0u;
            while (arguments[length] != '\0' && length < MAX_OPEN_FILE_SIZE) {
                file_buffer[length] = (uint8_t)arguments[length];
                ++length;
            }
            if (arguments[length] != '\0') {
                print("File is too large.\n");
            } else {
                (void)write_file_root(name, file_buffer, (uint32_t)length);
            }
        }
    } else if (equal_case_insensitive(command, "DEL")
        || equal_case_insensitive(command, "ERASE")) {
        char name[MAX_NAME];
        if (read_argument(&arguments, name, sizeof(name)) == NULL
            || *arguments != '\0') {
            print("Usage: DEL <name> (quote long names).\n");
        } else {
            (void)delete_file_root(name);
        }
    } else if (equal_case_insensitive(command, "MD")
        || equal_case_insensitive(command, "MKDIR")
        || equal_case_insensitive(command, "RD")
        || equal_case_insensitive(command, "RMDIR")) {
        print("FAT directory modification is deferred until allocation and recovery checks are complete.\n");
    } else {
        return false;
    }
    return true;
}

#define FS_WRAP_BOOL(name, parameters, arguments) \
    bool name parameters \
    { \
        uint64_t flags = sched_irq_save(); \
        bool result = name##_unlocked arguments; \
        sched_irq_restore(flags); \
        return result; \
    }

FS_WRAP_BOOL(fs_drive_extent,
    (uint32_t index, uint32_t *disk, uint64_t *first_lba,
        uint64_t *sector_count),
    (index, disk, first_lba, sector_count))
FS_WRAP_BOOL(fs_mount_disk_as_root, (uint32_t disk), (disk))
FS_WRAP_BOOL(fs_file_info,
    (const char *path, struct book8_file_info *info), (path, info))
FS_WRAP_BOOL(fs_list_directory,
    (const char *path, book8_directory_entry_fn visitor, void *context),
    (path, visitor, context))
FS_WRAP_BOOL(fs_list_root,
    (book8_directory_entry_fn visitor, void *context), (visitor, context))
FS_WRAP_BOOL(fs_drive_info,
    (uint32_t index, const char **type, bool *readable),
    (index, type, readable))
FS_WRAP_BOOL(fs_drive_space,
    (uint32_t index, struct book8_drive_space *space), (index, space))
FS_WRAP_BOOL(fs_rename_entry,
    (const char *path, const char *new_name), (path, new_name))
FS_WRAP_BOOL(fs_replace_boot_kernel,
    (const void *buffer, uint32_t size), (buffer, size))
FS_WRAP_BOOL(fs_delete_entry, (const char *path), (path))
FS_WRAP_BOOL(fs_copy_to_directory,
    (const char *source, const char *directory, bool cut),
    (source, directory, cut))
FS_WRAP_BOOL(fs_read_file,
    (const char *path, void *destination, uint32_t capacity, uint32_t *size),
    (path, destination, capacity, size))
FS_WRAP_BOOL(fs_write_file,
    (const char *path, const void *data, uint32_t size), (path, data, size))
FS_WRAP_BOOL(fs_create_file,
    (const char *path, const void *data, uint32_t size), (path, data, size))
FS_WRAP_BOOL(fs_make_directory, (const char *path), (path))
FS_WRAP_BOOL(fs_read_cluster,
    (char drive, uint32_t cluster, void *data, uint32_t capacity,
        uint32_t *size),
    (drive, cluster, data, capacity, size))
FS_WRAP_BOOL(fs_handle_command,
    (const char *command, char *arguments), (command, arguments))

#undef FS_WRAP_BOOL

int fs_last_error(void)
{
    uint64_t flags = sched_irq_save();
    int result = fs_last_error_unlocked();
    sched_irq_restore(flags);
    return result;
}

const char *fs_error_text(int error)
{
    uint64_t flags = sched_irq_save();
    const char *result = fs_error_text_unlocked(error);
    sched_irq_restore(flags);
    return result;
}

uint32_t fs_drive_count(void)
{
    uint64_t flags = sched_irq_save();
    uint32_t result = fs_drive_count_unlocked();
    sched_irq_restore(flags);
    return result;
}

char fs_drive_letter(uint32_t index)
{
    uint64_t flags = sched_irq_save();
    char result = fs_drive_letter_unlocked(index);
    sched_irq_restore(flags);
    return result;
}

void fs_space_poll(void)
{
    uint64_t flags = sched_irq_save();
    fs_space_poll_unlocked();
    sched_irq_restore(flags);
}

void fs_initialize(book8_read_sector_fn read_sector,
    book8_write_sector_fn write_sector, book8_disk_size_fn disk_size,
    book8_print_fn print_callback_arg, book8_confirm_fn confirm)
{
    uint64_t flags = sched_irq_save();
    fs_initialize_unlocked(read_sector, write_sector, disk_size,
        print_callback_arg, confirm);
    sched_irq_restore(flags);
}

const char *fs_current_path(void)
{
    uint64_t flags = sched_irq_save();
    const char *result = fs_current_path_unlocked();
    sched_irq_restore(flags);
    return result;
}
