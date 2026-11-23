#ifndef BOOK8_KERNEL_INTERNAL_H
#define BOOK8_KERNEL_INTERNAL_H

/* Internal glue for the split kernel image.
 *
 * Types, boot-time configuration macros and shared kernel state live here.
 * Subsystem .c files include this header instead of carrying private copies
 * of the Multiboot/driver/console declarations. */

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include "app_manifest.h"
#include "../include/string.h"

#include "filesystem.h"
#include "boot_hash.h"
#include "boot_log.h"
#include "book8gui.h"
#include "registry.h"
#include "session.h"
#include "mouse_input.h"
#include "usb.h"
#include "libc_selftest.h"
#include "syscall.h"
#include "../drivers/block.h"
#include "../drivers/ata_dma.h"
#include "../drivers/ahci.h"
#include "../drivers/fb.h"
#include "../drivers/hda.h"
#include "../drivers/hda_module.h"
#include "../drivers/nv_maxwell.h"
#include "../drivers/pcie.h"
#include "../drivers/pcie_probe.h"
#include "../drivers/rtl8168.h"
#include "../drivers/rtl8168_module.h"
#include "../net/net.h"
#include "../net/dhcp.h"
#include "../net/network_settings.h"
#include "../net/push.h"
#include "../kernel/relaunch.h"
#include "../fs/fs.h"
#include "../kernel/exec.h"
#include "../kernel/driver_loader.h"
#include "../kernel/apic.h"
#include "../kernel/sched.h"
#include "../kernel/smp.h"
#include "../kernel/vmm.h"
#include "../kernel/hwdiag.h"
#include "../include/stdlib.h"
#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"
#include "driver_trust.h"

/* Baked network baseline (build.sh: BOOK8_NET_IP/MASK/GATEWAY,
 * BOOK8_TELEMETRY_TARGET). Empty strings disable the baked config. */
#ifndef BOOK8_NET_DEFAULT_IP
#define BOOK8_NET_DEFAULT_IP ""
#endif
#ifndef BOOK8_NET_DEFAULT_MASK
#define BOOK8_NET_DEFAULT_MASK ""
#endif
#ifndef BOOK8_NET_DEFAULT_GATEWAY
#define BOOK8_NET_DEFAULT_GATEWAY ""
#endif
#ifndef BOOK8_NET_DEFAULT_TELEMETRY
#define BOOK8_NET_DEFAULT_TELEMETRY ""
#endif
#ifndef BOOK8_BUILD_ID
#define BOOK8_BUILD_ID 0u
#endif
#ifndef BOOK8_DIAG_BOOT_LOG
#define BOOK8_DIAG_BOOT_LOG 0
#endif
/* Push agent: requires build/generated/book8_push_key.h (build.sh). */
#ifndef BOOK8_PUSH
#define BOOK8_PUSH 0
#endif
#if BOOK8_PUSH
#include "book8_push_key.h"
#endif
#define BOOK8_NETCFG_PATH "A:\\BOOK8NET.CFG"

/* Freestanding libc (lib/stdio.c); the kernel keeps using the compiler's own headers. */
void stdio_set_output(void (*output)(char value));
int printf(const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format,
    va_list arguments);

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u
#define MULTIBOOT_TAG_END 0u
#define MULTIBOOT_TAG_CMDLINE 1u
#define MULTIBOOT_TAG_MODULE 3u
#define MULTIBOOT_TAG_BASIC_MEMINFO 4u
#define MULTIBOOT_TAG_MMAP 6u
#define MULTIBOOT_TAG_FRAMEBUFFER 8u
#define MULTIBOOT_TAG_ACPI_OLD 14u
#define MULTIBOOT_TAG_ACPI_NEW 15u
#define MULTIBOOT_FRAMEBUFFER_RGB_TAG_SIZE 38u
#define FRAMEBUFFER_MAX_BYTES 0x40000000ull
#define COM1 0x3f8u
#define VGA_MEMORY ((volatile uint16_t *)0xb8000u)
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define CELL_WIDTH 8u
#define CELL_HEIGHT 16u
#define MAX_LINE 640u
#define MAX_DISKS 16u
#define MAX_CONTROLLERS 8u
#define USB_KEY_QUEUE_SIZE 64u
#define MOUSE_EVENT_QUEUE_SIZE 64u
#define ATA_TIMEOUT 1000000u
#define INSTALL_ROOT_START_LBA 2048u
#define INSTALL_SWAP_SECTORS (4ull * 1024u * 1024u * 1024u / 512u)
#define INSTALL_MIN_ROOT_SECTORS (2u * 1024u * 1024u * 1024u / 512u)
#define SMP_TRAMPOLINE_PHYS 0x8000u
#define SMP_AP_STACK_SIZE 16384u
#define SMP_APIC_TIMER_VECTOR 0x40u
#define SMP_SCHEDULE_IPI_VECTOR 0x41u

/* BOOK8_DIAG_MARKERS: 0 off, 1 serial markers, 2 serial + on-screen [tags]
   around the first shell input poll (bare-metal freeze isolation). */
#ifndef BOOK8_DIAG_MARKERS
#define BOOK8_DIAG_MARKERS 1
#endif
#define SERIAL_TX_WAIT_LIMIT 100000u

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bits_per_pixel;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
};

struct multiboot_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t start;
    uint32_t end;
    char command_line[];
};

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t address;
} __attribute__((packed));

struct framebuffer {
    volatile uint8_t *pixels;
    uint32_t pitch_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t scanout_pitch_bytes;
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint8_t bytes_per_pixel;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
};

struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t programming_interface;
    uint16_t vendor;
    uint16_t device;
    uint32_t bars[6];
};

