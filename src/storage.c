#include "kernel_internal.h"

/* IDE/AHCI discovery, disk I/O and filesystem adapters. */

uint8_t ide_status(struct ide_channel *channel)
{
    return inb(channel->command_base + 7u);
}

void ide_delay(struct ide_channel *channel)
{
    (void)inb(channel->control_base + 2u);
    (void)inb(channel->control_base + 2u);
    (void)inb(channel->control_base + 2u);
    (void)inb(channel->control_base + 2u);
}

bool ide_wait_ready(struct ide_channel *channel)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ide_status(channel);
        if (status == 0xffu || status == 0u) {
            return false;
        }
        if ((status & 0x80u) == 0u) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool ide_wait_data(struct ide_channel *channel)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ide_status(channel);
        if (status == 0xffu || status == 0u || (status & 0x01u) != 0u) {
            return false;
        }
        if ((status & 0x80u) == 0u && (status & 0x08u) != 0u) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool ide_identify(struct ide_channel *channel, uint8_t drive,
    uint16_t identify[256])
{
    uint16_t base = channel->command_base;
    if (base == 0u) {
        return false;
    }
    outb(base + 6u, (uint8_t)(0xa0u | (drive << 4u)));
    ide_delay(channel);
    outb(base + 2u, 0u);
    outb(base + 3u, 0u);
    outb(base + 4u, 0u);
    outb(base + 5u, 0u);
    outb(base + 7u, 0xecu);
    if (ide_status(channel) == 0u || ide_status(channel) == 0xffu) {
        return false;
    }
    if (!ide_wait_data(channel)) {
        return false;
    }
    for (uint32_t i = 0u; i < 256u; ++i) {
        identify[i] = inw(base);
    }
    return true;
}

uint64_t identify_sector_count(const uint16_t identify[256],
    bool *lba48)
{
    *lba48 = (identify[83] & (1u << 10u)) != 0u
        && (identify[86] & (1u << 10u)) != 0u;
    if (*lba48) {
        return (uint64_t)identify[100] | ((uint64_t)identify[101] << 16u)
            | ((uint64_t)identify[102] << 32u)
            | ((uint64_t)identify[103] << 48u);
    }
    if ((identify[49] & (1u << 9u)) != 0u) {
        return (uint64_t)identify[60] | ((uint64_t)identify[61] << 16u);
    }
    return 0u;
}

void identify_model(char model[41], const uint16_t identify[256])
{
    for (uint32_t i = 0u; i < 20u; ++i) {
        model[i * 2u] = (char)(identify[27u + i] >> 8u);
        model[i * 2u + 1u] = (char)(identify[27u + i] & 0xffu);
    }
    model[40] = '\0';
    for (uint32_t i = 40u; i > 0u && model[i - 1u] == ' '; --i) {
        model[i - 1u] = '\0';
    }
}

struct disk_device *add_disk(const char *name, const uint16_t identify[256],
    bool ahci, uint8_t channel, uint8_t drive, struct ahci_port *ahci_port)
{
    if (disk_count >= MAX_DISKS) {
        return NULL;
    }
    struct disk_device *disk = &disks[disk_count++];
    zero_memory(disk, sizeof(*disk));
    copy_string(disk->name, sizeof(disk->name), name);
    identify_model(disk->model, identify);
    disk->sectors = identify_sector_count(identify, &disk->lba48);
    disk->ahci = ahci;
    disk->dma = !ahci && (identify[49] & (1u << 8u)) != 0u
        && ide_channels[channel].bus_master_base != 0u;
    disk->channel = channel;
    disk->drive = drive;
    disk->ahci_port = ahci_port;
    return disk;
}

