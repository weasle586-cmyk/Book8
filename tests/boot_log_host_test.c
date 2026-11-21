#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/boot_log.h"
#include "../src/filesystem.h"
static uint32_t writes, size, markers = 1u;
static char stored[BOOK8_BOOT_LOG_CAPACITY], path[32];
static int error_code;
static bool fail_write, corrupt, bad_marker, no_kernel;
uint32_t fs_drive_count(void) { return 3u; }
char fs_drive_letter(uint32_t i) { return (char)('A' + i); }
bool fs_drive_extent(uint32_t i, uint32_t *disk, uint64_t *first, uint64_t *count)
{ *disk = i; *first = 0; *count = 10000u; return true; }
bool fs_file_info(const char *name, struct book8_file_info *info)
{
    bool marker = strstr(name, "NVLOG.OK") != NULL;
    bool exists = name[0] == 'B' || (name[0] == 'C' && markers == 2u);
    if ((marker && (!markers || !exists)) || (!marker && no_kernel)) { error_code = BOOK8_FS_ERR_NOT_FOUND; return false; }
    *info = (struct book8_file_info){.size = marker ? sizeof(BOOK8_BOOT_LOG_MARKER) - 1u : 1234u};
    return true;
}
bool fs_read_file(const char *name, void *out, uint32_t capacity, uint32_t *length)
{
    if (strstr(name, "NVLOG.OK")) {
        assert(capacity >= sizeof(BOOK8_BOOT_LOG_MARKER));
        *length = sizeof(BOOK8_BOOT_LOG_MARKER) - 1u; memcpy(out, BOOK8_BOOT_LOG_MARKER, *length);
        if (bad_marker) { ((char *)out)[0] = 'X'; }
    } else { assert(capacity >= size); *length = size; memcpy(out, stored, size); if (corrupt && size) { ((char *)out)[0] ^= 1; } }
    return true;
}
bool fs_write_file(const char *name, const void *data, uint32_t length)
{
    assert(length <= sizeof(stored)); ++writes;
    book8_boot_log_char('R'); book8_boot_log_char('\n');
    if (fail_write) { error_code = BOOK8_FS_ERR_IO; return false; }
    strcpy(path, name); memcpy(stored, data, length); size = length; return true;
}
int fs_last_error(void) { return error_code; }
const char *fs_error_text(int code) { (void)code; return "disk I/O failed"; }
static void emit(const char *text) { while (*text) { book8_boot_log_char(*text++); } }
int main(void)
{
    book8_boot_log_begin(false); emit("normal boot\n"); assert(!book8_boot_log_mount(1u) && !writes);
    book8_boot_log_begin(true); emit("BOOK8-BOOT: entered\r\nPCIE1 INIT\r\n");
    assert(!writes); assert(book8_boot_log_mount(1u)); assert(!strcmp(path, "B:\\nvinfo.txt"));
    uint32_t checkpoint = writes; emit("BOOK8-NV: chipset "); assert(writes == checkpoint);
    emit("GM204\r\n"); assert(writes == checkpoint + 1u);
    assert(size == strlen("BOOK8-BOOT: entered\r\nPCIE1 INIT\r\nBOOK8-NV: chipset GM204\r\n"));
    assert(!memcmp(stored, "BOOK8-BOOT: entered\r\nPCIE1 INIT\r\nBOOK8-NV: chipset GM204\r\n", size));
    assert(book8_boot_log_finish()); emit("shell/password\n"); assert(writes == checkpoint + 1u);
    checkpoint = writes; markers = 0u; book8_boot_log_begin(true); emit("early\n");
    assert(!book8_boot_log_mount(1u) && writes == checkpoint && strstr(book8_boot_log_error(), "No opted-in"));
    markers = 2u; book8_boot_log_begin(true); assert(!book8_boot_log_mount(1u) && writes == checkpoint);
    markers = 1u; bad_marker = true; book8_boot_log_begin(true); assert(!book8_boot_log_mount(1u) && writes == checkpoint);
    bad_marker = false; no_kernel = true; book8_boot_log_begin(true); assert(!book8_boot_log_mount(1u) && writes == checkpoint);
    no_kernel = false; book8_boot_log_begin(true); emit("early\n"); assert(book8_boot_log_mount(1u));
    fail_write = true; emit("next\n"); assert(strstr(book8_boot_log_error(), "persist/verify")); assert(!book8_boot_log_finish());
    fail_write = false; corrupt = true; book8_boot_log_begin(true); emit("early\n"); assert(!book8_boot_log_mount(1u));
    corrupt = false; book8_boot_log_begin(true);
    for (uint32_t i = 0; i < BOOK8_BOOT_LOG_CAPACITY * 2u; ++i) { book8_boot_log_char('x'); }
    assert(book8_boot_log_mount(1u)); assert(size <= BOOK8_BOOT_LOG_CAPACITY && strstr(stored, "TRUNCATED"));
    assert(!book8_boot_log_finish() && strstr(book8_boot_log_error(), "truncated"));
    puts("PASS: boot capture, exact CRLF logs, line checkpoints, RAM exclusion, marker guards, errors, reentrancy and truncation");
    return 0;
}
