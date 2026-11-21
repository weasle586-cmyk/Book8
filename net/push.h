#ifndef BOOK8_PUSH_H
#define BOOK8_PUSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "sha256.h"

/* Authenticated workstation -> BOOK8 update agent on UDP 8081.
 *
 *   workstation                      BOOK8
 *   HELLO(client nonce)       --->
 *                             <---   CHALLENGE(server nonce, build id)
 *   COMMAND(server nonce, body, HMAC) --->   verified once, then queued
 *                             <---   RESULT(server nonce, status, HMAC)
 *
 * Every integer is big-endian. HMAC-SHA256 with the per-build 32-byte key
 * covers all preceding packet bytes (CHALLENGE additionally binds the client's
 * HELLO nonce, appended after its body, so the workstation can authenticate
 * the build id). A server nonce is single use and expires
 * after BOOK8_PUSH_CHALLENGE_MS; retransmitting the identical COMMAND returns
 * the stored RESULT instead of executing twice. The callback only validates
 * and queues: the kernel executes requests from its serialized service loop. */
#define BOOK8_PUSH_PORT 8081u
#define BOOK8_PUSH_MAGIC 0x424b3850u /* "BK8P" */
#define BOOK8_PUSH_VERSION 1u
#define BOOK8_PUSH_KEY_BYTES 32u
#define BOOK8_PUSH_NONCE_BYTES 16u
#define BOOK8_PUSH_CHALLENGE_MS 10000u
#define BOOK8_PUSH_RESULT_RETRY_MS 3000u
#define BOOK8_PUSH_REMOTE_MAX 64u
#define BOOK8_PUSH_LOCAL_MAX 16u
#define BOOK8_PUSH_MESSAGE_MAX 96u
#define BOOK8_PUSH_FLAG_PERSIST 1u

enum book8_push_type {
    BOOK8_PUSH_HELLO = 1,
    BOOK8_PUSH_CHALLENGE = 2,
    BOOK8_PUSH_COMMAND = 3,
    BOOK8_PUSH_RESULT = 4
};

enum book8_push_command_code {
    BOOK8_PUSH_STATUS = 0,
    BOOK8_PUSH_CONFIG = 1,
    BOOK8_PUSH_FILE = 2,
    BOOK8_PUSH_KERNEL = 3,
    BOOK8_PUSH_REBOOT = 4,
    BOOK8_PUSH_INSTALL_KERNEL = 5
};

enum book8_push_status {
    BOOK8_PUSH_OK = 0,
    BOOK8_PUSH_ACCEPTED = 1,
    BOOK8_PUSH_BAD_REQUEST = 2,
    BOOK8_PUSH_BUSY = 3,
    BOOK8_PUSH_FAILED = 4
};

#pragma pack(push, 1)
struct book8_push_header {
    uint32_t magic;
    uint8_t version, type;
    uint16_t length; /* body bytes, excluding header and MAC */
    uint32_t request_id;
    uint8_t nonce[BOOK8_PUSH_NONCE_BYTES];
};
struct book8_push_challenge {
    uint32_t build_id, window_ms;
};
struct book8_push_command {
    uint8_t command, flags;
    uint16_t reserved;
    uint32_t ip, mask, gateway, telemetry, size;
    uint8_t sha256[BOOK8_SHA256_BYTES];
    char remote[BOOK8_PUSH_REMOTE_MAX]; /* TFTP file name on the workstation */
    char local[BOOK8_PUSH_LOCAL_MAX];   /* FILE: root 8.3 destination name */
};
struct book8_push_result {
    uint8_t command, status;
    uint16_t reserved;
    uint32_t build_id;
    char message[BOOK8_PUSH_MESSAGE_MAX];
};
#pragma pack(pop)

#define BOOK8_PUSH_HELLO_BYTES (sizeof(struct book8_push_header))
#define BOOK8_PUSH_CHALLENGE_BYTES \
    (sizeof(struct book8_push_header) + sizeof(struct book8_push_challenge))
#define BOOK8_PUSH_COMMAND_BYTES (sizeof(struct book8_push_header) \
    + sizeof(struct book8_push_command) + BOOK8_SHA256_BYTES)
#define BOOK8_PUSH_RESULT_BYTES (sizeof(struct book8_push_header) \
    + sizeof(struct book8_push_result) + BOOK8_SHA256_BYTES)

/* Verified request, host order, strings NUL-terminated. */
struct book8_push_request {
    uint32_t source_ip, request_id;
    uint16_t source_port;
    uint8_t command, flags;
    uint32_t ip, mask, gateway, telemetry, size;
    uint8_t sha256[BOOK8_SHA256_BYTES];
    char remote[BOOK8_PUSH_REMOTE_MAX];
    char local[BOOK8_PUSH_LOCAL_MAX];
};

struct book8_push_platform {
    void *ctx;
    uint64_t (*milliseconds)(void *);
    /* Fill 32 bytes of best-effort entropy (TSC/RDRAND); mixed with the key. */
    void (*entropy)(void *, uint8_t out[BOOK8_SHA256_BYTES]);
};

struct book8_push_stats {
    uint32_t accepted, rejected, challenges, busy_replies;
    bool listening, request_pending, request_running, result_unsent;
};

/* Binds UDP 8081. net=NULL stops the agent and releases the port. Contexts
 * and key storage are copied; the network context must outlive the agent. */
enum book8_net_result book8_push_init(struct book8_net *net,
    const uint8_t key[BOOK8_PUSH_KEY_BYTES], uint32_t build_id,
    const struct book8_push_platform *platform);
/* Moves one verified queued request into the running state. */
bool book8_push_take(struct book8_push_request *request);
/* Finishes the running request: signs, stores and sends its RESULT. */
enum book8_net_result book8_push_complete(enum book8_push_status status,
    const char *message);
/* Retries an unsent RESULT (e.g. ARP pending) until BOOK8_PUSH_RESULT_RETRY_MS. */
enum book8_net_result book8_push_service(void);
void book8_push_get_stats(struct book8_push_stats *stats);
const char *book8_push_command_name(uint8_t command);

#endif
