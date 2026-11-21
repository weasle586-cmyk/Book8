#include "fs.h"

#define ATTR_VOLUME 0x08u
#define ATTR_DIR    0x10u
#define ATTR_LFN    0x0fu

struct volume {
    bool mounted;
    block_device_t *dev;
    uint32_t part_lba;
    uint32_t sectors_per_cluster;
    uint32_t reserved;
    uint32_t fat_size;
    uint32_t root_entries;
    uint32_t root_lba;      /* absolute, FAT16 fixed root */
    uint32_t root_sectors;
    uint32_t data_lba;      /* absolute LBA of cluster 2 */
    uint32_t cluster_total;
    uint32_t root_cluster;  /* FAT32 */
    uint32_t eoc;
    bool fat32;
};

struct dirent {
    uint32_t cluster;
    uint32_t size;
    uint8_t attr;
};

static struct volume vol;
static file_handle_t handles[FS_MAX_OPEN];
static fs_drive_resolver drive_resolver;

void fs_set_drive_resolver(fs_drive_resolver resolver)
{
    drive_resolver = resolver;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static bool bytes_equal(const uint8_t *a, const char *b, uint32_t n)
{
    while (n--) {
        if (*a++ != (uint8_t)*b++) {
            return false;
        }
    }
    return true;
}

static void copy_bytes(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
}

static bool is_power_of_two(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static bool parse_bpb(block_device_t *dev, uint32_t lba, uint64_t capacity,
    struct volume *out)
{
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(16)));
    uint32_t total, fat_size, root_sectors, data_sectors, i;
    uint32_t spc, reserved, fats;

    if (!block_cache_read(dev, lba, 1u, sector)
        || sector[510] != 0x55u || sector[511] != 0xaau) {
        return false;
    }
    spc = sector[13];
    reserved = rd16(sector + 14);
    fats = sector[16];
    if (rd16(sector + 11) != BLOCK_SECTOR_SIZE || !is_power_of_two(spc)
        || reserved == 0u || fats == 0u || fats > 2u) {
        return false;
    }
    total = rd16(sector + 19) != 0u ? rd16(sector + 19) : rd32(sector + 32);
    fat_size = rd16(sector + 22) != 0u ? rd16(sector + 22) : rd32(sector + 36);
    if (total == 0u || fat_size == 0u || total > capacity
        || (uint64_t)lba + total > 0x100000000ull) {
        return false;
    }
    root_sectors = ((uint32_t)rd16(sector + 17) * 32u + 511u) / 512u;
    if ((uint64_t)reserved + (uint64_t)fats * fat_size + root_sectors >= total) {
        return false;
    }
    data_sectors = total - reserved - fats * fat_size - root_sectors;

    for (i = 0; i < sizeof(*out); ++i) {
        ((uint8_t *)out)[i] = 0;
    }
    out->dev = dev;
    out->part_lba = lba;
    out->sectors_per_cluster = spc;
    out->reserved = reserved;
    out->fat_size = fat_size;
    out->root_entries = rd16(sector + 17);
    out->root_sectors = root_sectors;
    out->root_lba = lba + reserved + fats * fat_size;
    out->data_lba = out->root_lba + root_sectors;
    out->cluster_total = data_sectors / spc;
    if (out->cluster_total < 4085u) {
        return false; /* FAT12 is not supported */
    }
    out->fat32 = out->cluster_total >= 65525u;
    if (out->fat32) {
        if (!bytes_equal(sector + 82, "FAT32", 5) || out->root_entries != 0u) {
            return false;
        }
        out->root_cluster = rd32(sector + 44);
        out->eoc = 0x0ffffff8u;
    } else {
        if (!bytes_equal(sector + 54, "FAT", 3) || out->root_entries == 0u) {
            return false;
        }
        out->eoc = 0xfff8u;
    }
    out->mounted = true;
    return true;
}

bool fs_mount(block_device_t *dev)
{
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(16)));
    struct volume found;
    uint32_t i;

    vol.mounted = false;
    for (i = 0; i < FS_MAX_OPEN; ++i) {
        handles[i].used = false;
    }
    if (dev == 0) {
        return false;
    }
    if (parse_bpb(dev, 0u, dev->sector_count, &found)) {
        vol = found;
        return true;
    }
    if (!block_cache_read(dev, 0u, 1u, mbr) || mbr[510] != 0x55u
        || mbr[511] != 0xaau) {
        return false;
    }
    for (i = 0; i < 4u; ++i) {
        const uint8_t *entry = mbr + 446 + i * 16u;
        uint8_t type = entry[4];

        if ((type == 0x04u || type == 0x06u || type == 0x0bu || type == 0x0cu
                || type == 0x0eu || type == 0xefu)
            && (uint64_t)rd32(entry + 8) + rd32(entry + 12) <= dev->sector_count
            && parse_bpb(dev, rd32(entry + 8), rd32(entry + 12), &found)) {
            vol = found;
            return true;
        }
    }
    return false;
}

