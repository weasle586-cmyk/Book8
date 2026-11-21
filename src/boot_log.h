#ifndef BOOK8_BOOT_LOG_H
#define BOOK8_BOOT_LOG_H
#include <stdbool.h>
#include <stdint.h>
#define BOOK8_BOOT_LOG_CAPACITY (64u * 1024u)
#define BOOK8_BOOT_LOG_MARKER "BOOK8-NVLOG-1\r\n"
void book8_boot_log_begin(bool enabled);
void book8_boot_log_char(char value);
bool book8_boot_log_mount(uint32_t first_disk);
bool book8_boot_log_flush(void);
bool book8_boot_log_finish(void);
const char *book8_boot_log_path(void);
const char *book8_boot_log_error(void);
#endif