bool ide_read_sector(struct ide_channel *channel, uint8_t drive,
    uint64_t lba, uint8_t destination[512], bool lba48)
{
    if (channel->command_base == 0u) {
        return false;
    }
    if (lba48 && lba < (1ull << 48u)) {
        uint16_t base = channel->command_base;
        outb(base + 6u, (uint8_t)(0x40u | (drive << 4u)));
        ide_delay(channel);
        if (!ide_wait_ready(channel)) {
            console_write("IDE read timed out before command; status 0x");
            print_hex8(ide_status(channel));
            console_putc('\n');
            return false;
        }
        outb(base + 2u, 0u);
        outb(base + 3u, (uint8_t)(lba >> 24u));
        outb(base + 4u, (uint8_t)(lba >> 32u));
        outb(base + 5u, (uint8_t)(lba >> 40u));
        outb(base + 2u, 1u);
        outb(base + 3u, (uint8_t)lba);
        outb(base + 4u, (uint8_t)(lba >> 8u));
        outb(base + 5u, (uint8_t)(lba >> 16u));
        outb(base + 7u, 0x24u);
    } else {
        if (lba >= (1ull << 28u)) {
            return false;
        }
        uint16_t base = channel->command_base;
        outb(base + 6u,
            (uint8_t)(0xe0u | (drive << 4u) | ((lba >> 24u) & 0x0fu)));
        ide_delay(channel);
        if (!ide_wait_ready(channel)) {
            console_write("IDE read timed out before command; status 0x");
            print_hex8(ide_status(channel));
            console_putc('\n');
            return false;
        }
        outb(base + 2u, 1u);
        outb(base + 3u, (uint8_t)lba);
        outb(base + 4u, (uint8_t)(lba >> 8u));
        outb(base + 5u, (uint8_t)(lba >> 16u));
        outb(base + 7u, 0x20u);
    }
    if (!ide_wait_data(channel)) {
        console_write("IDE read data phase failed; status 0x");
        print_hex8(ide_status(channel));
        console_putc('\n');
        return false;
    }
    for (uint32_t i = 0u; i < 256u; ++i) {
        uint16_t word = inw(channel->command_base);
        destination[i * 2u] = (uint8_t)word;
        destination[i * 2u + 1u] = (uint8_t)(word >> 8u);
    }
    return true;
}

bool ide_write_sector(struct ide_channel *channel, uint8_t drive,
    uint64_t lba, const uint8_t source[512], bool lba48, bool flush)
{
    uint16_t base = channel->command_base;
    if (base == 0u) {
        return false;
    }

    if (lba48 && lba < (1ull << 48u)) {
        outb(base + 6u, (uint8_t)(0x40u | (drive << 4u)));
        ide_delay(channel);
        if (!ide_wait_ready(channel)) {
            return false;
        }
        outb(base + 2u, 0u);
        outb(base + 3u, (uint8_t)(lba >> 24u));
        outb(base + 4u, (uint8_t)(lba >> 32u));
        outb(base + 5u, (uint8_t)(lba >> 40u));
        outb(base + 2u, 1u);
        outb(base + 3u, (uint8_t)lba);
        outb(base + 4u, (uint8_t)(lba >> 8u));
        outb(base + 5u, (uint8_t)(lba >> 16u));
        outb(base + 7u, 0x34u);
    } else {
        if (lba >= (1ull << 28u)) {
            return false;
        }
        outb(base + 6u,
            (uint8_t)(0xe0u | (drive << 4u) | ((lba >> 24u) & 0x0fu)));
        ide_delay(channel);
        if (!ide_wait_ready(channel)) {
            return false;
        }
        outb(base + 2u, 1u);
        outb(base + 3u, (uint8_t)lba);
        outb(base + 4u, (uint8_t)(lba >> 8u));
        outb(base + 5u, (uint8_t)(lba >> 16u));
        outb(base + 7u, 0x30u);
    }

    if (!ide_wait_data(channel)) {
        return false;
    }
    for (uint32_t i = 0u; i < 256u; ++i) {
        uint16_t word = (uint16_t)source[i * 2u]
            | ((uint16_t)source[i * 2u + 1u] << 8u);
        outw(base, word);
    }
    if (!ide_wait_ready(channel) || (ide_status(channel) & 0x01u) != 0u) {
        return false;
    }

    if (!flush) {
        return true;
    }
    outb(base + 7u, lba48 ? 0xeau : 0xe7u);
    return ide_wait_ready(channel) && (ide_status(channel) & 0x01u) == 0u;
}

