#ifndef BOOK8_NTLM_H
#define BOOK8_NTLM_H
#include <stdbool.h>
#include <stdint.h>
#define BOOK8_NTLM_TOKEN 1152u
bool book8_ntlm_auth(const char *user, const char *domain, const char *password,
    const uint8_t *challenge, uint32_t length, uint64_t nonce,
    uint8_t token[BOOK8_NTLM_TOKEN], uint32_t *size, uint8_t key[16]);
void book8_ntlm_md4(const void *data, uint32_t length, uint8_t out[16]);
void book8_ntlm_md5(const void *data, uint32_t length, uint8_t out[16]);
void book8_ntlm_clear(void *data, uint32_t length);
#endif
