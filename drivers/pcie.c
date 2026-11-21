#include "pcie.h"
#include "amd_host_bridge.h"
#include "../src/registry.h"

int snprintf(char *, size_t, const char *, ...);

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u)
        | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t read_u64(const uint8_t *p)
{
    return read_u32(p) | ((uint64_t)read_u32(p + 4u) << 32u);
}

static void log_node(struct book8_pcie_state *s, const char *event,
    const struct book8_pcie_node *n, uint32_t value)
{
    char text[128];
    snprintf(text, sizeof(text), "PCIE1 %s %04x:%02x:%02x.%x %08x",
        event, n->segment, n->bus, n->slot, n->function, value);
    s->platform.log(text);
}

void book8_pcie_init(struct book8_pcie_state *s,
    const struct book8_pcie_platform *p)
{
    *s = (struct book8_pcie_state){.platform = *p,
        .topology_valid = true, .target_status = "target not enumerated"};
}

bool book8_pcie_enable_target_command(struct book8_pcie_state *s)
{
    if (book8_pcie_read32(s, 0u, 1u, 0u, 0u, 0u) != BOOK8_PCIE_TARGET_ID) {
        s->platform.log("PCIE1 ERROR target command enforcement: device absent/identity changed");
        return false;
    }
    if (!book8_amd_root_link_ready(s)) {
        s->platform.log("PCIE1 ERROR target LTSSM not verified; MSE/BME assertion blocked");
        return false;
    }
    uint32_t before = book8_pcie_read32(s, 0u, 1u, 0u, 0u, 4u);
    if (before == UINT32_MAX)
        goto failed;
    book8_pcie_write16(s, 0u, 1u, 0u, 0u, 4u, (uint16_t)(before | 6u));
    uint32_t after = book8_pcie_read32(s, 0u, 1u, 0u, 0u, 4u);
    if (after == UINT32_MAX || (after & 6u) != 6u)
        goto failed;
    char text[128];
    snprintf(text, sizeof(text), "PCIE1 TARGET-COMMAND 0000:01:00.0 before=%04x after=%04x MSE=1 BME=1",
        before & 0xffffu, after & 0xffffu);
    s->platform.log(text);
    return true;
failed:
    s->platform.log("PCIE1 ERROR target command bits did not latch; BAR/MMIO access blocked");
    return false;
}

bool book8_pcie_add_mcfg(struct book8_pcie_state *s, const uint8_t *table,
    size_t available)
{
    if (available < 44u || table[0] != 'M' || table[1] != 'C'
        || table[2] != 'F' || table[3] != 'G')
        goto invalid;
    uint32_t length = read_u32(table + 4u);
    if (length < 44u || length > available || (length - 44u) % 16u != 0u)
        goto invalid;
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + table[i]);
    if (sum != 0u || length == 44u || s->ecam_count != 0u)
        goto invalid;
    size_t count = (length - 44u) / 16u;
    if (count > BOOK8_PCIE_MAX_ECAM)
        goto invalid;
    struct book8_pcie_ecam entries[BOOK8_PCIE_MAX_ECAM] = {0};
    for (size_t i = 0u; i < count; ++i) {
        const uint8_t *p = table + 44u + i * 16u;
        entries[i] = (struct book8_pcie_ecam){
            .base = read_u64(p), .segment = (uint16_t)(p[8] | (p[9] << 8u)),
            .first = p[10], .last = p[11]};
        struct book8_pcie_ecam *e = &entries[i];
        /* MCFG's base corresponds to bus zero, even for a nonzero first bus. */
        uint64_t end = e->base + ((uint64_t)e->last + 1u) * 0x100000u;
        if (e->base == 0u || (e->base & 0xfffffu) != 0u
            || e->first > e->last || end < e->base || end > 0x100000000ull)
            goto invalid;
        uint64_t start = e->base + (uint64_t)e->first * 0x100000u;
        for (size_t j = 0u; j < i; ++j) {
            const struct book8_pcie_ecam *other = &entries[j];
            uint64_t other_start = other->base
                + (uint64_t)other->first * 0x100000u;
            uint64_t other_end = other->base
                + ((uint64_t)other->last + 1u) * 0x100000u;
            if ((start < other_end && other_start < end)
                || (e->segment == other->segment && e->first <= other->last
                    && other->first <= e->last))
                goto invalid;
        }
    }
    for (size_t i = 0u; i < count; ++i) {
        struct book8_pcie_ecam *e = &entries[i];
        uint64_t start = e->base + (uint64_t)e->first * 0x100000u;
        uint64_t size = ((uint64_t)e->last - e->first + 1u) * 0x100000u;
        e->mapped = s->platform.map(start, size);
        if (e->mapped == NULL) {
            s->platform.log("PCIE1 ERROR ECAM mapping failed; MCFG rejected");
            return false;
        }
    }
    for (size_t i = 0u; i < count; ++i)
        s->ecam[i] = entries[i];
    s->ecam_count = count;
    s->platform.log("PCIE1 ECAM MCFG accepted; uncached configuration access");
    return true;
