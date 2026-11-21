#include "usb.h"
#include "ohci.h"
#include "mouse_input.h"
#include "../drivers/pcie.h"
#include "registry.h"

#include <stddef.h>
#include "../include/stdio.h"

#ifdef BOOK8_USB_DRIVER_MODULE
extern uint32_t b8_hal_pci_read32(uint8_t, uint8_t, uint8_t, uint8_t);
extern void b8_hal_pci_write32(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
extern void b8_hal_pci_write16(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
extern int32_t b8_hal_dma_address(const void *, size_t, uint64_t *);
extern void b8_hal_serial_putc(char);
extern bool b8_hal_reg_delete_tree(const char *);
extern bool b8_hal_reg_set_str(const char *, const char *, const char *);
extern bool b8_hal_reg_set_u32(const char *, const char *, uint32_t);
extern int b8_hal_snprintf(char *, size_t, const char *, ...);
#define book8_pci_read32 b8_hal_pci_read32
#define book8_pci_write32 b8_hal_pci_write32
#define book8_pci_write16 b8_hal_pci_write16
#define book8_usb_debug_serial_putc b8_hal_serial_putc
#define reg_delete_tree b8_hal_reg_delete_tree
#define reg_set_str b8_hal_reg_set_str
#define reg_set_u32 b8_hal_reg_set_u32
#define snprintf b8_hal_snprintf
#endif

#define USB_MAX_UHCI 16u
#define USB_MAX_EHCI 8u
#define UHCI_FRAME_COUNT 1024u
#define UHCI_MAX_TDS 32u
#define UHCI_STATUS_ACTIVE (1u << 23u)
#define UHCI_STATUS_LOW_SPEED (1u << 26u)
#define UHCI_STATUS_ERROR_COUNT (3u << 27u)
#define UHCI_STATUS_SHORT_PACKET_DETECT (1u << 29u)
#define UHCI_STATUS_ISOCHRONOUS (1u << 25u)
#define UHCI_STATUS_ERRORS (0x3fu << 17u)
#define UHCI_LINK_TERMINATE 1u
#define UHCI_LINK_QH 2u
#define UHCI_LINK_DEPTH 4u
#define UHCI_LINK_POINTER_MASK 0xfffffff0u
#define UHCI_LINK_ATTRIBUTE_MASK \
    (UHCI_LINK_TERMINATE | UHCI_LINK_QH | UHCI_LINK_DEPTH)
#define UHCI_COMMAND_RUN 1u
#define UHCI_COMMAND_RESET 2u
#define UHCI_PCI_LEGACY_SUPPORT 0xc0u
#define UHCI_LEGSUP_CLEAR 0x8f00u

/* Build-time diagnostic switches (override with -D, see build.sh):
   BOOK8_DIAG_REGS=1         log PCI 0xC0 / EHCI PORTSC / UHCI USBSTS+PORTSC
   BOOK8_DIAG_SKIP_LEGSUP=1  test A: never write PCI 0xC0
   BOOK8_DIAG_NO_UHCI_POLL=1 test B: never poll UHCI HID devices */
#ifndef BOOK8_DIAG_REGS
#define BOOK8_DIAG_REGS 1
#endif
#ifndef BOOK8_DIAG_SKIP_LEGSUP
#define BOOK8_DIAG_SKIP_LEGSUP 0
#endif
#ifndef BOOK8_DIAG_NO_UHCI_POLL
#define BOOK8_DIAG_NO_UHCI_POLL 0
#endif
#define BOOK8_DIAG_LOG_SIZE 8192u
#define UHCI_LEGSUP_PIRQ_ENABLE 0x2000u
#define UHCI_COMMAND_CONFIGURED 0x40u
#define UHCI_COMMAND_MAX_PACKET 0x80u
#define UHCI_PORT_CONNECTED 1u
#define UHCI_PORT_CONNECT_CHANGE 2u
#define UHCI_PORT_ENABLED 4u
#define UHCI_PORT_ENABLE_CHANGE 8u
#define UHCI_PORT_LOW_SPEED 0x0100u
#define UHCI_PORT_RESET 0x200u
#define UHCI_PORT_CONTROL_MASK (UHCI_PORT_ENABLED | UHCI_PORT_RESET)
#define PCI_COMMAND_IO_SPACE (1u << 0u)
#define PCI_COMMAND_MEMORY_SPACE (1u << 1u)
#define PCI_COMMAND_BUS_MASTER (1u << 2u)
#define EHCI_HCC_EECP_SHIFT 8u
#define EHCI_LEGSUP_CAPABILITY_ID 1u
#define EHCI_LEGSUP_OS_OWNED (1u << 24u)
#define EHCI_LEGSUP_BIOS_OWNED (1u << 16u)
#define EHCI_HCSPARAMS_PORT_COUNT_MASK 0x0fu
#define EHCI_HCSPARAMS_COMPANION_COUNT_SHIFT 8u
#define EHCI_HCSPARAMS_COMPANION_COUNT_MASK (0x0fu << 8u)
#define EHCI_HCSPARAMS_PPC (1u << 4u)
#define EHCI_PORT_ROUTE_COUNT 16u
#define EHCI_PORT_ROUTE_BITS 4u
#define UHCI_STATUS_FRAME_NUMBER_MASK 0x03ffu
#define UHCI_STATUS_ACTUAL_LENGTH_MASK 0x07ffu
#define UHCI_STATUS_HALTED (1u << 5u)
#define UHCI_STOP_WAIT_LIMIT 100000u
#define UHCI_HID_STALL_POLL_LIMIT 10000000u
#define EHCI_PORT_CONNECTED (1u << 0u)
#define EHCI_PORT_ENABLE (1u << 2u)
#define EHCI_PORT_RESET (1u << 8u)
#define EHCI_PORT_POWER (1u << 12u)
#define EHCI_PORT_OWNER (1u << 13u)
#define EHCI_PORT_LINE_STATUS_MASK (3u << 10u)
#define USB_PID_OUT 0xe1u
#define USB_PID_IN 0x69u
#define USB_PID_SETUP 0x2du
#define USB_DEVICE_ADDRESS 1u
#define USB_MAX_PACKET 64u
#define USB_DESCRIPTOR_BUFFER 512u
#define USB_FRAME_TIMEOUT_MS 500u
#define USB_FRAME_FALLBACK 20000000u
#define USB_MAX_HID_DEVICES 8u
#define USB_MAX_BUS_BINDINGS 64u
#define USB_MAX_REGISTRY_DEVICES 256u
#define UHCI_DESCRIPTOR_DUMP_LIMIT 512u

enum usb_hid_kind {
    USB_HID_MOUSE,
    USB_HID_KEYBOARD
};

struct uhci_controller {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t io_base;
    uint8_t port_count;
};

struct ehci_controller {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint64_t mmio_base;
    uint32_t operational_offset;
    uint8_t port_count;
    uint8_t companion_count;
    uint8_t companion_mapping_count;
    uint64_t port_route;
    uint8_t companion_map[EHCI_PORT_ROUTE_COUNT];
    bool port_power_control;
    bool route_fallback;
};

struct uhci_queue_head {
    volatile uint32_t horizontal;
    volatile uint32_t element;
} __attribute__((aligned(16)));

struct uhci_transfer_descriptor {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct uhci_queue_head) == 16u,
    "UHCI queue heads must have a 16-byte stride");
_Static_assert(sizeof(struct uhci_transfer_descriptor) == 16u,
    "UHCI transfer descriptors must have a 16-byte stride");

struct usb_setup_packet {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

struct usb_hid_device {
    bool active;
    bool low_speed;
    enum usb_hid_kind kind;
    uint8_t address;
    uint8_t endpoint;
    uint8_t max_packet;
    uint8_t data_toggle;
    uint16_t controller_index;
    bool td_pending;
    uint16_t pending_last_frame;
    uint32_t stagnant_polls;
    uint8_t report_id;
    uint8_t report_size;
    uint16_t mouse_x_bit_offset;
    uint16_t mouse_y_bit_offset;
    uint16_t mouse_wheel_bit_offset;
    uint16_t mouse_buttons_bit_offset;
    uint8_t mouse_x_bit_size;
    uint8_t mouse_y_bit_size;
    uint8_t mouse_wheel_bit_size;
    uint8_t mouse_button_count;
    int32_t mouse_x_logical_min;
    int32_t mouse_x_logical_max;
    int32_t mouse_y_logical_min;
    int32_t mouse_y_logical_max;
    int32_t mouse_wheel_logical_min;
    int32_t mouse_wheel_logical_max;
    bool mouse_x_relative;
    bool mouse_y_relative;
    const struct book8_bus_driver *driver;
};

struct usb_hid_candidate {
    uint8_t interface_number;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    const struct book8_bus_driver *driver;
    uint8_t endpoint;
    uint8_t interval;
    uint16_t max_packet;
    uint16_t report_descriptor_length;
};

struct hid_mouse_layout {
    uint8_t report_id;
    uint16_t report_size;
    uint16_t x_bit_offset;
    uint16_t y_bit_offset;
    uint16_t wheel_bit_offset;
    uint16_t buttons_bit_offset;
    uint8_t x_bit_size;
    uint8_t y_bit_size;
    uint8_t wheel_bit_size;
    uint8_t button_count;
    int32_t x_logical_min;
    int32_t x_logical_max;
    int32_t y_logical_min;
    int32_t y_logical_max;
    int32_t wheel_logical_min;
    int32_t wheel_logical_max;
    bool x_relative;
    bool y_relative;
    bool has_wheel;
    bool valid;
};

static struct uhci_controller controllers[USB_MAX_UHCI];
static uint32_t controller_count;
static struct ehci_controller ehci_controllers[USB_MAX_EHCI];
static uint32_t ehci_controller_count;
static uint32_t detected_ohci;
static uint32_t detected_ehci;
static uint32_t detected_xhci;
static struct usb_hid_device hid_devices[USB_MAX_HID_DEVICES];
static uint32_t hid_device_count;
static uint32_t uhci_initialized[USB_MAX_UHCI];
static bool uhci_schedule_active[USB_MAX_UHCI];

static uint32_t frame_list[USB_MAX_UHCI][UHCI_FRAME_COUNT]
    __attribute__((aligned(4096)));
static struct uhci_queue_head control_queue
    __attribute__((aligned(16)));
static struct uhci_queue_head interrupt_queues[USB_MAX_HID_DEVICES]
    __attribute__((aligned(16)));
static struct uhci_transfer_descriptor transfer_descriptors[UHCI_MAX_TDS]
    __attribute__((aligned(16)));
static struct uhci_transfer_descriptor control_transfer_descriptors[UHCI_MAX_TDS]
    __attribute__((aligned(16)));
static uint8_t descriptor_buffer[USB_DESCRIPTOR_BUFFER]
    __attribute__((aligned(4096)));
static uint8_t setup_buffer[8] __attribute__((aligned(16)));
static uint8_t interrupt_buffers[USB_MAX_HID_DEVICES][USB_MAX_PACKET]
    __attribute__((aligned(4096)));
static struct book8_usb_diagnostics diagnostics;
static uint32_t uhci_descriptor_dump_count;
static const struct book8_bus_driver *bus_drivers;
static size_t bus_driver_count;
static struct book8_bus_device bus_bindings[USB_MAX_BUS_BINDINGS];
static uint32_t bus_binding_count;
static struct book8_bus_device usb_inventory[USB_MAX_REGISTRY_DEVICES];
static uint32_t usb_inventory_count;

static uint32_t usb_dma_address32(const void *address, size_t size)
{
#ifdef BOOK8_USB_DRIVER_MODULE
    uint64_t physical;
    if (b8_hal_dma_address(address, size, &physical) == 0
        || physical > UINT32_MAX)
        return 0u;
    return (uint32_t)physical;
#else
    (void)size;
    return (uint32_t)(uintptr_t)address;
#endif
}

static void descriptor_log_text(const char *text)
{
    while (*text != '\0') {
        book8_usb_debug_serial_putc(*text++);
    }
}

static void descriptor_log_hex32(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (uint32_t shift = 32u; shift != 0u; shift -= 4u) {
        book8_usb_debug_serial_putc(digits[(value >> (shift - 4u)) & 0x0fu]);
    }
}

static void log_uhci_descriptor(const char *event, uint32_t controller_index,
    const char *type, uint32_t descriptor_index,
    const volatile uint32_t *words, uint32_t word_count)
{
    if (uhci_descriptor_dump_count >= UHCI_DESCRIPTOR_DUMP_LIMIT) {
        return;
    }
    ++uhci_descriptor_dump_count;
    descriptor_log_text("UHCI-DESC seq=");
    descriptor_log_hex32(uhci_descriptor_dump_count);
    descriptor_log_text(" event=");
    descriptor_log_text(event);
    descriptor_log_text(" controller=");
    descriptor_log_hex32(controller_index);
    descriptor_log_text(" type=");
    descriptor_log_text(type);
    descriptor_log_text(" index=");
    descriptor_log_hex32(descriptor_index);
    descriptor_log_text(" pa=");
    descriptor_log_hex32((uint32_t)(uintptr_t)words);
    for (uint32_t word = 0u; word < word_count; ++word) {
        descriptor_log_text(" d");
        book8_usb_debug_serial_putc((char)('0' + word));
        descriptor_log_text("=");
        descriptor_log_hex32(words[word]);
    }
    book8_usb_debug_serial_putc('\r');
    book8_usb_debug_serial_putc('\n');
}

static void log_uhci_frame_list(const char *event, uint32_t controller_index,
    const uint32_t *frame_base)
{
    if (uhci_descriptor_dump_count >= UHCI_DESCRIPTOR_DUMP_LIMIT) {
        return;
    }
    ++uhci_descriptor_dump_count;
    descriptor_log_text("UHCI-FLBASE seq=");
    descriptor_log_hex32(uhci_descriptor_dump_count);
    descriptor_log_text(" event=");
    descriptor_log_text(event);
    descriptor_log_text(" controller=");
    descriptor_log_hex32(controller_index);
    descriptor_log_text(" pa=");
    descriptor_log_hex32((uint32_t)(uintptr_t)frame_base);
    descriptor_log_text(" entry0=");
    descriptor_log_hex32(frame_base[0]);
    descriptor_log_text("\r\n");
}

static uint32_t hid_item_value(const uint8_t *descriptor, uint32_t offset,
    uint8_t size)
{
    uint32_t value = 0u;
    for (uint8_t i = 0u; i < size; ++i) {
        value |= (uint32_t)descriptor[offset + i] << (8u * i);
    }
    return value;
}

static int32_t hid_signed_value(uint32_t value, uint8_t size)
{
    if (size == 0u) {
        return 0;
    }
    if (size >= 4u) {
        return (int32_t)value;
    }
    uint8_t sign_bit = (uint8_t)(size * 8u - 1u);
    uint32_t sign = 1u << sign_bit;
    return (int32_t)((value ^ sign) - sign);
}

static struct hid_mouse_layout parse_mouse_report_descriptor(
    const uint8_t *descriptor, uint16_t length)
{
    uint16_t input_bits[256] = {0};
    uint32_t usage_page = 0u;
    uint32_t report_size = 0u;
    uint32_t report_count = 0u;
    int32_t logical_minimum = 0;
    int32_t logical_maximum = 0;
    uint8_t report_id = 0u;
    uint32_t local_usages[16] = {0};
    uint8_t local_usage_count = 0u;
    uint32_t usage_minimum = 0u;
    uint32_t usage_maximum = 0u;
    bool has_usage_minimum = false;
    bool has_usage_maximum = false;
    bool collection_mouse[16] = {false};
    uint8_t collection_depth = 0u;
    bool found_mouse_application = false;
    bool found_x = false;
    bool found_y = false;
    uint8_t x_report_id = 0u;
    uint8_t y_report_id = 0u;
    uint8_t wheel_report_id = 0u;
    uint8_t buttons_report_id = 0u;
    struct hid_mouse_layout layout = {0};

    for (uint32_t offset = 0u; offset < length;) {
        uint8_t prefix = descriptor[offset++];
        if (prefix == 0xfeu) {
            if (offset + 2u > length) {
                break;
            }
            uint8_t long_length = descriptor[offset];
            offset += 2u;
            if (long_length > length - offset) {
                break;
            }
            offset += long_length;
            continue;
        }

        uint8_t size_code = prefix & 3u;
        uint8_t item_size = size_code == 3u ? 4u : size_code;
        uint8_t item_type = (prefix >> 2u) & 3u;
        uint8_t item_tag = prefix >> 4u;
        if (item_size > length - offset) {
            break;
        }
        uint32_t value = hid_item_value(descriptor, offset, item_size);
        offset += item_size;

        if (item_type == 1u) {
            if (item_tag == 0u) {
                usage_page = value;
            } else if (item_tag == 1u) {
                logical_minimum = hid_signed_value(value, item_size);
            } else if (item_tag == 2u) {
                logical_maximum = logical_minimum < 0
                    ? hid_signed_value(value, item_size)
                    : (value <= 0x7fffffffu ? (int32_t)value : 0);
            } else if (item_tag == 7u) {
                report_size = value;
            } else if (item_tag == 8u && value <= 255u) {
                report_id = (uint8_t)value;
            } else if (item_tag == 9u) {
                report_count = value;
            }
        } else if (item_type == 2u) {
            if (item_tag == 0u && local_usage_count < 16u) {
                local_usages[local_usage_count++] = value;
            } else if (item_tag == 1u) {
                usage_minimum = value;
                has_usage_minimum = true;
            } else if (item_tag == 2u) {
                usage_maximum = value;
                has_usage_maximum = true;
            }
        } else if (item_type == 0u) {
            if (item_tag == 10u) {
                if (collection_depth < 16u) {
                    uint32_t collection_usage = local_usage_count != 0u
                        ? local_usages[0] : 0u;
                    uint32_t collection_page = collection_usage > 0xffffu
                        ? collection_usage >> 16u : usage_page;
                    uint32_t collection_id = collection_usage & 0xffffu;
                    bool inherited = collection_depth != 0u
                        && collection_mouse[collection_depth - 1u];
                    collection_mouse[collection_depth] = inherited
                        || (value == 1u && collection_page == 1u
                            && collection_id == 2u);
                    if (collection_mouse[collection_depth]) {
                        found_mouse_application = true;
                    }
                    ++collection_depth;
                }
                local_usage_count = 0u;
                has_usage_minimum = false;
                has_usage_maximum = false;
            } else if (item_tag == 12u) {
                if (collection_depth != 0u) {
                    --collection_depth;
                }
                local_usage_count = 0u;
                has_usage_minimum = false;
                has_usage_maximum = false;
            } else if (item_tag == 8u) {
                bool in_mouse = collection_depth != 0u
                    && collection_mouse[collection_depth - 1u];
                bool variable = (value & 2u) != 0u;
                bool relative = (value & 4u) != 0u;
                if (in_mouse && variable && report_size != 0u
                    && report_size <= 32u && report_count <= 64u) {
                    uint16_t base_bit = input_bits[report_id];
                    for (uint32_t field = 0u; field < report_count; ++field) {
                        uint32_t usage = 0u;
                        if (field < local_usage_count) {
                            usage = local_usages[field];
                        } else if (has_usage_minimum && has_usage_maximum
                            && usage_minimum + field <= usage_maximum) {
                            usage = usage_minimum + field;
                        }
                        uint32_t field_page = usage > 0xffffu
                            ? usage >> 16u : usage_page;
                        uint32_t field_usage = usage & 0xffffu;
                        uint32_t bit_offset = (uint32_t)base_bit
                            + field * report_size;
                        if (field_page == 1u && field_usage == 0x30u
                            && !found_x && bit_offset <= 0xffffu) {
                            layout.x_bit_offset = (uint16_t)bit_offset;
                            layout.x_bit_size = (uint8_t)report_size;
                            layout.x_logical_min = logical_minimum;
                            layout.x_logical_max = logical_maximum;
                            layout.x_relative = relative;
                            x_report_id = report_id;
                            found_x = true;
                        } else if (field_page == 1u && field_usage == 0x31u
                            && !found_y && bit_offset <= 0xffffu) {
                            layout.y_bit_offset = (uint16_t)bit_offset;
                            layout.y_bit_size = (uint8_t)report_size;
                            layout.y_logical_min = logical_minimum;
                            layout.y_logical_max = logical_maximum;
                            layout.y_relative = relative;
                            y_report_id = report_id;
                            found_y = true;
                        } else if (relative && field_page == 1u
                            && field_usage == 0x38u && !layout.has_wheel
                            && bit_offset <= 0xffffu) {
                            layout.wheel_bit_offset = (uint16_t)bit_offset;
                            layout.wheel_bit_size = (uint8_t)report_size;
                            layout.wheel_logical_min = logical_minimum;
                            layout.wheel_logical_max = logical_maximum;
                            wheel_report_id = report_id;
                            layout.has_wheel = true;
                        } else if (field_page == 9u && field_usage >= 1u
                            && field_usage <= 8u
                            && report_size == 1u
                            && layout.button_count < 3u
                            && bit_offset <= 0xffffu) {
                            if (layout.button_count == 0u) {
                                layout.buttons_bit_offset =
                                    (uint16_t)bit_offset;
                                buttons_report_id = report_id;
                            }
                            ++layout.button_count;
                        }
                    }
                }
                uint64_t total_bits = (uint64_t)input_bits[report_id]
                    + (uint64_t)report_size * report_count;
                input_bits[report_id] = total_bits > 0xffffu
                    ? 0xffffu : (uint16_t)total_bits;
                local_usage_count = 0u;
                has_usage_minimum = false;
                has_usage_maximum = false;
            }
        }
    }

    if (found_mouse_application && found_x && found_y
        && x_report_id == y_report_id
        && layout.x_relative == layout.y_relative
        && (layout.button_count == 0u
            || buttons_report_id == x_report_id)) {
        layout.report_id = x_report_id;
        if (layout.has_wheel && wheel_report_id != x_report_id) {
            layout.has_wheel = false;
        }
        if (!layout.x_relative) {
            if (layout.x_logical_max <= layout.x_logical_min
                || layout.y_logical_max <= layout.y_logical_min) {
                layout.valid = false;
                return layout;
            }
        }
        uint32_t bytes = ((uint32_t)input_bits[layout.report_id] + 7u) / 8u;
        if (layout.report_id != 0u) {
            ++bytes;
        }
        if (bytes <= USB_MAX_PACKET && bytes != 0u
            && layout.x_bit_size != 0u && layout.y_bit_size != 0u) {
            layout.report_size = (uint16_t)bytes;
            layout.valid = true;
        }
    }
    return layout;
}

static uint32_t in32(uint16_t port);
static void out32(uint16_t port, uint32_t value);
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset);
static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset);
static void delay(uint32_t count);
static void pci_write32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint32_t value);
static void pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint16_t value);

