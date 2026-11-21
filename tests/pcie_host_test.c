#include "../drivers/pcie.h"
#include "../drivers/amd_host_bridge.h"
#include "../drivers/pcie_probe.h"
#include "../net/net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct book8_pcie_state state;
static uint32_t config[2][32][8][64];
static uint8_t *ecam;
static uint64_t mapped_start, mapped_size;
static unsigned reads, writes, delays, logs;
static bool stuck, delay_failure, map_failure;
static bool command_stuck;
static unsigned index_reads, l0_after;
static bool irq_active, data_unavailable;
static char last_log[128];
static char events[128][128];
static unsigned driver_probes;

static uint32_t legacy_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t reg)
{
    ++reads;
    return bus < 2u ? config[bus][slot][fn][reg / 4u] : UINT32_MAX;
}

static void legacy_write(uint8_t bus, uint8_t slot, uint8_t fn,
    uint8_t reg, uint32_t value)
{
    ++writes;
    assert(bus < 2u);
    config[bus][slot][fn][reg / 4u] = value;
}

static void legacy_write16(uint8_t bus, uint8_t slot, uint8_t fn,
    uint8_t reg, uint16_t value)
{
    ++writes;
    assert(bus < 2u);
    uint32_t *p = &config[bus][slot][fn][reg / 4u];
    if (reg == 0x44u && stuck)
        return;
    if (reg == 4u && bus == 1u && command_stuck)
        return;
    uint32_t shift = (reg & 2u) * 8u;
    if (reg == 4u) {
        *p = (*p & 0xffff0000u) | value;
    } else if (reg == 0x44u) {
        /* PME_Status remains set unless explicitly acknowledged. */
        *p = (*p & 0xffff8000u) | (value & ~0x8000u);
    } else {
        *p = (*p & ~(0xffffu << shift)) | ((uint32_t)value << shift);
    }
}

static volatile uint8_t *map(uint64_t start, uint64_t size)
{
    mapped_start = start;
    mapped_size = size;
    assert(size <= 0x200000u);
    return map_failure ? NULL : ecam;
}

static void log_event(const char *text)
{
    ++logs;
    snprintf(last_log, sizeof(last_log), "%s", text);
    if (logs <= 128u)
        snprintf(events[logs - 1u], sizeof(events[0]), "%s", text);
}

static bool delay_ms(uint32_t ms)
{
    assert(ms == 10u);
    ++delays;
    return !delay_failure;
}

static uint32_t *root_config(void)
{
    return (uint32_t *)(ecam + (2u << 15u));
}

static uint64_t irq_save(void)
{
    assert(!irq_active);
    irq_active = true;
    ++index_reads;
    root_config()[0xe4u / 4u] = data_unavailable ? UINT32_MAX
        : index_reads > l0_after ? 0xabc00010u : 0x6u;
    return 0x200u;
}

static void irq_restore(uint64_t flags)
{
    assert(irq_active && flags == 0x200u);
    assert(root_config()[0xe0u / 4u] == 0x52u);
    irq_active = false;
}

static uint32_t read32_safe(volatile uint32_t *address, uint32_t *value)
{
    assert(irq_active);
    *value = *address;
    return 0u;
}

static uint32_t write32_safe(volatile uint32_t *address, uint32_t value)
{
    assert(irq_active && address == root_config() + 0xe0u / 4u);
    *address = value;
    return 0u;
}

static const struct book8_pcie_platform platform = {
    .legacy_read = legacy_read, .legacy_write = legacy_write,
    .legacy_write16 = legacy_write16, .map = map, .log = log_event,
    .delay_ms = delay_ms,
    .read32_safe = read32_safe, .write32_safe = write32_safe,
    .irq_save = irq_save, .irq_restore = irq_restore
};

static void reset(void)
{
    memset(config, 0xff, sizeof(config));
    memset(ecam, 0xff, 0x200000u);
    reads = writes = delays = logs = 0u;
    stuck = delay_failure = map_failure = false;
    command_stuck = false;
    index_reads = l0_after = 0u;
    irq_active = data_unavailable = false;
    driver_probes = 0u;
    book8_pcie_init(&state, &platform);
}

static void checksum(uint8_t *table, size_t length)
{
    table[9] = 0u;
    uint8_t sum = 0u;
    for (size_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + table[i]);
    table[9] = (uint8_t)(0u - sum);
}

