#include "kernel_internal.h"

/* PCI configuration, driver MMIO mapping and the b8_hal export table. */

uint32_t pci_legacy_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16u)
        | ((uint32_t)slot << 11u) | ((uint32_t)function << 8u)
        | ((uint32_t)offset & 0xfcu);
    outl(0xcf8u, address);
    return inl(0xcfcu);
}

uint32_t book8_pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return book8_pcie_read32(&pcie, 0u, bus, slot, function,
        (uint16_t)(offset & 0xfcu));
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return book8_pci_read32(bus, slot, function, offset);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    uint32_t value = pci_read32(bus, slot, function, offset);
    return (uint16_t)(value >> ((offset & 2u) * 8u));
}

void pci_legacy_write16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint16_t value)
{
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16u)
        | ((uint32_t)slot << 11u) | ((uint32_t)function << 8u)
        | ((uint32_t)offset & 0xfcu);
    outl(0xcf8u, address);
    outw((uint16_t)(0xcfcu + (offset & 2u)), value);
}

void book8_pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint16_t value)
{
    book8_pcie_write16(&pcie, 0u, bus, slot, function, offset, value);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint16_t value)
{
    book8_pci_write16(bus, slot, function, offset, value);
}

void pci_get_device(struct pci_device *device, uint8_t bus,
    uint8_t slot, uint8_t function, uint32_t identity, uint32_t class_revision)
{
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    device->vendor = (uint16_t)identity;
    device->device = (uint16_t)(identity >> 16u);
    device->programming_interface = (uint8_t)(class_revision >> 8u);
    for (uint8_t bar = 0u; bar < 6u; ++bar) {
        device->bars[bar] = pci_read32(bus, slot, function,
            (uint8_t)(0x10u + bar * 4u));
    }
}

void ide_enable_bus_master(const struct pci_device *device)
{
    uint32_t bar4 = device->bars[4];
    if ((device->programming_interface & 0x80u) == 0u || (bar4 & 1u) == 0u
        || (bar4 & ~3u) == 0u || (bar4 & ~3u) > 0xfff0u) {
        return;
    }
    uint16_t command = pci_read16(device->bus, device->slot,
        device->function, 0x04u);
    pci_write16(device->bus, device->slot, device->function, 0x04u,
        (uint16_t)(command | (1u << 0u) | (1u << 2u)));
    for (uint8_t channel = 0u; channel < 2u; ++channel) {
        if (ide_channels[channel].bus_master_base == 0u) {
            ide_channels[channel].bus_master_base =
                (uint16_t)((bar4 & ~3u) + channel * 8u);
        }
    }
}

void ide_set_channel_controller(const struct pci_device *device)
{
    uint8_t programming_interface = device->programming_interface;
    ide_enable_bus_master(device);
    for (uint8_t channel = 0u; channel < 2u; ++channel) {
        uint8_t native_bit = channel == 0u ? 0u : 2u;
        bool native_mode = ((programming_interface >> native_bit) & 1u) != 0u;
        if (!native_mode) {
            continue;
        }
        uint32_t command = device->bars[channel * 2u] & ~3u;
        uint32_t control = device->bars[channel * 2u + 1u] & ~3u;
        if (command == 0u || command > 0xffffu || control == 0u
            || control > 0xfffbu) {
            continue;
        }
        ide_channels[channel].command_base = (uint16_t)command;
        ide_channels[channel].control_base = (uint16_t)control;
        ide_channels[channel].controller = *device;
        ide_channels[channel].has_pci_controller = true;
    }
}