static bool pci_enable_command(uint8_t bus, uint8_t slot, uint8_t function,
    uint16_t required_bits)
{
    uint16_t command = pci_read16(bus, slot, function, 0x04u);
    pci_write16(bus, slot, function, 0x04u,
        (uint16_t)(command | required_bits));
    return (pci_read16(bus, slot, function, 0x04u) & required_bits)
        == required_bits;
}

static uint16_t in16(uint16_t port);

/* Register-level diagnostics for the bare-metal G41 freeze. Every read here
   is non-destructive (PCI config reads, EHCI/UHCI status register reads);
   output goes to COM1 and to a buffer that the USBREG command prints. */
static char diag_log[BOOK8_DIAG_LOG_SIZE];
static uint32_t diag_log_length;

static void diag_putc(char value)
{
    if (diag_log_length + 1u < sizeof(diag_log)) {
        diag_log[diag_log_length++] = value;
        diag_log[diag_log_length] = '\0';
    }
    if (value == '\n') {
        book8_usb_debug_serial_putc('\r');
    }
    book8_usb_debug_serial_putc(value);
}

static void diag_text(const char *text)
{
    while (*text != '\0') {
        diag_putc(*text++);
    }
}

static void diag_hex(uint32_t value, uint32_t digits)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    for (uint32_t shift = digits * 4u; shift != 0u; shift -= 4u) {
        diag_putc(hex_digits[(value >> (shift - 4u)) & 0x0fu]);
    }
}