static void make_mcfg(uint8_t *table, uint8_t first, uint8_t last,
    uint16_t segment)
{
    memset(table, 0, 60u);
    memcpy(table, "MCFG", 4u);
    table[4] = 60u;
    uint64_t base = 0xe0000000ull;
    memcpy(table + 44u, &base, 8u);
    memcpy(table + 52u, &segment, 2u);
    table[54] = first;
    table[55] = last;
    checksum(table, 60u);
}

static void test_ecam(void)
{
    reset();
    uint8_t table[76];
    make_mcfg(table, 1u, 2u, 3u);
    assert(book8_pcie_add_mcfg(&state, table, 60u));
    assert(mapped_start == 0xe0100000ull && mapped_size == 0x200000u);
    uint32_t value = 0x12345678u;
    memcpy(ecam + 0x100000u + (4u << 15u) + (2u << 12u) + 0xabcu,
        &value, 4u);
    assert(book8_pcie_read32(&state, 3u, 2u, 4u, 2u, 0xabcu) == value);
    assert(reads == 0u);
    book8_pcie_write16(&state, 3u, 2u, 4u, 2u, 0xabeu, 0x9876u);
    assert(book8_pcie_read32(&state, 3u, 2u, 4u, 2u, 0xabcu) == 0x98765678u);
    book8_pcie_write32(&state, 3u, 1u, 0u, 0u, 0x100u, 0xabcdef01u);
    assert(book8_pcie_read32(&state, 3u, 1u, 0u, 0u, 0x100u) == 0xabcdef01u);
    assert(writes == 0u);
    assert(book8_pcie_read32(&state, 0u, 0u, 0u, 0u, 0u) == UINT32_MAX);
    assert(reads == 1u);
    unsigned before = logs;
    assert(book8_pcie_read32(&state, 3u, 2u, 32u, 0u, 0u) == UINT32_MAX);
    assert(book8_pcie_read32(&state, 3u, 2u, 0u, 0u, 0xfffu) == UINT32_MAX);
    assert(book8_pcie_read32(&state, 3u, 0u, 0u, 0u, 0u) == UINT32_MAX);
    book8_pcie_write32(&state, 3u, 0u, 0u, 0u, 0u, 0u);
    assert(logs == before + 4u && writes == 0u);

    reset();
    make_mcfg(table, 0u, 1u, 0u);
    table[9]++;
    assert(!book8_pcie_add_mcfg(&state, table, 60u));
    assert(state.ecam_count == 0u);
    checksum(table, 60u);
    assert(!book8_pcie_add_mcfg(&state, table, 59u));
    table[54] = 2u;
    checksum(table, 60u);
    assert(!book8_pcie_add_mcfg(&state, table, 60u));
    make_mcfg(table, 0u, 1u, 0u);
    uint64_t above = 0x100000000ull;
    memcpy(table + 44u, &above, 8u);
    checksum(table, 60u);
    assert(!book8_pcie_add_mcfg(&state, table, 60u));
    make_mcfg(table, 0u, 1u, 0u);
    memcpy(table + 60u, table + 44u, 16u);
    table[4] = 76u;
    checksum(table, 76u);
    assert(!book8_pcie_add_mcfg(&state, table, 76u));
    make_mcfg(table, 0u, 1u, 0u);
    map_failure = true;
    assert(!book8_pcie_add_mcfg(&state, table, 60u));
    assert(state.ecam_count == 0u);
}

