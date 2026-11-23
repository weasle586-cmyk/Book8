#include "kernel_internal.h"

/* Definitions for state shared between the kernel subsystem TUs. */

/* The normal boot seeds every packaged app; the installer excludes the ones
 * the operator deselected. */
uint8_t book8_install_app_excluded[BOOK8_MANIFEST_APP_COUNT];

struct framebuffer screen;
struct fb fbdev;
struct nv_gpu nv_gpu;
struct b8_loaded_driver nv_driver;
const struct b8_graphics_ops *nv_graphics;
struct b8_loaded_driver rtl8168_driver;
struct b8_loaded_driver usb_host_driver;
struct b8_loaded_driver hda_driver;
const struct book8_usb_module_api *usb_host_api;
const struct b8_hda_module_api *hda_api;
struct hda_device hda_controller;
uint8_t gui_cursor_under[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT * 4u];
uint32_t gui_cursor_under_x;
uint32_t gui_cursor_under_y;
uint32_t gui_cursor_under_width;
uint32_t gui_cursor_under_height;
bool gui_cursor_under_valid;
bool config_loaded;
char config_status[96] = "Configuration defaults.";
int32_t pointer_remainder_x, pointer_remainder_y;
char control_log[8][96];
uint32_t control_log_line, control_log_column;
struct kernel_memory_map_entry control_memory_map[12];
uint32_t control_memory_count;
uint64_t speaker_stop;
uint64_t tsc_hz;
bool software_clock_active;
struct book8gui_time software_clock;
uint64_t software_clock_start;
bool console_write_active;
struct ide_channel ide_channels[2] = {
    {0x1f0u, 0x3f4u, {0}, false, 0u},
    {0x170u, 0x374u, {0}, false, 0u}
};
struct pci_device ide_controllers[MAX_CONTROLLERS];
struct pci_device ahci_controllers[MAX_CONTROLLERS];
struct disk_device disks[MAX_DISKS];
struct environment_entry environment[16];
uint32_t ide_controller_count;
uint32_t ahci_controller_count;
uint32_t disk_count;
const uint8_t *installer_image;
uint64_t installer_image_size;
const uint8_t *splash_image;
uint64_t splash_image_size;
bool registry_write_allowed;
uint8_t install_transfer_buffer[ATA_BOUNCE_BYTES]
    __attribute__((aligned(16)));
uint8_t *application_image;
uint64_t application_image_size;
uint32_t cursor_x;
uint32_t cursor_y;
uint16_t vga_cursor;
bool use_vga;
bool serial_available;
bool shifted;
bool ps2_ctrl_down;
bool ps2_alt_down;
bool gui_active;
volatile uint32_t timer_irq_count;
bool autostart_desktop;
bool startup_quiet;
bool cmdline_network_off;
uint32_t boot_relaunch_count;
uint32_t book8_splash_cycles = 1u;
bool ps2_keyboard_controller_ready;
bool ps2_mouse_available;
uint8_t ps2_mouse_packet[4];
uint8_t ps2_mouse_packet_index;
uint8_t ps2_mouse_packet_length = 3u;
uint8_t ps2_mouse_id;
int32_t pointer_x;
int32_t pointer_y;
uint8_t pointer_buttons;
volatile bool ps2_mouse_irq_enabled;
struct book8gui_event mouse_events[MOUSE_EVENT_QUEUE_SIZE];
volatile uint8_t mouse_event_head;
volatile uint8_t mouse_event_tail;
uint8_t mouse_event_last_buttons;
bool mouse_event_last_coalescible;
volatile uint32_t ps2_mouse_irq_count;
volatile uint32_t ps2_mouse_packet_count;
volatile uint32_t ps2_mouse_queue_events;
volatile uint32_t ps2_mouse_queue_drops;
uint32_t usb_mouse_report_count;
uint32_t usb_mouse_queue_events;
uint32_t usb_mouse_queue_drops;
bool interrupt_table_ready;
struct idt_entry interrupt_table[256] __attribute__((aligned(16)));
uint32_t usb_key_queue[USB_KEY_QUEUE_SIZE];
uint8_t usb_key_queue_head;
uint8_t usb_key_queue_tail;
struct book8_usb_keyboard_report usb_previous_keyboard_report;
uint8_t usb_keyboard_leds;
char search_path[128] = "A:\\";
char prompt_format[64] = "$P$G ";
uint8_t sector_buffer[512] __attribute__((aligned(4096)));
struct ahci_hba ahci_hbas[MAX_CONTROLLERS];
uint64_t acpi_pm1a_control_address;
uint64_t acpi_pm1b_control_address;
uint8_t acpi_pm1a_address_space;
uint8_t acpi_pm1b_address_space;
uint8_t acpi_s5_sleep_type;
bool acpi_s5_available;
uint16_t acpi_smi_command_port;
uint8_t acpi_enable_command;
bool acpi_enable_available;
bool acpi_root_seen;
bool acpi_iommu_present;
bool boot_handoff_uefi;
struct book8_platform_info platform_inventory;
const uint8_t *smbios_table;
uint32_t smbios_table_size;
struct book8_smp_topology smp_topology;
struct smp_boot_state smp_boot_states[BOOK8_SMP_MAX_CPUS];
uint8_t smp_ap_stacks[BOOK8_SMP_MAX_CPUS][SMP_AP_STACK_SIZE]
    __attribute__((aligned(16)));
