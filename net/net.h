#ifndef BOOK8_NET_H
#define BOOK8_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK8_NET_FRAME_MAX 1514u
#define BOOK8_NET_UDP_MAX 1472u
#define BOOK8_NET_ARP_COUNT 8u
#define BOOK8_NET_SOCKET_COUNT 8u
#define BOOK8_NET_POLL_BUDGET 8u
#define BOOK8_NET_IP(a, b, c, d) \
    (((uint32_t)(a) << 24u) | ((uint32_t)(b) << 16u) \
        | ((uint32_t)(c) << 8u) | (uint32_t)(d))

/* ------------------------------------------------------------
 * Network & security feature flags (Phase 6).
 * These are compile-time switches; changing them requires a full
 * rebuild and regression test pass.
 * ------------------------------------------------------------ */

/* IPv6: 0 = disabled (no full stack, address/header stubs only).
   1 = reserved for a future full implementation. */
#ifndef BOOK8_NET_IPV6
#define BOOK8_NET_IPV6 0u
#endif

/* Firewall: 0 = disabled (default, legacy behavior).
   1 = compile in filtering; runtime toggle via book8_net_firewall_set().
   When active, drops unsolicited incoming IPv4 and allows established peers. */
#ifndef BOOK8_NET_FIREWALL
#define BOOK8_NET_FIREWALL 0u
#endif

/* DNS cache: in-memory only, no persistence across reboot. */
#ifndef BOOK8_NET_DNS_CACHE
#define BOOK8_NET_DNS_CACHE 1u
#endif
#define BOOK8_NET_DNS_CACHE_ENTRIES 8u
#define BOOK8_NET_DNS_CACHE_NAME_MAX 64u
#define BOOK8_NET_DNS_CACHE_TTL_MS 60000u

/* TLS placeholder: handshake omitted, certificate validation disabled.
   BROOK8 spelling is intentional (legacy compat identifier). */
#ifndef BROOK8_TLS_EMPTY
#define BROOK8_TLS_EMPTY 1u
#endif

/* BOOK8 is little-endian x86; API IPv4 addresses/ports are host-order values. */
static inline uint16_t book8_ntohs(uint16_t value) { return __builtin_bswap16(value); }
static inline uint32_t book8_ntohl(uint32_t value) { return __builtin_bswap32(value); }
static inline uint16_t book8_htons(uint16_t value) { return book8_ntohs(value); }
static inline uint32_t book8_htonl(uint32_t value) { return book8_ntohl(value); }
static inline uint64_t book8_ntohll(uint64_t value) { return __builtin_bswap64(value); }
static inline uint64_t book8_htonll(uint64_t value) { return book8_ntohll(value); }

#define BOOK8_TELEMETRY_MAGIC 0x424b3801u
#define BOOK8_TELEMETRY_MAGIC_V2 0x424b3802u
#define BOOK8_TELEMETRY_PORT 8080u
#define BOOK8_TELEMETRY_LOG_SIZE 128u
#define BOOK8_TELEMETRY_CPU_DESKTOP 1u
#define BOOK8_TELEMETRY_CPU_IRQ_ENABLED 2u
#define BOOK8_TELEMETRY_CPU_NIC_RUNNING 4u
/* v2 health_flags bits. */
#define BOOK8_HEALTH_IPV4 (1u << 0u)
#define BOOK8_HEALTH_CONFIG_BAKED (1u << 1u)
#define BOOK8_HEALTH_CONFIG_FILE (1u << 2u)
#define BOOK8_HEALTH_PUSH_AGENT (1u << 3u)
#define BOOK8_HEALTH_PHY_LINK (1u << 4u)
#define BOOK8_HEALTH_STORAGE (1u << 5u)
#define BOOK8_HEALTH_USB_KEYBOARD (1u << 6u)
#define BOOK8_HEALTH_USB_MOUSE (1u << 7u)
#define BOOK8_HEALTH_PS2_KEYBOARD (1u << 8u)
#define BOOK8_HEALTH_PS2_MOUSE (1u << 9u)
#define BOOK8_HEALTH_FRAMEBUFFER (1u << 10u)
#define BOOK8_HEALTH_PCI_SOUND (1u << 11u)
#define BOOK8_HEALTH_ACPI_S5 (1u << 12u)
#define BOOK8_HEALTH_RELAUNCHED (1u << 13u)