invalid:
    s->platform.log("PCIE1 ERROR invalid/unsupported MCFG; legacy segment 0 only");
    return false;
}

volatile uint8_t *book8_pcie_ecam_address(struct book8_pcie_state *s,
    uint16_t segment, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t reg)
{
    if (slot >= 32u || fn >= 8u || reg > 0xfffu) {
        s->platform.log("PCIE1 ERROR invalid ECAM address");
        return NULL;
    }
    for (size_t i = 0u; i < s->ecam_count; ++i) {
        const struct book8_pcie_ecam *e = &s->ecam[i];
        if (e->mapped != NULL && e->segment == segment
            && bus >= e->first && bus <= e->last)
            return e->mapped + ((uint32_t)(bus - e->first) << 20u)
                + ((uint32_t)slot << 15u) + ((uint32_t)fn << 12u) + reg;
    }
    return NULL;
}

uint32_t book8_pcie_read32(struct book8_pcie_state *s, uint16_t segment,
    uint8_t bus, uint8_t slot, uint8_t fn, uint16_t reg)
{
    if (slot >= 32u || fn >= 8u || reg > 0xffcu || (reg & 3u) != 0u) {
        s->platform.log("PCIE1 ERROR invalid configuration read");
        return UINT32_MAX;
    }
    volatile uint8_t *p = book8_pcie_ecam_address(s, segment, bus, slot, fn, reg);
    if (p != NULL)
        return *(volatile uint32_t *)p;
    if (segment == 0u && reg < 256u)
        return s->platform.legacy_read(bus, slot, fn, (uint8_t)reg);
    s->platform.log("PCIE1 ERROR configuration read outside mapped ECAM");
    return UINT32_MAX;
}

void book8_pcie_write32(struct book8_pcie_state *s, uint16_t segment,
    uint8_t bus, uint8_t slot, uint8_t fn, uint16_t reg, uint32_t value)
{
    if (slot >= 32u || fn >= 8u || reg > 0xffcu || (reg & 3u) != 0u)
        goto invalid;
    volatile uint8_t *p = book8_pcie_ecam_address(s, segment, bus, slot, fn, reg);
    if (p != NULL) {
        *(volatile uint32_t *)p = value;
        return;
    }
    if (segment == 0u && reg < 256u) {
        s->platform.legacy_write(bus, slot, fn, (uint8_t)reg, value);
        return;
    }
invalid:
    s->platform.log("PCIE1 ERROR invalid/unmapped configuration write");
}

void book8_pcie_write16(struct book8_pcie_state *s, uint16_t segment,
    uint8_t bus, uint8_t slot, uint8_t fn, uint16_t reg, uint16_t value)
{
    if (slot >= 32u || fn >= 8u || reg > 0xffeu || (reg & 1u) != 0u)
        goto invalid;
    volatile uint8_t *p = book8_pcie_ecam_address(s, segment, bus, slot, fn, reg);
    if (p != NULL) {
        *(volatile uint16_t *)p = value;
        return;
    }
    if (segment == 0u && reg < 256u) {
        s->platform.legacy_write16(bus, slot, fn, (uint8_t)reg, value);
        return;
    }
invalid:
    s->platform.log("PCIE1 ERROR invalid/unmapped configuration word write");
}