bool ahci_run(struct ahci_port *port, uint8_t command,
    uint64_t lba, uint16_t sector_count, void *data, uint32_t data_bytes,
    bool write_to_disk, bool has_data)
{
    return ahci_port_issue(port, command, lba, sector_count, data,
        has_data ? data_bytes : 0u, write_to_disk);
}

bool ahci_issue(struct ahci_port *port, uint8_t command,
    uint64_t lba, uint8_t sector_count, uint8_t data[512],
    bool write_to_disk, bool has_data)
{
    if (has_data && write_to_disk && data != sector_buffer) {
        for (uint32_t i = 0u; i < 512u; ++i) {
            sector_buffer[i] = data[i];
        }
    }
    if (!ahci_run(port, command, lba, sector_count, sector_buffer, 512u,
            write_to_disk, has_data)) {
        return false;
    }
    if (has_data && !write_to_disk && data != sector_buffer) {
        for (uint32_t j = 0u; j < 512u; ++j) {
            data[j] = sector_buffer[j];
        }
    }
    return true;
}

bool ahci_transfer(struct ahci_port *port, uint64_t lba,
    uint32_t count, void *buffer, bool write)
{
    uint8_t *bounce = ata_bounce_buffer();
    uint32_t bytes = count * 512u;

    if (count == 0u || count > ATA_BOUNCE_SECTORS
        || lba + count > (1ull << 48u) || ata_fault_take() != ATA_OK) {
        return false;
    }
    if (write) {
        for (uint32_t i = 0u; i < bytes; ++i) {
            bounce[i] = ((const uint8_t *)buffer)[i];
        }
    }
    if (!ahci_run(port, write ? 0x35u : 0x25u, lba, (uint16_t)count, bounce,
            bytes, write, true)) {
        return false;
    }
    if (!write) {
        for (uint32_t i = 0u; i < bytes; ++i) {
            ((uint8_t *)buffer)[i] = bounce[i];
        }
    }
    return true;
}

bool ahci_read_sector(struct ahci_port *port, uint64_t lba,
    uint8_t destination[512])
{
    if (lba >= (1ull << 48u)) {
        return false;
    }
    return ahci_issue(port, 0x25u, lba, 1u, destination, false, true);
}

bool ahci_identify(struct ahci_port *port, uint16_t identify[256])
{
    if (!ahci_issue(port, 0xecu, 0u, 0u, sector_buffer, false, true)) {
        return false;
    }
    for (uint32_t i = 0u; i < 256u; ++i) {
        identify[i] = (uint16_t)sector_buffer[i * 2u]
            | ((uint16_t)sector_buffer[i * 2u + 1u] << 8u);
    }
    return true;
}

bool ahci_write_sector(struct ahci_port *port, uint64_t lba,
    const uint8_t source[512], bool flush)
{
    if (lba >= (1ull << 48u)) {
        return false;
    }
    for (uint32_t i = 0u; i < 512u; ++i) {
        sector_buffer[i] = source[i];
    }
    if (!ahci_issue(port, 0x35u, lba, 1u, sector_buffer, true, true)) {
        return false;
    }
    return !flush || ahci_issue(port, 0xeau, 0u, 0u,
        sector_buffer, false, false);
}

