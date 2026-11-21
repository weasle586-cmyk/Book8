#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "../net/smb.h"

/* Loopback socket adapter supplies Ethernet/TCP peer packets to the real
 * BOOK8 network, TCP and SMB layers; it never contacts a non-loopback host. */
#define LOCAL BOOK8_NET_IP(10,0,2,15)
#define PEER BOOK8_NET_IP(10,0,2,2)
static struct book8_net net;
static struct book8_share share;
static int fd = -1;
static uint16_t port, local_port;
static uint32_t remote_seq, local_seq;
static bool syn_pending, ack_pending;
static uint8_t bytes[1400];
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] * 256u + p[1]); }
static uint32_t g32(const uint8_t *p) { return ((uint32_t)g16(p) << 16u) | g16(p + 2u); }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8u); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)(v >> 16u)); p16(p + 2u, (uint16_t)v); }
static uint16_t checksum(const uint8_t *p, size_t n, uint32_t sum)
{
    for (size_t i = 0u; i < n; ++i) { sum += (uint32_t)p[i] << (i % 2u ? 0u : 8u); }
    while (sum >> 16u) { sum = (sum & 65535u) + (sum >> 16u); }
    return (uint16_t)~sum;
}
static enum book8_net_result send_frame(void *ctx, const void *data, size_t size)
{
    (void)ctx; const uint8_t *frame = data;
    assert(size >= 54u && frame[23] == 6u);
    const uint8_t *p = frame + 34u; uint32_t header = (p[12] >> 4u) * 4u;
    assert(header <= size - 34u && g16(p + 2u) == 445u);
    assert(checksum(p, size - 34u, (LOCAL >> 16u) + (LOCAL & 65535u)
        + (PEER >> 16u) + (PEER & 65535u) + 6u + (uint32_t)size - 34u) == 0u);
    if (p[13] & 2u) {
        if (fd < 0) {
            fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port),
                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
            assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
            remote_seq = 10001u;
        }
        local_seq = g32(p + 4u) + 1u; local_port = g16(p); syn_pending = true;
    } else if (p[13] & 4u) { if (fd >= 0) { close(fd); fd = -1; } }
    else {
        uint32_t count = (uint32_t)size - 34u - header, seq = g32(p + 4u);
        if (count) {
            if (seq == local_seq) {
                uint32_t written = 0u;
                while (written < count) {
                    ssize_t result = send(fd, p + header + written, count - written, MSG_NOSIGNAL);
                    assert(result > 0); written += (uint32_t)result;
                }
                local_seq += count;
            } else { assert((int32_t)(seq - local_seq) < 0); }
            ack_pending = true;
        }
    }
    return BOOK8_NET_OK;
}
static enum book8_net_result receive_frame(void *ctx, void *data, size_t capacity, size_t *size)
{
    (void)ctx;
    uint8_t flags = 16u; uint32_t count = 0u;
    if (syn_pending) { flags = 18u; syn_pending = false; }
    else if (ack_pending) { ack_pending = false; }
    else {
        if (fd < 0) { return BOOK8_NET_EMPTY; }
        ssize_t result = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return BOOK8_NET_EMPTY; }
        if (result <= 0) { return BOOK8_NET_IO_ERROR; }
        count = (uint32_t)result; flags = 24u;
    }
    uint32_t header = flags & 2u ? 24u : 20u;
    *size = 34u + header + count; assert(*size <= capacity);
    uint8_t *frame = data; memset(frame, 0u, *size);
    memcpy(frame, net.mac, 6u); memcpy(frame + 6u, net.arp[0].mac, 6u); p16(frame + 12u, 0x800u);
    uint8_t *ip = frame + 14u; ip[0] = 0x45u; p16(ip + 2u, (uint16_t)(20u + header + count));
    ip[8] = 64u; ip[9] = 6u; p32(ip + 12u, PEER); p32(ip + 16u, LOCAL);
    p16(ip + 10u, checksum(ip, 20u, 0u));
    uint8_t *p = frame + 34u; p16(p, 445u); p16(p + 2u, local_port);
    p32(p + 4u, remote_seq); p32(p + 8u, local_seq); p[12] = (uint8_t)(header << 2u);
    p[13] = flags; p16(p + 14u, 32768u);
    if (flags & 2u) { p[20] = 2u; p[21] = 4u; p16(p + 22u, 1400u); ++remote_seq; }
    if (count) { memcpy(p + header, bytes, count); remote_seq += count; }
    p16(p + 16u, checksum(p, header + count, (LOCAL >> 16u) + (LOCAL & 65535u)
        + (PEER >> 16u) + (PEER & 65535u) + 6u + header + count));
    return BOOK8_NET_OK;
}
static uint64_t milliseconds(void)
{
    struct timespec time; assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000u + (uint64_t)time.tv_nsec / 1000000u;
}
static void wait_state(enum book8_share_state state)
{
    uint64_t deadline = milliseconds() + 20000u;
    while (share.state == SHARE_BUSY && milliseconds() < deadline) {
        enum book8_net_result r = book8_net_poll(&net);
        if (r != BOOK8_NET_OK) { fprintf(stderr, "network: %s\n", book8_net_result_string(r)); abort(); }
        book8_share_poll(&share, milliseconds()); usleep(1000u);
    }
    if (share.state != state) { fprintf(stderr, "SMB state %u error: %s (status %08x)\n", share.state, share.error, share.status); abort(); }
}
int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4); bool auth = argc == 4;
    unsigned long value = strtoul(argv[1], NULL, 10);
    assert(value && value <= 65535u); port = (uint16_t)value;
    struct book8_net_transport transport = {.send = send_frame, .receive = receive_frame};
    uint8_t mac[6] = {2,0,0,0,0,1};
    assert(book8_net_init(&net, &transport, mac) == BOOK8_NET_OK);
    assert(book8_net_configure(&net, LOCAL, 0xffffff00u, PEER) == BOOK8_NET_OK);
    net.arp[0].ip = PEER; net.arp[0].mac[0] = 2u; net.arp[0].mac[5] = 2u;
    if (auth) {
        assert(book8_share_connect_auth(&share, &net, PEER, "public", "tester", "WORKGROUP", "synthetic-pass", milliseconds()));
    } else { assert(book8_share_connect(&share, &net, PEER, "public", milliseconds())); }
    wait_state(SHARE_READY);
    if (auth) { assert(share.signing && share.authenticated && !share.password[0]); }
    bool found = false;
    for (uint32_t i = 0u; i < share.count; ++i) { found |= !strcmp(share.entries[i].name, "docs") && share.entries[i].directory; }
    assert(found);
    assert(book8_share_directory(&share, "docs", milliseconds())); wait_state(SHARE_READY);
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0u; i < share.count; ++i) { if (!strcmp(share.entries[i].name, "test.c")) { selected = i; } }
    assert(selected != UINT32_MAX);
    assert(book8_share_download(&share, selected, milliseconds())); wait_state(SHARE_DOWNLOADED);
    FILE *file = fopen(argv[2], "rb"); assert(file);
    uint8_t *expected = malloc(BOOK8_SHARE_DOWNLOAD); assert(expected);
    size_t size = fread(expected, 1u, BOOK8_SHARE_DOWNLOAD, file); assert(fclose(file) == 0);
    assert(size == share.downloaded && !memcmp(expected, share.data, size));
    if (auth) {
        assert(book8_share_upload(&share, "upload.c", expected, (uint32_t)size, milliseconds())); wait_state(SHARE_UPLOADED);
        assert(book8_share_directory(&share, "docs", milliseconds())); wait_state(SHARE_READY);
        selected = UINT32_MAX;
        for (uint32_t i = 0; i < share.count; ++i) { if (!strcmp(share.entries[i].name, "upload.c")) { selected = i; } }
        assert(selected != UINT32_MAX);
        assert(book8_share_download(&share, selected, milliseconds())); wait_state(SHARE_DOWNLOADED);
        assert(size == share.downloaded && !memcmp(expected, share.data, size));
        assert(book8_share_upload(&share, "upload.c", expected, (uint32_t)size, milliseconds())); wait_state(SHARE_FAILED);
        assert(share.status == 0xc0000035u);
        assert(book8_share_connect_auth(&share, &net, PEER, "public", "tester", "WORKGROUP", "synthetic-pass", milliseconds()));
        wait_state(SHARE_READY);
        assert(book8_share_upload(&share, "empty.bin", "", 0u, milliseconds())); wait_state(SHARE_UPLOADED);
    }
    free(expected);
    assert(book8_share_directory(&share, "", milliseconds())); wait_state(SHARE_READY);
    selected = UINT32_MAX;
    for (uint32_t i = 0u; i < share.count; ++i) { if (!strcmp(share.entries[i].name, "large.bin")) { selected = i; } }
    assert(selected != UINT32_MAX);
    assert(!book8_share_download(&share, selected, milliseconds()) && strstr(share.error, "1 MiB"));
    book8_share_close(&share);
    if (auth) { assert(book8_share_connect_auth(&share, &net, PEER, "missing", "tester", "WORKGROUP", "synthetic-pass", milliseconds())); }
    else { assert(book8_share_connect(&share, &net, PEER, "missing", milliseconds())); }
    wait_state(SHARE_FAILED); assert(share.status != 0u);
    book8_share_close(&share);
    if (auth) {
        assert(book8_share_connect_auth(&share, &net, PEER, "public", "tester", "WORKGROUP", "incorrect", milliseconds()));
        wait_state(SHARE_FAILED); assert(share.auth_failed && strstr(share.error, "Login failed"));
    }
    book8_share_close(&share);
    if (auth) { puts("PASS: real NTLMv2 login, byte-exact multi-write upload/download, empty upload, collision protection and rejected password"); }
    puts("PASS: real SMB2 server, BOOK8 TCP wire/stream, nested listing, exact multi-read download, bounds and missing-share error");
    return 0;
}