static void target_fixture(void)
{
    reset();
    uint8_t table[60];
    make_mcfg(table, 0u, 0u, 0u);
    assert(book8_pcie_add_mcfg(&state, table, sizeof(table)));
    uint32_t *host = (uint32_t *)ecam;
    memset(host, 0, 256u);
    host[0] = 0x96001022u;
    host[2] = 0x06000000u;
    for (unsigned fn = 0u; fn < 6u; ++fn) {
        uint32_t *cpu = (uint32_t *)(ecam + (0x18u << 15u) + (fn << 12u));
        memset(cpu, 0, 256u);
        cpu[0] = ((0x1600u + fn) << 16u) | 0x1022u;
        cpu[2] = 0x06000000u;
        if (fn == 0u)
            cpu[3] = 0x00800000u;
    }
    uint32_t *root = root_config();
    memset(root, 0, 256u);
    root[0] = 0x96031022u;
    root[2] = 0x06040000u;
    root[3] = 0x00010000u;
    root[6] = 0x00010100u;
    root[1] = 0x80100000u;
    root[0x34u / 4u] = 0x40u;
    root[0x40u / 4u] = 0x00030001u;
    root[0x44u / 4u] = 0x8100u;
    root[0xe0u / 4u] = 0x52u;
    root[0xe4u / 4u] = 0x10u;
    uint32_t *gpu = config[1][0][0];
    memset(gpu, 0, 256u);
    gpu[0] = BOOK8_PCIE_TARGET_ID;
    gpu[2] = 0x03000000u;
    gpu[1] = 0x00100000u;
    gpu[0x34u / 4u] = 0x40u;
    gpu[0x40u / 4u] = 0x00030001u;
    gpu[0x44u / 4u] = 0x8003u;
    gpu[4] = 0xd0000000u;
    gpu[5] = 0xc000000cu;
    gpu[0x3cu / 4u] = 0x0000010bu;
    book8_pcie_enumerate(&state);
    assert(state.node_count == 9u && state.target_present);
    assert(writes == 0u);
}

static bool nvidia_probe(struct book8_pcie_state *pcie,
    struct book8_pcie_node *node, volatile uint8_t *bar0)
{
    assert(pcie == &state && node->id == BOOK8_PCIE_TARGET_ID);
    assert(bar0 == ecam && node->bars[0].base == 0xd0000000u);
    ++driver_probes;
    return true;
}

static void test_driver_binding(void)
{
    static const struct book8_pcie_match matches[] = {
        {.vendor_id = 0x10deu, .device_id = 0x13c2u,
            .class_code = 0x020000u, .class_mask = 0xff0000u},
        {.vendor_id = 0x10deu, .device_id = 0x13c2u,
            .class_code = 0x030000u, .class_mask = 0xff0000u},
    };
    static const struct book8_pcie_driver drivers[] = {
        {.name = "nvidia-test", .matches = matches,
            .match_count = sizeof(matches) / sizeof(matches[0]),
            .bar0_window_size = 0x204u, .probe = nvidia_probe},
    };
    target_fixture();
    const struct book8_pcie_node *gpu = NULL;
    for (size_t i = 0u; i < state.node_count; ++i) {
        if (state.nodes[i].bus == 1u && state.nodes[i].slot == 0u
            && state.nodes[i].function == 0u) {
            gpu = &state.nodes[i];
            break;
        }
    }
    assert(gpu != NULL && gpu->bar_count == BOOK8_PCIE_BAR_COUNT);
    assert(gpu->bars[0].valid && !gpu->bars[0].io
        && !gpu->bars[0].is_64 && gpu->bars[0].base == 0xd0000000u);
    assert(gpu->bars[1].valid && gpu->bars[1].is_64
        && gpu->bars[1].prefetchable && gpu->bars[1].base == 0xc0000000u);
    assert(gpu->irq_line == 0x0bu && gpu->irq_pin == 1u);
    assert(book8_pcie_bind_drivers(&state, drivers,
        sizeof(drivers) / sizeof(drivers[0])) == 1u);
    assert(driver_probes == 1u && state.driver_count == 1u);
    assert(gpu->driver == &drivers[0]);
    assert(mapped_start == 0xd0000000u && mapped_size == 0x204u);
}

static uint8_t frame[BOOK8_NET_FRAME_MAX];
static size_t frame_size;
static bool send_failure;

static enum book8_net_result send_frame(void *ctx, const void *data, size_t size)
{
    (void)ctx;
    if (send_failure)
        return BOOK8_NET_IO_ERROR;
    assert(size <= sizeof(frame));
    memcpy(frame, data, size);
    frame_size = size;
    return BOOK8_NET_OK;
}

static enum book8_net_result receive_frame(void *ctx, void *data,
    size_t capacity, size_t *size)
{
    (void)ctx;
    (void)data;
    (void)capacity;
    *size = 0u;
    return BOOK8_NET_EMPTY;
}

