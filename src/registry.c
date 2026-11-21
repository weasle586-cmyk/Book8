#include "registry.h"

#include "../include/stdio.h"

#define REGISTRY_MAX_ENTRIES 10240u
#define REGISTRY_FILE_CAPACITY (1024u * 1024u)

struct registry_entry {
    char path[BOOK8_REGISTRY_KEY_MAX];
    char name[BOOK8_REGISTRY_NAME_MAX];
    char value[BOOK8_REGISTRY_VALUE_MAX];
};

static struct registry_entry entries[REGISTRY_MAX_ENTRIES];
static char enum_seen[1024u][BOOK8_REGISTRY_NAME_MAX];
static size_t entry_count;
static char file_buffer[REGISTRY_FILE_CAPACITY];
static reg_read_file_fn registry_read_file;
static reg_write_file_fn registry_write_file;

static char normalize_char(char value)
{
    if (value == '/') {
        return '\\';
    }
    if (value >= 'a' && value <= 'z') {
        return (char)(value - ('a' - 'A'));
    }
    return value;
}

static bool equal_text(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (normalize_char(*left++) != normalize_char(*right++)) {
            return false;
        }
    }
    return *left == *right;
}

static bool valid_text(const char *text, size_t limit, bool path)
{
    size_t length = 0u;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    while (text[length] != '\0') {
        unsigned char value = (unsigned char)text[length];
        if (++length >= limit || value < 32u || value > 126u
            || value == '=' || (path && value == '\n')) {
            return false;
        }
    }
    return true;
}

static bool valid_value(const char *value)
{
    if (value == NULL) {
        return false;
    }
    for (size_t i = 0u; value[i] != '\0'; ++i) {
        unsigned char character = (unsigned char)value[i];
        if (i + 1u >= BOOK8_REGISTRY_VALUE_MAX || character < 32u
            || character > 126u) {
            return false;
        }
    }
    return true;
}

static struct registry_entry *find_entry(const char *path, const char *name)
{
    for (size_t i = 0u; i < entry_count; ++i) {
        if (equal_text(entries[i].path, path)
            && equal_text(entries[i].name, name)) {
            return &entries[i];
        }
    }
    return NULL;
}

static void copy_normalized(char *destination, size_t capacity,
    const char *source)
{
    size_t index = 0u;
    while (index + 1u < capacity && source[index] != '\0') {
        char character = source[index];
        destination[index] = character == '/' ? '\\' : character;
        ++index;
    }
    destination[index] = '\0';
}

void reg_set_storage(reg_read_file_fn read_file, reg_write_file_fn write_file)
{
    registry_read_file = read_file;
    registry_write_file = write_file;
}

bool reg_set_str(const char *path, const char *name, const char *value)
{
    struct registry_entry *entry;

    if (!valid_text(path, BOOK8_REGISTRY_KEY_MAX, true)
        || !valid_text(name, BOOK8_REGISTRY_NAME_MAX, false)
        || !valid_value(value)) {
        return false;
    }
    entry = find_entry(path, name);
    if (entry == NULL) {
        if (entry_count == REGISTRY_MAX_ENTRIES) {
            return false;
        }
        entry = &entries[entry_count++];
        copy_normalized(entry->path, sizeof(entry->path), path);
        copy_normalized(entry->name, sizeof(entry->name), name);
    }
    copy_normalized(entry->value, sizeof(entry->value), value);
    return true;
}

bool reg_set_u32(const char *path, const char *name, uint32_t value)
{
    char text[BOOK8_REGISTRY_VALUE_MAX];
    int length = snprintf(text, sizeof(text), "%u", value);
    return length > 0 && (size_t)length < sizeof(text)
        && reg_set_str(path, name, text);
}

bool reg_get_str(const char *path, const char *name, char *value,
    size_t capacity)
{
    struct registry_entry *entry;
    size_t length = 0u;

    if (value == NULL || capacity == 0u) {
        return false;
    }
    entry = find_entry(path, name);
    if (entry == NULL) {
        return false;
    }
    while (entry->value[length] != '\0') {
        ++length;
    }
    if (length >= capacity) {
        return false;
    }
    for (size_t i = 0u; i <= length; ++i) {
        value[i] = entry->value[i];
    }
    return true;
}