void discover_ide_disks(void)
{
    static const char *const names[2][2] = {
        {"IDE0-M", "IDE0-S"}, {"IDE1-M", "IDE1-S"}
    };
    uint16_t identify[256];
    for (uint8_t channel_index = 0u; channel_index < 2u; ++channel_index) {
        struct ide_channel *channel = &ide_channels[channel_index];
        for (uint8_t drive = 0u; drive < 2u; ++drive) {
            if (ide_identify(channel, drive, identify)) {
                (void)add_disk(names[channel_index][drive], identify,
                    false, channel_index, drive, NULL);
            }
        }
    }
}

void discover_ahci_disks(void)
{
    uint16_t identify[256];
    for (uint32_t controller_index = 0u;
        controller_index < ahci_controller_count; ++controller_index) {
        struct pci_device *device = &ahci_controllers[controller_index];
        uint64_t base = device->bars[5] & ~0x0full;
        if (base == 0u || device->bars[5] == 0xffffffffu
            || (device->bars[5] & 1u) != 0u
            || (device->bars[5] & 6u) == 4u
            || base + 0x1100u >= 0x100000000ull) {
            continue;
        }
        uint16_t command = pci_read16(device->bus, device->slot,
            device->function, 0x04u);
        pci_write16(device->bus, device->slot, device->function, 0x04u,
            (uint16_t)(command | (1u << 1u) | (1u << 2u)));

        struct ahci_hba *hba = &ahci_hbas[controller_index];
        if (ahci_init(hba, (void *)(uintptr_t)base) != AHCI_OK) {
            continue;
        }
        for (uint32_t i = 0u; i < hba->port_count; ++i) {
            struct ahci_port *port = &hba->ports[i];
            uint32_t port_index = port->index;
            if (!ahci_port_probe(port)
                || port->signature != AHCI_SIG_ATA) {
                continue;
            }
            char name[8] = {'S','A','T','A','0','0','\0','\0'};
            name[4] = (char)('0' + controller_index);
            name[5] = (char)('0' + port_index / 10u);
            name[6] = (char)('0' + port_index % 10u);
            if (ahci_identify(port, identify)) {
                (void)add_disk(name, identify, true, 0u, 0u, port);
            }
        }
    }
}

void discover_storage(void)
{
    discover_pci_controllers();
    discover_ide_disks();
    discover_ahci_disks();
}

uint64_t disk_io_begin(void)
{
    return gui_active ? pit_clock_ticks() : 0u;
}

bool disk_io_end(bool write, bool ok, uint64_t started)
{
    if (ok) {
        if (write) {
            disk_write_bytes_total += 512u;
        } else {
            disk_read_bytes_total += 512u;
        }
    }
    if (gui_active) {
        disk_busy_ticks_total += pit_clock_ticks() - started;
    }
    return ok;
}

bool disk_read_sector(struct disk_device *disk, uint64_t lba,
    uint8_t destination[512])
{
    if (lba >= disk->sectors) {
        return false;
    }
    uint64_t started = disk_io_begin();
    bool ok;
    if (disk->ahci) {
        ok = ahci_read_sector(disk->ahci_port, lba, destination);
    } else {
        ok = ide_read_sector(&ide_channels[disk->channel], disk->drive,
            lba, destination, disk->lba48);
    }
    return disk_io_end(false, ok, started);
}

bool disk_write_sector(struct disk_device *disk, uint64_t lba,
    const uint8_t source[512])
{
    if (lba >= disk->sectors) {
        return false;
    }
    uint64_t started = disk_io_begin();
    bool ok;
    if (disk->ahci) {
        if (!disk->lba48) {
            return false;
        }
        ok = ahci_write_sector(disk->ahci_port, lba, source, true);
    } else {
        ok = ide_write_sector(&ide_channels[disk->channel], disk->drive,
            lba, source, disk->lba48, true);
    }
    return disk_io_end(true, ok, started);
}