struct ide_channel {
    uint16_t command_base;
    uint16_t control_base;
    struct pci_device controller;
    bool has_pci_controller;
    uint16_t bus_master_base;
};

struct disk_device {
    char name[8];
    char model[41];
    uint64_t sectors;
    bool lba48;
    bool ahci;
    bool dma;
    uint8_t channel;
    uint8_t drive;
    struct ahci_port *ahci_port;
};

struct environment_entry {
    char name[24];
    char value[64];
};

struct smp_boot_state {
    uint32_t cpu_index;
    uint32_t apic_id;
    volatile uint32_t status;
};

#define GUI_CURSOR_WIDTH 8u
#define GUI_CURSOR_HEIGHT 12u
uint64_t read_tsc(void);

/* Parsed from the Multiboot2 command line in kernel_main. */
/* book8.splash=N: static splash display holds (0 disables, >16 clamps).
 * Each hold is 3 seconds, matching one cycle of the original animation. */

/* Whole-word match of one option inside the Multiboot2 command line. */


#define PCIE_BOOT_EVENTS 1024u
void pcie_bootstrap(void);
void telemetry_service(void);
void cpu_id(uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
void network_early_telemetry(void);
void network_dhcp_service(void);
void tsc_calibrate(void);
void smp_ap_main(void *argument) __attribute__((noreturn));
bool network_enabled_service(void);

void run_command(char *line);
void gui_launch(bool quiet);
void console_write(const char *text);
void console_putc(char value);
void serial_write(const char *text);
void print_unsigned(uint64_t value);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t value);
void outb(uint16_t port, uint8_t value);
















/* ---- SMBIOS/DMI inventory -------------------------------------------------
 * Read-only parse of the firmware table handed over by Multiboot2 (or found
 * in the legacy EBDA/ROM area).  The kernel already identity-maps firmware
 * tables for ACPI, so the same bounded pointer access is used here.  Only
 * fixed-offset fields and DMI strings are copied; nothing is written. */










extern void book8_unhandled_interrupt(void);
extern void book8_ps2_mouse_irq_entry(void);
extern void book8_timer_irq_entry(void);
extern void book8_apic_timer_entry(void);
extern void book8_sched_ipi_entry(void);
extern void book8_gp_entry(void);
extern void book8_pf_entry(void);
extern bool book8_read_msr_safe(uint32_t, uint64_t *);
extern char book8_ap_trampoline_start[];
extern char book8_ap_trampoline_end[];
extern char book8_ap_trampoline_data[];
extern char book8_ap_relaunch_halt[];
extern uint64_t boot_pml4[];

/* APs must stop executing the old kernel image before the relaunch stub
 * overwrites it. The halt loop lives in the low trampoline page, which the
 * relaunch image never touches. */

























void console_clear(void);





































/* Class detection is not evidence of an operational network/audio service. */


/* NVIDIA Maxwell (GTX 970/980) foundation.  Probing only reads PCI config
 * space and BAR0's PMC_BOOT_0; no engine is touched, so a failed or absent
 * GPU leaves the software framebuffer path unchanged. */
extern uint64_t boot_pd0[512];
extern uint64_t boot_pd1[512];
extern uint64_t boot_pd2[512];
extern uint64_t boot_pd3[512];




/* boot.S identity maps 0..4 GiB with 2 MiB write-back pages; register windows
 * must be PWT|PCD (uncached).  Save each original PDE so unmap can restore it
 * instead of blindly clearing PWT|PCD that another mapping may still need. */
#define DRIVER_MMIO_MAP_MAX 64u

struct driver_mmio_record {
    uint64_t page;          /* 2 MiB page frame number */
    uint64_t original_pde;  /* PDE before the first uncached map */
    uint32_t refs;          /* 0 == free slot */
};









void pcie_log(const char *text);













































#ifndef BOOK8_NV_BIND_CHANNEL
/* Opt-in (-DBOOK8_NV_BIND_CHANNEL=1): programs PFIFO/BAR1 of a real GPU. */
#define BOOK8_NV_BIND_CHANNEL 0
#endif








bool rtl8168_bus_probe(struct book8_bus_device *device);
void rtl8168_bus_remove(struct book8_bus_device *device);
bool hda_bus_probe(struct book8_bus_device *device);
void hda_bus_remove(struct book8_bus_device *device);
bool nvidia_hda_bus_probe(struct book8_bus_device *device);











void telemetry_service(void);
void push_agent_service(void);
uint64_t read_tsc(void);
/* Driver-loop timing for telemetry v2 (reset after each sample). */

























/* Issues one command on slot 0 through the AHCI driver. data points at
 * identity-mapped memory and is transferred as data_bytes (1..4 MiB) when
 * has_data is set. */


/* Multi-sector READ/WRITE DMA EXT through the shared bounce buffer. */







uint64_t pit_clock_ticks(void);

/* Disk activity counters feed the Task Manager; timing only runs while the
 * desktop owns the PIT clock. */









/* One hardware command for up to ATA_BOUNCE_SECTORS sectors; no fallback. */




/* Boot-time check: a multi-sector DMA read must match single-sector PIO reads. */

#define SCRATCH_BASE_LBA 64u
#define SCRATCH_FULL_SECTORS 200u
#define SCRATCH_MIN_SECTORS (SCRATCH_BASE_LBA + SCRATCH_FULL_SECTORS + 8u)


/* Mirrored by tests/qemu_storage_test.py when it checks the image afterwards. */



/* Guards are always checked through the PIO path, independent of DMA. */



/* Destructive: runs only on a disk whose sector 0 starts with "BOOK8SCRATCH". */

/* Publish every discovered IDE/AHCI disk through the hardware-neutral layer. */









struct clock_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};






/* Adds BOOK8GUI_KEY_CTRL/SHIFT flags where the key contract allows them. */







