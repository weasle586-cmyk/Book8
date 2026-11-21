#ifndef BOOK8_USB_H
#define BOOK8_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mouse_input.h"
#include "../drivers/bus.h"

struct book8_pcie_state;
#define BOOK8_USB_MAX_UHCI_CONTROLLERS 16u
#define BOOK8_USB_UHCI_PORTS_PER_CONTROLLER 2u

struct book8_usb_mouse_report {
    int32_t x;
    int32_t y;
    int32_t logical_min_x;
    int32_t logical_max_x;
    int32_t logical_min_y;
    int32_t logical_max_y;
    int16_t wheel_delta;
    uint8_t buttons;
    bool absolute;
};

struct book8_usb_keyboard_report {
    uint8_t modifiers;
    uint8_t keys[6];
};

struct book8_usb_uhci_diagnostic {
    uint32_t bus;
    uint32_t slot;
    uint32_t function;
    uint32_t io_base;
    uint32_t pci_bar;
    uint32_t pci_command;
    uint32_t controller_status;
    uint32_t frame_number;
    uint32_t port_status[BOOK8_USB_UHCI_PORTS_PER_CONTROLLER];
};

struct book8_usb_diagnostics {
    uint32_t uhci_controllers;
    uint32_t uhci_diagnostic_count;
    struct book8_usb_uhci_diagnostic
        uhci[BOOK8_USB_MAX_UHCI_CONTROLLERS];
    uint32_t uhci_running_controllers;
    uint32_t uhci_schedule_failures;
    uint32_t uhci_interrupt_completions;
    uint32_t uhci_interrupt_stalls;
    uint32_t ohci_controllers;
    uint32_t ehci_controllers;
    uint32_t pci_enable_failures;
    uint32_t connected_ports;
    uint32_t reset_ports;
    uint32_t addressed_devices;
    uint32_t boot_hid_interfaces;
    uint32_t active_hid_devices;
    uint32_t ehci_legacy_handoffs;
    uint32_t ehci_legacy_handoff_failures;
    uint32_t ehci_legacy_handoff_skipped;
    uint32_t ehci_powered_ports;
    uint32_t ehci_companion_ports;
    uint32_t ehci_uhci_companion_ports;
    uint32_t ehci_unmapped_companion_ports;
    uint32_t ehci_companion_mappings;
    uint32_t ehci_route_fallbacks;
    uint32_t device_descriptor_failures;
    uint32_t configuration_descriptor_failures;
    uint32_t non_boot_hid_devices;
    uint32_t hid_control_failures;
    uint32_t ohci_started;
    uint32_t ohci_ports;
    uint32_t ohci_connected_ports;
    uint32_t ohci_hid_devices;
    uint32_t ohci_hub_devices;
    uint32_t ohci_reports;
    uint32_t ohci_errors;
    uint32_t ohci_reset_failures;
    uint32_t ohci_control_failures;
    uint32_t ohci_ownership_failures;
    uint32_t ehci_released_to_ohci;
    uint32_t ohci_last_cc;
    uint32_t ohci_last_stage;
    uint32_t ohci_control_retries;
    uint32_t registered_bus_drivers;
    uint32_t bound_bus_devices;
};

#define BOOK8_USB_MODULE_API_VERSION 1u
#define BOOK8_USB_IOCTL_GET_API 1u

struct book8_usb_module_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void (*initialize)(void);
    void (*initialize_from_pci)(const struct book8_pcie_state *);
    void (*register_bus_drivers)(const struct book8_bus_driver *, size_t);
    void (*rescan)(const struct book8_pcie_state *);
    bool (*registry_sync)(void);
    const struct book8_bus_driver *(*bind_interface)(uint16_t, uint16_t,
        uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
    void (*bind_configuration_interfaces)(uint16_t, uint16_t, const uint8_t *,
        uint16_t, uint8_t);
    uint32_t (*registered_driver_count)(void);
    uint32_t (*bound_device_count)(void);
    const char *(*diag_log)(void);
    void (*get_diagnostics)(struct book8_usb_diagnostics *);
    bool (*mouse_active)(void);
    bool (*keyboard_active)(void);
    uint32_t (*detected_controller_count)(void);
    uint32_t (*uhci_controller_count)(void);
    uint32_t (*ohci_controller_count)(void);
    uint32_t (*ehci_controller_count)(void);
    uint32_t (*xhci_controller_count)(void);
    bool (*poll_mouse)(struct book8_usb_mouse_report *);
    bool (*poll_keyboard)(struct book8_usb_keyboard_report *);
};

void book8_usb_initialize(void);
void book8_usb_initialize_from_pci(const struct book8_pcie_state *);
void book8_usb_register_bus_drivers(const struct book8_bus_driver *, size_t);
void book8_usb_rescan(const struct book8_pcie_state *);
bool book8_usb_registry_sync(void);
const struct book8_bus_driver *book8_usb_bind_interface(uint16_t, uint16_t,
    uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
void book8_usb_bind_configuration_interfaces(uint16_t, uint16_t,
    const uint8_t *, uint16_t, uint8_t);
uint32_t book8_usb_registered_driver_count(void);
uint32_t book8_usb_bound_device_count(void);
void book8_usb_debug_serial_putc(char value);
const char *book8_usb_diag_log(void);
void book8_usb_get_diagnostics(struct book8_usb_diagnostics *diagnostics);
bool book8_usb_mouse_active(void);
bool book8_usb_keyboard_active(void);
uint32_t book8_usb_detected_controller_count(void);
uint32_t book8_usb_uhci_controller_count(void);
uint32_t book8_usb_ohci_controller_count(void);
uint32_t book8_usb_ehci_controller_count(void);
uint32_t book8_usb_xhci_controller_count(void);
bool book8_usb_poll_mouse(struct book8_usb_mouse_report *report);
bool book8_usb_poll_keyboard(struct book8_usb_keyboard_report *report);

#endif