static uint32_t node_read(struct book8_pcie_state *s,
    const struct book8_pcie_node *n, uint16_t reg)
{
    return book8_pcie_read32(s, n->segment, n->bus, n->slot, n->function, reg);
}

static void node_write16(struct book8_pcie_state *s,
    const struct book8_pcie_node *n, uint16_t reg, uint16_t value)
{
    book8_pcie_write16(s, n->segment, n->bus, n->slot, n->function, reg, value);
}

static uint64_t bar_size_from_mask(const struct book8_pcie_bar *bar,
    uint32_t mask_low, uint32_t mask_high)
{
    uint64_t mask, size;

    if (mask_low == UINT32_MAX
        && (!bar->is_64 || mask_high == UINT32_MAX)) {
        return 0u;
    }
    if (bar->io) {
        mask = mask_low & ~3u;
        size = (uint32_t)(~(uint32_t)mask + 1u);
    } else if (bar->is_64) {
        mask = ((uint64_t)mask_high << 32u) | (mask_low & ~0x0fu);
        size = ~mask + 1u;
    } else {
        mask = mask_low & ~0x0fu;
        size = (uint32_t)(~(uint32_t)mask + 1u);
    }
    return size != 0u && (size & (size - 1u)) == 0u ? size : 0u;
}

static void size_node_bars(struct book8_pcie_state *s,
    struct book8_pcie_node *n)
{
    uint32_t command = node_read(s, n, 4u);

    if (command == UINT32_MAX || (command & 7u) != 0u) {
        return;
    }
    node_write16(s, n, 4u, (uint16_t)(command & ~7u));
    for (uint8_t bar = 0u; bar < n->bar_count; ++bar) {
        struct book8_pcie_bar *resource = &n->bars[bar];
        uint16_t offset = (uint16_t)(0x10u + bar * 4u);
        uint32_t original_low, original_high = 0u, mask_low, mask_high = 0u;

        if (!resource->valid) {
            continue;
        }
        original_low = node_read(s, n, offset);
        if (original_low == UINT32_MAX) {
            continue;
        }
        if (resource->is_64) {
            original_high = node_read(s, n, (uint16_t)(offset + 4u));
            if (original_high == UINT32_MAX) {
                continue;
            }
            book8_pcie_write32(s, n->segment, n->bus, n->slot,
                n->function, (uint16_t)(offset + 4u), UINT32_MAX);
        }
        book8_pcie_write32(s, n->segment, n->bus, n->slot,
            n->function, offset, UINT32_MAX);
        mask_low = node_read(s, n, offset);
        if (resource->is_64) {
            mask_high = node_read(s, n, (uint16_t)(offset + 4u));
        }
        if (resource->is_64) {
            book8_pcie_write32(s, n->segment, n->bus, n->slot,
                n->function, (uint16_t)(offset + 4u), original_high);
        }
        book8_pcie_write32(s, n->segment, n->bus, n->slot,
            n->function, offset, original_low);
        if (resource->is_64) {
            ++bar;
        }
        resource->size = bar_size_from_mask(resource, mask_low, mask_high);
    }
    node_write16(s, n, 4u, (uint16_t)command);
}