void ps2_flush_output(void);





/* Sends one byte to the auxiliary device and waits for 0xFA. Some Super I/O
 * and USB-legacy-emulation controllers omit the AUX status bit, so only the
 * data value is checked; 0xFE (resend) is retried. */













#define PIT_HZ 1193182u
#define GUI_BACKBUFFER_BYTES (10u * 1024u * 1024u)


/* PIT channel 0 free-runs in rate-generator mode with a 65536 reload.
 * The GUI polls it independently of IRQ0. Each sample accumulates the elapsed
 * counts, so it must be called more often than every ~54 ms. */



































struct registry_query_context {
    const char *path;
};










bool parse_disk_number(const char *text, uint32_t *disk_index);

/* Reads one echoed line; returns the length (excess characters are dropped). */

/* Asks a Y/N question until a valid answer is given. */


/* Numbered disk menu; returns false if cancelled. Entries are 1-based. */









































bool gui_save_config(const struct book8_system_config *config);














bool gui_system_info(struct book8gui_sysinfo *info);


/* EXEC <path> [args...]: load an ELF64 from a FAT volume and run it in Ring 3. */

extern char __kernel_end[];





/* Live platform inventory for the NV Diagnostic hardware/firmware tabs.
 * Static SMBIOS strings were captured at boot; changing state (boot mode, GPU
 * BAR placement, IOMMU tables) is re-evaluated on every call. */














/* Shown in the telemetry log field for the next ten packets. */




















/* ---- Baked network baseline, authenticated push agent, in-RAM relaunch ---- */
#define RELAUNCH_STAGE_CAPACITY (BOOK8_TFTP_CAPACITY - BOOK8_RELAUNCH_PLAN_BYTES)
#define RELAUNCH_PLAN_PHYSICAL (BOOK8_NETBOOT_ADDRESS + RELAUNCH_STAGE_CAPACITY)



/* Same rules as A:\BOOK8NET.CFG: nonzero IP, contiguous mask, valid dotted quads. */











#if BOOK8_PUSH

#endif


/* Polls until the RESULT datagram left (ARP may need a round trip). */



/* Fetches request->remote from the requester's TFTP server into the staging
 * window and verifies size and SHA-256. Returns NULL on success. */

void relaunch_enter_now(void) __attribute__((noreturn));




/* Clears bus mastering on every endpoint except display (framebuffer
 * scan-out); bridges keep forwarding. The new kernel re-enables what it uses. */





/* Runs once after the filesystem is up: A:\BOOK8NET.CFG overrides the baked
 * build defaults; book8.net=off on the command line skips both. */




/* Show a pre-rendered flat image supplied as the "book8-splash" Multiboot2
 * module.  The asset is a 16-byte header ("B8SPLASH", little-endian width and
 * height) followed by packed RGBA pixels.  No decoder or script runs at boot.
 * One 3-second hold is shown by default; book8.splash=N requests N holds. */
#define BOOK8_SPLASH_HEADER_BYTES 16u
#define BOOK8_SPLASH_MAX_WIDTH 1024u
#define BOOK8_SPLASH_MAX_HEIGHT 768u
#define BOOK8_SPLASH_HOLD_MS 3000u






/* Anonymous Multiboot memory-map record made named so it can be shared. */
struct kernel_memory_map_entry {
    uint64_t base, size;
    uint32_t type;
};

