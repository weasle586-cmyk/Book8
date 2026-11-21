#ifndef BOOK8_B8PKG_H
#define BOOK8_B8PKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "syscall.h"

#define B8PKG_MAX_FILE_SIZE 8192u
#define B8PKG_MAX_LINE_SIZE 256u
#define B8PKG_MAX_SECTIONS 16u
#define B8PKG_MAX_KEYS_PER_SECTION 64u
#define B8PKG_MAX_VALUE_SIZE 256u
#define B8PKG_MAX_LIST_ITEMS 16u
#define B8PKG_MAX_ASSOCIATIONS B8PKG_MAX_KEYS_PER_SECTION
#define B8PKG_MAX_PATH_SIZE B8PKG_MAX_VALUE_SIZE
#define B8PKG_MAX_NAME_SIZE B8PKG_MAX_VALUE_SIZE
#define B8PKG_MAX_VERSION_SIZE B8PKG_MAX_VALUE_SIZE
#define B8PKG_MAX_APP_CLASS_SIZE 64u
#define B8PKG_MAX_EXTENSION_SIZE 16u

enum b8pkg_privilege {
    B8PKG_PRIVILEGE_USER = 0u,
    B8PKG_PRIVILEGE_SYSTEM,
    B8PKG_PRIVILEGE_ROOT
};

struct b8pkg_association {
    char extension[B8PKG_MAX_EXTENSION_SIZE + 1u];
    char app_class[B8PKG_MAX_APP_CLASS_SIZE + 1u];
};

struct b8pkg_manifest {
    char name[B8PKG_MAX_NAME_SIZE + 1u];
    char version[B8PKG_MAX_VERSION_SIZE + 1u];
    char entry[B8PKG_MAX_PATH_SIZE + 1u];
    uint16_t min_uid;
    enum b8pkg_privilege privilege;
    bool start_menu;
    uint32_t library_count;
    char libraries[B8PKG_MAX_LIST_ITEMS][B8PKG_MAX_PATH_SIZE + 1u];
    uint32_t association_count;
    struct b8pkg_association associations[B8PKG_MAX_ASSOCIATIONS];
};

/* Parses [Package], optional [Dependencies], [Associations], and [UI] sections.
 * Paths are package-relative; supported values are schema-validated. On
 * failure, *manifest is unchanged and a negative BOOK8 errno is returned. */
int32_t b8pkg_parse(const char *data, size_t size,
    struct b8pkg_manifest *manifest);

#endif