bool smp_trampoline_page_usable;
bool smp_trampoline_page_conflict;
bool local_apic_ready;
volatile uint32_t smp_relaunch_requested;
volatile uint64_t smp_relaunch_halted_mask;
struct book8_pcie_state pcie;
char pcie_boot_events[PCIE_BOOT_EVENTS][BOOK8_TELEMETRY_LOG_SIZE];
uint32_t pcie_event_head, pcie_event_tail, pcie_event_dropped;
bool network_suspended;
struct book8_dhcp dhcp_client;
struct book8_network_settings network_settings = {
    .mode = BOOK8_NETWORK_DHCP, .retry_ms = 1000u
};
uint32_t dhcp_generation;
bool pci_network_present;
bool pci_sound_present;
struct driver_mmio_record driver_mmio_records[DRIVER_MMIO_MAP_MAX];
const b8_sym_export_t b8_kernel_hal_exports[] = {
    {.name = "b8_hal_pci_read32",
        .ptr = (void *)(uintptr_t)&b8_hal_pci_read32,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_pci_write32",
        .ptr = (void *)(uintptr_t)&b8_hal_pci_write32,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_pci_write16",
        .ptr = (void *)(uintptr_t)&b8_hal_pci_write16,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_map_mmio",
        .ptr = (void *)(uintptr_t)&b8_hal_map_mmio,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_unmap_mmio",
        .ptr = (void *)(uintptr_t)&b8_hal_unmap_mmio,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_dma_address",
        .ptr = (void *)(uintptr_t)&b8_hal_dma_address,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_relax", .ptr = (void *)(uintptr_t)&b8_hal_relax,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_serial_putc",
        .ptr = (void *)(uintptr_t)&b8_hal_serial_putc,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_log", .ptr = (void *)(uintptr_t)&b8_hal_log,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_reg_delete_tree",
        .ptr = (void *)(uintptr_t)&b8_hal_reg_delete_tree,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_reg_set_str",
        .ptr = (void *)(uintptr_t)&b8_hal_reg_set_str,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_reg_set_u32",
        .ptr = (void *)(uintptr_t)&b8_hal_reg_set_u32,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_snprintf",
        .ptr = (void *)(uintptr_t)&b8_hal_snprintf,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_framebuffer_info",
        .ptr = (void *)(uintptr_t)&b8_hal_framebuffer_info,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
};
const struct book8_bus_match nv_maxwell_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = NV_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = NV_DEVICE_GTX980, .device_mask = 0xffffu,
        .class_code = 0x030000u, .class_mask = 0xff0000u},
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = NV_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = NV_DEVICE_GTX970, .device_mask = 0xffffu,
        .class_code = 0x030000u, .class_mask = 0xff0000u},
};
const struct book8_bus_match usb_hid_matches[] = {
    {.bus_type = BOOK8_BUS_USB, .class_code = 0x030000u,
        .class_mask = 0xff0000u},
};
const struct book8_bus_match usb_storage_matches[] = {
    {.bus_type = BOOK8_BUS_USB, .class_code = 0x080000u,
        .class_mask = 0xff0000u},
};
const struct book8_bus_match usb_hub_matches[] = {
    {.bus_type = BOOK8_BUS_USB, .class_code = 0x090000u,
        .class_mask = 0xff0000u},
};
const struct book8_bus_match usb_host_matches[] = {
    {.bus_type = BOOK8_BUS_PCI, .class_code = 0x0c0300u,
        .class_mask = 0xffff00u},
};
const struct book8_bus_match rtl8168_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = RTL8168_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = RTL8168_DEVICE_ID, .device_mask = 0xffffu,
        .class_code = 0x020000u, .class_mask = 0xff0000u},
};
const struct book8_bus_match hda_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = HDA_ATI_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = HDA_ATI_DEVICE_ID, .device_mask = 0xffffu,
        .class_code = 0x040300u, .class_mask = 0xffff00u},
};
const struct book8_bus_match nvidia_hda_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = NV_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = 0x0fbbu, .device_mask = 0xffffu,
        .class_code = 0x040300u, .class_mask = 0xffff00u},
};
const struct book8_bus_driver bus_drivers[] = {
    {.name = "nvidia-maxwell", .matches = nv_maxwell_matches,
        .match_count = sizeof(nv_maxwell_matches) / sizeof(nv_maxwell_matches[0]),
        .bar0_window_size = NV_PMC_ENABLE + sizeof(uint32_t),
        .probe = nv_maxwell_bus_probe, .remove = nv_maxwell_bus_remove},
    {.name = "usb-hid", .matches = usb_hid_matches,
        .match_count = sizeof(usb_hid_matches) / sizeof(usb_hid_matches[0]),
        .probe = usb_hid_bus_probe},
    {.name = "usb-storage-discovery", .matches = usb_storage_matches,
        .match_count = sizeof(usb_storage_matches) / sizeof(usb_storage_matches[0]),
        .probe = usb_storage_bus_probe},
    {.name = "usb-hub-discovery", .matches = usb_hub_matches,
        .match_count = sizeof(usb_hub_matches) / sizeof(usb_hub_matches[0]),
        .probe = usb_hub_bus_probe},
    {.name = "usb-host-controller", .matches = usb_host_matches,
        .match_count = sizeof(usb_host_matches) / sizeof(usb_host_matches[0]),
        .probe = usb_host_bus_probe},
    {.name = "rtl8168", .matches = rtl8168_matches,
        .match_count = sizeof(rtl8168_matches) / sizeof(rtl8168_matches[0]),
        .probe = rtl8168_bus_probe, .remove = rtl8168_bus_remove},
    {.name = "hda", .matches = hda_matches,
        .match_count = sizeof(hda_matches) / sizeof(hda_matches[0]),
        .probe = hda_bus_probe, .remove = hda_bus_remove},
    {.name = "nvidia-hda-function", .matches = nvidia_hda_matches,
        .match_count = sizeof(nvidia_hda_matches) / sizeof(nvidia_hda_matches[0]),
        .probe = nvidia_hda_bus_probe},
};
struct rtl8168_device rtl_nic;
struct rtl8168_dma rtl_dma;
struct book8_net kernel_net;
bool network_ready;
bool initialization_complete;
uint32_t telemetry_target;
uint64_t telemetry_last_tsc;
uint32_t netboot_info_address;
uint32_t poll_iterations;
uint64_t poll_tsc_total;
uint64_t poll_tsc_max;
bool push_executing;
uint64_t disk_read_bytes_total;
uint64_t disk_write_bytes_total;
uint64_t disk_busy_ticks_total;
bool storage_quiet;
uint8_t scratch_source[SCRATCH_FULL_SECTORS * 512u];
uint8_t scratch_verify[SCRATCH_FULL_SECTORS * 512u];
uint8_t gui_backbuffer[GUI_BACKBUFFER_BYTES] __attribute__((aligned(4096)));
volatile uint64_t smp_test_completed;
volatile uint32_t smp_test_errors;
volatile uint32_t smp_test_cpu_runs[BOOK8_SMP_MAX_CPUS];
uint64_t boot_tsc;
uint64_t memory_total_kb;
const char *network_config_source = "none";
char telemetry_note[BOOK8_TELEMETRY_LOG_SIZE];
uint32_t telemetry_note_packets;
uint32_t telemetry_failures;
enum book8_net_result telemetry_last_result = BOOK8_NET_OK;
struct book8_pcie_probe manual_probe;
bool manual_probe_initialized;
uint8_t netboot_stack[16384] __attribute__((aligned(16)));
struct book8_warm_info warm_info;
char push_message[BOOK8_PUSH_MESSAGE_MAX];
uint64_t network_boot_tsc;
