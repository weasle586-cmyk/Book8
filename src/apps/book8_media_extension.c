#include "book8_media_extension.h"

#include "../../include/stdio.h"
#include "../../include/string.h"

enum media_container {
    MEDIA_CONTAINER_NONE,
    MEDIA_CONTAINER_AUDIO,
    MEDIA_CONTAINER_VIDEO
};

static char lower_ascii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch + ('a' - 'A')) : ch;
}

static bool equal_ascii(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (lower_ascii(*left++) != lower_ascii(*right++)) {
            return false;
        }
    }
    return *left == '\0' && *right == '\0';
}

static bool ends_with_ascii(const char *text, const char *suffix)
{
    size_t text_size = strlen(text);
    size_t suffix_size = strlen(suffix);
    if (text_size < suffix_size) {
        return false;
    }
    text += text_size - suffix_size;
    while (*suffix != '\0') {
        if (lower_ascii(*text++) != lower_ascii(*suffix++)) {
            return false;
        }
    }
    return true;
}

static bool tag_name_is(const char *tag, uint32_t length, const char *name,
    bool closing)
{
    uint32_t at = closing ? 1u : 0u;
    if (closing && (length == 0u || tag[0] != '/')) {
        return false;
    }
    for (uint32_t i = 0u; name[i] != '\0'; ++i) {
        if (at + i >= length
            || lower_ascii(tag[at + i]) != lower_ascii(name[i])) {
            return false;
        }
    }
    at += (uint32_t)strlen(name);
    return at == length || tag[at] == ' ' || tag[at] == '\t'
        || tag[at] == '/';
}

enum book8_media_kind book8_media_classify_url(const char *url)
{
    if (url == NULL) {
        return BOOK8_MEDIA_OTHER;
    }
    char path[BOOK8_MEDIA_URL_CAPACITY];
    size_t length = 0u;
    while (url[length] != '\0' && url[length] != '?' && url[length] != '#'
        && length + 1u < sizeof(path)) {
        path[length] = url[length];
        ++length;
    }
    path[length] = '\0';
    if (ends_with_ascii(path, ".wav") || ends_with_ascii(path, ".wave")) {
        return BOOK8_MEDIA_WAV;
    }
    if (ends_with_ascii(path, ".mp3") || ends_with_ascii(path, ".ogg")
        || ends_with_ascii(path, ".oga") || ends_with_ascii(path, ".flac")
        || ends_with_ascii(path, ".aac") || ends_with_ascii(path, ".m4a")) {
        return BOOK8_MEDIA_UNSUPPORTED_AUDIO;
    }
    if (ends_with_ascii(path, ".mp4") || ends_with_ascii(path, ".webm")
        || ends_with_ascii(path, ".mov") || ends_with_ascii(path, ".avi")
        || ends_with_ascii(path, ".mkv") || ends_with_ascii(path, ".mpeg")) {
        return BOOK8_MEDIA_VIDEO;
    }
    return BOOK8_MEDIA_OTHER;
}

static bool tag_attribute(const char *tag, uint32_t length,
    const char *name, char *value, uint32_t capacity)
{
    uint32_t at = 0u;
    while (at < length && tag[at] != ' ' && tag[at] != '\t') {
        ++at;
    }
    while (at < length) {
        while (at < length
            && (tag[at] == ' ' || tag[at] == '\t' || tag[at] == '/')) {
            ++at;
        }
        uint32_t begin = at;
        while (at < length && tag[at] != '=' && tag[at] != ' '
            && tag[at] != '\t' && tag[at] != '/') {
            ++at;
        }
        uint32_t name_length = at - begin;
        if (name_length == 0u) {
            break;
        }
        bool matches = strlen(name) == name_length;
        for (uint32_t i = 0u; matches && i < name_length; ++i) {
            matches = lower_ascii(tag[begin + i]) == lower_ascii(name[i]);
        }
        while (at < length && (tag[at] == ' ' || tag[at] == '\t')) {
            ++at;
        }
        if (at == length || tag[at++] != '=') {
            continue;
        }
        while (at < length && (tag[at] == ' ' || tag[at] == '\t')) {
            ++at;
        }
        char quote = tag[at] == '"' || tag[at] == '\'' ? tag[at++] : '\0';
        uint32_t value_begin = at;
        while (at < length && (quote != '\0'
            ? tag[at] != quote : tag[at] != ' ' && tag[at] != '\t')) {
            ++at;
        }
        uint32_t value_size = at - value_begin;
        if (quote == '\0' && value_size != 0u
            && tag[value_begin + value_size - 1u] == '/'
            && at == length) {
            --value_size;
        }
        if (matches) {
            if (value_size == 0u || value_size >= capacity) {
                return false;
            }
            memcpy(value, tag + value_begin, value_size);
            value[value_size] = '\0';
            return true;
        }
        if (quote != '\0' && at < length) {
            ++at;
        }
    }
    return false;
}

static enum book8_media_kind classify_type(const char *type)
{
    if (type == NULL) {
        return BOOK8_MEDIA_OTHER;
    }
    char mime[64];
    uint32_t length = 0u;
    while (type[length] != '\0' && type[length] != ';'
        && type[length] != ' ' && type[length] != '\t'
        && length + 1u < sizeof(mime)) {
        mime[length] = lower_ascii(type[length]);
        ++length;
    }
    mime[length] = '\0';
    if (equal_ascii(mime, "audio/wav") || equal_ascii(mime, "audio/wave")
        || equal_ascii(mime, "audio/x-wav")) {
        return BOOK8_MEDIA_WAV;
    }
    if (strncmp(mime, "audio/", 6u) == 0) {
        return BOOK8_MEDIA_UNSUPPORTED_AUDIO;
    }
    if (strncmp(mime, "video/", 6u) == 0) {
        return BOOK8_MEDIA_VIDEO;
    }
    return BOOK8_MEDIA_OTHER;
}