bool disk_write_sector_unflushed(struct disk_device *disk, uint64_t lba,
    const uint8_t source[512])
{
    if (lba >= disk->sectors) {
        return false;
    }
    uint64_t started = disk_io_begin();
    bool ok;
    if (disk->ahci) {
        if (!disk->lba48) {
            return false;
        }
        ok = ahci_write_sector(disk->ahci_port, lba, source, false);
    } else {
        ok = ide_write_sector(&ide_channels[disk->channel], disk->drive,
            lba, source, disk->lba48, false);
    }
    return disk_io_end(true, ok, started);
}

bool disk_flush(struct disk_device *disk)
{
    if (disk->ahci) {
        return ahci_issue(disk->ahci_port, 0xeau, 0u, 0u,
            sector_buffer, false, false);
    }
    struct ide_channel *channel = &ide_channels[disk->channel];
    outb(channel->command_base + 6u,
        (uint8_t)(0xe0u | (disk->drive << 4u)));
    ide_delay(channel);
    if (!ide_wait_ready(channel)) {
        return false;
    }
    outb(channel->command_base + 7u, disk->lba48 ? 0xeau : 0xe7u);
    return ide_wait_ready(channel)
        && (ide_status(channel) & 0x01u) == 0u;
}

bool disk_dma_channel(const struct disk_device *disk,
    struct ata_dma_channel *out)
{
    const struct ide_channel *channel = &ide_channels[disk->channel];

    out->command_base = channel->command_base;
    out->control_base = channel->control_base;
    out->bus_master_base = channel->bus_master_base;
    return disk->dma;
}

bool dma_report_failure(const struct disk_device *disk, bool write,
    enum ata_status status)
{
    if (storage_quiet) {
        return false;
    }
    console_write("STORAGE: ");
    console_write(disk->name);
    console_write(write ? " DMA write failed: " : " DMA read failed: ");
    console_write(ata_status_name(status));
    console_putc('\n');
    return false;
}

bool disk_transfer_hw(struct disk_device *disk, uint64_t lba,
    uint32_t count, void *buffer, bool write)
{
    struct ata_dma_channel channel;

    if (lba + count > disk->sectors) {
        return false;
    }
    if (disk->ahci) {
        return disk->lba48
            && ahci_transfer(disk->ahci_port, lba, count, buffer, write);
    }
    if (!disk_dma_channel(disk, &channel)) {
        return false;
    }
    struct ata_dma_request request = {
        disk->drive, disk->lba48, write, lba, count, buffer
    };
    enum ata_status status = ata_dma_transfer(&channel, &request);
    return status == ATA_OK || dma_report_failure(disk, write, status);
}

bool disk_transfer(struct disk_device *disk, uint64_t lba,
    uint32_t count, uint8_t *buffer, bool write)
{
    while (count != 0u) {
        uint32_t chunk = count > ATA_BOUNCE_SECTORS ? ATA_BOUNCE_SECTORS : count;
        bool done = false;

        if (disk->ahci || disk->dma) {
            done = disk_transfer_hw(disk, lba, chunk, buffer, write)
                || disk_transfer_hw(disk, lba, chunk, buffer, write);
            if (done && write && !disk->ahci) {
                done = disk_flush(disk);
            }
        }
        if (!done) {
            for (uint32_t i = 0u; i < chunk; ++i) {
                uint8_t *sector = buffer + (uint64_t)i * 512u;
                if (!(write ? disk_write_sector(disk, lba + i, sector)
                        : disk_read_sector(disk, lba + i, sector))) {
                    return false;
                }
            }
        }
        lba += chunk;
        buffer += (uint64_t)chunk * 512u;
        count -= chunk;
    }
    return true;
}

bool block_adapter_read(block_device_t *dev, uint32_t start_lba,
    uint32_t sector_count, void *buf)
{
    return disk_transfer(dev->priv, start_lba, sector_count, buf, false);
}

bool block_adapter_write(block_device_t *dev, uint32_t start_lba,
    uint32_t sector_count, const void *buf)
{
    return disk_transfer(dev->priv, start_lba, sector_count,
        (uint8_t *)(uintptr_t)buf, true);
}