bool fs_is_mounted(void)
{
    return vol.mounted;
}

static uint32_t cluster_lba(uint32_t cluster)
{
    return vol.data_lba + (cluster - 2u) * vol.sectors_per_cluster;
}

static bool valid_cluster(uint32_t cluster)
{
    return cluster >= 2u && cluster < vol.cluster_total + 2u;
}

/* Next cluster in the chain; *next is >= eoc at the end. */
static bool fat_next(uint32_t cluster, uint32_t *next)
{
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(16)));
    uint32_t offset = cluster * (vol.fat32 ? 4u : 2u);

    if (!block_cache_read(vol.dev, vol.part_lba + vol.reserved + offset / 512u,
            1u, sector)) {
        return false;
    }
    if (vol.fat32) {
        *next = rd32(sector + offset % 512u) & 0x0fffffffu;
    } else {
        *next = rd16(sector + offset % 512u);
    }
    return true;
}

static void to_short_name(const char *start, uint32_t length, uint8_t out[11],
    bool *ok)
{
    uint32_t i, dot = length, name_len, ext_len;

    for (i = 0; i < 11u; ++i) {
        out[i] = ' ';
    }
    *ok = false;
    for (i = 0; i < length; ++i) {
        if (start[i] == '.') {
            dot = i;
        }
    }
    if (length == 0u || dot == 0u) {
        return;
    }
    name_len = dot;
    ext_len = dot < length ? length - dot - 1u : 0u;
    if (name_len > 8u || ext_len > 3u) {
        return;
    }
    for (i = 0; i < name_len; ++i) {
        char c = start[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[i] = (uint8_t)c;
    }
    for (i = 0; i < ext_len; ++i) {
        char c = start[dot + 1u + i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[8u + i] = (uint8_t)c;
    }
    *ok = true;
}

/* Search one directory (cluster 0 = FAT16 fixed root) for a short name. */
static bool dir_find(uint32_t dir_cluster, const uint8_t name[11],
    struct dirent *out)
{
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(16)));
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0u;

    for (;;) {
        uint32_t lba, count, s, e;

        if (dir_cluster == 0u) {
            lba = vol.root_lba;
            count = vol.root_sectors;
        } else {
            if (!valid_cluster(cluster)) {
                return false;
            }
            lba = cluster_lba(cluster);
            count = vol.sectors_per_cluster;
        }
        for (s = 0; s < count; ++s) {
            if (!block_cache_read(vol.dev, lba + s, 1u, sector)) {
                return false;
            }
            for (e = 0; e < 16u; ++e) {
                const uint8_t *d = sector + e * 32u;

                if (d[0] == 0x00u) {
                    return false;
                }
                if (d[0] == 0xe5u || d[11] == ATTR_LFN
                    || (d[11] & ATTR_VOLUME) != 0u) {
                    continue;
                }
                if (bytes_equal(d, (const char *)name, 11)) {
                    out->attr = d[11];
                    out->size = rd32(d + 28);
                    out->cluster = (uint32_t)rd16(d + 26)
                        | (vol.fat32 ? (uint32_t)rd16(d + 20) << 16 : 0u);
                    return true;
                }
            }
        }
        if (dir_cluster == 0u) {
            return false;
        }
        if (!fat_next(cluster, &cluster) || cluster >= vol.eoc
            || ++guard > vol.cluster_total) {
            return false;
        }
    }
}

static file_handle_t *open_on_current(const char *path)
{
    uint32_t dir = vol.fat32 ? vol.root_cluster : 0u;
    file_handle_t *file = 0;
    struct dirent entry;
    uint32_t i;

    if (!vol.mounted) {
        return 0;
    }
    while (*path == '/' || *path == '\\') {
        ++path;
    }
    for (;;) {
        const char *part = path;
        uint8_t name[11];
        bool ok;
        uint32_t length = 0u;

        while (path[length] != '\0' && path[length] != '/'
            && path[length] != '\\') {
            ++length;
        }
        to_short_name(part, length, name, &ok);
        if (!ok || !dir_find(dir, name, &entry)) {
            return 0;
        }
        path += length;
        while (*path == '/' || *path == '\\') {
            ++path;
        }
        if (*path == '\0') {
            if ((entry.attr & ATTR_DIR) != 0u) {
                return 0;
            }
            break;
        }
        if ((entry.attr & ATTR_DIR) == 0u) {
            return 0;
        }
        dir = entry.cluster; /* ".." to the root would be 0, which is the root */
        if (dir == 0u && vol.fat32) {
            dir = vol.root_cluster;
        }
    }
    for (i = 0; i < FS_MAX_OPEN; ++i) {
        if (!handles[i].used) {
            file = &handles[i];
            break;
        }
    }
    if (file == 0) {
        return 0;
    }
    file->used = true;
    file->first_cluster = entry.cluster;
    file->size = entry.size;
    file->offset = 0u;
    file->cur_cluster = entry.cluster;
    file->cur_index = 0u;
    return file;
}

