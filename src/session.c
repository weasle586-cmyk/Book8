#include "session.h"

#include <stddef.h>
#include "../net/sha256.h"
#include "registry.h"
#include "../include/string.h"

#define ADMIN_KEY "HKLM\\SOFTWARE\\BOOK8\\Users\\admin"
#define SESSION_HASH_BYTES 64u
#define SESSION_PASSWORD_MIN 4u

static bool initialized;
static bool authenticated;
static uint64_t token_state;
static uint64_t active_token;

static bool copy_registry_value(const char *name, const char *fallback)
{
    char existing[BOOK8_REGISTRY_VALUE_MAX];
    return reg_get_str(ADMIN_KEY, name, existing, sizeof(existing))
        || reg_set_str(ADMIN_KEY, name, fallback);
}

static void password_digest(const char *password, uint8_t digest[BOOK8_SHA256_BYTES])
{
    static const char prefix[] = "BOOK8:admin:v1:";
    struct book8_sha256 context;
    size_t length = 0u;
    while (password[length] != '\0') { ++length; }
    book8_sha256_init(&context);
    book8_sha256_update(&context, prefix, sizeof(prefix) - 1u);
    book8_sha256_update(&context, password, length);
    book8_sha256_final(&context, digest);
}

static void digest_text(const uint8_t digest[BOOK8_SHA256_BYTES],
    char text[SESSION_HASH_BYTES + 1u])
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < BOOK8_SHA256_BYTES; ++i) {
        text[i * 2u] = hex[digest[i] >> 4u];
        text[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    text[SESSION_HASH_BYTES] = '\0';
}

static bool parse_digest(const char *text, uint8_t digest[BOOK8_SHA256_BYTES])
{
    for (uint32_t i = 0u; i < BOOK8_SHA256_BYTES; ++i) {
        uint8_t pair[2];
        for (uint32_t j = 0u; j < 2u; ++j) {
            char value = text[i * 2u + j];
            if (value >= '0' && value <= '9') { pair[j] = (uint8_t)(value - '0'); }
            else if (value >= 'a' && value <= 'f') { pair[j] = (uint8_t)(value - 'a' + 10); }
            else if (value >= 'A' && value <= 'F') { pair[j] = (uint8_t)(value - 'A' + 10); }
            else { return false; }
        }
        digest[i] = (uint8_t)((pair[0] << 4u) | pair[1]);
    }
    return text[SESSION_HASH_BYTES] == '\0';
}

static bool password_matches(const char *password)
{
    char stored_text[SESSION_HASH_BYTES + 1u];
    uint8_t stored[BOOK8_SHA256_BYTES], candidate[BOOK8_SHA256_BYTES];
    bool matches = reg_get_str(ADMIN_KEY, "PasswordSHA256", stored_text,
            sizeof(stored_text))
        && parse_digest(stored_text, stored);
    if (matches) {
        password_digest(password, candidate);
        matches = book8_digest_equal(stored, candidate, sizeof(stored));
    }
    memset(stored_text, 0, sizeof(stored_text));
    memset(stored, 0, sizeof(stored));
    memset(candidate, 0, sizeof(candidate));
    return matches;
}

static uint32_t bounded_length(const char *text, uint32_t capacity)
{
    uint32_t length = 0u;
    if (text == NULL) { return capacity; }
    while (length < capacity && text[length] != '\0') { ++length; }
    return length;
}

static bool valid_name(const char *text)
{
    uint32_t length = bounded_length(text, BOOK8_SESSION_NAME_MAX);
    if (length == 0u || length >= BOOK8_SESSION_NAME_MAX) { return false; }
    for (uint32_t i = 0u; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32u || ch == '\\' || ch == '/' || ch == ':' || ch == '=') {
            return false;
        }
    }
    return true;
}

static bool valid_password(const char *password)
{
    uint32_t length = bounded_length(password, BOOK8_SESSION_PASSWORD_MAX + 1u);
    return length >= SESSION_PASSWORD_MIN
        && length <= BOOK8_SESSION_PASSWORD_MAX;
}

bool book8_session_seed_defaults(void)
{
    uint8_t digest[BOOK8_SHA256_BYTES];
    char text[SESSION_HASH_BYTES + 1u];
    uint32_t uid;
    bool ok = copy_registry_value("Username", "admin")
        && copy_registry_value("DisplayName", "admin");
    if (!reg_get_u32(ADMIN_KEY, "UID", &uid)) {
        ok = reg_set_u32(ADMIN_KEY, "UID", 0u) && ok;
    } else if (uid != 0u) {
        ok = reg_set_u32(ADMIN_KEY, "UID", 0u) && ok;
    }
    ok = reg_set_u32(ADMIN_KEY, "Root", 1u) && ok;
    char stored[BOOK8_REGISTRY_VALUE_MAX];
    if (!reg_get_str(ADMIN_KEY, "PasswordSHA256", stored, sizeof(stored))) {
        password_digest("default", digest);
        digest_text(digest, text);
        ok = reg_set_str(ADMIN_KEY, "PasswordSHA256", text) && ok;
    }
    memset(digest, 0, sizeof(digest));
    memset(text, 0, sizeof(text));
    memset(stored, 0, sizeof(stored));
    return ok;
}