static void test_event_telemetry(void)
{
    struct book8_net net;
    const struct book8_net_transport transport = {
        .send = send_frame, .receive = receive_frame};
    const uint8_t mac[6] = {2u, 0u, 0u, 0u, 0u, 1u};
    uint32_t peer = BOOK8_NET_IP(192, 168, 50, 17);
    assert(book8_net_init(&net, &transport, mac) == BOOK8_NET_OK);
    assert(book8_net_configure(&net, BOOK8_NET_IP(192, 168, 50, 208),
        BOOK8_NET_IP(255, 255, 255, 0), 0u) == BOOK8_NET_OK);
    net.arp[0].ip = peer;
    memcpy(net.arp[0].mac, mac, 6u);
    assert(net_telemetry_init(&net, peer) == BOOK8_NET_OK);
    const struct net_telemetry_metrics metrics = {.cpu_mhz = 4000u};
    const struct net_telemetry_extended extended = {.build_id = 123u};
    target_fixture();
    assert(book8_pcie_prepare_target(&state));
    assert(strstr(last_log, "TARGET ready") != NULL);
    send_failure = true;
    assert(net_telemetry_send_v2(10u, &metrics, &extended, last_log)
        == BOOK8_NET_IO_ERROR);
    send_failure = false;
    assert(net_telemetry_send_v2(20u, &metrics, &extended, last_log)
        == BOOK8_NET_OK);
    size_t offset = sizeof(struct book8_eth_header)
        + sizeof(struct book8_ipv4_header);
    struct book8_udp_header udp;
    memcpy(&udp, frame + offset, sizeof(udp));
    assert(book8_ntohs(udp.source) == 8080u
        && book8_ntohs(udp.destination) == 8080u);
    offset += sizeof(udp);
    struct net_telemetry_packet_v2 packet;
    assert(sizeof(packet) == 292u && frame_size == offset + sizeof(packet));
    memcpy(&packet, frame + offset, sizeof(packet));
    assert(book8_ntohl(packet.header.magic) == BOOK8_TELEMETRY_MAGIC_V2);
    assert(book8_ntohl(packet.header.sequence) == 0u);
    assert(book8_ntohll(packet.header.timestamp_ms) == 20u);
    assert(book8_ntohl(packet.payload.metrics.cpu_mhz) == 4000u);
    assert(book8_ntohl(packet.payload.extended.build_id) == 123u);
    assert(strcmp(packet.payload.log, last_log) == 0);
    assert(net_telemetry_init(&net, 0u) == BOOK8_NET_OK);
}

static void test_target(void)
{
    target_fixture();
    uint32_t before[6];
    memcpy(before, config[1][0][0] + 4u, sizeof(before));
    assert(book8_pcie_prepare_target(&state));
    assert(delays == 1u);
    assert(root_config()[1] == 0x80100006u);
    assert(root_config()[0x44u / 4u] == 0x8100u);
    assert(config[1][0][0][0x44u / 4u] == 0x8000u);
    assert(config[1][0][0][1] == 0x00100006u);
    assert(memcmp(before, config[1][0][0] + 4u, sizeof(before)) == 0);
    assert(state.target_ready && strstr(last_log, "TARGET ready") != NULL);
    assert(writes == 2u);
    assert(state.target_link_verified && state.amd_host_verified && !irq_active);
    unsigned command_event = 0u, bar_event = 0u;
    for (unsigned i = 0u; i < logs; ++i) {
        if (strstr(events[i], "TARGET-COMMAND") != NULL)
            command_event = i;
        if (strstr(events[i], "BAR0-RAW") != NULL)
            bar_event = i;
    }
    assert(command_event != 0u && bar_event > command_event);

    target_fixture();
    config[1][0][0][4] = UINT32_MAX;
    assert(!book8_pcie_prepare_target(&state));
    assert(!state.target_ready);

    target_fixture();
    for (size_t i = 0u; i < state.node_count; ++i)
        if (state.nodes[i].bus == 1u)
            state.nodes[i].class_revision = 0u;
    assert(!book8_pcie_prepare_target(&state));
    assert(!state.target_ready);

    target_fixture();
    command_stuck = true;
    assert(!book8_pcie_enable_target_command(&state));
    assert(config[1][0][0][1] == 0x00100000u);
    assert(strstr(last_log, "BAR/MMIO access blocked") != NULL);

    target_fixture();
    config[1][0][0][1] = UINT32_MAX;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u);

    target_fixture();
    config[1][0][0][0] = 0x123410deu;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u);

    target_fixture();
    stuck = true;
    assert(!book8_pcie_prepare_target(&state));
    assert(delays == 50u && writes == 1u);
    assert(strstr(last_log, "timed out") != NULL);

    target_fixture();
    delay_failure = true;
    assert(!book8_pcie_prepare_target(&state));
    assert(delays == 1u);

    target_fixture();
    config[1][0][0][0x40u / 4u] = 0x4005u;
    assert(!book8_pcie_prepare_target(&state));
    assert(strstr(last_log, "D0/command") != NULL);

    target_fixture();
    state.nodes[1].primary = 2u;
    assert(!book8_pcie_prepare_target(&state));
    assert(writes == 0u && strstr(last_log, "topology") != NULL);

    target_fixture();
    state.nodes[9] = state.nodes[1];
    state.node_count = 10u;
    assert(!book8_pcie_prepare_target(&state));
    assert(writes == 0u);

    target_fixture();
    config[1][0][0][4] = 0u;
    assert(!book8_pcie_prepare_target(&state));
    assert(strstr(last_log, "BAR assignment") != NULL);

    reset();
    book8_pcie_enumerate(&state);
    assert(!book8_pcie_prepare_target(&state));
    assert(writes == 0u && !state.target_present);
}

