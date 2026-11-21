#ifndef BOOK8_B8_SHLIB_H
#define BOOK8_B8_SHLIB_H

#include <stdint.h>

#define B8_SHLIB_ABI_VERSION 1u
#define B8_SHLIB_MAGIC "B8S1"
#define B8_SHLIB_MAGIC_SIZE 4u
#define B8_SHLIB_EXPORT_SECTION ".b8s_exports"
#define B8_SHLIB_IMPORT_SECTION ".b8s_imports"
#define B8_SHLIB_MAX_SYMBOLS 1024u
#define B8_SYM_NAME_SIZE 64u

enum b8_sym_type {
    B8_SYM_FUNC = 1u,
    B8_SYM_DATA = 2u
};

typedef struct __attribute__((packed)) b8_sym_table_header {
    char magic[B8_SHLIB_MAGIC_SIZE];
    uint32_t abi_version;
    uint32_t count;
} b8_sym_table_header_t;

typedef struct __attribute__((packed)) b8_sym_export_record {
    char name[B8_SYM_NAME_SIZE];
    /* Module-relative virtual address (ELF st_value), not a runtime pointer. */
    uint64_t value;
    uint32_t type;
    uint32_t version_major;
    uint32_t version_minor;
} b8_sym_export_record_t;

typedef struct b8_sym_export {
    char name[B8_SYM_NAME_SIZE];
    void *ptr;
    uint32_t type;
    uint32_t version_major;
    uint32_t version_minor;
} b8_sym_export_t;

typedef struct __attribute__((packed)) b8_sym_import {
    char name[B8_SYM_NAME_SIZE];
    /* A provider must match the major and meet or exceed the minor version. */
    uint32_t version_major;
    uint32_t version_minor;
} b8_sym_import_t;

#endif
