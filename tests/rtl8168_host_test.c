#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/rtl8168.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

static uint32_t cfg[64], masks[6];
static bool sizing[6], absent, stuck, map_fail, refuse_command;
static uint8_t target_fn;
static unsigned writes, maps, polls;
static uint8_t regs[0x1000] __attribute__((aligned(8)));
static uint8_t extra[0x4000] __attribute__((aligned(8)));
static const uint8_t mac[6] = {0x00, 0xe0, 0x4c, 0x68, 0x11, 0x01};
static struct rtl8168_dma dma;
static struct rtl8168_device *active_nic;
static uint64_t dma_phys;
static bool dma_fail, tx_stuck;
static unsigned tx_polls;

static uint64_t register_address(uint32_t offset)
{
    uint32_t low, high;
    memcpy(&low, regs + offset, sizeof(low));
    memcpy(&high, regs + offset + 4u, sizeof(high));
    return ((uint64_t)high << 32u) | low;
}

static uint32_t read_config(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t fn, uint8_t offset)
{
    (void)ctx;
    if (absent || bus != 3u || slot != 0u) {
        return 0xffffffffu;
    }
    if (target_fn != 0u && fn == 0u) {
        return offset == 0u ? 0x12348086u
            : offset == 0x0cu ? 0x00800000u : 0u;
    }
    if (fn != target_fn) {
        return 0xffffffffu;
    }
    if (offset >= 0x10u && offset <= 0x24u) {
        unsigned i = (offset - 0x10u) / 4u;
        return sizing[i] ? masks[i] : cfg[offset / 4u];
    }
    return cfg[offset / 4u];
}

static void write_config(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t fn, uint8_t offset, uint32_t value)
{
    (void)ctx;
    CHECK(bus == 3u && slot == 0u && fn == target_fn && !absent);
    ++writes;
    if (offset == 4u) {
        CHECK((value & 0xffff0000u) == 0u);
        if (active_nic != NULL && (value & 4u) != 0u) {
            CHECK(register_address(0x20u)
                == dma_phys + offsetof(struct rtl8168_dma, tx));
            CHECK(register_address(0xe4u)
                == dma_phys + offsetof(struct rtl8168_dma, rx));
            CHECK(regs[RTL8168_CR] == 0u);
            CHECK(regs[RTL8168_CFG9346] == 0u);
            CHECK((dma.rx[0].opts1 & RTL8168_OWN) != 0u);
        }
        if (!refuse_command) {
            cfg[1] = (cfg[1] & 0xffff0000u) | value;
        }
    } else {
        CHECK(offset >= 0x10u && offset <= 0x24u);
        unsigned i = (offset - 0x10u) / 4u;
        CHECK((cfg[1] & 7u) == 0u);
        sizing[i] = value == 0xffffffffu;
        if (!sizing[i]) {
            cfg[offset / 4u] = value;
        }
    }
}

static volatile uint8_t *map_mmio(void *ctx, uint64_t phys, uint64_t size,
    bool uncached)
{
    (void)ctx;
    CHECK(uncached);
    ++maps;
    if (map_fail) {
        return NULL;
    }
    if (phys == 0xfddfb000u) {
        CHECK(size == sizeof(regs));
        return regs;
    }
    CHECK(phys == 0xfddfc000u && size == sizeof(extra));
    return extra;
}

static void relax(void *ctx)
{
    (void)ctx;
    if (active_nic != NULL && active_nic->running) {
        CHECK(regs[RTL8168_CR] == 0x0cu);
        CHECK(regs[0x38u] == 0x40u);
        CHECK(regs[RTL8168_CFG9346] == 0u);
        CHECK((cfg[1] & 0x407u) == 0x407u);
        ++tx_polls;
        if (!tx_stuck) {
            unsigned index = (active_nic->tx_next + RTL8168_RING_COUNT - 1u)
                % RTL8168_RING_COUNT;
            CHECK((dma.tx[index].opts1 & (RTL8168_OWN | RTL8168_FS | RTL8168_LS))
                == (RTL8168_OWN | RTL8168_FS | RTL8168_LS));
            dma.tx[index].opts1 &= ~RTL8168_OWN;
        }
        return;
    }
    CHECK(regs[RTL8168_CR] == RTL8168_RESET);
    CHECK(regs[RTL8168_CFG9346] == 0u);
    CHECK((cfg[1] & 0x407u) == 0x403u);
    ++polls;
    if (!stuck && polls == 3u) {
        regs[RTL8168_CR] = 0u;
    }
}