void storage_selftest(struct disk_device *disk)
{
    static uint8_t bulk[8u * 512u];
    uint8_t one[512];
    bool ok = disk->sectors >= 8u && (disk->ahci || disk->dma);

    if (!ok) {
        return;
    }
    ok = disk_transfer_hw(disk, 0u, 8u, bulk, false);
    for (uint32_t i = 0u; ok && i < 8u; ++i) {
        ok = disk->ahci ? ahci_read_sector(disk->ahci_port, i, one)
            : ide_read_sector(&ide_channels[disk->channel], disk->drive, i,
                one, disk->lba48);
        for (uint32_t j = 0u; ok && j < 512u; ++j) {
            ok = one[j] == bulk[i * 512u + j];
        }
    }
    console_write("STORAGE: ");
    console_write(disk->name);
    console_write(disk->ahci ? " AHCI multi-sector" : " ATA bus-master DMA");
    console_write(ok ? " selftest PASS\n" : " selftest FAIL\n");
    if (!ok) {
        disk->dma = false;
    }
}

void scratch_pattern(uint8_t *buf, uint32_t sectors, uint8_t seed)
{
    for (uint32_t i = 0u; i < sectors * 512u; ++i) {
        buf[i] = (uint8_t)(i * 131u + (i >> 9u) * 7u + seed);
    }
}