static void enumerate_bus(struct book8_pcie_state *s, uint16_t segment,
    uint8_t bus)
{
    for (uint8_t slot = 0u; slot < 32u; ++slot) {
        uint8_t functions = 1u;
        for (uint8_t fn = 0u; fn < functions; ++fn) {
            uint32_t id = book8_pcie_read32(s, segment, bus, slot, fn, 0u);
            if ((uint16_t)id == 0xffffu || (uint16_t)id == 0u)
                continue;
            uint8_t header = (uint8_t)(book8_pcie_read32(s, segment,
                bus, slot, fn, 0x0cu) >> 16u);
            if (fn == 0u && (header & 0x80u) != 0u)
                functions = 8u;
            if (s->node_count == BOOK8_PCIE_MAX_NODES) {
                if (s->topology_valid)
                    s->platform.log("PCIE1 ERROR topology capacity exceeded");
                s->topology_valid = false;
                return;
            }
            struct book8_pcie_node *n = &s->nodes[s->node_count++];
            *n = (struct book8_pcie_node){.id = id, .segment = segment,
                .bus = bus, .slot = slot, .function = fn,
                .header = header & 0x7fu,
                .class_revision = book8_pcie_read32(s, segment,
                    bus, slot, fn, 8u),
                .bus_device = {.bus_type = BOOK8_BUS_PCI,
                    .vendor_id = (uint16_t)id,
                    .device_id = (uint16_t)(id >> 16u),
                    .class_code = book8_pcie_read32(s, segment,
                        bus, slot, fn, 8u) >> 8u,
                    .segment = segment, .bus = bus, .device = slot,
                    .function = fn, .private_data = n}};
            n->bar_count = n->header == 0u ? BOOK8_PCIE_BAR_COUNT
                : n->header == 1u ? 2u : 0u;
            for (uint8_t bar = 0u; bar < n->bar_count; ++bar) {
                uint16_t offset = (uint16_t)(0x10u + bar * 4u);
                uint32_t low = node_read(s, n, offset);
                struct book8_pcie_bar *resource = &n->bars[bar];

                if (low == UINT32_MAX)
                    continue;
                resource->io = (low & 1u) != 0u;
                resource->is_64 = !resource->io
                    && ((low >> 1u) & 3u) == 2u;
                resource->prefetchable = !resource->io
                    && (low & 8u) != 0u;
                if (!resource->io && ((low >> 1u) & 3u) == 3u)
                    continue;
                resource->base = resource->io
                    ? (uint64_t)(low & ~3u) : (uint64_t)(low & ~0xfu);
                resource->valid = true;
                if (resource->is_64) {
                    if ((uint8_t)(bar + 1u) >= n->bar_count)
                        resource->valid = false;
                    else {
                        uint32_t high = node_read(s, n,
                            (uint16_t)(offset + 4u));
                        if (high == UINT32_MAX)
                            resource->valid = false;
                        else
                            resource->base |= (uint64_t)high << 32u;
                        ++bar;
                    }
                }
            }
            uint32_t interrupt = node_read(s, n, 0x3cu);
            n->irq_line = (uint8_t)interrupt;
            n->irq_pin = (uint8_t)(interrupt >> 8u);
            n->bus_device.irq_line = n->irq_line;
            log_node(s, "DEVICE", n, id);
            if (n->header == 1u && (n->class_revision >> 16u) == 0x0604u) {
                uint32_t buses = node_read(s, n, 0x18u);
                n->primary = (uint8_t)buses;
                n->secondary = (uint8_t)(buses >> 8u);
                n->subordinate = (uint8_t)(buses >> 16u);
                log_node(s, "BRIDGE", n, buses & 0xffffffu);
            }
            if (segment == 0u && bus == 1u && slot == 0u && fn == 0u
                && id == BOOK8_PCIE_TARGET_ID) {
                s->target_present = true;
            }
        }
    }
}

void book8_pcie_enumerate(struct book8_pcie_state *s)
{
    s->node_count = 0u;
    s->topology_valid = true;
    s->target_present = false;
    s->target_ready = false;
    s->target_link_verified = false;
    s->target_status = "target absent";
    s->platform.log(s->ecam_count != 0u ? "PCIE1 ENUM begin ECAM/legacy"
        : "PCIE1 ENUM begin legacy; MCFG unavailable");
    for (uint32_t bus = 0u; bus < 256u; ++bus)
        enumerate_bus(s, 0u, (uint8_t)bus);
    for (size_t i = 0u; i < s->ecam_count; ++i) {
        const struct book8_pcie_ecam *e = &s->ecam[i];
        if (e->segment != 0u)
            for (uint32_t bus = e->first; bus <= e->last; ++bus)
                enumerate_bus(s, e->segment, (uint8_t)bus);
    }
    s->platform.log("PCIE1 ENUM complete; firmware bus numbers and BARs preserved");
}

