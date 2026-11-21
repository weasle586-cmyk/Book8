#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net/net.h"

#define CHECK(c) do { if (!(c)) { \
    printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define LOCAL BOOK8_NET_IP(192, 168, 9, 10)
#define SERVER BOOK8_NET_IP(192, 168, 9, 20)
static struct book8_net net;
static uint8_t input[600], guarded[1026];
static size_t input_size;
static uint64_t now;
static unsigned rrqs, acks, idles, data_delivered;
static uint16_t client_port;
static bool pending, duplicate_sent, skipped, wrong_sent;
enum mode { NORMAL, LOST_RRQ, LOST_DATA, DUPLICATE, WRONG_TID, ERROR_REPLY,
    OVERFLOW, SILENT, EXACT, EMPTY_FILE, BAD_FIRST, FINAL_DUP, IO_FAIL, ARP_FIRST,
    WARM_IMAGE };
static enum mode mode;
static const uint8_t local_mac[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t server_mac[6] = {2, 0, 0, 0, 0, 2};

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8u) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8u); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v >> 16u)); put16(p + 2, (uint16_t)v);
}

static void data_packet(uint16_t block, size_t length, uint16_t port)
{
    memset(input, 0, sizeof(input));
    memcpy(input, local_mac, 6);
    memcpy(input + 6, server_mac, 6);
    put16(input + 12, 0x800);
    input[14] = 0x45;
    put16(input + 16, (uint16_t)(32 + length));
    input[22] = 64;
    input[23] = 17;
    put32(input + 26, SERVER);
    put32(input + 30, LOCAL);
    put16(input + 24, book8_net_checksum(input + 14, 20));
    put16(input + 34, port);
    put16(input + 36, client_port);
    put16(input + 38, (uint16_t)(12 + length));
    put16(input + 42, 3);
    put16(input + 44, block);
    for (size_t i = 0; i < length; ++i) {
        input[46 + i] = (uint8_t)(block + i);
    }
    input_size = 46 + length;
    pending = true;
}

static void arp_reply(void)
{
    memset(input, 0, sizeof(input));
    memcpy(input, local_mac, 6);
    memcpy(input + 6, server_mac, 6);
    put16(input + 12, 0x806);
    put16(input + 14, 1);
    put16(input + 16, 0x800);
    input[18] = 6; input[19] = 4;
    put16(input + 20, 2);
    memcpy(input + 22, server_mac, 6);
    put32(input + 28, SERVER);
    memcpy(input + 32, local_mac, 6);
    put32(input + 38, LOCAL);
    input_size = 42;
    pending = true;
}

static enum book8_net_result send_packet(void *ctx, const void *packet, size_t size)
{
    CHECK(ctx == &net && size >= 42);
    const uint8_t *p = packet;
    if (mode == IO_FAIL) {
        return BOOK8_NET_IO_ERROR;
    }
    if (get16(p + 12) == 0x806) {
        arp_reply();
        return BOOK8_NET_OK;
    }
    CHECK(get16(p + 12) == 0x800 && p[23] == 17);
    CHECK(book8_net_checksum(p + 14, 20) == 0);
    CHECK(tftp_fetch_kernel("nested", guarded + 1) == BOOK8_NET_BUSY);
    uint16_t opcode = get16(p + 42);
    client_port = get16(p + 34);
    CHECK(client_port >= 49152);
    if (opcode == 1) {
        CHECK(get16(p + 36) == 69 && size == 42 + 19);
        CHECK(memcmp(p + 44, "kernel.bin\0octet\0", 17) == 0);
        ++rrqs;
        if (mode == SILENT || (mode == LOST_RRQ && rrqs == 1)) {
            return BOOK8_NET_OK;
        }
        data_packet(mode == BAD_FIRST && rrqs == 1 ? 2 : 1,
            mode == EMPTY_FILE ? 0 : mode == WARM_IMAGE ? 64 : 512, 53000);
        if (mode == WARM_IMAGE) {
            const struct book8_warm_header h = {
                .magic = BOOK8_WARM_MAGIC, .version = 1, .header_bytes = 32,
                .image_bytes = 64, .memory_bytes = 128, .entry_offset = 32,
                .crc32 = 0xba345d41u /* Independent Python zlib CRC32 oracle. */
            };
            memset(input + 46, 0, 64);
            memcpy(input + 46, &h, sizeof(h));
            input[78] = 0xc3;
        }
        if (mode == ERROR_REPLY) {
            put16(input + 42, 5);
            put16(input + 44, 1);
            input[46] = 0;
            put16(input + 38, 13);
            put16(input + 16, 33);
            put16(input + 24, 0);
            put16(input + 24, book8_net_checksum(input + 14, 20));
            input_size = 47;
        }
    } else {
        CHECK(opcode == 4 && size == 46 && get16(p + 36) == 53000);
        uint16_t block = get16(p + 44);
        ++acks;
        if (block == 1 && mode != EMPTY_FILE && mode != WARM_IMAGE) {
            if (mode == LOST_DATA && !skipped) {
                skipped = true;
            } else if (mode == DUPLICATE && !duplicate_sent) {
                duplicate_sent = true;
                data_packet(1, 512, 53000);
            } else if (mode == WRONG_TID && !wrong_sent) {
                wrong_sent = true;
                data_packet(2, 7, 53001);
            } else {
                data_packet(2, mode == EXACT ? 0 : 7, 53000);
            }
        } else if (mode == FINAL_DUP && !duplicate_sent) {
            duplicate_sent = true;
            data_packet(block, 7, 53000);
        }
    }
    return BOOK8_NET_OK;
}