bool scratch_equal(const uint8_t *a, const uint8_t *b, uint32_t sectors)
{
    for (uint32_t i = 0u; i < sectors * 512u; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool scratch_set_guard(struct disk_device *disk, uint32_t lba,
    uint8_t fill)
{
    uint8_t sector[512];

    for (uint32_t i = 0u; i < 512u; ++i) {
        sector[i] = fill;
    }
    return disk_write_sector(disk, lba, sector);
}

bool scratch_guard_ok(struct disk_device *disk, uint32_t lba,
    uint8_t fill)
{
    uint8_t sector[512];

    if (!disk_read_sector(disk, lba, sector)) {
        return false;
    }
    for (uint32_t i = 0u; i < 512u; ++i) {
        if (sector[i] != fill) {
            return false;
        }
    }
    return true;
}

bool scratch_check_pio(struct disk_device *disk, uint32_t lba,
    const uint8_t *expected, uint32_t sectors)
{
    uint8_t sector[512];

    for (uint32_t s = 0u; s < sectors; ++s) {
        if (!disk_read_sector(disk, lba + s, sector)
            || !scratch_equal(sector, expected + s * 512u, 1u)) {
            return false;
        }
    }
    return true;
}

const char *storage_write_phases(struct disk_device *disk)
{
    static const struct { enum ata_status fault; uint32_t count; } faults[] = {
        {ATA_ERR_BUS_MASTER, 1u}, {ATA_ERR_TIMEOUT, 2u}, {ATA_ERR_DEVICE, 1u}
    };
    const uint32_t first = SCRATCH_BASE_LBA + 1u;

    /* A: raw DMA write, flush, DMA read-back, strict compare, PIO cross-check. */
    if (!scratch_set_guard(disk, SCRATCH_BASE_LBA, 0xa5u)
        || !scratch_set_guard(disk, first + 8u, 0x5au)) {
        return "guard setup";
    }
    scratch_pattern(scratch_source, 8u, 0x11u);
    if (!disk_transfer_hw(disk, first, 8u, scratch_source, true)
        || !disk_flush(disk)) {
        return "A: DMA write/flush";
    }
    if (!disk_transfer_hw(disk, first, 8u, scratch_verify, false)
        || !scratch_equal(scratch_source, scratch_verify, 8u)) {
        return "A: DMA read-back compare";
    }
    if (!scratch_check_pio(disk, first, scratch_source, 8u)) {
        return "A: PIO cross-check";
    }
    if (!scratch_guard_ok(disk, SCRATCH_BASE_LBA, 0xa5u)
        || !scratch_guard_ok(disk, first + 8u, 0x5au)) {
        return "A: guard sectors";
    }

    /* B: injected write faults must recover (retry or PIO) with no collateral damage. */
    for (uint32_t k = 0u; k < sizeof(faults) / sizeof(faults[0]); ++k) {
        bool ok;

        scratch_pattern(scratch_source, 8u, (uint8_t)(0x20u + k));
        ata_inject_fault(faults[k].fault, faults[k].count);
        storage_quiet = true;
        ok = disk_transfer(disk, first, 8u, scratch_source, true);
        storage_quiet = false;
        ata_inject_fault(ATA_OK, 0u);
        if (!ok) {
            return "B: write did not recover";
        }
        if (!disk_transfer_hw(disk, first, 8u, scratch_verify, false)
            || !scratch_equal(scratch_source, scratch_verify, 8u)
            || !scratch_check_pio(disk, first, scratch_source, 8u)) {
            return "B: data after fault";
        }
        if (!scratch_guard_ok(disk, SCRATCH_BASE_LBA, 0xa5u)
            || !scratch_guard_ok(disk, first + 8u, 0x5au)) {
            return "B: guard sectors after fault";
        }
    }
    ata_inject_fault(ATA_ERR_BUS_MASTER, 2u);
    storage_quiet = true;
    {
        bool ok = disk_transfer(disk, first, 8u, scratch_verify, false);

        storage_quiet = false;
        ata_inject_fault(ATA_OK, 0u);
        if (!ok || !scratch_equal(scratch_source, scratch_verify, 8u)) {
            return "B: read did not recover";
        }
    }

    /* C: multi-chunk write (crosses the 128-sector bounce limit) and read-back. */
    if (!scratch_set_guard(disk, first + SCRATCH_FULL_SECTORS, 0x5au)) {
        return "C: guard setup";
    }
    scratch_pattern(scratch_source, SCRATCH_FULL_SECTORS, 0x3cu);
    if (!disk_transfer(disk, first, SCRATCH_FULL_SECTORS, scratch_source, true)
        || !disk_transfer(disk, first, SCRATCH_FULL_SECTORS, scratch_verify,
            false)
        || !scratch_equal(scratch_source, scratch_verify, SCRATCH_FULL_SECTORS)) {
        return "C: chunked write/read-back";
    }
    if (!scratch_guard_ok(disk, SCRATCH_BASE_LBA, 0xa5u)
        || !scratch_guard_ok(disk, first + SCRATCH_FULL_SECTORS, 0x5au)) {
        return "C: guard sectors";
    }
    return 0;
}

void storage_write_selftest(struct disk_device *disk)
{
    static const char magic[] = "BOOK8SCRATCH";
    uint8_t sector[512];
    const char *failure;

    if (!(disk->ahci || disk->dma) || disk->sectors < SCRATCH_MIN_SECTORS
        || !disk_read_sector(disk, 0u, sector)) {
        return;
    }
    for (uint32_t i = 0u; i + 1u < sizeof(magic); ++i) {
        if (sector[i] != (uint8_t)magic[i]) {
            return;
        }
    }
    failure = storage_write_phases(disk);
    console_write("STORAGE: ");
    console_write(disk->name);
    console_write(" write selftest ");
    if (failure == 0) {
        console_write("PASS\n");
        return;
    }
    console_write("FAIL: ");
    console_write(failure);
    console_putc('\n');
    disk->dma = false;
}

bool application_read(block_device_t *dev, uint32_t lba,
    uint32_t count, void *buffer)
{
    (void)dev;
    if ((uint64_t)lba + count > application_image_size / 512u) { return false; }
    __builtin_memcpy(buffer, application_image + (uint64_t)lba * 512u, (size_t)count * 512u);
    return true;
}

bool application_write(block_device_t *dev, uint32_t lba,
    uint32_t count, const void *buffer)
{
    (void)dev;
    if ((uint64_t)lba + count > application_image_size / 512u) { return false; }
    __builtin_memcpy(application_image + (uint64_t)lba * 512u, buffer, (size_t)count * 512u);
    return true;
}

void register_block_devices(void)
{
    if (application_image != NULL) {
        if (block_register("APPS", application_image_size / 512u,
            application_read, application_write, NULL) == NULL) {
            console_write("BOOK8-APPS: cannot register internal application volume.\n");
            application_image = NULL;
        } else {
            console_write("BOOK8-APPS: internal application volume registered at A:.\n");
        }
    }
    for (uint32_t i = 0u; i < disk_count; ++i) {
        uint64_t sectors = disks[i].sectors;

        storage_selftest(&disks[i]);
        storage_write_selftest(&disks[i]);
        if (sectors > 0xffffffffull) {
            sectors = 0xffffffffull;
        }
        if (block_register(disks[i].name, sectors, block_adapter_read,
            block_adapter_write, &disks[i]) == NULL) {
            console_write("STORAGE: block device registry capacity reached.\n");
            break;
        }
    }
}

bool filesystem_read_sector(uint32_t disk, uint64_t lba,
    uint8_t sector[512])
{
    if (application_image != NULL) {
        if (disk == 0u) {
            return lba <= UINT32_MAX && application_read(NULL, (uint32_t)lba, 1u, sector);
        }
        --disk;
    }
    return disk < disk_count && disk_read_sector(&disks[disk], lba, sector);
}

bool filesystem_write_sector(uint32_t disk, uint64_t lba,
    const uint8_t sector[512])
{
    if (application_image != NULL) {
        if (disk == 0u) {
            return lba <= UINT32_MAX && application_write(NULL, (uint32_t)lba, 1u, sector);
        }
        --disk;
    }
    return disk < disk_count && disk_write_sector(&disks[disk], lba, sector);
}

bool registry_write_file(const char *path, const void *data,
    uint32_t size)
{
    return registry_write_allowed && fs_write_file(path, data, size);
}

uint64_t filesystem_disk_size(uint32_t disk)
{
    if (application_image != NULL) {
        if (disk == 0u) { return application_image_size / 512u; }
        --disk;
    }
    return disk < disk_count ? disks[disk].sectors : 0u;
}

void filesystem_print(const char *text)
{
    console_write(text);
}

void print_disk_inventory(void)
{
    console_write("PCI IDE controllers: ");
    print_unsigned(ide_controller_count);
    console_write("; AHCI SATA controllers: ");
    print_unsigned(ahci_controller_count);
    console_putc('\n');
    if (disk_count == 0u) {
        console_write("No ATA disks identified. Check controller mode and VM disk settings.\n");
        return;
    }
    for (uint32_t i = 0u; i < disk_count; ++i) {
        console_putc('[');
        print_unsigned(i);
        console_write("] ");
        console_write(disks[i].name);
        console_write(": ");
        console_write(disks[i].model);
        console_write(" (");
        print_unsigned(disks[i].sectors);
        console_write(" sectors, ");
        console_write(disks[i].ahci ? "AHCI SATA" : disks[i].dma ? "ATA IDE DMA" : "ATA IDE PIO");
        console_write(disks[i].lba48 ? ", LBA48)\n" : ", LBA28)\n");
    }
}

bool exec_drive_extent(uint32_t drive, struct fs_extent *extent)
{
    if (drive == UINT32_MAX)
        drive = (uint32_t)(fs_current_path()[0] - 'A');
    uint32_t disk;
    uint64_t first, count;
    if (!fs_drive_extent(drive, &disk, &first, &count)
        || first > UINT32_MAX || count > UINT32_MAX)
        return false;
    extent->dev = block_get(disk);
    extent->first_lba = (uint32_t)first;
    extent->sector_count = (uint32_t)count;
    return extent->dev != NULL;
}