static bool driver_matches(const struct book8_pcie_node *n,
    const struct book8_pcie_match *match)
{
    uint16_t vendor = (uint16_t)n->id;
    uint16_t device = (uint16_t)(n->id >> 16u);
    uint32_t class_code = n->class_revision >> 8u;

    return (match->vendor_id == BOOK8_PCIE_ANY_ID
            || match->vendor_id == vendor)
        && (match->device_id == BOOK8_PCIE_ANY_ID
            || match->device_id == device)
        && (class_code & match->class_mask)
            == (match->class_code & match->class_mask);
}

volatile uint8_t *book8_pcie_map_bar(struct book8_pcie_state *s,
    const struct book8_pcie_node *n, uint8_t index, uint64_t size)
{
    if (n == NULL || index >= n->bar_count || size == 0u
        || s->platform.map == NULL || !n->bars[index].valid
        || n->bars[index].io || n->bars[index].base == 0u
        || n->bars[index].base >= 0x100000000ull
        || size > 0x100000000ull - n->bars[index].base) {
        s->platform.log("PCIE1 ERROR invalid/unassigned BAR mapping request");
        return NULL;
    }
    return s->platform.map(n->bars[index].base, size);
}

size_t book8_pcie_bind_drivers(struct book8_pcie_state *s,
    const struct book8_pcie_driver *drivers, size_t driver_count)
{
    size_t bound = 0u;

    s->driver_count = 0u;
    for (size_t i = 0u; i < s->node_count; ++i) {
        struct book8_pcie_node *n = &s->nodes[i];

        n->driver = NULL;
        for (size_t d = 0u; d < driver_count; ++d) {
            const struct book8_pcie_driver *driver = &drivers[d];
            bool matched = false;

            if (driver->name == NULL || driver->matches == NULL
                || driver->probe == NULL)
                continue;
            for (size_t m = 0u; m < driver->match_count; ++m) {
                if (driver_matches(n, &driver->matches[m])) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                continue;

            volatile uint8_t *bar0 = NULL;
            if (driver->bar0_window_size != 0u) {
                bar0 = book8_pcie_map_bar(s, n, 0u,
                    driver->bar0_window_size);
                if (bar0 == NULL)
                    continue;
            }
            if (!driver->probe(s, n, bar0)) {
                log_node(s, "DRIVER-REJECT", n, (uint32_t)d);
                continue;
            }
            n->driver = driver;
            ++bound;
            log_node(s, "DRIVER-BIND", n, (uint32_t)d);
            break;
        }
    }
    s->driver_count = bound;
    return bound;
}

static bool bus_driver_matches(const struct book8_bus_driver *driver,
    const struct book8_bus_device *device)
{
    for (size_t i = 0u; i < driver->match_count; ++i) {
        if (book8_bus_match_device(&driver->matches[i], device)) {
            return true;
        }
    }
    return false;
}

size_t book8_pcie_bind_bus_drivers(struct book8_pcie_state *s,
    const struct book8_bus_driver *drivers, size_t driver_count)
{
    size_t bound = 0u;

    s->registered_driver_count = 0u;
    for (size_t d = 0u; d < driver_count; ++d) {
        for (size_t m = 0u; m < drivers[d].match_count; ++m) {
            if (drivers[d].matches[m].bus_type == BOOK8_BUS_PCI) {
                ++s->registered_driver_count;
                break;
            }
        }
    }
    for (size_t i = 0u; i < s->node_count; ++i) {
        struct book8_pcie_node *node = &s->nodes[i];
        struct book8_bus_device *device = &node->bus_device;

        if (device->driver != NULL) {
            ++bound;
            continue;
        }
        size_node_bars(s, node);
        for (size_t d = 0u; d < driver_count; ++d) {
            const struct book8_bus_driver *driver = &drivers[d];
            if (driver->name == NULL || driver->probe == NULL
                || !bus_driver_matches(driver, device)) {
                continue;
            }
            if (driver->bar0_window_size != 0u) {
                volatile uint8_t *mapped = book8_pcie_map_bar(s, node, 0u,
                    driver->bar0_window_size);
                if (mapped == NULL) {
                    continue;
                }
                device->mmio_base = node->bars[0].base;
                device->mmio_size = driver->bar0_window_size;
                device->mapped_mmio = mapped;
                node->bars[0].mapped = true;
                node->bars[0].mapped_size = driver->bar0_window_size;
                node->bars[0].cache_policy = BOOK8_PCIE_CACHE_UC;
            }
            if (!driver->probe(device)) {
                continue;
            }
            device->driver = driver;
            ++bound;
            log_node(s, "DRIVER-BIND", node, (uint32_t)d);
            break;
        }
    }
    s->driver_count = bound;
    return bound;
}

size_t book8_pcie_rescan(struct book8_pcie_state *s,
    const struct book8_bus_driver *drivers, size_t driver_count)
{
    static struct book8_pcie_node previous[BOOK8_PCIE_MAX_NODES];
    size_t previous_count = s->node_count;

    if (previous_count > BOOK8_PCIE_MAX_NODES) {
        previous_count = BOOK8_PCIE_MAX_NODES;
    }
    for (size_t i = 0u; i < previous_count; ++i) {
        previous[i] = s->nodes[i];
    }
    book8_pcie_enumerate(s);
    for (size_t i = 0u; i < previous_count; ++i) {
        struct book8_pcie_node *old = &previous[i];
        bool retained = false;
        for (size_t j = 0u; j < s->node_count; ++j) {
            struct book8_pcie_node *current = &s->nodes[j];
            if (old->segment != current->segment || old->bus != current->bus
                || old->slot != current->slot
                || old->function != current->function || old->id != current->id) {
                continue;
            }
            for (uint8_t bar = 0u; bar < current->bar_count; ++bar) {
                if (old->bars[bar].base == current->bars[bar].base
                    && old->bars[bar].valid == current->bars[bar].valid) {
                    current->bars[bar].size = old->bars[bar].size;
                    current->bars[bar].mapped = old->bars[bar].mapped;
                    current->bars[bar].mapped_size = old->bars[bar].mapped_size;
                    current->bars[bar].cache_policy = old->bars[bar].cache_policy;
                }
            }
            if (old->bus_device.driver != NULL) {
                if (bus_driver_matches(old->bus_device.driver,
                        &current->bus_device)) {
                    current->bus_device.driver = old->bus_device.driver;
                    current->bus_device.mmio_base =
                        old->bus_device.mmio_base;
                    current->bus_device.mmio_size =
                        old->bus_device.mmio_size;
                    current->bus_device.mapped_mmio =
                        old->bus_device.mapped_mmio;
                } else if (old->bus_device.driver->remove != NULL) {
                    old->bus_device.driver->remove(&old->bus_device);
                }
            }
            retained = true;
            break;
        }
        if (!retained && old->bus_device.driver != NULL
            && old->bus_device.driver->remove != NULL) {
            old->bus_device.driver->remove(&old->bus_device);
        }
    }
    return book8_pcie_bind_bus_drivers(s, drivers, driver_count);
}

bool book8_pcie_registry_sync(const struct book8_pcie_state *s)
{
    static const char root[] = "HKLM\\HARDWARE\\DEVICEMAP\\PCI";

    if (s == NULL) {
        return false;
    }
    (void)reg_delete_tree(root);
    for (size_t i = 0u; i < s->node_count; ++i) {
        const struct book8_pcie_node *node = &s->nodes[i];
        char path[BOOK8_REGISTRY_KEY_MAX];
        char value[BOOK8_REGISTRY_VALUE_MAX];
        int length = snprintf(path, sizeof(path),
            "%s\\%04x_%02x_%02x.%u", root, node->segment, node->bus,
            node->slot, node->function);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            return false;
        }
        length = snprintf(value, sizeof(value), "0x%04x",
            (uint16_t)node->id);
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "VendorID", value)) {
            return false;
        }
        length = snprintf(value, sizeof(value), "0x%04x",
            (uint16_t)(node->id >> 16u));
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "DeviceID", value)) {
            return false;
        }
        length = snprintf(value, sizeof(value), "0x%06x",
            (uint32_t)(node->class_revision >> 8u));
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "ClassCode", value)
            || !reg_set_u32(path, "Subclass",
                (node->class_revision >> 16u) & 0xffu)
            || !reg_set_u32(path, "IRQ", node->irq_line)
            || !reg_set_str(path, "Driver",
                node->bus_device.driver != NULL
                    ? node->bus_device.driver->name : "unbound")) {
            return false;
        }
        for (uint8_t bar = 0u; bar < BOOK8_PCIE_BAR_COUNT; ++bar) {
            const struct book8_pcie_bar *resource = &node->bars[bar];
            const char *cache = resource->cache_policy == BOOK8_PCIE_CACHE_UC
                ? "UC" : resource->cache_policy == BOOK8_PCIE_CACHE_WB
                ? "WB" : resource->cache_policy == BOOK8_PCIE_CACHE_WC
                ? "WC" : "unknown";
            length = snprintf(value, sizeof(value),
                "Base=0x%016llx Size=0x%016llx Cache=%s",
                (unsigned long long)resource->base,
                (unsigned long long)resource->size, cache);
            if (length < 0 || (size_t)length >= sizeof(value)
                || !reg_set_str(path, bar == 0u ? "BAR0" :
                    bar == 1u ? "BAR1" : bar == 2u ? "BAR2" :
                    bar == 3u ? "BAR3" : bar == 4u ? "BAR4" : "BAR5",
                    value)) {
                return false;
            }
        }
    }
    return true;
}