/* Shared kernel state. Definitions live in src/kernel_state.c. */
extern struct framebuffer screen;
extern struct fb fbdev;
extern struct nv_gpu nv_gpu;
extern struct b8_loaded_driver nv_driver;
extern const struct b8_graphics_ops *nv_graphics;
extern struct b8_loaded_driver rtl8168_driver;
extern struct b8_loaded_driver usb_host_driver;
extern struct b8_loaded_driver hda_driver;
extern const struct book8_usb_module_api *usb_host_api;
extern const struct b8_hda_module_api *hda_api;
extern struct hda_device hda_controller;
extern uint8_t gui_cursor_under[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT * 4u];
extern uint32_t gui_cursor_under_x;
extern uint32_t gui_cursor_under_y;
extern uint32_t gui_cursor_under_width;
extern uint32_t gui_cursor_under_height;
extern bool gui_cursor_under_valid;
extern bool config_loaded;
extern char config_status[96];
extern int32_t pointer_remainder_x, pointer_remainder_y;
extern char control_log[8][96];
extern uint32_t control_log_line, control_log_column;
extern struct kernel_memory_map_entry control_memory_map[12];
extern uint32_t control_memory_count;
extern uint64_t speaker_stop;
extern uint64_t tsc_hz;
extern bool software_clock_active;
extern struct book8gui_time software_clock;
extern uint64_t software_clock_start;
extern bool console_write_active;
extern struct ide_channel ide_channels[2];
extern struct pci_device ide_controllers[MAX_CONTROLLERS];
extern struct pci_device ahci_controllers[MAX_CONTROLLERS];
extern struct disk_device disks[MAX_DISKS];
extern struct environment_entry environment[16];
extern uint32_t ide_controller_count;
extern uint32_t ahci_controller_count;
extern uint32_t disk_count;
extern const uint8_t *installer_image;
extern uint64_t installer_image_size;
extern const uint8_t *splash_image;
extern uint64_t splash_image_size;
extern bool registry_write_allowed;
extern uint8_t install_transfer_buffer[ATA_BOUNCE_BYTES];
extern uint8_t *application_image;
extern uint64_t application_image_size;
extern uint32_t cursor_x;
extern uint32_t cursor_y;
extern uint16_t vga_cursor;
extern bool use_vga;
extern bool serial_available;
extern bool shifted;
extern bool ps2_ctrl_down;
extern bool ps2_alt_down;
extern bool gui_active;
extern volatile uint32_t timer_irq_count;
extern bool autostart_desktop;
extern bool startup_quiet;
extern bool cmdline_network_off;
extern uint32_t boot_relaunch_count;
extern uint32_t book8_splash_cycles;
extern bool ps2_keyboard_controller_ready;
extern bool ps2_mouse_available;
extern uint8_t ps2_mouse_packet[4];
extern uint8_t ps2_mouse_packet_index;
extern uint8_t ps2_mouse_packet_length;
extern uint8_t ps2_mouse_id;
extern int32_t pointer_x;
extern int32_t pointer_y;
extern uint8_t pointer_buttons;
extern volatile bool ps2_mouse_irq_enabled;
extern struct book8gui_event mouse_events[MOUSE_EVENT_QUEUE_SIZE];
extern volatile uint8_t mouse_event_head;
extern volatile uint8_t mouse_event_tail;
extern uint8_t mouse_event_last_buttons;
extern bool mouse_event_last_coalescible;
extern volatile uint32_t ps2_mouse_irq_count;
extern volatile uint32_t ps2_mouse_packet_count;
extern volatile uint32_t ps2_mouse_queue_events;
extern volatile uint32_t ps2_mouse_queue_drops;
extern uint32_t usb_mouse_report_count;
extern uint32_t usb_mouse_queue_events;
extern uint32_t usb_mouse_queue_drops;
extern bool interrupt_table_ready;
extern struct idt_entry interrupt_table[256];
extern uint32_t usb_key_queue[USB_KEY_QUEUE_SIZE];
extern uint8_t usb_key_queue_head;
extern uint8_t usb_key_queue_tail;
extern struct book8_usb_keyboard_report usb_previous_keyboard_report;
extern uint8_t usb_keyboard_leds;
extern char search_path[128];
extern char prompt_format[64];
extern uint8_t sector_buffer[512];
extern struct ahci_hba ahci_hbas[MAX_CONTROLLERS];
extern uint64_t acpi_pm1a_control_address;
extern uint64_t acpi_pm1b_control_address;
extern uint8_t acpi_pm1a_address_space;
extern uint8_t acpi_pm1b_address_space;
extern uint8_t acpi_s5_sleep_type;
extern bool acpi_s5_available;
extern uint16_t acpi_smi_command_port;
extern uint8_t acpi_enable_command;
extern bool acpi_enable_available;
extern bool acpi_root_seen;
extern bool acpi_iommu_present;
extern bool boot_handoff_uefi;
extern struct book8_platform_info platform_inventory;
extern const uint8_t *smbios_table;
extern uint32_t smbios_table_size;
extern struct book8_smp_topology smp_topology;
extern struct smp_boot_state smp_boot_states[BOOK8_SMP_MAX_CPUS];
extern uint8_t smp_ap_stacks[BOOK8_SMP_MAX_CPUS][SMP_AP_STACK_SIZE];
extern bool smp_trampoline_page_usable;
extern bool smp_trampoline_page_conflict;
extern bool local_apic_ready;
extern volatile uint32_t smp_relaunch_requested;
extern volatile uint64_t smp_relaunch_halted_mask;
extern struct book8_pcie_state pcie;
extern char pcie_boot_events[PCIE_BOOT_EVENTS][BOOK8_TELEMETRY_LOG_SIZE];
extern uint32_t pcie_event_head, pcie_event_tail, pcie_event_dropped;
extern bool network_suspended;
extern struct book8_dhcp dhcp_client;
extern struct book8_network_settings network_settings;
extern uint32_t dhcp_generation;
extern bool pci_network_present;
extern bool pci_sound_present;
extern struct driver_mmio_record driver_mmio_records[DRIVER_MMIO_MAP_MAX];
extern const b8_sym_export_t b8_kernel_hal_exports[14];
extern const struct book8_bus_match nv_maxwell_matches[2];
extern const struct book8_bus_match usb_hid_matches[1];
extern const struct book8_bus_match usb_storage_matches[1];
extern const struct book8_bus_match usb_hub_matches[1];
extern const struct book8_bus_match usb_host_matches[1];
extern const struct book8_bus_match rtl8168_matches[1];
extern const struct book8_bus_match hda_matches[1];
extern const struct book8_bus_match nvidia_hda_matches[1];
extern const struct book8_bus_driver bus_drivers[8];
extern struct rtl8168_device rtl_nic;
extern struct rtl8168_dma rtl_dma;
extern struct book8_net kernel_net;
extern bool network_ready;
extern bool initialization_complete;
extern uint32_t telemetry_target;
extern uint64_t telemetry_last_tsc;
extern uint32_t netboot_info_address;
extern uint32_t poll_iterations;
extern uint64_t poll_tsc_total;
extern uint64_t poll_tsc_max;
extern bool push_executing;
extern uint64_t disk_read_bytes_total;
extern uint64_t disk_write_bytes_total;
extern uint64_t disk_busy_ticks_total;
extern bool storage_quiet;
extern uint8_t scratch_source[SCRATCH_FULL_SECTORS * 512u];
extern uint8_t scratch_verify[SCRATCH_FULL_SECTORS * 512u];
extern uint8_t gui_backbuffer[GUI_BACKBUFFER_BYTES];
extern volatile uint64_t smp_test_completed;
extern volatile uint32_t smp_test_errors;
extern volatile uint32_t smp_test_cpu_runs[BOOK8_SMP_MAX_CPUS];
extern uint64_t boot_tsc;
extern uint64_t memory_total_kb;
extern const char *network_config_source;
extern char telemetry_note[BOOK8_TELEMETRY_LOG_SIZE];
extern uint32_t telemetry_note_packets;
extern uint32_t telemetry_failures;
extern enum book8_net_result telemetry_last_result;
extern struct book8_pcie_probe manual_probe;
extern bool manual_probe_initialized;
extern uint8_t netboot_stack[16384];
extern struct book8_warm_info warm_info;
extern char push_message[BOOK8_PUSH_MESSAGE_MAX];
extern uint64_t network_boot_tsc;