static void diag_legsup_all(const char *tag)
{
    if (!BOOK8_DIAG_REGS) {
        return;
    }
    for (uint32_t i = 0u; i < controller_count; ++i) {
        const struct uhci_controller *c = &controllers[i];
        diag_text("DIAG LEGSUP ");
        diag_text(tag);
        diag_text(" U");
        diag_hex(i, 1u);
        diag_text(" PCI=");
        diag_hex(c->bus, 2u);
        diag_putc(':');
        diag_hex(c->slot, 2u);
        diag_putc('.');
        diag_hex(c->function, 1u);
        diag_text(" C0=");
        diag_hex(pci_read16(c->bus, c->slot, c->function,
            UHCI_PCI_LEGACY_SUPPORT), 4u);
        diag_text("\n");
    }
}

static void diag_uhci_regs(const char *tag, uint32_t index)
{
    if (!BOOK8_DIAG_REGS) {
        return;
    }
    const struct uhci_controller *c = &controllers[index];
    diag_text("DIAG UHCI ");
    diag_text(tag);
    diag_text(" U");
    diag_hex(index, 1u);
    diag_text(" USBCMD=");
    diag_hex(in16(c->io_base), 4u);
    diag_text(" USBSTS=");
    diag_hex(in16(c->io_base + 0x02u), 4u);
    diag_text(" USBINTR=");
    diag_hex(in16(c->io_base + 0x04u), 4u);
    for (uint32_t port = 0u; port < c->port_count; ++port) {
        diag_text(" PORTSC");
        diag_hex(port + 1u, 1u);
        diag_putc('=');
        diag_hex(in16(c->io_base + 0x10u + 2u * port), 4u);
    }
    diag_text(" C0=");
    diag_hex(pci_read16(c->bus, c->slot, c->function,
        UHCI_PCI_LEGACY_SUPPORT), 4u);
    diag_text("\n");
}

static void diag_ehci_ports(const char *tag, uint32_t index)
{
    if (!BOOK8_DIAG_REGS) {
        return;
    }
    const struct ehci_controller *c = &ehci_controllers[index];
    volatile const uint32_t *operational =
        (volatile const uint32_t *)(uintptr_t)(c->mmio_base
            + c->operational_offset);
    diag_text("DIAG EHCI ");
    diag_text(tag);
    diag_text(" E");
    diag_hex(index, 1u);
    diag_text(" CONFIGFLAG=");
    diag_hex(operational[0x10u], 8u);
    for (uint32_t port = 0u; port < c->port_count; ++port) {
        diag_text(" PORTSC");
        diag_hex(port + 1u, 1u);
        diag_putc('=');
        diag_hex(operational[0x11u + port], 8u);
    }
    diag_text("\n");
}

const char *book8_usb_diag_log(void)
{
    return diag_log;
}

static void ehci_legacy_handoff(uint8_t bus, uint8_t slot,
    uint8_t function)
{
    uint32_t bar = pci_read32(bus, slot, function, 0x10u);
    if ((bar & 1u) != 0u || (bar & 0x06u) == 0x04u
        || (bar & ~0x0fu) == 0u) {
        return;
    }
    uint64_t base = bar & ~0x0full;
    if (base + 0x100u >= 0x100000000ull) {
        return;
    }
    if (!pci_enable_command(bus, slot, function,
        PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER)) {
        ++diagnostics.pci_enable_failures;
        return;
    }
    volatile uint8_t *capability = (volatile uint8_t *)(uintptr_t)base;
    uint8_t capability_length = capability[0];
    if (capability_length < 0x10u) {
        return;
    }

    uint32_t hccparams = *(volatile uint32_t *)(uintptr_t)(base + 8u);
    uint32_t hcsparams = *(volatile uint32_t *)(uintptr_t)(base + 4u);
    uint8_t extended_pointer =
        (uint8_t)((hccparams >> EHCI_HCC_EECP_SHIFT) & 0xffu);
    struct ehci_controller controller = {
        .bus = bus,
        .slot = slot,
        .function = function,
        .vendor = pci_read16(bus, slot, function, 0u),
        .mmio_base = base,
        .operational_offset = capability_length,
        .port_count = (uint8_t)(hcsparams
            & EHCI_HCSPARAMS_PORT_COUNT_MASK),
        .companion_count = (uint8_t)((hcsparams
            & EHCI_HCSPARAMS_COMPANION_COUNT_MASK)
            >> EHCI_HCSPARAMS_COMPANION_COUNT_SHIFT),
        .port_route =
            *(volatile uint32_t *)(uintptr_t)(base + 0x0cu)
            | ((uint64_t)*(volatile uint32_t *)(uintptr_t)(base + 0x10u)
                << 32u),
        .port_power_control = (hcsparams & EHCI_HCSPARAMS_PPC) != 0u
    };
    for (uint32_t i = 0u; i < EHCI_PORT_ROUTE_COUNT; ++i) {
        controller.companion_map[i] = 0xffu;
    }

    for (uint32_t traversed = 0u; extended_pointer >= 0x40u
        && extended_pointer <= 0xfcu && (extended_pointer & 3u) == 0u
        && traversed < 48u; ++traversed) {
        uint32_t legacy = pci_read32(bus, slot, function,
            extended_pointer);
        uint8_t capability_id = (uint8_t)legacy;
        uint8_t next_pointer = (uint8_t)(legacy >> 8u);
        if (capability_id == EHCI_LEGSUP_CAPABILITY_ID) {
            if (BOOK8_DIAG_REGS) {
                diag_text("DIAG EHCI LEGSUP before handoff CAP=");
                diag_hex(legacy, 8u);
                diag_text(" CTLSTS=");
                diag_hex(extended_pointer <= 0xf8u ? pci_read32(bus, slot,
                    function, (uint8_t)(extended_pointer + 4u)) : 0u, 8u);
                diag_text("\n");
            }
            pci_write32(bus, slot, function, extended_pointer,
                legacy | EHCI_LEGSUP_OS_OWNED);
            bool ownership_released = false;
            for (uint32_t attempt = 0u; attempt < 10000000u; ++attempt) {
                legacy = pci_read32(bus, slot, function, extended_pointer);
                if ((legacy & EHCI_LEGSUP_BIOS_OWNED) == 0u) {
                    if (extended_pointer <= 0xf8u) {
                        pci_write32(bus, slot, function,
                            (uint8_t)(extended_pointer + 4u), 0u);
                    }
                    ++diagnostics.ehci_legacy_handoffs;
                    ownership_released = true;
                    break;
                }
                __asm__ volatile ("pause");
            }
            if (!ownership_released) {
                ++diagnostics.ehci_legacy_handoff_failures;
                return;
            }
            if (BOOK8_DIAG_REGS) {
                diag_text("DIAG EHCI LEGSUP after handoff CAP=");
                diag_hex(pci_read32(bus, slot, function, extended_pointer),
                    8u);
                diag_text("\n");
            }
            break;
        }
        if (next_pointer == 0u) {
            break;
        }
        extended_pointer = next_pointer;
    }

    if (ehci_controller_count < USB_MAX_EHCI
        && controller.port_count != 0u
        && controller.operational_offset + 0x44u
            + (uint32_t)controller.port_count * 4u < 0x100000u) {
        ehci_controllers[ehci_controller_count++] = controller;
    }
}

static void ehci_release_all_to_companions(void)
{
    for (uint32_t index = 0u; index < ehci_controller_count; ++index) {
        struct ehci_controller *controller = &ehci_controllers[index];
        volatile uint32_t *operational =
            (volatile uint32_t *)(uintptr_t)(controller->mmio_base
                + controller->operational_offset);
        if (controller->port_power_control) {
            for (uint32_t port = 0u; port < controller->port_count; ++port) {
                volatile uint32_t *portsc = operational + 0x11u + port;
                uint32_t status = *portsc;
                if ((status & EHCI_PORT_POWER) == 0u) {
                    *portsc = (status & (EHCI_PORT_ENABLE | EHCI_PORT_OWNER))
                        | EHCI_PORT_POWER;
                    ++diagnostics.ehci_powered_ports;
                }
            }
            delay(20000u);
        }
        /* Release every port explicitly (CONFIGFLAG=1 makes the owner bit
         * writable), then CONFIGFLAG=0 returns the rest to the companions. */
        operational[0x10u] = 1u;
        (void)operational[0x10u];
        for (uint32_t port = 0u; port < controller->port_count; ++port) {
            volatile uint32_t *portsc = operational + 0x11u + port;
            uint32_t status = *portsc;
            *portsc = (status & (EHCI_PORT_ENABLE | EHCI_PORT_POWER))
                | EHCI_PORT_OWNER;
        }
        delay(20000u);
        operational[0x10u] = 0u;
        (void)operational[0x10u];
        ++diagnostics.ehci_released_to_ohci;
    }
}

static void ehci_route_companion_ports(void)
{
    for (uint32_t index = 0u; index < ehci_controller_count; ++index) {
        diag_ehci_ports("before-route", index);
    }
    for (uint32_t index = 0u; index < ehci_controller_count; ++index) {
        struct ehci_controller *controller = &ehci_controllers[index];
        volatile uint32_t *operational =
            (volatile uint32_t *)(uintptr_t)(controller->mmio_base
                + controller->operational_offset);
        volatile uint32_t *configflag = operational + 0x10u;
        *configflag = 1u;
        (void)*configflag;
        for (uint32_t port = 0u; port < controller->port_count; ++port) {
            volatile uint32_t *portsc = operational + 0x11u + port;
            uint32_t status = *portsc;
            uint32_t writable = status & (EHCI_PORT_ENABLE
                | EHCI_PORT_RESET | EHCI_PORT_POWER | EHCI_PORT_OWNER);
            if (controller->port_power_control
                && (status & EHCI_PORT_POWER) == 0u) {
                writable |= EHCI_PORT_POWER;
                *portsc = writable;
                delay(20000u);
                status = *portsc;
                if ((status & EHCI_PORT_POWER) != 0u) {
                    ++diagnostics.ehci_powered_ports;
                }
                writable = status & (EHCI_PORT_ENABLE | EHCI_PORT_RESET
                    | EHCI_PORT_POWER | EHCI_PORT_OWNER);
            }
            uint32_t line_status = status & EHCI_PORT_LINE_STATUS_MASK;
            if ((status & EHCI_PORT_CONNECTED) == 0u
                || (status & EHCI_PORT_OWNER) != 0u
                || line_status == 0u
                || line_status == EHCI_PORT_LINE_STATUS_MASK) {
                continue;
            }
            if (controller->companion_mapping_count == 0u) {
                ++diagnostics.ehci_unmapped_companion_ports;
                continue;
            }
            uint32_t companion = controller->route_fallback
                ? port % controller->companion_mapping_count
                : (uint32_t)((controller->port_route
                    >> (port * EHCI_PORT_ROUTE_BITS)) & 0x0fu);
            if ((!controller->route_fallback
                    && companion >= controller->companion_count)
                || companion >= controller->companion_mapping_count) {
                companion = port % controller->companion_mapping_count;
                if (!controller->route_fallback) {
                    ++diagnostics.ehci_route_fallbacks;
                }
                controller->route_fallback = true;
            }
            uint8_t target = controller->companion_map[companion];
            if (target == 0xffu || target >= controller_count) {
                ++diagnostics.ehci_unmapped_companion_ports;
            } else {
                ++diagnostics.ehci_uhci_companion_ports;
            }
            *portsc = (writable & ~EHCI_PORT_ENABLE) | EHCI_PORT_OWNER;
            if ((*portsc & EHCI_PORT_OWNER) != 0u) {
                ++diagnostics.ehci_companion_ports;
            }
        }
    }
    for (uint32_t index = 0u; index < ehci_controller_count; ++index) {
        diag_ehci_ports("after-route", index);
    }
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return book8_pci_read32(bus, slot, function, offset);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint32_t value)
{
    book8_pci_write32(bus, slot, function, offset, value);
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint16_t value)
{
    book8_pci_write16(bus, slot, function, offset, value);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return (uint8_t)(pci_read32(bus, slot, function, offset)
        >> ((offset & 3u) * 8u));
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return (uint16_t)(pci_read32(bus, slot, function, offset)
        >> ((offset & 2u) * 8u));
}