#pragma pack(push, 1)
struct net_telemetry_header {
    uint32_t magic, sequence;
    uint64_t timestamp_ms;
};
struct net_telemetry_metrics {
    uint32_t cpu_flags, cpu_mhz;
    uint64_t memory_total_bytes, kernel_reserved_bytes, heap_used_bytes, heap_total_bytes;
    uint32_t tx_packets, rx_packets, tx_dropped, rx_dropped, tx_timeouts;
};
struct net_telemetry_payload {
    struct net_telemetry_metrics metrics;
    char log[BOOK8_TELEMETRY_LOG_SIZE];
};
struct net_telemetry_packet {
    struct net_telemetry_header header;
    struct net_telemetry_payload payload;
};
/* v2 diagnostics appended after the unchanged v1 metrics block. */
struct net_telemetry_extended {
    uint32_t build_id, health_flags, relaunch_count, phy_status;
    uint32_t tx_ring_pending, rx_ring_ready;
    uint32_t net_rx_frames, net_tx_frames, net_invalid_frames, net_ignored_frames;
    uint32_t net_rx_errors, net_tx_errors, arp_replies, echo_replies, udp_received;
    uint32_t arp_entries, poll_iterations, poll_avg_us, poll_max_us;
    uint32_t telemetry_failures, push_accepted, push_rejected;
};
struct net_telemetry_payload_v2 {
    struct net_telemetry_metrics metrics;
    struct net_telemetry_extended extended;
    char log[BOOK8_TELEMETRY_LOG_SIZE];
};
struct net_telemetry_packet_v2 {
    struct net_telemetry_header header;
    struct net_telemetry_payload_v2 payload;
};
#pragma pack(pop)

struct book8_eth_header {
    uint8_t destination[6], source[6];
    uint16_t type;
} __attribute__((packed));