void discover_pci_controllers(void)
{
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint32_t identity = pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 0u);
            uint16_t vendor = (uint16_t)identity;
            if (vendor == 0xffffu || vendor == 0u) {
                continue;
            }
            uint32_t class_revision =
                pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 8u);
            uint8_t class_code = (uint8_t)(class_revision >> 24u);
            uint8_t subclass = (uint8_t)(class_revision >> 16u);
            uint8_t header_type =
                (uint8_t)(pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x0cu) >> 16u);
            uint8_t function_count = (header_type & 0x80u) != 0u ? 8u : 1u;

            for (uint32_t function = 0u; function < function_count; ++function) {
                identity = pci_read32((uint8_t)bus, (uint8_t)slot,
                    (uint8_t)function, 0u);
                if ((uint16_t)identity == 0xffffu || (uint16_t)identity == 0u) {
                    continue;
                }
                class_revision = pci_read32((uint8_t)bus, (uint8_t)slot,
                    (uint8_t)function, 8u);
                class_code = (uint8_t)(class_revision >> 24u);
                subclass = (uint8_t)(class_revision >> 16u);
                if (class_code == 0x02u) {
                    pci_network_present = true;
                } else if (class_code == 0x04u) {
                    pci_sound_present = true;
                }
                if (class_code != 0x01u || (subclass != 0x01u && subclass != 0x06u)) {
                    continue;
                }

                struct pci_device device = {0};
                pci_get_device(&device, (uint8_t)bus, (uint8_t)slot,
                    (uint8_t)function, identity, class_revision);
                /* Storage DMA needs bus mastering; firmware usually sets it, but
                 * an in-RAM relaunch clears it on every endpoint first. */
                uint16_t pci_command = pci_read16((uint8_t)bus, (uint8_t)slot,
                    (uint8_t)function, 4u);
                if ((pci_command & 4u) == 0u) {
                    pci_write16((uint8_t)bus, (uint8_t)slot, (uint8_t)function, 4u,
                        (uint16_t)(pci_command | 4u));
                }
                if (subclass == 0x01u && ide_controller_count < MAX_CONTROLLERS) {
                    ide_controllers[ide_controller_count++] = device;
                    ide_set_channel_controller(&device);
                } else if (subclass == 0x06u
                    && device.programming_interface == 0x01u
                    && ahci_controller_count < MAX_CONTROLLERS) {
                    ahci_controllers[ahci_controller_count++] = device;
                }
            }
        }
    }
}

void pci_legacy_write32(uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint32_t value)
{
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16u)
        | ((uint32_t)slot << 11u) | ((uint32_t)function << 8u)
        | ((uint32_t)offset & 0xfcu);
    outl(0xcf8u, address);
    outl(0xcfcu, value);
}

void book8_pci_write32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint32_t value)
{
    book8_pcie_write32(&pcie, 0u, bus, slot, function, offset, value);
}

void driver_pci_write32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint32_t value)
{
    (void)ctx;
    book8_pci_write32(bus, slot, function, offset, value);
}

uint64_t *driver_mmio_pde(uint64_t page)
{
    uint64_t directory_index = page / 512u;
    uint64_t entry_index = page % 512u;
    uint64_t *directory = directory_index == 0u ? boot_pd0
        : directory_index == 1u ? boot_pd1
        : directory_index == 2u ? boot_pd2 : boot_pd3;
    return &directory[entry_index];
}

struct driver_mmio_record *driver_mmio_find(uint64_t page)
{
    for (uint32_t i = 0u; i < DRIVER_MMIO_MAP_MAX; ++i) {
        if (driver_mmio_records[i].refs != 0u
            && driver_mmio_records[i].page == page) {
            return &driver_mmio_records[i];
        }
    }
    return NULL;
}

struct driver_mmio_record *driver_mmio_free_slot(void)
{
    for (uint32_t i = 0u; i < DRIVER_MMIO_MAP_MAX; ++i) {
        if (driver_mmio_records[i].refs == 0u) {
            return &driver_mmio_records[i];
        }
    }
    return NULL;
}

void driver_mmio_reload_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0\n\tmov %0, %%cr3"
        : "=r"(cr3) :: "memory");
}