static void io_wait(void)
{
    __asm__ volatile ("outb %0, %1" : :
        "a"((uint8_t)0), "Nd"((uint16_t)0x80u) : "memory");
}

static uint16_t in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static uint32_t in32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : :
        "a"(value), "Nd"(port) : "memory");
}

static void out16(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : :
        "a"(value), "Nd"(port) : "memory");
}

static void out32(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : :
        "a"(value), "Nd"(port) : "memory");
}

static void delay(uint32_t count)
{
    while (count-- != 0u) {
        io_wait();
    }
}

static bool wait_frames(uint16_t io_base, uint32_t frame_count)
{
    uint16_t command = in16(io_base);
    bool was_running = (command & UHCI_COMMAND_RUN) != 0u;
    if (!was_running) {
        out16(io_base, UHCI_COMMAND_CONFIGURED
            | UHCI_COMMAND_MAX_PACKET | UHCI_COMMAND_RUN);
    }
    uint16_t previous = in16(io_base + 0x06u) & 0x03ffu;
    uint32_t elapsed = 0u;
    for (uint32_t attempt = 0u; attempt < USB_FRAME_FALLBACK; ++attempt) {
        uint16_t current = in16(io_base + 0x06u) & 0x03ffu;
        uint16_t advanced = (uint16_t)((current - previous) & 0x03ffu);
        elapsed += advanced;
        previous = current;
        if (elapsed >= frame_count) {
            if (!was_running) {
                out16(io_base, command);
            }
            return true;
        }
        __asm__ volatile ("pause");
    }
    if (!was_running) {
        out16(io_base, command);
    }
    return false;
}

static uint32_t td_actual_length(uint32_t status)
{
    uint32_t encoded = status & 0x7ffu;
    return encoded == 0x7ffu ? 0u : encoded + 1u;
}

static bool stop_uhci_controller(uint16_t io_base)
{
    uint16_t command = in16(io_base);
    out16(io_base, command & (uint16_t)~UHCI_COMMAND_RUN);
    for (uint32_t attempt = 0u; attempt < UHCI_STOP_WAIT_LIMIT; ++attempt) {
        if ((in16(io_base + 0x02u) & UHCI_STATUS_HALTED) != 0u) {
            return true;
        }
        io_wait();
    }
    return false;
}

