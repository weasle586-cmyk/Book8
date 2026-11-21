#include "../include/b8pkg.h"
#include "../include/string.h"

enum manifest_section {
    SECTION_NONE = 0,
    SECTION_PACKAGE,
    SECTION_DEPENDENCIES,
    SECTION_ASSOCIATIONS,
    SECTION_UI
};

static bool whitespace(char c)
{
    return c == ' ' || c == '\t';
}

static char *trim(char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    uint32_t start = 0u;

    while (start < length && whitespace(text[start])) {
        ++start;
    }
    while (length > start && whitespace(text[length - 1u])) {
        --length;
    }
    text[length] = '\0';
    return text + start;
}

static bool ascii_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}

static bool path_char(char c)
{
    return ascii_alnum(c) || c == '_' || c == '-' || c == '.' || c == '/';
}

static bool equal_text(const char *left, const char *right)
{
    return strcmp(left, right) == 0;
}

static bool suffix_equal(const char *text, const char *suffix)
{
    uint32_t text_length = (uint32_t)strlen(text);
    uint32_t suffix_length = (uint32_t)strlen(suffix);

    if (text_length < suffix_length) {
        return false;
    }
    text += text_length - suffix_length;
    for (uint32_t i = 0u; i < suffix_length; ++i) {
        char a = text[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') { a = (char)(a + ('a' - 'A')); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b + ('a' - 'A')); }
        if (a != b) { return false; }
    }
    return true;
}

static bool valid_name(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);

    if (length == 0u || equal_text(text, ".") || equal_text(text, "..")) {
        return false;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        char c = text[i];
        if (!ascii_alnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

static bool valid_relative_path(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    uint32_t component = 0u;

    if (length == 0u || text[0] == '/' || !suffix_equal(text, ".elf")) {
        return false;
    }
    for (uint32_t i = 0u; i <= length; ++i) {
        char c = text[i];
        if (c == '/' || c == '\0') {
            uint32_t component_length = i - component;
            if (component_length == 0u
                || (component_length == 1u && text[component] == '.')
                || (component_length == 2u && text[component] == '.'
                    && text[component + 1u] == '.')) {
                return false;
            }
            component = i + 1u;
        } else if (!path_char(c) || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

static bool valid_library(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);

    if (length == 0u || !suffix_equal(text, ".b8s")) {
        return false;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        char c = text[i];
        if (!ascii_alnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

static bool valid_extension(const char *text, char normalized[17])
{
    uint32_t length = (uint32_t)strlen(text);

    if (length < 2u || length > B8PKG_MAX_EXTENSION_SIZE || text[0] != '.') {
        return false;
    }
    normalized[0] = '.';
    for (uint32_t i = 1u; i < length; ++i) {
        char c = text[i];
        if (!ascii_alnum(c)) { return false; }
        if (c >= 'A' && c <= 'Z') { c = (char)(c + ('a' - 'A')); }
        normalized[i] = c;
    }
    normalized[length] = '\0';
    return true;
}

static bool valid_app_class(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);

    if (length == 0u || length > B8PKG_MAX_APP_CLASS_SIZE) { return false; }
    for (uint32_t i = 0u; i < length; ++i) {
        char c = text[i];
        if (!ascii_alnum(c) && c != '.' && c != '_') { return false; }
    }
    return true;
}

static bool valid_semver(const char *text)
{
    uint32_t component_start = 0u;
    uint32_t components = 0u;
    uint32_t length = (uint32_t)strlen(text);

    for (uint32_t i = 0u; i <= length; ++i) {
        if (text[i] == '.' || text[i] == '\0') {
            uint32_t component_length = i - component_start;
            if (component_length == 0u
                || (component_length > 1u && text[component_start] == '0')) {
                return false;
            }
            for (uint32_t j = component_start; j < i; ++j) {
                if (text[j] < '0' || text[j] > '9') { return false; }
            }
            ++components;
            component_start = i + 1u;
        }
    }
    return components == 3u;
}

static bool parse_uid(const char *text, uint16_t *uid)
{
    uint32_t value = 0u;

    if (*text == '\0') { return false; }
    for (; *text != '\0'; ++text) {
        uint32_t digit;
        if (*text < '0' || *text > '9') { return false; }
        digit = (uint32_t)(*text - '0');
        if (value > (65535u - digit) / 10u) { return false; }
        value = value * 10u + digit;
    }
    *uid = (uint16_t)value;
    return true;
}

static bool parse_boolean(const char *text, bool *value)
{
    if (equal_text(text, "true") || equal_text(text, "yes")
        || equal_text(text, "1")) {
        *value = true;
        return true;
    }
    if (equal_text(text, "false") || equal_text(text, "no")
        || equal_text(text, "0")) {
        *value = false;
        return true;
    }
    return false;
}

static char *strip_value(char *value)
{
    char *result = trim(value);
    uint32_t length = (uint32_t)strlen(result);

    if (length >= 2u && (result[0] == '"' || result[0] == '\'')
        && result[length - 1u] == result[0]) {
        result[length - 1u] = '\0';
        ++result;
        result = trim(result);
    }
    return result;
}

static void strip_inline_comment(char *line)
{
    char quote = '\0';

    for (uint32_t i = 0u; line[i] != '\0'; ++i) {
        if (quote != '\0') {
            if (line[i] == quote) { quote = '\0'; }
        } else if (line[i] == '"' || line[i] == '\'') {
            quote = line[i];
        } else if (line[i] == '#' || line[i] == ';') {
            line[i] = '\0';
            return;
        }
    }
}

static bool parse_list(struct b8pkg_manifest *manifest, char *value)
{
    char *item = value;

    while (true) {
        char *comma = strchr(item, ',');
        char *trimmed;
        uint32_t length;

        if (comma != NULL) { *comma = '\0'; }
        trimmed = trim(item);
        length = (uint32_t)strlen(trimmed);
        if (length == 0u || length > B8PKG_MAX_PATH_SIZE
            || manifest->library_count == B8PKG_MAX_LIST_ITEMS
            || !valid_library(trimmed)) {
            return false;
        }
        memcpy(manifest->libraries[manifest->library_count], trimmed,
            length + 1u);
        ++manifest->library_count;
        if (comma == NULL) { return true; }
        item = comma + 1u;
    }
}

int32_t b8pkg_parse(const char *data, size_t size,
    struct b8pkg_manifest *manifest)
{
    struct b8pkg_manifest parsed;
    bool package_seen = false, dependencies_seen = false;
    bool associations_seen = false, ui_seen = false;
    bool name_seen = false, version_seen = false, entry_seen = false;
    bool min_uid_seen = false, privilege_seen = false, libs_seen = false;
    bool start_menu_seen = false;
    uint32_t section_count = 0u;
    uint32_t section_keys[5] = {0u};
    enum manifest_section section = SECTION_NONE;
    size_t position = 0u;

    if (data == NULL || manifest == NULL || size == 0u
        || size > B8PKG_MAX_FILE_SIZE) {
        return -BOOK8_EINVAL;
    }
    memset(&parsed, 0, sizeof(parsed));
    parsed.min_uid = 1000u;
    parsed.privilege = B8PKG_PRIVILEGE_USER;
    parsed.start_menu = true;

    while (position < size) {
        char line[B8PKG_MAX_LINE_SIZE + 1u];
        uint32_t line_length = 0u;
        char *content;

        while (position < size && data[position] != '\n') {
            if (data[position] == '\0' || line_length == B8PKG_MAX_LINE_SIZE) {
                return -BOOK8_EINVAL;
            }
            line[line_length++] = data[position++];
        }
        if (position < size && data[position] == '\n') { ++position; }
        if (line_length != 0u && line[line_length - 1u] == '\r') {
            --line_length;
        }
        line[line_length] = '\0';
        content = trim(line);
        if (*content == '\0') { continue; }
        if (*content == '#' || *content == ';') { continue; }

        if (*content == '[') {
            char *end = strchr(content, ']');
            char *section_name;
            if (end == NULL) { return -BOOK8_EINVAL; }
            *end = '\0';
            section_name = trim(content + 1u);
            if (end[1] != '\0' || section_count == B8PKG_MAX_SECTIONS) {
                return -BOOK8_EINVAL;
            }
            ++section_count;
            if (equal_text(section_name, "Package")) {
                if (package_seen) { return -BOOK8_EINVAL; }
                package_seen = true; section = SECTION_PACKAGE;
            } else if (equal_text(section_name, "Dependencies")) {
                if (dependencies_seen) { return -BOOK8_EINVAL; }
                dependencies_seen = true; section = SECTION_DEPENDENCIES;
            } else if (equal_text(section_name, "Associations")) {
                if (associations_seen) { return -BOOK8_EINVAL; }
                associations_seen = true; section = SECTION_ASSOCIATIONS;
            } else if (equal_text(section_name, "UI")) {
                if (ui_seen) { return -BOOK8_EINVAL; }
                ui_seen = true; section = SECTION_UI;
            } else {
                return -BOOK8_EINVAL;
            }
            continue;
        }

        strip_inline_comment(content);
        content = trim(content);
        if (*content == '\0' || section == SECTION_NONE) {
            return -BOOK8_EINVAL;
        }
        char *equals = strchr(content, '=');
        if (equals == NULL) { return -BOOK8_EINVAL; }
        *equals = '\0';
        char *key = trim(content);
        char *value = strip_value(equals + 1u);
        uint32_t value_length = (uint32_t)strlen(value);
        uint32_t section_index = (uint32_t)section;

        if (*key == '\0' || value_length > B8PKG_MAX_VALUE_SIZE
            || ++section_keys[section_index] > B8PKG_MAX_KEYS_PER_SECTION) {
            return -BOOK8_EINVAL;
        }

        if (section == SECTION_PACKAGE) {
            if (equal_text(key, "name")) {
                if (name_seen || value_length > B8PKG_MAX_NAME_SIZE
                    || !valid_name(value)) { return -BOOK8_EINVAL; }
                name_seen = true;
                memcpy(parsed.name, value, value_length + 1u);
            } else if (equal_text(key, "version")) {
                if (version_seen || value_length > B8PKG_MAX_VERSION_SIZE
                    || !valid_semver(value)) { return -BOOK8_EINVAL; }
                version_seen = true;
                memcpy(parsed.version, value, value_length + 1u);
            } else if (equal_text(key, "entry")) {
                if (entry_seen || value_length > B8PKG_MAX_PATH_SIZE
                    || !valid_relative_path(value)) { return -BOOK8_EINVAL; }
                entry_seen = true;
                memcpy(parsed.entry, value, value_length + 1u);
            } else if (equal_text(key, "min_uid")) {
                if (min_uid_seen || !parse_uid(value, &parsed.min_uid)) {
                    return -BOOK8_EINVAL;
                }
                min_uid_seen = true;
            } else if (equal_text(key, "privilege")) {
                if (privilege_seen) { return -BOOK8_EINVAL; }
                if (equal_text(value, "user")) {
                    parsed.privilege = B8PKG_PRIVILEGE_USER;
                } else if (equal_text(value, "system")) {
                    parsed.privilege = B8PKG_PRIVILEGE_SYSTEM;
                } else if (equal_text(value, "root")) {
                    parsed.privilege = B8PKG_PRIVILEGE_ROOT;
                } else {
                    return -BOOK8_EINVAL;
                }
                privilege_seen = true;
            } else {
                return -BOOK8_EINVAL;
            }
        } else if (section == SECTION_DEPENDENCIES) {
            if (!equal_text(key, "libs") || libs_seen
                || !parse_list(&parsed, value)) { return -BOOK8_EINVAL; }
            libs_seen = true;
        } else if (section == SECTION_ASSOCIATIONS) {
            struct b8pkg_association *association;
            char extension[B8PKG_MAX_EXTENSION_SIZE + 1u];
            if (!valid_extension(key, extension)
                || !valid_app_class(value)
                || parsed.association_count == B8PKG_MAX_ASSOCIATIONS) {
                return -BOOK8_EINVAL;
            }
            for (uint32_t i = 0u; i < parsed.association_count; ++i) {
                if (equal_text(parsed.associations[i].extension, extension)) {
                    return -BOOK8_EINVAL;
                }
            }
            association = &parsed.associations[parsed.association_count++];
            memcpy(association->extension, extension, strlen(extension) + 1u);
            memcpy(association->app_class, value, value_length + 1u);
        } else if (section == SECTION_UI) {
            if (!equal_text(key, "start_menu") || start_menu_seen
                || !parse_boolean(value, &parsed.start_menu)) {
                return -BOOK8_EINVAL;
            }
            start_menu_seen = true;
        }
    }

    if (!package_seen || !name_seen || !version_seen || !entry_seen
        || ((parsed.privilege == B8PKG_PRIVILEGE_ROOT) && parsed.min_uid != 0u)
        || ((parsed.privilege == B8PKG_PRIVILEGE_SYSTEM)
            && (parsed.min_uid == 0u || parsed.min_uid >= 1000u))
        || ((parsed.privilege == B8PKG_PRIVILEGE_USER)
            && parsed.min_uid < 1000u)) {
        return -BOOK8_EINVAL;
    }
    *manifest = parsed;
    return 0;
}
