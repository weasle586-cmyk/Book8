#ifndef BOOK8_SESSION_H
#define BOOK8_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#define BOOK8_SESSION_NAME_MAX 32u
#define BOOK8_SESSION_PASSWORD_MAX 64u

#define BOOK8_SESSION_READ 0x01u
#define BOOK8_SESSION_WRITE 0x02u
#define BOOK8_SESSION_PROFILE 0x04u
#define BOOK8_SESSION_NETSHARE 0x08u

struct book8_session_info {
    uint32_t uid;
    uint64_t token;
    bool root;
    char username[BOOK8_SESSION_NAME_MAX];
    char display_name[BOOK8_SESSION_NAME_MAX];
};

bool book8_session_seed_defaults(void);
bool book8_session_initialize(uint64_t seed);
bool book8_session_login_required(void);
bool book8_session_authenticated(void);
bool book8_session_login(const char *username, const char *password);
bool book8_session_current(struct book8_session_info *info);
bool book8_session_profile(char *username, uint32_t username_capacity,
    char *display_name, uint32_t display_capacity);
bool book8_session_authorize(uint32_t uid, uint64_t token, uint32_t access);
bool book8_session_authorize_current(uint32_t access);
bool book8_session_update_profile(uint32_t uid, uint64_t token,
    const char *current_password, const char *username,
    const char *display_name, const char *new_password);

#endif
