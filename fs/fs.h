#ifndef BOOK8_FS_H
#define BOOK8_FS_H

#include <stdbool.h>
#include <stdint.h>
#include "../drivers/block.h"

/* Read-only FAT16/FAT32 (8.3 names, subdirectories, MBR or bare volume).
 * Paths use '/' or '\' and are case-insensitive. A registered drive resolver
 * shares the shell's volume selection, including EFI/GPT FAT partitions. */

struct fs_extent {
    block_device_t *dev;
    uint32_t first_lba, sector_count;
};
/* drive is A=0..Z=25, or UINT32_MAX for the shell's current drive. */
typedef bool (*fs_drive_resolver)(uint32_t drive, struct fs_extent *extent);
void fs_set_drive_resolver(fs_drive_resolver resolver);

typedef struct file_handle {
    bool used;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t offset;
    uint32_t cur_cluster; /* cluster containing cur_index */
    uint32_t cur_index;   /* cluster index within the file chain */
} file_handle_t;

#define FS_MAX_OPEN 16u

/* Mount the first FAT volume found on dev (superfloppy or MBR partition).
 * Replaces any earlier mount; open handles are invalidated. */
bool fs_mount(block_device_t *dev);
bool fs_is_mounted(void);

/* Returns a handle or NULL. Falls back to mounting every block device in
 * turn when the current mount does not contain path. */
file_handle_t *fs_open(const char *path);
file_handle_t *fs_open_any(const char *path);

/* Sequential read; returns bytes read (0 at EOF) or -1 on I/O error. */
int32_t fs_read(file_handle_t *file, void *buf, uint32_t count);
bool fs_seek(file_handle_t *file, uint32_t offset);
void fs_close(file_handle_t *file);

#endif