static bool wait_td(uint16_t io_base, struct uhci_transfer_descriptor *td)
{
    uint16_t previous = in16(io_base + 0x06u) & 0x03ffu;
    uint32_t elapsed = 0u;
    for (uint32_t attempt = 0u; attempt < USB_FRAME_FALLBACK; ++attempt) {
        uint32_t status = td->status;
        if ((status & UHCI_STATUS_ACTIVE) == 0u) {
            return (status & UHCI_STATUS_ERRORS) == 0u;
        }
        uint16_t current = in16(io_base + 0x06u) & 0x03ffu;
        uint16_t advanced = (uint16_t)((current - previous) & 0x03ffu);
        elapsed += advanced;
        previous = current;
        if (elapsed >= USB_FRAME_TIMEOUT_MS) {
            return false;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static void discover_controller(uint8_t bus, uint8_t slot, uint8_t function,
    uint32_t class_revision, bool bar4_valid, bool bar4_io, uint64_t bar4_base,
    uint8_t pending_ehci[USB_MAX_EHCI][3], uint32_t *pending_ehci_count)
{
    uint8_t class_code = (uint8_t)(class_revision >> 24u);
    uint8_t subclass = (uint8_t)(class_revision >> 16u);
    uint8_t programming_interface = (uint8_t)(class_revision >> 8u);

    if (class_code != 0x0cu || subclass != 0x03u) {
        return;
    }
    if (programming_interface == 0x00u
        && controller_count < USB_MAX_UHCI && bar4_valid && bar4_io) {
        uint16_t io_base = (uint16_t)(bar4_base & 0xfff0u);
        if (io_base != 0u) {
            if (!pci_enable_command(bus, slot, function,
                    PCI_COMMAND_IO_SPACE | PCI_COMMAND_BUS_MASTER)) {
                ++diagnostics.pci_enable_failures;
                return;
            }
            controllers[controller_count++] = (struct uhci_controller){
                bus, slot, function, io_base, 2u
            };
        }
    } else if (programming_interface == 0x10u) {
        ++detected_ohci;
        book8_ohci_add(bus, slot, function);
    } else if (programming_interface == 0x20u) {
        ++detected_ehci;
        if (*pending_ehci_count < USB_MAX_EHCI) {
            pending_ehci[*pending_ehci_count][0] = bus;
            pending_ehci[*pending_ehci_count][1] = slot;
            pending_ehci[*pending_ehci_count][2] = function;
            ++*pending_ehci_count;
        }
    } else if (programming_interface == 0x30u) {
        ++detected_xhci;
    }
}

static void discover_controllers(const struct book8_pcie_state *pcie_state)
{
    uint8_t pending_ehci[USB_MAX_EHCI][3];
    uint32_t pending_ehci_count = 0u;
    controller_count = 0u;
    ehci_controller_count = 0u;
    detected_ohci = 0u;
    detected_ehci = 0u;
    detected_xhci = 0u;
    for (uint32_t i = 0u; i < USB_MAX_UHCI; ++i) {
        uhci_initialized[i] = 0u;
        uhci_schedule_active[i] = false;
    }
    if (pcie_state != NULL) {
        for (size_t i = 0u; i < pcie_state->node_count; ++i) {
            const struct book8_pcie_node *node = &pcie_state->nodes[i];
            if (node->segment != 0u) {
                continue;
            }
            const struct book8_pcie_bar *bar4 = &node->bars[4];
            discover_controller(node->bus, node->slot, node->function,
                node->class_revision, bar4->valid, bar4->io, bar4->base,
                pending_ehci, &pending_ehci_count);
        }
    } else {
        for (uint32_t bus = 0u; bus < 256u; ++bus) {
            for (uint32_t slot = 0u; slot < 32u; ++slot) {
                uint32_t identity = pci_read32((uint8_t)bus, (uint8_t)slot,
                    0u, 0u);
                if ((uint16_t)identity == 0xffffu
                    || (uint16_t)identity == 0u) {
                    continue;
                }
                uint8_t header_type = pci_read8((uint8_t)bus,
                    (uint8_t)slot, 0u, 0x0eu);
                uint32_t function_count =
                    (header_type & 0x80u) != 0u ? 8u : 1u;
                for (uint32_t function = 0u; function < function_count;
                    ++function) {
                    identity = pci_read32((uint8_t)bus, (uint8_t)slot,
                        (uint8_t)function, 0u);
                    if ((uint16_t)identity == 0xffffu
                        || (uint16_t)identity == 0u) {
                        continue;
                    }
                    uint32_t class_revision = pci_read32((uint8_t)bus,
                        (uint8_t)slot, (uint8_t)function, 8u);
                    if ((uint8_t)(class_revision >> 24u) != 0x0cu
                        || (uint8_t)(class_revision >> 16u) != 0x03u) {
                        continue;
                    }
                    uint32_t bar = pci_read32((uint8_t)bus, (uint8_t)slot,
                        (uint8_t)function, 0x20u);
                    discover_controller((uint8_t)bus, (uint8_t)slot,
                        (uint8_t)function, class_revision,
                        bar != UINT32_MAX, (bar & 1u) != 0u,
                        (uint64_t)(bar & ~3u), pending_ehci,
                        &pending_ehci_count);
                }
            }
        }
    }
    if (controller_count == 0u && detected_ohci != 0u) {
        /* OHCI-only chipsets (AMD SB7xx): EHCI must release every port to
         * its OHCI companions, including low-speed devices. */
        diag_legsup_all("before-ehci-handoff");
        for (uint32_t i = 0u; i < pending_ehci_count; ++i) {
            ehci_legacy_handoff(pending_ehci[i][0], pending_ehci[i][1],
                pending_ehci[i][2]);
        }
        diag_legsup_all("after-ehci-handoff");
        ehci_release_all_to_companions();
        return;
    }
    if (controller_count == 0u) {
        diagnostics.ehci_legacy_handoff_skipped = pending_ehci_count;
    } else {
        diag_legsup_all("before-ehci-handoff");
        for (uint32_t i = 0u; i < pending_ehci_count; ++i) {
            ehci_legacy_handoff(pending_ehci[i][0], pending_ehci[i][1],
                pending_ehci[i][2]);
        }
        diag_legsup_all("after-ehci-handoff");
    }
    for (uint32_t ehci = 0u; ehci < ehci_controller_count; ++ehci) {
        struct ehci_controller *controller = &ehci_controllers[ehci];
        uint32_t mapping_index = 0u;
        for (uint32_t pass = 0u; pass < 2u
            && mapping_index < EHCI_PORT_ROUTE_COUNT; ++pass) {
            for (uint32_t companion = 0u;
                companion < controller_count
                    && mapping_index < EHCI_PORT_ROUTE_COUNT;
                ++companion) {
                struct uhci_controller *candidate = &controllers[companion];
                if ((pass == 0u
                        && (candidate->bus != controller->bus
                            || candidate->slot != controller->slot))
                    || (pass == 1u
                        && candidate->bus == controller->bus
                        && candidate->slot == controller->slot)) {
                    continue;
                }
                controller->companion_map[mapping_index++] =
                    (uint8_t)companion;
            }
        }
        controller->companion_mapping_count = (uint8_t)mapping_index;
        controller->route_fallback = controller->port_route == 0u
            || controller->companion_count == 0u
            || mapping_index == 0u;
        if (controller->route_fallback && mapping_index != 0u) {
            ++diagnostics.ehci_route_fallbacks;
        }
        diagnostics.ehci_companion_mappings += mapping_index;
    }
    ehci_route_companion_ports();
}

static uint32_t td_token(uint8_t pid, uint8_t address, uint8_t endpoint,
    uint8_t toggle, uint16_t length)
{
    uint32_t encoded_length = length == 0u ? 0x7ffu : (uint32_t)length - 1u;
    return pid | ((uint32_t)(address & 0x7fu) << 8u)
        | ((uint32_t)(endpoint & 0x0fu) << 15u)
        | ((uint32_t)(toggle & 1u) << 19u)
        | ((encoded_length & 0x7ffu) << 21u);
}

static uint32_t td_status(bool low_speed)
{
    uint32_t status = UHCI_STATUS_ACTIVE | UHCI_STATUS_ERROR_COUNT
        | UHCI_STATUS_ACTUAL_LENGTH_MASK
        | (low_speed ? UHCI_STATUS_LOW_SPEED : 0u);
    return status & ~(UHCI_STATUS_ERRORS | UHCI_STATUS_ISOCHRONOUS);
}

static uint32_t uhci_link_pointer(const void *address, uint32_t attributes)
{
    return (usb_dma_address32(address, 1u) & UHCI_LINK_POINTER_MASK)
        | (attributes & UHCI_LINK_ATTRIBUTE_MASK);
}

static bool execute_control_transfer(struct uhci_controller *controller,
    bool low_speed,
    uint8_t address, uint8_t endpoint_size,
    const struct usb_setup_packet *setup, uint8_t *data,
    uint16_t data_capacity, uint16_t *actual_length)
{
    if (setup->length > data_capacity
        || setup->length > USB_DESCRIPTOR_BUFFER
        || endpoint_size == 0u || endpoint_size > USB_MAX_PACKET
        || (setup->length != 0u && data == NULL)) {
        return false;
    }
    uint32_t data_td_count = (setup->length + endpoint_size - 1u)
        / endpoint_size;
    uint32_t td_count = data_td_count + 2u;
    if (td_count > UHCI_MAX_TDS) {
        return false;
    }
    if (!stop_uhci_controller(controller->io_base)) {
        return false;
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        setup_buffer[i] = ((const uint8_t *)setup)[i];
    }
    control_queue.horizontal = UHCI_LINK_TERMINATE;
    control_queue.element = UHCI_LINK_TERMINATE;
    for (uint32_t i = 0u; i < td_count; ++i) {
        control_transfer_descriptors[i].link = i + 1u < td_count
            ? uhci_link_pointer(&control_transfer_descriptors[i + 1u], 0u)
            : UHCI_LINK_TERMINATE;
        control_transfer_descriptors[i].status = td_status(low_speed);
    }
    control_transfer_descriptors[0].token =
        td_token(USB_PID_SETUP, address, 0u, 0u, 8u);
    control_transfer_descriptors[0].buffer =
        usb_dma_address32(setup_buffer, sizeof(setup_buffer));

    uint16_t total_actual = 0u;
    uint8_t toggle = 1u;
    for (uint32_t i = 0u; i < data_td_count; ++i) {
        uint16_t offset = (uint16_t)(i * endpoint_size);
        uint16_t remaining = (uint16_t)(setup->length - offset);
        uint16_t amount = remaining > endpoint_size ? endpoint_size : remaining;
        uint8_t pid = (setup->request_type & 0x80u) != 0u
            ? USB_PID_IN : USB_PID_OUT;
        control_transfer_descriptors[i + 1u].token =
            td_token(pid, address, 0u, toggle, amount);
        control_transfer_descriptors[i + 1u].buffer =
            usb_dma_address32(descriptor_buffer + offset, amount);
        if (pid == USB_PID_IN) {
            control_transfer_descriptors[i + 1u].status |=
                UHCI_STATUS_SHORT_PACKET_DETECT;
        }
        if ((setup->request_type & 0x80u) == 0u) {
            for (uint16_t byte = 0u; byte < amount; ++byte) {
                descriptor_buffer[offset + byte] = data[offset + byte];
            }
        }
        toggle ^= 1u;
    }
    uint32_t status_index = data_td_count + 1u;
    uint8_t status_pid = (setup->request_type & 0x80u) != 0u
        ? USB_PID_OUT : USB_PID_IN;
    control_transfer_descriptors[status_index].token =
        td_token(status_pid, address, 0u, 1u, 0u);
    control_transfer_descriptors[status_index].buffer = 0u;

    uint32_t controller_index = (uint32_t)(controller - controllers);
    for (uint32_t i = 0u; i < td_count; ++i) {
        log_uhci_descriptor("control-submit", controller_index, "TD", i,
            (const volatile uint32_t *)&control_transfer_descriptors[i], 4u);
    }
    control_queue.element =
        uhci_link_pointer(&control_transfer_descriptors[0], 0u);
    log_uhci_descriptor("control-submit", controller_index, "QH", 0u,
        (const volatile uint32_t *)&control_queue, 2u);
    for (uint32_t i = 0u; i < UHCI_FRAME_COUNT; ++i) {
        frame_list[controller_index][i] =
            uhci_link_pointer(&control_queue, UHCI_LINK_QH);
    }
    log_uhci_frame_list("control-submit", controller_index,
        frame_list[controller_index]);
    __asm__ volatile ("" : : : "memory");
    out16(controller->io_base + 0x06u, 0u);
    out16(controller->io_base + 0x00u, UHCI_COMMAND_CONFIGURED
        | UHCI_COMMAND_MAX_PACKET | UHCI_COMMAND_RUN);
    for (uint32_t i = 0u; i < td_count; ++i) {
        if (!wait_td(controller->io_base,
            &control_transfer_descriptors[i])) {
            out16(controller->io_base + 0x00u, 0u);
            return false;
        }
        uint32_t status = control_transfer_descriptors[i].status;
        uint32_t actual = td_actual_length(status);
        if (i > 0u && i <= data_td_count) {
            uint32_t remaining = (uint32_t)setup->length - total_actual;
            uint32_t amount = remaining > endpoint_size
                ? endpoint_size : remaining;
            if (actual > amount) {
                out16(controller->io_base + 0x00u, 0u);
                return false;
            }
            total_actual = (uint16_t)(total_actual + actual);
            if ((setup->request_type & 0x80u) != 0u
                && actual < amount) {
                control_queue.element =
                    uhci_link_pointer(
                        &control_transfer_descriptors[status_index], 0u);
                i = status_index - 1u;
            }
        }
    }
    out16(controller->io_base + 0x00u, 0u);
    if (setup->length != 0u && (setup->request_type & 0x80u) != 0u) {
        for (uint16_t i = 0u; i < total_actual; ++i) {
            data[i] = descriptor_buffer[i];
        }
    }
    if (actual_length != NULL) {
        *actual_length = total_actual;
    }
    return true;
}

static bool execute_control(struct uhci_controller *controller, bool low_speed,
    uint8_t address, uint8_t endpoint_size,
    const struct usb_setup_packet *setup, uint8_t *data,
    uint16_t data_capacity, uint16_t *actual_length)
{
    return execute_control_transfer(controller, low_speed, address,
        endpoint_size, setup, data, data_capacity, actual_length);
}

static bool control_request(struct uhci_controller *controller,
    bool low_speed, uint8_t address, uint8_t endpoint_size,
    uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
    uint8_t *data, uint16_t length, uint16_t *actual)
{
    struct usb_setup_packet setup = {
        request_type, request, value, index, length
    };
    for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
        if (execute_control(controller, low_speed, address,
            endpoint_size, &setup, data, length, actual)) {
            return true;
        }
        if (request != 6u || !wait_frames(controller->io_base, 10u)) {
            break;
        }
    }
    return false;
}

static bool reset_port(struct uhci_controller *controller, uint8_t port_index,
    bool *low_speed)
{
    uint32_t controller_index = (uint32_t)(controller - controllers);
    struct book8_usb_uhci_diagnostic *detail =
        &diagnostics.uhci[controller_index];
    uint16_t port = controller->io_base + 0x10u + 2u * port_index;
    if (!wait_frames(controller->io_base, 100u)) {
        return false;
    }
    uint16_t status = in16(port);
    detail->port_status[port_index] = status;
    if ((status & UHCI_PORT_CONNECTED) == 0u) {
        return false;
    }
    ++diagnostics.connected_ports;
    *low_speed = (status & UHCI_PORT_LOW_SPEED) != 0u;
    out16(port, (uint16_t)((status & UHCI_PORT_ENABLED) | UHCI_PORT_RESET));
    if (!wait_frames(controller->io_base, 50u)) {
        return false;
    }
    status = in16(port);
    detail->port_status[port_index] = status;
    out16(port, (uint16_t)(status & UHCI_PORT_ENABLED));
    if (!wait_frames(controller->io_base, 10u)) {
        return false;
    }
    status = in16(port);
    detail->port_status[port_index] = status;
    if ((status & UHCI_PORT_CONNECTED) == 0u) {
        return false;
    }
    if ((status & UHCI_PORT_ENABLED) == 0u) {
        out16(port, (uint16_t)((status & UHCI_PORT_CONTROL_MASK)
            | UHCI_PORT_ENABLED));
        if (!wait_frames(controller->io_base, 1u)) {
            return false;
        }
        status = in16(port);
        detail->port_status[port_index] = status;
    }
    if ((status & UHCI_PORT_ENABLED) == 0u) {
        return false;
    }
    ++diagnostics.reset_ports;
    return true;
}

void book8_usb_register_bus_drivers(const struct book8_bus_driver *drivers,
    size_t count)
{
    bus_drivers = drivers;
    bus_driver_count = count;
    diagnostics.registered_bus_drivers = book8_usb_registered_driver_count();
}

uint32_t book8_usb_registered_driver_count(void)
{
    uint32_t count = 0u;
    for (size_t i = 0u; i < bus_driver_count; ++i) {
        for (size_t m = 0u; m < bus_drivers[i].match_count; ++m) {
            if (bus_drivers[i].matches[m].bus_type == BOOK8_BUS_USB) {
                ++count;
                break;
            }
        }
    }
    return count;
}

uint32_t book8_usb_bound_device_count(void)
{
    return bus_binding_count;
}

static struct book8_bus_device *record_usb_device(
    const struct book8_bus_device *device)
{
    for (uint32_t i = 0u; i < usb_inventory_count; ++i) {
        struct book8_bus_device *existing = &usb_inventory[i];
        if (existing->address == device->address
            && existing->interface_number == device->interface_number
            && existing->vendor_id == device->vendor_id
            && existing->device_id == device->device_id
            && existing->class_code == device->class_code) {
            return existing;
        }
    }
    if (usb_inventory_count == USB_MAX_REGISTRY_DEVICES) {
        return NULL;
    }
    usb_inventory[usb_inventory_count] = *device;
    return &usb_inventory[usb_inventory_count++];
}

bool book8_usb_registry_sync(void)
{
    static const char root[] = "HKLM\\HARDWARE\\DEVICEMAP\\USB";

    (void)reg_delete_tree(root);
    for (uint32_t i = 0u; i < usb_inventory_count; ++i) {
        const struct book8_bus_device *device = &usb_inventory[i];
        char path[BOOK8_REGISTRY_KEY_MAX];
        char value[BOOK8_REGISTRY_VALUE_MAX];
        int length = snprintf(path, sizeof(path), "%s\\DEVICE_%03u", root, i);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            return false;
        }
        length = snprintf(value, sizeof(value), "0x%04x", device->vendor_id);
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "VendorID", value)) {
            return false;
        }
        length = snprintf(value, sizeof(value), "0x%04x", device->device_id);
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "DeviceID", value)) {
            return false;
        }
        length = snprintf(value, sizeof(value), "0x%06x", device->class_code);
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "ClassCode", value)
            || !reg_set_u32(path, "Subclass",
                (device->class_code >> 8u) & 0xffu)
            || !reg_set_str(path, "IRQ", "N/A")
            || !reg_set_str(path, "Driver",
                device->driver != NULL ? device->driver->name : "unbound")) {
            return false;
        }
        length = snprintf(value, sizeof(value), "%u", device->address);
        if (length < 0 || (size_t)length >= sizeof(value)
            || !reg_set_str(path, "Address", value)
            || !reg_set_u32(path, "Interface", device->interface_number)) {
            return false;
        }
        for (uint8_t bar = 0u; bar < BOOK8_PCIE_BAR_COUNT; ++bar) {
            const char *bar_name = bar == 0u ? "BAR0" :
                bar == 1u ? "BAR1" : bar == 2u ? "BAR2" :
                bar == 3u ? "BAR3" : bar == 4u ? "BAR4" : "BAR5";
            if (!reg_set_str(path, bar_name, "N/A")) {
                return false;
            }
        }
    }
    return true;
}

static void usb_remove_bindings(void)
{
    while (bus_binding_count != 0u) {
        struct book8_bus_device *device =
            &bus_bindings[--bus_binding_count];
        if (device->driver != NULL && device->driver->remove != NULL) {
            device->driver->remove(device);
        }
        *device = (struct book8_bus_device){0};
    }
}