static bool dma_address(void *ctx, const void *ptr, size_t size, uint64_t *phys)
{
    (void)ctx;
    CHECK(ptr == &dma && size == sizeof(dma));
    CHECK((cfg[1] & 4u) == 0u && regs[RTL8168_CR] == 0u);
    *phys = dma_phys;
    return !dma_fail;
}

static const struct rtl8168_platform platform = {
    .pci_read32 = read_config, .pci_write32 = write_config,
    .map_mmio = map_mmio, .relax = relax, .dma_address = dma_address
};

static void setup(void)
{
    memset(cfg, 0, sizeof(cfg));
    memset(masks, 0, sizeof(masks));
    memset(sizing, 0, sizeof(sizing));
    memset(regs, 0, sizeof(regs));
    memset(extra, 0, sizeof(extra));
    memcpy(regs, mac, sizeof(mac));
    cfg[0] = 0x816810ecu;
    cfg[1] = 0xf9000007u;
    cfg[2] = 0x02000015u;
    cfg[4] = 0x0000ce01u;
    cfg[6] = 0xfddfb004u;
    cfg[8] = 0xfddfc00cu;
    masks[0] = 0xffffff01u;
    masks[2] = 0xfffff004u;
    masks[3] = 0xffffffffu;
    masks[4] = 0xffffc00cu;
    masks[5] = 0xffffffffu;
    absent = stuck = map_fail = refuse_command = false;
    writes = maps = polls = 0u;
    target_fn = 0u;
    active_nic = NULL;
    dma_phys = 0x123400000ull;
    dma_fail = tx_stuck = false;
    tx_polls = 0u;
}

static void check_restored(void)
{
    CHECK(cfg[1] == 0xf9000007u);
    CHECK(cfg[4] == 0xce01u && cfg[6] == 0xfddfb004u);
    CHECK(cfg[7] == 0u && cfg[8] == 0xfddfc00cu && cfg[9] == 0u);
    for (unsigned i = 0u; i < 6u; ++i) {
        CHECK(!sizing[i]);
    }
}

static void packet_setup(struct rtl8168_device *d)
{
    setup();
    CHECK(rtl8168_probe(&platform, d) == RTL8168_OK);
    CHECK(rtl8168_init(&platform, d) == RTL8168_OK);
    active_nic = d;
}

