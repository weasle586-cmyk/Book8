#ifndef BOOK8_FILESYSTEM_H
#define BOOK8_FILESYSTEM_H

#include <stdbool.h>
#include <stdint.h>

typedef bool (*book8_read_sector_fn)(uint32_t disk, uint64_t lba,
    uint8_t sector[512]);
typedef bool (*book8_write_sector_fn)(uint32_t disk, uint64_t lba,
    const uint8_t sector[512]);
typedef uint64_t (*book8_disk_size_fn)(uint32_t disk);
typedef void (*book8_print_fn)(const char *text);
typedef bool (*book8_confirm_fn)(const char *prompt);
typedef bool (*book8_directory_entry_fn)(const char *name, bool directory,
    uint32_t size, void *context);

void fs_initialize(book8_read_sector_fn read_sector,
    book8_write_sector_fn write_sector, book8_disk_size_fn disk_size,
    book8_print_fn print, book8_confirm_fn confirm);
/* Rescans volumes, selects the target disk's FAT32 root as A:, and mounts it. */
bool fs_mount_disk_as_root(uint32_t disk);
bool fs_handle_command(const char *command, char *arguments);
bool fs_list_root(book8_directory_entry_fn visitor, void *context);
/* Drive roots may be A: or A:\; empty/root-relative paths use the active
 * command drive, falling back to the first readable volume if unavailable. */
bool fs_list_directory(const char *path, book8_directory_entry_fn visitor,
    void *context);
const char *fs_current_path(void);
/* FAT reads/writes support absolute directory paths; parent must exist.
 * Replacing an existing FAT file verifies a new chain before directory commit. */
bool fs_read_file(const char *path, void *destination, uint32_t capacity,
    uint32_t *size);
bool fs_write_file(const char *path, const void *data, uint32_t size);
bool fs_create_file(const char *path, const void *data, uint32_t size);
/* Creates one FAT directory (existing directory succeeds); parent must exist. */
bool fs_make_directory(const char *path);
/* Read-only FAT data cluster, numbered from 2, on the selected mounted drive. */
bool fs_read_cluster(char drive, uint32_t cluster, void *data,
    uint32_t capacity, uint32_t *size);
struct book8_file_info {
    uint64_t size, allocated_bytes;
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    bool directory, read_only, system, allocation_valid, creation_valid, creation_utc;
};
/* Absolute path; allocation reflects the filesystem's stored allocation. */
bool fs_file_info(const char *path, struct book8_file_info *info);
/* FAT rename accepts 8.3 names or ASCII VFAT names when the rename fits in one
 * directory sector; deletion supports validated ASCII VFAT names. */
bool fs_rename_entry(const char *path, const char *new_name);
bool fs_delete_entry(const char *path);
/* Copies a bounded FAT tree with validated ASCII names or a file without overwriting.
 * Cut removes the source after the destination has been verified. Failure can
 * leave a partial destination or an incompletely removed source. */
bool fs_copy_to_directory(const char *source, const char *directory, bool cut);
/* FAT-only copy-on-write replacement of existing A:\boot\book8.elf.
 * The old file remains referenced until all candidate sectors verify.
 * Failure means do not reboot; a commit I/O failure may be indeterminate. */
bool fs_replace_boot_kernel(const void *data, uint32_t size);

/* Status codes reported through fs_last_error(). */
enum {
    BOOK8_FS_OK = 0,
    BOOK8_FS_ERR_BAD_PATH = -1,
    BOOK8_FS_ERR_NO_DRIVE = -2,
    BOOK8_FS_ERR_NO_VOLUME = -3,
    BOOK8_FS_ERR_NO_PARTITION = -4,
    BOOK8_FS_ERR_NOT_FOUND = -5,
    BOOK8_FS_ERR_IO = -6,
    BOOK8_FS_ERR_UNSUPPORTED = -7,
    BOOK8_FS_ERR_PROTECTED = -8,
    BOOK8_FS_ERR_EXISTS = -9,
    BOOK8_FS_ERR_NOT_EMPTY = -10,
    BOOK8_FS_ERR_CUT_PARTIAL = -11,
    BOOK8_FS_ERR_COPY_PARTIAL = -12
};

int fs_last_error(void);
const char *fs_error_text(int error);
/* Drive letters A: onward are assigned in discovery order. */
uint32_t fs_drive_count(void);
char fs_drive_letter(uint32_t index);
bool fs_drive_info(uint32_t index, const char **type, bool *readable);
/* FAT extent selected by the same drive-letter registry as root file I/O. */
bool fs_drive_extent(uint32_t index, uint32_t *disk, uint64_t *first_lba,
    uint64_t *sector_count);

enum book8_space_state {
    BOOK8_SPACE_PENDING, BOOK8_SPACE_READY, BOOK8_SPACE_UNAVAILABLE,
    BOOK8_SPACE_ERROR
};
struct book8_drive_space {
    uint64_t total_bytes, free_bytes;
    enum book8_space_state state;
    bool mounted;
};
/* Incremental FAT accounting: at most one FAT sector per poll, never writes. */
bool fs_drive_space(uint32_t index, struct book8_drive_space *space);
void fs_space_poll(void);

#endif