static void test_ltssm(void)
{
    target_fixture();
    l0_after = 3u;
    assert(book8_pcie_enable_target_command(&state));
    assert(index_reads == 4u && delays == 3u);
    assert(config[1][0][0][1] == 0x00100006u);
    assert(root_config()[0xe0u / 4u] == 0x52u);
    assert(root_config()[0xe4u / 4u] == 0xabc00010u);
    assert(!irq_active && state.target_link_verified);

    target_fixture();
    l0_after = 1000u;
    assert(!book8_pcie_prepare_target(&state));
    assert(index_reads == 50u && delays == 50u);
    assert(writes == 0u && root_config()[1] == 0x80100000u);
    assert(config[1][0][0][1] == 0x00100000u);
    assert(!state.target_link_verified && !state.target_ready);
    /* Direct callers cannot bypass the link prerequisite after a failure. */
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u);

    target_fixture();
    data_unavailable = true;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u && !irq_active && !state.target_link_verified);
    assert(root_config()[0xe0u / 4u] == 0x52u);

    target_fixture();
    state.ecam_count = 0u;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u && index_reads == 0u);

    target_fixture();
    root_config()[0] = 0x96021022u;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u && index_reads == 0u);

    target_fixture();
    *(uint32_t *)(ecam + (0x18u << 15u) + (5u << 12u)) = 0x12341022u;
    assert(book8_pcie_enable_target_command(&state));
    assert(writes == 1u && index_reads > 0u);

    target_fixture();
    state.platform.irq_save = NULL;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u && index_reads == 0u);

    target_fixture();
    l0_after = 1u;
    delay_failure = true;
    assert(!book8_pcie_enable_target_command(&state));
    assert(writes == 0u && delays == 1u && index_reads == 1u);

}

static struct book8_pcie_probe probe;
static unsigned probe_accesses, probe_stores, probe_sends, probe_fault_at;
static bool probe_send_ok, probe_mismatch;
static char probe_packet[128];

static bool probe_emit(const char *record)
{
    assert(!irq_active);
    assert(strlen(record) < sizeof(probe_packet));
    ++probe_sends;
    snprintf(probe_packet, sizeof(probe_packet), "%s", record);
    return probe_send_ok;
}

static uint32_t probe_read(volatile uint32_t *address, uint32_t *value)
{
    assert(irq_active);
    assert(address >= root_config() && address < root_config() + 64u);
    if (++probe_accesses == probe_fault_at)
        return 14u;
    *value = *address;
    if (probe_mismatch && probe_accesses == 3u)
        *value = 0x52u;
    return 0u;
}

static uint32_t probe_write(volatile uint32_t *address, uint32_t value)
{
    assert(irq_active && address == root_config() + 0xe0u / 4u);
    ++probe_stores;
    if (++probe_accesses == probe_fault_at)
        return 13u;
    *address = value;
    return 0u;
}

static uint64_t probe_irq_save(void)
{
    assert(!irq_active);
    irq_active = true;
    return 0x200u;
}

static void probe_irq_restore(uint64_t flags)
{
    assert(irq_active && flags == 0x200u);
    irq_active = false;
}

