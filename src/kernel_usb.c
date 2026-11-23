#include "kernel_internal.h"

/* Kernel-side USB module API forwarders and diagnostics. */

bool usb_api_available(void)
{
    return usb_host_api != NULL;
}

void book8_usb_initialize(void)
{
    if (usb_api_available())
        usb_host_api->initialize();
}

void book8_usb_initialize_from_pci(const struct book8_pcie_state *pcie_state)
{
    if (usb_api_available())
        usb_host_api->initialize_from_pci(pcie_state);
}

void book8_usb_register_bus_drivers(const struct book8_bus_driver *drivers,
    size_t count)
{
    if (usb_api_available())
        usb_host_api->register_bus_drivers(drivers, count);
}

void book8_usb_rescan(const struct book8_pcie_state *pcie_state)
{
    if (usb_api_available())
        usb_host_api->rescan(pcie_state);
}

bool book8_usb_registry_sync(void)
{
    return !usb_api_available() || usb_host_api->registry_sync();
}

const struct book8_bus_driver *book8_usb_bind_interface(uint16_t vendor_id,
    uint16_t device_id, uint8_t class_code, uint8_t subclass, uint8_t protocol,
    uint8_t address, uint8_t interface_number)
{
    return usb_api_available()
        ? usb_host_api->bind_interface(vendor_id, device_id, class_code,
            subclass, protocol, address, interface_number)
        : NULL;
}

void book8_usb_bind_configuration_interfaces(uint16_t vendor_id,
    uint16_t device_id, const uint8_t *configuration, uint16_t length,
    uint8_t address)
{
    if (usb_api_available())
        usb_host_api->bind_configuration_interfaces(vendor_id, device_id,
            configuration, length, address);
}

uint32_t book8_usb_registered_driver_count(void)
{
    return usb_api_available()
        ? usb_host_api->registered_driver_count() : 0u;
}

uint32_t book8_usb_bound_device_count(void)
{
    return usb_api_available() ? usb_host_api->bound_device_count() : 0u;
}

const char *book8_usb_diag_log(void)
{
    return usb_api_available() ? usb_host_api->diag_log() : "";
}

void book8_usb_get_diagnostics(struct book8_usb_diagnostics *diagnostics)
{
    if (diagnostics != NULL) {
        *diagnostics = (struct book8_usb_diagnostics){0};
        if (usb_api_available())
            usb_host_api->get_diagnostics(diagnostics);
    }
}

bool book8_usb_mouse_active(void)
{
    return usb_api_available() && usb_host_api->mouse_active();
}

bool book8_usb_keyboard_active(void)
{
    return usb_api_available() && usb_host_api->keyboard_active();
}

uint32_t book8_usb_detected_controller_count(void)
{
    return usb_api_available()
        ? usb_host_api->detected_controller_count() : 0u;
}

uint32_t book8_usb_uhci_controller_count(void)
{
    return usb_api_available() ? usb_host_api->uhci_controller_count() : 0u;
}

uint32_t book8_usb_ohci_controller_count(void)
{
    return usb_api_available() ? usb_host_api->ohci_controller_count() : 0u;
}

uint32_t book8_usb_ehci_controller_count(void)
{
    return usb_api_available() ? usb_host_api->ehci_controller_count() : 0u;
}

uint32_t book8_usb_xhci_controller_count(void)
{
    return usb_api_available() ? usb_host_api->xhci_controller_count() : 0u;
}

bool book8_usb_poll_mouse(struct book8_usb_mouse_report *report)
{
    return usb_api_available() && usb_host_api->poll_mouse(report);
}

bool book8_usb_poll_keyboard(struct book8_usb_keyboard_report *report)
{
    return usb_api_available() && usb_host_api->poll_keyboard(report);
}
