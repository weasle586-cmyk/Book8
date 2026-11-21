#ifndef BOOK8_B8_DRIVER_H
#define BOOK8_B8_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define B8_DRIVER_ABI_VERSION 2u
#define B8_DRIVER_ENTRY_SYMBOL "b8_driver_entry"
#define B8_DRIVER_MAX_IMAGE_SIZE (8u * 1024u * 1024u)

typedef int32_t (*b8_driver_init_fn)(void);
typedef int32_t (*b8_driver_shutdown_fn)(void);
typedef int32_t (*b8_driver_read_fn)(uint64_t offset, void *buffer,
    size_t size);
typedef int32_t (*b8_driver_write_fn)(uint64_t offset, const void *buffer,
    size_t size);
typedef int32_t (*b8_driver_ioctl_fn)(uint32_t command, void *argument);
typedef int32_t (*b8_driver_irq_handler_fn)(void);

struct b8_driver_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t programming_interface;
    uint32_t class_code;
    uint64_t bar0_base;
    uint64_t bar1_base;
};

struct b8_framebuffer_info {
    uint64_t physical_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bits_per_pixel;
    uint8_t red_position;
    uint8_t green_position;
    uint8_t blue_position;
};

typedef int32_t (*b8_graphics_fill_rect_fn)(void *state, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height, uint32_t xrgb);
typedef int32_t (*b8_graphics_copy_rect_fn)(void *state, uint32_t src_x,
    uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width,
    uint32_t height);

struct b8_graphics_ops {
    uint32_t abi_version;
    b8_graphics_fill_rect_fn fill_rect;
    b8_graphics_copy_rect_fn copy_rect;
};

typedef int32_t (*b8_driver_probe_fn)(const struct b8_driver_device *device,
    void *state);
typedef void (*b8_driver_remove_fn)(const struct b8_driver_device *device,
    void *state);
typedef void (*b8_driver_refresh_fn)(void *state);
typedef const struct b8_graphics_ops *(*b8_driver_graphics_fn)(void);

/* Callbacks execute in ring 0. Only kernel-verified modules may be loaded. */
typedef struct b8_driver {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t subsystem_id;
    uint8_t pci_class;
    uint8_t pci_subclass;
    uint8_t pci_progif;
    uint8_t reserved;
    b8_driver_init_fn init;
    b8_driver_shutdown_fn shutdown;
    b8_driver_probe_fn probe;
    b8_driver_remove_fn remove;
    b8_driver_refresh_fn refresh;
    b8_driver_read_fn read;
    b8_driver_write_fn write;
    b8_driver_ioctl_fn ioctl;
    b8_driver_irq_handler_fn irq_handler;
    b8_driver_graphics_fn graphics;
} b8_driver_t;

typedef const b8_driver_t *(*b8_driver_entry_fn)(void);

#endif