static void packet_tests(void)
{
    struct rtl8168_device d = {0};
    uint8_t packet[RTL8168_FRAME_MAX], output[RTL8168_FRAME_MAX];
    size_t length;
    memset(packet, 0x5a, sizeof(packet));
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_BAD_STATE);
    CHECK(rtl8168_transmit(&platform, &d, packet, 60u) == RTL8168_BAD_STATE);
    CHECK(rtl8168_receive(&d, output, sizeof(output), &length) == RTL8168_BAD_STATE);

    packet_setup(&d);
    unsigned old_writes = writes;
    CHECK(rtl8168_start(&platform, &d, NULL) == RTL8168_BAD_DMA);
    CHECK(rtl8168_start(&platform, &d,
        (struct rtl8168_dma *)((uint8_t *)&dma + 32u)) == RTL8168_BAD_DMA);
    dma_fail = true;
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_BAD_DMA);
    dma_fail = false;
    dma_phys = 0u;
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_BAD_DMA);
    dma_phys = 0x123400020ull; /* 32 bytes is not enough for hardware ring alignment. */
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_BAD_DMA);
    dma_phys = UINT64_MAX & ~255ull;
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_BAD_DMA);
    CHECK(writes == old_writes && regs[RTL8168_CR] == 0u && !d.running);
    dma_phys = 0x123400000ull;
    struct rtl8168_platform no_dma = platform;
    no_dma.dma_address = NULL;
    CHECK(rtl8168_start(&no_dma, &d, &dma) == RTL8168_BAD_DMA);
    refuse_command = true;
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_PCI_DISABLED);
    CHECK(!d.running && regs[RTL8168_CR] == 0u && (cfg[1] & 4u) == 0u);
    refuse_command = false;
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_OK);
    CHECK(d.running && d.dma == &dma && regs[RTL8168_CR] == 0x0cu);
    CHECK((uintptr_t)dma.rx % 256u == 0u && (uintptr_t)dma.tx % 256u == 0u);
    CHECK(register_address(0xe4u) == dma_phys + offsetof(struct rtl8168_dma, rx));
    CHECK(register_address(0x20u) == dma_phys + offsetof(struct rtl8168_dma, tx));
    CHECK((register_address(0xe4u) >> 32u) == 1u);
    CHECK(regs[0xdau] == 0u && regs[0xdbu] == 6u && regs[0xecu] == 0x3fu);
    CHECK(regs[0x3cu] == 0u && regs[0x3du] == 0u);
    CHECK(regs[0xe0u] == 0u && regs[0xe1u] == 0u);
    uint32_t config;
    memcpy(&config, regs + 0x40u, sizeof(config));
    CHECK(config == ((3u << 24u) | (7u << 8u)));
    memcpy(&config, regs + 0x44u, sizeof(config));
    CHECK(config == ((7u << 13u) | (7u << 8u) | 0x0au));
    for (unsigned i = 0u; i < RTL8168_RING_COUNT; ++i) {
        uint32_t end = i == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u;
        CHECK(dma.rx[i].opts1 == (RTL8168_OWN | end | RTL8168_BUFFER_SIZE));
        CHECK(dma.tx[i].opts1 == end);
        CHECK(dma.rx[i].opts2 == 0u && dma.tx[i].opts2 == 0u);
        CHECK(dma.rx[i].address == dma_phys
            + offsetof(struct rtl8168_dma, rx_buffers) + i * RTL8168_BUFFER_SIZE);
        CHECK(dma.tx[i].address == dma_phys
            + offsetof(struct rtl8168_dma, tx_buffers) + i * RTL8168_BUFFER_SIZE);
    }
    old_writes = writes;
    CHECK(rtl8168_start(&platform, &d, &dma) == RTL8168_BAD_STATE);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_BAD_STATE);
    CHECK(writes == old_writes && d.running && (cfg[1] & 4u) != 0u);

    CHECK(rtl8168_transmit(&platform, &d, NULL, 60u) == RTL8168_BAD_PACKET);
    CHECK(rtl8168_transmit(&platform, &d, packet, 13u) == RTL8168_BAD_PACKET);
    CHECK(rtl8168_transmit(&platform, &d, packet, sizeof(packet) + 1u)
        == RTL8168_BAD_PACKET);
    CHECK(d.tx_next == 0u && tx_polls == 0u);
    CHECK(d.tx_dropped == 3u);
    for (unsigned i = 0u; i < RTL8168_RING_COUNT * 2u; ++i) {
        unsigned index = d.tx_next;
        size_t sent = i == 0u ? 14u : sizeof(packet);
        CHECK(rtl8168_transmit(&platform, &d, packet, sent) == RTL8168_OK);
        CHECK(tx_polls == 0u && (dma.tx[index].opts1 & RTL8168_OWN) != 0u);
        CHECK(memcmp(dma.tx_buffers[index], packet, sent) == 0);
        CHECK((dma.tx[index].opts1 & RTL8168_LENGTH_MASK)
            == (sent < 60u ? 60u : sent));
        CHECK((dma.tx[index].opts1 & RTL8168_EOR)
            == (index == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u));
        if (sent < 60u) {
            for (unsigned j = (unsigned)sent; j < 60u; ++j) {
                CHECK(dma.tx_buffers[index][j] == 0u);
            }
        }
        dma.tx[index].opts1 &= ~RTL8168_OWN;
    }
    CHECK(d.tx_next == 0u && d.tx_packets == RTL8168_RING_COUNT * 2u);
    tx_stuck = true;
    for (unsigned i = 0u; i < RTL8168_RING_COUNT; ++i) {
        CHECK(rtl8168_transmit(&platform, &d, packet, 60u) == RTL8168_OK);
        CHECK((dma.tx[i].opts1 & RTL8168_OWN) != 0u);
    }
    CHECK(tx_polls == 0u && d.tx_timeouts == 0u);
    CHECK(d.tx_packets == RTL8168_RING_COUNT * 3u);
    uint8_t saved[RTL8168_BUFFER_SIZE];
    memcpy(saved, dma.tx_buffers[0], sizeof(saved));
    unsigned old_polls = tx_polls;
    CHECK(rtl8168_transmit(&platform, &d, packet, 14u) == RTL8168_TX_BUSY);
    CHECK(memcmp(saved, dma.tx_buffers[0], sizeof(saved)) == 0);
    CHECK(tx_polls == old_polls && d.tx_next == 0u);
    CHECK(d.tx_dropped == 4u);
    dma.tx[0].opts1 &= ~RTL8168_OWN; /* Late completion permits safe reuse. */
    tx_stuck = false;
    CHECK(rtl8168_transmit(&platform, &d, packet, 60u) == RTL8168_OK);

    memset(output, 0xa5, sizeof(output));
    length = 99u;
    CHECK(rtl8168_receive(&d, output, sizeof(output), &length) == RTL8168_RX_EMPTY);
    CHECK(length == 0u && output[0] == 0xa5u && d.rx_next == 0u);
    CHECK(rtl8168_receive(&d, output, sizeof(output), NULL) == RTL8168_BAD_PACKET);
    CHECK(rtl8168_receive(&d, NULL, sizeof(output), &length) == RTL8168_BAD_PACKET);
    CHECK(rtl8168_receive(&d, output, 0u, &length) == RTL8168_BAD_PACKET);
    for (unsigned i = 0u; i < RTL8168_RING_COUNT * 2u; ++i) {
        unsigned index = d.rx_next;
        size_t received = i == 0u ? sizeof(packet) : 60u;
        memcpy(dma.rx_buffers[index], packet, received);
        memset(dma.rx_buffers[index] + received, 0xff, 4u);
        dma.rx[index].opts2 = 0x1234u;
        dma.rx[index].opts1 = RTL8168_FS | RTL8168_LS | (uint32_t)(received + 4u);
        CHECK(rtl8168_receive(&d, output, sizeof(output), &length) == RTL8168_OK);
        CHECK(length == received && memcmp(output, packet, length) == 0);
        uint32_t end = index == RTL8168_RING_COUNT - 1u ? RTL8168_EOR : 0u;
        CHECK(dma.rx[index].opts1 == (RTL8168_OWN | end | RTL8168_BUFFER_SIZE));
        CHECK(dma.rx[index].opts2 == 0u);
    }
    CHECK(d.rx_next == 0u && d.rx_packets == RTL8168_RING_COUNT * 2u);
    dma.rx[0].opts1 = RTL8168_FS | RTL8168_LS | 64u;
    memset(output, 0xa5, sizeof(output));
    CHECK(rtl8168_receive(&d, output, 59u, &length) == RTL8168_BUFFER_SMALL);
    CHECK(length == 60u && output[0] == 0xa5u && (dma.rx[0].opts1 & RTL8168_OWN));
    const uint32_t bad_status[] = {
        RTL8168_FS | RTL8168_LS | RTL8168_RX_ERROR | 64u,
        RTL8168_FS | 64u, RTL8168_LS | 64u,
        RTL8168_FS | RTL8168_LS | 63u,
        RTL8168_FS | RTL8168_LS | (RTL8168_BUFFER_SIZE + 1u),
        RTL8168_FS | RTL8168_LS | 0x3fffu
    };
    for (unsigned i = 0u; i < sizeof(bad_status) / sizeof(bad_status[0]); ++i) {
        unsigned index = d.rx_next;
        dma.rx[index].opts1 = bad_status[i];
        CHECK(rtl8168_receive(&d, output, sizeof(output), &length) == RTL8168_RX_DROPPED);
        CHECK(length == 0u && output[0] == 0xa5u);
        CHECK((dma.rx[index].opts1 & RTL8168_OWN) != 0u);
    }
    CHECK(d.rx_dropped == 7u);
    uint32_t tx_pending = 99u, rx_ready = 99u, tx_base, rx_base;
    rtl8168_ring_state(&d, &tx_base, &rx_base);
    CHECK(tx_base <= RTL8168_RING_COUNT && rx_base == 0u);
    for (unsigned i = 0u; i < RTL8168_RING_COUNT; ++i) {
        dma.tx[i].opts1 &= ~RTL8168_OWN;
    }
    rtl8168_ring_state(&d, &tx_pending, &rx_ready);
    CHECK(tx_pending == 0u && rx_ready == 0u);
    dma.tx[3].opts1 |= RTL8168_OWN;
    dma.rx[5].opts1 = RTL8168_FS | RTL8168_LS | 64u;
    dma.rx[6].opts1 = RTL8168_FS | RTL8168_LS | 64u;
    rtl8168_ring_state(&d, &tx_pending, &rx_ready);
    CHECK(tx_pending == 1u && rx_ready == 2u);
    dma.tx[3].opts1 &= ~RTL8168_OWN;
    dma.rx[5].opts1 = dma.rx[6].opts1 = RTL8168_OWN | RTL8168_BUFFER_SIZE;
    regs[RTL8168_PHY_STATUS] = RTL8168_PHY_LINK | RTL8168_PHY_1000M
        | RTL8168_PHY_FULL_DUPLEX;
    CHECK(rtl8168_phy_status(&d) == 0x13u);
    struct rtl8168_device idle = {0};
    CHECK(rtl8168_phy_status(&idle) == 0u && rtl8168_phy_status(NULL) == 0u);
    rtl8168_ring_state(&idle, &tx_pending, &rx_ready);
    CHECK(tx_pending == 0u && rx_ready == 0u);
    regs[RTL8168_PHY_STATUS] = 0u;
    puts("PASS: RTL8168 DMA rings, non-blocking TX, full-ring backpressure and packet polling");
}

