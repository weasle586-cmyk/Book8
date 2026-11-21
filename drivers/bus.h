#ifndef BOOK8_BUS_H
#define BOOK8_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum book8_bus_type {
    BOOK8_BUS_PCI,
    BOOK8_BUS_USB
};

struct book8_bus_match {
    enum book8_bus_type bus_type;
    uint16_t vendor_id, vendor_mask;
    uint16_t device_id, device_mask;
    uint32_t class_code, class_mask;
};

struct book8_bus_driver;

struct book8_bus_device {
    enum book8_bus_type bus_type;
    uint16_t vendor_id, device_id;
    uint32_t class_code;
    uint8_t irq_line;
    uint16_t segment;
    uint8_t bus, device, function;
    uint8_t address, interface_number;
    uint64_t mmio_base, mmio_size;
    volatile uint8_t *mapped_mmio;
    const struct book8_bus_driver *driver;
    void *private_data;
};

struct book8_bus_driver {
    const char *name;
    const struct book8_bus_match *matches;
    size_t match_count;
    uint32_t bar0_window_size;
    bool (*probe)(struct book8_bus_device *);
    void (*remove)(struct book8_bus_device *);
};

static inline bool book8_bus_match_device(const struct book8_bus_match *match,
    const struct book8_bus_device *device)
{
    return match->bus_type == device->bus_type
        && ((device->vendor_id & match->vendor_mask)
            == (match->vendor_id & match->vendor_mask))
        && ((device->device_id & match->device_mask)
            == (match->device_id & match->device_mask))
        && ((device->class_code & match->class_mask)
            == (match->class_code & match->class_mask));
}

#endif
