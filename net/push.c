#include "push.h"

#include <string.h>

_Static_assert(sizeof(struct book8_push_header) == 28u, "push header");
_Static_assert(sizeof(struct book8_push_challenge) == 8u, "push challenge");
_Static_assert(sizeof(struct book8_push_command) == 136u, "push command");
_Static_assert(sizeof(struct book8_push_result) == 104u, "push result");

enum push_slot_state {
    SLOT_IDLE,
    SLOT_CHALLENGED,
    SLOT_QUEUED,
    SLOT_RUNNING,
    SLOT_DONE
};

static struct {
    struct book8_net *net;
    struct book8_push_platform platform;
    uint8_t key[BOOK8_PUSH_KEY_BYTES];
    uint32_t build_id;
    uint64_t counter;
    enum push_slot_state state;
    uint8_t nonce[BOOK8_PUSH_NONCE_BYTES];
    uint32_t client_ip, request_id;
    uint16_t client_port;
    uint64_t issued_ms, result_ms;
    uint8_t command_mac[BOOK8_SHA256_BYTES];
    struct book8_push_request request;
    bool result_unsent;
    uint8_t result[BOOK8_PUSH_RESULT_BYTES];
    uint8_t scratch[BOOK8_PUSH_RESULT_BYTES];
    struct book8_push_stats stats;
} push;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8u);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24u);
    p[1] = (uint8_t)(v >> 16u);
    p[2] = (uint8_t)(v >> 8u);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8u) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u)
        | ((uint32_t)p[2] << 8u) | p[3];
}

static uint64_t now_ms(void)
{
    return push.platform.milliseconds(push.platform.ctx);
}

static void write_header(uint8_t *p, uint8_t type, uint16_t length,
    uint32_t request_id, const uint8_t nonce[BOOK8_PUSH_NONCE_BYTES])
{
    put32(p, BOOK8_PUSH_MAGIC);
    p[4] = BOOK8_PUSH_VERSION;
    p[5] = type;
    put16(p + 6u, length);
    put32(p + 8u, request_id);
    memcpy(p + 12u, nonce, BOOK8_PUSH_NONCE_BYTES);
}

/* MAC over packet bytes, optionally followed by extra bound data (client nonce). */
static void sign(const uint8_t *data, size_t length, const uint8_t *extra,
    size_t extra_length, uint8_t mac[BOOK8_SHA256_BYTES])
{
    uint8_t buffer[BOOK8_PUSH_RESULT_BYTES + BOOK8_PUSH_NONCE_BYTES];
    memcpy(buffer, data, length);
    if (extra_length != 0u) {
        memcpy(buffer + length, extra, extra_length);
    }
    book8_hmac_sha256(push.key, sizeof(push.key), buffer, length + extra_length, mac);
}

static void new_nonce(const uint8_t client_nonce[BOOK8_PUSH_NONCE_BYTES])
{
    uint8_t entropy[BOOK8_SHA256_BYTES] = {0};
    uint8_t digest[BOOK8_SHA256_BYTES];
    uint64_t ms = now_ms();
    struct book8_sha256 ctx;
    if (push.platform.entropy != NULL) {
        push.platform.entropy(push.platform.ctx, entropy);
    }
    ++push.counter;
    book8_sha256_init(&ctx);
    book8_sha256_update(&ctx, push.key, sizeof(push.key));
    book8_sha256_update(&ctx, &push.counter, sizeof(push.counter));
    book8_sha256_update(&ctx, &ms, sizeof(ms));
    book8_sha256_update(&ctx, entropy, sizeof(entropy));
    book8_sha256_update(&ctx, client_nonce, BOOK8_PUSH_NONCE_BYTES);
    book8_sha256_final(&ctx, digest);
    memcpy(push.nonce, digest, BOOK8_PUSH_NONCE_BYTES);
}

static enum book8_net_result send_result(void)
{
    enum book8_net_result result = book8_udp_send(push.net, BOOK8_PUSH_PORT,
        push.client_ip, push.client_port, push.result, sizeof(push.result));
    push.result_unsent = result != BOOK8_NET_OK;
    return result;
}

static void build_result(uint8_t *out, uint32_t request_id,
    const uint8_t nonce[BOOK8_PUSH_NONCE_BYTES], uint8_t command,
    enum book8_push_status status, const char *message)
{
    const size_t header = sizeof(struct book8_push_header);
    memset(out, 0, BOOK8_PUSH_RESULT_BYTES);
    write_header(out, BOOK8_PUSH_RESULT, sizeof(struct book8_push_result),
        request_id, nonce);
    uint8_t *body = out + header;
    body[0] = command;
    body[1] = (uint8_t)status;
    put32(body + 4u, push.build_id);
    if (message != NULL) {
        size_t length = strlen(message);
        if (length >= BOOK8_PUSH_MESSAGE_MAX) {
            length = BOOK8_PUSH_MESSAGE_MAX - 1u;
        }
        memcpy(body + 8u, message, length);
    }
    sign(out, header + sizeof(struct book8_push_result), NULL, 0u,
        out + header + sizeof(struct book8_push_result));
}

