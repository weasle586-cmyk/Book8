#ifndef BOOK8_REGISTRY_H
#define BOOK8_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK8_REGISTRY_FILE "A:\\BOOK8REG.INI"
#define BOOK8_REGISTRY_KEY_MAX 96u
#define BOOK8_REGISTRY_NAME_MAX 32u
#define BOOK8_REGISTRY_VALUE_MAX 96u

typedef bool (*reg_read_file_fn)(const char *, void *, uint32_t, uint32_t *);
typedef bool (*reg_write_file_fn)(const char *, const void *, uint32_t);
typedef bool (*reg_enum_fn)(const char *, void *);

void reg_set_storage(reg_read_file_fn read_file, reg_write_file_fn write_file);
bool reg_set_str(const char *path, const char *name, const char *value);
bool reg_set_u32(const char *path, const char *name, uint32_t value);
bool reg_get_str(const char *path, const char *name, char *value,
    size_t capacity);
bool reg_get_u32(const char *path, const char *name, uint32_t *value);
size_t reg_enum_keys(const char *path, reg_enum_fn visitor, void *context);
size_t reg_enum_values(const char *path, reg_enum_fn visitor, void *context);
bool reg_delete_tree(const char *path);
bool reg_save(void);
bool reg_load(void);

#endif