static bool select_path_volume(const char **path, bool use_default)
{
    uint32_t drive = UINT32_MAX;
    if ((*path)[0] != '\0' && (*path)[1] == ':') {
        char letter = (*path)[0];
        if (letter >= 'a' && letter <= 'z')
            letter = (char)(letter - 'a' + 'A');
        if (letter < 'A' || letter > 'Z' || drive_resolver == 0)
            return false;
        drive = (uint32_t)(letter - 'A');
        *path += 2;
    } else if (!use_default || drive_resolver == 0) {
        return true;
    }
    struct fs_extent extent;
    if (!drive_resolver(drive, &extent) || extent.dev == 0
        || extent.sector_count == 0u
        || (uint64_t)extent.first_lba + extent.sector_count > extent.dev->sector_count)
        return false;
    if (vol.mounted && vol.dev == extent.dev && vol.part_lba == extent.first_lba)
        return true;
    /* Switching volumes must never invalidate another open descriptor. */
    for (uint32_t i = 0u; i < FS_MAX_OPEN; ++i) {
        if (handles[i].used)
            return false;
    }
    struct volume found;
    if (!parse_bpb(extent.dev, extent.first_lba, extent.sector_count, &found))
        return false;
    vol = found;
    return true;
}

file_handle_t *fs_open(const char *path)
{
    if (!select_path_volume(&path, false))
        return 0;
    return open_on_current(path);
}

file_handle_t *fs_open_any(const char *path)
{
    if (drive_resolver != 0) {
        if (!select_path_volume(&path, true))
            return 0;
        return open_on_current(path);
    }
    file_handle_t *file = open_on_current(path);
    struct volume saved = vol;
    uint32_t i;

    for (i = 0; file == 0 && i < block_count(); ++i) {
        if (saved.mounted && saved.dev == block_get(i)) {
            continue;
        }
        if (fs_mount(block_get(i))) {
            file = open_on_current(path);
        }
    }
    if (file == 0) {
        vol = saved;
    }
    return file;
}

/* Position cur_cluster on the cluster that holds byte `offset`. */
static bool locate(file_handle_t *file, uint32_t offset)
{
    uint32_t bytes = vol.sectors_per_cluster * 512u;
    uint32_t want = offset / bytes;

    if (want < file->cur_index) {
        file->cur_index = 0u;
        file->cur_cluster = file->first_cluster;
    }
    while (file->cur_index < want) {
        uint32_t next;

        if (!valid_cluster(file->cur_cluster)
            || !fat_next(file->cur_cluster, &next) || !valid_cluster(next)) {
            return false;
        }
        file->cur_cluster = next;
        ++file->cur_index;
    }
    return valid_cluster(file->cur_cluster);
}

int32_t fs_read(file_handle_t *file, void *buf, uint32_t count)
{
    uint8_t *out = buf;
    uint32_t total = 0u;

    if (file == 0 || !file->used || !vol.mounted) {
        return -1;
    }
    if (file->offset >= file->size) {
        return 0;
    }
    if (count > file->size - file->offset) {
        count = file->size - file->offset;
    }
    if (count > 0x7fffffffu) {
        count = 0x7fffffffu;
    }
    while (total < count) {
        uint32_t cluster_bytes = vol.sectors_per_cluster * 512u;
        uint32_t in_cluster = file->offset % cluster_bytes;
        uint32_t sector = in_cluster / 512u;
        uint32_t in_sector = in_cluster % 512u;
        uint32_t lba, chunk;

        if (!locate(file, file->offset)) {
            return -1;
        }
        lba = cluster_lba(file->cur_cluster) + sector;
        chunk = count - total;
        if (in_sector == 0u && chunk >= 512u && ((uint64_t)out & 15u) == 0u) {
            /* Whole sectors straight into the caller, bypassing the cache. */
            uint32_t sectors = chunk / 512u;
            uint32_t left = vol.sectors_per_cluster - sector;

            if (sectors > left) {
                sectors = left;
            }
            if (!block_read(vol.dev, lba, sectors, out)) {
                return -1;
            }
            chunk = sectors * 512u;
        } else {
            uint8_t tmp[BLOCK_SECTOR_SIZE] __attribute__((aligned(16)));

            if (!block_cache_read(vol.dev, lba, 1u, tmp)) {
                return -1;
            }
            if (chunk > 512u - in_sector) {
                chunk = 512u - in_sector;
            }
            copy_bytes(out, tmp + in_sector, chunk);
        }
        out += chunk;
        total += chunk;
        file->offset += chunk;
    }
    return (int32_t)total;
}

bool fs_seek(file_handle_t *file, uint32_t offset)
{
    if (file == 0 || !file->used || offset > file->size) {
        return false;
    }
    file->offset = offset;
    return true;
}

void fs_close(file_handle_t *file)
{
    if (file != 0) {
        file->used = false;
    }
}