static void finish(enum book8_push_status status, const char *message)
{
    build_result(push.result, push.request_id, push.nonce, push.request.command,
        status, message);
    push.state = SLOT_DONE;
    push.result_ms = now_ms();
    (void)send_result();
}

static bool copy_string(char *out, const uint8_t *in, size_t capacity)
{
    const void *end = memchr(in, '\0', capacity);
    if (end == NULL) {
        return false;
    }
    size_t length = (size_t)((const uint8_t *)end - in);
    memcpy(out, in, length);
    memset(out + length, 0, capacity - length);
    /* Bytes after the terminator must be zero so the MAC has one meaning. */
    for (size_t i = length; i < capacity; ++i) {
        if (in[i] != 0u) {
            return false;
        }
    }
    for (size_t i = 0u; i < length; ++i) {
        if (in[i] < 0x21u || in[i] > 0x7eu) {
            return false;
        }
    }
    return true;
}

static void handle_hello(uint32_t ip, uint16_t port, const uint8_t *data)
{
    const uint8_t *client_nonce = data + 12u;
    uint32_t request_id = get32(data + 8u);
    if (push.state == SLOT_QUEUED || push.state == SLOT_RUNNING) {
        ++push.stats.busy_replies;
        build_result(push.scratch, request_id, client_nonce, push.request.command,
            BOOK8_PUSH_BUSY, "update agent busy");
        (void)book8_udp_send(push.net, BOOK8_PUSH_PORT, ip, port,
            push.scratch, BOOK8_PUSH_RESULT_BYTES);
        return;
    }
    new_nonce(client_nonce);
    push.state = SLOT_CHALLENGED;
    push.client_ip = ip;
    push.client_port = port;
    push.issued_ms = now_ms();
    push.result_unsent = false;
    ++push.stats.challenges;
    uint8_t *out = push.scratch;
    const size_t body = sizeof(struct book8_push_header);
    write_header(out, BOOK8_PUSH_CHALLENGE, sizeof(struct book8_push_challenge),
        request_id, push.nonce);
    put32(out + body, push.build_id);
    put32(out + body + 4u, BOOK8_PUSH_CHALLENGE_MS);
    sign(out, BOOK8_PUSH_CHALLENGE_BYTES, client_nonce, BOOK8_PUSH_NONCE_BYTES,
        out + BOOK8_PUSH_CHALLENGE_BYTES);
    (void)book8_udp_send(push.net, BOOK8_PUSH_PORT, ip, port, out,
        BOOK8_PUSH_CHALLENGE_BYTES + BOOK8_SHA256_BYTES);
}

static void handle_command(uint32_t ip, uint16_t port, const uint8_t *data)
{
    const size_t header = sizeof(struct book8_push_header);
    const size_t signed_length = header + sizeof(struct book8_push_command);
    const uint8_t *nonce = data + 12u;
    const uint8_t *mac = data + signed_length;
    uint8_t expected[BOOK8_SHA256_BYTES];
    if (push.state == SLOT_IDLE
        || memcmp(nonce, push.nonce, BOOK8_PUSH_NONCE_BYTES) != 0
        || ip != push.client_ip || port != push.client_port) {
        ++push.stats.rejected;
        return;
    }
    if (push.state != SLOT_CHALLENGED) {
        /* Identical retransmission: answer from the stored RESULT only. */
        if (get32(data + 8u) == push.request_id
            && book8_digest_equal(mac, push.command_mac, BOOK8_SHA256_BYTES)) {
            if (push.state == SLOT_DONE) {
                (void)send_result();
            }
        } else {
            ++push.stats.rejected;
        }
        return;
    }
    if (now_ms() - push.issued_ms > BOOK8_PUSH_CHALLENGE_MS) {
        push.state = SLOT_IDLE;
        ++push.stats.rejected;
        return;
    }
    sign(data, signed_length, NULL, 0u, expected);
    if (!book8_digest_equal(mac, expected, BOOK8_SHA256_BYTES)) {
        ++push.stats.rejected;
        return;
    }
    ++push.stats.accepted;
    push.request_id = get32(data + 8u);
    memcpy(push.command_mac, mac, BOOK8_SHA256_BYTES);
    const uint8_t *b = data + header;
    struct book8_push_request *r = &push.request;
    memset(r, 0, sizeof(*r));
    r->source_ip = ip;
    r->source_port = port;
    r->request_id = push.request_id;
    r->command = b[0];
    r->flags = b[1];
    r->ip = get32(b + 4u);
    r->mask = get32(b + 8u);
    r->gateway = get32(b + 12u);
    r->telemetry = get32(b + 16u);
    r->size = get32(b + 20u);
    memcpy(r->sha256, b + 24u, BOOK8_SHA256_BYTES);
    bool valid = r->command <= BOOK8_PUSH_INSTALL_KERNEL && get16(b + 2u) == 0u
        && copy_string(r->remote, b + 56u, BOOK8_PUSH_REMOTE_MAX)
        && copy_string(r->local, b + 56u + BOOK8_PUSH_REMOTE_MAX, BOOK8_PUSH_LOCAL_MAX);
    if (valid && (r->command == BOOK8_PUSH_FILE || r->command == BOOK8_PUSH_KERNEL
        || r->command == BOOK8_PUSH_INSTALL_KERNEL)) {
        valid = r->remote[0] != '\0' && r->size != 0u
            && (r->command != BOOK8_PUSH_FILE || r->local[0] != '\0');
    }
    if (valid && r->command == BOOK8_PUSH_INSTALL_KERNEL) {
        valid = r->flags == 0u && r->local[0] == '\0';
    }
    if (!valid) {
        push.state = SLOT_RUNNING;
        finish(BOOK8_PUSH_BAD_REQUEST, "malformed command body");
        return;
    }
    push.state = SLOT_QUEUED;
}

