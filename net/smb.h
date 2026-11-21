#ifndef BOOK8_SMB_H
#define BOOK8_SMB_H
#include "tcp.h"
#define BOOK8_SHARE_ENTRIES 128u
#define BOOK8_SHARE_DOWNLOAD (1024u * 1024u)
struct book8_share_entry {
    char name[256];
    uint64_t size, allocated, created, modified;
    uint32_t attributes;
    bool directory;
};
enum book8_share_state { SHARE_IDLE, SHARE_BUSY, SHARE_READY, SHARE_DOWNLOADED, SHARE_FAILED, SHARE_UPLOADED };
struct book8_share {
    struct book8_tcp tcp;
    enum book8_share_state state;
    char error[160], share[64], path[256], file[256];
    char username[64], domain[64], password[128];
    struct book8_share_entry entries[BOOK8_SHARE_ENTRIES];
    uint32_t count, downloaded, expected, request_length, read_size;
    uint64_t session, message_id, pending_id, deadline;
    uint32_t tree, status;
    uint16_t command;
    uint8_t stage, next_stage, file_id[16], request[1400];
    uint8_t auth_token[1152], session_key[16];
    uint32_t auth_size, max_write;
    bool waiting, authenticated, downloading, uploading, signing, auth_failed, credentials;
    uint8_t data[BOOK8_SHARE_DOWNLOAD];
};
bool book8_share_connect(struct book8_share *, struct book8_net *, uint32_t ip,
    const char *share, uint64_t now);
bool book8_share_connect_auth(struct book8_share *, struct book8_net *, uint32_t ip,
    const char *share, const char *username, const char *domain, const char *password, uint64_t now);
bool book8_share_directory(struct book8_share *, const char *path, uint64_t now);
bool book8_share_download(struct book8_share *, uint32_t entry, uint64_t now);
bool book8_share_upload(struct book8_share *, const char *name, const void *data, uint32_t size, uint64_t now);
void book8_share_poll(struct book8_share *, uint64_t now);
void book8_share_close(struct book8_share *);
#endif