static void probe_fixture(void)
{
    target_fixture();
    assert(book8_amd_root_link_ready(&state));
    state.platform.irq_save = probe_irq_save;
    state.platform.irq_restore = probe_irq_restore;
    const struct book8_probe_ops ops = {
        .emit = probe_emit, .read = probe_read, .write = probe_write
    };
    book8_probe_init(&probe, &state, &ops);
    probe_accesses = probe_stores = probe_sends = probe_fault_at = 0u;
    probe_send_ok = true;
    probe_mismatch = false;
}

static void test_single_probe(void)
{
    probe_fixture();
    assert(!book8_probe_begin(&probe, 0xa4u, 0u));
    assert(!book8_probe_begin(&probe, 0xb0u, 0u));
    assert(probe_accesses == 0u && probe_sends == 0u);
    probe_send_ok = false;
    assert(book8_probe_begin(&probe, 0xa5u, 0u));
    assert(probe.phase == BOOK8_PROBE_INTENT && !probe.sent);
    assert(strstr(probe_packet, "INTENT 0000:00:02.0 E0/E4 idx=a5") != NULL);
    assert(!book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(probe_accesses == 0u);
    probe_send_ok = true;
    assert(book8_probe_retry(&probe));
    assert(!book8_probe_ack(&probe, 2u, BOOK8_PROBE_INTENT));
    assert(!book8_probe_ack(&probe, 1u, BOOK8_PROBE_RESULT));
    assert(!book8_probe_begin(&probe, 0xa6u, 0u));
    assert(probe_accesses == 0u);
    probe_send_ok = false;
    assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(probe_accesses == 6u && probe_stores == 2u);
    assert(probe.value_valid && probe.error == 0u && probe.exception == 0u);
    assert(probe.restore_error == 0u && root_config()[0xe0u / 4u] == 0x52u);
    assert(probe.phase == BOOK8_PROBE_RESULT && !probe.sent);
    assert(strstr(probe_packet, "RESULT idx=a5 step=data valid=1") != NULL);
    assert(!book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(!book8_probe_ack(&probe, 1u, BOOK8_PROBE_RESULT));
    assert(!book8_probe_retry(&probe));
    probe_send_ok = true;
    assert(book8_probe_retry(&probe));
    assert(book8_probe_retry(&probe));
    assert(!book8_probe_begin(&probe, 0xafu, 0u));
    assert(probe_accesses == 6u);
    assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_RESULT));
    assert(probe.phase == BOOK8_PROBE_IDLE && probe_accesses == 6u);
    assert(!book8_probe_ack(&probe, 1u, BOOK8_PROBE_RESULT));
    assert(book8_probe_begin(&probe, 0xafu, 0u));
    assert(probe.id == 2u && probe_accesses == 6u);
    assert(!book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(book8_probe_ack(&probe, 2u, BOOK8_PROBE_INTENT));
    assert(probe_accesses == 12u && probe.index == 0xafu);

    probe_fixture();
    assert(book8_probe_begin(&probe, 0xa6u, 1u));
    assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(probe.error == 1u && probe_accesses == 0u && probe_stores == 0u);
    assert(strstr(probe_packet, "errno=1") != NULL);

    for (unsigned gate = 0u; gate < 4u; ++gate) {
        probe_fixture();
        if (gate == 0u) state.amd_host_verified = false;
        if (gate == 1u) state.ecam_count = 0u;
        if (gate == 2u) state.platform.irq_save = NULL;
        if (gate == 3u) probe.ops.write = NULL;
        assert(book8_probe_begin(&probe, 0xa5u, 0u));
        assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
        assert(probe.error == 19u && probe_accesses == 0u);
        assert(strstr(probe_packet, "errno=19") != NULL);
    }

    for (unsigned fault = 1u; fault <= 6u; ++fault) {
        probe_fixture();
        probe_fault_at = fault;
        assert(book8_probe_begin(&probe, 0xa5u, 0u));
        assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
        assert(!irq_active);
        if (fault < 5u) {
            assert(probe.exception == (fault == 2u ? 13u : 14u));
            assert(probe.error == 5u && !probe.value_valid && !probe.poisoned);
            assert(root_config()[0xe0u / 4u] == 0x52u);
        } else {
            assert(probe.restore_error == 5u && probe.poisoned);
            assert(probe.restore_exception == (fault == 5u ? 13u : 14u));
            assert(!state.amd_host_verified && !state.target_link_verified);
            assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_RESULT));
            unsigned before = probe_accesses;
            assert(!book8_probe_begin(&probe, 0xa6u, 0u));
            assert(probe_accesses == before);
            assert(!book8_amd_root_link_ready(&state));
            assert(state.rs780_access_blocked && probe_accesses == before);
        }
    }
    probe_fixture();
    probe_mismatch = true;
    assert(book8_probe_begin(&probe, 0xa5u, 0u));
    assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(probe.error == 5u && !probe.value_valid && probe_accesses == 5u);
    assert(root_config()[0xe0u / 4u] == 0x52u);

    probe_fixture();
    root_config()[0xe4u / 4u] = UINT32_MAX;
    assert(book8_probe_begin(&probe, 0xa5u, 0u));
    assert(book8_probe_ack(&probe, 1u, BOOK8_PROBE_INTENT));
    assert(probe.error == 19u && !probe.value_valid && probe.restore_error == 0u);

    probe_fixture();
    probe.id = UINT32_MAX - 1u;
    probe_fault_at = 3u;
    assert(book8_probe_begin(&probe, 0xafu, 0u));
    assert(book8_probe_ack(&probe, UINT32_MAX, BOOK8_PROBE_INTENT));
    assert(strstr(probe_packet, "id=4294967295") != NULL);
    assert(strstr(probe_packet, "restore-errno=0") != NULL);
    assert(strlen(probe_packet) < 128u);

    probe_fixture();
    probe.id = UINT32_MAX;
    assert(!book8_probe_begin(&probe, 0xa5u, 0u));
    assert(probe_accesses == 0u);
}

