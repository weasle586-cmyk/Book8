#include "boot_log.h"
#include "filesystem.h"
#include "../include/string.h"
#include "../include/stdio.h"

static char buffer[BOOK8_BOOT_LOG_CAPACITY], destination[32], error[160];
static char verify[BOOK8_BOOT_LOG_CAPACITY];
static uint32_t used, saved;
static bool enabled, mounted, busy, truncated, failed;

void book8_boot_log_begin(bool active)
{
    enabled = active; mounted = busy = truncated = failed = false;
    used = saved = 0u; destination[0] = error[0] = '\0';
}
static bool reject(const char *message)
{
    snprintf(error, sizeof(error), "%s", message); failed = true; return false;
}
bool book8_boot_log_flush(void)
{
    if (!enabled || !mounted || failed || busy) { return false; }
    if (used == saved) { return true; }
    busy = true;
    bool ok = fs_write_file(destination, buffer, used);
    if (ok) {
        uint32_t size;
        ok = fs_read_file(destination, verify, sizeof(verify), &size)
            && size == used && !memcmp(verify, buffer, used);
    }
    busy = false;
    if (!ok) {
        char message[160];
        snprintf(message, sizeof(message), "Cannot persist/verify nvinfo.txt: %s. Last successful checkpoint retained if replacement failed.",
            fs_error_text(fs_last_error()));
        return reject(message);
    }
    saved = used; return true;
}
void book8_boot_log_char(char value)
{
    if (!enabled || busy || failed || truncated) { return; }
    if (value != '\r' && value != '\n' && value != '\t' && ((uint8_t)value < 32u || (uint8_t)value >= 127u)) { return; }
    static const char notice[] = "\r\nBOOK8-NVLOG: TRUNCATED at 64 KiB; later initialization output omitted.\r\n";
    if (used >= sizeof(buffer) - sizeof(notice)) {
        memcpy(buffer + used, notice, sizeof(notice) - 1u); used += sizeof(notice) - 1u; truncated = true;
        if (mounted) { (void)book8_boot_log_flush(); }
        return;
    }
    buffer[used++] = value;
    if (mounted && value == '\n') { (void)book8_boot_log_flush(); }
}
bool book8_boot_log_mount(uint32_t first_disk)
{
    if (!enabled || failed) { return false; }
    busy = true;
    char candidate[32], marker[sizeof(BOOK8_BOOT_LOG_MARKER)];
    uint32_t matches = 0u;
    for (uint32_t i = 0; i < fs_drive_count(); ++i) {
        uint32_t disk; uint64_t first, count; struct book8_file_info info;
        if (!fs_drive_extent(i, &disk, &first, &count) || disk < first_disk) { continue; }
        snprintf(candidate, sizeof(candidate), "%c:\\NVLOG.OK", fs_drive_letter(i));
        if (!fs_file_info(candidate, &info)) {
            if (fs_last_error() != BOOK8_FS_ERR_NOT_FOUND) {
                busy = false; return reject("Cannot inspect diagnostic marker; no boot volume was written.");
            }
            continue;
        }
        uint32_t size;
        if (info.directory || info.size != sizeof(BOOK8_BOOT_LOG_MARKER) - 1u
            || !fs_read_file(candidate, marker, sizeof(marker), &size)
            || size != sizeof(BOOK8_BOOT_LOG_MARKER) - 1u
            || memcmp(marker, BOOK8_BOOT_LOG_MARKER, size)) {
            busy = false; return reject("Invalid NVLOG.OK marker. Copy the supplied marker unchanged; no boot volume was written.");
        }
        snprintf(candidate, sizeof(candidate), "%c:\\BOOT\\BOOK8.ELF", fs_drive_letter(i));
        if (!fs_file_info(candidate, &info) || info.directory || !info.size) {
            busy = false; return reject("Marked FAT volume has no BOOT\\BOOK8.ELF; refusing to write a non-BOOK8 volume.");
        }
        snprintf(destination, sizeof(destination), "%c:\\nvinfo.txt", fs_drive_letter(i));
        ++matches;
    }
    busy = false;
    if (matches != 1u) {
        destination[0] = '\0';
        return reject(matches ? "Multiple opted-in BOOK8 volumes found; remove extra NVLOG.OK markers. No volume written."
            : "No opted-in physical FAT BOOK8 volume found. Copy NVLOG.OK to its root; ISO/RAM volumes cannot retain logs.");
    }
    mounted = true; return book8_boot_log_flush();
}
bool book8_boot_log_finish(void)
{
    bool ok = book8_boot_log_flush();
    enabled = false;
    if (truncated && ok) { return reject("nvinfo.txt persisted but truncated at 64 KiB; capture serial for the complete output."); }
    return ok;
}
const char *book8_boot_log_path(void) { return destination; }
const char *book8_boot_log_error(void) { return error; }
