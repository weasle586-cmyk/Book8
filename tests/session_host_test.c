#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/registry.h"
#include "../src/session.h"

static char registry_bytes[8192];
static uint32_t registry_size;
static bool fail_write;

static bool read_registry(const char *path, void *destination,
    uint32_t capacity, uint32_t *size)
{
    if (strcmp(path, BOOK8_REGISTRY_FILE) != 0
        || registry_size == 0u || registry_size > capacity) {
        return false;
    }
    memcpy(destination, registry_bytes, registry_size);
    *size = registry_size;
    return true;
}

static bool write_registry(const char *path, const void *source,
    uint32_t size)
{
    if (fail_write || strcmp(path, BOOK8_REGISTRY_FILE) != 0
        || size > sizeof(registry_bytes)) {
        return false;
    }
    memcpy(registry_bytes, source, size);
    registry_size = size;
    return true;
}

static void check_persisted_password_is_hashed(void)
{
    assert(reg_save());
    registry_bytes[registry_size] = '\0';
    assert(strstr(registry_bytes, "admin") != NULL);
    assert(strstr(registry_bytes, "default") == NULL);
    assert(strstr(registry_bytes, "PasswordSHA256=") != NULL);
}

int main(void)
{
    reg_set_storage(read_registry, write_registry);
    assert(book8_session_initialize(0x12345678u));
    assert(book8_session_login_required());
    assert(!book8_session_authenticated());
    check_persisted_password_is_hashed();

    char username[BOOK8_SESSION_NAME_MAX], display_name[BOOK8_SESSION_NAME_MAX];
    assert(book8_session_profile(username, sizeof(username), display_name,
        sizeof(display_name)));
    assert(strcmp(username, "admin") == 0);
    assert(strcmp(display_name, "admin") == 0);
    assert(!book8_session_login("admin", "wrong"));
    assert(book8_session_login("admin", "default"));

    struct book8_session_info session;
    assert(book8_session_current(&session));
    assert(session.uid == 0u && session.root && session.token != 0u);
    assert(book8_session_authorize(session.uid, session.token,
        BOOK8_SESSION_READ | BOOK8_SESSION_WRITE));
    assert(!book8_session_authorize(session.uid + 1u, session.token,
        BOOK8_SESSION_READ));
    assert(!book8_session_authorize(session.uid, session.token + 1u,
        BOOK8_SESSION_READ));

    assert(!book8_session_update_profile(session.uid, session.token, "default",
        "operator", "Primary User", "abc"));
    assert(!book8_session_update_profile(session.uid, session.token, "badpass",
        "operator", "Primary User", "newpass"));
    assert(book8_session_update_profile(session.uid, session.token, "default",
        "operator", "Primary User", "newpass"));
    assert(book8_session_current(&session));
    assert(session.uid == 0u && session.root);
    assert(strcmp(session.username, "operator") == 0);
    assert(strcmp(session.display_name, "Primary User") == 0);

    assert(reg_load());
    assert(reg_set_u32("HKLM\\SOFTWARE\\BOOK8\\Users\\admin", "UID", 9u));
    assert(reg_set_u32("HKLM\\SOFTWARE\\BOOK8\\Users\\admin", "Root", 0u));
    assert(book8_session_initialize(0x87654321u));
    uint32_t uid = 9u, root = 0u;
    assert(reg_get_u32("HKLM\\SOFTWARE\\BOOK8\\Users\\admin", "UID", &uid));
    assert(reg_get_u32("HKLM\\SOFTWARE\\BOOK8\\Users\\admin", "Root", &root));
    assert(uid == 0u && root == 1u);
    assert(book8_session_login_required());
    assert(!book8_session_login("admin", "newpass"));
    assert(!book8_session_login("operator", "default"));
    assert(book8_session_login("operator", "newpass"));
    assert(book8_session_current(&session));
    assert(session.uid == 0u && session.root);
    check_persisted_password_is_hashed();

    struct book8_session_info old_session = session;
    fail_write = true;
    assert(!book8_session_update_profile(session.uid, session.token,
        "newpass", "rootless", "Should Roll Back", "changedpass"));
    fail_write = false;
    assert(book8_session_login("operator", "newpass"));
    assert(!book8_session_login("rootless", "changedpass"));
    assert(book8_session_current(&session));
    assert(session.uid == 0u && session.root);
    assert(session.token != old_session.token);

    puts("session host test: PASS");
    return 0;
}