static enum book8_net_result receive_packet(void *ctx, void *packet,
    size_t capacity, size_t *length)
{
    CHECK(ctx == &net && capacity >= input_size);
    if (!pending) {
        return BOOK8_NET_EMPTY;
    }
    memcpy(packet, input, input_size);
    *length = input_size;
    pending = false;
    ++data_delivered;
    return BOOK8_NET_OK;
}

static uint64_t milliseconds(void *ctx) { CHECK(ctx == &now); return now; }
static void idle(void *ctx) { CHECK(ctx == &now); now += 250; CHECK(++idles < 200); }
static const struct book8_tftp_clock clock_hooks = {&now, milliseconds, idle};
static const struct book8_net_transport transport = {&net, send_packet, receive_packet};

static void setup(enum mode selected, size_t capacity)
{
    CHECK(book8_net_init(&net, &transport, local_mac) == BOOK8_NET_OK);
    CHECK(book8_net_configure(&net, LOCAL, 0xffffff00, 0) == BOOK8_NET_OK);
    mode = selected;
    if (mode != ARP_FIRST) {
        net.arp[0].ip = SERVER;
        memcpy(net.arp[0].mac, server_mac, 6);
    }
    rrqs = acks = idles = data_delivered = 0;
    now = 0;
    pending = duplicate_sent = skipped = wrong_sent = false;
    input_size = 0;
    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(tftp_client_init(&net, SERVER, capacity, &clock_hooks) == BOOK8_NET_OK);
}