volatile uint8_t *driver_map_mmio(void *ctx, uint64_t phys, uint64_t size,
    bool uncached)
{
    (void)ctx;
    if (size == 0u || phys >= 0x100000000ull
        || size > 0x100000000ull - phys) {
        return NULL;
    }
    if (uncached) {
        uint64_t first = phys >> 21u;
        uint64_t last = (phys + size - 1u) >> 21u;
        uint32_t needed = 0u;
        uint32_t available = 0u;

        for (uint64_t page = first; page <= last; ++page) {
            if (driver_mmio_find(page) == NULL) {
                ++needed;
            }
        }
        for (uint32_t i = 0u; i < DRIVER_MMIO_MAP_MAX; ++i) {
            if (driver_mmio_records[i].refs == 0u) {
                ++available;
            }
        }
        if (needed > available) {
            return NULL;
        }
        for (uint64_t page = first; page <= last; ++page) {
            uint64_t *pde = driver_mmio_pde(page);
            struct driver_mmio_record *record = driver_mmio_find(page);
            if (record == NULL) {
                record = driver_mmio_free_slot();
                record->page = page;
                record->original_pde = *pde;
                record->refs = 0u;
            }
            ++record->refs;
            *pde |= 0x18u;
        }
        driver_mmio_reload_cr3();
    }
    return (volatile uint8_t *)(uintptr_t)phys;
}

void driver_unmap_mmio(void *ctx, const volatile void *pointer,
    uint64_t size)
{
    (void)ctx;
    if (pointer == NULL || size == 0u) {
        return;
    }
    uint64_t phys = (uint64_t)(uintptr_t)pointer;
    if (phys >= 0x100000000ull || size > 0x100000000ull - phys) {
        return;
    }
    uint64_t first = phys >> 21u;
    uint64_t last = (phys + size - 1u) >> 21u;
    bool changed = false;

    for (uint64_t page = first; page <= last; ++page) {
        struct driver_mmio_record *record = driver_mmio_find(page);
        if (record == NULL) {
            continue;
        }
        if (record->refs > 1u) {
            --record->refs;
            continue;
        }
        *driver_mmio_pde(page) = record->original_pde;
        record->refs = 0u;
        record->page = 0u;
        record->original_pde = 0u;
        changed = true;
    }
    if (changed) {
        driver_mmio_reload_cr3();
    }
}

volatile uint8_t *book8_apic_map_mmio(uint64_t physical, uint64_t size)
{
    return driver_map_mmio(NULL, physical, size, true);
}

uint32_t b8_hal_pci_read32(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset)
{
    return book8_pci_read32(bus, device, function, offset);
}

void b8_hal_pci_write32(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint32_t value)
{
    book8_pci_write32(bus, device, function, offset, value);
}

void b8_hal_pci_write16(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint16_t value)
{
    book8_pci_write16(bus, device, function, offset, value);
}

volatile uint8_t *b8_hal_map_mmio(uint64_t physical, uint64_t size,
    uint32_t uncached)
{
    return driver_map_mmio(NULL, physical, size, uncached != 0u);
}

void b8_hal_unmap_mmio(const volatile void *pointer, uint64_t size)
{
    driver_unmap_mmio(NULL, pointer, size);
}

int32_t b8_hal_dma_address(const void *pointer, size_t size,
    uint64_t *physical)
{
    return vmm_dma_address(pointer, size, physical) ? 1 : 0;
}

void b8_hal_relax(void)
{
    __asm__ volatile ("pause");
}

void b8_hal_serial_putc(char value)
{
    serial_putc(value);
}

void b8_hal_log(const char *text)
{
    if (text != NULL)
        pcie_log(text);
}

bool b8_hal_reg_delete_tree(const char *path)
{
    return reg_delete_tree(path);
}

bool b8_hal_reg_set_str(const char *path, const char *name,
    const char *value)
{
    return reg_set_str(path, name, value);
}

bool b8_hal_reg_set_u32(const char *path, const char *name, uint32_t value)
{
    return reg_set_u32(path, name, value);
}

int b8_hal_snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

int32_t b8_hal_framebuffer_info(struct b8_framebuffer_info *info)
{
    if (info == NULL || screen.pixels == NULL)
        return -BOOK8_ENODEV;
    *info = (struct b8_framebuffer_info){
        .physical_base = (uint64_t)(uintptr_t)screen.pixels,
        .width = screen.scanout_width,
        .height = screen.scanout_height,
        .pitch = screen.scanout_pitch_bytes,
        .bits_per_pixel = screen.bytes_per_pixel * 8u,
        .red_position = screen.red_position,
        .green_position = screen.green_position,
        .blue_position = screen.blue_position,
    };
    return 0;
}