static void on_datagram(void *ctx, uint32_t ip, uint16_t port,
    const uint8_t *data, size_t length)
{
    (void)ctx;
    if (length < sizeof(struct book8_push_header) || get32(data) != BOOK8_PUSH_MAGIC
        || data[4] != BOOK8_PUSH_VERSION) {
        ++push.stats.rejected;
        return;
    }
    uint16_t body = get16(data + 6u);
    if (data[5] == BOOK8_PUSH_HELLO && body == 0u && length == BOOK8_PUSH_HELLO_BYTES) {
        handle_hello(ip, port, data);
    } else if (data[5] == BOOK8_PUSH_COMMAND && body == sizeof(struct book8_push_command)
        && length == BOOK8_PUSH_COMMAND_BYTES) {
        handle_command(ip, port, data);
    } else {
        ++push.stats.rejected;
    }
}

enum book8_net_result book8_push_init(struct book8_net *net,
    const uint8_t key[BOOK8_PUSH_KEY_BYTES], uint32_t build_id,
    const struct book8_push_platform *platform)
{
    if (net != NULL && (key == NULL || platform == NULL
        || platform->milliseconds == NULL)) {
        return BOOK8_NET_INVALID;
    }
    if (push.net != NULL) {
        enum book8_net_result result = book8_udp_unbind(push.net, BOOK8_PUSH_PORT);
        if (result == BOOK8_NET_BUSY) {
            return result;
        }
    }
    memset(&push, 0, sizeof(push));
    if (net == NULL) {
        return BOOK8_NET_OK;
    }
    enum book8_net_result result = book8_udp_bind(net, BOOK8_PUSH_PORT, on_datagram, NULL);
    if (result != BOOK8_NET_OK) {
        return result;
    }
    push.net = net;
    push.platform = *platform;
    memcpy(push.key, key, sizeof(push.key));
    push.build_id = build_id;
    push.stats.listening = true;
    return BOOK8_NET_OK;
}

bool book8_push_take(struct book8_push_request *request)
{
    if (push.net == NULL || push.state != SLOT_QUEUED || request == NULL) {
        return false;
    }
    *request = push.request;
    push.state = SLOT_RUNNING;
    return true;
}

enum book8_net_result book8_push_complete(enum book8_push_status status,
    const char *message)
{
    if (push.net == NULL || push.state != SLOT_RUNNING) {
        return BOOK8_NET_INVALID;
    }
    finish(status, message);
    return push.result_unsent ? BOOK8_NET_ARP_PENDING : BOOK8_NET_OK;
}

enum book8_net_result book8_push_service(void)
{
    if (push.net == NULL || !push.result_unsent) {
        return BOOK8_NET_EMPTY;
    }
    if (now_ms() - push.result_ms > BOOK8_PUSH_RESULT_RETRY_MS) {
        push.result_unsent = false;
        return BOOK8_NET_TIMEOUT;
    }
    return send_result();
}

void book8_push_get_stats(struct book8_push_stats *stats)
{
    if (stats == NULL) {
        return;
    }
    *stats = push.stats;
    stats->listening = push.net != NULL;
    stats->request_pending = push.state == SLOT_QUEUED;
    stats->request_running = push.state == SLOT_RUNNING;
    stats->result_unsent = push.result_unsent;
}

const char *book8_push_command_name(uint8_t command)
{
    switch (command) {
    case BOOK8_PUSH_STATUS: return "STATUS";
    case BOOK8_PUSH_CONFIG: return "CONFIG";
    case BOOK8_PUSH_FILE: return "FILE";
    case BOOK8_PUSH_KERNEL: return "KERNEL";
    case BOOK8_PUSH_REBOOT: return "REBOOT";
    case BOOK8_PUSH_INSTALL_KERNEL: return "INSTALL_KERNEL";
    default: return "UNKNOWN";
    }
}