bool reg_get_u32(const char *path, const char *name, uint32_t *value)
{
    char text[BOOK8_REGISTRY_VALUE_MAX];
    uint32_t result = 0u;
    uint32_t base = 10u;
    size_t index = 0u;

    if (value == NULL || !reg_get_str(path, name, text, sizeof(text))) {
        return false;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16u;
        index = 2u;
    }
    if (text[index] == '\0') {
        return false;
    }
    for (; text[index] != '\0'; ++index) {
        uint32_t digit;
        char character = text[index];
        if (character >= '0' && character <= '9') {
            digit = (uint32_t)(character - '0');
        } else if (base == 16u && character >= 'a' && character <= 'f') {
            digit = (uint32_t)(character - 'a') + 10u;
        } else if (base == 16u && character >= 'A' && character <= 'F') {
            digit = (uint32_t)(character - 'A') + 10u;
        } else {
            return false;
        }
        if (digit >= base || result > (UINT32_MAX - digit) / base) {
            return false;
        }
        result = result * base + digit;
    }
    *value = result;
    return true;
}

static bool path_prefix(const char *prefix, const char *path)
{
    while (*prefix != '\0') {
        if (*path == '\0'
            || normalize_char(*prefix++) != normalize_char(*path++)) {
            return false;
        }
    }
    return *path == '\0' || *path == '\\' || *path == '/';
}