static bool enter_d0(struct book8_pcie_state *s, const struct book8_pcie_node *n)
{
    uint32_t command = node_read(s, n, 4u);
    if (command == UINT32_MAX)
        return false;
    if ((command & (1u << 20u)) == 0u) {
        log_node(s, "NO-PM-CAP", n, command);
        return true;
    }
    uint32_t cap = node_read(s, n, 0x34u) & 0xffu;
    uint64_t seen = 0u;
    if (cap == 0u)
        return false;
    while (cap != 0u) {
        if (cap < 0x40u || cap > 0xf8u || (cap & 3u) != 0u)
            return false;
        uint64_t bit = 1ull << (cap >> 2u);
        if ((seen & bit) != 0u)
            return false;
        seen |= bit;
        uint32_t value = node_read(s, n, (uint16_t)cap);
        if (value == UINT32_MAX)
            return false;
        if ((value & 0xffu) == 1u) {
            uint16_t reg = (uint16_t)(cap + 4u);
            uint32_t pmcsr = node_read(s, n, reg);
            if (pmcsr == UINT32_MAX)
                return false;
            log_node(s, "PM-BEFORE", n, pmcsr & 0xffffu);
            if ((pmcsr & 3u) != 0u) {
                /* Preserve PME enable; never acknowledge the W1C PME status. */
                node_write16(s, n, reg, (uint16_t)(pmcsr & ~0x8003u));
                for (uint32_t poll = 0u; poll < 50u; ++poll) {
                    if (!s->platform.delay_ms(10u))
                        return false;
                    pmcsr = node_read(s, n, reg);
                    if (pmcsr == UINT32_MAX)
                        return false;
                    if ((pmcsr & 3u) == 0u) {
                        log_node(s, "D0", n, pmcsr & 0xffffu);
                        return true;
                    }
                }
                return false;
            }
            log_node(s, "D0", n, pmcsr & 0xffffu);
            return true;
        }
        cap = (value >> 8u) & 0xffu;
    }
    log_node(s, "NO-PM-CAP", n, command);
    return true;
}