const struct book8_bus_driver *book8_usb_bind_interface(uint16_t vendor_id,
    uint16_t device_id, uint8_t class_code, uint8_t subclass, uint8_t protocol,
    uint8_t address, uint8_t interface_number)
{
    struct book8_bus_device candidate = {
        .bus_type = BOOK8_BUS_USB,
        .vendor_id = vendor_id,
        .device_id = device_id,
        .class_code = ((uint32_t)class_code << 16u)
            | ((uint32_t)subclass << 8u) | protocol,
        .address = address,
        .interface_number = interface_number
    };
    struct book8_bus_device *inventory_device =
        record_usb_device(&candidate);

    for (uint32_t i = 0u; i < bus_binding_count; ++i) {
        struct book8_bus_device *existing = &bus_bindings[i];
        if (existing->address == address
            && existing->interface_number == interface_number
            && existing->vendor_id == vendor_id
            && existing->device_id == device_id
            && existing->class_code == candidate.class_code) {
            if (inventory_device != NULL) {
                inventory_device->driver = existing->driver;
            }
            return existing->driver;
        }
    }
    if (bus_binding_count >= USB_MAX_BUS_BINDINGS) {
        return NULL;
    }
    for (size_t i = 0u; i < bus_driver_count; ++i) {
        const struct book8_bus_driver *driver = &bus_drivers[i];
        bool matches = false;
        if (driver->name == NULL || driver->matches == NULL
            || driver->probe == NULL) {
            continue;
        }
        for (size_t m = 0u; m < driver->match_count; ++m) {
            if (book8_bus_match_device(&driver->matches[m], &candidate)) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        struct book8_bus_device *binding =
            &bus_bindings[bus_binding_count];
        *binding = candidate;
        if (driver->probe(binding)) {
            binding->driver = driver;
            if (inventory_device != NULL) {
                inventory_device->driver = driver;
            }
            ++bus_binding_count;
            diagnostics.bound_bus_devices = bus_binding_count;
            return driver;
        }
        *binding = (struct book8_bus_device){0};
    }
    return NULL;
}

void book8_usb_bind_configuration_interfaces(uint16_t vendor_id,
    uint16_t device_id, const uint8_t *configuration, uint16_t length,
    uint8_t address)
{
    if (configuration == NULL || length < 9u) {
        return;
    }
    for (uint32_t offset = 0u; offset + 2u <= length;) {
        uint8_t descriptor_length = configuration[offset];
        uint8_t descriptor_type = configuration[offset + 1u];
        if (descriptor_length < 2u || descriptor_length > length - offset) {
            return;
        }
        if (descriptor_type == 4u && descriptor_length >= 9u
            && configuration[offset + 3u] == 0u) {
            (void)book8_usb_bind_interface(vendor_id, device_id,
                configuration[offset + 5u], configuration[offset + 6u],
                configuration[offset + 7u], address,
                configuration[offset + 2u]);
        }
        offset += descriptor_length;
    }
}

static bool enumerate_hid(struct uhci_controller *controller,
    uint8_t port_index, bool low_speed, uint8_t address,
    bool *address_assigned)
{
    uint16_t vendor_id, product_id;
    *address_assigned = false;
    uint16_t actual = 0u;
    uint16_t initial_descriptor_length = low_speed ? 8u : 64u;
    if (!control_request(controller, low_speed, 0u, 8u, 0x80u, 6u,
        0x0100u, 0u, descriptor_buffer, initial_descriptor_length, &actual)
        || actual < 8u) {
        ++diagnostics.device_descriptor_failures;
        return false;
    }
    uint8_t endpoint_size = descriptor_buffer[7u];
    if (endpoint_size != 8u && endpoint_size != 16u
        && endpoint_size != 32u && endpoint_size != 64u) {
        return false;
    }
    bool reset_low_speed = false;
    if (!reset_port(controller, port_index, &reset_low_speed)
        || reset_low_speed != low_speed) {
        return false;
    }
    if (!control_request(controller, low_speed, 0u, endpoint_size, 0u,
        5u, address, 0u, NULL, 0u, NULL)) {
        return false;
    }
    *address_assigned = true;
    ++diagnostics.addressed_devices;
    if (!wait_frames(controller->io_base, 2u)) {
        return false;
    }
    if (!control_request(controller, low_speed, address,
        endpoint_size, 0x80u, 6u, 0x0100u, 0u,
        descriptor_buffer, 18u, &actual) || actual < 18u
        || descriptor_buffer[1u] != 1u
        || descriptor_buffer[17u] == 0u) {
        ++diagnostics.device_descriptor_failures;
        return false;
    }
    vendor_id = (uint16_t)descriptor_buffer[8u]
        | ((uint16_t)descriptor_buffer[9u] << 8u);
    product_id = (uint16_t)descriptor_buffer[10u]
        | ((uint16_t)descriptor_buffer[11u] << 8u);
    if (descriptor_buffer[4u] != 0u) {
        (void)book8_usb_bind_interface(vendor_id, product_id,
            descriptor_buffer[4u], descriptor_buffer[5u],
            descriptor_buffer[6u], address, 0xffu);
    }
    uint16_t configuration_length = 9u;
    if (!control_request(controller, low_speed, address,
        endpoint_size, 0x80u, 6u, 0x0200u, 0u,
        descriptor_buffer, configuration_length, &actual) || actual < 9u
        || descriptor_buffer[1u] != 2u) {
        ++diagnostics.configuration_descriptor_failures;
        return false;
    }
    configuration_length = (uint16_t)descriptor_buffer[2u]
        | ((uint16_t)descriptor_buffer[3u] << 8u);
    if (configuration_length < 9u
        || configuration_length > USB_DESCRIPTOR_BUFFER
        || !control_request(controller, low_speed, address,
            endpoint_size, 0x80u, 6u, 0x0200u, 0u, descriptor_buffer,
            configuration_length, &actual)
        || actual < configuration_length) {
        ++diagnostics.configuration_descriptor_failures;
        return false;
    }
    book8_usb_bind_configuration_interfaces(vendor_id, product_id,
        descriptor_buffer, configuration_length, address);

    uint8_t configuration_value = 0u;
    struct usb_hid_candidate candidates[USB_MAX_HID_DEVICES] = {0};
    uint32_t candidate_count = 0u;
    uint32_t current_candidate = USB_MAX_HID_DEVICES;
    for (uint32_t offset = 0u; offset + 2u <= configuration_length;) {
        uint8_t length = descriptor_buffer[offset];
        uint8_t type = descriptor_buffer[offset + 1u];
        if (length < 2u || length > configuration_length - offset) {
            ++diagnostics.configuration_descriptor_failures;
            return false;
        }
        if (type == 2u && length >= 9u) {
            configuration_value = descriptor_buffer[offset + 5u];
        } else if (type == 4u && length >= 9u) {
            current_candidate = USB_MAX_HID_DEVICES;
            if (descriptor_buffer[offset + 3u] == 0u
                && candidate_count < USB_MAX_HID_DEVICES) {
                current_candidate = candidate_count++;
                candidates[current_candidate].interface_number =
                    descriptor_buffer[offset + 2u];
                candidates[current_candidate].class_code =
                    descriptor_buffer[offset + 5u];
                candidates[current_candidate].subclass =
                    descriptor_buffer[offset + 6u];
                candidates[current_candidate].protocol =
                    descriptor_buffer[offset + 7u];
            }
        } else if (type == 0x21u && length >= 9u
            && current_candidate < candidate_count) {
            uint8_t descriptor_count = descriptor_buffer[offset + 5u];
            uint32_t descriptor_offset = offset + 6u;
            for (uint8_t item = 0u; item < descriptor_count
                && descriptor_offset + 3u <= offset + length; ++item) {
                uint8_t descriptor_type = descriptor_buffer[descriptor_offset];
                uint16_t descriptor_length =
                    (uint16_t)descriptor_buffer[descriptor_offset + 1u]
                    | ((uint16_t)descriptor_buffer[descriptor_offset + 2u]
                        << 8u);
                if (descriptor_type == 0x22u) {
                    candidates[current_candidate].report_descriptor_length =
                        descriptor_length;
                    break;
                }
                descriptor_offset += 3u;
            }
        } else if (type == 5u && length >= 7u
            && current_candidate < candidate_count) {
            uint16_t packet_size = (uint16_t)descriptor_buffer[offset + 4u]
                | ((uint16_t)descriptor_buffer[offset + 5u] << 8u);
            packet_size &= 0x07ffu;
            if ((descriptor_buffer[offset + 2u] & 0x80u) != 0u
                && (descriptor_buffer[offset + 3u] & 3u) == 3u
                && packet_size >= 3u && packet_size <= USB_MAX_PACKET
                && candidates[current_candidate].endpoint == 0u) {
                candidates[current_candidate].endpoint =
                    descriptor_buffer[offset + 2u] & 0x0fu;
                candidates[current_candidate].max_packet = packet_size;
                candidates[current_candidate].interval =
                    descriptor_buffer[offset + 6u];
            }
        }
        offset += length;
    }

    if (configuration_value == 0u || !control_request(controller, low_speed, address,
        endpoint_size, 0u, 9u, configuration_value, 0u,
        NULL, 0u, NULL)
        || !wait_frames(controller->io_base, 10u)) {
        ++diagnostics.hid_control_failures;
        return false;
    }
    uint32_t devices_added = 0u;
    for (uint32_t i = 0u; i < candidate_count; ++i) {
        struct usb_hid_candidate *candidate = &candidates[i];
        candidate->driver = book8_usb_bind_interface(vendor_id, product_id,
            candidate->class_code, candidate->subclass, candidate->protocol,
            address, candidate->interface_number);
        if (candidate->driver == NULL || candidate->class_code != 3u) {
            continue;
        }
        bool boot_keyboard = candidate->subclass == 1u
            && candidate->protocol == 1u;
        bool boot_mouse = candidate->subclass == 1u
            && candidate->protocol == 2u;
        if (boot_keyboard || boot_mouse) {
            ++diagnostics.boot_hid_interfaces;
        }

        bool boot_mouse_report_protocol = false;
        bool boot_mouse_boot_protocol = false;
        if (boot_keyboard && !control_request(controller, low_speed, address,
                endpoint_size, 0x21u, 0x0bu, 0u,
                candidate->interface_number, NULL, 0u, NULL)) {
            ++diagnostics.hid_control_failures;
            continue;
        }
        if (boot_mouse) {
            boot_mouse_report_protocol = control_request(controller,
                low_speed, address, endpoint_size, 0x21u, 0x0bu, 1u,
                candidate->interface_number, NULL, 0u, NULL);
            if (!boot_mouse_report_protocol) {
                boot_mouse_boot_protocol = control_request(controller,
                    low_speed, address, endpoint_size, 0x21u, 0x0bu, 0u,
                    candidate->interface_number, NULL, 0u, NULL);
            }
        }
        (void)control_request(controller, low_speed, address,
            endpoint_size, 0x21u, 0x0au, 0u,
            candidate->interface_number, NULL, 0u, NULL);

        if (candidate->endpoint == 0u || candidate->interval == 0u) {
            continue;
        }
        enum usb_hid_kind kind = USB_HID_MOUSE;
        uint8_t report_id = 0u;
        uint16_t report_size = 0u;
        uint16_t x_bit_offset = 8u;
        uint16_t y_bit_offset = 16u;
        uint16_t wheel_bit_offset = 0u;
        uint16_t buttons_bit_offset = 0u;
        uint8_t x_bit_size = 8u;
        uint8_t y_bit_size = 8u;
        uint8_t wheel_bit_size = 0u;
        uint8_t button_count = 3u;
        int32_t x_logical_min = -127;
        int32_t x_logical_max = 127;
        int32_t y_logical_min = -127;
        int32_t y_logical_max = 127;
        int32_t wheel_logical_min = 0;
        int32_t wheel_logical_max = 0;
        bool x_relative = true;
        bool y_relative = true;
        if (boot_keyboard) {
            if (candidate->max_packet < 8u) {
                ++diagnostics.non_boot_hid_devices;
                continue;
            }
            kind = USB_HID_KEYBOARD;
            report_size = 8u;
        } else if (boot_mouse && boot_mouse_boot_protocol) {
            report_size = 3u;
        } else if (candidate->protocol != 1u) {
            uint16_t actual_report_length = 0u;
            bool report_descriptor_read =
                candidate->report_descriptor_length != 0u
                && candidate->report_descriptor_length
                    <= USB_DESCRIPTOR_BUFFER
                && control_request(controller, low_speed, address,
                    endpoint_size, 0x81u, 6u, 0x2200u,
                    candidate->interface_number, descriptor_buffer,
                    candidate->report_descriptor_length,
                    &actual_report_length)
                && actual_report_length
                    >= candidate->report_descriptor_length;
            struct hid_mouse_layout layout = {0};
            if (report_descriptor_read) {
                layout = parse_mouse_report_descriptor(descriptor_buffer,
                    actual_report_length);
            }
            if (!layout.valid || layout.report_size > candidate->max_packet) {
                if (candidate->max_packet < 3u) {
                    ++diagnostics.non_boot_hid_devices;
                    continue;
                }
                report_id = 0u;
                report_size = 3u;
                x_bit_offset = 8u;
                y_bit_offset = 16u;
                buttons_bit_offset = 0u;
                button_count = 3u;
                ++diagnostics.non_boot_hid_devices;
            } else {
                report_id = layout.report_id;
                report_size = layout.report_size;
                x_bit_offset = layout.x_bit_offset;
                y_bit_offset = layout.y_bit_offset;
                wheel_bit_offset = layout.wheel_bit_offset;
                buttons_bit_offset = layout.buttons_bit_offset;
                x_bit_size = layout.x_bit_size;
                y_bit_size = layout.y_bit_size;
                wheel_bit_size = layout.wheel_bit_size;
                button_count = layout.button_count;
                x_logical_min = layout.x_logical_min;
                x_logical_max = layout.x_logical_max;
                y_logical_min = layout.y_logical_min;
                y_logical_max = layout.y_logical_max;
                wheel_logical_min = layout.wheel_logical_min;
                wheel_logical_max = layout.wheel_logical_max;
                x_relative = layout.x_relative;
                y_relative = layout.y_relative;
            }
        } else {
            ++diagnostics.non_boot_hid_devices;
            continue;
        }
        if (candidate->max_packet < report_size
            || hid_device_count >= USB_MAX_HID_DEVICES) {
            continue;
        }
        hid_devices[hid_device_count++] = (struct usb_hid_device){
            .active = true,
            .low_speed = low_speed,
            .kind = kind,
            .address = address,
            .endpoint = candidate->endpoint,
            .max_packet = (uint8_t)candidate->max_packet,
            .data_toggle = 0u,
            .controller_index = (uint16_t)(controller - controllers),
            .td_pending = false,
            .report_id = report_id,
            .report_size = (uint8_t)report_size,
            .mouse_x_bit_offset = x_bit_offset,
            .mouse_y_bit_offset = y_bit_offset,
            .mouse_wheel_bit_offset = wheel_bit_offset,
            .mouse_buttons_bit_offset = buttons_bit_offset,
            .mouse_x_bit_size = x_bit_size,
            .mouse_y_bit_size = y_bit_size,
            .mouse_wheel_bit_size = wheel_bit_size,
            .mouse_button_count = button_count,
            .mouse_x_logical_min = x_logical_min,
            .mouse_x_logical_max = x_logical_max,
            .mouse_y_logical_min = y_logical_min,
            .mouse_y_logical_max = y_logical_max,
            .mouse_wheel_logical_min = wheel_logical_min,
            .mouse_wheel_logical_max = wheel_logical_max,
            .mouse_x_relative = x_relative,
            .mouse_y_relative = y_relative,
            .driver = candidate->driver
        };
        ++devices_added;
    }
    return devices_added != 0u;
}

static bool start_interrupt_schedule(uint32_t controller_index)
{
    if (controller_index >= controller_count) {
        return false;
    }
    uint32_t first_device = hid_device_count;
    for (uint32_t device = 0u; device < hid_device_count; ++device) {
        if (hid_devices[device].controller_index == controller_index) {
            first_device = device;
            break;
        }
    }
    if (first_device == hid_device_count
        || uhci_initialized[controller_index] == 0u) {
        return false;
    }

    uint16_t io_base = controllers[controller_index].io_base;
    if (!stop_uhci_controller(io_base)) {
        uhci_schedule_active[controller_index] = false;
        ++diagnostics.uhci_schedule_failures;
        return false;
    }
    for (uint32_t device = first_device; device < hid_device_count; ++device) {
        if (hid_devices[device].controller_index == controller_index) {
            log_uhci_descriptor("periodic-submit", controller_index, "QH",
                device, (const volatile uint32_t *)&interrupt_queues[device],
                2u);
        }
    }
    for (uint32_t frame = 0u; frame < UHCI_FRAME_COUNT; ++frame) {
        frame_list[controller_index][frame] =
            uhci_link_pointer(&interrupt_queues[first_device],
                UHCI_LINK_QH);
    }
    log_uhci_frame_list("periodic-submit", controller_index,
        frame_list[controller_index]);
    __asm__ volatile ("" : : : "memory");
    out16(io_base + 0x06u, 0u);
    out32(io_base + 0x08u,
        usb_dma_address32(frame_list[controller_index],
            sizeof(frame_list[controller_index])));
    out8(io_base + 0x0cu, 64u);
    out16(io_base + 0x00u, UHCI_COMMAND_CONFIGURED
        | UHCI_COMMAND_MAX_PACKET | UHCI_COMMAND_RUN);
    uint16_t command = in16(io_base + 0x00u);
    struct book8_usb_uhci_diagnostic *detail =
        &diagnostics.uhci[controller_index];
    detail->controller_status = in16(io_base + 0x02u);
    detail->frame_number = in16(io_base + 0x06u);
    if ((command & UHCI_COMMAND_RUN) != 0u
        && in32(io_base + 0x08u)
            == usb_dma_address32(frame_list[controller_index],
                sizeof(frame_list[controller_index]))) {
        if (!uhci_schedule_active[controller_index]) {
            ++diagnostics.uhci_running_controllers;
        }
        uhci_schedule_active[controller_index] = true;
        return true;
    }
    uhci_schedule_active[controller_index] = false;
    ++diagnostics.uhci_schedule_failures;
    return false;
}

static bool initialize_controller(struct uhci_controller *controller)
{
    uint32_t controller_index = (uint32_t)(controller - controllers);
    uhci_schedule_active[controller_index] = false;
    if (!pci_enable_command(controller->bus, controller->slot,
        controller->function,
        PCI_COMMAND_IO_SPACE | PCI_COMMAND_BUS_MASTER)) {
        ++diagnostics.pci_enable_failures;
        return false;
    }
    uhci_initialized[controller_index] = 0u;
    uint16_t io_base = controller->io_base;
    /* Release BIOS legacy emulation (PCI 0xC0) before touching the HC:
       clear the SMI enables and write-1-to-clear the status bits. */
    out16(io_base + 0x04u, 0u);
    diag_uhci_regs("pre-reset", controller_index);
    if (!BOOK8_DIAG_SKIP_LEGSUP) {
        pci_write16(controller->bus, controller->slot, controller->function,
            UHCI_PCI_LEGACY_SUPPORT, UHCI_LEGSUP_CLEAR);
    }
    diag_legsup_all("after-clear");
    if (!stop_uhci_controller(io_base)) {
        return false;
    }
    out16(io_base + 0x00u, UHCI_COMMAND_RESET);
    bool reset_complete = false;
    for (uint32_t attempt = 0u; attempt < UHCI_STOP_WAIT_LIMIT; ++attempt) {
        if ((in16(io_base + 0x00u) & UHCI_COMMAND_RESET) == 0u) {
            reset_complete = true;
            break;
        }
        io_wait();
    }
    if (!reset_complete) {
        return false;
    }
    out16(io_base + 0x00u, 0u);
    diag_uhci_regs("post-reset", controller_index);
    if (!BOOK8_DIAG_SKIP_LEGSUP) {
        pci_write16(controller->bus, controller->slot, controller->function,
            UHCI_PCI_LEGACY_SUPPORT, UHCI_LEGSUP_PIRQ_ENABLE);
    }
    diag_legsup_all("after-pirq");
    if (!stop_uhci_controller(io_base)) {
        return false;
    }
    out16(io_base + 0x04u, 0u);
    out16(io_base + 0x06u, 0u);
    out8(io_base + 0x0cu, 64u);
    for (uint32_t i = 0u; i < UHCI_FRAME_COUNT; ++i) {
        frame_list[controller_index][i] = UHCI_LINK_TERMINATE;
    }
    control_queue.horizontal = UHCI_LINK_TERMINATE;
    control_queue.element = UHCI_LINK_TERMINATE;
    log_uhci_descriptor("controller-init", controller_index, "QH", 0u,
        (const volatile uint32_t *)&control_queue, 2u);
    log_uhci_frame_list("controller-init", controller_index,
        frame_list[controller_index]);
    __asm__ volatile ("" : : : "memory");
    out32(io_base + 0x08u,
        usb_dma_address32(frame_list[controller_index],
            sizeof(frame_list[controller_index])));
    out16(io_base + 0x00u, UHCI_COMMAND_CONFIGURED
        | UHCI_COMMAND_MAX_PACKET | UHCI_COMMAND_RUN);
    if ((in16(io_base + 0x00u) & UHCI_COMMAND_RUN) == 0u) {
        return false;
    }
    delay(200000u);
    uint32_t first_device = hid_device_count;
    uint8_t next_address = 1u;
    for (uint8_t port = 0u; port < controller->port_count; ++port) {
        bool low_speed = false;
        if (next_address > 127u || !reset_port(controller, port, &low_speed)) {
            continue;
        }
        bool address_assigned = false;
        (void)enumerate_hid(controller, port, low_speed, next_address,
            &address_assigned);
        if (address_assigned) {
            ++next_address;
        }
    }
    diag_uhci_regs("post-enum", controller_index);
    out16(io_base + 0x00u, 0u);
    uhci_initialized[controller_index] = 1u;
    return hid_device_count != first_device;
}

static bool pcie_controller_present(const struct book8_pcie_state *pcie_state,
    const struct uhci_controller *controller)
{
    for (size_t i = 0u; i < pcie_state->node_count; ++i) {
        const struct book8_pcie_node *node = &pcie_state->nodes[i];
        if (node->segment == 0u && node->bus == controller->bus
            && node->slot == controller->slot
            && node->function == controller->function
            && (node->class_revision & 0xffff00u) == 0x0c0300u) {
            return true;
        }
    }
    return false;
}

static void usb_initialize(const struct book8_pcie_state *pcie_state)
{
    usb_remove_bindings();
    usb_inventory_count = 0u;
    uhci_descriptor_dump_count = 0u;
    diagnostics = (struct book8_usb_diagnostics){0};
    diagnostics.registered_bus_drivers = book8_usb_registered_driver_count();
    for (uint32_t controller = 0u; controller < controller_count;
        ++controller) {
        if (pcie_state != NULL
            && !pcie_controller_present(pcie_state,
                &controllers[controller])) {
            continue;
        }
        if (!stop_uhci_controller(controllers[controller].io_base)) {
            ++diagnostics.uhci_schedule_failures;
            return;
        }
    }
    hid_device_count = 0u;
    for (uint32_t controller = 0u; controller < USB_MAX_UHCI;
        ++controller) {
        uhci_initialized[controller] = 0u;
        uhci_schedule_active[controller] = false;
        for (uint32_t frame = 0u; frame < UHCI_FRAME_COUNT; ++frame) {
            frame_list[controller][frame] = 0u;
        }
    }
    control_queue.horizontal = 0u;
    control_queue.element = 0u;
    for (uint32_t i = 0u; i < UHCI_MAX_TDS; ++i) {
        control_transfer_descriptors[i].link = 0u;
        control_transfer_descriptors[i].status = 0u;
        control_transfer_descriptors[i].token = 0u;
        control_transfer_descriptors[i].buffer = 0u;
        transfer_descriptors[i].link = 0u;
        transfer_descriptors[i].status = 0u;
        transfer_descriptors[i].token = 0u;
        transfer_descriptors[i].buffer = 0u;
    }
    for (uint32_t i = 0u; i < USB_MAX_HID_DEVICES; ++i) {
        hid_devices[i] = (struct usb_hid_device){0};
        interrupt_queues[i].horizontal = 0u;
        interrupt_queues[i].element = 0u;
    }
    control_queue.horizontal = UHCI_LINK_TERMINATE;
    control_queue.element = UHCI_LINK_TERMINATE;
    for (uint32_t i = 0u; i < UHCI_MAX_TDS; ++i) {
        control_transfer_descriptors[i].link = UHCI_LINK_TERMINATE;
        transfer_descriptors[i].link = UHCI_LINK_TERMINATE;
    }
    for (uint32_t i = 0u; i < USB_MAX_HID_DEVICES; ++i) {
        interrupt_queues[i].horizontal = UHCI_LINK_TERMINATE;
        interrupt_queues[i].element = UHCI_LINK_TERMINATE;
    }
    controller_count = 0u;
    book8_ohci_reset();
    discover_controllers(pcie_state);
    diagnostics.uhci_controllers = controller_count;
    diagnostics.uhci_diagnostic_count = controller_count;
    diagnostics.ohci_controllers = detected_ohci;
    diagnostics.ehci_controllers = detected_ehci;
    for (uint32_t i = 0u; i < controller_count; ++i) {
        struct uhci_controller *controller = &controllers[i];
        diagnostics.uhci[i] = (struct book8_usb_uhci_diagnostic){
            .bus = controller->bus,
            .slot = controller->slot,
            .function = controller->function,
            .io_base = controller->io_base,
            .pci_bar = pci_read32(controller->bus, controller->slot,
                controller->function, 0x20u),
            .pci_command = pci_read16(controller->bus, controller->slot,
                controller->function, 0x04u),
            .port_status = {0xffffu, 0xffffu}
        };
        (void)initialize_controller(&controllers[i]);
    }

    for (uint32_t controller = 0u; controller < controller_count;
        ++controller) {
        uint32_t first_device = hid_device_count;
        for (uint32_t device = 0u; device < hid_device_count; ++device) {
            if (hid_devices[device].controller_index == controller) {
                if (first_device == hid_device_count) {
                    first_device = device;
                }
                uint32_t next_device = device + 1u;
                while (next_device < hid_device_count
                    && hid_devices[next_device].controller_index != controller) {
                    ++next_device;
                }
                interrupt_queues[device].horizontal =
                    next_device < hid_device_count
                    ? uhci_link_pointer(&interrupt_queues[next_device],
                        UHCI_LINK_QH)
                    : UHCI_LINK_TERMINATE;
                interrupt_queues[device].element = UHCI_LINK_TERMINATE;
            }
        }
        if (first_device == hid_device_count
            || uhci_initialized[controller] == 0u) {
            continue;
        }
        if (!start_interrupt_schedule(controller)) {
            continue;
        }
    }
    diagnostics.active_hid_devices = hid_device_count;
    book8_ohci_start(&diagnostics);
    diagnostics.bound_bus_devices = bus_binding_count;
}

void book8_usb_initialize(void)
{
    usb_initialize(NULL);
}

void book8_usb_initialize_from_pci(const struct book8_pcie_state *pcie_state)
{
    usb_initialize(pcie_state);
}

void book8_usb_rescan(const struct book8_pcie_state *pcie_state)
{
    usb_initialize(pcie_state);
}

void book8_usb_get_diagnostics(struct book8_usb_diagnostics *result)
{
    if (result != NULL) {
        *result = diagnostics;
    }
}

bool book8_usb_mouse_active(void)
{
    if (book8_ohci_mouse_active()) {
        return true;
    }
    for (uint32_t i = 0u; i < hid_device_count; ++i) {
        if (hid_devices[i].active && hid_devices[i].kind == USB_HID_MOUSE) {
            return true;
        }
    }
    return false;
}

bool book8_usb_keyboard_active(void)
{
    if (book8_ohci_keyboard_active()) {
        return true;
    }
    for (uint32_t i = 0u; i < hid_device_count; ++i) {
        if (hid_devices[i].active
            && hid_devices[i].kind == USB_HID_KEYBOARD) {
            return true;
        }
    }
    return false;
}

uint32_t book8_usb_detected_controller_count(void)
{
    return controller_count + detected_ohci + detected_ehci + detected_xhci;
}

uint32_t book8_usb_uhci_controller_count(void)
{
    return controller_count;
}

uint32_t book8_usb_ohci_controller_count(void)
{
    return detected_ohci;
}

uint32_t book8_usb_ehci_controller_count(void)
{
    return detected_ehci;
}

uint32_t book8_usb_xhci_controller_count(void)
{
    return detected_xhci;
}

static bool hid_report_bits(const uint8_t *buffer, uint32_t actual_length,
    uint8_t report_id, uint16_t bit_offset, uint8_t bit_count,
    uint32_t *value)
{
    uint32_t report_offset = report_id != 0u ? 1u : 0u;
    if (report_id != 0u
        && (actual_length == 0u || buffer[0] != report_id)) {
        return false;
    }
    uint32_t result = 0u;
    for (uint8_t bit = 0u; bit < bit_count; ++bit) {
        uint32_t current_bit = (uint32_t)bit_offset + bit;
        uint32_t byte_offset = report_offset + current_bit / 8u;
        if (byte_offset >= actual_length) {
            return false;
        }
        uint8_t bit_value = (buffer[byte_offset] >> (current_bit & 7u)) & 1u;
        result |= (uint32_t)bit_value << bit;
    }
    *value = result;
    return true;
}

static int32_t hid_signed_field(uint32_t value, uint8_t bit_count)
{
    if (bit_count == 0u) {
        return 0;
    }
    if (bit_count >= 32u) {
        return (int32_t)value;
    }
    uint32_t sign = 1u << (bit_count - 1u);
    return (int32_t)((value ^ sign) - sign);
}

static void disable_hid_schedule(struct usb_hid_device *device,
    uint32_t device_index)
{
    if (device->active) {
        device->active = false;
        device->td_pending = false;
        interrupt_queues[device_index].element = UHCI_LINK_TERMINATE;
        ++diagnostics.uhci_interrupt_stalls;
    }
}

static bool hid_schedule_stalled(struct usb_hid_device *device,
    uint32_t device_index)
{
    struct uhci_controller *controller =
        &controllers[device->controller_index];
    uint16_t io_base = controller->io_base;
    uint16_t command = in16(io_base);
    uint32_t frame_list_base = in32(io_base + 0x08u);
    if ((command & UHCI_COMMAND_RUN) == 0u
        || frame_list_base
            != usb_dma_address32(frame_list[device->controller_index],
                sizeof(frame_list[device->controller_index]))) {
        disable_hid_schedule(device, device_index);
        return true;
    }
    uint16_t frame = in16(io_base + 0x06u)
        & UHCI_STATUS_FRAME_NUMBER_MASK;
    if (frame != device->pending_last_frame) {
        device->pending_last_frame = frame;
        device->stagnant_polls = 0u;
        return false;
    }
    if (++device->stagnant_polls < UHCI_HID_STALL_POLL_LIMIT) {
        return false;
    }

    disable_hid_schedule(device, device_index);
    return true;
}

static void queue_hid_interrupt(struct usb_hid_device *device,
    uint32_t device_index, struct uhci_transfer_descriptor *td,
    uint8_t *buffer)
{
    td->link = UHCI_LINK_TERMINATE;
    td->status = td_status(device->low_speed);
    td->token = td_token(USB_PID_IN, device->address,
        device->endpoint, device->data_toggle, device->max_packet);
    td->buffer = usb_dma_address32(buffer, USB_MAX_PACKET);
    log_uhci_descriptor("interrupt-submit", device->controller_index,
        "TD", device_index, (const volatile uint32_t *)td, 4u);
    interrupt_queues[device_index].element =
        uhci_link_pointer(td, 0u);
    device->pending_last_frame =
        in16(controllers[device->controller_index].io_base + 0x06u)
        & UHCI_STATUS_FRAME_NUMBER_MASK;
    device->stagnant_polls = 0u;
    device->td_pending = true;
}

bool book8_usb_poll_mouse(struct book8_usb_mouse_report *report)
{
    if (report != NULL && book8_ohci_poll_mouse(report)) {
        return true;
    }
    if (report == NULL || BOOK8_DIAG_NO_UHCI_POLL) {
        return false;
    }
    for (uint32_t i = 0u; i < hid_device_count; ++i) {
        struct usb_hid_device *device = &hid_devices[i];
        if (!device->active || device->kind != USB_HID_MOUSE
            || device->controller_index >= controller_count) {
            continue;
        }
        if (hid_schedule_stalled(device, i)) {
            continue;
        }
        struct uhci_transfer_descriptor *td = &transfer_descriptors[i];
        uint8_t *buffer = interrupt_buffers[i];
        if (device->td_pending) {
            uint32_t status = td->status;
            if ((status & UHCI_STATUS_ACTIVE) != 0u) {
                continue;
            }
            device->td_pending = false;
            if ((status & UHCI_STATUS_ERRORS) == 0u) {
                uint32_t actual = td_actual_length(status);
                device->data_toggle ^= 1u;
                uint32_t button_value = 0u;
                uint32_t x_value = 0u;
                uint32_t y_value = 0u;
                uint32_t wheel_value = 0u;
                if (actual >= device->report_size
                    && hid_report_bits(buffer, actual, device->report_id,
                        device->mouse_x_bit_offset,
                        device->mouse_x_bit_size, &x_value)
                    && hid_report_bits(buffer, actual, device->report_id,
                        device->mouse_y_bit_offset,
                        device->mouse_y_bit_size, &y_value)
                    && (device->mouse_button_count == 0u
                        || hid_report_bits(buffer, actual, device->report_id,
                            device->mouse_buttons_bit_offset,
                            device->mouse_button_count, &button_value))) {
                    if (device->mouse_wheel_bit_size == 0u
                        || hid_report_bits(buffer, actual,
                            device->report_id,
                            device->mouse_wheel_bit_offset,
                            device->mouse_wheel_bit_size, &wheel_value)) {
                        ++diagnostics.uhci_interrupt_completions;
                        report->buttons =
                            (uint8_t)(button_value & 0x07u);
                        report->absolute = !device->mouse_x_relative;
                        report->x = device->mouse_x_logical_min < 0
                            ? hid_signed_field(x_value,
                                device->mouse_x_bit_size)
                            : (int32_t)x_value;
                        report->y = device->mouse_y_logical_min < 0
                            ? hid_signed_field(y_value,
                                device->mouse_y_bit_size)
                            : (int32_t)y_value;
                        report->logical_min_x =
                            device->mouse_x_logical_min;
                        report->logical_max_x =
                            device->mouse_x_logical_max;
                        report->logical_min_y =
                            device->mouse_y_logical_min;
                        report->logical_max_y =
                            device->mouse_y_logical_max;
                        int32_t wheel_delta = device->mouse_wheel_bit_size
                                != 0u
                                && device->mouse_wheel_logical_min < 0
                            ? hid_signed_field(wheel_value,
                                device->mouse_wheel_bit_size)
                            : (int32_t)wheel_value;
                        report->wheel_delta = book8_wheel_detents(wheel_delta);
                        queue_hid_interrupt(device, i, td, buffer);
                        return true;
                    }
                }
            }
        }
        queue_hid_interrupt(device, i, td, buffer);
    }
    return false;
}

bool book8_usb_poll_keyboard(struct book8_usb_keyboard_report *report)
{
    if (report != NULL && book8_ohci_poll_keyboard(report)) {
        return true;
    }
    if (report == NULL || BOOK8_DIAG_NO_UHCI_POLL) {
        return false;
    }
    for (uint32_t i = 0u; i < hid_device_count; ++i) {
        struct usb_hid_device *device = &hid_devices[i];
        if (!device->active || device->kind != USB_HID_KEYBOARD
            || device->controller_index >= controller_count) {
            continue;
        }
        if (hid_schedule_stalled(device, i)) {
            continue;
        }
        struct uhci_transfer_descriptor *td = &transfer_descriptors[i];
        uint8_t *buffer = interrupt_buffers[i];
        if (device->td_pending) {
            uint32_t status = td->status;
            if ((status & UHCI_STATUS_ACTIVE) != 0u) {
                continue;
            }
            device->td_pending = false;
            if ((status & UHCI_STATUS_ERRORS) == 0u) {
                uint32_t actual = td_actual_length(status);
                device->data_toggle ^= 1u;
                if (actual >= 8u) {
                    ++diagnostics.uhci_interrupt_completions;
                    report->modifiers = buffer[0];
                    for (uint32_t key = 0u; key < 6u; ++key) {
                        report->keys[key] = buffer[2u + key];
                    }
                    queue_hid_interrupt(device, i, td, buffer);
                    return true;
                }
            }
        }
        queue_hid_interrupt(device, i, td, buffer);
    }
    return false;
}