static bool emit_source(const char *url, const char *type,
    enum book8_media_kind context_kind, book8_media_visitor visitor,
    void *context)
{
    if (url == NULL || url[0] == '\0' || visitor == NULL) {
        return true;
    }
    struct book8_media_source source = {0};
    source.kind = book8_media_classify_url(url);
    enum book8_media_kind type_kind = classify_type(type);
    if (source.kind == BOOK8_MEDIA_OTHER) {
        source.kind = type_kind != BOOK8_MEDIA_OTHER
            ? type_kind : context_kind;
    }
    if (source.kind == BOOK8_MEDIA_OTHER) {
        return true;
    }
    snprintf(source.url, sizeof(source.url), "%s", url);
    if (source.kind == BOOK8_MEDIA_WAV) {
        snprintf(source.label, sizeof(source.label), "[Audio WAV] %.64s", url);
    } else if (source.kind == BOOK8_MEDIA_VIDEO) {
        snprintf(source.label, sizeof(source.label), "[Video unsupported] %.56s",
            url);
    } else {
        snprintf(source.label, sizeof(source.label), "[Audio unsupported] %.58s",
            url);
    }
    return visitor(context, &source);
}

uint32_t book8_media_scan_html(const char *html, uint32_t size,
    book8_media_visitor visitor, void *context)
{
    if (html == NULL || visitor == NULL) {
        return 0u;
    }
    uint32_t found = 0u;
    enum media_container container = MEDIA_CONTAINER_NONE;
    bool skip_script = false;
    for (uint32_t i = 0u; i < size;) {
        if (html[i] != '<') {
            ++i;
            continue;
        }
        uint32_t end = i + 1u;
        while (end < size && html[end] != '>') {
            ++end;
        }
        if (end == size) {
            break;
        }
        const char *tag = html + i + 1u;
        uint32_t length = end - i - 1u;
        if (tag_name_is(tag, length, "script", true)
            || tag_name_is(tag, length, "style", true)) {
            skip_script = false;
        } else if (tag_name_is(tag, length, "script", false)
            || tag_name_is(tag, length, "style", false)) {
            skip_script = true;
        }
        if (skip_script) {
            i = end + 1u;
            continue;
        }
        if (length != 0u && tag[0] == '/') {
            if (container == MEDIA_CONTAINER_AUDIO
                && length >= 6u && lower_ascii(tag[1]) == 'a'
                && lower_ascii(tag[2]) == 'u'
                && lower_ascii(tag[3]) == 'd'
                && lower_ascii(tag[4]) == 'i'
                && lower_ascii(tag[5]) == 'o') {
                container = MEDIA_CONTAINER_NONE;
            } else if (container == MEDIA_CONTAINER_VIDEO
                && length >= 6u && lower_ascii(tag[1]) == 'v'
                && lower_ascii(tag[2]) == 'i'
                && lower_ascii(tag[3]) == 'd'
                && lower_ascii(tag[4]) == 'e'
                && lower_ascii(tag[5]) == 'o') {
                container = MEDIA_CONTAINER_NONE;
            }
            i = end + 1u;
            continue;
        }
        enum media_container found_container = MEDIA_CONTAINER_NONE;
        if (length >= 5u && lower_ascii(tag[0]) == 'a'
            && lower_ascii(tag[1]) == 'u' && lower_ascii(tag[2]) == 'd'
            && lower_ascii(tag[3]) == 'i' && lower_ascii(tag[4]) == 'o'
            && (length == 5u || tag[5] == ' ' || tag[5] == '\t'
                || tag[5] == '/')) {
            container = found_container = MEDIA_CONTAINER_AUDIO;
        } else if (length >= 5u && lower_ascii(tag[0]) == 'v'
            && lower_ascii(tag[1]) == 'i' && lower_ascii(tag[2]) == 'd'
            && lower_ascii(tag[3]) == 'e' && lower_ascii(tag[4]) == 'o'
            && (length == 5u || tag[5] == ' ' || tag[5] == '\t'
                || tag[5] == '/')) {
            container = found_container = MEDIA_CONTAINER_VIDEO;
        }
        bool source_tag = length >= 6u && lower_ascii(tag[0]) == 's'
            && lower_ascii(tag[1]) == 'o' && lower_ascii(tag[2]) == 'u'
            && lower_ascii(tag[3]) == 'r' && lower_ascii(tag[4]) == 'c'
            && lower_ascii(tag[5]) == 'e'
            && (length == 6u || tag[6] == ' ' || tag[6] == '\t'
                || tag[6] == '/');
        if (found_container != MEDIA_CONTAINER_NONE || source_tag) {
            char url[BOOK8_MEDIA_URL_CAPACITY];
            char type[64];
            bool have_url = tag_attribute(tag, length, "src", url,
                sizeof(url));
            bool have_type = tag_attribute(tag, length, "type", type,
                sizeof(type));
            enum book8_media_kind context_kind =
                found_container == MEDIA_CONTAINER_VIDEO
                    || container == MEDIA_CONTAINER_VIDEO
                    ? BOOK8_MEDIA_VIDEO
                    : found_container == MEDIA_CONTAINER_AUDIO
                        || container == MEDIA_CONTAINER_AUDIO
                        ? BOOK8_MEDIA_UNSUPPORTED_AUDIO : BOOK8_MEDIA_OTHER;
            if (have_url && emit_source(url, have_type ? type : NULL,
                    context_kind, visitor, context)) {
                ++found;
            } else if (have_url) {
                return found;
            }
        }
        i = end + 1u;
    }
    return found;
}