static void transfers(void)
{
    const enum mode successful[] = {NORMAL, LOST_RRQ, LOST_DATA, DUPLICATE,
        WRONG_TID, EXACT, EMPTY_FILE, BAD_FIRST, FINAL_DUP, ARP_FIRST};
    for (unsigned test = 0; test < sizeof(successful) / sizeof(successful[0]); ++test) {
        setup(successful[test], sizeof(guarded) - 2);
        CHECK(tftp_fetch_kernel("kernel.bin", guarded + 1) == BOOK8_NET_OK);
        size_t expected = mode == EMPTY_FILE ? 0 : mode == EXACT ? 512 : 519;
        CHECK(tftp_fetched_size() == expected);
        CHECK(guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0xa5);
        for (size_t i = 0; i < expected; ++i) {
            CHECK(guarded[1 + i] == (uint8_t)((i < 512 ? 1 : 2) + i % 512));
        }
        CHECK(guarded[1 + expected] == 0xa5);
        CHECK(acks >= (expected < 512 ? 1u : 2u));
        if (mode == LOST_RRQ || mode == BAD_FIRST) { CHECK(rrqs == 2); }
        if (mode == DUPLICATE || mode == FINAL_DUP) { CHECK(acks >= 3); }
        for (unsigned i = 0; i < BOOK8_NET_SOCKET_COUNT; ++i) {
            CHECK(net.sockets[i].port == 0);
        }
    }
    setup(OVERFLOW, 518);
    CHECK(tftp_fetch_kernel("kernel.bin", guarded + 1) == BOOK8_NET_TOO_LARGE);
    CHECK(tftp_fetched_size() == 0 && guarded[519] == 0xa5);
    setup(SILENT, 1024);
    CHECK(tftp_fetch_kernel("kernel.bin", guarded + 1) == BOOK8_NET_TIMEOUT);
    CHECK(rrqs == BOOK8_TFTP_RETRIES + 1 && tftp_fetched_size() == 0);
    setup(ERROR_REPLY, 1024);
    CHECK(tftp_fetch_kernel("kernel.bin", guarded + 1) == BOOK8_NET_REMOTE_ERROR);
    CHECK(tftp_remote_error() == 1 && tftp_fetched_size() == 0);
    setup(IO_FAIL, 1024);
    CHECK(tftp_fetch_kernel("kernel.bin", guarded + 1) == BOOK8_NET_IO_ERROR);
    CHECK(tftp_fetched_size() == 0);
    setup(WARM_IMAGE, 1024);
    CHECK(tftp_fetch_kernel("kernel.bin", guarded + 1) == BOOK8_NET_OK);
    struct book8_warm_header fetched;
    CHECK(tftp_fetched_size() == 64);
    CHECK(book8_netboot_validate(guarded + 1, tftp_fetched_size(), 1024, &fetched)
        == BOOK8_NET_OK);
    CHECK(fetched.entry_offset == 32 && fetched.memory_bytes == 128);
    setup(NORMAL, 1024);
    CHECK(tftp_fetch_kernel("", guarded + 1) == BOOK8_NET_INVALID);
    CHECK(tftp_fetch_kernel("bad\nname", guarded + 1) == BOOK8_NET_INVALID);
    CHECK(tftp_fetch_kernel(NULL, guarded + 1) == BOOK8_NET_INVALID);
    CHECK(tftp_fetch_kernel("kernel.bin", NULL) == BOOK8_NET_INVALID);
    char long_name[BOOK8_TFTP_FILENAME_MAX + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    CHECK(tftp_fetch_kernel(long_name, guarded + 1) == BOOK8_NET_INVALID);
    CHECK(tftp_client_init(&net, SERVER, 0, &clock_hooks) == BOOK8_NET_INVALID);
    CHECK(tftp_client_init(&net, SERVER, BOOK8_TFTP_CAPACITY + 1, &clock_hooks)
        == BOOK8_NET_INVALID);
    CHECK(tftp_client_init(&net, LOCAL, 1024, &clock_hooks) == BOOK8_NET_INVALID);
    CHECK(tftp_client_init(&net, SERVER, 1024, NULL) == BOOK8_NET_INVALID);
    CHECK(book8_net_configure(&net, 0, 0, 0) == BOOK8_NET_OK);
    CHECK(tftp_client_init(&net, SERVER, 1024, &clock_hooks) == BOOK8_NET_UNCONFIGURED);
}

static void images(void)
{
    CHECK(sizeof(struct book8_tftp_rrq) == 2);
    CHECK(sizeof(struct book8_tftp_data) == 4);
    CHECK(sizeof(struct book8_tftp_ack) == 4);
    CHECK(sizeof(struct book8_tftp_error) == 4);
    CHECK(_Alignof(struct book8_tftp_data) == 1);
    uint8_t image[64] = {0};
    struct book8_warm_header header = {
        .magic = BOOK8_WARM_MAGIC, .version = 1, .header_bytes = 32,
        .image_bytes = 64, .memory_bytes = 128, .entry_offset = 32
    }, out;
    memcpy(image, &header, sizeof(header));
    image[32] = 0xc3;
    header.crc32 = book8_netboot_crc32(image, sizeof(image));
    CHECK(header.crc32 == 0xba345d41u);
    memcpy(image, &header, sizeof(header));
    CHECK(book8_netboot_validate(image, 64, 128, &out) == BOOK8_NET_OK);
    CHECK(out.entry_offset == 32 && out.memory_bytes == 128);
    image[33] ^= 1;
    CHECK(book8_netboot_validate(image, 64, 128, &out) == BOOK8_NET_BAD_IMAGE);
    image[33] ^= 1;
    CHECK(book8_netboot_validate(image, 63, 128, &out) == BOOK8_NET_BAD_IMAGE);
    CHECK(book8_netboot_validate(image, 64, 63, &out) == BOOK8_NET_BAD_IMAGE);
    for (unsigned field = 0; field < 7; ++field) {
        struct book8_warm_header bad = header;
        switch (field) {
        case 0: bad.magic = 0x464c457f; break;
        case 1: bad.version = 2; break;
        case 2: bad.header_bytes = 31; break;
        case 3: bad.image_bytes = 65; break;
        case 4: bad.memory_bytes = 129; break;
        case 5: bad.entry_offset = 64; break;
        case 6: bad.entry_offset = 31; break;
        }
        memcpy(image, &bad, sizeof(bad));
        bad.crc32 = book8_netboot_crc32(image, 64);
        memcpy(image, &bad, sizeof(bad));
        CHECK(book8_netboot_validate(image, 64, 128, &out) == BOOK8_NET_BAD_IMAGE);
    }
    puts("PASS: TFTP RRQ/DATA/ACK/ERROR, retries, duplicates, TID, bounds and warm images");
}

int main(void) { transfers(); images(); return 0; }