size_t reg_enum_keys(const char *path, reg_enum_fn visitor, void *context)
{
    size_t seen_count = 0u;
    size_t path_length = 0u;

    if (!valid_text(path, BOOK8_REGISTRY_KEY_MAX, true)) {
        return 0u;
    }
    while (path[path_length] != '\0') {
        ++path_length;
    }
    for (size_t i = 0u; i < entry_count; ++i) {
        const char *child;
        size_t child_length = 0u;
        bool duplicate = false;

        if (!path_prefix(path, entries[i].path)
            || entries[i].path[path_length] == '\0') {
            continue;
        }
        child = entries[i].path + path_length + 1u;
        while (child[child_length] != '\0' && child[child_length] != '\\') {
            ++child_length;
        }
        if (child_length == 0u || child_length >= BOOK8_REGISTRY_NAME_MAX) {
            continue;
        }
        for (size_t prior = 0u; prior < seen_count; ++prior) {
            if (equal_text(enum_seen[prior], child)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || seen_count == sizeof(enum_seen) / sizeof(enum_seen[0])) {
            continue;
        }
        for (size_t j = 0u; j < child_length; ++j) {
            enum_seen[seen_count][j] = child[j];
        }
        enum_seen[seen_count][child_length] = '\0';
        ++seen_count;
        if (visitor != NULL && !visitor(enum_seen[seen_count - 1u], context)) {
            break;
        }
    }
    return seen_count;
}

size_t reg_enum_values(const char *path, reg_enum_fn visitor, void *context)
{
    size_t count = 0u;

    if (!valid_text(path, BOOK8_REGISTRY_KEY_MAX, true)) {
        return 0u;
    }
    for (size_t i = 0u; i < entry_count; ++i) {
        if (!equal_text(entries[i].path, path)) {
            continue;
        }
        ++count;
        if (visitor != NULL && !visitor(entries[i].name, context)) {
            break;
        }
    }
    return count;
}

bool reg_delete_tree(const char *path)
{
    size_t retained = 0u;
    bool removed = false;

    if (!valid_text(path, BOOK8_REGISTRY_KEY_MAX, true)) {
        return false;
    }
    for (size_t i = 0u; i < entry_count; ++i) {
        if (path_prefix(path, entries[i].path)) {
            removed = true;
            continue;
        }
        if (retained != i) {
            entries[retained] = entries[i];
        }
        ++retained;
    }
    entry_count = retained;
    return removed;
}

bool reg_save(void)
{
    size_t used = 0u;

    if (registry_write_file == NULL) {
        return false;
    }
    for (size_t i = 0u; i < entry_count; ++i) {
        const struct registry_entry *entry = &entries[i];
        const char *parts[] = {entry->path, "\\", entry->name, "=",
            entry->value, "\n"};
        for (size_t part = 0u; part < sizeof(parts) / sizeof(parts[0]); ++part) {
            size_t length = 0u;
            while (parts[part][length] != '\0') {
                ++length;
            }
            if (length > sizeof(file_buffer) - used) {
                return false;
            }
            for (size_t character = 0u; character < length; ++character) {
                file_buffer[used++] = parts[part][character];
            }
        }
    }
    return registry_write_file(BOOK8_REGISTRY_FILE, file_buffer,
        (uint32_t)used);
}

static bool validate_line(const char *line, size_t length)
{
    size_t equals = length;
    size_t separator = length;

    if (length == 0u || line[0] == '#') {
        return true;
    }
    for (size_t i = 0u; i < length; ++i) {
        if (line[i] == '=' && equals == length) {
            equals = i;
        }
    }
    if (equals == length || equals == 0u) {
        return false;
    }
    for (size_t i = 0u; i < equals; ++i) {
        if (line[i] == '\\' || line[i] == '/') {
            separator = i;
        }
        if ((unsigned char)line[i] < 32u || (unsigned char)line[i] > 126u) {
            return false;
        }
    }
    for (size_t i = equals + 1u; i < length; ++i) {
        if ((unsigned char)line[i] < 32u || (unsigned char)line[i] > 126u) {
            return false;
        }
    }
    return separator != length && separator != 0u
        && equals - separator - 1u < BOOK8_REGISTRY_NAME_MAX
        && equals - separator - 1u != 0u
        && separator < BOOK8_REGISTRY_KEY_MAX
        && length - equals - 1u < BOOK8_REGISTRY_VALUE_MAX;
}

bool reg_load(void)
{
    uint32_t size = 0u;
    size_t line_start = 0u;
    size_t lines = 0u;

    if (registry_read_file == NULL
        || !registry_read_file(BOOK8_REGISTRY_FILE, file_buffer,
            sizeof(file_buffer), &size)
        || size > sizeof(file_buffer)) {
        return false;
    }
    for (size_t i = 0u; i <= size; ++i) {
        if (i != size && file_buffer[i] != '\n') {
            continue;
        }
        size_t length = i - line_start;
        if (length != 0u && file_buffer[line_start + length - 1u] == '\r') {
            --length;
        }
        if (!validate_line(file_buffer + line_start, length)
            || (length != 0u && file_buffer[line_start] != '#'
                && ++lines > REGISTRY_MAX_ENTRIES)) {
            return false;
        }
        line_start = i + 1u;
    }
    entry_count = 0u;
    line_start = 0u;
    for (size_t i = 0u; i <= size; ++i) {
        if (i != size && file_buffer[i] != '\n') {
            continue;
        }
        size_t length = i - line_start;
        if (length != 0u && file_buffer[line_start + length - 1u] == '\r') {
            --length;
        }
        if (length != 0u && file_buffer[line_start] != '#') {
            size_t equals = 0u;
            size_t separator = 0u;
            char path[BOOK8_REGISTRY_KEY_MAX];
            char name[BOOK8_REGISTRY_NAME_MAX];
            char value[BOOK8_REGISTRY_VALUE_MAX];
            while (equals < length && file_buffer[line_start + equals] != '=') {
                if (file_buffer[line_start + equals] == '\\'
                    || file_buffer[line_start + equals] == '/') {
                    separator = equals;
                }
                ++equals;
            }
            size_t path_length = separator;
            size_t name_length = equals - separator - 1u;
            size_t value_length = length - equals - 1u;
            for (size_t j = 0u; j < path_length; ++j) {
                path[j] = file_buffer[line_start + j];
            }
            path[path_length] = '\0';
            for (size_t j = 0u; j < name_length; ++j) {
                name[j] = file_buffer[line_start + separator + 1u + j];
            }
            name[name_length] = '\0';
            for (size_t j = 0u; j < value_length; ++j) {
                value[j] = file_buffer[line_start + equals + 1u + j];
            }
            value[value_length] = '\0';
            if (!reg_set_str(path, name, value)) {
                entry_count = 0u;
                return false;
            }
        }
        line_start = i + 1u;
    }
    return true;
}