bool book8_session_initialize(uint64_t seed)
{
    authenticated = false;
    active_token = 0u;
    token_state = seed ^ 0x9e3779b97f4a7c15ull;
    if (token_state == 0u) { token_state = 0xd1b54a32d192ed03ull; }
    bool seeded = book8_session_seed_defaults();
    initialized = true;
    return seeded;
}

bool book8_session_authenticated(void)
{
    return initialized && authenticated && active_token != 0u;
}

bool book8_session_login_required(void)
{
    return initialized && !book8_session_authenticated();
}

bool book8_session_profile(char *username, uint32_t username_capacity,
    char *display_name, uint32_t display_capacity)
{
    return initialized && username != NULL && display_name != NULL
        && reg_get_str(ADMIN_KEY, "Username", username, username_capacity)
        && reg_get_str(ADMIN_KEY, "DisplayName", display_name, display_capacity);
}

bool book8_session_login(const char *username, const char *password)
{
    char stored_username[BOOK8_SESSION_NAME_MAX];
    if (!initialized || !valid_name(username) || password == NULL
        || bounded_length(password, BOOK8_SESSION_PASSWORD_MAX + 1u)
            > BOOK8_SESSION_PASSWORD_MAX
        || !reg_get_str(ADMIN_KEY, "Username", stored_username,
            sizeof(stored_username))
        || strcmp(username, stored_username) != 0
        || !password_matches(password)) {
        return false;
    }
    token_state ^= token_state << 13u;
    token_state ^= token_state >> 7u;
    token_state ^= token_state << 17u;
    if (token_state == 0u) { token_state = 1u; }
    active_token = token_state;
    authenticated = true;
    memset(stored_username, 0, sizeof(stored_username));
    return true;
}

bool book8_session_current(struct book8_session_info *info)
{
    if (info == NULL || !book8_session_authenticated()) { return false; }
    *info = (struct book8_session_info){.uid = 0u, .token = active_token,
        .root = true};
    return book8_session_profile(info->username, sizeof(info->username),
        info->display_name, sizeof(info->display_name));
}

bool book8_session_authorize(uint32_t uid, uint64_t token, uint32_t access)
{
    const uint32_t allowed = BOOK8_SESSION_READ | BOOK8_SESSION_WRITE
        | BOOK8_SESSION_PROFILE | BOOK8_SESSION_NETSHARE;
    if (!book8_session_authenticated() || token == 0u
        || token != active_token || uid != 0u || access == 0u
        || (access & ~allowed) != 0u) {
        return false;
    }
    return true;
}

bool book8_session_authorize_current(uint32_t access)
{
    return book8_session_authorize(0u, active_token, access);
}

bool book8_session_update_profile(uint32_t uid, uint64_t token,
    const char *current_password, const char *username,
    const char *display_name, const char *new_password)
{
    char old_username[BOOK8_SESSION_NAME_MAX];
    char old_display_name[BOOK8_SESSION_NAME_MAX];
    char old_digest[SESSION_HASH_BYTES + 1u];
    char new_digest_text[SESSION_HASH_BYTES + 1u];
    uint8_t new_digest[BOOK8_SHA256_BYTES];
    bool ok = false;
    if (!book8_session_authorize(uid, token, BOOK8_SESSION_PROFILE)
        || !valid_name(username) || !valid_name(display_name)
        || current_password == NULL || !valid_password(new_password)
        || !password_matches(current_password)
        || !reg_get_str(ADMIN_KEY, "Username", old_username, sizeof(old_username))
        || !reg_get_str(ADMIN_KEY, "DisplayName", old_display_name, sizeof(old_display_name))
        || !reg_get_str(ADMIN_KEY, "PasswordSHA256", old_digest, sizeof(old_digest))) {
        return false;
    }
    password_digest(new_password, new_digest);
    digest_text(new_digest, new_digest_text);
    if (reg_set_str(ADMIN_KEY, "Username", username)
        && reg_set_str(ADMIN_KEY, "DisplayName", display_name)
        && reg_set_str(ADMIN_KEY, "PasswordSHA256", new_digest_text)
        && reg_set_u32(ADMIN_KEY, "UID", 0u)
        && reg_set_u32(ADMIN_KEY, "Root", 1u)
        && reg_save()) {
        ok = true;
    } else {
        (void)reg_set_str(ADMIN_KEY, "Username", old_username);
        (void)reg_set_str(ADMIN_KEY, "DisplayName", old_display_name);
        (void)reg_set_str(ADMIN_KEY, "PasswordSHA256", old_digest);
        (void)reg_set_u32(ADMIN_KEY, "UID", 0u);
        (void)reg_set_u32(ADMIN_KEY, "Root", 1u);
        (void)reg_save();
    }
    memset(old_username, 0, sizeof(old_username));
    memset(old_display_name, 0, sizeof(old_display_name));
    memset(old_digest, 0, sizeof(old_digest));
    memset(new_digest_text, 0, sizeof(new_digest_text));
    memset(new_digest, 0, sizeof(new_digest));
    return ok;
}
