#include "../src/usb.h"
#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"
#include "../include/syscall.h"

extern uint32_t b8_hal_pci_read32(uint8_t, uint8_t, uint8_t, uint8_t);
extern void b8_hal_pci_write32(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
extern void b8_hal_pci_write16(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
extern int32_t b8_hal_dma_address(const void *, size_t, uint64_t *);
extern void b8_hal_serial_putc(char);
extern bool b8_hal_reg_delete_tree(const char *);
extern bool b8_hal_reg_set_str(const char *, const char *, const char *);
extern bool b8_hal_reg_set_u32(const char *, const char *, uint32_t);
extern int b8_hal_snprintf(char *, size_t, const char *, ...);

struct usb_host_import_table {
    b8_sym_table_header_t header;
    b8_sym_import_t imports[9];
};

__attribute__((used, section(B8_SHLIB_IMPORT_SECTION)))
static const struct usb_host_import_table usb_host_imports = {
    .header = {
        .magic = {'B', '8', 'S', '1'},
        .abi_version = B8_SHLIB_ABI_VERSION,
        .count = 9u,
    },
    .imports = {
        {"b8_hal_pci_read32", 1u, 0u},
        {"b8_hal_pci_write32", 1u, 0u},
        {"b8_hal_pci_write16", 1u, 0u},
        {"b8_hal_dma_address", 1u, 0u},
        {"b8_hal_serial_putc", 1u, 0u},
        {"b8_hal_reg_delete_tree", 1u, 0u},
        {"b8_hal_reg_set_str", 1u, 0u},
        {"b8_hal_reg_set_u32", 1u, 0u},
        {"b8_hal_snprintf", 1u, 0u},
    },
};

static const struct book8_usb_module_api usb_host_api = {
    .abi_version = BOOK8_USB_MODULE_API_VERSION,
    .struct_size = sizeof(struct book8_usb_module_api),
    .initialize = book8_usb_initialize,
    .initialize_from_pci = book8_usb_initialize_from_pci,
    .register_bus_drivers = book8_usb_register_bus_drivers,
    .rescan = book8_usb_rescan,
    .registry_sync = book8_usb_registry_sync,
    .bind_interface = book8_usb_bind_interface,
    .bind_configuration_interfaces = book8_usb_bind_configuration_interfaces,
    .registered_driver_count = book8_usb_registered_driver_count,
    .bound_device_count = book8_usb_bound_device_count,
    .diag_log = book8_usb_diag_log,
    .get_diagnostics = book8_usb_get_diagnostics,
    .mouse_active = book8_usb_mouse_active,
    .keyboard_active = book8_usb_keyboard_active,
    .detected_controller_count = book8_usb_detected_controller_count,
    .uhci_controller_count = book8_usb_uhci_controller_count,
    .ohci_controller_count = book8_usb_ohci_controller_count,
    .ehci_controller_count = book8_usb_ehci_controller_count,
    .xhci_controller_count = book8_usb_xhci_controller_count,
    .poll_mouse = book8_usb_poll_mouse,
    .poll_keyboard = book8_usb_poll_keyboard,
};

static int32_t usb_host_module_init(void)
{
    return 0;
}

static int32_t usb_host_module_ioctl(uint32_t command, void *argument)
{
    if (command != BOOK8_USB_IOCTL_GET_API || argument == NULL)
        return -BOOK8_EINVAL;
    *(const struct book8_usb_module_api **)argument = &usb_host_api;
    return 0;
}

static const b8_driver_t usb_host_module_descriptor = {
    .abi_version = B8_DRIVER_ABI_VERSION,
    .struct_size = sizeof(b8_driver_t),
    .version_major = 1u,
    .init = usb_host_module_init,
    .ioctl = usb_host_module_ioctl,
};

__attribute__((visibility("default")))
const b8_driver_t *b8_driver_entry(void)
{
    return &usb_host_module_descriptor;
}