struct book8_arp_header {
    uint16_t hardware_type, protocol_type;
    uint8_t hardware_length, protocol_length;
    uint16_t operation;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

struct book8_ipv4_header {
    uint8_t version_ihl, tos;
    uint16_t total_length, identification, fragment;
    uint8_t ttl, protocol;
    uint16_t checksum;
    uint32_t source, destination;
} __attribute__((packed));

#if BOOK8_NET_IPV6
/* IPv6 header stub - layout only, no stack implementation.
   BOOK8_NET_IPV6 is 0 by default; this exists to freeze the wire ABI
   for a future full stack port. Addresses are 16 bytes in network order. */
struct book8_ipv6_header {
    uint32_t version_traffic_flow;
    uint16_t payload_length;
    uint8_t next_header, hop_limit;
    uint8_t source[16], destination[16];
} __attribute__((packed));
#define BOOK8_IPV6_HEADER_SIZE 40u
_Static_assert(sizeof(struct book8_ipv6_header) == BOOK8_IPV6_HEADER_SIZE,
    "IPv6 header size");
#endif /* BOOK8_NET_IPV6 */
/* Query is always available so callers can detect support at runtime. */
bool book8_net_ipv6_supported(void);

struct book8_icmp_header {
    uint8_t type, code;
    uint16_t checksum, identifier, sequence;
} __attribute__((packed));

struct book8_udp_header {
    uint16_t source, destination, length, checksum;
} __attribute__((packed));

enum book8_net_result {
    BOOK8_NET_OK,
    BOOK8_NET_EMPTY,
    BOOK8_NET_IGNORED,
    BOOK8_NET_INVALID,
    BOOK8_NET_UNCONFIGURED,
    BOOK8_NET_NO_ROUTE,
    BOOK8_NET_ARP_PENDING,
    BOOK8_NET_PORT_IN_USE,
    BOOK8_NET_NO_SLOTS,
    BOOK8_NET_NOT_BOUND,
    BOOK8_NET_IO_ERROR,
    BOOK8_NET_BUSY,
    BOOK8_NET_RX_DROPPED,
    BOOK8_NET_TIMEOUT,
    BOOK8_NET_REMOTE_ERROR,
    BOOK8_NET_TOO_LARGE,
    BOOK8_NET_BAD_IMAGE
};

struct book8_net_transport {
    void *ctx;
    enum book8_net_result (*send)(void *, const void *, size_t);
    enum book8_net_result (*receive)(void *, void *, size_t, size_t *);
};

/* Payload is borrowed only for the callback; send replies via book8_udp_send. */
typedef void (*book8_udp_callback)(void *ctx, uint32_t source_ip,
    uint16_t source_port, const uint8_t *payload, size_t length);

struct book8_udp_binding {
    uint16_t port;
    book8_udp_callback callback;
    void *ctx;
};

struct book8_arp_entry {
    uint32_t ip;
    uint8_t mac[6];
};

struct book8_net {
    struct book8_net_transport transport;
    uint8_t mac[6];
    uint32_t ip, mask, gateway;
    uint16_t next_id;
    struct book8_arp_entry arp[BOOK8_NET_ARP_COUNT];
    struct book8_udp_binding sockets[BOOK8_NET_SOCKET_COUNT];
    uint8_t arp_next;
    bool receiving, sending;
    uint32_t rx_frames, tx_frames, invalid_frames, ignored_frames;
    uint32_t rx_errors, tx_errors, arp_replies, echo_replies, udp_received;
    uint8_t rx[BOOK8_NET_FRAME_MAX], tx[BOOK8_NET_FRAME_MAX];
    void *tcp_context;
    enum book8_net_result (*tcp_input)(void *, uint32_t, const uint8_t *, size_t);
#if BOOK8_NET_FIREWALL
    /* Established-peer table: destinations of recent outgoing IPv4. */
    uint32_t fw_peers[8u];
    uint8_t fw_next;
    bool firewall_enabled;
    uint32_t fw_dropped;
#endif
};

enum book8_net_result book8_net_init(struct book8_net *net,
    const struct book8_net_transport *transport, const uint8_t mac[6]);
/* ip=mask=gateway=0 disables IPv4. Configuration changes flush the ARP cache. */
enum book8_net_result book8_net_configure(struct book8_net *net,
    uint32_t ip, uint32_t mask, uint32_t gateway);
bool book8_net_parse_ip(const char *text, uint32_t *ip);
uint16_t book8_net_checksum(const void *data, size_t length);
enum book8_net_result book8_net_input(struct book8_net *net,
    const void *frame, size_t length);
enum book8_net_result book8_net_poll(struct book8_net *net);
enum book8_net_result book8_net_send_ipv4(struct book8_net *, uint32_t destination,
    uint8_t protocol, const void *payload, size_t length);
enum book8_net_result book8_udp_bind(struct book8_net *net, uint16_t port,
    book8_udp_callback callback, void *ctx);
enum book8_net_result book8_udp_unbind(struct book8_net *net, uint16_t port);
/* On ARP_PENDING an ARP request was sent, not the datagram. Poll, then retry. */
enum book8_net_result book8_udp_send(struct book8_net *net, uint16_t local_port,
    uint32_t destination_ip, uint16_t destination_port,
    const void *payload, size_t length);
/* DHCP-only bootstrap broadcast: bound UDP/68 -> 67, source 0 before lease. */
enum book8_net_result book8_udp_dhcp_broadcast(struct book8_net *,
    uint32_t source_ip, const void *, size_t);
const char *book8_net_result_string(enum book8_net_result result);
/* Kernel integration: NULL until a supported NIC and transport are ready. */
struct book8_net *book8_kernel_network(void);
/* Single static dispatcher, serialized kernel context; target=0 stops it.
 * Metrics and timestamp are host order. Successful sends advance sequence;
 * ARP_PENDING/failures do not queue a sample or consume a sequence number. */
enum book8_net_result net_telemetry_init(struct book8_net *net, uint32_t target);
enum book8_net_result net_telemetry_send(uint64_t timestamp_ms,
    const struct net_telemetry_metrics *metrics, const char *log);
/* Same dispatcher/sequence, but emits the 292-byte v2 packet (magic 0x424B3802). */
enum book8_net_result net_telemetry_send_v2(uint64_t timestamp_ms,
    const struct net_telemetry_metrics *metrics,
    const struct net_telemetry_extended *extended, const char *log);
uint32_t book8_net_arp_entries(const struct book8_net *net);

/* In-memory DNS cache (Phase 6). No persistence across reboot; entries
   expire BOOK8_NET_DNS_CACHE_TTL_MS after insertion. Names are compared
   case-insensitively and truncated to BOOK8_NET_DNS_CACHE_NAME_MAX. */
struct book8_dns_cache_entry {
    char name[BOOK8_NET_DNS_CACHE_NAME_MAX];
    uint32_t ip;
    uint64_t expires_ms;
};
#if BOOK8_NET_DNS_CACHE
bool book8_dns_cache_lookup(const char *name, uint64_t now_ms, uint32_t *ip);
void book8_dns_cache_insert(const char *name, uint32_t ip, uint64_t now_ms);
void book8_dns_cache_flush(void);
uint32_t book8_dns_cache_entries(void);
#endif

/* Stateless firewall hook (Phase 6). Tracks destinations of outgoing IPv4
   as "established peers"; unsolicited IPv4 from unknown sources is dropped
   while enabled. DHCP replies (UDP 67->68) are always permitted. */
#if BOOK8_NET_FIREWALL
void book8_net_firewall_set(struct book8_net *net, bool enabled);
bool book8_net_firewall_enabled(const struct book8_net *net);
uint32_t book8_net_firewall_dropped(const struct book8_net *net);
void book8_net_firewall_peers_clear(struct book8_net *net);
#endif

/* TLS placeholder (Phase 6). The BROOK8 spelling is intentional.
   No handshake, no certificate validation - API exists only so callers
   can detect the stub and fail closed on HTTPS. */
#define BROOK8_TLS_HANDSHAKE_OMITTED 1u
#define BROOK8_TLS_CERT_VALIDATION_DISABLED 1u
bool brook8_tls_available(void);

/* Persisted network override text (A:\BOOK8NET.CFG), host-order addresses:
 *   BOOK8NET 1 / IP a.b.c.d / MASK a.b.c.d / GATEWAY a.b.c.d / TELEMETRY a.b.c.d
 * GATEWAY and TELEMETRY may be 0.0.0.0 or omitted. Unknown keys are rejected. */
#define BOOK8_NETCFG_TEXT_MAX 160u
struct book8_netcfg {
    uint32_t ip, mask, gateway, telemetry;
};
bool book8_netcfg_parse(const char *text, size_t length, struct book8_netcfg *config);
/* Returns bytes written (no NUL counted) or 0 if capacity is too small. */
size_t book8_netcfg_format(const struct book8_netcfg *config, char *text, size_t capacity);

#define BOOK8_TFTP_BLOCK 512u
#define BOOK8_TFTP_FILENAME_MAX 127u
#define BOOK8_TFTP_CAPACITY (8u * 1024u * 1024u)
#define BOOK8_TFTP_TIMEOUT_MS 1000u
#define BOOK8_TFTP_RETRIES 5u
/* Must stay above __kernel_end: the 128 KiB Code Editor document buffers make
 * the kernel image about 70.5 MiB. The 8 MiB staging window ends at 88 MiB. */
#define BOOK8_NETBOOT_ADDRESS 0x05000000ull
#define BOOK8_WARM_MAGIC 0x314d524157384b42ull /* "BK8WARM1", little endian */

#pragma pack(push, 1)
struct book8_tftp_rrq { uint16_t opcode; }; /* filename NUL, "octet" NUL follow */
struct book8_tftp_data { uint16_t opcode, block; }; /* up to 512 bytes follow */
struct book8_tftp_ack { uint16_t opcode, block; };
struct book8_tftp_error { uint16_t opcode, code; }; /* NUL-terminated text follows */
/* Little-endian executable envelope. CRC32 covers image_bytes with crc32=0. */
struct book8_warm_header {
    uint64_t magic;
    uint32_t version, header_bytes, image_bytes, memory_bytes, entry_offset, crc32;
};
#pragma pack(pop)

struct book8_tftp_clock {
    void *ctx;
    uint64_t (*milliseconds)(void *);
    void (*idle)(void *);
};

/* Persistent network/clock contexts required; capacity bounds every copy.
 * The clock must advance while polling. Fetch is synchronous, non-reentrant. */
enum book8_net_result tftp_client_init(struct book8_net *net, uint32_t server,
    size_t capacity, const struct book8_tftp_clock *clock);
enum book8_net_result tftp_fetch_kernel(const char *filename, uint8_t *load_address);
size_t tftp_fetched_size(void);
uint16_t tftp_remote_error(void);
uint32_t book8_netboot_crc32(const void *image, size_t length);
enum book8_net_result book8_netboot_validate(const void *image, size_t length,
    size_t capacity, struct book8_warm_header *header);

/* Warm entry ABI: CPL0 long mode, low 4 GiB identity mapping, IF=0/DF=0,
 * RDI=BOOK8_WARM_MAGIC, RSI=&info, fresh 16 KiB stack, bus mastering disabled.
 * The image must install its own descriptors/interrupts/drivers; never return. */
struct book8_warm_info {
    uint64_t magic, version, load_address, image_bytes, memory_bytes, cr3;
};
void book8_netboot_jump(uint64_t entry, uint64_t stack_top,
    const struct book8_warm_info *info) __attribute__((noreturn));

#endif