bool book8_pcie_prepare_target(struct book8_pcie_state *s)
{
    s->target_ready = false;
    if (!s->target_present) {
        s->target_status = "target absent";
        s->platform.log("PCIE1 TARGET 0000:01:00.0 13c210de absent");
        return false;
    }
    if (!s->topology_valid)
        goto topology_error;
    const struct book8_pcie_node *path[256];
    size_t depth = 0u;
    uint8_t bus = 1u;
    bool visited[256] = {false};
    while (bus != 0u) {
        if (visited[bus])
            goto topology_error;
        visited[bus] = true;
        const struct book8_pcie_node *parent = NULL;
        for (size_t i = 0u; i < s->node_count; ++i) {
            const struct book8_pcie_node *n = &s->nodes[i];
            if (n->segment != 0u || n->header != 1u
                || (n->class_revision >> 16u) != 0x0604u
                || n->secondary != bus)
                continue;
            if (parent != NULL || n->primary != n->bus
                || n->secondary <= n->bus || n->subordinate < 1u
                || n->subordinate < n->secondary)
                goto topology_error;
            parent = n;
        }
        if (parent == NULL)
            goto topology_error;
        path[depth++] = parent;
        bus = parent->bus;
    }
    while (depth != 0u) {
        const struct book8_pcie_node *n = path[--depth];
        if (!enter_d0(s, n))
            goto power_error;
        if (!book8_amd_root_link_ready(s))
            goto link_error;
        uint32_t cmd = node_read(s, n, 4u);
        if (cmd == UINT32_MAX)
            goto power_error;
        node_write16(s, n, 4u, (uint16_t)(cmd | 6u));
        uint32_t readback = node_read(s, n, 4u);
        if (readback == UINT32_MAX || (readback & 6u) != 6u)
            goto power_error;
        log_node(s, "UPSTREAM-COMMAND", n, readback & 0xffffu);
    }
    bool endpoint_found = false;
    for (size_t i = 0u; i < s->node_count; ++i) {
        const struct book8_pcie_node *n = &s->nodes[i];
        if (n->segment != 0u || n->bus != 1u || n->slot != 0u
            || n->function != 0u)
            continue;
        endpoint_found = true;
        if (n->id != BOOK8_PCIE_TARGET_ID || n->header != 0u
            || (n->class_revision >> 24u) != 0x03u || !enter_d0(s, n))
            goto power_error;
        if (!book8_pcie_enable_target_command(s))
            goto power_error;
        for (uint16_t bar = 0x10u; bar <= 0x24u; bar += 4u) {
            char label[16];
            snprintf(label, sizeof(label), "BAR%u-RAW",
                (unsigned)(bar - 0x10u) / 4u);
            log_node(s, label, n, node_read(s, n, bar));
        }
        uint32_t bar0 = node_read(s, n, 0x10u);
        uint32_t bar1 = node_read(s, n, 0x14u);
        if (bar0 == UINT32_MAX || bar1 == UINT32_MAX
            || (bar0 & ~0xfu) == 0u || (bar1 & ~0xfu) == 0u) {
            s->target_status = "BAR allocation blocked: no verified ACPI _CRS allocator";
            s->platform.log("PCIE1 BLOCKED BAR assignment requires verified firmware resource windows");
            return false;
        }
    }
    if (!endpoint_found)
        goto topology_error;
    s->target_ready = true;
    s->target_status = "RS780 L0 and PCI configuration verified";
    s->platform.log("PCIE1 TARGET ready: RS780 L0 and PCI configuration verified; lane power unmeasured");
    return true;
topology_error:
    s->target_status = "invalid or incomplete upstream topology";
    s->platform.log("PCIE1 ERROR target upstream topology invalid/incomplete; GPU MMIO blocked");
    return false;
power_error:
    s->target_status = "PCI D0/command transition failed";
    s->platform.log("PCIE1 ERROR PCI D0/command transition failed or timed out; GPU MMIO blocked");
    return false;
link_error:
    s->target_status = "RS780 LTSSM L0 unverified; command assertion blocked";
    s->platform.log("PCIE1 ERROR root link prerequisite failed; GPU MMIO blocked");
    return false;
}