int main(void)
{
    struct rtl8168_device d;
    setup();
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    check_restored();
    CHECK(d.present && d.bus == 3u && d.slot == 0u && d.function == 0u);
    CHECK(d.io_base == 0xce00u && d.bars[0].size == 0x100u);
    CHECK(d.bars[2].phys == 0xfddfb000u && d.bars[2].size == 0x1000u);
    CHECK(d.bars[4].phys == 0xfddfc000u && d.bars[4].size == 0x4000u);
    CHECK(d.bars[1].size == 0u && d.bars[3].size == 0u && d.bars[5].size == 0u);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_OK);
    CHECK(d.initialized && d.regs == regs && maps == 2u && polls == 3u);
    CHECK(memcmp(d.mac, mac, 6u) == 0 && memcmp(regs, mac, 6u) == 0);
    CHECK(regs[RTL8168_CFG9346] == 0u && regs[RTL8168_CR] == 0u);
    CHECK(regs[0x3c] == 0u && regs[0x3d] == 0u);
    CHECK((cfg[1] & 0x407u) == 0x403u && (cfg[1] >> 16u) == 0xf900u);

    setup();
    target_fn = 3u;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK && d.function == 3u);
    check_restored();

    setup();
    masks[0] = 0x0000ff01u; /* 16-bit I/O BAR mask. */
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    CHECK(d.io_base == 0xce00u && d.bars[0].size == 0x100u);

    setup();
    cfg[6] = 0xfddfb000u; /* 32-bit register BAR. */
    masks[2] = 0xfffff000u;
    masks[3] = 0u;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_OK);

    setup();
    absent = true;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_ABSENT);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_ABSENT);
    CHECK(writes == 0u && maps == 0u);
    setup();
    cfg[0] = 0x813910ecu;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_ABSENT && writes == 0u);
    setup();
    cfg[2] = 0x03000000u;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_ABSENT && writes == 0u);
    setup();
    cfg[3] = 0x00010000u;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_ABSENT && writes == 0u);

    setup();
    cfg[4] = 0u;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_BAD_BAR);
    CHECK(cfg[1] == 0xf9000007u && cfg[4] == 0u);
    setup();
    masks[2] = 0xffffe804u; /* Non-power-of-two aperture. */
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_BAD_BAR);
    check_restored();
    setup();
    cfg[8] = masks[4] = masks[5] = 0u;
    cfg[9] = 4u; /* Truncated 64-bit BAR in slot 5. */
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_BAD_BAR);
    CHECK(cfg[1] == 0xf9000007u && !sizing[2] && !sizing[3]);

    setup();
    refuse_command = true;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_PCI_DISABLED);
    CHECK(writes == 2u && maps == 0u);
    setup();
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    refuse_command = true;
    CHECK(rtl8168_init(&platform, &d) == RTL8168_PCI_DISABLED);
    CHECK(!d.initialized && polls == 0u);

    setup();
    cfg[7] = 1u; /* Valid 64-bit BAR above the identity-map limit. */
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_MAP_FAILED);
    CHECK(maps == 0u && !d.initialized);
    setup();
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    map_fail = true;
    CHECK(rtl8168_init(&platform, &d) == RTL8168_MAP_FAILED && !d.initialized);
    CHECK(cfg[1] == 0xf9000007u && polls == 0u);
    setup();
    masks[2] = masks[3] = masks[4] = masks[5] = 0u;
    cfg[6] = cfg[8] = 0u;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_BAD_BAR && maps == 0u);

    setup();
    stuck = true;
    CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
    CHECK(rtl8168_init(&platform, &d) == RTL8168_RESET_TIMEOUT);
    CHECK(!d.initialized && polls == RTL8168_RESET_POLLS);
    CHECK(regs[RTL8168_CFG9346] == 0u && (cfg[1] & 4u) == 0u);

    for (unsigned pattern = 0u; pattern < 3u; ++pattern) {
        setup();
        memset(regs, pattern == 1u ? 0xff : 0, 6u);
        if (pattern == 2u) {
            regs[0] = 1u;
        }
        CHECK(rtl8168_probe(&platform, &d) == RTL8168_OK);
        CHECK(rtl8168_init(&platform, &d) == RTL8168_BAD_MAC);
        CHECK(!d.initialized && regs[RTL8168_CFG9346] == 0u);
    }
    puts("PASS: RTL8168 discovery, BAR mapping, reset and MAC tests");
    packet_tests();
    return 0;
}