static void link_fixture(void)
{
    probe_fixture();
    uint32_t *root = root_config();
    root[0x40u / 4u] = 0x00036001u; /* PM capability followed by PCIe at 60. */
    root[0x60u / 4u] = 0x00410010u;
    root[0x6cu / 4u] = (1u << 20u) | (16u << 4u) | 2u;
    root[0x70u / 4u] = ((1u << 13u) | (16u << 4u) | 1u) << 16u;
}

static void complete_link(bool status)
{
    assert(book8_probe_begin_link(&probe, status));
    unsigned before = probe_accesses;
    assert(probe.phase == BOOK8_PROBE_INTENT && probe_accesses == before);
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
    assert(probe_accesses == before + 5u && probe_stores == 0u);
    assert(!book8_probe_link_verified(&probe));
    assert(probe.value_valid && probe.error == 0u);
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));
}

static void test_link_diagnostics(void)
{
    link_fixture();
    assert(!book8_probe_link_verified(&probe));
    complete_link(false);
    assert(probe.offset == 0x6cu && probe.link_cap_acknowledged);
    assert(strstr(probe_packet, "target=LINKCAP off=06c") != NULL);
    complete_link(true);
    assert(probe.offset == 0x70u && probe.link_status_acknowledged);
    assert(!book8_probe_link_verified(&probe)); /* No acknowledged LTSSM sample. */
    unsigned stores = probe_stores;
    assert(book8_probe_begin(&probe, BOOK8_RS780_LC_STATE0, 0u));
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
    assert(!probe.ltssm_acknowledged);
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));
    assert(book8_probe_link_verified(&probe) && !state.target_ready);
    assert(probe_stores == stores + 2u);
    assert(book8_probe_begin(&probe, BOOK8_RS780_LINK_WIDTH, 0u));
    assert(!book8_probe_link_verified(&probe)); /* Pending transaction. */
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));
    assert(book8_probe_link_verified(&probe));

    uint32_t good_status = probe.link_status;
    for (unsigned bad = 0u; bad < 6u; ++bad) {
        uint32_t status = good_status >> 16u;
        if (bad == 0u) status &= ~0xfu;          /* No negotiated speed. */
        if (bad == 1u) status &= ~(0x3fu << 4u); /* No negotiated lanes. */
        if (bad == 2u) status = (status & ~0xfu) | 3u;
        if (bad == 3u) status = (status & ~(0x3fu << 4u)) | (32u << 4u);
        if (bad == 4u) status |= 1u << 11u;     /* Training. */
        if (bad == 5u) status &= ~(1u << 13u);  /* DLL inactive. */
        probe.link_status = status << 16u;
        assert(!book8_probe_link_verified(&probe));
    }
    probe.link_cap &= ~(1u << 20u); /* Optional DLL reporting unsupported. */
    assert(book8_probe_link_verified(&probe));
    probe.link_status = good_status;
    assert(book8_probe_begin_link(&probe, false));
    assert(!probe.link_cap_acknowledged && !book8_probe_link_verified(&probe));
    probe_send_ok = false;
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
    unsigned accesses = probe_accesses;
    assert(!book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));
    assert(!book8_probe_retry(&probe));
    probe_send_ok = true;
    assert(book8_probe_retry(&probe) && probe_accesses == accesses);
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));

    for (unsigned fault = 1u; fault <= 5u; ++fault) {
        link_fixture();
        probe_fault_at = fault;
        assert(book8_probe_begin_link(&probe, false));
        assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
        assert(probe.exception == 14u && probe.error == 5u && !probe.value_valid);
        assert(probe_accesses == fault && probe_stores == 0u && !irq_active);
        assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));
        assert(!probe.link_cap_acknowledged && !book8_probe_link_verified(&probe));
    }
    for (unsigned malformed = 0u; malformed < 9u; ++malformed) {
        link_fixture();
        uint32_t *root = root_config();
        if (malformed == 0u) root[0x34u / 4u] = 0x41u;
        if (malformed == 1u) root[0x34u / 4u] = 0x3cu;
        if (malformed == 2u) root[0x40u / 4u] = 0x00034001u; /* Cycle. */
        if (malformed == 3u) root[0x60u / 4u] = 0x00400010u; /* Version zero. */
        if (malformed == 4u) root[0x60u / 4u] = 0x00010010u; /* Not Root Port. */
        if (malformed == 5u) root[0x60u / 4u] = 0x00430010u; /* Future version. */
        if (malformed == 6u) {
            root[0x40u / 4u] = 0x0003f001u;
            root[0xf0u / 4u] = 0x00410010u; /* Link fields outside config. */
        }
        if (malformed == 7u) root[0x40u / 4u] = 0x00030001u; /* PCIe absent. */
        if (malformed == 8u) root[1] &= ~(1u << 20u); /* No cap list. */
        assert(book8_probe_begin_link(&probe, true));
        assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
        assert(probe.error == (malformed >= 7u ? 19u : 5u) && !probe.value_valid);
        assert(probe_accesses <= 5u && probe_stores == 0u && !irq_active);
    }
    link_fixture();
    root_config()[0x6cu / 4u] = UINT32_MAX;
    assert(book8_probe_begin_link(&probe, false));
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
    assert(probe.error == 19u && !probe.value_valid);

    link_fixture();
    state.target_link_verified = false; /* Reading training status needs no prior L0. */
    assert(book8_probe_begin(&probe, BOOK8_RS780_LC_STATE0, 0u));
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_INTENT));
    assert(probe.value_valid && probe.error == 0u);
    assert(book8_probe_ack(&probe, probe.id, BOOK8_PROBE_RESULT));
    assert(probe.ltssm_acknowledged && !state.target_link_verified && !state.target_ready);

    target_fixture();
    state.platform.read32_safe = NULL;
    assert(!book8_amd_root_link_ready(&state) && !state.target_link_verified);
    target_fixture();
    state.platform.write32_safe = NULL;
    assert(!book8_amd_root_link_ready(&state) && !state.target_link_verified);
}

int main(void)
{
    ecam = malloc(0x200000u);
    assert(ecam != NULL);
    test_ecam();
    test_target();
    test_driver_binding();
    test_event_telemetry();
    test_ltssm();
    test_single_probe();
    test_link_diagnostics();
    free(ecam);
    puts("PASS: PCIe ECAM, topology, BAR/IRQ inventory, driver matching, UC BAR mapping, D0 timeout and readiness");
    puts("PASS: PCIe events -> unchanged 292-byte v2 UDP/8080 packets and send-failure sequence");
    puts("PASS: target readiness without CPU/MSR prerequisite and target MSE/BME enforcement");
    puts("PASS: RS780 ECAM identity, true LTSSM L0, index/IRQ restoration, timeouts and no command bypass");
    puts("PASS: single-index probe intent/result ACKs, denied writes, retry without access, #GP/#PF and restore lockout");
    puts("PASS: shared guarded RS780 transport, manual link capabilities/status, malformed chains and acknowledged-only preflight");
    return 0;
}