/* Internal subsystem interfaces. */
void control_log_char(char c);
bool cmdline_has_option(const char *cmdline, uint32_t size, const char *option);
uint32_t book8_splash_cycles_option(const char *cmdline, uint32_t size);
void acpi_log_hex64(uint64_t value);
uint32_t acpi_read_u32(const uint8_t *bytes);
uint64_t acpi_read_u64(const uint8_t *bytes);
bool acpi_bytes_equal(const uint8_t *bytes, const char *text, size_t length);
bool acpi_checksum_valid(const uint8_t *bytes, uint32_t length);
const uint8_t *acpi_table(uint64_t address, const char *signature);
bool acpi_aml_integer(const uint8_t *aml, uint32_t end, uint32_t *offset, uint64_t *value);
bool acpi_aml_package_length(const uint8_t *aml, uint32_t end, uint32_t *offset, uint32_t *length);
bool acpi_find_s5_types(const uint8_t *dsdt, uint32_t length, uint8_t *sleep_type);
bool acpi_fadt_shutdown(const uint8_t *fadt);
bool acpi_root_table(uint64_t address, bool xsdt);
void acpi_initialize_from_rsdp(const uint8_t *rsdp, uint32_t available);
void acpi_discover(const uint8_t *info, uint32_t total_size);
void acpi_scan_rsdp_range(uint32_t start, uint32_t length);
void acpi_discover_firmware(void);
void smbios_copy_string(char *destination, uint32_t capacity, const uint8_t *strings, const uint8_t *end, uint8_t index);
uint32_t smbios_structure_end(const uint8_t *strings, const uint8_t *end);
uint32_t smbios_u16(const uint8_t *bytes);
uint32_t smbios_u32(const uint8_t *bytes);
void smbios_append_slot(const char *slot, const char *capacity);
void smbios_parse_table(void);
void smbios_table_init(const uint8_t *entry, uint32_t available);
void smbios_discover(const uint8_t *info, uint32_t total_size);
void acpi_shutdown(void);
__attribute__((noreturn)) void smp_ap_relaunch_halt(void);
uint8_t inb(uint16_t port);
void idt_set_gate(uint8_t vector, void (*handler)(void));
void interrupt_table_load(void);
void interrupt_table_initialize(void);
struct timer_frame *book8_timer_tick(struct timer_frame *frame);
uint32_t book8_smp_current_cpu_index(void);
void book8_sched_request_reschedule(uint32_t cpu_id);
struct timer_frame *book8_apic_timer_tick(struct timer_frame *frame);
struct timer_frame *book8_sched_ipi_tick(struct timer_frame *frame);
bool smp_prepare_trampoline(void);
bool smp_start_application_processors(void);
void smp_runtime_initialize(void);
uint64_t irq_save(void);
void irq_restore(uint64_t flags);
void reboot_system(void);
void outl(uint16_t port, uint32_t value);
uint32_t inl(uint16_t port);
void zero_memory(void *memory, size_t size);
void copy_string(char *destination, size_t capacity, const char *source);
bool ascii_equal(const char *left, const char *right);
char *skip_spaces(char *text);
void serial_init(void);
bool serial_ready(void);
void serial_putc(char value);
void book8_usb_debug_serial_putc(char value);
uint32_t pack_rgb(uint32_t color);
long book8_framebuffer_info(struct book8_fb_info *info);
long book8_framebuffer_draw_pixel(uint64_t x, uint64_t y, uint32_t rgb);
void draw_char_colored(char value, uint32_t x, uint32_t y, uint32_t foreground, uint32_t background);
void draw_char(char value, uint32_t x, uint32_t y);
void gui_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void gui_draw_rgba(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *pixels, uint32_t stride, uint32_t scale);
void gui_draw_text(uint32_t x, uint32_t y, const char *text, uint32_t foreground, uint32_t background);
void gui_draw_text_scaled(uint32_t x, uint32_t y, const char *text, uint32_t foreground, uint32_t background, uint32_t scale, bool bold);
bool gui_read_file(const char *path, void *destination, uint32_t capacity, uint32_t *size);
bool gui_write_file(const char *path, const void *data, uint32_t size);
void scroll_screen(void);
void console_present(void);
void print_signed(int32_t value);
void print_hex8(uint8_t value);
void print_hex32(uint32_t value);
void print_hex64(uint64_t value);
uint32_t pci_legacy_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint32_t book8_pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_legacy_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
void book8_pci_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
void pci_get_device(struct pci_device *device, uint8_t bus, uint8_t slot, uint8_t function, uint32_t identity, uint32_t class_revision);
void ide_enable_bus_master(const struct pci_device *device);
void ide_set_channel_controller(const struct pci_device *device);
void discover_pci_controllers(void);
void pci_legacy_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
void book8_pci_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
void driver_pci_write32(void *ctx, uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
uint64_t *driver_mmio_pde(uint64_t page);
struct driver_mmio_record *driver_mmio_find(uint64_t page);
struct driver_mmio_record *driver_mmio_free_slot(void);
void driver_mmio_reload_cr3(void);
volatile uint8_t *driver_map_mmio(void *ctx, uint64_t phys, uint64_t size, bool uncached);
void driver_unmap_mmio(void *ctx, const volatile void *pointer, uint64_t size);
volatile uint8_t *book8_apic_map_mmio(uint64_t physical, uint64_t size);
uint32_t b8_hal_pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void b8_hal_pci_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
void b8_hal_pci_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
volatile uint8_t *b8_hal_map_mmio(uint64_t physical, uint64_t size, uint32_t uncached);
void b8_hal_unmap_mmio(const volatile void *pointer, uint64_t size);
int32_t b8_hal_dma_address(const void *pointer, size_t size, uint64_t *physical);
void b8_hal_relax(void);
void b8_hal_serial_putc(char value);
void b8_hal_log(const char *text);
bool b8_hal_reg_delete_tree(const char *path);
bool b8_hal_reg_set_str(const char *path, const char *name, const char *value);
bool b8_hal_reg_set_u32(const char *path, const char *name, uint32_t value);
int b8_hal_snprintf(char *buffer, size_t size, const char *format, ...);
int32_t b8_hal_framebuffer_info(struct b8_framebuffer_info *info);
const uint8_t *driver_trust_digest(const char *path);
int32_t load_trusted_driver(const char *path, struct b8_loaded_driver *loaded);
bool usb_host_module_load(void);
bool hda_module_load(void);
bool usb_api_available(void);
void book8_usb_initialize(void);
void book8_usb_initialize_from_pci(const struct book8_pcie_state *pcie_state);
void book8_usb_register_bus_drivers(const struct book8_bus_driver *drivers, size_t count);
void book8_usb_rescan(const struct book8_pcie_state *pcie_state);
bool book8_usb_registry_sync(void);
const struct book8_bus_driver *book8_usb_bind_interface(uint16_t vendor_id, uint16_t device_id, uint8_t class_code, uint8_t subclass, uint8_t protocol, uint8_t address, uint8_t interface_number);
void book8_usb_bind_configuration_interfaces(uint16_t vendor_id, uint16_t device_id, const uint8_t *configuration, uint16_t length, uint8_t address);
uint32_t book8_usb_registered_driver_count(void);
uint32_t book8_usb_bound_device_count(void);
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
volatile uint8_t *pcie_map(uint64_t phys, uint64_t size);
bool pcie_delay_ms(uint32_t milliseconds);
bool nv_maxwell_bus_probe(struct book8_bus_device *device);
void nv_maxwell_bus_remove(struct book8_bus_device *device);
bool usb_hid_bus_probe(struct book8_bus_device *device);
bool usb_storage_bus_probe(struct book8_bus_device *device);
bool usb_hub_bus_probe(struct book8_bus_device *device);
bool usb_host_bus_probe(struct book8_bus_device *device);
void disable_bus_master(struct book8_bus_device *device);
struct book8_net *book8_kernel_network(void);
enum book8_net_result network_send(void *ctx, const void *frame, size_t size);
enum book8_net_result network_receive(void *ctx, void *frame, size_t capacity, size_t *size);
void network_rx_poll(void);
void rtl8168_report_status(const char *operation, int32_t status);
void nv_gpu_init(void);
void record_nv_gpu_bars(void);
void nv_driver_refresh_state(void);
bool nv_registry_sync(void);
uint8_t ide_status(struct ide_channel *channel);
void ide_delay(struct ide_channel *channel);
bool ide_wait_ready(struct ide_channel *channel);
bool ide_wait_data(struct ide_channel *channel);
bool ide_identify(struct ide_channel *channel, uint8_t drive, uint16_t identify[256]);
uint64_t identify_sector_count(const uint16_t identify[256], bool *lba48);
void identify_model(char model[41], const uint16_t identify[256]);
struct disk_device *add_disk(const char *name, const uint16_t identify[256], bool ahci, uint8_t channel, uint8_t drive, struct ahci_port *ahci_port);
bool ide_read_sector(struct ide_channel *channel, uint8_t drive, uint64_t lba, uint8_t destination[512], bool lba48);
bool ide_write_sector(struct ide_channel *channel, uint8_t drive, uint64_t lba, const uint8_t source[512], bool lba48, bool flush);
bool ahci_run(struct ahci_port *port, uint8_t command, uint64_t lba, uint16_t sector_count, void *data, uint32_t data_bytes, bool write_to_disk, bool has_data);
bool ahci_issue(struct ahci_port *port, uint8_t command, uint64_t lba, uint8_t sector_count, uint8_t data[512], bool write_to_disk, bool has_data);
bool ahci_transfer(struct ahci_port *port, uint64_t lba, uint32_t count, void *buffer, bool write);
bool ahci_read_sector(struct ahci_port *port, uint64_t lba, uint8_t destination[512]);
bool ahci_identify(struct ahci_port *port, uint16_t identify[256]);
bool ahci_write_sector(struct ahci_port *port, uint64_t lba, const uint8_t source[512], bool flush);
void discover_ide_disks(void);
void discover_ahci_disks(void);
void discover_storage(void);
uint64_t disk_io_begin(void);
bool disk_io_end(bool write, bool ok, uint64_t started);
bool disk_read_sector(struct disk_device *disk, uint64_t lba, uint8_t destination[512]);
bool disk_write_sector(struct disk_device *disk, uint64_t lba, const uint8_t source[512]);
bool disk_write_sector_unflushed(struct disk_device *disk, uint64_t lba, const uint8_t source[512]);
bool disk_flush(struct disk_device *disk);
bool disk_dma_channel(const struct disk_device *disk, struct ata_dma_channel *out);
bool dma_report_failure(const struct disk_device *disk, bool write, enum ata_status status);
bool disk_transfer_hw(struct disk_device *disk, uint64_t lba, uint32_t count, void *buffer, bool write);
bool disk_transfer(struct disk_device *disk, uint64_t lba, uint32_t count, uint8_t *buffer, bool write);
bool block_adapter_read(block_device_t *dev, uint32_t start_lba, uint32_t sector_count, void *buf);
bool block_adapter_write(block_device_t *dev, uint32_t start_lba, uint32_t sector_count, const void *buf);
void storage_selftest(struct disk_device *disk);
void scratch_pattern(uint8_t *buf, uint32_t sectors, uint8_t seed);
bool scratch_equal(const uint8_t *a, const uint8_t *b, uint32_t sectors);
bool scratch_set_guard(struct disk_device *disk, uint32_t lba, uint8_t fill);
bool scratch_guard_ok(struct disk_device *disk, uint32_t lba, uint8_t fill);
bool scratch_check_pio(struct disk_device *disk, uint32_t lba, const uint8_t *expected, uint32_t sectors);
const char *storage_write_phases(struct disk_device *disk);
void storage_write_selftest(struct disk_device *disk);
bool application_read(block_device_t *dev, uint32_t lba, uint32_t count, void *buffer);
bool application_write(block_device_t *dev, uint32_t lba, uint32_t count, const void *buffer);
void register_block_devices(void);
bool filesystem_read_sector(uint32_t disk, uint64_t lba, uint8_t sector[512]);
bool filesystem_write_sector(uint32_t disk, uint64_t lba, const uint8_t sector[512]);
bool registry_write_file(const char *path, const void *data, uint32_t size);
uint64_t filesystem_disk_size(uint32_t disk);
void filesystem_print(const char *text);
void print_disk_inventory(void);
uint8_t cmos_read(uint8_t register_index);
uint8_t bcd_to_binary(uint8_t value);
struct clock_time read_clock(void);
void print_two_digits(uint8_t value);
char keyboard_char(uint8_t scan);
bool usb_report_has_key(const struct book8_usb_keyboard_report *report, uint8_t key);
uint32_t gui_key_with_modifiers(uint32_t key, bool ctrl, bool shift);
bool usb_queue_key(uint32_t key);
uint32_t usb_key_value(uint8_t usage, uint8_t modifiers);
void usb_keyboard_update(void);
bool usb_keyboard_get_key(uint32_t *key);
bool ps2_wait_input_empty(void);
bool ps2_wait_output_full(uint8_t *status);
bool ps2_read_command_byte(uint8_t *command_byte);
bool ps2_keyboard_initialize(void);
bool ps2_write_command_byte(uint8_t value);
bool ps2_mouse_send(uint8_t value);
bool ps2_mouse_read_id(uint8_t *device_id);
bool ps2_mouse_set_sample_rate(uint8_t rate);
bool ps2_mouse_initialize(void);
bool ps2_mouse_set_irq_mode(bool enabled);
bool ps2_mouse_irq_enable(void);
bool ps2_mouse_irq_disable(void);
bool ps2_mouse_feed(uint8_t data, struct book8gui_event *event);
bool mouse_event_enqueue(const struct book8gui_event *event, bool usb);
bool mouse_event_dequeue(struct book8gui_event *event);
void book8_ps2_mouse_irq_dispatch(void);
void gui_pump_input(void);
bool gui_poll_event(struct book8gui_event *event);
void pit_clock_start(void);
void scheduler_test(bool user_mode);
void smp_test_worker(void *argument);
void smp_scheduler_test(void);
void gui_present(void);
bool gui_cursor_save_under(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
bool gui_cursor_restore_under(void);
void gui_present_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void diag_marker(const char *tag);
char read_char(void);
void print_prompt(void);
void print_help(void);
void nvinfo_command(void);
void print_usb_poll_status(void);
void print_input_status(void);
bool parse_integer(const char *text, uint8_t base, uint64_t *value);
char *next_command_token(char **cursor);
bool memdump_page_mapped(uint64_t address);
bool memdump_range_mapped(uint64_t address, uint32_t length);
void memdump_command(char *arguments);
void ioport_command(char *arguments);
bool same_pci_function(const struct book8_pcie_node *node, uint8_t bus, uint8_t slot, uint8_t function);
uint64_t pcie_dump_bar_size(const struct book8_pcie_node *node, uint8_t index, const char **cache);
void pcidump_command(void);
void pnp_scan_command(void);
void registry_seed_program(const char *id, const char *name, const char *path, const char *app_class, uint32_t hidden, uint32_t system_component);
void registry_seed_value(const char *path, const char *name, const char *value);
void registry_format_ipv4(uint32_t address, char *text, size_t capacity);
bool registry_parse_ipv4(const char *text, uint32_t *address);
void registry_migrate_legacy_settings(void);
void registry_seed_settings(void);
void registry_seed_file_associations(void);
void registry_seed_defaults(void);
bool registry_print_value(const char *name, void *context);
bool registry_print_key(const char *name, void *context);
void registry_command(char *arguments);
bool read_active_tss(uint16_t *selector, uint64_t *rsp0);
void hwdiag_command(void);
void print_date(void);
void print_time(void);
void run_read_command(char *arguments);
bool confirm_text(const char *prompt);
uint32_t read_install_line(char *buffer, uint32_t capacity);
bool confirm_yes_no(const char *prompt);
void print_install_disk(const struct disk_device *disk);
bool select_install_disk(uint32_t *disk_index);
bool confirm_install_target(uint32_t disk_index);
bool select_install_apps(void);
/* Zero means install; the installer sets entries before seeding the registry. */
extern uint8_t book8_install_app_excluded[BOOK8_MANIFEST_APP_COUNT];
uint32_t read_u32_le(const uint8_t *bytes);
uint16_t read_u16_le(const uint8_t *bytes);
void write_u32_le(uint8_t *bytes, uint32_t value);
void install_report_progress(const char *operation, uint64_t completed, uint64_t total);
bool install_clear_sector_range(struct disk_device *disk, uint64_t lba, uint32_t sectors, const char *operation);
bool install_seed_default_config(void);
bool install_resize_fat32(struct disk_device *disk, uint32_t start_lba, uint32_t total_sectors);
void install_set_mbr_partition(uint8_t *mbr, uint32_t index, uint8_t active, uint8_t type, uint32_t start_lba, uint32_t sectors);
void install_command(const char *arguments);
void run_write_command(char *arguments);
void set_environment(char *argument);
void run_filesystem_command(const char *command);
void run_external_command(const char *command);
void gui_run_command(char *command);
void gui_shutdown(void);
void gui_reboot(void);
uint32_t gui_status_flags(void);
bool gui_network_activity(uint32_t *tx, uint32_t *rx);
uint32_t gui_month_days(uint32_t year, uint32_t month);
bool gui_shift_time(struct book8gui_time *time, int64_t minutes);
bool gui_read_time(struct book8gui_time *time);
void cmos_write(uint8_t register_index, uint8_t value);
bool gui_set_time(const struct book8gui_time *time);
bool gui_usb_mouse_active(void);
bool gui_list_files(const char *path, book8_directory_entry_fn visitor, void *context);
bool gui_file_info(const char *path, struct book8_file_info *info);
bool gui_rename_entry(const char *path, const char *new_name);
bool gui_delete_entry(const char *path);
bool gui_copy_to_directory(const char *source, const char *directory, bool cut);
bool gui_make_directory(const char *path);
bool gui_create_file(const char *path, const void *data, uint32_t size);
bool gui_read_cluster(char drive, uint32_t cluster, void *data, uint32_t capacity, uint32_t *size);
uint32_t gui_session_uid(void);
uint64_t gui_session_token(void);
uint32_t gui_drive_count(void);
char gui_drive_letter(uint32_t index);
const char *gui_last_error_text(void);
void gui_diagnostic(const char *message);
void gui_apply_registry_config(struct book8_system_config *config);
void gui_load_config(void);
bool gui_test_tone(uint32_t frequency);
bool gui_audio_state(uint8_t *volume, bool *muted);
bool gui_audio_set_volume(uint8_t volume);
bool gui_audio_set_mute(bool muted);
bool gui_media_play_wav(const uint8_t *wav, uint32_t size);
bool gui_media_pause(bool paused);
void gui_media_stop(void);
bool gui_media_seek(uint32_t position_ms);
bool gui_media_state(struct book8gui_media_status *status);
bool gui_media_pcm_window(const uint8_t **pcm, uint32_t *size,
    uint64_t *position_bytes);
void gui_media_service(void);
bool gui_control_readout(enum book8_control_page page, uint32_t row, char *text, uint32_t capacity, uint32_t *bar);
void run_exec(char *line);
bool gui_platform_info(struct book8_platform_info *info);
void gui_display_size(uint32_t *width, uint32_t *height);
bool gui_set_display_mode(uint32_t width, uint32_t height);
bool gui_nvdiag_snapshot(struct book8_nvdiag_snapshot *snapshot);
struct book8_net *gui_share_network(void);
bool gui_launch_program(const char *path);
long book8_gui_utility_request(const struct book8_gui_utility_request *request);
bool gui_launch_utility(uint32_t kind, const char *document);
bool gui_drive_readable(uint32_t index);
bool gui_program_drive_readable(uint32_t index);
uint32_t gui_thread_snapshot(struct book8gui_thread_info *out, uint32_t capacity);
void network_print_ip(uint32_t ip);
void telemetry_event(const char *text);
uint32_t health_flags(uint8_t phy);
uint32_t tsc_to_us(uint64_t ticks);
bool telemetry_sample(const char *pulse);
bool probe_telemetry_available(void);
bool probe_emit(const char *record);
bool probe_hex(const char *text, uint32_t *value);
void probe_command(char *argument, bool ack);
void telemetry_command(char *argument);
void network_status(void);
void network_configure_command(char *argument);
bool netboot_overlap(uint64_t start, uint64_t length);
bool netboot_memory_available(void);
uint64_t netboot_milliseconds(void *ctx);
void netboot_idle(void *ctx);
bool netboot_disable_dma(void);
void network_boot_command(char *argument);
bool network_calibrate_clock(void);
bool netcfg_valid(const struct book8_netcfg *config);
bool network_baked_config(struct book8_netcfg *config);
enum book8_net_result network_apply_config(const struct book8_netcfg *config, const char *source);
uint64_t network_milliseconds(void);
bool book8_network_settings_get(struct book8_network_status *status);
bool book8_network_settings_apply(const struct book8_network_settings *settings, bool persist);
bool book8_network_settings_renew(void);
void msr_guard_test(void);
uint64_t push_milliseconds(void *ctx);
void push_entropy(void *ctx, uint8_t out[BOOK8_SHA256_BYTES]);
void push_agent_start(void);
void push_flush_result(uint32_t milliseconds);
void push_finish(enum book8_push_status status, const char *message);
bool push_valid_83(const char *name);
const char *push_fetch(const struct book8_push_request *request, size_t capacity);
bool relaunch_prepare_staged(size_t bytes, struct book8_relaunch_info *info, const char **error);
void push_execute(const struct book8_push_request *request);
bool relaunch_disable_dma(void);
void smp_quiesce_for_relaunch(void);
void relaunch_command(char *argument);
void network_save_command(void);
void network_boot_configure(void);
void print_threads(void);
void boot_splash_delay_ms(uint32_t milliseconds);
void play_boot_splash(void);
void shell(void);
bool exec_drive_extent(uint32_t drive, struct fs_extent *extent);
long book8_console_write(const char *buf, unsigned long len);
void kernel_main(uint32_t magic, uint32_t info_address);

#endif /* BOOK8_KERNEL_INTERNAL_H */
