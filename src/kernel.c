#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include "app_manifest.h"
#include "../include/string.h"

#include "filesystem.h"
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

static struct framebuffer screen;
static struct fb fbdev;
static struct nv_gpu nv_gpu;
static struct b8_loaded_driver nv_driver;
static const struct b8_graphics_ops *nv_graphics;
static struct b8_loaded_driver rtl8168_driver;
static struct b8_loaded_driver usb_host_driver;
static struct b8_loaded_driver hda_driver;
static const struct book8_usb_module_api *usb_host_api;
static const struct b8_hda_module_api *hda_api;
static struct hda_device hda_controller;
#define GUI_CURSOR_WIDTH 8u
#define GUI_CURSOR_HEIGHT 12u
static uint8_t gui_cursor_under[GUI_CURSOR_WIDTH * GUI_CURSOR_HEIGHT * 4u];
static uint32_t gui_cursor_under_x;
static uint32_t gui_cursor_under_y;
static uint32_t gui_cursor_under_width;
static uint32_t gui_cursor_under_height;
static bool gui_cursor_under_valid;
static bool config_loaded;
static char config_status[96] = "Configuration defaults.";
static int32_t pointer_remainder_x, pointer_remainder_y;
static char control_log[8][96];
static uint32_t control_log_line, control_log_column;
static struct {
    uint64_t base, size;
    uint32_t type;
} control_memory_map[12];
static uint32_t control_memory_count;
static uint64_t speaker_stop;
static uint64_t tsc_hz;
static inline uint64_t read_tsc(void);
static bool software_clock_active;
static struct book8gui_time software_clock;
static uint64_t software_clock_start;
static bool console_write_active;

static void control_log_char(char c)
{
    if (c == '\r') { return; }
    if (c == '\n') {
        control_log_line = (control_log_line + 1u) % 8u;
        control_log_column = 0u;
        control_log[control_log_line][0] = '\0';
    } else if (control_log_column + 1u < sizeof(control_log[0])) {
        control_log[control_log_line][control_log_column++] = c;
        control_log[control_log_line][control_log_column] = '\0';
    }
}
static struct ide_channel ide_channels[2] = {
    {0x1f0u, 0x3f4u, {0}, false, 0u},
    {0x170u, 0x374u, {0}, false, 0u}
};
static struct pci_device ide_controllers[MAX_CONTROLLERS];
static struct pci_device ahci_controllers[MAX_CONTROLLERS];
static struct disk_device disks[MAX_DISKS];
static struct environment_entry environment[16];
static uint32_t ide_controller_count;
static uint32_t ahci_controller_count;
static uint32_t disk_count;
static const uint8_t *installer_image;
static uint64_t installer_image_size;
static bool registry_write_allowed;
static uint8_t install_transfer_buffer[ATA_BOUNCE_BYTES]
    __attribute__((aligned(16)));
static uint8_t *application_image;
static uint64_t application_image_size;
static uint32_t cursor_x;
static uint32_t cursor_y;
static uint16_t vga_cursor;
static bool use_vga;
static bool serial_available;
static bool shifted;
static bool ps2_ctrl_down;
static bool ps2_alt_down;
static bool gui_active;
static volatile uint32_t timer_irq_count;
static bool autostart_desktop;
static bool startup_quiet;
/* Parsed from the Multiboot2 command line in kernel_main. */
static bool cmdline_network_off;
static uint32_t boot_relaunch_count;

/* Whole-word match of one option inside the Multiboot2 command line. */
static bool cmdline_has_option(const char *cmdline, uint32_t size,
    const char *option)
{
    uint32_t start = 0u;
    while (start < size && cmdline[start] != '\0') {
        while (start < size && cmdline[start] == ' ') {
            ++start;
        }
        uint32_t end = start;
        while (end < size && cmdline[end] != '\0' && cmdline[end] != ' ') {
            ++end;
        }
        uint32_t i = 0u;
        while (start + i < end && option[i] != '\0'
            && cmdline[start + i] == option[i]) {
            ++i;
        }
        if (option[i] == '\0' && start + i == end) {
            return true;
        }
        start = end;
    }
    return false;
}
static bool ps2_keyboard_controller_ready;
static bool ps2_mouse_available;
static uint8_t ps2_mouse_packet[4];
static uint8_t ps2_mouse_packet_index;
static uint8_t ps2_mouse_packet_length = 3u;
static uint8_t ps2_mouse_id;
static int32_t pointer_x;
static int32_t pointer_y;
static uint8_t pointer_buttons;
static volatile bool ps2_mouse_irq_enabled;
static struct book8gui_event mouse_events[MOUSE_EVENT_QUEUE_SIZE];
static volatile uint8_t mouse_event_head;
static volatile uint8_t mouse_event_tail;
static uint8_t mouse_event_last_buttons;
static bool mouse_event_last_coalescible;
static volatile uint32_t ps2_mouse_irq_count;
static volatile uint32_t ps2_mouse_packet_count;
static volatile uint32_t ps2_mouse_queue_events;
static volatile uint32_t ps2_mouse_queue_drops;
static uint32_t usb_mouse_report_count;
static uint32_t usb_mouse_queue_events;
static uint32_t usb_mouse_queue_drops;
static bool interrupt_table_ready;
static struct idt_entry interrupt_table[256] __attribute__((aligned(16)));
static uint32_t usb_key_queue[USB_KEY_QUEUE_SIZE];
static uint8_t usb_key_queue_head;
static uint8_t usb_key_queue_tail;
static struct book8_usb_keyboard_report usb_previous_keyboard_report;
static uint8_t usb_keyboard_leds;
static char search_path[128] = "A:\\";
static char prompt_format[64] = "$P$G ";
static uint8_t sector_buffer[512] __attribute__((aligned(4096)));
static struct ahci_hba ahci_hbas[MAX_CONTROLLERS];
static uint64_t acpi_pm1a_control_address;
static uint64_t acpi_pm1b_control_address;
static uint8_t acpi_pm1a_address_space;
static uint8_t acpi_pm1b_address_space;
static uint8_t acpi_s5_sleep_type;
static bool acpi_s5_available;
static uint16_t acpi_smi_command_port;
static uint8_t acpi_enable_command;
static bool acpi_enable_available;
static struct book8_smp_topology smp_topology;
static struct smp_boot_state smp_boot_states[BOOK8_SMP_MAX_CPUS];
static uint8_t smp_ap_stacks[BOOK8_SMP_MAX_CPUS][SMP_AP_STACK_SIZE]
    __attribute__((aligned(16)));
static bool smp_trampoline_page_usable;
static bool smp_trampoline_page_conflict;
static bool local_apic_ready;
static volatile uint32_t smp_relaunch_requested;
static volatile uint64_t smp_relaunch_halted_mask;
static struct book8_pcie_state pcie;
#define PCIE_BOOT_EVENTS 1024u
static char pcie_boot_events[PCIE_BOOT_EVENTS][BOOK8_TELEMETRY_LOG_SIZE];
static uint32_t pcie_event_head, pcie_event_tail, pcie_event_dropped;
static void pcie_bootstrap(void);
static void telemetry_service(void);
static void cpu_id(uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
static void network_early_telemetry(void);
static void network_dhcp_service(void);
static void tsc_calibrate(void);
static void smp_ap_main(void *argument) __attribute__((noreturn));
static bool network_enabled_service(void);
static bool network_suspended;
static struct book8_dhcp dhcp_client;
static struct book8_network_settings network_settings = {
    .mode = BOOK8_NETWORK_DHCP, .retry_ms = 1000u
};
static uint32_t dhcp_generation;

static void run_command(char *line);
static void gui_launch(bool quiet);
static void console_write(const char *text);
static void console_putc(char value);
static void serial_write(const char *text);
static void print_unsigned(uint64_t value);
static inline uint16_t inw(uint16_t port);
static inline void outw(uint16_t port, uint16_t value);
static inline void outb(uint16_t port, uint8_t value);

static void acpi_log_hex64(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int32_t shift = 60; shift >= 0; shift -= 4) {
        console_putc(digits[(value >> (uint32_t)shift) & 0x0fu]);
    }
}

static uint32_t acpi_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t acpi_read_u64(const uint8_t *bytes)
{
    return (uint64_t)acpi_read_u32(bytes)
        | ((uint64_t)acpi_read_u32(bytes + 4u) << 32u);
}

static bool acpi_bytes_equal(const uint8_t *bytes, const char *text,
    size_t length)
{
    for (size_t i = 0u; i < length; ++i) {
        if (bytes[i] != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

static bool acpi_checksum_valid(const uint8_t *bytes, uint32_t length)
{
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < length; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0u;
}

static const uint8_t *acpi_table(uint64_t address, const char *signature)
{
    if (address == 0u || address > 0x100000000ull - 36u) {
        return NULL;
    }
    const uint8_t *table = (const uint8_t *)(uintptr_t)address;
    uint32_t length = acpi_read_u32(table + 4u);
    if (length < 36u || length > 1024u * 1024u
        || length > 0x100000000ull - address
        || !acpi_bytes_equal(table, signature, 4u)
        || !acpi_checksum_valid(table, length)) {
        return NULL;
    }
    return table;
}

static bool acpi_aml_integer(const uint8_t *aml, uint32_t end,
    uint32_t *offset, uint64_t *value)
{
    if (*offset >= end) {
        return false;
    }
    uint8_t opcode = aml[(*offset)++];
    if (opcode == 0x00u) {
        *value = 0u;
        return true;
    }
    if (opcode == 0x01u) {
        *value = 1u;
        return true;
    }
    if (opcode == 0xffu) {
        *value = UINT64_MAX;
        return true;
    }
    uint32_t size;
    if (opcode == 0x0au) {
        size = 1u;
    } else if (opcode == 0x0bu) {
        size = 2u;
    } else if (opcode == 0x0cu) {
        size = 4u;
    } else if (opcode == 0x0eu) {
        size = 8u;
    } else {
        return false;
    }
    if (size > end - *offset) {
        return false;
    }
    *value = 0u;
    for (uint32_t i = 0u; i < size; ++i) {
        *value |= (uint64_t)aml[(*offset)++] << (i * 8u);
    }
    return true;
}

static bool acpi_aml_package_length(const uint8_t *aml, uint32_t end,
    uint32_t *offset, uint32_t *length)
{
    if (*offset >= end) {
        return false;
    }
    uint8_t lead = aml[(*offset)++];
    uint32_t follow_bytes = lead >> 6u;
    if (follow_bytes > 3u || follow_bytes > end - *offset) {
        return false;
    }
    if (follow_bytes == 0u) {
        *length = lead & 0x3fu;
        return true;
    }
    *length = lead & 0x0fu;
    for (uint32_t i = 0u; i < follow_bytes; ++i) {
        *length |= (uint32_t)aml[(*offset)++] << (4u + i * 8u);
    }
    return true;
}

static bool acpi_find_s5_types(const uint8_t *dsdt, uint32_t length,
    uint8_t *sleep_type)
{
    for (uint32_t i = 37u; i + 5u < length; ++i) {
        if (!acpi_bytes_equal(dsdt + i, "_S5_", 4u)
            || dsdt[i + 4u] != 0x12u) {
            continue;
        }
        uint32_t name_start = i;
        while (name_start > 37u
            && (dsdt[name_start - 1u] == 0x5eu
                || dsdt[name_start - 1u] == 0x5cu)) {
            --name_start;
        }
        if (name_start == 0u || dsdt[name_start - 1u] != 0x08u) {
            continue;
        }
        uint32_t offset = i + 5u;
        uint32_t package_length;
        if (!acpi_aml_package_length(dsdt, length, &offset,
            &package_length)) {
            continue;
        }
        uint32_t package_length_bytes = offset - (i + 5u);
        if (package_length < package_length_bytes) {
            continue;
        }
        uint32_t package_end = i + 5u + package_length;
        if (package_end > length || offset >= package_end) {
            continue;
        }
        uint8_t element_count = dsdt[offset++];
        uint64_t type_a;
        uint64_t type_b;
        if (element_count < 2u
            || !acpi_aml_integer(dsdt, package_end, &offset, &type_a)
            || !acpi_aml_integer(dsdt, package_end, &offset, &type_b)
            || type_a > 7u || type_b > 7u) {
            continue;
        }
        *sleep_type = (uint8_t)type_a;
        return true;
    }
    return false;
}

static bool acpi_fadt_shutdown(const uint8_t *fadt)
{
    uint32_t fadt_length = acpi_read_u32(fadt + 4u);
    if (fadt_length < 116u) {
        return false;
    }

    uint64_t dsdt_address = acpi_read_u32(fadt + 40u);
    uint64_t control_address = acpi_read_u32(fadt + 64u);
    uint8_t control_length = fadt[89u];
    uint8_t control_address_space = 1u;
    uint64_t second_control_address = acpi_read_u32(fadt + 68u);
    uint8_t second_control_address_space = 1u;
    uint32_t smi_command = acpi_read_u32(fadt + 48u);
    uint8_t enable_command = fadt[52u];
    if (fadt_length >= 184u) {
        const uint8_t *gas = fadt + 172u;
        uint64_t extended_address = acpi_read_u64(gas + 4u);
        if ((gas[0] == 0u || gas[0] == 1u) && gas[1] >= 16u
            && gas[2] == 0u && extended_address != 0u) {
            control_address = extended_address;
            control_address_space = gas[0];
            control_length = (uint8_t)((gas[1] + 7u) / 8u);
        }
        gas = fadt + 184u;
        if (fadt_length >= 196u) {
            extended_address = acpi_read_u64(gas + 4u);
            if ((gas[0] == 0u || gas[0] == 1u) && gas[1] >= 16u
                && gas[2] == 0u && extended_address != 0u) {
                second_control_address = extended_address;
                second_control_address_space = gas[0];
            }
        }
    }
    if (control_address == 0u || control_length < 2u
        || (control_address_space == 1u
            && control_address > UINT16_MAX - 1u)
        || (control_address_space == 0u
            && control_address > UINT32_MAX - 1u)) {
        return false;
    }

    if (fadt_length >= 148u) {
        uint64_t extended_dsdt = acpi_read_u64(fadt + 140u);
        if (extended_dsdt != 0u) {
            dsdt_address = extended_dsdt;
        }
    }
    const uint8_t *dsdt = acpi_table(dsdt_address, "DSDT");
    uint8_t sleep_type;
    if (dsdt == NULL
        || !acpi_find_s5_types(dsdt, acpi_read_u32(dsdt + 4u),
            &sleep_type)) {
        return false;
    }
    acpi_pm1a_control_address = control_address;
    acpi_pm1a_address_space = control_address_space;
    if (second_control_address != 0u
        && (second_control_address_space == 0u
            || second_control_address <= UINT16_MAX - 1u)
        && (second_control_address_space != 0u
            || second_control_address <= UINT32_MAX - 1u)) {
        acpi_pm1b_control_address = second_control_address;
        acpi_pm1b_address_space = second_control_address_space;
    }
    acpi_s5_sleep_type = sleep_type;
    acpi_s5_available = true;
    if (smi_command != 0u && smi_command <= UINT16_MAX
        && enable_command != 0u) {
        acpi_smi_command_port = (uint16_t)smi_command;
        acpi_enable_command = enable_command;
        acpi_enable_available = true;
    }

    console_write("ACPI: FADT rev 0x");
    acpi_log_hex64(fadt[8u]);
    console_write(" at 0x");
    acpi_log_hex64((uint64_t)(uintptr_t)fadt);
    console_write("; DSDT 0x");
    acpi_log_hex64(dsdt_address);
    console_write("; PM1a ");
    console_write(control_address_space == 1u ? "I/O " : "MMIO ");
    console_write("0x");
    acpi_log_hex64(control_address);
    console_write("; S5 type ");
    print_unsigned(sleep_type);
    console_write(".\n");
    return true;
}

static bool acpi_root_table(uint64_t address, bool xsdt)
{
    const uint8_t *root = acpi_table(address, xsdt ? "XSDT" : "RSDT");
    if (root == NULL) {
        return false;
    }
    uint32_t length = acpi_read_u32(root + 4u);
    uint32_t entry_size = xsdt ? 8u : 4u;
    if ((length - 36u) % entry_size != 0u) {
        return false;
    }
    for (uint32_t offset = 36u; offset < length; offset += entry_size) {
        uint64_t table_address = xsdt
            ? acpi_read_u64(root + offset)
            : acpi_read_u32(root + offset);
        const uint8_t *madt = acpi_table(table_address, "APIC");
        if (madt != NULL && smp_topology.cpu_count == 0u) {
            if (book8_smp_parse_madt(madt, acpi_read_u32(madt + 4u),
                    &smp_topology)) {
                char message[96];
                snprintf(message, sizeof(message),
                    "BOOK8-SMP: MADT LAPIC=0x%08x enabled CPUs=%u\r\n",
                    (uint32_t)smp_topology.local_apic_address,
                    smp_topology.cpu_count);
                serial_write(message);
            } else {
                serial_write("BOOK8-SMP: invalid or unsupported MADT; BSP-only mode\r\n");
            }
        }
        const uint8_t *mcfg = acpi_table(table_address, "MCFG");
        if (mcfg != NULL && pcie.ecam_count == 0u) {
            (void)book8_pcie_add_mcfg(&pcie, mcfg, acpi_read_u32(mcfg + 4u));
        }
        const uint8_t *fadt = acpi_table(table_address, "FACP");
        if (fadt != NULL && !acpi_s5_available) {
            (void)acpi_fadt_shutdown(fadt);
        }
    }
    return true;
}

static void acpi_initialize_from_rsdp(const uint8_t *rsdp,
    uint32_t available)
{
    if (available < 20u || !acpi_bytes_equal(rsdp, "RSD PTR ", 8u)
        || !acpi_checksum_valid(rsdp, 20u)) {
        return;
    }
    uint8_t revision = rsdp[15u];
    if (revision >= 2u && available >= 36u) {
        uint32_t length = acpi_read_u32(rsdp + 20u);
        if (length < 36u || length > available
            || !acpi_checksum_valid(rsdp, length)) {
            return;
        }
        uint64_t xsdt_address = acpi_read_u64(rsdp + 24u);
        console_write("ACPI: RSDP rev ");
        print_unsigned(revision);
        console_write(" XSDT 0x");
        acpi_log_hex64(xsdt_address);
        console_write(" RSDT 0x");
        acpi_log_hex64(acpi_read_u32(rsdp + 16u));
        console_write(".\n");
        if (xsdt_address != 0u && acpi_root_table(xsdt_address, true)) {
            return;
        }
    } else {
        console_write("ACPI: RSDP rev ");
        print_unsigned(revision);
        console_write(" RSDT 0x");
        acpi_log_hex64(acpi_read_u32(rsdp + 16u));
        console_write(".\n");
    }
    (void)acpi_root_table(acpi_read_u32(rsdp + 16u), false);
}

static void acpi_discover(const uint8_t *info, uint32_t total_size)
{
    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(info + offset);
        if (tag->size < sizeof(*tag) || tag->size > total_size - offset) {
            return;
        }
        if ((tag->type == MULTIBOOT_TAG_ACPI_OLD
                || tag->type == MULTIBOOT_TAG_ACPI_NEW)
            && tag->size > sizeof(*tag)) {
            acpi_initialize_from_rsdp((const uint8_t *)tag + sizeof(*tag),
                tag->size - sizeof(*tag));
            if (acpi_s5_available) {
                return;
            }
        }
        if (tag->type == MULTIBOOT_TAG_END) {
            return;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            return;
        }
        offset += next;
    }
}

static void acpi_scan_rsdp_range(uint32_t start, uint32_t length)
{
    uint32_t end = start + length;
    for (uint32_t address = (start + 15u) & ~15u;
        address <= end - 20u; address += 16u) {
        const uint8_t *rsdp = (const uint8_t *)(uintptr_t)address;
        if (acpi_bytes_equal(rsdp, "RSD PTR ", 8u)) {
            uint32_t available = end - address;
            if (available > 4096u) {
                available = 4096u;
            }
            acpi_initialize_from_rsdp(rsdp, available);
            if (acpi_s5_available) {
                return;
            }
        }
    }
}

static void acpi_discover_firmware(void)
{
    uintptr_t ebda_pointer_address = 0x40eu;
    uint16_t ebda_segment;
    __asm__ volatile ("movw (%1), %0"
        : "=r"(ebda_segment) : "r"(ebda_pointer_address) : "memory");
    uint32_t ebda = (uint32_t)ebda_segment << 4u;
    if (ebda >= 0x80000u && ebda < 0xa0000u) {
        acpi_scan_rsdp_range(ebda, 1024u);
    }
    if (!acpi_s5_available) {
        acpi_scan_rsdp_range(0xe0000u, 0x20000u);
    }
}

static void acpi_shutdown(void)
{
    if (!acpi_s5_available || acpi_pm1a_control_address == 0u) {
        console_write("ACPI S5 shutdown is unavailable; halting CPU.\n");
    } else {
        uint16_t control = acpi_pm1a_address_space == 1u
            ? inw((uint16_t)acpi_pm1a_control_address)
            : *(volatile uint16_t *)(uintptr_t)acpi_pm1a_control_address;
        if ((control & 1u) == 0u && acpi_enable_available) {
            outb(acpi_smi_command_port, acpi_enable_command);
            for (uint32_t attempt = 0u; attempt < 10000000u; ++attempt) {
                control = acpi_pm1a_address_space == 1u
                    ? inw((uint16_t)acpi_pm1a_control_address)
                    : *(volatile uint16_t *)(uintptr_t)
                        acpi_pm1a_control_address;
                if ((control & 1u) != 0u) {
                    break;
                }
                __asm__ volatile ("pause");
            }
        }
        if ((control & 1u) == 0u && acpi_enable_available) {
            console_write("ACPI: SCI enable timed out; attempting S5.\n");
        }
        control = (uint16_t)((control & ~((7u << 10u) | (1u << 13u)))
            | ((uint16_t)acpi_s5_sleep_type << 10u) | (1u << 13u));
        if (acpi_pm1a_address_space == 1u) {
            outw((uint16_t)acpi_pm1a_control_address, control);
        } else {
            *(volatile uint16_t *)(uintptr_t)acpi_pm1a_control_address =
                control;
        }
        if (acpi_pm1b_control_address != 0u) {
            uint16_t control_b = acpi_pm1b_address_space == 1u
                ? inw((uint16_t)acpi_pm1b_control_address)
                : *(volatile uint16_t *)(uintptr_t)
                    acpi_pm1b_control_address;
            control_b = (uint16_t)((control_b & ~((7u << 10u) | (1u << 13u)))
                | ((uint16_t)acpi_s5_sleep_type << 10u) | (1u << 13u));
            if (acpi_pm1b_address_space == 1u) {
                outw((uint16_t)acpi_pm1b_control_address, control_b);
            } else {
                *(volatile uint16_t *)(uintptr_t)acpi_pm1b_control_address =
                    control_b;
            }
        }
    }
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

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
__attribute__((noreturn)) static void smp_ap_relaunch_halt(void)
{
    uintptr_t offset = (uintptr_t)book8_ap_relaunch_halt
        - (uintptr_t)book8_ap_trampoline_start;
    void (*halt)(void) = (void (*)(void))(uintptr_t)(SMP_TRAMPOLINE_PHYS + offset);
    halt();
    __builtin_unreachable();
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void idt_set_gate(uint8_t vector, void (*handler)(void))
{
    uintptr_t address = (uintptr_t)handler;
    struct idt_entry *entry = &interrupt_table[vector];

    entry->offset_low = (uint16_t)address;
    entry->selector = 0x08u;
    entry->ist = 0u;
    entry->attributes = 0x8eu;
    entry->offset_middle = (uint16_t)(address >> 16u);
    entry->offset_high = (uint32_t)(address >> 32u);
    entry->reserved = 0u;
}

static void interrupt_table_load(void)
{
    struct idt_pointer pointer = {
        .limit = (uint16_t)(sizeof(interrupt_table) - 1u),
        .address = (uintptr_t)interrupt_table,
    };
    __asm__ volatile ("lidt %0" : : "m"(pointer) : "memory");
}

static void interrupt_table_initialize(void)
{
    if (interrupt_table_ready) {
        interrupt_table_load();
        return;
    }
    for (uint32_t vector = 0u; vector < 256u; ++vector) {
        idt_set_gate((uint8_t)vector, book8_unhandled_interrupt);
    }
    idt_set_gate(0x20u, book8_timer_irq_entry);
    idt_set_gate(0x2cu, book8_ps2_mouse_irq_entry);
    idt_set_gate(SMP_APIC_TIMER_VECTOR, book8_apic_timer_entry);
    idt_set_gate(SMP_SCHEDULE_IPI_VECTOR, book8_sched_ipi_entry);
    idt_set_gate(13u, book8_gp_entry);
    idt_set_gate(14u, book8_pf_entry);

    outb(0x20u, 0x11u);
    outb(0x80u, 0u);
    outb(0xa0u, 0x11u);
    outb(0x80u, 0u);
    outb(0x21u, 0x20u);
    outb(0xa1u, 0x28u);
    outb(0x21u, 0x04u);
    outb(0xa1u, 0x02u);
    outb(0x21u, 0x01u);
    outb(0xa1u, 0x01u);
    outb(0x21u, 0xfeu);
    outb(0xa1u, 0xffu);
    interrupt_table_ready = true;
    interrupt_table_load();
}

struct timer_frame *book8_timer_tick(struct timer_frame *frame)
{
    book8_apic_eoi();
    ++timer_irq_count;
    return exec_scheduler_test_active()
        ? exec_timer_tick(frame) : sched_timer_tick(frame);
}

uint32_t book8_smp_current_cpu_index(void)
{
    uint32_t apic_id = book8_apic_current_id();
    if (apic_id == UINT32_MAX)
        return 0u;
    for (uint32_t i = 0u; i < smp_topology.cpu_count; ++i) {
        if (smp_topology.apic_ids[i] == apic_id)
            return i;
    }
    return 0u;
}

void book8_sched_request_reschedule(uint32_t cpu_id)
{
    uint32_t current_cpu = book8_smp_current_cpu_index();
    if (cpu_id == current_cpu) {
        __asm__ volatile ("int $0x41" : : : "memory");
    } else if (local_apic_ready && cpu_id < smp_topology.cpu_count) {
        (void)book8_apic_send_ipi(smp_topology.apic_ids[cpu_id],
            SMP_SCHEDULE_IPI_VECTOR);
    }
}

struct timer_frame *book8_apic_timer_tick(struct timer_frame *frame)
{
    book8_apic_eoi();
    uint32_t cpu = book8_smp_current_cpu_index();
    if (cpu != 0u && __atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_or_fetch(&smp_relaunch_halted_mask, 1ull << cpu, __ATOMIC_RELEASE);
        smp_ap_relaunch_halt();
    }
    return sched_timer_tick_cpu(frame, cpu, false, false);
}

struct timer_frame *book8_sched_ipi_tick(struct timer_frame *frame)
{
    book8_apic_eoi();
    uint32_t cpu = book8_smp_current_cpu_index();
    if (cpu != 0u && __atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_or_fetch(&smp_relaunch_halted_mask, 1ull << cpu, __ATOMIC_RELEASE);
        smp_ap_relaunch_halt();
    }
    return sched_timer_tick_cpu(frame, cpu, false, true);
}

static void smp_ap_main(void *argument)
{
    struct smp_boot_state *state = argument;
    if (state == NULL || !book8_apic_initialize_local()
        || book8_apic_current_id() != state->apic_id) {
        if (state != NULL)
            __atomic_store_n(&state->status, 2u, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    interrupt_table_load();
    uint32_t timer_count = book8_apic_calibrate_timer(tsc_hz);
    if (timer_count == 0u || !sched_cpu_online(state->cpu_index)) {
        __atomic_store_n(&state->status, 2u, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    book8_apic_timer_start(timer_count, SMP_APIC_TIMER_VECTOR);
    __atomic_store_n(&state->status, 1u, __ATOMIC_RELEASE);
    for (;;) {
        if (__atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE) != 0u) {
            __atomic_or_fetch(&smp_relaunch_halted_mask,
                1ull << state->cpu_index, __ATOMIC_RELEASE);
            smp_ap_relaunch_halt();
        }
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}

static bool smp_prepare_trampoline(void)
{
    uintptr_t source = (uintptr_t)book8_ap_trampoline_start;
    uintptr_t end = (uintptr_t)book8_ap_trampoline_end;
    uintptr_t data_offset = (uintptr_t)book8_ap_trampoline_data - source;
    if (!smp_trampoline_page_usable || end <= source
        || end - source > 4096u || data_offset + 4u * sizeof(uint64_t) > 4096u)
        return false;
    volatile uint8_t *destination =
        (volatile uint8_t *)(uintptr_t)SMP_TRAMPOLINE_PHYS;
    const uint8_t *bytes = (const uint8_t *)source;
    for (uintptr_t i = 0u; i < end - source; ++i)
        destination[i] = bytes[i];
    return true;
}

static bool smp_start_application_processors(void)
{
    if (smp_topology.cpu_count <= 1u)
        return true;
    if (!smp_prepare_trampoline())
        return false;

    uint64_t data_offset = (uintptr_t)book8_ap_trampoline_data
        - (uintptr_t)book8_ap_trampoline_start;
    volatile uint64_t *startup_data = (volatile uint64_t *)(uintptr_t)
        (SMP_TRAMPOLINE_PHYS + data_offset);
    startup_data[0] = (uint64_t)(uintptr_t)boot_pml4;
    startup_data[3] = (uint64_t)(uintptr_t)smp_ap_main;

    for (uint32_t cpu = 1u; cpu < smp_topology.cpu_count; ++cpu) {
        struct smp_boot_state *state = &smp_boot_states[cpu];
        state->cpu_index = cpu;
        state->apic_id = smp_topology.apic_ids[cpu];
        __atomic_store_n(&state->status, 0u, __ATOMIC_RELAXED);
        uintptr_t stack_top = (uintptr_t)&smp_ap_stacks[cpu][SMP_AP_STACK_SIZE];
        startup_data[1] = (uint64_t)(stack_top - sizeof(uint64_t));
        startup_data[2] = (uint64_t)(uintptr_t)state;
        if (!book8_apic_start_cpu(state->apic_id,
                (uint8_t)(SMP_TRAMPOLINE_PHYS >> 12u), tsc_hz)) {
            __atomic_store_n(&state->status, 2u, __ATOMIC_RELEASE);
            continue;
        }
        uint64_t start = read_tsc();
        uint64_t timeout = tsc_hz / 2u;
        while (__atomic_load_n(&state->status, __ATOMIC_ACQUIRE) == 0u
            && read_tsc() - start < timeout)
            __asm__ volatile ("pause");
        if (__atomic_load_n(&state->status, __ATOMIC_ACQUIRE) != 1u)
            continue;
        char message[80];
        snprintf(message, sizeof(message),
            "BOOK8-SMP: AP online id=%u logical=%u\r\n",
            state->apic_id, cpu);
        serial_write(message);
    }
    return true;
}

static void smp_runtime_initialize(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpu_id(1u, &eax, &ebx, &ecx, &edx);
    uint32_t bsp_apic_id = ebx >> 24u;
    if (smp_topology.cpu_count == 0u
        || !book8_smp_set_bsp(&smp_topology, bsp_apic_id)
        || !book8_apic_initialize(smp_topology.local_apic_address)) {
        local_apic_ready = false;
        sched_init(1u);
        serial_write("BOOK8-SMP: APIC/MADT unavailable; BSP-only scheduling\r\n");
        return;
    }
    local_apic_ready = true;
    book8_apic_enable_legacy_pic();
    uint32_t supported = 1u;
    for (uint32_t i = 1u; i < smp_topology.cpu_count; ++i) {
        if (smp_topology.apic_ids[i] <= 255u)
            smp_topology.apic_ids[supported++] = smp_topology.apic_ids[i];
    }
    smp_topology.cpu_count = supported;
    if (supported > 1u && !smp_trampoline_page_usable) {
        smp_topology.cpu_count = 1u;
        sched_init(1u);
        serial_write("BOOK8-SMP: no usable low trampoline page; BSP-only scheduling\r\n");
        return;
    }
    sched_init(smp_topology.cpu_count);
    if (!smp_start_application_processors()) {
        sched_init(1u);
        smp_topology.cpu_count = 1u;
        serial_write("BOOK8-SMP: AP trampoline unavailable; BSP-only scheduling\r\n");
        return;
    }
    char message[80];
    snprintf(message, sizeof(message),
        "BOOK8-SMP: online CPUs=%u detected=%u\r\n",
        sched_online_cpu_count(), smp_topology.cpu_count);
    serial_write(message);
}

static uint64_t irq_save(void)
{
    uint64_t flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint64_t flags)
{
    if ((flags & 0x200u) != 0u) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static void reboot_system(void)
{
    __asm__ volatile ("cli" : : : "memory");
    bool input_buffer_clear = false;
    for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
        if ((inb(0x64u) & 0x02u) == 0u) {
            input_buffer_clear = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    if (input_buffer_clear) {
        outb(0x64u, 0xfeu);
        for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
            __asm__ volatile ("pause");
        }
    }

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) empty_idt = {0u, 0u};
    __asm__ volatile ("lidt %0; int3"
        : : "m"(empty_idt) : "memory");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void zero_memory(void *memory, size_t size)
{
    uint8_t *bytes = memory;
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0u;
    }
}

static void copy_string(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0u) {
        return;
    }
    size_t i = 0u;
    while (i + 1u < capacity && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static bool ascii_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        char a = *left++;
        char b = *right++;
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (a != b) {
            return false;
        }
    }
    return *left == *right;
}

static char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text;
}

static void console_clear(void);

static void serial_init(void)
{
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x80u);
    outb(COM1, 0x03u);
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x03u);
    outb(COM1 + 2u, 0xc7u);
    outb(COM1 + 4u, 0x0bu);
    serial_available = inb(COM1 + 5u) != 0xffu;
}

static bool serial_ready(void)
{
    return serial_available && (inb(COM1 + 5u) & 1u) != 0u;
}

static void serial_putc(char value)
{
    book8_boot_log_char(value);
    if (!serial_available) {
        return;
    }
    for (uint32_t attempt = 0u; attempt < SERIAL_TX_WAIT_LIMIT; ++attempt) {
        if ((inb(COM1 + 5u) & 0x20u) != 0u) {
            outb(COM1, (uint8_t)value);
            return;
        }
        __asm__ volatile ("pause");
    }
}

static void serial_write(const char *text)
{
    while (*text != '\0') {
        serial_putc(*text++);
    }
}

void book8_usb_debug_serial_putc(char value)
{
    serial_putc(value);
}

static uint32_t pack_rgb(uint32_t color)
{
    return fb_pack(&fbdev, color);
}

long book8_framebuffer_info(struct book8_fb_info *info)
{
    if (screen.pixels == NULL) {
        return -BOOK8_ENODEV;
    }
    info->width = screen.width;
    info->height = screen.height;
    info->pitch = screen.pitch_bytes;
    info->bits_per_pixel = screen.bytes_per_pixel * 8u;
    info->red_position = screen.red_position;
    info->red_mask_size = screen.red_mask_size;
    info->green_position = screen.green_position;
    info->green_mask_size = screen.green_mask_size;
    info->blue_position = screen.blue_position;
    info->blue_mask_size = screen.blue_mask_size;
    info->reserved = 0u;
    return 0;
}

long book8_framebuffer_draw_pixel(uint64_t x, uint64_t y, uint32_t rgb)
{
    if (screen.pixels == NULL) {
        return -BOOK8_ENODEV;
    }
    if (x >= screen.width || y >= screen.height) {
        return -BOOK8_EINVAL;
    }
    fb_put_pixel(&fbdev, (int32_t)x, (int32_t)y, pack_rgb(rgb));
    return 0;
}

static void draw_char_colored(char value, uint32_t x, uint32_t y,
    uint32_t foreground, uint32_t background)
{
    fb_draw_char(&fbdev, (int32_t)x, (int32_t)y, value,
        pack_rgb(foreground), pack_rgb(background));
}

static void draw_char(char value, uint32_t x, uint32_t y)
{
    draw_char_colored(value, x, y, 0xffffffu, 0u);
}

static void gui_fill_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t color)
{
    if (screen.pixels == NULL || x >= screen.width || y >= screen.height) {
        return;
    }
    if (fbdev.draw == fbdev.front && nv_graphics != NULL
        && nv_graphics->abi_version == 1u && nv_graphics->fill_rect != NULL
        && nv_graphics->fill_rect(&nv_gpu, x, y, width, height,
            pack_rgb(color)) == 0) {
            return;
    }
    fb_fill_rect(&fbdev, (int32_t)x, (int32_t)y, width, height,
        pack_rgb(color));
}

static void gui_draw_rgba(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const uint8_t *pixels, uint32_t stride, uint32_t scale)
{
    if (screen.pixels == NULL || pixels == NULL || scale == 0u
        || width > 0xffffffffu / 4u
        || stride < width * 4u) {
        return;
    }
    for (uint32_t sy = 0u; sy < height; ++sy) {
        uint64_t dy = (uint64_t)y + (uint64_t)sy * scale;
        if (dy >= screen.height) {
            break;
        }
        const uint8_t *source = pixels + (uint64_t)sy * stride;
        for (uint32_t sx = 0u; sx < width; ++sx) {
            uint64_t dx = (uint64_t)x + (uint64_t)sx * scale;
            if (dx >= screen.width) {
                break;
            }
            const uint8_t *pixel = source + sx * 4u;
            uint32_t alpha = pixel[3];
            uint32_t red = (pixel[0] * alpha + 255u * (255u - alpha)) / 255u;
            uint32_t green = (pixel[1] * alpha + 255u * (255u - alpha)) / 255u;
            uint32_t blue = (pixel[2] * alpha + 255u * (255u - alpha)) / 255u;
            uint32_t packed = pack_rgb((red << 16u) | (green << 8u) | blue);
            for (uint32_t yy = 0u; yy < scale && dy + yy < screen.height; ++yy) {
                for (uint32_t xx = 0u; xx < scale && dx + xx < screen.width; ++xx) {
                    uint8_t *target = fbdev.draw
                        + (dy + yy) * fbdev.pitch
                        + (dx + xx) * fbdev.bytes;
                    for (uint32_t byte = 0u; byte < screen.bytes_per_pixel; ++byte) {
                        target[byte] = (uint8_t)(packed >> (byte * 8u));
                    }
                }
            }
        }
    }
}

static void gui_draw_text(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background)
{
    while (*text != '\0' && x < screen.width) {
        if (*text == '\n') {
            y += CELL_HEIGHT;
            x = 0u;
        } else if (*text == '\r') {
            x = 0u;
        } else {
            draw_char_colored(*text, x, y, foreground, background);
            x += CELL_WIDTH;
        }
        ++text;
    }
}

static void gui_draw_text_scaled(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background, uint32_t scale, bool bold)
{
    if (scale < 1u) {
        scale = 1u;
    } else if (scale > 4u) {
        scale = 4u;
    }
    uint32_t packed = pack_rgb(foreground);
    for (; *text != '\0' && x < screen.width; ++text, x += 8u * scale) {
        uint32_t code = (uint8_t)*text;
        if (code < FB_FONT_FIRST || code >= FB_FONT_FIRST + FB_FONT_GLYPHS) {
            code = '?';
        }
        const uint8_t *glyph = fb_font8x16[code - FB_FONT_FIRST];
        gui_fill_rect(x, y, 8u * scale, 16u * scale, background);
        for (uint32_t row = 0u; row < 16u; ++row) {
            uint32_t bits = glyph[row];
            if (bold) {
                bits |= bits >> 1u;
            }
            uint32_t col = 0u;
            while (col < 8u) {
                if (((bits >> (7u - col)) & 1u) == 0u) {
                    ++col;
                    continue;
                }
                uint32_t run = 1u;
                while (col + run < 8u
                    && ((bits >> (7u - (col + run))) & 1u) != 0u) {
                    ++run;
                }
                fb_fill_rect(&fbdev, (int32_t)(x + col * scale),
                    (int32_t)(y + row * scale), run * scale, scale, packed);
                col += run;
            }
        }
    }
}

static bool gui_read_file(const char *path, void *destination,
    uint32_t capacity, uint32_t *size)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_read_file(path, destination, capacity, size);
}

static bool gui_write_file(const char *path, const void *data, uint32_t size)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_write_file(path, data, size);
}

static void scroll_screen(void)
{
    if (screen.pixels != NULL) {
        uint32_t rows = screen.height / CELL_HEIGHT;
        if (rows > 1u) {
            fb_scroll_up(&fbdev, CELL_HEIGHT, 0u);
            cursor_y = rows - 1u;
        }
    } else if (use_vga) {
        for (uint32_t i = 0; i < (VGA_ROWS - 1u) * VGA_COLUMNS; ++i) {
            VGA_MEMORY[i] = VGA_MEMORY[i + VGA_COLUMNS];
        }
        for (uint32_t i = (VGA_ROWS - 1u) * VGA_COLUMNS; i < VGA_ROWS * VGA_COLUMNS; ++i) {
            VGA_MEMORY[i] = 0x0720u;
        }
        cursor_y = VGA_ROWS - 1u;
    }
}

static void console_clear(void)
{
    if (gui_active) {
        book8gui_console_clear();
        return;
    }
    if (screen.pixels != NULL) {
        fb_clear(&fbdev, 0u);
    } else if (use_vga) {
        for (uint32_t i = 0; i < VGA_COLUMNS * VGA_ROWS; ++i) {
            VGA_MEMORY[i] = 0x0720u;
        }
    }
    cursor_x = 0u;
    cursor_y = 0u;
    vga_cursor = 0u;
    if (!gui_active) {
        fb_present(&fbdev);
    }
}

static void console_present(void)
{
    if (!gui_active) {
        fb_present(&fbdev);
    }
}

static void console_putc(char value)
{
    if (startup_quiet && !gui_active) {
        if (value == '\n') {
            book8_boot_log_char('\r');
        }
        book8_boot_log_char(value);
        return;
    }

    if (value == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    } else {
        serial_putc(value);
    }

    if (gui_active) {
        book8gui_console_char(value);
        return;
    }

    if (value == '\n') {
        cursor_x = 0u;
        ++cursor_y;
    } else if (value == '\r') {
        cursor_x = 0u;
    } else if (value == '\b') {
        if (cursor_x > 0u) {
            --cursor_x;
            if (screen.pixels != NULL) {
                draw_char(' ', cursor_x * CELL_WIDTH, cursor_y * CELL_HEIGHT);
            } else if (use_vga) {
                vga_cursor = (uint16_t)(cursor_y * VGA_COLUMNS + cursor_x);
                VGA_MEMORY[vga_cursor] = 0x0720u;
            }
        }
    } else {
        if (screen.pixels != NULL) {
            draw_char(value, cursor_x * CELL_WIDTH, cursor_y * CELL_HEIGHT);
        } else if (use_vga) {
            vga_cursor = (uint16_t)(cursor_y * VGA_COLUMNS + cursor_x);
            VGA_MEMORY[vga_cursor] = (uint16_t)(0x0700u | (uint8_t)value);
        }
        ++cursor_x;
    }

    uint32_t max_columns = screen.pixels != NULL
        ? screen.width / CELL_WIDTH : (use_vga ? VGA_COLUMNS : 80u);
    uint32_t max_rows = screen.pixels != NULL
        ? screen.height / CELL_HEIGHT : (use_vga ? VGA_ROWS : 25u);
    if (max_columns == 0u || max_rows == 0u) {
        return;
    }
    if (cursor_x >= max_columns) {
        cursor_x = 0u;
        ++cursor_y;
    }
    if (cursor_y >= max_rows) {
        scroll_screen();
    }
    if (value == '\n' && !console_write_active) {
        console_present();
    }
}

static void console_write(const char *text)
{
    bool has_newline = false;
    console_write_active = true;
    while (*text != '\0') {
        if (*text == '\n') {
            has_newline = true;
        }
        control_log_char(*text);
        console_putc(*text++);
    }
    console_write_active = false;
    if (has_newline) {
        console_present();
    }
}

static void print_unsigned(uint64_t value)
{
    char digits[21];
    size_t count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count > 0u) {
        console_putc(digits[--count]);
    }
}

static void print_signed(int32_t value)
{
    if (value < 0) {
        console_putc('-');
        print_unsigned((uint32_t)(-(value + 1)) + 1u);
    } else {
        print_unsigned((uint32_t)value);
    }
}

static void print_hex8(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    console_putc(digits[value >> 4u]);
    console_putc(digits[value & 0x0fu]);
}

static void print_hex32(uint32_t value)
{
    print_hex8((uint8_t)(value >> 24u));
    print_hex8((uint8_t)(value >> 16u));
    print_hex8((uint8_t)(value >> 8u));
    print_hex8((uint8_t)value);
}

static void print_hex64(uint64_t value)
{
    print_hex32((uint32_t)(value >> 32u));
    print_hex32((uint32_t)value);
}

static uint32_t pci_legacy_read32(uint8_t bus, uint8_t slot, uint8_t function,
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

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return book8_pci_read32(bus, slot, function, offset);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    uint32_t value = pci_read32(bus, slot, function, offset);
    return (uint16_t)(value >> ((offset & 2u) * 8u));
}

static void pci_legacy_write16(uint8_t bus, uint8_t slot, uint8_t function,
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

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint16_t value)
{
    book8_pci_write16(bus, slot, function, offset, value);
}

static void pci_get_device(struct pci_device *device, uint8_t bus,
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

static void ide_enable_bus_master(const struct pci_device *device)
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

static void ide_set_channel_controller(const struct pci_device *device)
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

/* Class detection is not evidence of an operational network/audio service. */
static bool pci_network_present;
static bool pci_sound_present;

static void discover_pci_controllers(void)
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

/* NVIDIA Maxwell (GTX 970/980) foundation.  Probing only reads PCI config
 * space and BAR0's PMC_BOOT_0; no engine is touched, so a failed or absent
 * GPU leaves the software framebuffer path unchanged. */
extern uint64_t boot_pd0[512];
extern uint64_t boot_pd1[512];
extern uint64_t boot_pd2[512];
extern uint64_t boot_pd3[512];

static void pci_legacy_write32(uint8_t bus, uint8_t slot,
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

static void driver_pci_write32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint32_t value)
{
    (void)ctx;
    book8_pci_write32(bus, slot, function, offset, value);
}

/* boot.S identity maps 0..4 GiB with 2 MiB write-back pages; register windows
 * must be PWT|PCD (uncached).  Save each original PDE so unmap can restore it
 * instead of blindly clearing PWT|PCD that another mapping may still need. */
#define DRIVER_MMIO_MAP_MAX 64u

struct driver_mmio_record {
    uint64_t page;          /* 2 MiB page frame number */
    uint64_t original_pde;  /* PDE before the first uncached map */
    uint32_t refs;          /* 0 == free slot */
};

static struct driver_mmio_record driver_mmio_records[DRIVER_MMIO_MAP_MAX];

static uint64_t *driver_mmio_pde(uint64_t page)
{
    uint64_t directory_index = page / 512u;
    uint64_t entry_index = page % 512u;
    uint64_t *directory = directory_index == 0u ? boot_pd0
        : directory_index == 1u ? boot_pd1
        : directory_index == 2u ? boot_pd2 : boot_pd3;
    return &directory[entry_index];
}

static struct driver_mmio_record *driver_mmio_find(uint64_t page)
{
    for (uint32_t i = 0u; i < DRIVER_MMIO_MAP_MAX; ++i) {
        if (driver_mmio_records[i].refs != 0u
            && driver_mmio_records[i].page == page) {
            return &driver_mmio_records[i];
        }
    }
    return NULL;
}

static struct driver_mmio_record *driver_mmio_free_slot(void)
{
    for (uint32_t i = 0u; i < DRIVER_MMIO_MAP_MAX; ++i) {
        if (driver_mmio_records[i].refs == 0u) {
            return &driver_mmio_records[i];
        }
    }
    return NULL;
}

static void driver_mmio_reload_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0\n\tmov %0, %%cr3"
        : "=r"(cr3) :: "memory");
}

static volatile uint8_t *driver_map_mmio(void *ctx, uint64_t phys, uint64_t size,
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

static void driver_unmap_mmio(void *ctx, const volatile void *pointer,
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

static void pcie_log(const char *text);

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

static const b8_sym_export_t b8_kernel_hal_exports[] = {
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

static const uint8_t *driver_trust_digest(const char *path)
{
    for (size_t i = 0u;
        i < sizeof(book8_driver_trust_table)
            / sizeof(book8_driver_trust_table[0]); ++i) {
        if (ascii_equal(path, book8_driver_trust_table[i].path))
            return book8_driver_trust_table[i].digest;
    }
    return NULL;
}

static int32_t load_trusted_driver(const char *path,
    struct b8_loaded_driver *loaded)
{
    const uint8_t *digest = driver_trust_digest(path);
    if (digest == NULL)
        return -BOOK8_EPERM;
    int32_t status = b8_driver_load(path, digest, b8_kernel_hal_exports,
        sizeof(b8_kernel_hal_exports) / sizeof(b8_kernel_hal_exports[0]),
        loaded);
    char message[112];
    if (status == 0) {
        (void)snprintf(message, sizeof(message),
            "BOOK8-DRIVER: loaded %s\r\n", path);
    } else {
        (void)snprintf(message, sizeof(message),
            "BOOK8-DRIVER: load failed %s status=%ld\r\n", path,
            (long)status);
    }
    serial_write(message);
    return status;
}

static bool usb_host_module_load(void)
{
    int32_t status = load_trusted_driver("A:\\DRIVERS\\USBHOST.DRV",
        &usb_host_driver);
    if (status != 0)
        return false;
    const struct book8_usb_module_api *api = NULL;
    status = usb_host_driver.descriptor->ioctl(BOOK8_USB_IOCTL_GET_API, &api);
    if (status != 0 || api == NULL
        || api->abi_version != BOOK8_USB_MODULE_API_VERSION
        || api->struct_size < sizeof(*api)) {
        serial_write("BOOK8-USB: module API validation failed\r\n");
        b8_driver_unload(&usb_host_driver);
        return false;
    }
    usb_host_api = api;
    return true;
}

static bool hda_module_load(void)
{
    int32_t status = load_trusted_driver("A:\\DRIVERS\\AC97.DRV",
        &hda_driver);
    if (status != 0)
        return false;
    if (hda_driver.descriptor->ioctl == NULL) {
        serial_write("BOOK8-HDA: module API ioctl unavailable\r\n");
        b8_driver_unload(&hda_driver);
        return false;
    }
    const struct b8_hda_module_api *api = NULL;
    status = hda_driver.descriptor->ioctl(B8_HDA_IOCTL_GET_API, &api);
    if (status != 0 || api == NULL
        || api->abi_version != B8_HDA_MODULE_API_VERSION
        || api->struct_size < sizeof(*api)
        || api->get_device == NULL || api->is_available == NULL
        || api->get_volume == NULL || api->get_mute == NULL
        || api->set_volume == NULL || api->set_mute == NULL
        || api->play_wav == NULL || api->pause == NULL
        || api->stop == NULL || api->seek == NULL
        || api->get_stream_info == NULL || api->service == NULL
        || api->status_name == NULL || api->stream_result_name == NULL) {
        serial_write("BOOK8-HDA: module API validation failed\r\n");
        b8_driver_unload(&hda_driver);
        return false;
    }
    hda_api = api;
    hda_api->get_device(&hda_controller);
    return true;
}

static bool usb_api_available(void)
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

static volatile uint8_t *pcie_map(uint64_t phys, uint64_t size)
{
    return driver_map_mmio(NULL, phys, size, true);
}

static void pcie_log(const char *text)
{
    console_write(text);
    console_write("\n");
    if (pcie_event_tail - pcie_event_head < PCIE_BOOT_EVENTS) {
        snprintf(pcie_boot_events[pcie_event_tail % PCIE_BOOT_EVENTS],
            BOOK8_TELEMETRY_LOG_SIZE, "%s", text);
        ++pcie_event_tail;
    } else {
        if (pcie_event_dropped == 0u)
            console_write("PCIE1 ERROR boot telemetry queue full; events lost\n");
        ++pcie_event_dropped;
    }
}

static bool pcie_delay_ms(uint32_t milliseconds)
{
    if (tsc_hz == 0u) {
        console_write("PCIE1 ERROR D0 delay requires a calibrated clock\n");
        return false;
    }
    uint64_t start = read_tsc();
    uint64_t ticks = tsc_hz / 1000u * milliseconds;
    while (read_tsc() - start < ticks) {
        __asm__ volatile ("pause");
    }
    return true;
}

static void pcie_bootstrap(void)
{
    const struct book8_pcie_platform platform = {
        .legacy_read = pci_legacy_read32,
        .legacy_write = pci_legacy_write32,
        .legacy_write16 = pci_legacy_write16,
        .map = pcie_map, .log = pcie_log, .delay_ms = pcie_delay_ms,
        .read32_safe = book8_read32_safe, .write32_safe = book8_write32_safe,
        .irq_save = irq_save, .irq_restore = irq_restore
    };
    interrupt_table_initialize();
    book8_pcie_init(&pcie, &platform);
    pcie_log("PCIE1 INIT after core CPU setup; early events buffered until UDP is ready");
    pcie_log("PCIE1 PMC SKIP unverified MSR c0012121; no PMC prerequisite");
}

#ifndef BOOK8_NV_BIND_CHANNEL
/* Opt-in (-DBOOK8_NV_BIND_CHANNEL=1): programs PFIFO/BAR1 of a real GPU. */
#define BOOK8_NV_BIND_CHANNEL 0
#endif

static bool nv_maxwell_bus_probe(struct book8_bus_device *device)
{
    struct book8_pcie_node *node = device->private_data;
    uint32_t command;

    if (node == NULL || node->segment != 0u
        || device->mapped_mmio == NULL)
        return false;
    command = book8_pcie_read32(&pcie, node->segment, node->bus,
        node->slot, node->function, 4u);
    return command != UINT32_MAX && (command & 6u) == 6u;
}

static void nv_maxwell_bus_remove(struct book8_bus_device *device)
{
    if (device->bus == nv_gpu.bus && device->device == nv_gpu.slot
        && device->function == nv_gpu.function) {
        const struct book8_pcie_node *node = device->private_data;
        if (node != NULL) {
            struct b8_driver_device driver_device = {
                .vendor_id = device->vendor_id,
                .device_id = device->device_id,
                .bus = device->bus,
                .device = device->device,
                .function = device->function,
                .programming_interface = (uint8_t)device->class_code,
                .class_code = device->class_code,
                .bar0_base = node->bars[0].base,
                .bar1_base = node->bars[1].base,
            };
            b8_driver_remove(&nv_driver, &driver_device, &nv_gpu);
        }
        nv_gpu = (struct nv_gpu){0};
    }
}

static bool usb_hid_bus_probe(struct book8_bus_device *device)
{
    return (device->class_code >> 16u) == 3u;
}

static bool usb_storage_bus_probe(struct book8_bus_device *device)
{
    return (device->class_code >> 16u) == 8u;
}

static bool usb_hub_bus_probe(struct book8_bus_device *device)
{
    return (device->class_code >> 16u) == 9u;
}

static bool usb_host_bus_probe(struct book8_bus_device *device)
{
    uint8_t programming_interface = (uint8_t)device->class_code;
    return (device->class_code & 0xffff00u) == 0x0c0300u
        && (programming_interface == 0x00u
            || programming_interface == 0x10u
            || programming_interface == 0x20u);
}

static void disable_bus_master(struct book8_bus_device *device)
{
    uint32_t command;

    if (device->bus_type != BOOK8_BUS_PCI || device->segment != 0u) {
        return;
    }
    command = book8_pcie_read32(&pcie, device->segment, device->bus,
        device->device, device->function, 4u);
    if (command != UINT32_MAX) {
        book8_pcie_write16(&pcie, device->segment, device->bus,
            device->device, device->function, 4u,
            (uint16_t)(command & ~(1u << 2u)));
    }
}

static bool rtl8168_bus_probe(struct book8_bus_device *device);
static void rtl8168_bus_remove(struct book8_bus_device *device);
static bool hda_bus_probe(struct book8_bus_device *device);
static void hda_bus_remove(struct book8_bus_device *device);
static bool nvidia_hda_bus_probe(struct book8_bus_device *device);

static const struct book8_bus_match nv_maxwell_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = NV_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = NV_DEVICE_GTX980, .device_mask = 0xffffu,
        .class_code = 0x030000u, .class_mask = 0xff0000u},
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = NV_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = NV_DEVICE_GTX970, .device_mask = 0xffffu,
        .class_code = 0x030000u, .class_mask = 0xff0000u},
};

static const struct book8_bus_match usb_hid_matches[] = {
    {.bus_type = BOOK8_BUS_USB, .class_code = 0x030000u,
        .class_mask = 0xff0000u},
};

static const struct book8_bus_match usb_storage_matches[] = {
    {.bus_type = BOOK8_BUS_USB, .class_code = 0x080000u,
        .class_mask = 0xff0000u},
};

static const struct book8_bus_match usb_hub_matches[] = {
    {.bus_type = BOOK8_BUS_USB, .class_code = 0x090000u,
        .class_mask = 0xff0000u},
};

static const struct book8_bus_match usb_host_matches[] = {
    {.bus_type = BOOK8_BUS_PCI, .class_code = 0x0c0300u,
        .class_mask = 0xffff00u},
};

static const struct book8_bus_match rtl8168_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = RTL8168_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = RTL8168_DEVICE_ID, .device_mask = 0xffffu,
        .class_code = 0x020000u, .class_mask = 0xff0000u},
};

static const struct book8_bus_match hda_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = HDA_ATI_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = HDA_ATI_DEVICE_ID, .device_mask = 0xffffu,
        .class_code = 0x040300u, .class_mask = 0xffff00u},
};

static const struct book8_bus_match nvidia_hda_matches[] = {
    {.bus_type = BOOK8_BUS_PCI,
        .vendor_id = NV_VENDOR_ID, .vendor_mask = 0xffffu,
        .device_id = 0x0fbbu, .device_mask = 0xffffu,
        .class_code = 0x040300u, .class_mask = 0xffff00u},
};

static const struct book8_bus_driver bus_drivers[] = {
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

static struct rtl8168_device rtl_nic;
static struct rtl8168_dma rtl_dma;

static struct book8_net kernel_net;
static bool network_ready;
static bool initialization_complete;
static uint32_t telemetry_target;
static uint64_t telemetry_last_tsc;
static void telemetry_service(void);
static void push_agent_service(void);
static inline uint64_t read_tsc(void);
static uint32_t netboot_info_address;
/* Driver-loop timing for telemetry v2 (reset after each sample). */
static uint32_t poll_iterations;
static uint64_t poll_tsc_total;
static uint64_t poll_tsc_max;
static bool push_executing;

struct book8_net *book8_kernel_network(void)
{
    return network_ready ? &kernel_net : NULL;
}

static enum book8_net_result network_send(void *ctx, const void *frame, size_t size)
{
    (void)ctx;
    if (rtl8168_driver.descriptor == NULL
        || rtl8168_driver.descriptor->write == NULL)
        return BOOK8_NET_IO_ERROR;
    int32_t status = rtl8168_driver.descriptor->write(0u, frame, size);
    if (status == 0) {
        return BOOK8_NET_OK;
    }
    console_write("BOOK8-NET: TX ");
    struct b8_rtl8168_status_text text = {
        .status = (uint32_t)(status < 0 ? -status : status)
    };
    if (rtl8168_driver.descriptor->ioctl != NULL
        && rtl8168_driver.descriptor->ioctl(B8_RTL8168_IOCTL_STATUS_TEXT,
            &text) == 0 && text.text != NULL)
        console_write(text.text);
    else
        console_write("driver error");
    console_write("\n");
    return text.status == RTL8168_TX_BUSY
        ? BOOK8_NET_BUSY : BOOK8_NET_IO_ERROR;
}

static enum book8_net_result network_receive(void *ctx, void *frame,
    size_t capacity, size_t *size)
{
    (void)ctx;
    if (rtl8168_driver.descriptor == NULL
        || rtl8168_driver.descriptor->read == NULL)
        return BOOK8_NET_IO_ERROR;
    int32_t result = rtl8168_driver.descriptor->read(0u, frame, capacity);
    if (result > 0) {
        *size = (size_t)result;
        return BOOK8_NET_OK;
    }
    if (result == 0) {
        return BOOK8_NET_EMPTY;
    }
    enum rtl8168_status status = (enum rtl8168_status)-result;
    if (status == RTL8168_RX_DROPPED || status == RTL8168_BUFFER_SMALL) {
        return BOOK8_NET_RX_DROPPED;
    }
    console_write("BOOK8-NET: RX ");
    struct b8_rtl8168_status_text text = {.status = (uint32_t)status};
    if (rtl8168_driver.descriptor->ioctl != NULL
        && rtl8168_driver.descriptor->ioctl(B8_RTL8168_IOCTL_STATUS_TEXT,
            &text) == 0 && text.text != NULL)
        console_write(text.text);
    else
        console_write("driver error");
    console_write("\n");
    return BOOK8_NET_IO_ERROR;
}

static void network_rx_poll(void)
{
    if (network_ready) {
        if (!network_enabled_service())
            return;
        uint64_t started = read_tsc();
        enum book8_net_result result = book8_net_poll(&kernel_net);
        uint64_t spent = read_tsc() - started;
        ++poll_iterations;
        poll_tsc_total += spent;
        if (spent > poll_tsc_max) {
            poll_tsc_max = spent;
        }
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-NET: poll ");
            console_write(book8_net_result_string(result));
            console_write("\n");
        }
        network_dhcp_service();
        telemetry_service();
        push_agent_service();
    }
}

static void rtl8168_report_status(const char *operation, int32_t status)
{
    struct b8_rtl8168_status_text text = {
        .status = (uint32_t)(status < 0 ? -status : status)
    };
    console_write("BOOK8-RTL8168: ");
    console_write(operation);
    console_write(": ");
    if (rtl8168_driver.descriptor != NULL
        && rtl8168_driver.descriptor->ioctl != NULL
        && rtl8168_driver.descriptor->ioctl(B8_RTL8168_IOCTL_STATUS_TEXT,
            &text) == 0 && text.text != NULL) {
        console_write(text.text);
    } else {
        print_signed(status);
    }
    console_write("\n");
}

static bool rtl8168_bus_probe(struct book8_bus_device *device)
{
    struct book8_pcie_node *node = device->private_data;
    if (node == NULL || node->segment != 0u
        || rtl8168_driver.descriptor == NULL) {
        return false;
    }
    if (!rtl_nic.running) {
        const struct b8_driver_device module_device = {
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .bus = device->bus,
            .device = device->device,
            .function = device->function,
            .class_code = device->class_code,
            .bar0_base = node->bars[0].base,
            .bar1_base = node->bars[1].base,
        };
        int32_t status = b8_driver_probe(&rtl8168_driver, &module_device,
            &rtl_nic);
        if (status != 0) {
            rtl8168_report_status("probe/init failed", status);
            return false;
        }
        status = rtl8168_driver.descriptor->ioctl(
            B8_RTL8168_IOCTL_START, &rtl_dma);
        if (status != 0) {
            rtl8168_report_status("start failed", status);
            b8_driver_remove(&rtl8168_driver, &module_device, &rtl_nic);
            return false;
        }
        console_write("BOOK8-RTL8168: PCI ");
        print_unsigned(rtl_nic.bus);
        console_putc(':');
        print_unsigned(rtl_nic.slot);
        console_putc('.');
        print_unsigned(rtl_nic.function);
        console_write(" vendor/device=10EC:8168\n");
        console_write("BOOK8-RTL8168: MAC=");
        for (uint8_t i = 0u; i < 6u; ++i) {
            if (i != 0u)
                console_putc(':');
            print_hex8(rtl_nic.mac[i]);
        }
        console_write("\nBOOK8-RTL8168: polling TX/RX ready; 8 descriptors per ring\n");
        const struct book8_net_transport transport = {
            .ctx = &rtl_nic, .send = network_send, .receive = network_receive
        };
        enum book8_net_result result = book8_net_init(&kernel_net,
            &transport, rtl_nic.mac);
        network_ready = result == BOOK8_NET_OK;
        console_write("BOOK8-NET: ");
        console_write(network_ready ? "stack ready; IPv4 unconfigured"
            : book8_net_result_string(result));
        console_write("\n");
    }
    if (!rtl_nic.present || !rtl_nic.running
        || rtl_nic.bus != node->bus || rtl_nic.slot != node->slot
        || rtl_nic.function != node->function) {
        return false;
    }
    device->mapped_mmio = rtl_nic.regs;
    device->mmio_base = 0u;
    device->mmio_size = 0u;
    for (uint8_t bar = 0u; bar < 6u; ++bar) {
        if (rtl_nic.bars[bar].mapped == rtl_nic.regs) {
            device->mmio_base = rtl_nic.bars[bar].phys;
            device->mmio_size = rtl_nic.bars[bar].size;
            node->bars[bar].mapped = true;
            node->bars[bar].mapped_size = rtl_nic.bars[bar].size;
            node->bars[bar].cache_policy = BOOK8_PCIE_CACHE_UC;
            break;
        }
    }
    return device->mmio_size != 0u;
}

static void rtl8168_bus_remove(struct book8_bus_device *device)
{
    if (rtl_nic.bus != device->bus || rtl_nic.slot != device->device
        || rtl_nic.function != device->function) {
        return;
    }
    struct b8_driver_device module_device = {
        .vendor_id = device->vendor_id,
        .device_id = device->device_id,
        .bus = device->bus,
        .device = device->device,
        .function = device->function,
    };
    b8_driver_remove(&rtl8168_driver, &module_device, &rtl_nic);
    network_ready = false;
    kernel_net = (struct book8_net){0};
}

static bool hda_bus_probe(struct book8_bus_device *device)
{
    struct book8_pcie_node *node = device->private_data;
    if (node == NULL || node->segment != 0u || hda_api == NULL
        || hda_driver.descriptor == NULL) {
        return false;
    }
    if (!hda_controller.present) {
        const struct b8_driver_device module_device = {
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .bus = device->bus,
            .device = device->device,
            .function = device->function,
            .class_code = device->class_code,
            .bar0_base = node->bars[0].base,
        };
        int32_t status = b8_driver_probe(&hda_driver, &module_device, NULL);
        if (status != 0) {
            serial_write("BOOK8-HDA: probe failed: ");
            serial_write(hda_api->status_name(-status));
            serial_write("\r\n");
            return false;
        }
        hda_api->get_device(&hda_controller);
    }
    if (hda_controller.bus != node->bus
        || hda_controller.slot != node->slot
        || hda_controller.function != node->function
        || hda_controller.regs == NULL) {
        return false;
    }
    device->mapped_mmio = hda_controller.regs;
    device->mmio_base = hda_controller.mmio_base;
    device->mmio_size = 0x4000u;
    node->bars[0].mapped = true;
    node->bars[0].mapped_size = device->mmio_size;
    node->bars[0].cache_policy = BOOK8_PCIE_CACHE_UC;
    return true;
}

static void hda_bus_remove(struct book8_bus_device *device)
{
    if (hda_controller.bus != device->bus
        || hda_controller.slot != device->device
        || hda_controller.function != device->function) {
        return;
    }
    disable_bus_master(device);
    if (hda_api != NULL && hda_driver.descriptor != NULL) {
        const struct b8_driver_device module_device = {
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .bus = device->bus,
            .device = device->device,
            .function = device->function,
            .class_code = device->class_code,
        };
        b8_driver_remove(&hda_driver, &module_device, NULL);
    }
    hda_controller = (struct hda_device){0};
}

static bool nvidia_hda_bus_probe(struct book8_bus_device *device)
{
    return device->bus_type == BOOK8_BUS_PCI
        && device->vendor_id == NV_VENDOR_ID
        && device->device_id == 0x0fbbu
        && (device->class_code & 0xffff00u) == 0x040300u;
}

static void nv_gpu_init(void)
{
    nv_graphics = NULL;
    nv_gpu = (struct nv_gpu){0};
    (void)book8_pcie_bind_bus_drivers(&pcie, bus_drivers,
        sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    int32_t status = load_trusted_driver("A:\\DRIVERS\\NVMAXWEL.DRV",
        &nv_driver);
    if (status != 0) {
        console_write("NVDRV: trusted module unavailable; status=");
        print_signed(status);
        console_putc('\n');
        char status_text[64];
        (void)snprintf(status_text,
            sizeof(status_text),
            "NVDRV: trusted module unavailable; status=%ld",
            (long)status);
        serial_write(status_text);
        serial_write("\r\n");
        return;
    }
    if (nv_driver.descriptor->graphics != NULL) {
        nv_graphics = nv_driver.descriptor->graphics();
    }
    pcie_log("NVDRV: module digest verified, symbols resolved and init complete");
    serial_write("NVDRV: module digest verified, symbols resolved and init complete\r\n");
    bool matched_device = false;
    for (size_t i = 0u; i < pcie.node_count; ++i) {
        const struct book8_pcie_node *node = &pcie.nodes[i];
        const b8_driver_t *descriptor = nv_driver.descriptor;
        const struct book8_bus_device *bus_device = &node->bus_device;
        if (node->segment != 0u || bus_device->bus_type != BOOK8_BUS_PCI
            || (descriptor->vendor_id != 0u
                && bus_device->vendor_id != descriptor->vendor_id)
            || (descriptor->product_id != 0u
                && bus_device->device_id != descriptor->product_id)
            || (descriptor->pci_class != 0u
                && (uint8_t)(bus_device->class_code >> 16u)
                    != descriptor->pci_class)
            || (bus_device->driver != &bus_drivers[0]))
            continue;
        if (node->id == BOOK8_PCIE_TARGET_ID
            && (!pcie.target_ready || node->bus != 1u || node->slot != 0u
                || node->function != 0u))
            continue;
        matched_device = true;
        if (node->id == BOOK8_PCIE_TARGET_ID
            && (node->bars[0].base != NV_GM204_BAR0_PHYS
                || node->bars[1].base != NV_GM204_BAR1_PHYS)) {
            pcie_log("PCIE1 ERROR GM204 verified BAR0/BAR1 base mismatch; GPU MMIO blocked");
            continue;
        }
        struct b8_driver_device device = {
            .vendor_id = bus_device->vendor_id,
            .device_id = bus_device->device_id,
            .bus = bus_device->bus,
            .device = bus_device->device,
            .function = bus_device->function,
            .programming_interface = (uint8_t)bus_device->class_code,
            .class_code = bus_device->class_code,
            .bar0_base = node->bars[0].base,
            .bar1_base = node->bars[1].base,
        };
        status = b8_driver_probe(&nv_driver, &device, &nv_gpu);
        if (status == 0) {
            pcie_log("NVDRV: supported PCI device initialized");
            break;
        }
        console_write("NVDRV: matching device probe failed; status=");
        print_signed(status);
        console_putc('\n');
    }
    if (!matched_device) {
        pcie_log("NVDRV: module loaded and initialized; no matching GPU found");
        serial_write("NVDRV: module loaded and initialized; no matching GPU found\r\n");
    }
    if (!nv_gpu.present && pcie.target_present && !pcie.target_ready) {
        for (size_t i = 0u; i < pcie.node_count; ++i) {
            const struct book8_pcie_node *node = &pcie.nodes[i];

            if (node->id != BOOK8_PCIE_TARGET_ID || node->segment != 0u
                || node->bus != 1u || node->slot != 0u
                || node->function != 0u)
                continue;
            nv_gpu = (struct nv_gpu){.present = true,
                .device_id = NV_DEVICE_GTX970, .bus = node->bus,
                .slot = node->slot, .function = node->function,
                .bar0_phys = node->bars[0].base,
                .bar1_phys = node->bars[1].base};
            return;
        }
    }
}

static void record_nv_gpu_bars(void)
{
    if (!nv_gpu.present) {
        return;
    }
    for (size_t i = 0u; i < pcie.node_count; ++i) {
        struct book8_pcie_node *node = &pcie.nodes[i];
        if (node->segment != 0u || node->bus != nv_gpu.bus
            || node->slot != nv_gpu.slot
            || node->function != nv_gpu.function) {
            continue;
        }
        if (nv_gpu.bar0 != NULL) {
            node->bars[0].mapped = true;
            node->bars[0].mapped_size = nv_gpu.bar0_size;
            node->bars[0].cache_policy = BOOK8_PCIE_CACHE_UC;
            node->bus_device.mapped_mmio = nv_gpu.bar0;
            node->bus_device.mmio_base = nv_gpu.bar0_phys;
            node->bus_device.mmio_size = nv_gpu.bar0_size;
        }
        if (nv_gpu.bar1 != NULL) {
            node->bars[1].mapped = true;
            node->bars[1].mapped_size = nv_gpu.bar1_size;
            node->bars[1].cache_policy = BOOK8_PCIE_CACHE_WB;
        }
        return;
    }
}

static void nv_driver_refresh_state(void)
{
    if (nv_driver.descriptor != NULL
        && nv_driver.descriptor->refresh != NULL)
        nv_driver.descriptor->refresh(&nv_gpu);
}

static bool nv_registry_sync(void)
{
    static const char path[] = "HKLM\\HARDWARE\\DEVICEMAP\\VIDEO\\NVIDIA0";

    if (!nv_gpu.present || !nv_gpu.mapped) {
        return true;
    }
    nv_driver_refresh_state();
    return reg_set_str(path, "Arch", "GM204")
        && reg_set_u32(path, "ChipRev", nv_gpu.chip_rev)
        && reg_set_u32(path, "PMC_BOOT_0", nv_gpu.boot0)
        && reg_set_u32(path, "PMC_ENABLE", nv_gpu.pmc_enable)
        && reg_set_u32(path, "DetectedVRAM_MB", nv_gpu.detected_vram_mb)
        && reg_set_u32(path, "ScanoutBase",
            nv_gpu.scanout.valid ? (uint32_t)nv_gpu.scanout.physical_base : 0u)
        && reg_set_u32(path, "BAR0_UC", (uint32_t)nv_gpu.bar0_phys)
        && reg_set_u32(path, "BAR1_WB", (uint32_t)nv_gpu.bar1_phys);
}

static uint8_t ide_status(struct ide_channel *channel)
{
    return inb(channel->command_base + 7u);
}

static void ide_delay(struct ide_channel *channel)
{
    (void)inb(channel->control_base + 2u);
    (void)inb(channel->control_base + 2u);
    (void)inb(channel->control_base + 2u);
    (void)inb(channel->control_base + 2u);
}

static bool ide_wait_ready(struct ide_channel *channel)
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

static bool ide_wait_data(struct ide_channel *channel)
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

static bool ide_identify(struct ide_channel *channel, uint8_t drive,
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

static uint64_t identify_sector_count(const uint16_t identify[256],
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

static void identify_model(char model[41], const uint16_t identify[256])
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

static struct disk_device *add_disk(const char *name, const uint16_t identify[256],
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

static bool ide_read_sector(struct ide_channel *channel, uint8_t drive,
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

static bool ide_write_sector(struct ide_channel *channel, uint8_t drive,
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

/* Issues one command on slot 0 through the AHCI driver. data points at
 * identity-mapped memory and is transferred as data_bytes (1..4 MiB) when
 * has_data is set. */
static bool ahci_run(struct ahci_port *port, uint8_t command,
    uint64_t lba, uint16_t sector_count, void *data, uint32_t data_bytes,
    bool write_to_disk, bool has_data)
{
    return ahci_port_issue(port, command, lba, sector_count, data,
        has_data ? data_bytes : 0u, write_to_disk);
}

static bool ahci_issue(struct ahci_port *port, uint8_t command,
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

/* Multi-sector READ/WRITE DMA EXT through the shared bounce buffer. */
static bool ahci_transfer(struct ahci_port *port, uint64_t lba,
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

static bool ahci_read_sector(struct ahci_port *port, uint64_t lba,
    uint8_t destination[512])
{
    if (lba >= (1ull << 48u)) {
        return false;
    }
    return ahci_issue(port, 0x25u, lba, 1u, destination, false, true);
}

static bool ahci_identify(struct ahci_port *port, uint16_t identify[256])
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

static bool ahci_write_sector(struct ahci_port *port, uint64_t lba,
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

static void discover_ide_disks(void)
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

static void discover_ahci_disks(void)
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

static void discover_storage(void)
{
    discover_pci_controllers();
    discover_ide_disks();
    discover_ahci_disks();
}

static uint64_t pit_clock_ticks(void);
static uint64_t disk_read_bytes_total;
static uint64_t disk_write_bytes_total;
static uint64_t disk_busy_ticks_total;

/* Disk activity counters feed the Task Manager; timing only runs while the
 * desktop owns the PIT clock. */
static uint64_t disk_io_begin(void)
{
    return gui_active ? pit_clock_ticks() : 0u;
}

static bool disk_io_end(bool write, bool ok, uint64_t started)
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

static bool disk_read_sector(struct disk_device *disk, uint64_t lba,
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

static bool disk_write_sector(struct disk_device *disk, uint64_t lba,
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

static bool disk_write_sector_unflushed(struct disk_device *disk, uint64_t lba,
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

static bool disk_flush(struct disk_device *disk)
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

static bool disk_dma_channel(const struct disk_device *disk,
    struct ata_dma_channel *out)
{
    const struct ide_channel *channel = &ide_channels[disk->channel];

    out->command_base = channel->command_base;
    out->control_base = channel->control_base;
    out->bus_master_base = channel->bus_master_base;
    return disk->dma;
}

static bool storage_quiet;

static bool dma_report_failure(const struct disk_device *disk, bool write,
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

/* One hardware command for up to ATA_BOUNCE_SECTORS sectors; no fallback. */
static bool disk_transfer_hw(struct disk_device *disk, uint64_t lba,
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

static bool disk_transfer(struct disk_device *disk, uint64_t lba,
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

static bool block_adapter_read(block_device_t *dev, uint32_t start_lba,
    uint32_t sector_count, void *buf)
{
    return disk_transfer(dev->priv, start_lba, sector_count, buf, false);
}

static bool block_adapter_write(block_device_t *dev, uint32_t start_lba,
    uint32_t sector_count, const void *buf)
{
    return disk_transfer(dev->priv, start_lba, sector_count,
        (uint8_t *)(uintptr_t)buf, true);
}

/* Boot-time check: a multi-sector DMA read must match single-sector PIO reads. */
static void storage_selftest(struct disk_device *disk)
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

#define SCRATCH_BASE_LBA 64u
#define SCRATCH_FULL_SECTORS 200u
#define SCRATCH_MIN_SECTORS (SCRATCH_BASE_LBA + SCRATCH_FULL_SECTORS + 8u)

static uint8_t scratch_source[SCRATCH_FULL_SECTORS * 512u];
static uint8_t scratch_verify[SCRATCH_FULL_SECTORS * 512u];

/* Mirrored by tests/qemu_storage_test.py when it checks the image afterwards. */
static void scratch_pattern(uint8_t *buf, uint32_t sectors, uint8_t seed)
{
    for (uint32_t i = 0u; i < sectors * 512u; ++i) {
        buf[i] = (uint8_t)(i * 131u + (i >> 9u) * 7u + seed);
    }
}

static bool scratch_equal(const uint8_t *a, const uint8_t *b, uint32_t sectors)
{
    for (uint32_t i = 0u; i < sectors * 512u; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static bool scratch_set_guard(struct disk_device *disk, uint32_t lba,
    uint8_t fill)
{
    uint8_t sector[512];

    for (uint32_t i = 0u; i < 512u; ++i) {
        sector[i] = fill;
    }
    return disk_write_sector(disk, lba, sector);
}

/* Guards are always checked through the PIO path, independent of DMA. */
static bool scratch_guard_ok(struct disk_device *disk, uint32_t lba,
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

static bool scratch_check_pio(struct disk_device *disk, uint32_t lba,
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

static const char *storage_write_phases(struct disk_device *disk)
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

/* Destructive: runs only on a disk whose sector 0 starts with "BOOK8SCRATCH". */
static void storage_write_selftest(struct disk_device *disk)
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

/* Publish every discovered IDE/AHCI disk through the hardware-neutral layer. */
static bool application_read(block_device_t *dev, uint32_t lba,
    uint32_t count, void *buffer)
{
    (void)dev;
    if ((uint64_t)lba + count > application_image_size / 512u) { return false; }
    __builtin_memcpy(buffer, application_image + (uint64_t)lba * 512u, (size_t)count * 512u);
    return true;
}

static bool application_write(block_device_t *dev, uint32_t lba,
    uint32_t count, const void *buffer)
{
    (void)dev;
    if ((uint64_t)lba + count > application_image_size / 512u) { return false; }
    __builtin_memcpy(application_image + (uint64_t)lba * 512u, buffer, (size_t)count * 512u);
    return true;
}

static void register_block_devices(void)
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
static bool filesystem_read_sector(uint32_t disk, uint64_t lba,
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

static bool filesystem_write_sector(uint32_t disk, uint64_t lba,
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

static bool registry_write_file(const char *path, const void *data,
    uint32_t size)
{
    return registry_write_allowed && fs_write_file(path, data, size);
}

static uint64_t filesystem_disk_size(uint32_t disk)
{
    if (application_image != NULL) {
        if (disk == 0u) { return application_image_size / 512u; }
        --disk;
    }
    return disk < disk_count ? disks[disk].sectors : 0u;
}

static void filesystem_print(const char *text)
{
    console_write(text);
}

static void print_disk_inventory(void)
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

static uint8_t cmos_read(uint8_t register_index)
{
    outb(0x70u, register_index);
    return inb(0x71u);
}

struct clock_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

static uint8_t bcd_to_binary(uint8_t value)
{
    return (uint8_t)((value & 0x0fu) + (value >> 4u) * 10u);
}

static struct clock_time read_clock(void)
{
    struct clock_time clock;
    while ((cmos_read(0x0au) & 0x80u) != 0u) {
    }
    uint8_t mode = cmos_read(0x0bu);
    clock.second = cmos_read(0x00u);
    clock.minute = cmos_read(0x02u);
    uint8_t hour = cmos_read(0x04u);
    clock.day = cmos_read(0x07u);
    clock.month = cmos_read(0x08u);
    uint8_t year = cmos_read(0x09u);
    uint8_t century = cmos_read(0x32u);
    if ((mode & 0x04u) == 0u) {
        clock.second = bcd_to_binary(clock.second);
        clock.minute = bcd_to_binary(clock.minute);
        clock.day = bcd_to_binary(clock.day);
        clock.month = bcd_to_binary(clock.month);
        year = bcd_to_binary(year);
        century = bcd_to_binary(century);
        hour = (uint8_t)((hour & 0x80u) | bcd_to_binary(hour & 0x7fu));
    }
    if ((mode & 0x02u) == 0u) {
        bool pm = (hour & 0x80u) != 0u;
        hour &= 0x7fu;
        hour = (uint8_t)(hour % 12u + (pm ? 12u : 0u));
    }
    clock.hour = hour;
    clock.year = century >= 19u && century <= 99u
        ? (uint16_t)(century * 100u + year)
        : (uint16_t)(2000u + year);
    return clock;
}

static void print_two_digits(uint8_t value)
{
    console_putc((char)('0' + value / 10u));
    console_putc((char)('0' + value % 10u));
}

static char keyboard_char(uint8_t scan)
{
    static const char keys[58] = {
        0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted_keys[58] = {
        0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
        '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (scan == 0x2au || scan == 0x36u) {
        shifted = true;
        return 0;
    }
    if (scan == 0xaau || scan == 0xb6u) {
        shifted = false;
        return 0;
    }
    if ((scan & 0x80u) != 0u || scan >= sizeof(keys)) {
        return 0;
    }
    return shifted ? shifted_keys[scan] : keys[scan];
}

static bool usb_report_has_key(const struct book8_usb_keyboard_report *report,
    uint8_t key)
{
    for (uint32_t i = 0u; i < sizeof(report->keys); ++i) {
        if (report->keys[i] == key) {
            return true;
        }
    }
    return false;
}

/* Adds BOOK8GUI_KEY_CTRL/SHIFT flags where the key contract allows them. */
static uint32_t gui_key_with_modifiers(uint32_t key, bool ctrl, bool shift)
{
    if (key >= 0x20u && key <= 0x7eu) {
        if (!ctrl) {
            return key;
        }
        if (key >= 'A' && key <= 'Z') {
            key += 'a' - 'A';
        }
        return key | BOOK8GUI_KEY_CTRL | (shift ? BOOK8GUI_KEY_SHIFT : 0u);
    }
    if ((key >= BOOK8GUI_KEY_UP && key <= BOOK8GUI_KEY_F12)
        || key == BOOK8GUI_KEY_TAB) {
        return key | (ctrl ? BOOK8GUI_KEY_CTRL : 0u)
            | (shift ? BOOK8GUI_KEY_SHIFT : 0u);
    }
    return key;
}

static bool usb_queue_key(uint32_t key)
{
    uint8_t next = (uint8_t)((usb_key_queue_tail + 1u)
        % USB_KEY_QUEUE_SIZE);
    if (next == usb_key_queue_head) {
        return false;
    }
    usb_key_queue[usb_key_queue_tail] = key;
    usb_key_queue_tail = next;
    return true;
}

static uint32_t usb_key_value(uint8_t usage, uint8_t modifiers)
{
    bool shift = (modifiers & 0x22u) != 0u;
    if (usage >= 0x04u && usage <= 0x1du) {
        bool uppercase = shift != ((usb_keyboard_leds & 0x02u) != 0u);
        return (uint32_t)(uppercase
            ? 'A' + usage - 0x04u : 'a' + usage - 0x04u);
    }
    if (usage >= 0x1eu && usage <= 0x27u) {
        static const char digits[] = "1234567890";
        static const char shifted_digits[] = "!@#$%^&*()";
        uint32_t index = usage - 0x1eu;
        return (uint8_t)(shift ? shifted_digits[index] : digits[index]);
    }
    switch (usage) {
    case 0x28u: return BOOK8GUI_KEY_ENTER;
    case 0x29u: return BOOK8GUI_KEY_ESCAPE;
    case 0x2au: return BOOK8GUI_KEY_BACKSPACE;
    case 0x2bu: return BOOK8GUI_KEY_TAB;
    case 0x2cu: return ' ';
    case 0x2du: return shift ? '_' : '-';
    case 0x2eu: return shift ? '+' : '=';
    case 0x2fu: return shift ? '{' : '[';
    case 0x30u: return shift ? '}' : ']';
    case 0x31u: return shift ? '|' : '\\';
    case 0x32u: return shift ? '~' : '#';
    case 0x33u: return shift ? ':' : ';';
    case 0x34u: return shift ? '"' : '\'';
    case 0x35u: return shift ? '~' : '`';
    case 0x36u: return shift ? '<' : ',';
    case 0x37u: return shift ? '>' : '.';
    case 0x38u: return shift ? '?' : '/';
    case 0x39u: return 0u;
    case 0x51u: return BOOK8GUI_KEY_DOWN;
    case 0x52u: return BOOK8GUI_KEY_UP;
    case 0x50u: return BOOK8GUI_KEY_LEFT;
    case 0x4fu: return BOOK8GUI_KEY_RIGHT;
    case 0x4cu: return BOOK8GUI_KEY_DELETE;
    case 0x4au: return BOOK8GUI_KEY_HOME;
    case 0x4du: return BOOK8GUI_KEY_END;
    case 0x4bu: return BOOK8GUI_KEY_PAGE_UP;
    case 0x4eu: return BOOK8GUI_KEY_PAGE_DOWN;
    default:
        if (usage >= 0x3au && usage <= 0x45u) {
            return BOOK8GUI_KEY_F1 + (usage - 0x3au);
        }
        return 0u;
    }
}

static void usb_keyboard_update(void)
{
    struct book8_usb_keyboard_report report;
    if (!book8_usb_poll_keyboard(&report)) {
        return;
    }
    for (uint32_t i = 0u; i < sizeof(report.keys); ++i) {
        uint8_t usage = report.keys[i];
        if (usage == 0u || usage == 1u
            || usb_report_has_key(&usb_previous_keyboard_report, usage)) {
            continue;
        }
        uint8_t led_mask = usage == 0x39u ? 0x02u
            : usage == 0x53u ? 0x01u
            : usage == 0x47u ? 0x04u : 0u;
        if (led_mask != 0u) {
            usb_keyboard_leds ^= led_mask;
            continue;
        }
        uint32_t key = usb_key_value(usage, report.modifiers);
        if (key != 0u) {
            key = gui_key_with_modifiers(key,
                (report.modifiers & 0x11u) != 0u,
                (report.modifiers & 0x22u) != 0u);
            if (key == BOOK8GUI_KEY_ESCAPE
                && (report.modifiers & 0x11u) != 0u
                && (report.modifiers & 0x44u) != 0u) {
                key |= BOOK8GUI_KEY_CTRL | BOOK8GUI_KEY_ALT;
            }
            (void)usb_queue_key(key);
        }
    }
    usb_previous_keyboard_report = report;
}

static bool usb_keyboard_get_key(uint32_t *key)
{
    usb_keyboard_update();
    if (usb_key_queue_head == usb_key_queue_tail) {
        return false;
    }
    *key = usb_key_queue[usb_key_queue_head];
    usb_key_queue_head = (uint8_t)((usb_key_queue_head + 1u)
        % USB_KEY_QUEUE_SIZE);
    return true;
}

static bool ps2_wait_input_empty(void)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        if ((inb(0x64u) & 2u) == 0u) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool ps2_wait_output_full(uint8_t *status)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t current = inb(0x64u);
        if ((current & 1u) != 0u) {
            *status = current;
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static void ps2_flush_output(void);

static bool ps2_read_command_byte(uint8_t *command_byte)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }
    /* A pending keyboard byte (e.g. a key release) would otherwise be read
     * back as the command byte and then written to the controller. */
    ps2_flush_output();
    outb(0x64u, 0x20u);
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t status = inb(0x64u);
        if ((status & 1u) != 0u) {
            uint8_t value = inb(0x60u);
            if ((status & 0x20u) == 0u) {
                *command_byte = value;
                return true;
            }
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool ps2_keyboard_initialize(void)
{
    if (inb(0x64u) == 0xffu) {
        return false;
    }
    uint8_t command_byte;
    if (!ps2_read_command_byte(&command_byte)) {
        return false;
    }
    command_byte = (uint8_t)((command_byte & ~0x10u) | 0x40u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0x60u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x60u, command_byte);

    uint8_t verified_command_byte;
    return ps2_read_command_byte(&verified_command_byte)
        && (verified_command_byte & 0x50u) == 0x40u;
}

static void ps2_flush_output(void)
{
    for (uint32_t i = 0u; i < 32u && (inb(0x64u) & 1u) != 0u; ++i) {
        (void)inb(0x60u);
        for (uint32_t d = 0u; d < 2000u; ++d) {
            __asm__ volatile ("pause");
        }
    }
}

static bool ps2_write_command_byte(uint8_t value)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0x60u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x60u, value);
    return ps2_wait_input_empty();
}

/* Sends one byte to the auxiliary device and waits for 0xFA. Some Super I/O
 * and USB-legacy-emulation controllers omit the AUX status bit, so only the
 * data value is checked; 0xFE (resend) is retried. */
static bool ps2_mouse_send(uint8_t value)
{
    for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
        uint8_t status;
        if (!ps2_wait_input_empty()) {
            return false;
        }
        outb(0x64u, 0xd4u);
        if (!ps2_wait_input_empty()) {
            return false;
        }
        outb(0x60u, value);
        if (!ps2_wait_output_full(&status)) {
            return false;
        }
        uint8_t reply = inb(0x60u);
        if (reply == 0xfau) {
            return true;
        }
        if (reply != 0xfeu) {
            return false;
        }
    }
    return false;
}

static bool ps2_mouse_read_id(uint8_t *device_id)
{
    uint8_t status;
    if (!ps2_mouse_send(0xf2u) || !ps2_wait_output_full(&status)) {
        return false;
    }
    *device_id = inb(0x60u);
    return true;
}

static bool ps2_mouse_set_sample_rate(uint8_t rate)
{
    return ps2_mouse_send(0xf3u) && ps2_mouse_send(rate);
}

static bool ps2_mouse_initialize(void)
{
    uint8_t command_byte;
    if (inb(0x64u) == 0xffu) {
        return false;
    }
    ps2_flush_output();
    /* Park the aux port, flush, then enable it and clear the aux clock
     * disable bit (5) in the controller command byte. IRQ12 (bit 1) stays
     * as-is; ps2_mouse_irq_enable() manages it. */
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0xa7u);
    ps2_flush_output();
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0xa8u);
    if (!ps2_read_command_byte(&command_byte)) {
        return false;
    }
    command_byte = (uint8_t)(command_byte & ~0x20u);
    if (!ps2_write_command_byte(command_byte)) {
        return false;
    }
    ps2_flush_output();

    bool ready = false;
    for (uint32_t attempt = 0u; attempt < 3u && !ready; ++attempt) {
        uint8_t status;
        /* Reset: FA, AA, 00. Absent or slow devices fall through to F6/F4. */
        if (ps2_mouse_send(0xffu)) {
            for (uint32_t i = 0u; i < 2u; ++i) {
                if (!ps2_wait_output_full(&status)) {
                    break;
                }
                (void)inb(0x60u);
            }
        }
        ps2_flush_output();
        ready = ps2_mouse_send(0xf6u) && ps2_mouse_send(0xf4u);
    }
    if (!ready) {
        return false;
    }
    ps2_mouse_packet_length = 3u;
    ps2_mouse_id = 0u;
    uint8_t mouse_id = 0u;
    if (ps2_mouse_send(0xf5u)
        && ps2_mouse_set_sample_rate(200u)
        && ps2_mouse_set_sample_rate(100u)
        && ps2_mouse_set_sample_rate(80u)
        && ps2_mouse_read_id(&mouse_id)
        && (mouse_id == 3u || mouse_id == 4u)) {
        ps2_mouse_packet_length = 4u;
        ps2_mouse_id = mouse_id;
    }
    if (!ps2_mouse_send(0xf4u)) {
        console_write("PS/2 mouse reporting could not be enabled.\n");
        return false;
    }
    ps2_mouse_packet_index = 0u;
    pointer_buttons = 0u;
    pointer_x = screen.width != 0u ? (int32_t)(screen.width / 2u) : 0;
    pointer_y = screen.height != 0u ? (int32_t)(screen.height / 2u) : 0;
    ps2_mouse_available = true;
    return true;
}

static bool ps2_mouse_set_irq_mode(bool enabled)
{
    uint8_t command_byte;
    if (!ps2_read_command_byte(&command_byte)) {
        return false;
    }
    if (enabled) {
        command_byte = (uint8_t)((command_byte | 0x02u) & ~0x20u);
    } else {
        command_byte = (uint8_t)(command_byte & ~0x02u);
    }
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0x60u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x60u, command_byte);
    uint8_t verified;
    return ps2_read_command_byte(&verified)
        && ((verified & 0x02u) != 0u) == enabled;
}

static bool ps2_mouse_irq_enable(void)
{
    uint64_t flags = irq_save();

    if (!ps2_mouse_available) {
        irq_restore(flags);
        return false;
    }
    interrupt_table_initialize();
    if (!ps2_mouse_set_irq_mode(true)) {
        irq_restore(flags);
        return false;
    }
    mouse_event_head = 0u;
    mouse_event_tail = 0u;
    mouse_event_last_buttons = pointer_buttons;
    mouse_event_last_coalescible = false;
    outb(0x21u, 0xfau);
    outb(0xa1u, 0xefu);
    ps2_mouse_irq_enabled = true;
    __asm__ volatile ("sti" : : : "memory");
    return true;
}

static bool ps2_mouse_irq_disable(void)
{
    uint64_t flags = irq_save();
    bool disabled;

    outb(0x21u, 0xfeu);
    outb(0xa1u, 0xffu);
    ps2_mouse_irq_enabled = false;
    ps2_mouse_packet_index = 0u;
    disabled = ps2_mouse_set_irq_mode(false);
    irq_restore(flags);
    return disabled;
}

static bool ps2_mouse_feed(uint8_t data, struct book8gui_event *event)
{
    if (!ps2_mouse_available) {
        return false;
    }
    if (ps2_mouse_packet_index == 0u && (data & 0x08u) == 0u) {
        return false;
    }
    ps2_mouse_packet[ps2_mouse_packet_index++] = data;
    if (ps2_mouse_packet_index < ps2_mouse_packet_length) {
        return false;
    }
    ps2_mouse_packet_index = 0u;
    if ((ps2_mouse_packet[0] & 0xc0u) != 0u) {
        return false;
    }
    ++ps2_mouse_packet_count;
    pointer_x += book8_pointer_delta((int8_t)ps2_mouse_packet[1], &pointer_remainder_x);
    pointer_y += book8_pointer_delta(-(int32_t)(int8_t)ps2_mouse_packet[2], &pointer_remainder_y);
    if (pointer_x < 0) {
        pointer_x = 0;
    } else if ((uint32_t)pointer_x >= screen.width) {
        pointer_x = (int32_t)(screen.width - 1u);
    }
    if (pointer_y < 0) {
        pointer_y = 0;
    } else if ((uint32_t)pointer_y >= screen.height) {
        pointer_y = (int32_t)(screen.height - 1u);
    }
    pointer_buttons = ps2_mouse_packet[0] & 0x07u;
    event->wheel_delta = 0;
    if (ps2_mouse_packet_length == 4u) {
        event->wheel_delta = book8_ps2_wheel(ps2_mouse_packet[3], ps2_mouse_id);
        if (ps2_mouse_id == 4u) {
            pointer_buttons |= (uint8_t)((ps2_mouse_packet[3] & 0x30u) >> 1u);
        }
    }
    event->type = BOOK8GUI_EVENT_MOUSE;
    event->key = 0u;
    event->mouse_x = pointer_x;
    event->mouse_y = pointer_y;
    event->mouse_buttons = book8_pointer_buttons(pointer_buttons);
    return true;
}

static bool mouse_event_enqueue(const struct book8gui_event *event, bool usb)
{
    uint64_t flags = irq_save();
    uint8_t head = mouse_event_head;
    bool button_transition =
        event->mouse_buttons != mouse_event_last_buttons;

    if (!button_transition && event->wheel_delta == 0
        && mouse_event_last_coalescible && head != mouse_event_tail) {
        uint8_t last = (uint8_t)((head - 1u)
            & (MOUSE_EVENT_QUEUE_SIZE - 1u));
        if (mouse_events[last].wheel_delta == 0
            && mouse_events[last].mouse_buttons == event->mouse_buttons) {
            mouse_events[last] = *event;
            mouse_event_last_buttons = event->mouse_buttons;
            if (usb) {
                ++usb_mouse_queue_events;
            } else {
                ++ps2_mouse_queue_events;
            }
            irq_restore(flags);
            return true;
        }
    }

    uint8_t next = (uint8_t)((head + 1u) & (MOUSE_EVENT_QUEUE_SIZE - 1u));

    if (next == mouse_event_tail) {
        if (usb) {
            ++usb_mouse_queue_drops;
        } else {
            ++ps2_mouse_queue_drops;
        }
        irq_restore(flags);
        return false;
    }
    mouse_events[head] = *event;
    mouse_event_head = next;
    mouse_event_last_buttons = event->mouse_buttons;
    mouse_event_last_coalescible =
        !button_transition && event->wheel_delta == 0;
    if (usb) {
        ++usb_mouse_queue_events;
    } else {
        ++ps2_mouse_queue_events;
    }
    irq_restore(flags);
    return true;
}

static bool mouse_event_dequeue(struct book8gui_event *event)
{
    uint64_t flags = irq_save();
    uint8_t tail = mouse_event_tail;

    if (tail == mouse_event_head) {
        irq_restore(flags);
        return false;
    }
    *event = mouse_events[tail];
    mouse_event_tail = (uint8_t)((tail + 1u) & (MOUSE_EVENT_QUEUE_SIZE - 1u));
    irq_restore(flags);
    return true;
}

void book8_ps2_mouse_irq_dispatch(void)
{
    uint8_t status = inb(0x64u);

    ++ps2_mouse_irq_count;
    if ((status & 1u) != 0u && (status & 0x20u) != 0u) {
        struct book8gui_event event;
        if (ps2_mouse_feed(inb(0x60u), &event)) {
            (void)mouse_event_enqueue(&event, false);
        }
    }
    outb(0xa0u, 0x20u);
    outb(0x20u, 0x20u);
    book8_apic_eoi();
}

static void gui_pump_input(void)
{
    network_rx_poll();
    fs_space_poll();
    if (speaker_stop != 0u && read_tsc() >= speaker_stop) {
        outb(0x61u, (uint8_t)(inb(0x61u) & ~3u));
        speaker_stop = 0u;
    }
    if (!ps2_mouse_irq_enabled && ps2_mouse_available) {
        for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
            uint8_t status = inb(0x64u);
            if ((status & 1u) == 0u || (status & 0x20u) == 0u) {
                break;
            }
            struct book8gui_event event;
            if (ps2_mouse_feed(inb(0x60u), &event)) {
                (void)mouse_event_enqueue(&event, false);
            }
        }
    }

    struct book8_usb_mouse_report usb_report;
    for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
        if (!book8_usb_poll_mouse(&usb_report)) {
            break;
        }
        ++usb_mouse_report_count;
        struct book8gui_event usb_event;
        uint64_t flags = irq_save();
        if (usb_report.absolute) {
            int64_t x_range = (int64_t)usb_report.logical_max_x
                - usb_report.logical_min_x;
            int64_t y_range = (int64_t)usb_report.logical_max_y
                - usb_report.logical_min_y;
            int64_t x_value = usb_report.x;
            int64_t y_value = usb_report.y;
            if (x_value < usb_report.logical_min_x) {
                x_value = usb_report.logical_min_x;
            } else if (x_value > usb_report.logical_max_x) {
                x_value = usb_report.logical_max_x;
            }
            if (y_value < usb_report.logical_min_y) {
                y_value = usb_report.logical_min_y;
            } else if (y_value > usb_report.logical_max_y) {
                y_value = usb_report.logical_max_y;
            }
            pointer_x = x_range > 0
                ? (int32_t)(((uint64_t)(x_value - usb_report.logical_min_x)
                    * (screen.width - 1u)) / (uint64_t)x_range)
                : 0;
            pointer_y = y_range > 0
                ? (int32_t)(((uint64_t)(y_value - usb_report.logical_min_y)
                    * (screen.height - 1u)) / (uint64_t)y_range)
                : 0;
        } else {
            int64_t next_x = (int64_t)pointer_x
                + book8_pointer_delta(usb_report.x, &pointer_remainder_x);
            /* HID +Y is toward the user (down); screen Y also grows down. */
            int64_t next_y = (int64_t)pointer_y
                + book8_pointer_delta(usb_report.y, &pointer_remainder_y);
            pointer_x = next_x < 0 ? 0
                : next_x >= screen.width ? (int32_t)(screen.width - 1u)
                : (int32_t)next_x;
            pointer_y = next_y < 0 ? 0
                : next_y >= screen.height ? (int32_t)(screen.height - 1u)
                : (int32_t)next_y;
        }
        pointer_buttons = usb_report.buttons;
        usb_event.type = BOOK8GUI_EVENT_MOUSE;
        usb_event.key = 0u;
        usb_event.mouse_x = pointer_x;
        usb_event.mouse_y = pointer_y;
        usb_event.mouse_buttons = book8_pointer_buttons(pointer_buttons);
        usb_event.wheel_delta = usb_report.wheel_delta;
        irq_restore(flags);
        (void)mouse_event_enqueue(&usb_event, true);
    }
}

static bool gui_poll_event(struct book8gui_event *event)
{
    if (serial_ready()) {
        uint8_t value = inb(COM1);
        event->type = BOOK8GUI_EVENT_KEY;
        event->key = value == '\r' || value == '\n'
            ? BOOK8GUI_KEY_ENTER : (value == '\b' ? BOOK8GUI_KEY_BACKSPACE
                : (value == 0x1bu ? BOOK8GUI_KEY_ESCAPE
                    : (value == '\t' ? BOOK8GUI_KEY_TAB : value)));
        return true;
    }

    uint32_t usb_key;
    if (usb_keyboard_get_key(&usb_key)) {
        event->type = BOOK8GUI_EVENT_KEY;
        event->key = usb_key;
        return true;
    }

    if (mouse_event_dequeue(event)) {
        if (event->wheel_delta != 0) {
            uint32_t buttons = event->mouse_buttons;
            uintptr_t wparam = (uintptr_t)(uint16_t)event->wheel_delta << 16u;
            wparam |= (buttons & 3u) | ((buttons & 4u) << 2u)
                | ((buttons & 24u) << 2u);
            (void)book8gui_decode_wheel_message(event, BOOK8_WM_MOUSEWHEEL, wparam,
                event->mouse_x, event->mouse_y);
            console_write("BOOK8-MOUSE: X=");
            print_signed(event->mouse_x);
            console_write(" Y=");
            print_signed(event->mouse_y);
            console_write(" WHEEL=");
            print_signed(event->wheel_delta / BOOK8_WHEEL_DELTA);
            console_write(" WHEEL_DELTA=");
            print_signed(event->wheel_delta);
            console_write(" BUTTONS=");
            print_unsigned(event->mouse_buttons);
            console_putc('\n');
        }
        return true;
    }

    static bool extended_scan = false;
    for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
        uint8_t status = inb(0x64u);
        if ((status & 1u) == 0u) {
            break;
        }
        if ((status & 0x20u) != 0u && ps2_mouse_irq_enabled) {
            continue;
        }
        uint8_t scan = inb(0x60u);
        if ((status & 0x20u) != 0u) {
            if (ps2_mouse_feed(scan, event)) {
                (void)mouse_event_enqueue(event, false);
            }
            continue;
        }
        if (scan == 0xe0u) {
            extended_scan = true;
            continue;
        }
        if (extended_scan) {
            extended_scan = false;
            if (scan == 0x1du || scan == 0x9du) {
                ps2_ctrl_down = scan == 0x1du;
                continue;
            }
            if (scan == 0x38u || scan == 0xb8u) {
                ps2_alt_down = scan == 0x38u;
                continue;
            }
            uint32_t extended_key = 0u;
            switch (scan) {
            case 0x48u: extended_key = BOOK8GUI_KEY_UP; break;
            case 0x50u: extended_key = BOOK8GUI_KEY_DOWN; break;
            case 0x4bu: extended_key = BOOK8GUI_KEY_LEFT; break;
            case 0x4du: extended_key = BOOK8GUI_KEY_RIGHT; break;
            case 0x53u: extended_key = BOOK8GUI_KEY_DELETE; break;
            case 0x47u: extended_key = BOOK8GUI_KEY_HOME; break;
            case 0x4fu: extended_key = BOOK8GUI_KEY_END; break;
            case 0x49u: extended_key = BOOK8GUI_KEY_PAGE_UP; break;
            case 0x51u: extended_key = BOOK8GUI_KEY_PAGE_DOWN; break;
            default: break;
            }
            if (extended_key != 0u) {
                event->type = BOOK8GUI_EVENT_KEY;
                event->key = gui_key_with_modifiers(extended_key,
                    ps2_ctrl_down, shifted);
                return true;
            }
            continue;
        }
        if (scan == 0x1du || scan == 0x9du) {
            ps2_ctrl_down = scan == 0x1du;
            continue;
        }
        if (scan == 0x38u || scan == 0xb8u) {
            ps2_alt_down = scan == 0x38u;
            continue;
        }
        uint32_t function_key = 0u;
        if (scan >= 0x3bu && scan <= 0x44u) {
            function_key = BOOK8GUI_KEY_F1 + (scan - 0x3bu);
        } else if (scan == 0x57u || scan == 0x58u) {
            function_key = BOOK8GUI_KEY_F11 + (scan - 0x57u);
        }
        if (function_key != 0u) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = gui_key_with_modifiers(function_key,
                ps2_ctrl_down, shifted);
            return true;
        }
        if (scan == 0x01u) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = ps2_ctrl_down && ps2_alt_down
                ? (BOOK8GUI_KEY_ESCAPE | BOOK8GUI_KEY_CTRL
                    | BOOK8GUI_KEY_ALT)
                : BOOK8GUI_KEY_ESCAPE;
            return true;
        }
        if (scan == 0x1cu) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = BOOK8GUI_KEY_ENTER;
            return true;
        }
        if (scan == 0x0eu) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = BOOK8GUI_KEY_BACKSPACE;
            return true;
        }
        if (scan == 0x0fu) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = gui_key_with_modifiers(BOOK8GUI_KEY_TAB,
                ps2_ctrl_down, shifted);
            return true;
        }
        char value = keyboard_char(scan);
        if (value != '\0') {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = gui_key_with_modifiers((uint8_t)value,
                ps2_ctrl_down, shifted);
            return true;
        }
    }
    return mouse_event_dequeue(event);
}

#define PIT_HZ 1193182u
#define GUI_BACKBUFFER_BYTES (10u * 1024u * 1024u)

static uint8_t gui_backbuffer[GUI_BACKBUFFER_BYTES] __attribute__((aligned(4096)));

/* PIT channel 0 free-runs in rate-generator mode with a 65536 reload.
 * The GUI polls it independently of IRQ0. Each sample accumulates the elapsed
 * counts, so it must be called more often than every ~54 ms. */
static void pit_clock_start(void)
{
    outb(0x43u, 0x34u);
    outb(0x40u, 0u);
    outb(0x40u, 0u);
}

static uint64_t pit_clock_ticks(void)
{
    static uint64_t total;
    static uint16_t last;
    outb(0x43u, 0x00u);
    uint16_t count = inb(0x40u);
    count = (uint16_t)(count | ((uint16_t)inb(0x40u) << 8u));
    total += (uint16_t)(last - count);
    last = count;
    return total;
}

static void scheduler_test(bool user_mode)
{
    if (gui_active) {
        console_write("SCHEDTEST: unavailable while the desktop is active\n");
        return;
    }
    uint64_t flags = irq_save();
    interrupt_table_initialize();
    uint8_t master_mask = inb(0x21u);
    uint8_t slave_mask = inb(0xa1u);
    outb(0x21u, 0xfeu);
    outb(0xa1u, 0xffu);
    pit_clock_start();
    if (user_mode && tsc_hz == 0u) {
        (void)pit_clock_ticks();
        tsc_calibrate();
        if (tsc_hz == 0u) {
            outb(0x21u, master_mask);
            outb(0xa1u, slave_mask);
            irq_restore(flags);
            console_write("SCHEDUSER: clock calibration failed\n");
            return;
        }
    }
    if (!exec_scheduler_test_start()) {
        outb(0x21u, master_mask);
        outb(0xa1u, slave_mask);
        irq_restore(flags);
        console_write("SCHEDTEST: unavailable while a task is active\n");
        return;
    }
    long status = 0;
    if (user_mode)
        status = syscall_scheduler_smoke(exec_scheduler_test_current(), tsc_hz);
    else {
        while (exec_scheduler_test_ticks() < 12u)
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }
    bool user_switched = !user_mode || exec_scheduler_test_user_switches() >= 2u;
    bool passed = exec_scheduler_test_finish() && user_switched;
    outb(0x21u, master_mask);
    outb(0xa1u, slave_mask);
    irq_restore(flags);
    if (user_mode)
        console_write(passed && status == 0 ? "SCHEDUSER: PASS\n" : "SCHEDUSER: FAIL\n");
    else
        console_write(passed ? "SCHEDTEST: PASS\n" : "SCHEDTEST: FAIL\n");
}

static volatile uint64_t smp_test_completed;
static volatile uint32_t smp_test_errors;
static volatile uint32_t smp_test_cpu_runs[BOOK8_SMP_MAX_CPUS];

static void smp_test_worker(void *argument)
{
    uint32_t expected_cpu = (uint32_t)(uintptr_t)argument;
    uint32_t actual_cpu = book8_smp_current_cpu_index();
    if (actual_cpu != expected_cpu)
        __atomic_add_fetch(&smp_test_errors, 1u, __ATOMIC_RELAXED);
    for (uint32_t i = 0u; i < 50000u; ++i)
        __atomic_add_fetch(&smp_test_cpu_runs[expected_cpu], 1u,
            __ATOMIC_RELAXED);
    __atomic_fetch_or(&smp_test_completed, 1ull << expected_cpu,
        __ATOMIC_RELEASE);
}

static void smp_scheduler_test(void)
{
    if (gui_active || sched_online_cpu_count() == 0u) {
        console_write("BOOK8-SMP-TEST: unavailable\r\n");
        return;
    }
    __atomic_store_n(&smp_test_completed, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&smp_test_errors, 0u, __ATOMIC_RELAXED);
    uint64_t expected = 0u;
    for (uint32_t cpu = 0u; cpu < sched_cpu_capacity(); ++cpu) {
        __atomic_store_n(&smp_test_cpu_runs[cpu], 0u, __ATOMIC_RELAXED);
        uint32_t tid = sched_spawn_on("smp-check", smp_test_worker,
            (void *)(uintptr_t)cpu, 1ull << cpu);
        if (tid != 0u)
            expected |= 1ull << cpu;
    }
    uint64_t start = read_tsc();
    uint64_t timeout = tsc_hz == 0u ? UINT64_MAX : tsc_hz * 5u;
    while ((__atomic_load_n(&smp_test_completed, __ATOMIC_ACQUIRE)
            & expected) != expected
        && read_tsc() - start < timeout) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
    uint32_t completed = 0u;
    for (uint32_t cpu = 0u; cpu < sched_cpu_capacity(); ++cpu)
        completed += (expected & (1ull << cpu)) != 0u;
    bool passed = expected != 0u
        && (__atomic_load_n(&smp_test_completed, __ATOMIC_ACQUIRE) & expected)
            == expected
        && __atomic_load_n(&smp_test_errors, __ATOMIC_RELAXED) == 0u;
    for (uint32_t cpu = 0u; passed && cpu < sched_cpu_capacity(); ++cpu) {
        if ((expected & (1ull << cpu)) != 0u
            && __atomic_load_n(&smp_test_cpu_runs[cpu], __ATOMIC_RELAXED)
                != 50000u)
            passed = false;
    }
    char message[96];
    snprintf(message, sizeof(message),
        "BOOK8-SMP-TEST: %s cores=%u online=%u\r\n",
        passed ? "PASS" : "FAIL", completed, sched_online_cpu_count());
    serial_write(message);
}

static void gui_present(void)
{
    fb_present(&fbdev);
}

static bool gui_cursor_save_under(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    gui_cursor_under_valid = false;
    if (fbdev.draw == NULL || width == 0u || height == 0u
        || width > GUI_CURSOR_WIDTH || height > GUI_CURSOR_HEIGHT
        || x >= fbdev.width || y >= fbdev.height
        || width > fbdev.width - x || height > fbdev.height - y) {
        return false;
    }

    uint32_t row_bytes = width * fbdev.bytes;
    for (uint32_t row = 0u; row < height; ++row) {
        const uint8_t *source = fbdev.draw + (size_t)(y + row) * fbdev.pitch
            + (size_t)x * fbdev.bytes;
        uint8_t *destination = gui_cursor_under + (size_t)row * row_bytes;
        for (uint32_t byte = 0u; byte < row_bytes; ++byte) {
            destination[byte] = source[byte];
        }
    }
    gui_cursor_under_x = x;
    gui_cursor_under_y = y;
    gui_cursor_under_width = width;
    gui_cursor_under_height = height;
    gui_cursor_under_valid = true;
    return true;
}

static bool gui_cursor_restore_under(void)
{
    if (!gui_cursor_under_valid || fbdev.draw == NULL
        || gui_cursor_under_x >= fbdev.width
        || gui_cursor_under_y >= fbdev.height
        || gui_cursor_under_width > fbdev.width - gui_cursor_under_x
        || gui_cursor_under_height > fbdev.height - gui_cursor_under_y) {
        gui_cursor_under_valid = false;
        return false;
    }

    uint32_t row_bytes = gui_cursor_under_width * fbdev.bytes;
    for (uint32_t row = 0u; row < gui_cursor_under_height; ++row) {
        uint8_t *destination = fbdev.draw
            + (size_t)(gui_cursor_under_y + row) * fbdev.pitch
            + (size_t)gui_cursor_under_x * fbdev.bytes;
        const uint8_t *source = gui_cursor_under + (size_t)row * row_bytes;
        for (uint32_t byte = 0u; byte < row_bytes; ++byte) {
            destination[byte] = source[byte];
        }
    }
    gui_cursor_under_valid = false;
    return true;
}

static void gui_present_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    if (x <= INT32_MAX && y <= INT32_MAX) {
        fb_present_rect(&fbdev, (int32_t)x, (int32_t)y, width, height);
    }
}

static void diag_marker(const char *tag)
{
    if (BOOK8_DIAG_MARKERS == 0) {
        return;
    }
    serial_write("BOOK8-DIAG: ");
    serial_write(tag);
    serial_write("\r\n");
    if (BOOK8_DIAG_MARKERS >= 2) {
        console_putc('[');
        for (const char *c = tag; *c != '\0' && *c != ' '; ++c) {
            console_putc(*c);
        }
        console_putc(']');
    }
}

static char read_char(void)
{
    static bool first_poll = true;
    for (;;) {
        network_rx_poll();
        if (first_poll) {
            diag_marker("M3 read_char entered");
        }
        if (serial_ready()) {
            char value = (char)inb(COM1);
            return value == '\r' ? '\n' : value;
        }
        if (first_poll) {
            diag_marker("S serial polled");
        }
        uint32_t usb_key;
        if (usb_keyboard_get_key(&usb_key)) {
            switch (usb_key) {
            case BOOK8GUI_KEY_ENTER: return '\n';
            case BOOK8GUI_KEY_BACKSPACE: return '\b';
            case BOOK8GUI_KEY_DELETE: return '\b';
            case BOOK8GUI_KEY_ESCAPE: return '\x1b';
            case BOOK8GUI_KEY_TAB: return '\t';
            default:
                if (usb_key <= 0x7fu) {
                    return (char)usb_key;
                }
                break;
            }
        }
        if (first_poll) {
            diag_marker("U usb polled");
        }
        uint8_t status = inb(0x64u);
        if (first_poll) {
            diag_marker("P ps2 polled");
            first_poll = false;
        }
        if ((status & 1u) != 0u) {
            uint8_t scan = inb(0x60u);
            if ((status & 0x20u) != 0u) {
                continue;
            }
            char value = keyboard_char(scan);
            if (value != 0) {
                return value;
            }
        }
        __asm__ volatile ("pause");
    }
}

static void print_prompt(void)
{
    for (size_t i = 0u; prompt_format[i] != '\0'; ++i) {
        if (prompt_format[i] != '$' || prompt_format[i + 1u] == '\0') {
            console_putc(prompt_format[i]);
            continue;
        }
        char code = prompt_format[++i];
        if (code == 'P' || code == 'p') {
            console_write(fs_current_path());
        } else if (code == 'G' || code == 'g') {
            console_putc('>');
        } else if (code == 'V' || code == 'v') {
            console_write("BOOK8");
        } else if (code == '$') {
            console_putc('$');
        } else {
            console_putc('$');
            console_putc(code);
        }
    }
    console_present();
}

static void print_help(void)
{
    console_write("BOOK8 x86-64 commands:\n");
    console_write("  HELP ? VER CLS ECHO DATE TIME DISK READ WRITE MOUNT VOL DIR TYPE NVINFO\n");
    console_write("  WRITEFILE COPY DEL FORMAT FDISK INSTALL CD CHDIR\n");
    console_write("  CD CHDIR COPY DEL ERASE MD MKDIR RD RMDIR SET PATH PROMPT\n");
    console_write("  REM ATTRIB CHKDSK DEFRAG EDIT FDISK FORMAT MOVE SCANDISK SYS XCOPY\n");
    console_write("  PCIDUMP LSPCI HWDIAG PNPSCAN MEMDUMP <hex_addr> [len] IOPORT <in|out> <hex_port> [width|hex_val]\n");
    console_write("  REG QUERY <path> | REG ADD <path> <key> <value> | REG SAVE\n");
    console_write("  IOPORT out performs a privileged hardware write; PCI BAR sizes may be unknown unless safely measured.\n");
    console_write("  USBSTAT INPUTSTAT THREADS SWAPTEST USBREG LIBTEST RING3TEST SCHEDTEST SCHEDUSER SMPTEST VMTEST MSRGUARDTEST EXEC BOOK8GUI HALT REBOOT RESET\n");
    console_write("  PCIEPROBE <A2/A5..AF>|LTSSM|LINKWIDTH|LINKCAP|LINKSTATUS|STATUS|RETRY\n");
    console_write("  ACK <id> INTENT|RESULT (verify collector receipt before each ACK)\n");
    console_write("  NETSTAT NETCONFIG <ip> <mask> <gateway> | OFF | DHCP | RENEW\n");
    console_write("  TELEMETRY <target-ip> | TELEMETRY OFF (UDP 8080, once/second)\n");
    console_write("  NETBOOT <tftp-ip> <warm-kernel-file> (trusted images only)\n");
    console_write("  NETSAVE (save DHCP/static mode and fallback to A:\\BOOK8DHCP.CFG)\n");
    console_write("  RELAUNCH <kernel.elf> (restart a BOOK8 Multiboot2 ELF from A: in RAM)\n");
    console_write("FAT16/FAT32 + VFAT/LFN supported; NTFS root-file support is limited.\n");
    console_write("FORMAT/FDISK/INSTALL erase data; use disposable disks and read prompts carefully.\n");
    console_write("INSTALL creates a FAT32 root and 4 GiB raw swap partition after Y/N confirmations.\n");
    console_write("BOOK8GUI opens the framebuffer explorer and command window on request.\n");
}

static void nvinfo_command(void)
{
    if (!nv_gpu.present || !nv_gpu.mapped) {
        console_write("NVINFO: GM204 telemetry unavailable (device not mapped).\n");
        return;
    }
    nv_driver_refresh_state();
    console_write("NVINFO: GM204 10DE:");
    print_hex8((uint8_t)(nv_gpu.device_id >> 8u));
    print_hex8((uint8_t)nv_gpu.device_id);
    console_write(" at ");
    print_hex8(nv_gpu.bus);
    console_putc(':');
    print_hex8(nv_gpu.slot);
    console_putc('.');
    print_hex8(nv_gpu.function);
    console_write(" BAR0 UC=0x");
    print_hex64(nv_gpu.bar0_phys);
    console_write(" BAR1 WB=0x");
    print_hex64(nv_gpu.bar1_phys);
    console_putc('\n');

    console_write("PMC_BOOT_0=0x");
    print_hex32(nv_gpu.boot0);
    console_write(" ChipRev=");
    print_unsigned(nv_gpu.chip_rev);
    console_write(" Foundry=0x");
    print_hex8(nv_gpu.foundry_id);
    console_write(" PMC_ENABLE=0x");
    print_hex32(nv_gpu.pmc_enable);
    console_write(" [PFIFO=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PFIFO) != 0u);
    console_write(" PGRAPH=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PGRAPH) != 0u);
    console_write(" PDISP=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PDISP) != 0u);
    console_write(" PCRTC=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PCRTC) != 0u);
    console_write("]\n");

    console_write("PMC_INTR_0=0x");
    print_hex32(nv_gpu.pmc_intr);
    console_write(" PMC_INTR_EN_0=0x");
    print_hex32(nv_gpu.pmc_intr_en_0);
    console_write(" PFB_CONFIG=0x");
    print_hex32(nv_gpu.pfb_config);
    console_write(" PFB_CSTATUS=0x");
    print_hex32(nv_gpu.pfb_cstatus);
    console_write(" DetectedVRAM_MB=");
    print_unsigned(nv_gpu.detected_vram_mb);
    console_putc('\n');

    console_write("PDISP head mask=0x");
    print_hex32(nv_gpu.pdisp_head_mask);
    console_write(" active head=");
    if (nv_gpu.pdisp_active_head == UINT32_MAX) {
        console_write("none");
    } else {
        print_unsigned(nv_gpu.pdisp_active_head);
        console_write(" timing total=");
        print_unsigned(nv_gpu.pdisp_htotal);
        console_putc('x');
        print_unsigned(nv_gpu.pdisp_vtotal);
    }
    console_putc('\n');
    if (nv_gpu.scanout.valid) {
        console_write("ScanoutBase=0x");
        print_hex64(nv_gpu.scanout.physical_base);
        console_write(" ");
        print_unsigned(nv_gpu.scanout.width);
        console_putc('x');
        print_unsigned(nv_gpu.scanout.height);
        console_write(" stride=");
        print_unsigned(nv_gpu.scanout.pitch);
        console_write(" bytes\n");
    } else {
        console_write("ScanoutBase=unavailable (no validated firmware framebuffer handoff)\n");
    }
}

static void print_usb_poll_status(void)
{
    struct book8_usb_diagnostics diagnostics;
    book8_usb_get_diagnostics(&diagnostics);
    console_write("USB HID poll (active/completions/stalls): ");
    print_unsigned(diagnostics.active_hid_devices);
    console_putc('/');
    print_unsigned(diagnostics.uhci_interrupt_completions);
    console_putc('/');
    print_unsigned(diagnostics.uhci_interrupt_stalls);
    console_putc('\n');
    for (uint32_t i = 0u; i < diagnostics.uhci_diagnostic_count; ++i) {
        const struct book8_usb_uhci_diagnostic *detail =
            &diagnostics.uhci[i];
        console_write("UHCI[");
        print_unsigned(i);
        console_write("] PCI=");
        print_hex8((uint8_t)detail->bus);
        console_putc(':');
        print_hex8((uint8_t)detail->slot);
        console_putc('.');
        print_hex8((uint8_t)detail->function);
        console_write(" BAR=");
        print_hex32(detail->pci_bar);
        console_write(" CMD=");
        print_hex8((uint8_t)(detail->pci_command >> 8u));
        print_hex8((uint8_t)detail->pci_command);
        console_putc('\n');
        console_write("  io=");
        print_hex32(detail->io_base);
        console_write(" sts=");
        print_hex8((uint8_t)(detail->controller_status >> 8u));
        print_hex8((uint8_t)detail->controller_status);
        console_write(" frame=");
        print_hex8((uint8_t)(detail->frame_number >> 8u));
        print_hex8((uint8_t)detail->frame_number);
        console_write(" ports=");
        print_hex8((uint8_t)(detail->port_status[0] >> 8u));
        print_hex8((uint8_t)detail->port_status[0]);
        console_putc('/');
        print_hex8((uint8_t)(detail->port_status[1] >> 8u));
        print_hex8((uint8_t)detail->port_status[1]);
        console_putc('\n');
    }
    console_write("SWAP used/total slots: ");
    print_unsigned(swap_used_slots());
    console_putc('/');
    print_unsigned(swap_total_slots());
    console_write("  VMM resident/swapped pages: ");
    print_unsigned(vmm_page_count());
    console_putc('/');
    print_unsigned(vmm_swapped_page_count());
    console_putc('\n');
}

static void print_input_status(void)
{
    uint64_t flags = irq_save();
    uint32_t queue_depth = (mouse_event_head - mouse_event_tail)
        & (MOUSE_EVENT_QUEUE_SIZE - 1u);
    uint32_t irq_count = ps2_mouse_irq_count;
    uint32_t timer_count = timer_irq_count;
    uint32_t packet_count = ps2_mouse_packet_count;
    uint32_t ps2_events = ps2_mouse_queue_events;
    uint32_t ps2_drops = ps2_mouse_queue_drops;
    uint32_t usb_reports = usb_mouse_report_count;
    uint32_t usb_events = usb_mouse_queue_events;
    uint32_t usb_drops = usb_mouse_queue_drops;
    irq_restore(flags);

    console_write("BOOK8-INPUT: IRQ12=");
    print_unsigned(irq_count);
    console_write(" IRQ0=");
    print_unsigned(timer_count);
    console_write(" PS2_PACKETS=");
    print_unsigned(packet_count);
    console_write(" PS2_QUEUED=");
    print_unsigned(ps2_events);
    console_write(" PS2_QUEUE_DROPS=");
    print_unsigned(ps2_drops);
    console_write(" USB_REPORTS=");
    print_unsigned(usb_reports);
    console_write(" USB_QUEUED=");
    print_unsigned(usb_events);
    console_write(" USB_QUEUE_DROPS=");
    print_unsigned(usb_drops);
    console_write(" QUEUE_DEPTH=");
    print_unsigned(queue_depth);
    console_putc('\n');
}

static bool parse_integer(const char *text, uint8_t base, uint64_t *value)
{
    uint64_t parsed = 0u;
    bool has_digit = false;

    if (base == 16u && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    while (*text != '\0') {
        uint8_t digit;
        if (*text >= '0' && *text <= '9') {
            digit = (uint8_t)(*text - '0');
        } else if (base == 16u && *text >= 'a' && *text <= 'f') {
            digit = (uint8_t)(*text - 'a' + 10);
        } else if (base == 16u && *text >= 'A' && *text <= 'F') {
            digit = (uint8_t)(*text - 'A' + 10);
        } else {
            return false;
        }
        if (digit >= base || parsed > (UINT64_MAX - digit) / base) {
            return false;
        }
        parsed = parsed * base + digit;
        has_digit = true;
        ++text;
    }
    if (!has_digit) {
        return false;
    }
    *value = parsed;
    return true;
}

static char *next_command_token(char **cursor)
{
    char *token = skip_spaces(*cursor);
    char *end = token;
    while (*end != '\0' && *end != ' ' && *end != '\t') {
        ++end;
    }
    if (*end != '\0') {
        *end++ = '\0';
    }
    *cursor = skip_spaces(end);
    return *token == '\0' ? NULL : token;
}

static bool memdump_page_mapped(uint64_t address)
{
    uint64_t cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if ((cr0 & (1ull << 31u)) == 0u || (cr4 & (1ull << 12u)) != 0u) {
        return false;
    }

    uint64_t table_phys = cr3 & 0x000ffffffffff000ull;
    static const uint8_t shifts[] = {39u, 30u, 21u, 12u};
    for (uint32_t level = 0u; level < 4u; ++level) {
        if (table_phys >= 0x100000000ull - 4096u) {
            return false;
        }
        const volatile uint64_t *table =
            (const volatile uint64_t *)(uintptr_t)table_phys;
        uint16_t index = (uint16_t)((address >> shifts[level]) & 0x1ffu);
        uint64_t entry = table[index];
        if ((entry & 1u) == 0u) {
            return false;
        }
        if ((level == 1u || level == 2u) && (entry & (1ull << 7u)) != 0u) {
            return true;
        }
        if (level == 3u) {
            return true;
        }
        table_phys = entry & 0x000ffffffffff000ull;
    }
    return false;
}

static bool memdump_range_mapped(uint64_t address, uint32_t length)
{
    if (length == 0u || address >= 0x100000000ull
        || length > 0x100000000ull - address) {
        return false;
    }
    uint64_t page = address & ~0xfffull;
    uint64_t last_page = (address + length - 1u) & ~0xfffull;
    for (;;) {
        if (!memdump_page_mapped(page)) {
            return false;
        }
        if (page == last_page) {
            return true;
        }
        page += 0x1000u;
    }
}

static void memdump_command(char *arguments)
{
    char *address_text = next_command_token(&arguments);
    char *length_text = next_command_token(&arguments);
    uint64_t address, parsed_length = 64u;
    uint32_t length;

    if (address_text == NULL
        || !parse_integer(address_text, 16u, &address)
        || (length_text != NULL
            && !parse_integer(length_text,
                length_text[0] == '0' && (length_text[1] == 'x'
                    || length_text[1] == 'X') ? 16u : 10u,
                &parsed_length))
        || *arguments != '\0' || parsed_length == 0u || parsed_length > 256u) {
        console_write("Usage: MEMDUMP <hex_addr> [len (1-256, decimal)]\n");
        return;
    }
    length = (uint32_t)parsed_length;
    if (!memdump_range_mapped(address, length)) {
        console_write("MEMDUMP: range is unmapped or outside identity-mapped memory\n");
        return;
    }

    const volatile uint8_t *bytes = (const volatile uint8_t *)(uintptr_t)address;
    for (uint32_t offset = 0u; offset < length; offset += 16u) {
        uint32_t count = length - offset;
        if (count > 16u) {
            count = 16u;
        }
        print_hex32((uint32_t)(address + offset));
        console_write(": ");
        for (uint32_t i = 0u; i < 16u; ++i) {
            if (i < count) {
                print_hex8(bytes[offset + i]);
                console_putc(' ');
            } else {
                console_write("   ");
            }
        }
        console_write(" |");
        for (uint32_t i = 0u; i < count; ++i) {
            uint8_t byte = bytes[offset + i];
            console_putc(byte >= 32u && byte <= 126u ? (char)byte : '.');
        }
        console_write("|\n");
    }
}

static void ioport_command(char *arguments)
{
    char *operation = next_command_token(&arguments);
    char *port_text = next_command_token(&arguments);
    char *option = next_command_token(&arguments);
    uint64_t port_value, data_value;

    if (operation == NULL || port_text == NULL
        || !parse_integer(port_text, 16u, &port_value)
        || port_value > 0xffffu) {
        console_write("Usage: IOPORT <in|out> <hex_port> [b|w|l for in; hex_val for out]\n");
        return;
    }
    if (ascii_equal(operation, "IN")) {
        char width = 'b';
        uint32_t value;
        if (*arguments != '\0') {
            console_write("Usage: IOPORT in <hex_port> [b|w|l]\n");
            return;
        }
        if (option != NULL) {
            if (option[1] != '\0'
                || (option[0] != 'b' && option[0] != 'B'
                    && option[0] != 'w' && option[0] != 'W'
                    && option[0] != 'l' && option[0] != 'L')) {
                console_write("IOPORT: read width must be b, w, or l\n");
                return;
            }
            width = option[0];
        }
        if (width == 'b' || width == 'B') {
            value = inb((uint16_t)port_value);
        } else if (width == 'w' || width == 'W') {
            value = inw((uint16_t)port_value);
        } else {
            value = inl((uint16_t)port_value);
        }
        console_write("IOPORT ");
        console_putc(width);
        console_write(" 0x");
        print_hex32((uint32_t)port_value);
        console_write(" = 0x");
        print_hex32(value);
        console_putc('\n');
    } else if (ascii_equal(operation, "OUT")) {
        if (option == NULL || *arguments != '\0'
            || !parse_integer(option, 16u, &data_value)
            || data_value > 0xffu) {
            console_write("Usage: IOPORT out <hex_port> <hex_val (00-FF)>\n");
            return;
        }
        outb((uint16_t)port_value, (uint8_t)data_value);
        console_write("IOPORT outb 0x");
        print_hex32((uint32_t)port_value);
        console_write(" <= 0x");
        print_hex8((uint8_t)data_value);
        console_write(" (write issued)\n");
    } else {
        console_write("IOPORT: operation must be in or out\n");
    }
}

static bool same_pci_function(const struct book8_pcie_node *node,
    uint8_t bus, uint8_t slot, uint8_t function)
{
    return node->segment == 0u && node->bus == bus && node->slot == slot
        && node->function == function;
}

static uint64_t pcie_dump_bar_size(const struct book8_pcie_node *node,
    uint8_t index, const char **cache)
{
    const struct book8_pcie_bar *resource = &node->bars[index];
    *cache = resource->io ? "I/O" : "not mapped";
    if (resource->cache_policy == BOOK8_PCIE_CACHE_UC) {
        *cache = "UC";
    } else if (resource->cache_policy == BOOK8_PCIE_CACHE_WC) {
        *cache = "WC";
    } else if (resource->cache_policy == BOOK8_PCIE_CACHE_WB) {
        *cache = "WB";
    }
    if (same_pci_function(node, rtl_nic.bus, rtl_nic.slot, rtl_nic.function)
        && rtl_nic.present && index < 6u) {
        if (rtl_nic.bars[index].mapped != NULL && !rtl_nic.bars[index].io) {
            *cache = "UC";
        }
        return rtl_nic.bars[index].size;
    }
    if (same_pci_function(node, hda_controller.bus, hda_controller.slot,
            hda_controller.function)
        && hda_controller.present && index == 0u
        && node->bars[index].base == hda_controller.mmio_base) {
        *cache = hda_controller.regs != NULL ? "UC" : "not mapped";
        return hda_controller.regs != NULL ? 0x4000u : 0u;
    }
    if (same_pci_function(node, nv_gpu.bus, nv_gpu.slot, nv_gpu.function)
        && nv_gpu.present) {
        if (index == 0u) {
            *cache = nv_gpu.bar0 != NULL ? "UC" : "not mapped";
            return nv_gpu.bar0_size;
        }
        if (index == 1u) {
            *cache = nv_gpu.bar1 != NULL ? "WB" : "not mapped";
            return nv_gpu.bar1_size;
        }
    }
    if (node->bus_device.driver != NULL && index == 0u
        && node->bus_device.driver->bar0_window_size != 0u) {
        *cache = "UC";
    }
    return resource->size;
}

static void pcidump_command(void)
{
    console_write("PCI/PCIe devices: ");
    print_unsigned(pcie.node_count);
    console_write(" (bound drivers=");
    print_unsigned(pcie.driver_count);
    console_write(")\n");
    for (size_t i = 0u; i < pcie.node_count; ++i) {
        const struct book8_pcie_node *node = &pcie.nodes[i];
        uint16_t vendor = (uint16_t)node->id;
        uint16_t device = (uint16_t)(node->id >> 16u);
        uint32_t class_code = node->class_revision >> 8u;
        console_write(" ");
        if (node->segment != 0u) {
            print_hex32(node->segment);
            console_putc(':');
        }
        print_hex8(node->bus);
        console_putc(':');
        print_hex8(node->slot);
        console_putc('.');
        print_hex8(node->function);
        console_write(" ");
        print_hex8((uint8_t)(vendor >> 8u));
        print_hex8((uint8_t)vendor);
        console_putc(':');
        print_hex8((uint8_t)(device >> 8u));
        print_hex8((uint8_t)device);
        console_write(" class=");
        print_hex8((uint8_t)(class_code >> 16u));
        console_putc('/');
        print_hex8((uint8_t)(class_code >> 8u));
        console_write(" IRQ=");
        print_hex8(node->irq_line);
        console_write(" driver=");
        console_write(node->bus_device.driver != NULL
            ? node->bus_device.driver->name
            : node->driver != NULL ? node->driver->name : "none");
        console_putc('\n');
        for (uint8_t bar = 0u; bar < node->bar_count; ++bar) {
            const struct book8_pcie_bar *resource = &node->bars[bar];
            console_write("   BAR");
            print_unsigned(bar);
            if (!resource->valid) {
                console_write(" unassigned/upper-half\n");
                continue;
            }
            console_write(resource->io ? " I/O=" : " MMIO=");
            console_write("0x");
            print_hex64(resource->base);
            console_write(" size=");
            const char *cache;
            uint64_t size = pcie_dump_bar_size(node, bar, &cache);
            if (size != 0u) {
                console_write("0x");
                print_hex64(size);
            } else {
                console_write("unknown");
            }
            console_write(" cache=");
            console_write(cache);
            console_write(" prefetch=");
            console_write(resource->prefetchable ? "yes\n" : "no\n");
        }
    }
}

static void pnp_scan_command(void)
{
    size_t old_pci_bound = pcie.driver_count;
    uint32_t old_usb_bound = book8_usb_bound_device_count();
    size_t pci_bound = book8_pcie_rescan(&pcie, bus_drivers,
        sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    book8_usb_rescan(&pcie);
    uint32_t usb_bound = book8_usb_bound_device_count();
    bool registry_sync_ok = book8_pcie_registry_sync(&pcie)
        && book8_usb_registry_sync();
    bool registry_ok = registry_sync_ok
        && (!registry_write_allowed || reg_save());

    console_write("PNPSCAN: PCI devices=");
    print_unsigned(pcie.node_count);
    console_write(" bound=");
    print_unsigned(pci_bound);
    console_write(" new=");
    print_unsigned(pci_bound > old_pci_bound
        ? pci_bound - old_pci_bound : 0u);
    console_write("; USB bound=");
    print_unsigned(usb_bound);
    console_write(" new=");
    print_unsigned(usb_bound > old_usb_bound
        ? usb_bound - old_usb_bound : 0u);
    if (!registry_sync_ok) {
        console_write(" (registry update failed)");
    } else if (!registry_write_allowed) {
        console_write(" (registry persistence deferred until installation)");
    } else if (!registry_ok) {
        console_write(" (registry update failed)");
    }
    console_putc('\n');
}

static void registry_seed_program(const char *id, const char *name,
    const char *path, const char *app_class, uint32_t hidden,
    uint32_t system_component)
{
    char key[BOOK8_REGISTRY_KEY_MAX];
    char existing[BOOK8_REGISTRY_VALUE_MAX];
    int length = snprintf(key, sizeof(key),
        "HKLM\\SOFTWARE\\BOOK8\\Programs\\%s", id);

    if (length < 0 || (size_t)length >= sizeof(key)
        || reg_get_str(key, "Name", existing, sizeof(existing))) {
        return;
    }
    (void)reg_set_str(key, "Name", name);
    (void)reg_set_str(key, "Path", path);
    (void)reg_set_str(key, "AppClass", app_class);
    (void)reg_set_u32(key, "Hidden", hidden);
    (void)reg_set_u32(key, "SystemComponent", system_component);
}

static void registry_seed_value(const char *path, const char *name,
    const char *value)
{
    char existing[BOOK8_REGISTRY_VALUE_MAX];
    if (!reg_get_str(path, name, existing, sizeof(existing))) {
        (void)reg_set_str(path, name, value);
    }
}

static void registry_format_ipv4(uint32_t address, char *text, size_t capacity)
{
    (void)snprintf(text, capacity, "%u.%u.%u.%u",
        (unsigned)(address >> 24u), (unsigned)((address >> 16u) & 255u),
        (unsigned)((address >> 8u) & 255u), (unsigned)(address & 255u));
}

static bool registry_parse_ipv4(const char *text, uint32_t *address)
{
    uint32_t result = 0u;
    for (uint32_t octet = 0u; octet < 4u; ++octet) {
        uint32_t value = 0u, digits = 0u;
        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (uint32_t)(*text++ - '0');
            if (value > 255u) { return false; }
            ++digits;
        }
        if (digits == 0u || (octet != 3u && *text++ != '.')
            || (octet == 3u && *text != '\0')) {
            return false;
        }
        result = (result << 8u) | value;
    }
    *address = result;
    return true;
}

static void registry_migrate_legacy_settings(void)
{
    static const char *const settings_path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    char existing[BOOK8_REGISTRY_VALUE_MAX], value[BOOK8_REGISTRY_VALUE_MAX];
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    uint32_t size = 0u;
    struct book8_system_config config;
    if (!reg_get_str(settings_path, "KeyRate", existing, sizeof(existing))
        && fs_read_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes), &size)
        && book8_config_decode(&config, bytes, size)) {
        (void)reg_set_u32(settings_path, "KeyRate",
            config.values[BOOK8_CFG_REPEAT_RATE]);
        (void)reg_set_u32(settings_path, "KeyDelay",
            (config.values[BOOK8_CFG_KEY_DELAY] + 1u) * 250u);
        (void)reg_set_u32(settings_path, "MouseSpeed",
            config.values[BOOK8_CFG_POINTER_SPEED]);
        (void)reg_set_u32(settings_path, "DblClickMs",
            config.values[BOOK8_CFG_DOUBLE_CLICK]);
        (void)reg_set_str(settings_path, "Theme",
            config.values[BOOK8_CFG_DARK] != 0u ? "Dark" : "Light");
        (void)reg_set_u32(settings_path, "Accent",
            config.values[BOOK8_CFG_ACCENT]);
        (void)reg_set_u32(settings_path, "Volume",
            config.values[BOOK8_CFG_VOLUME]);
        (void)reg_set_u32(settings_path, "Tone",
            config.values[BOOK8_CFG_TONE]);
    }
    if (reg_get_str(settings_path, "NetMode", existing, sizeof(existing))) {
        return;
    }
    uint8_t network_bytes[BOOK8_NETWORK_SETTINGS_BYTES + 1u];
    struct book8_network_settings network;
    if (fs_read_file(BOOK8_NETWORK_SETTINGS_PATH, network_bytes,
            sizeof(network_bytes), &size)
        && book8_network_settings_decode(&network, network_bytes, size)) {
        (void)reg_set_str(settings_path, "NetMode",
            network.mode == BOOK8_NETWORK_DHCP ? "DHCP" : "Static");
        registry_format_ipv4(network.ip, value, sizeof(value));
        (void)reg_set_str(settings_path, "StaticIP", value);
        registry_format_ipv4(network.mask, value, sizeof(value));
        (void)reg_set_str(settings_path, "Netmask", value);
        registry_format_ipv4(network.gateway, value, sizeof(value));
        (void)reg_set_str(settings_path, "Gateway", value);
        registry_format_ipv4(network.dns, value, sizeof(value));
        (void)reg_set_str(settings_path, "DNS", value);
    }
}

static void registry_seed_settings(void)
{
    static const char *const values[][2] = {
        {"Clock24H", "0"}, {"KeyDelay", "500"}, {"KeyRate", "25"},
        {"MouseSpeed", "100"}, {"DblClickMs", "400"},
        {"Theme", "Light"}, {"Accent", "0"}, {"OverscanX", "0"},
        {"OverscanY", "0"}, {"Volume", "75"}, {"UiBeep", "1"},
        {"Tone", "880"}, {"NetMode", "DHCP"},
        {"StaticIP", "192.168.50.208"}, {"Netmask", "255.255.255.0"},
        {"Gateway", "192.168.50.1"}, {"DNS", "0.0.0.0"}
    };
    const char *path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    for (uint32_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        registry_seed_value(path, values[i][0], values[i][1]);
    }
}

static void registry_seed_file_associations(void)
{
    static const char *const values[][2] = {
        {".TXT", "editor"}, {".INI", "editor"}, {".CFG", "editor"},
        {".LOG", "editor"}, {".C", "codeedit"}, {".H", "codeedit"},
        {".ASM", "codeedit"}, {".PS1", "codeedit"}
    };
    const char *path = "HKLM\\SOFTWARE\\BOOK8\\FileAssoc";
    for (uint32_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        registry_seed_value(path, values[i][0], values[i][1]);
    }
}

static void registry_seed_defaults(void)
{
    registry_migrate_legacy_settings();
    registry_seed_program("CommandShell", "Command Shell", "", "shell", 0u, 1u);
    registry_seed_program("FileExplorer", "File Explorer", "", "explorer", 0u, 1u);
    registry_seed_program("NoteEditor", "NoteEditor", "", "editor", 0u, 1u);
    registry_seed_program("TaskManager", "Task Manager", "", "taskmgr", 0u, 1u);
    registry_seed_program("SystemSettings", "System Settings", "", "settings", 0u, 1u);
    registry_seed_program("BOOK8Player", "BOOK8 Player", "", "player", 0u, 1u);
    registry_seed_program("CAT", "CAT", "A:\\CAT.ELF", "", 0u, 1u);
    for (uint32_t i = 0u; i < BOOK8_MANIFEST_APP_COUNT; ++i) {
        const struct book8_manifest_app *app = &book8_manifest_apps[i];
        char path[BOOK8_REGISTRY_VALUE_MAX];
        int length = snprintf(path, sizeof(path), "A:\\BIN\\%s", app->target);
        if (length > 0 && (size_t)length < sizeof(path)) {
            registry_seed_program(app->key, app->name, path, app->app_class,
                app->hidden, app->system_component);
        }
    }
    registry_seed_settings();
    registry_seed_file_associations();
    if (!book8_session_seed_defaults()) {
        console_write("SESSION: unable to seed primary account in registry.\n");
    }
}

struct registry_query_context {
    const char *path;
};

static bool registry_print_value(const char *name, void *context)
{
    struct registry_query_context *query = context;
    char value[BOOK8_REGISTRY_VALUE_MAX];
    if (!reg_get_str(query->path, name, value, sizeof(value))) {
        return true;
    }
    console_write("  ");
    console_write(name);
    console_write(" = ");
    console_write(value);
    console_write("\n");
    return true;
}

static bool registry_print_key(const char *name, void *context)
{
    struct registry_query_context *parent = context;
    struct registry_query_context child;
    char path[BOOK8_REGISTRY_KEY_MAX];
    int length = snprintf(path, sizeof(path), "%s\\%s", parent->path, name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return true;
    }
    console_write("  ");
    console_write(name);
    console_write("\\\n");
    child.path = path;
    (void)reg_enum_values(path, registry_print_value, &child);
    return true;
}

static void registry_command(char *arguments)
{
    char *operation = skip_spaces(arguments);
    char *rest = operation;
    while (*rest != '\0' && *rest != ' ' && *rest != '\t') {
        ++rest;
    }
    if (*rest != '\0') {
        *rest++ = '\0';
        rest = skip_spaces(rest);
    }
    if (ascii_equal(operation, "SAVE") && *rest == '\0') {
        if (!registry_write_allowed) {
            console_write("REG: save deferred until INSTALL mounts the target root.\n");
            return;
        }
        console_write(reg_save() ? "REG: saved A:\\BOOK8REG.INI\n"
            : "REG: save failed.\n");
        return;
    }
    if (ascii_equal(operation, "QUERY") && *rest != '\0') {
        struct registry_query_context query = {.path = rest};
        console_write(rest);
        console_write("\n");
        size_t values = reg_enum_values(rest, registry_print_value, &query);
        size_t keys = reg_enum_keys(rest, registry_print_key, &query);
        if (values == 0u && keys == 0u) {
            console_write("  (no values or subkeys)\n");
        }
        return;
    }
    if (ascii_equal(operation, "ADD")) {
        char *path = rest;
        char *name = path;
        while (*name != '\0' && *name != ' ' && *name != '\t') {
            ++name;
        }
        if (*name == '\0') {
            goto invalid;
        }
        *name++ = '\0';
        name = skip_spaces(name);
        char *value = name;
        while (*value != '\0' && *value != ' ' && *value != '\t') {
            ++value;
        }
        if (*value == '\0') {
            goto invalid;
        }
        *value++ = '\0';
        value = skip_spaces(value);
        if (*path == '\0' || *name == '\0' || *value == '\0'
            || !reg_set_str(path, name, value)) {
            console_write("REG: invalid path, name or value.\n");
            return;
        }
        if (gui_active) {
            (void)book8gui_refresh_programs();
        }
        console_write("REG: value added in memory; use REG SAVE to persist.\n");
        return;
    }
invalid:
    console_write("Usage: REG QUERY <path> | REG ADD <path> <key> <value> | REG SAVE\n");
}

static bool read_active_tss(uint16_t *selector, uint64_t *rsp0)
{
    struct gdtr {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;
    uint16_t tr;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("str %0" : "=r"(tr));
    *selector = tr;
    if ((tr & 4u) != 0u || (tr & 0xfff8u) == 0u) {
        return false;
    }
    uint64_t descriptor_offset = tr & 0xfff8u;
    if (descriptor_offset + 15u > gdtr.limit
        || gdtr.base > 0x100000000ull - 16u
        || descriptor_offset > 0x100000000ull - gdtr.base - 16u) {
        return false;
    }
    const volatile uint64_t *descriptor =
        (const volatile uint64_t *)(uintptr_t)(gdtr.base + descriptor_offset);
    uint64_t low = descriptor[0], high = descriptor[1];
    uint8_t type = (uint8_t)((low >> 40u) & 0x0fu);
    uint32_t limit = (uint32_t)(low & 0xffffu)
        | (uint32_t)((low >> 32u) & 0x000f0000u);
    uint64_t base = ((low >> 16u) & 0xffffffu)
        | (((low >> 56u) & 0xffu) << 24u)
        | ((high & 0xffffffffu) << 32u);
    if ((type != 9u && type != 11u) || limit < 11u
        || base >= 0x100000000ull - 12u) {
        return false;
    }
    *rsp0 = *(const volatile uint64_t *)(uintptr_t)(base + 4u);
    return true;
}

static void hwdiag_command(void)
{
    struct gdtr {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;
    uint64_t cr0, cr3, cr4, rsp0 = 0u;
    uint16_t tr = 0u;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    bool tss_valid = read_active_tss(&tr, &rsp0);
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    console_write("HWDIAG GDT: base=0x");
    print_hex64(gdtr.base);
    console_write(" limit=0x");
    print_hex32(gdtr.limit);
    console_write(" TSS TR=0x");
    print_hex32(tr);
    console_write(tss_valid ? " active RSP0=0x" : " unavailable RSP0=unavailable");
    if (tss_valid) {
        print_hex64(rsp0);
        console_write(rsp0 != 0u && rsp0 < 0x100000000ull
                && (rsp0 & 0x0fu) == 0u ? " valid\n" : " invalid\n");
    } else {
        console_putc('\n');
    }
    console_write("Paging: CR0=0x");
    print_hex64(cr0);
    console_write(" CR3=0x");
    print_hex64(cr3);
    console_write(" PML4=0x");
    print_hex64(cr3 & 0x000ffffffffff000ull);
    console_write(" CR4=0x");
    print_hex64(cr4);
    console_write((cr0 & (1ull << 31u)) != 0u ? " PG=on" : " PG=off");
    console_write((cr4 & (1ull << 5u)) != 0u ? " PAE=on\n" : " PAE=off\n");
    console_write("PnP: PCI devices=");
    print_unsigned(pcie.node_count);
    console_write(" registered=");
    print_unsigned(pcie.registered_driver_count);
    console_write(" bound=");
    print_unsigned(pcie.driver_count);
    console_write("; USB registered=");
    print_unsigned(book8_usb_registered_driver_count());
    console_write(" bound=");
    print_unsigned(book8_usb_bound_device_count());
    console_putc('\n');
    console_write("PnP totals: registered drivers=");
    print_unsigned(pcie.registered_driver_count
        + book8_usb_registered_driver_count());
    console_write(" bound devices=");
    print_unsigned((uint32_t)pcie.driver_count
        + book8_usb_bound_device_count());
    console_putc('\n');
    console_write("Registered bus_driver entries: ");
    print_unsigned(sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    console_putc('\n');
    for (size_t i = 0u; i < sizeof(bus_drivers) / sizeof(bus_drivers[0]); ++i) {
        const struct book8_bus_driver *driver = &bus_drivers[i];
        console_write("  ");
        console_write(driver->name);
        console_write(" matches=");
        print_unsigned(driver->match_count);
        if (driver->bar0_window_size != 0u) {
            console_write(" BAR0-window=0x");
            print_hex64(driver->bar0_window_size);
        }
        console_putc('\n');
    }
    print_input_status();
}

static void print_date(void)
{
    struct clock_time clock = read_clock();
    print_unsigned(clock.year);
    console_putc('-');
    print_two_digits(clock.month);
    console_putc('-');
    print_two_digits(clock.day);
    console_putc('\n');
}

static void print_time(void)
{
    struct clock_time clock = read_clock();
    print_two_digits(clock.hour);
    console_putc(':');
    print_two_digits(clock.minute);
    console_putc(':');
    print_two_digits(clock.second);
    console_putc('\n');
}

static void run_read_command(char *arguments)
{
    char *device_text = skip_spaces(arguments);
    char *lba_text = device_text;
    while (*lba_text != '\0' && *lba_text != ' ' && *lba_text != '\t') {
        ++lba_text;
    }
    if (*lba_text != '\0') {
        *lba_text++ = '\0';
        lba_text = skip_spaces(lba_text);
    }
    char *extra = lba_text;
    while (*extra != '\0' && *extra != ' ' && *extra != '\t') {
        ++extra;
    }
    if (*extra != '\0') {
        *extra++ = '\0';
        extra = skip_spaces(extra);
    }
    if (*device_text == '\0' || *lba_text == '\0' || *extra != '\0') {
        console_write("Usage: READ <disk-number> <LBA>\n");
        return;
    }

    uint32_t device_index = 0u;
    uint64_t lba = 0u;
    for (const char *p = device_text; *p != '\0'; ++p) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (*p < '0' || *p > '9'
            || device_index > (UINT32_MAX - digit) / 10u) {
            console_write("Invalid disk number.\n");
            return;
        }
        device_index = device_index * 10u + digit;
    }
    for (const char *p = lba_text; *p != '\0'; ++p) {
        uint64_t digit = (uint64_t)(*p - '0');
        uint64_t maximum_lba = (1ull << 48u) - 1u;
        if (*p < '0' || *p > '9'
            || lba > (maximum_lba - digit) / 10u) {
            console_write("Invalid or unsupported LBA.\n");
            return;
        }
        lba = lba * 10u + digit;
    }
    if (device_index >= disk_count || lba >= disks[device_index].sectors) {
        console_write("Disk number or LBA is out of range.\n");
        return;
    }
    if (!disk_read_sector(&disks[device_index], lba, sector_buffer)) {
        console_write("Read failed; the disk was not modified.\n");
        return;
    }
    console_write("Sector ");
    print_unsigned(lba);
    console_write(" (first 32 bytes):");
    for (uint32_t i = 0u; i < 32u; ++i) {
        if ((i & 7u) == 0u) {
            console_putc('\n');
            console_write("  ");
        }
        print_hex8(sector_buffer[i]);
        console_putc(' ');
    }
    console_putc('\n');
}

static bool confirm_text(const char *prompt)
{
    char answer[8];
    uint32_t length = 0u;
    console_write(prompt);
    for (;;) {
        char value = read_char();
        if (value == '\r' || value == '\n') {
            console_putc('\n');
            answer[length] = '\0';
            return ascii_equal(answer, "YES");
        }
        if ((value == '\b' || value == 0x7f) && length > 0u) {
            --length;
            console_putc('\b');
        } else if (value >= 0x20 && value <= 0x7e
            && length + 1u < sizeof(answer)) {
            answer[length++] = value;
            console_putc(value);
        }
    }
}

static bool parse_disk_number(const char *text, uint32_t *disk_index);

/* Reads one echoed line; returns the length (excess characters are dropped). */
static uint32_t read_install_line(char *buffer, uint32_t capacity)
{
    uint32_t length = 0u;
    for (;;) {
        char value = read_char();
        if (value == '\r' || value == '\n') {
            console_putc('\n');
            buffer[length] = '\0';
            return length;
        }
        if ((value == '\b' || value == 0x7f) && length > 0u) {
            --length;
            console_putc('\b');
        } else if (value >= 0x20 && value <= 0x7e && length + 1u < capacity) {
            buffer[length++] = value;
            console_putc(value);
        }
    }
}

/* Asks a Y/N question until a valid answer is given. */
static bool confirm_yes_no(const char *prompt)
{
    char answer[8];
    for (;;) {
        console_write(prompt);
        uint32_t length = read_install_line(answer, sizeof(answer));
        if (length == 1u && (answer[0] == 'Y' || answer[0] == 'y')) {
            return true;
        }
        if (length == 1u && (answer[0] == 'N' || answer[0] == 'n')) {
            return false;
        }
        console_write("Please answer Y or N.\n");
    }
}

static void print_install_disk(const struct disk_device *disk)
{
    console_write(disk->name);
    console_write("  ");
    console_write(disk->model);
    console_write("  ");
    print_unsigned(disk->sectors);
    console_write(" sectors (");
    print_unsigned(disk->sectors / 2048u);
    console_write(" MiB)");
}

/* Numbered disk menu; returns false if cancelled. Entries are 1-based. */
static bool select_install_disk(uint32_t *disk_index)
{
    uint32_t menu[MAX_DISKS];
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < disk_count && count < MAX_DISKS; ++i) {
        if (disks[i].sectors != 0u) {
            menu[count++] = i;
        }
    }
    if (count == 0u) {
        console_write("No disks were detected. No disk was modified.\n");
        return false;
    }
    console_write("Available disks:\n");
    for (uint32_t n = 0u; n < count; ++n) {
        console_write("  ");
        print_unsigned(n + 1u);
        console_write(") ");
        print_install_disk(&disks[menu[n]]);
        console_putc('\n');
    }
    for (;;) {
        char answer[8];
        uint32_t choice = 0u;
        console_write("Select the target disk number (1-");
        print_unsigned(count);
        console_write(", or Q to cancel): ");
        uint32_t length = read_install_line(answer, sizeof(answer));
        if (length == 1u && (answer[0] == 'Q' || answer[0] == 'q')) {
            return false;
        }
        if (parse_disk_number(answer, &choice) && choice >= 1u
            && choice <= count) {
            *disk_index = menu[choice - 1u];
            return true;
        }
        console_write("Invalid selection.\n");
    }
}

static bool confirm_install_target(uint32_t disk_index)
{
    const struct disk_device *disk = &disks[disk_index];
    console_write("Selected disk: ");
    print_install_disk(disk);
    console_putc('\n');
    if (!confirm_yes_no("Is this the correct disk? (Y/N): ")) {
        return false;
    }
    console_write("WARNING: all existing partitions on ");
    console_write(disk->name);
    console_write(" will be deleted and all data on them will be lost.\n");
    if (!confirm_yes_no("Delete the existing partitions on this disk? (Y/N): ")) {
        return false;
    }
    console_write("BOOK8 will create a FAT32 root partition and a fixed 4 GiB raw swap partition across this disk.\n");
    return true;
}

static bool parse_disk_number(const char *text, uint32_t *disk_index)
{
    if (text == NULL || *text < '0' || *text > '9') {
        return false;
    }
    uint32_t value = 0u;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    if (*text != '\0') {
        return false;
    }
    *disk_index = value;
    return true;
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void install_report_progress(const char *operation, uint64_t completed,
    uint64_t total)
{
    console_write("[INSTALL] ");
    console_write(operation);
    console_write(": ");
    print_unsigned(total == 0u ? 100u
        : (uint32_t)(completed * 100u / total));
    console_write("%\n");
}

static bool install_clear_sector_range(struct disk_device *disk, uint64_t lba,
    uint32_t sectors, const char *operation)
{
    __builtin_memset(install_transfer_buffer, 0,
        sizeof(install_transfer_buffer));
    uint32_t remaining = sectors;
    uint32_t completed = 0u;
    while (remaining > 0u) {
        uint32_t count = remaining > ATA_BOUNCE_SECTORS
            ? ATA_BOUNCE_SECTORS : remaining;
        if (!disk_transfer(disk, lba, count, install_transfer_buffer, true)) {
            return false;
        }
        lba += count;
        remaining -= count;
        completed += count;
        if ((completed % 16384u) == 0u || remaining == 0u) {
            install_report_progress(operation, completed, sectors);
        }
    }
    return true;
}

static bool install_seed_default_config(void)
{
    struct book8_system_config defaults;
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    book8_config_defaults(&defaults);
    if (!book8_config_encode(&defaults, bytes)) {
        console_write("[INSTALL] could not encode the default desktop profile.\n");
        return false;
    }
    if (!fs_write_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes))) {
        console_write("[INSTALL] default desktop profile write failed: ");
        console_write(fs_error_text(fs_last_error()));
        console_write("\n");
        return false;
    }
    console_write("[INSTALL] default desktop profile created.\n");
    return true;
}

static bool install_resize_fat32(struct disk_device *disk, uint32_t start_lba,
    uint32_t total_sectors)
{
    uint8_t boot[512];
    uint8_t sector[512];
    if (!disk_read_sector(disk, start_lba, boot)) {
        return false;
    }

    uint32_t old_total = read_u32_le(boot + 32u);
    uint32_t reserved = read_u16_le(boot + 14u);
    uint32_t fat_count = boot[16u];
    uint32_t old_fat_sectors = read_u32_le(boot + 36u);
    uint32_t sectors_per_cluster = boot[13u];
    uint32_t backup_boot = read_u16_le(boot + 50u);
    uint32_t fsinfo_sector = read_u16_le(boot + 48u);
    if (boot[510u] != 0x55u || boot[511u] != 0xaau
        || read_u16_le(boot + 11u) != 512u || old_total == 0u
        || old_total > total_sectors || reserved == 0u
        || fat_count != 2u || old_fat_sectors == 0u
        || sectors_per_cluster == 0u
        || (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0u
        || (uint64_t)reserved + (uint64_t)fat_count * old_fat_sectors
            >= old_total
        || backup_boot == 0u || backup_boot >= reserved
        || fsinfo_sector == 0u || fsinfo_sector >= reserved) {
        return false;
    }

    uint32_t new_fat_sectors = old_fat_sectors;
    bool fat_size_stable = false;
    uint64_t new_cluster_count = 0u;
    for (uint32_t pass = 0u; pass < 32u; ++pass) {
        uint64_t metadata_sectors = (uint64_t)reserved
            + (uint64_t)fat_count * new_fat_sectors;
        if (metadata_sectors >= total_sectors) {
            return false;
        }
        new_cluster_count = (total_sectors - metadata_sectors)
            / sectors_per_cluster;
        uint64_t needed = ((new_cluster_count + 2u) * 4u + 511u) / 512u;
        if (needed > UINT32_MAX) {
            return false;
        }
        if (needed == new_fat_sectors) {
            fat_size_stable = true;
            break;
        }
        new_fat_sectors = (uint32_t)needed;
    }
    if (!fat_size_stable || new_fat_sectors < old_fat_sectors
        || new_cluster_count < 65525u || new_cluster_count > 0x0ffffff5u
        || new_cluster_count + 2u
            > (uint64_t)new_fat_sectors * 512u / 4u) {
        return false;
    }

    uint64_t old_data_start = (uint64_t)start_lba + reserved
        + (uint64_t)fat_count * old_fat_sectors;
    uint64_t old_data_sectors = old_total - reserved
        - (uint64_t)fat_count * old_fat_sectors;
    uint64_t data_shift = (uint64_t)fat_count
        * (new_fat_sectors - old_fat_sectors);
    if (old_data_start + old_data_sectors + data_shift
        > (uint64_t)start_lba + total_sectors) {
        return false;
    }

    if (data_shift != 0u) {
        console_write("[INSTALL] relocating FAT32 data into expanded partition...\n");
        uint64_t remaining = old_data_sectors;
        uint64_t completed = 0u;
        while (remaining > 0u) {
            uint32_t count = remaining > ATA_BOUNCE_SECTORS
                ? ATA_BOUNCE_SECTORS : (uint32_t)remaining;
            uint64_t source_lba = old_data_start + remaining - count;
            if (!disk_transfer(disk, source_lba, count,
                    install_transfer_buffer, false)
                || !disk_transfer(disk, source_lba + data_shift, count,
                    install_transfer_buffer, true)) {
                return false;
            }
            remaining -= count;
            completed += count;
            if ((completed % 16384u) == 0u || remaining == 0u) {
                install_report_progress("FAT32 data relocation", completed,
                    old_data_sectors);
            }
        }
    }

    if (new_fat_sectors != old_fat_sectors) {
        console_write("[INSTALL] relocating FAT32 allocation table...\n");
        uint64_t old_second_fat = (uint64_t)start_lba + reserved
            + old_fat_sectors;
        uint64_t new_second_fat = (uint64_t)start_lba + reserved
            + new_fat_sectors;
        uint32_t remaining = old_fat_sectors;
        while (remaining > 0u) {
            uint32_t count = remaining > ATA_BOUNCE_SECTORS
                ? ATA_BOUNCE_SECTORS : remaining;
            uint64_t offset = remaining - count;
            if (!disk_transfer(disk, old_second_fat + offset, count,
                    install_transfer_buffer, false)
                || !disk_transfer(disk, new_second_fat + offset, count,
                    install_transfer_buffer, true)) {
                return false;
            }
            remaining -= count;
        }
        uint32_t added_fat_sectors = new_fat_sectors - old_fat_sectors;
        console_write("[INSTALL] initializing expanded FAT32 tables...\n");
        if (!install_clear_sector_range(disk,
                (uint64_t)start_lba + reserved + old_fat_sectors,
                added_fat_sectors, "FAT32 table 1")
            || !install_clear_sector_range(disk,
                (uint64_t)start_lba + reserved + new_fat_sectors
                    + old_fat_sectors,
                added_fat_sectors, "FAT32 table 2")) {
            return false;
        }
    }

    write_u32_le(boot + 28u, start_lba);
    write_u32_le(boot + 32u, total_sectors);
    write_u32_le(boot + 36u, new_fat_sectors);
    if (!disk_write_sector_unflushed(disk, start_lba, boot)
        || !disk_read_sector(disk, start_lba + backup_boot, sector)) {
        return false;
    }
    write_u32_le(sector + 28u, start_lba);
    write_u32_le(sector + 32u, total_sectors);
    write_u32_le(sector + 36u, new_fat_sectors);
    if (!disk_write_sector_unflushed(disk, start_lba + backup_boot, sector)) {
        return false;
    }

    uint64_t fsinfo_lba = (uint64_t)start_lba + fsinfo_sector;
    if (!disk_read_sector(disk, fsinfo_lba, sector)) {
        return false;
    }
    write_u32_le(sector + 488u, UINT32_MAX);
    if (!disk_write_sector_unflushed(disk, fsinfo_lba, sector)
        || !disk_read_sector(disk, fsinfo_lba + backup_boot, sector)) {
        return false;
    }
    write_u32_le(sector + 488u, UINT32_MAX);
    return disk_write_sector_unflushed(disk, fsinfo_lba + backup_boot, sector);
}

static void install_set_mbr_partition(uint8_t *mbr, uint32_t index,
    uint8_t active, uint8_t type, uint32_t start_lba, uint32_t sectors)
{
    uint8_t *entry = mbr + 446u + index * 16u;
    for (uint32_t i = 0u; i < 16u; ++i) {
        entry[i] = 0u;
    }
    entry[0] = active;
    entry[1] = 0xffu;
    entry[2] = 0xffu;
    entry[3] = 0xffu;
    entry[4] = type;
    entry[5] = 0xffu;
    entry[6] = 0xffu;
    entry[7] = 0xffu;
    write_u32_le(entry + 8u, start_lba);
    write_u32_le(entry + 12u, sectors);
}

static void install_command(const char *arguments)
{
    uint32_t disk_index;
    if (installer_image == NULL || installer_image_size < 512u
        || (installer_image_size & 511u) != 0u) {
        console_write("Installer payload is unavailable; boot the BOOK8 install ISO.\n");
        return;
    }
    console_write("[INSTALL] probing target disk list...\n");
    if (*arguments == '\0') {
        if (!select_install_disk(&disk_index)) {
            console_write("Installation cancelled; disk was not modified.\n");
            return;
        }
    } else if (!parse_disk_number(arguments, &disk_index)) {
        console_write("Usage: INSTALL (shows a numbered disk menu).\n");
        return;
    }
    if (disk_index >= disk_count || disks[disk_index].sectors == 0u) {
        console_write("Disk number is not present. No disk was modified.\n");
        return;
    }
    if (disks[disk_index].ahci && !disks[disk_index].lba48) {
        console_write("This AHCI disk lacks LBA48 support; installation is refused.\n");
        return;
    }

    uint64_t image_sectors = installer_image_size / 512u;
    const uint8_t *source_mbr = installer_image;
    uint64_t disk_sectors = disks[disk_index].sectors;
    if (source_mbr[510] != 0x55u || source_mbr[511] != 0xaau
        || source_mbr[446] != 0x80u || source_mbr[450] != 0x0cu) {
        console_write("Installer payload has no validated active FAT32 MBR partition.\n");
        return;
    }
    uint32_t source_start = read_u32_le(source_mbr + 454u);
    uint32_t source_sectors = read_u32_le(source_mbr + 458u);
    if (source_start != INSTALL_ROOT_START_LBA || source_sectors == 0u
        || (uint64_t)source_start + source_sectors > image_sectors
        || image_sectors > disk_sectors
        || disk_sectors > 0x100000000ull
        || disk_sectors < (uint64_t)INSTALL_ROOT_START_LBA
            + INSTALL_MIN_ROOT_SECTORS + INSTALL_SWAP_SECTORS) {
        console_write("Target disk needs at least a 2 GiB root plus 4 GiB raw swap and must fit MBR address limits.\n");
        return;
    }
    uint32_t root_sectors = (uint32_t)(disk_sectors
        - INSTALL_ROOT_START_LBA - INSTALL_SWAP_SECTORS);
    uint32_t swap_start = INSTALL_ROOT_START_LBA + root_sectors;
    const uint8_t *source_boot_sector = installer_image
        + (size_t)source_start * 512u;
    uint8_t cluster_sectors = source_boot_sector[13u];
    if (source_boot_sector[510u] != 0x55u || source_boot_sector[511u] != 0xaau
        || read_u16_le(source_boot_sector + 11u) != 512u
        || read_u32_le(source_boot_sector + 28u) != source_start
        || read_u32_le(source_boot_sector + 32u) == 0u
        || read_u32_le(source_boot_sector + 32u) > source_sectors
        || cluster_sectors == 0u
        || (cluster_sectors & (cluster_sectors - 1u)) != 0u
        || read_u16_le(source_boot_sector + 22u) != 0u
        || read_u32_le(source_boot_sector + 36u) == 0u) {
        console_write("Installer payload does not contain a valid FAT32 boot sector.\n");
        return;
    }
    console_write("[INSTALL] probing target disk...\n");
    if (!disk_read_sector(&disks[disk_index], 0u, sector_buffer)) {
        console_write("Could not read the target disk; installation was not started.\n");
        return;
    }
    if (!confirm_install_target(disk_index)) {
        console_write("Installation cancelled; disk was not modified.\n");
        return;
    }

    console_write("Installing BIOS/UEFI BOOK8 image to ");
    console_write(disks[disk_index].name);
    console_write(". Do not power off during installation.\n");
    console_write("[INSTALL] copying installer image to target...\n");
    uint64_t lba = 0u;
    while (lba < image_sectors) {
        uint32_t count = image_sectors - lba > ATA_BOUNCE_SECTORS
            ? ATA_BOUNCE_SECTORS : (uint32_t)(image_sectors - lba);
        size_t bytes = (size_t)count * 512u;
        __builtin_memcpy(install_transfer_buffer,
            installer_image + (size_t)(lba * 512u), bytes);
        if (!disk_transfer(&disks[disk_index], lba, count,
                install_transfer_buffer, true)) {
            console_write("Install write failed at LBA ");
            print_unsigned(lba);
            console_write("; the disk may be partially installed.\n");
            return;
        }
        lba += count;
        if ((lba % 16384u) == 0u || lba == image_sectors) {
            install_report_progress("installer image copy", lba,
                image_sectors);
        }
    }
    console_write("[INSTALL] flushing copied image and expanding FAT32...\n");
    if (!disk_flush(&disks[disk_index])
        || !install_resize_fat32(&disks[disk_index], INSTALL_ROOT_START_LBA,
            root_sectors)) {
        console_write("FAT32 expansion failed; the target may be partially installed.\n");
        return;
    }
    console_write("[INSTALL] writing installed partition table...\n");
    if (!disk_read_sector(&disks[disk_index], 0u, sector_buffer)) {
        console_write("Could not read the target MBR after FAT32 expansion.\n");
        return;
    }
    install_set_mbr_partition(sector_buffer, 0u, 0x80u, 0x0cu,
        INSTALL_ROOT_START_LBA, root_sectors);
    install_set_mbr_partition(sector_buffer, 1u, 0u, 0x82u, swap_start,
        INSTALL_SWAP_SECTORS);
    sector_buffer[510] = 0x55u;
    sector_buffer[511] = 0xaau;
    if (!disk_write_sector_unflushed(&disks[disk_index], 0u, sector_buffer)
        || !disk_flush(&disks[disk_index])) {
        console_write("Could not write or flush the installed MBR.\n");
        return;
    }
    if (!disk_read_sector(&disks[disk_index], 0u, sector_buffer)
        || sector_buffer[510] != 0x55u || sector_buffer[511] != 0xaau
        || sector_buffer[450] != 0x0cu || sector_buffer[466] != 0x82u
        || read_u32_le(sector_buffer + 454u) != INSTALL_ROOT_START_LBA
        || read_u32_le(sector_buffer + 458u) != root_sectors
        || read_u32_le(sector_buffer + 470u) != swap_start
        || read_u32_le(sector_buffer + 474u) != INSTALL_SWAP_SECTORS) {
        console_write("Install verification failed at the MBR; do not reboot.\n");
        return;
    }
    if (!disk_read_sector(&disks[disk_index], INSTALL_ROOT_START_LBA,
            sector_buffer)
        || sector_buffer[510] != 0x55u || sector_buffer[511] != 0xaau
        || read_u32_le(sector_buffer + 32u) != root_sectors
        || read_u32_le(sector_buffer + 36u) == 0u
        || read_u32_le(sector_buffer + 28u) != INSTALL_ROOT_START_LBA) {
        console_write("Install verification failed at the expanded FAT32 boot sector.\n");
        return;
    }
    console_write("[INSTALL] mounting installed FAT32 root as A:...\n");
    uint32_t filesystem_disk = disk_index
        + (application_image != NULL ? 1u : 0u);
    if (!fs_mount_disk_as_root(filesystem_disk)) {
        console_write("The target was installed, but its FAT32 root could not be mounted; registry was not written.\n");
        return;
    }
    registry_write_allowed = true;
    console_write("[INSTALL] seeding default desktop profile...\n");
    if (!install_seed_default_config()) {
        console_write("The target was installed, but the desktop profile could not be initialized.\n");
        return;
    }
    console_write("[INSTALL] writing initial registry state...\n");
    if (!reg_save()) {
        console_write("The target was installed, but initial registry save failed; retry with REG SAVE.\n");
        return;
    }
    console_write("BOOK8 installed and verified: FAT32 root plus 4 GiB raw swap. Remove the install medium and reboot.\n");
}

static void run_write_command(char *arguments)
{
    char *device_text = skip_spaces(arguments);
    char *cursor = device_text;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        console_write("Usage: WRITE <disk-number> <LBA> <text>\n");
        return;
    }
    *cursor++ = '\0';
    char *lba_text = skip_spaces(cursor);
    cursor = lba_text;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        console_write("Usage: WRITE <disk-number> <LBA> <text>\n");
        return;
    }
    *cursor++ = '\0';
    char *text = skip_spaces(cursor);
    if (*text == '\0') {
        console_write("Write data cannot be empty. Use 1-511 printable ASCII characters.\n");
        return;
    }

    uint32_t device_index = 0u;
    for (const char *p = device_text; *p != '\0'; ++p) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (*p < '0' || *p > '9'
            || device_index > (UINT32_MAX - digit) / 10u) {
            console_write("Invalid disk number.\n");
            return;
        }
        device_index = device_index * 10u + digit;
    }

    uint64_t lba = 0u;
    const uint64_t maximum_lba = (1ull << 48u) - 1u;
    for (const char *p = lba_text; *p != '\0'; ++p) {
        uint64_t digit = (uint64_t)(*p - '0');
        if (*p < '0' || *p > '9'
            || lba > (maximum_lba - digit) / 10u) {
            console_write("Invalid or unsupported LBA.\n");
            return;
        }
        lba = lba * 10u + digit;
    }
    if (device_index >= disk_count || lba >= disks[device_index].sectors) {
        console_write("Disk number or LBA is out of range.\n");
        return;
    }
    if (!disks[device_index].lba48
        && (disks[device_index].ahci || lba >= (1ull << 28u))) {
        console_write("This device does not support writes at the requested LBA.\n");
        return;
    }

    size_t text_length = 0u;
    while (text[text_length] != '\0') {
        if (text_length >= 511u || (uint8_t)text[text_length] < 0x20u
            || (uint8_t)text[text_length] > 0x7eu) {
            console_write("Write data must be 1-511 printable ASCII characters.\n");
            return;
        }
        ++text_length;
    }

    console_write("Disk ");
    print_unsigned(device_index);
    console_write(" (");
    console_write(disks[device_index].name);
    console_write("), LBA ");
    print_unsigned(lba);
    console_write(": text will be followed by zero bytes to fill one sector.\n");
    if (!confirm_text("WARNING: this overwrites one disk sector. Type YES to continue: ")) {
        console_write("Write cancelled. Disk was not modified.\n");
        return;
    }

    zero_memory(sector_buffer, sizeof(sector_buffer));
    for (size_t i = 0u; i < text_length; ++i) {
        sector_buffer[i] = (uint8_t)text[i];
    }
    if (!disk_write_sector(&disks[device_index], lba, sector_buffer)) {
        console_write("Write or cache flush failed; the disk may have accepted the sector. Verify with READ.\n");
        return;
    }
    console_write("One sector written and cache flush completed. Use READ to verify.\n");
}

static void set_environment(char *argument)
{
    char *equals = argument;
    while (*equals != '\0' && *equals != '=') {
        ++equals;
    }
    if (*equals == '\0') {
        if (*argument == '\0') {
            for (uint32_t i = 0u; i < 16u; ++i) {
                if (environment[i].name[0] != '\0') {
                    console_write(environment[i].name);
                    console_putc('=');
                    console_write(environment[i].value);
                    console_putc('\n');
                }
            }
        } else {
            console_write("Usage: SET [NAME=VALUE]\n");
        }
        return;
    }

    *equals = '\0';
    if (*argument == '\0') {
        console_write("Environment variable name cannot be empty.\n");
        return;
    }
    if (ascii_equal(argument, "PATH")) {
        copy_string(search_path, sizeof(search_path), equals + 1);
        console_write("PATH=");
        console_write(search_path);
        console_putc('\n');
        return;
    }
    for (uint32_t i = 0u; i < 16u; ++i) {
        if (environment[i].name[0] == '\0'
            || ascii_equal(environment[i].name, argument)) {
            copy_string(environment[i].name, sizeof(environment[i].name), argument);
            copy_string(environment[i].value, sizeof(environment[i].value), equals + 1);
            return;
        }
    }
    console_write("Environment table is full.\n");
}

static void run_filesystem_command(const char *command)
{
    console_write(command);
    console_write(": no filesystem is mounted. FAT/VFAT/LFN and NTFS support is not implemented.\n");
}

static void run_external_command(const char *command)
{
    console_write(command);
    console_write(": utility is not implemented. No disk operation was performed.\n");
}

static void gui_run_command(char *command)
{
    run_command(command);
}

static void gui_shutdown(void)
{
    console_write("BOOK8 shutdown requested.\n");
    acpi_shutdown();
}

static void gui_reboot(void)
{
    console_write("BOOK8 reboot requested.\n");
    reboot_system();
}

static uint32_t gui_status_flags(void)
{
    uint32_t flags = 0u;
    if (ps2_keyboard_controller_ready || book8_usb_keyboard_active()) {
        flags |= BOOK8GUI_STATUS_KEYBOARD;
    }
    if (ps2_mouse_available || book8_usb_mouse_active()) {
        flags |= BOOK8GUI_STATUS_MOUSE;
    }
    if (disk_count != 0u) {
        flags |= BOOK8GUI_STATUS_STORAGE;
    }
    if (pci_network_present) {
        flags |= BOOK8GUI_STATUS_NETWORK;
    }
    if (pci_sound_present) {
        flags |= BOOK8GUI_STATUS_SOUND;
    }
    return flags;
}

static bool gui_network_activity(uint32_t *tx, uint32_t *rx)
{
    if (!network_ready)
        return false;
    *tx = rtl_nic.tx_packets;
    *rx = rtl_nic.rx_packets;
    return true;
}

static uint32_t gui_month_days(uint32_t year, uint32_t month)
{
    static const uint32_t days[] = {31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u};
    return days[month - 1u] + (month == 2u && year % 4u == 0u
        && (year % 100u != 0u || year % 400u == 0u) ? 1u : 0u);
}

static bool gui_shift_time(struct book8gui_time *time, int64_t minutes)
{
    if (time->month < 1u || time->month > 12u
        || time->day < 1u || time->day > gui_month_days(time->year, time->month)) {
        return false;
    }
    minutes += (int64_t)time->hour * 60 + time->minute;
    while (minutes < 0) {
        minutes += 1440;
        if (--time->day == 0u) {
            if (--time->month == 0u) { time->month = 12u; --time->year; }
            time->day = (uint8_t)gui_month_days(time->year, time->month);
        }
    }
    while (minutes >= 1440) {
        minutes -= 1440;
        if (++time->day > gui_month_days(time->year, time->month)) {
            time->day = 1u;
            if (++time->month > 12u) { time->month = 1u; ++time->year; }
        }
    }
    time->hour = (uint8_t)(minutes / 60);
    time->minute = (uint8_t)(minutes % 60);
    return time->year >= 1980u && time->year <= 2099u;
}

static bool gui_read_time(struct book8gui_time *time)
{
    struct clock_time now = read_clock();
    time->hour = now.hour;
    time->minute = now.minute;
    time->day = now.day;
    time->month = now.month;
    time->year = now.year;
    int64_t minutes = (int32_t)book8_config_value(BOOK8_CFG_TIMEZONE) * 15 - 720;
    if (software_clock_active) {
        *time = software_clock;
        if (tsc_hz != 0u) {
            minutes += (int64_t)((read_tsc() - software_clock_start) / tsc_hz / 60u);
        }
    }
    return gui_shift_time(time, minutes);
}

static void cmos_write(uint8_t register_index, uint8_t value)
{
    outb(0x70u, register_index);
    outb(0x71u, value);
}

static bool gui_set_time(const struct book8gui_time *time)
{
    static const uint8_t month_days[12] =
        {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (time->hour > 23u || time->minute > 59u || time->month < 1u
        || time->month > 12u || time->year < 1980u || time->year > 2099u) {
        return false;
    }
    bool leap = (time->year % 4u == 0u && time->year % 100u != 0u)
        || time->year % 400u == 0u;
    uint8_t days = month_days[time->month - 1u];
    if (time->month == 2u && leap) {
        days = 29u;
    }
    if (time->day < 1u || time->day > days) {
        return false;
    }
    struct book8gui_time utc = *time;
    if (!gui_shift_time(&utc,
        720 - (int32_t)book8_config_value(BOOK8_CFG_TIMEZONE) * 15)) {
        return false;
    }
    if (book8_config_value(BOOK8_CFG_RTC_SYNC) == 0u) {
        if (tsc_hz == 0u) { return false; }
        software_clock = utc;
        software_clock_start = read_tsc();
        software_clock_active = true;
        return true;
    }
    time = &utc;
    software_clock_active = false;

    uint8_t mode = cmos_read(0x0bu);
    uint8_t second = 0u;
    uint8_t minute = time->minute;
    uint8_t hour = time->hour;
    uint8_t day = time->day;
    uint8_t month = time->month;
    uint8_t year = (uint8_t)(time->year % 100u);
    uint8_t century = (uint8_t)(time->year / 100u);
    if ((mode & 0x02u) == 0u) {
        bool pm = hour >= 12u;
        hour = (uint8_t)(hour % 12u);
        if (hour == 0u) {
            hour = 12u;
        }
        if (pm) {
            hour |= 0x80u;
        }
    }
    if ((mode & 0x04u) == 0u) {
        uint8_t high = (uint8_t)(hour & 0x80u);
        hour &= 0x7fu;
        hour = (uint8_t)(((hour / 10u) << 4u) | (hour % 10u) | high);
        minute = (uint8_t)(((minute / 10u) << 4u) | (minute % 10u));
        day = (uint8_t)(((day / 10u) << 4u) | (day % 10u));
        month = (uint8_t)(((month / 10u) << 4u) | (month % 10u));
        year = (uint8_t)(((year / 10u) << 4u) | (year % 10u));
        century = (uint8_t)(((century / 10u) << 4u) | (century % 10u));
    }
    /* SET bit freezes the update cycle while the fields are rewritten. */
    cmos_write(0x0bu, (uint8_t)(mode | 0x80u));
    cmos_write(0x00u, second);
    cmos_write(0x02u, minute);
    cmos_write(0x04u, hour);
    cmos_write(0x07u, day);
    cmos_write(0x08u, month);
    cmos_write(0x09u, year);
    cmos_write(0x32u, century);
    cmos_write(0x0bu, (uint8_t)(mode & 0x7fu));
    return true;
}

static bool gui_usb_mouse_active(void)
{
    return book8_usb_mouse_active();
}

static bool gui_list_files(const char *path,
    book8_directory_entry_fn visitor, void *context)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_list_directory(path, visitor, context);
}

static bool gui_file_info(const char *path, struct book8_file_info *info)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_file_info(path, info);
}

static bool gui_rename_entry(const char *path, const char *new_name)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_rename_entry(path, new_name);
}

static bool gui_delete_entry(const char *path)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_delete_entry(path);
}

static bool gui_copy_to_directory(const char *source, const char *directory,
    bool cut)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ
        | BOOK8_SESSION_WRITE)
        && fs_copy_to_directory(source, directory, cut);
}

static bool gui_make_directory(const char *path)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_make_directory(path);
}

static bool gui_create_file(const char *path, const void *data, uint32_t size)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_create_file(path, data, size);
}

static bool gui_read_cluster(char drive, uint32_t cluster, void *data,
    uint32_t capacity, uint32_t *size)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_read_cluster(drive, cluster, data, capacity, size);
}

static uint32_t gui_session_uid(void)
{
    struct book8_session_info info;
    return book8_session_current(&info) ? info.uid : UINT32_MAX;
}

static uint64_t gui_session_token(void)
{
    struct book8_session_info info;
    return book8_session_current(&info) ? info.token : 0u;
}

static uint32_t gui_drive_count(void)
{
    return fs_drive_count();
}

static char gui_drive_letter(uint32_t index)
{
    return fs_drive_letter(index);
}

static const char *gui_last_error_text(void)
{
    return fs_error_text(fs_last_error());
}

static void gui_diagnostic(const char *message)
{
    while (*message != '\0') {
        control_log_char(*message);
        serial_putc(*message++);
    }
    control_log_char('\n');
}

static bool gui_save_config(const struct book8_system_config *config);

static void gui_apply_registry_config(struct book8_system_config *config)
{
    static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    uint32_t value;
    char theme[BOOK8_REGISTRY_VALUE_MAX];
    if (reg_get_u32(path, "KeyRate", &value)
        && value >= book8_config_ranges[BOOK8_CFG_REPEAT_RATE].min
        && value <= book8_config_ranges[BOOK8_CFG_REPEAT_RATE].max) {
        config->values[BOOK8_CFG_REPEAT_RATE] = value;
    }
    if (reg_get_u32(path, "KeyDelay", &value) && value >= 250u
        && value <= 1000u && value % 250u == 0u) {
        config->values[BOOK8_CFG_KEY_DELAY] = value / 250u - 1u;
    }
    if (reg_get_u32(path, "MouseSpeed", &value)
        && value >= book8_config_ranges[BOOK8_CFG_POINTER_SPEED].min
        && value <= book8_config_ranges[BOOK8_CFG_POINTER_SPEED].max) {
        config->values[BOOK8_CFG_POINTER_SPEED] = value;
    }
    if (reg_get_u32(path, "DblClickMs", &value)
        && value >= book8_config_ranges[BOOK8_CFG_DOUBLE_CLICK].min
        && value <= book8_config_ranges[BOOK8_CFG_DOUBLE_CLICK].max) {
        config->values[BOOK8_CFG_DOUBLE_CLICK] = value;
    }
    if (reg_get_str(path, "Theme", theme, sizeof(theme))) {
        if (strcmp(theme, "Dark") == 0) { config->values[BOOK8_CFG_DARK] = 1u; }
        else if (strcmp(theme, "Light") == 0) { config->values[BOOK8_CFG_DARK] = 0u; }
    }
    if (reg_get_u32(path, "Accent", &value) && value < 8u) {
        config->values[BOOK8_CFG_ACCENT] = value;
    }
    if (reg_get_u32(path, "Volume", &value) && value <= 100u
        && value % 5u == 0u) {
        config->values[BOOK8_CFG_VOLUME] = value;
    }
    if (reg_get_u32(path, "Tone", &value)
        && value >= book8_config_ranges[BOOK8_CFG_TONE].min
        && value <= book8_config_ranges[BOOK8_CFG_TONE].max) {
        config->values[BOOK8_CFG_TONE] = value;
    }
    if (!book8_config_valid(config)) {
        book8_config_defaults(config);
    }
}

static void gui_load_config(void)
{
    if (config_loaded) { return; }
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    uint32_t size = 0u;
    struct book8_system_config loaded;
    bool seed_defaults = false;
    if (!fs_read_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes), &size)) {
        int error = fs_last_error();
        snprintf(config_status, sizeof(config_status), "%s",
            error == BOOK8_FS_ERR_NOT_FOUND ? "No config file; defaults active."
            : "Config read failed; defaults active.");
        seed_defaults = error == BOOK8_FS_ERR_NOT_FOUND;
        book8_config_defaults(&loaded);
        (void)book8_config_commit(&loaded);
    } else if (!book8_config_decode(&loaded, bytes, size)) {
        snprintf(config_status, sizeof(config_status), "Invalid config file; defaults active.");
        book8_config_defaults(&loaded);
        (void)book8_config_commit(&loaded);
        seed_defaults = true;
    } else {
        (void)book8_config_commit(&loaded);
        snprintf(config_status, sizeof(config_status), "Loaded A:\\BOOK8.CFG.");
    }
    gui_apply_registry_config(&loaded);
    (void)book8_config_commit(&loaded);
    console_write("BOOK8-CONFIG: ");
    console_write(config_status);
    console_write("\n");
    char config_diagnostic[160];
    snprintf(config_diagnostic, sizeof(config_diagnostic),
        "BOOK8-CONFIG: %s\n", config_status);
    gui_diagnostic(config_diagnostic);
    console_write("BOOK8-CONFIG: active resolution=");
    print_unsigned(book8_config_value(BOOK8_CFG_RESOLUTION));
    console_write(" pointer-speed=");
    print_unsigned(book8_config_value(BOOK8_CFG_POINTER_SPEED));
    console_write(" double-click=");
    print_unsigned(book8_config_value(BOOK8_CFG_DOUBLE_CLICK));
    console_write(" swapped=");
    print_unsigned(book8_config_value(BOOK8_CFG_SWAP_BUTTONS));
    console_write("\n");
    config_loaded = true;
    if (seed_defaults) {
        console_write("BOOK8-CONFIG: generating a default profile.\n");
        (void)gui_save_config(&loaded);
    }
}

static bool gui_save_config(const struct book8_system_config *config)
{
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    char theme[8];
    if (!book8_config_encode(config, bytes)) {
        console_write("BOOK8-CONFIG: invalid preferences; not saved.\n");
        return false;
    }
    (void)snprintf(theme, sizeof(theme), "%s",
        config->values[BOOK8_CFG_DARK] != 0u ? "Dark" : "Light");
    if (!reg_set_u32(path, "KeyRate", config->values[BOOK8_CFG_REPEAT_RATE])
        || !reg_set_u32(path, "KeyDelay",
            (config->values[BOOK8_CFG_KEY_DELAY] + 1u) * 250u)
        || !reg_set_u32(path, "MouseSpeed",
            config->values[BOOK8_CFG_POINTER_SPEED])
        || !reg_set_u32(path, "DblClickMs",
            config->values[BOOK8_CFG_DOUBLE_CLICK])
        || !reg_set_str(path, "Theme", theme)
        || !reg_set_u32(path, "Accent", config->values[BOOK8_CFG_ACCENT])
        || !reg_set_u32(path, "Volume", config->values[BOOK8_CFG_VOLUME])
        || !reg_set_u32(path, "Tone", config->values[BOOK8_CFG_TONE])
        || !reg_save()) {
        console_write("BOOK8-CONFIG: registry save failed; preferences unchanged.\n");
        return false;
    }
    if (!fs_write_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes))) {
        console_write("BOOK8-CONFIG: save failed: ");
        console_write(fs_error_text(fs_last_error()));
        console_write("\n");
        return false;
    }
    snprintf(config_status, sizeof(config_status), "Saved A:\\BOOK8.CFG.");
    gui_diagnostic("BOOK8-CONFIG: saved A:\\BOOK8.CFG.\n");
    return true;
}

static bool gui_test_tone(uint32_t frequency)
{
    if (frequency < 100u || frequency > 2000u || tsc_hz == 0u
        || book8_config_value(BOOK8_CFG_MUTE) != 0u
        || book8_config_value(BOOK8_CFG_VOLUME) == 0u) {
        return false;
    }
    uint32_t divisor = PIT_HZ / frequency;
    outb(0x43u, 0xb6u);
    outb(0x42u, (uint8_t)divisor);
    outb(0x42u, (uint8_t)(divisor >> 8u));
    outb(0x61u, (uint8_t)(inb(0x61u) | 3u));
    speaker_stop = read_tsc() + tsc_hz / 5u;
    return true;
}

static bool gui_audio_state(uint8_t *volume, bool *muted)
{
    if (volume == NULL || muted == NULL || hda_api == NULL
        || !hda_api->is_available()) {
        return false;
    }
    *volume = hda_api->get_volume();
    *muted = hda_api->get_mute();
    return true;
}

static bool gui_audio_set_volume(uint8_t volume)
{
    return hda_api != NULL && hda_api->set_volume(volume) == HDA_OK;
}

static bool gui_audio_set_mute(bool muted)
{
    return hda_api != NULL && hda_api->set_mute(muted) == HDA_OK;
}

static bool gui_media_play_wav(const uint8_t *wav, uint32_t size)
{
    if (hda_api == NULL)
        return false;
    int32_t result = hda_api->play_wav(wav, size);
    if (result != HDA_STREAM_OK) {
        serial_write("BOOK8-HDA: WAV playback failed: ");
        serial_write(hda_api->stream_result_name(result));
        serial_write("\r\n");
        return false;
    }
    return true;
}

static bool gui_media_pause(bool paused)
{
    return hda_api != NULL
        && hda_api->pause(paused) == HDA_STREAM_OK;
}

static void gui_media_stop(void)
{
    if (hda_api != NULL)
        hda_api->stop();
}

static bool gui_media_seek(uint32_t position_ms)
{
    return hda_api != NULL
        && hda_api->seek(position_ms) == HDA_STREAM_OK;
}

static bool gui_media_state(struct book8gui_media_status *status)
{
    if (status == NULL || hda_api == NULL || !hda_api->is_available()) {
        return false;
    }
    struct hda_stream_info info;
    hda_api->get_stream_info(&info);
    status->position_ms = info.position_ms;
    status->duration_ms = info.duration_ms;
    status->loaded = info.loaded;
    status->playing = info.playing;
    status->paused = info.paused;
    status->failed = info.failed;
    return true;
}

static void gui_media_service(void)
{
    if (hda_api == NULL)
        return;
    int32_t result = hda_api->service();
    if (result != HDA_STREAM_OK && result != HDA_STREAM_UNAVAILABLE
        && result != HDA_STREAM_BAD_ARGUMENT) {
        serial_write("BOOK8-HDA: WAV stream stopped: ");
        serial_write(hda_api->stream_result_name(result));
        serial_write("\r\n");
    }
}

static bool gui_system_info(struct book8gui_sysinfo *info);

static bool gui_control_readout(enum book8_control_page page, uint32_t row,
    char *text, uint32_t capacity, uint32_t *bar)
{
    *bar = UINT32_MAX;
    struct book8gui_sysinfo info;
    heap_stats_t heap;
    if (page == BOOK8_PANEL_ABOUT || page == BOOK8_PANEL_ADMIN) {
        heap_get_stats(&heap);
    }
    if (page == BOOK8_PANEL_TIME) {
        if (row > 1u) { return false; }
        snprintf(text, capacity, "%s", row == 0u
            ? "Clock: RTC assumed UTC; fields are local."
            : "RTC sync off: session-only software clock.");
    } else if (page == BOOK8_PANEL_KEYBOARD) {
        if (row > 1u) { return false; }
        snprintf(text, capacity, "%s", row == 0u
            ? "Input: PS/2 / USB polling" : "Repeat/delay: saved hardware requests.");
    } else if (page == BOOK8_PANEL_MOUSE) {
        if (row > 0u) { return false; }
        snprintf(text, capacity, "Speed/buttons active; absolute input unscaled.");
    } else if (page == BOOK8_PANEL_DISPLAY) {
        if (row == 0u) {
            snprintf(text, capacity, "Canvas: %ux%ux%u pitch %u", screen.width,
                screen.height, screen.bytes_per_pixel * 8u, screen.pitch_bytes);
        } else if (row == 1u) {
            snprintf(text, capacity, "Presentation: %s", fbdev.draw != fbdev.front
                ? "buffered copy" : "direct drawing");
        } else if (row == 2u) {
            snprintf(text, capacity, "Firmware scanout: %ux%u; no GPU modesetting.",
                screen.scanout_width, screen.scanout_height);
        } else { return false; }
    } else if (page == BOOK8_PANEL_NETWORK) {
        if (row > 4u) { return false; }
        if (row == 0u && rtl_nic.initialized) {
            snprintf(text, capacity, "RTL8168 MAC %02X:%02X:%02X:%02X:%02X:%02X",
                (unsigned)rtl_nic.mac[0], (unsigned)rtl_nic.mac[1],
                (unsigned)rtl_nic.mac[2], (unsigned)rtl_nic.mac[3],
                (unsigned)rtl_nic.mac[4], (unsigned)rtl_nic.mac[5]);
            return true;
        }
        if (row >= 2u && kernel_net.ip != 0u) {
            uint32_t address = row == 2u ? kernel_net.ip
                : row == 3u ? kernel_net.mask : kernel_net.gateway;
            snprintf(text, capacity, "%s: %u.%u.%u.%u",
                row == 2u ? "IP" : row == 3u ? "Subnet" : "Gateway",
                (unsigned)(address >> 24u), (unsigned)((address >> 16u) & 255u),
                (unsigned)((address >> 8u) & 255u), (unsigned)(address & 255u));
            return true;
        }
        const char *lines[] = {
            pci_network_present ? "PCI network adapter detected" : "PCI network adapter: none detected",
            "Link: not sampled", "IP default: 0.0.0.0",
            "Subnet default: 0.0.0.0", "Gateway default: 0.0.0.0"
        };
        snprintf(text, capacity, "%s", lines[row]);
    } else if (page == BOOK8_PANEL_STORAGE) {
        if (row == 0u) {
            snprintf(text, capacity, "Root A:; %u disks / %u volumes", disk_count, fs_drive_count());
        } else if (row <= fs_drive_count()) {
            const char *type = "";
            bool readable = false;
            struct book8_drive_space space;
            if (!fs_drive_info(row - 1u, &type, &readable)
                || !fs_drive_space(row - 1u, &space)) {
                snprintf(text, capacity, "Drive sample failed.");
            } else {
                if (space.state == BOOK8_SPACE_READY) {
                    uint64_t used = space.total_bytes - space.free_bytes;
                    *bar = space.total_bytes != 0u
                        ? (uint32_t)(used * 1000u / space.total_bytes) : 0u;
                    snprintf(text, capacity, "%c: %s %llu used / %llu free KiB",
                        fs_drive_letter(row - 1u), space.mounted ? "mounted" : type,
                        (unsigned long long)(used / 1024u),
                        (unsigned long long)(space.free_bytes / 1024u));
                } else {
                    snprintf(text, capacity, "%c: %s %llu KiB %s",
                        fs_drive_letter(row - 1u), readable ? type : "NOFS",
                        (unsigned long long)(space.total_bytes / 1024u),
                        space.state == BOOK8_SPACE_PENDING ? "sampling" :
                        space.state == BOOK8_SPACE_ERROR ? "read error" : "capacity only");
                }
            }
        } else if (row == fs_drive_count() + 1u) {
            snprintf(text, capacity, "Bars: used clusters; sampling is read-only.");
        } else if (row == fs_drive_count() + 2u) {
            snprintf(text, capacity, "USB mass-storage inventory: no driver");
        } else { return false; }
    } else if (page == BOOK8_PANEL_SOUND) {
        if (row > 1u) { return false; }
        snprintf(text, capacity, "%s", row == 0u
            ? (pci_sound_present ? "PCI audio controller detected" : "Audio: PC speaker")
            : "Mixer request saved; speaker uses mute/zero.");
    } else if (page == BOOK8_PANEL_ABOUT) {
        (void)gui_system_info(&info);
        if (row == 0u) { snprintf(text, capacity, "%s", BOOK8_GUI_VERSION); }
        else if (row == 1u) { snprintf(text, capacity, "x86-64: %.40s", info.cpu_name); }
        else if (row == 2u) { snprintf(text, capacity, "%u logical CPUs / BSP active", info.cpu_threads); }
        else if (row == 3u) { snprintf(text, capacity, "Physical RAM: %llu KiB", (unsigned long long)info.memory_total_kb); }
        else if (row == 4u) {
            snprintf(text, capacity, "Heap: %llu / %llu bytes", (unsigned long long)heap.used,
                (unsigned long long)heap.total);
            *bar = heap.total != 0u ? (uint32_t)(heap.used * 1000u / heap.total) : 0u;
        } else if (row == 5u) { snprintf(text, capacity, "Uptime: %llu seconds", (unsigned long long)info.uptime_seconds); }
        else if (row == 6u) { snprintf(text, capacity, "%s", config_status); }
        else { return false; }
    } else if (page == BOOK8_PANEL_ADMIN) {
        if (row == 0u) {
            snprintf(text, capacity, "Heap range: 0x%llx - 0x%llx",
                (unsigned long long)heap.start, (unsigned long long)heap.end);
        } else if (row == 1u) {
            snprintf(text, capacity, "Heap free: %llu bytes", (unsigned long long)heap.free_bytes);
            *bar = heap.total != 0u ? (uint32_t)(heap.used * 1000u / heap.total) : 0u;
        } else if (row == 2u) {
            snprintf(text, capacity, "NV BAR0: 0x%llx / BOOT0: %08x",
                (unsigned long long)nv_gpu.bar0_phys, nv_gpu.boot0);
        } else if (row == 3u) {
            snprintf(text, capacity, "NV present=%u mapped=%u (cached probe)", nv_gpu.present, nv_gpu.mapped);
        } else if (row < 4u + control_memory_count) {
            uint32_t i = row - 4u;
            snprintf(text, capacity, "Map %u: %llx +%llx type %u", i,
                (unsigned long long)control_memory_map[i].base,
                (unsigned long long)control_memory_map[i].size, control_memory_map[i].type);
        } else if (row < 12u + control_memory_count) {
            uint32_t i = (control_log_line + 1u + row - 4u - control_memory_count) % 8u;
            snprintf(text, capacity, "Log: %.80s", control_log[i]);
        } else { return false; }
    } else { return false; }
    return true;
}

/* EXEC <path> [args...]: load an ELF64 from a FAT volume and run it in Ring 3. */
static void run_exec(char *line)
{
    char *argv[EXEC_MAX_ARGS];
    int argc = 0;
    long status;

    while (*line != '\0' && argc < (int)EXEC_MAX_ARGS) {
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ') {
            ++line;
        }
        if (*line != '\0') {
            *line++ = '\0';
            line = skip_spaces(line);
        }
    }
    if (*line != '\0') {
        console_write("EXEC: too many arguments (maximum 16)\n");
        return;
    }
    if (argc == 0) {
        console_write("usage: EXEC <path> [args]\n");
        return;
    }
    status = exec_run(argv[0], argc, argv);
    stdio_set_output(console_putc);
    if (status == -2) {
        printf("EXEC: %s: file not found\n", argv[0]);
    } else if (status < 0 && status > -4096) {
        printf("EXEC: cannot execute %s (error %d)\n", argv[0], (int)-status);
    } else {
        printf("EXEC: %s exited with status %d\n", argv[0], (int)status);
    }
    if (book8gui_utility_pending()) {
        if (status != 0) { book8gui_cancel_pending_utility(); }
        else if (gui_active) { (void)book8gui_open_pending_utility(); }
        else { gui_launch(false); }
    }
}

static uint64_t boot_tsc;
static uint64_t memory_total_kb;
extern char __kernel_end[];

static inline uint64_t read_tsc(void)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32u) | low;
}

static void cpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
    uint32_t *d)
{
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0u));
}

static void tsc_calibrate(void)
{
    uint64_t pit_start = pit_clock_ticks();
    uint64_t tsc_start = read_tsc();
    uint64_t elapsed = 0u;
    for (uint32_t spin = 0u; elapsed < PIT_HZ / 20u && spin < 20000000u;
        ++spin) {
        elapsed = pit_clock_ticks() - pit_start;
    }
    if (elapsed != 0u) {
        tsc_hz = (read_tsc() - tsc_start) * PIT_HZ / elapsed;
    }
}

static bool gui_system_info(struct book8gui_sysinfo *info)
{
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    info->cpu_name[0] = '\0';
    info->cpu_threads = 1u;
    cpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        uint32_t words[12];
        for (uint32_t leaf = 0u; leaf < 3u; ++leaf) {
            cpu_id(0x80000002u + leaf, &words[leaf * 4u],
                &words[leaf * 4u + 1u], &words[leaf * 4u + 2u],
                &words[leaf * 4u + 3u]);
        }
        const char *raw = (const char *)words;
        uint32_t start = 0u;
        while (start < 48u && raw[start] == ' ') {
            ++start;
        }
        uint32_t n = 0u;
        for (; start < 48u && raw[start] != '\0'; ++start) {
            info->cpu_name[n++] = raw[start];
        }
        while (n != 0u && info->cpu_name[n - 1u] == ' ') {
            --n;
        }
        info->cpu_name[n] = '\0';
    }
    cpu_id(1u, &a, &b, &c, &d);
    if ((d & (1u << 28u)) != 0u && ((b >> 16u) & 0xffu) != 0u) {
        info->cpu_threads = (b >> 16u) & 0xffu;
    }
    cpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000008u) {
        cpu_id(0x80000008u, &a, &b, &c, &d);
        if ((c & 0xffu) + 1u > info->cpu_threads) {
            info->cpu_threads = (c & 0xffu) + 1u;
        }
    }
    info->cpu_mhz = (uint32_t)(tsc_hz / 1000000u);
    info->memory_total_kb = memory_total_kb;
    info->memory_used_kb = (uint64_t)(uintptr_t)__kernel_end / 1024u;
    if (info->memory_total_kb != 0u
        && info->memory_used_kb > info->memory_total_kb) {
        info->memory_used_kb = info->memory_total_kb;
    }
    info->uptime_seconds = tsc_hz != 0u
        ? (read_tsc() - boot_tsc) / tsc_hz : 0u;
    info->disk_read_bytes = disk_read_bytes_total;
    info->disk_write_bytes = disk_write_bytes_total;
    info->disk_busy_ticks = disk_busy_ticks_total;
    info->disk_count = disk_count;
    info->disk_total_kb = 0u;
    info->disk_label[0] = '\0';
    for (uint32_t i = 0u; i < disk_count; ++i) {
        info->disk_total_kb += disks[i].sectors / 2u;
    }
    if (disk_count != 0u) {
        uint32_t n = 0u;
        const char *model = disks[0].model;
        for (; model[n] != '\0' && n + 1u < sizeof(info->disk_label); ++n) {
            info->disk_label[n] = model[n];
        }
        info->disk_label[n] = '\0';
    }
    return true;
}

static void gui_display_size(uint32_t *width, uint32_t *height)
{
    *width = screen.width;
    *height = screen.height;
}

static bool gui_set_display_mode(uint32_t width, uint32_t height)
{
    uint32_t old_width = screen.width;
    uint32_t old_height = screen.height;

    if (screen.pixels == NULL
        || !fb_set_logical_mode(&fbdev, width, height, gui_backbuffer,
            sizeof(gui_backbuffer))) {
        return false;
    }
    screen.width = width;
    screen.height = height;
    screen.pitch_bytes = fbdev.pitch;
    if (old_width != 0u && old_height != 0u
        && pointer_x >= 0 && pointer_y >= 0) {
        pointer_x = (int32_t)((uint64_t)(uint32_t)pointer_x * width
            / old_width);
        pointer_y = (int32_t)((uint64_t)(uint32_t)pointer_y * height
            / old_height);
    } else {
        pointer_x = (int32_t)(width / 2u);
        pointer_y = (int32_t)(height / 2u);
    }
    if ((uint32_t)pointer_x >= width) {
        pointer_x = (int32_t)(width - 1u);
    }
    if ((uint32_t)pointer_y >= height) {
        pointer_y = (int32_t)(height - 1u);
    }
    return true;
}

static bool gui_nvdiag_snapshot(struct book8_nvdiag_snapshot *snapshot)
{
    return hwdiag_snapshot(snapshot) == 0;
}

static struct book8_net *gui_share_network(void)
{
    return !cmdline_network_off && book8_config_value(BOOK8_CFG_NETWORK)
        ? book8_kernel_network() : NULL;
}

static bool gui_launch_program(const char *path)
{
    char target[BOOK8GUI_PROGRAM_PATH_SIZE];
    int length = snprintf(target, sizeof(target), "%s", path);
    if (length < 0 || (uint32_t)length >= sizeof(target)) {
        console_write("EXEC: program path exceeds launcher capacity.\n");
        return false;
    }
    char *argv[] = {target};
    long status = exec_run(target, 1, argv);
    stdio_set_output(console_putc);
    if (status < 0 && status > -4096) {
        book8gui_cancel_pending_utility();
        console_write("EXEC: cannot execute ");
        console_write(path);
        printf(" (error %d)\n", (int)-status);
        return false;
    }
    console_write("EXEC: ");
    console_write(path);
    printf(" exited with status %d\n", (int)status);
    if (status != 0) { book8gui_cancel_pending_utility(); return true; }
    return book8gui_open_pending_utility();
}

long book8_gui_utility_request(const struct book8_gui_utility_request *request)
{
    if (screen.pixels == NULL) { return -BOOK8_ENODEV; }
    return book8gui_queue_utility(request->kind, request->path);
}

static bool gui_launch_utility(uint32_t kind, const char *document)
{
    const char *path = kind == BOOK8_GUI_HEXVIEW ? "A:\\BIN\\HEXVIEW.ELF"
        : kind == BOOK8_GUI_CODEEDIT ? "A:\\BIN\\CODEEDIT.ELF"
        : kind == BOOK8_GUI_NETSHARE ? "A:\\BIN\\NETSHARE.ELF"
        : kind == BOOK8_GUI_PAINTBOOK ? "A:\\BIN\\PAINTBK.ELF"
        : kind == BOOK8_GUI_BROWSER ? "A:\\BIN\\BROWSER.ELF"
        : "A:\\BIN\\NVDIAG.ELF";
    char target[256], file[256];
    snprintf(target, sizeof(target), "%s", path);
    snprintf(file, sizeof(file), "%s", document != NULL ? document : "");
    char *argv[] = {target, file};
    long status = exec_run(target, file[0] != '\0' ? 2 : 1, argv);
    stdio_set_output(console_putc);
    if (status != 0) {
        book8gui_cancel_pending_utility();
        printf("GUI utility: %s launch failed (status %d)\n", path, (int)status);
        return false;
    }
    if (!book8gui_utility_pending()) {
        printf("GUI utility: %s did not request a window\n", path);
        return false;
    }
    return book8gui_open_pending_utility();
}

static bool gui_drive_readable(uint32_t index)
{
    bool readable = false;
    return fs_drive_info(index, NULL, &readable) && readable;
}

static bool gui_program_drive_readable(uint32_t index)
{
    if (index != 0u || !gui_drive_readable(index)) { return false; }
    if (application_image != NULL) { return true; }
    struct book8_file_info info;
    return fs_file_info("A:\\BOOT\\BOOK8.ELF", &info) && !info.directory;
}

static uint32_t gui_thread_snapshot(struct book8gui_thread_info *out,
    uint32_t capacity)
{
    struct sched_thread_info threads[SCHED_MAX_THREADS];
    uint32_t count = sched_snapshot(threads, SCHED_MAX_THREADS);
    uint32_t copied = count < capacity ? count : capacity;

    for (uint32_t i = 0u; out != NULL && i < copied; ++i) {
        out[i].tid = threads[i].tid;
        out[i].cpu_ticks = threads[i].cpu_ticks;
        copy_string(out[i].name, sizeof(out[i].name), threads[i].name);
        copy_string(out[i].state, sizeof(out[i].state),
            sched_state_name(threads[i].state));
    }
    return count;
}

static void gui_launch(bool quiet)
{
    if (gui_active) {
        if (!quiet) {
            console_write("BOOK8GUI is already running.\n");
        }
        return;
    }
    if (screen.pixels == NULL) {
        if (!quiet) {
            console_write("BOOK8GUI requires a supported RGB framebuffer.\n");
        }
        return;
    }
    if (quiet) {
        serial_write("BOOK8-BOOT: automatic desktop initialization started\r\n");
    }
    gui_load_config();
    if (quiet) {
        serial_write("BOOK8-BOOT: desktop profile initialization complete\r\n");
    }
    static const uint32_t mode_widths[] = {800u, 1024u, 1440u};
    static const uint32_t mode_heights[] = {600u, 768u, 900u};
    uint32_t mode = book8_config_value(BOOK8_CFG_RESOLUTION);
    if (mode >= sizeof(mode_widths) / sizeof(mode_widths[0])
        || !gui_set_display_mode(mode_widths[mode], mode_heights[mode])) {
        console_write("BOOK8GUI: saved logical display mode unavailable; using current mode.\n");
    }
    struct book8gui_platform platform = {
        .width = screen.width,
        .height = screen.height,
        .fill_rect = gui_fill_rect,
        .draw_rgba = gui_draw_rgba,
        .draw_text = gui_draw_text,
        .pump_input = gui_pump_input,
        .poll_event = gui_poll_event,
        .usb_mouse_active = gui_usb_mouse_active,
        .run_command = gui_run_command,
        .shutdown = gui_shutdown,
        .reboot = gui_reboot,
        .status_flags = gui_status_flags,
        .network_activity = gui_network_activity,
        .audio_state = gui_audio_state,
        .audio_set_volume = gui_audio_set_volume,
        .audio_set_mute = gui_audio_set_mute,
        .media_play_wav = gui_media_play_wav,
        .media_pause = gui_media_pause,
        .media_stop = gui_media_stop,
        .media_seek = gui_media_seek,
        .media_state = gui_media_state,
        .media_service = gui_media_service,
        .read_time = gui_read_time,
        .set_time = gui_set_time,
        .system_info = gui_system_info,
        .session_uid = gui_session_uid,
        .session_token = gui_session_token,
        .thread_snapshot = gui_thread_snapshot,
        .read_file = gui_read_file,
        .write_file = gui_write_file,
        .draw_text_scaled = gui_draw_text_scaled,
        .list_directory = gui_list_files,
        .file_info = gui_file_info,
        .rename_entry = gui_rename_entry,
        .delete_entry = gui_delete_entry,
        .copy_to_directory = gui_copy_to_directory,
        .drive_count = gui_drive_count,
        .drive_letter = gui_drive_letter,
        .drive_readable = gui_drive_readable,
        .program_drive_readable = gui_program_drive_readable,
        .last_error_text = gui_last_error_text,
        .diagnostic = gui_diagnostic,
        .clock_ticks = pit_clock_ticks,
        .clock_hz = PIT_HZ,
        .present = NULL,
        .cursor_save_under = gui_cursor_save_under,
        .cursor_restore_under = gui_cursor_restore_under,
        .present_rect = gui_present_rect,
        .save_config = gui_save_config,
        .control_readout = gui_control_readout,
        .test_tone = gui_test_tone,
        .set_display_mode = gui_set_display_mode,
        .display_size = gui_display_size,
        .launch_program = gui_launch_program,
        .launch_utility = gui_launch_utility,
        .nvdiag_snapshot = gui_nvdiag_snapshot,
        .read_cluster = gui_read_cluster,
        .network = gui_share_network,
        .make_directory = gui_make_directory,
        .create_file = gui_create_file
    };
    if (!quiet) {
        console_write("Launching BOOK8GUI. Use Start > Shutdown > Exit to return to the shell.\n");
    }
    gui_active = true;
    gui_cursor_under_valid = false;
    startup_quiet = false;
    pit_clock_start();
    (void)pit_clock_ticks();
    if (tsc_hz == 0u) {
        tsc_calibrate();
    }
    bool double_buffered = fbdev.draw != fbdev.front;
    if (double_buffered) {
        platform.present = gui_present;
    } else {
        console_write("BOOK8GUI: surface exceeds backbuffer; direct drawing active.\n");
    }
    bool mouse_irq = ps2_mouse_irq_enable();
    if (ps2_mouse_available && !mouse_irq) {
        console_write("PS/2 IRQ12 unavailable; mouse polling fallback active.\n");
    }
    bool started = book8gui_run(&platform);
    if (speaker_stop != 0u) {
        outb(0x61u, (uint8_t)(inb(0x61u) & ~3u));
        speaker_stop = 0u;
    }
    bool mouse_irq_disable_failed = mouse_irq
        && !ps2_mouse_irq_disable();
    gui_active = false;
    startup_quiet = false;
    console_clear();
    console_write(started ? "BOOK8GUI closed.\n"
        : "BOOK8GUI could not start on this display.\n");
    if (mouse_irq_disable_failed) {
        console_write("PS/2 IRQ12 disable failed; controller IRQ masked.\n");
    }
}

static void network_print_ip(uint32_t ip)
{
    for (unsigned i = 0u; i < 4u; ++i) {
        if (i != 0u) {
            console_putc('.');
        }
        print_unsigned((ip >> (24u - i * 8u)) & 255u);
    }
}

static const char *network_config_source = "none";
static char telemetry_note[BOOK8_TELEMETRY_LOG_SIZE];
static uint32_t telemetry_note_packets;
static uint32_t telemetry_failures;
static enum book8_net_result telemetry_last_result = BOOK8_NET_OK;

/* Shown in the telemetry log field for the next ten packets. */
static void telemetry_event(const char *text)
{
    size_t i = 0u;
    for (; text[i] != '\0' && i + 1u < sizeof(telemetry_note); ++i) {
        telemetry_note[i] = text[i];
    }
    telemetry_note[i] = '\0';
    telemetry_note_packets = 10u;
}

static uint32_t health_flags(uint8_t phy)
{
    struct book8_push_stats push;
    book8_push_get_stats(&push);
    uint32_t flags = 0u;
    flags |= kernel_net.ip != 0u ? BOOK8_HEALTH_IPV4 : 0u;
    flags |= ascii_equal(network_config_source, "baked") ? BOOK8_HEALTH_CONFIG_BAKED : 0u;
    flags |= ascii_equal(network_config_source, BOOK8_NETCFG_PATH)
        ? BOOK8_HEALTH_CONFIG_FILE : 0u;
    flags |= push.listening ? BOOK8_HEALTH_PUSH_AGENT : 0u;
    flags |= (phy & RTL8168_PHY_LINK) != 0u ? BOOK8_HEALTH_PHY_LINK : 0u;
    flags |= ide_controller_count + ahci_controller_count != 0u ? BOOK8_HEALTH_STORAGE : 0u;
    flags |= book8_usb_keyboard_active() ? BOOK8_HEALTH_USB_KEYBOARD : 0u;
    flags |= book8_usb_mouse_active() ? BOOK8_HEALTH_USB_MOUSE : 0u;
    flags |= ps2_keyboard_controller_ready ? BOOK8_HEALTH_PS2_KEYBOARD : 0u;
    flags |= ps2_mouse_available ? BOOK8_HEALTH_PS2_MOUSE : 0u;
    flags |= screen.pixels != NULL ? BOOK8_HEALTH_FRAMEBUFFER : 0u;
    flags |= pci_sound_present ? BOOK8_HEALTH_PCI_SOUND : 0u;
    flags |= acpi_s5_available ? BOOK8_HEALTH_ACPI_S5 : 0u;
    flags |= boot_relaunch_count != 0u ? BOOK8_HEALTH_RELAUNCHED : 0u;
    return flags;
}

static uint32_t tsc_to_us(uint64_t ticks)
{
    if (tsc_hz == 0u) {
        return 0u;
    }
    uint64_t us = ticks / tsc_hz * 1000000u + ticks % tsc_hz * 1000000u / tsc_hz;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static bool telemetry_sample(const char *pulse)
{
    if (!initialization_complete || telemetry_target == 0u
        || kernel_net.ip == 0u || tsc_hz == 0u) {
        return false;
    }
    uint64_t now = read_tsc();
    bool pcie_pending = pcie_event_head != pcie_event_tail
        || pcie_event_dropped != 0u;
    uint64_t interval = pcie_pending && telemetry_last_result == BOOK8_NET_OK
        ? tsc_hz / 100u : tsc_hz;
    if (pulse == NULL && now - telemetry_last_tsc < interval) {
        return false;
    }
    telemetry_last_tsc = now;
    heap_stats_t heap;
    heap_get_stats(&heap);
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    struct net_telemetry_metrics metrics = {
        .cpu_flags = (gui_active ? BOOK8_TELEMETRY_CPU_DESKTOP : 0u)
            | ((flags & 0x200u) ? BOOK8_TELEMETRY_CPU_IRQ_ENABLED : 0u)
            | (rtl_nic.running ? BOOK8_TELEMETRY_CPU_NIC_RUNNING : 0u),
        .cpu_mhz = (uint32_t)(tsc_hz / 1000000u),
        .memory_total_bytes = memory_total_kb * 1024u,
        .kernel_reserved_bytes = (uint64_t)(uintptr_t)__kernel_end,
        .heap_used_bytes = heap.used, .heap_total_bytes = heap.total,
        .tx_packets = rtl_nic.tx_packets, .rx_packets = rtl_nic.rx_packets,
        .tx_dropped = rtl_nic.tx_dropped, .rx_dropped = rtl_nic.rx_dropped,
        .tx_timeouts = rtl_nic.tx_timeouts
    };
    uint8_t phy = 0u;
    struct b8_rtl8168_ring_state ring_state = {0};
    if (rtl8168_driver.descriptor != NULL
        && rtl8168_driver.descriptor->ioctl != NULL) {
        (void)rtl8168_driver.descriptor->ioctl(
            B8_RTL8168_IOCTL_PHY_STATUS, &phy);
        (void)rtl8168_driver.descriptor->ioctl(
            B8_RTL8168_IOCTL_RING_STATE, &ring_state);
    }
    struct book8_push_stats push;
    book8_push_get_stats(&push);
    struct net_telemetry_extended extended = {
        .build_id = BOOK8_BUILD_ID, .health_flags = health_flags(phy),
        .relaunch_count = boot_relaunch_count, .phy_status = phy,
        .tx_ring_pending = ring_state.tx_pending,
        .rx_ring_ready = ring_state.rx_ready,
        .net_rx_frames = kernel_net.rx_frames, .net_tx_frames = kernel_net.tx_frames,
        .net_invalid_frames = kernel_net.invalid_frames,
        .net_ignored_frames = kernel_net.ignored_frames,
        .net_rx_errors = kernel_net.rx_errors, .net_tx_errors = kernel_net.tx_errors,
        .arp_replies = kernel_net.arp_replies, .echo_replies = kernel_net.echo_replies,
        .udp_received = kernel_net.udp_received,
        .arp_entries = book8_net_arp_entries(&kernel_net),
        .poll_iterations = poll_iterations,
        .poll_avg_us = poll_iterations != 0u
            ? tsc_to_us(poll_tsc_total / poll_iterations) : 0u,
        .poll_max_us = tsc_to_us(poll_tsc_max),
        .telemetry_failures = telemetry_failures,
        .push_accepted = push.accepted, .push_rejected = push.rejected
    };
    poll_iterations = 0u;
    poll_tsc_total = 0u;
    poll_tsc_max = 0u;
    uint64_t elapsed = now - boot_tsc;
    uint64_t timestamp = elapsed / tsc_hz * 1000u
        + elapsed % tsc_hz * 1000u / tsc_hz;
    const char *note = gui_active ? "desktop service" : "CLI service";
    if (telemetry_note_packets != 0u && !pcie_pending && pulse == NULL) {
        --telemetry_note_packets;
        note = telemetry_note;
    }
    bool pcie_event = pulse == NULL && pcie_event_head != pcie_event_tail;
    bool pcie_overflow = pulse == NULL && !pcie_event && pcie_event_dropped != 0u;
    char pcie_overflow_note[BOOK8_TELEMETRY_LOG_SIZE];
    if (pcie_event) {
        note = pcie_boot_events[pcie_event_head % PCIE_BOOT_EVENTS];
    } else if (pcie_overflow) {
        snprintf(pcie_overflow_note, sizeof(pcie_overflow_note),
            "PCIE1 ERROR telemetry events lost: %u", pcie_event_dropped);
        note = pcie_overflow_note;
    }
    if (pulse != NULL)
        note = pulse;
    enum book8_net_result result = net_telemetry_send_v2(timestamp, &metrics,
        &extended, note);
    if (result == BOOK8_NET_OK) {
        if (pcie_event)
            ++pcie_event_head;
        else if (pcie_overflow)
            pcie_event_dropped = 0u;
    }
    if (result != BOOK8_NET_OK) {
        ++telemetry_failures;
    }
    /* Report transitions only: an absent collector must not flood the console. */
    if (result != telemetry_last_result) {
        console_write("BOOK8-TELEMETRY: ");
        console_write(book8_net_result_string(result));
        console_write("\n");
        telemetry_last_result = result;
    }
    return result == BOOK8_NET_OK;
}

static void telemetry_service(void)
{
    telemetry_sample(NULL);
}

static struct book8_pcie_probe manual_probe;
static bool manual_probe_initialized;

static bool probe_telemetry_available(void)
{
    return network_ready && telemetry_target == 0xc0a83211u
        && kernel_net.ip != 0u && tsc_hz != 0u;
}

static bool probe_emit(const char *record)
{
    if (!probe_telemetry_available()) {
        console_write("PROBE1 ERROR requires active telemetry to 192.168.50.17:8080\n");
        return false;
    }
    return telemetry_sample(record);
}

static bool probe_hex(const char *text, uint32_t *value)
{
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    if (*text == '\0')
        return false;
    uint32_t result = 0u;
    for (; *text != '\0'; ++text) {
        uint32_t digit;
        if (*text >= '0' && *text <= '9')
            digit = (uint32_t)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (uint32_t)(*text - 'a') + 10u;
        else if (*text >= 'A' && *text <= 'F')
            digit = (uint32_t)(*text - 'A') + 10u;
        else
            return false;
        if (result > (UINT32_MAX - digit) / 16u)
            return false;
        result = result * 16u + digit;
    }
    *value = result;
    return true;
}

static void probe_command(char *argument, bool ack)
{
    if (!manual_probe_initialized) {
        const struct book8_probe_ops ops = {
            .emit = probe_emit, .read = book8_read32_safe, .write = book8_write32_safe
        };
        book8_probe_init(&manual_probe, &pcie, &ops);
        manual_probe_initialized = true;
    }
    char *extra = argument;
    while (*extra != '\0' && *extra != ' ' && *extra != '\t')
        ++extra;
    if (*extra != '\0') {
        *extra++ = '\0';
        extra = skip_spaces(extra);
    }
    if (ack) {
        uint32_t id;
        enum book8_probe_phase phase = ascii_equal(extra, "INTENT")
            ? BOOK8_PROBE_INTENT : ascii_equal(extra, "RESULT")
            ? BOOK8_PROBE_RESULT : BOOK8_PROBE_IDLE;
        if (!parse_disk_number(argument, &id) || phase == BOOK8_PROBE_IDLE) {
            console_write("PROBE1 ERROR expected ACK <decimal-id> INTENT|RESULT\n");
            return;
        }
        if (phase == BOOK8_PROBE_INTENT && !probe_telemetry_available()) {
            manual_probe.sent = false;
            console_write("PROBE1 ERROR telemetry unavailable; no access; restore config then RETRY\n");
            return;
        }
        if (book8_probe_ack(&manual_probe, id, phase)
            && manual_probe.phase == BOOK8_PROBE_IDLE)
            console_write("PROBE1 ACK accepted; enter next index individually; no automatic advance\n");
    } else if (ascii_equal(argument, "STATUS") && *extra == '\0') {
        if (manual_probe.phase != BOOK8_PROBE_IDLE) {
            console_write(manual_probe.record);
            console_write("\n");
        }
    } else if (ascii_equal(argument, "RETRY") && *extra == '\0') {
        (void)book8_probe_retry(&manual_probe);
    } else if ((ascii_equal(argument, "LINKCAP") || ascii_equal(argument, "LINKSTATUS"))
        && *extra == '\0') {
        (void)book8_probe_begin_link(&manual_probe, ascii_equal(argument, "LINKSTATUS"));
    } else if ((ascii_equal(argument, "LTSSM") || ascii_equal(argument, "LINKWIDTH"))
        && *extra == '\0') {
        (void)book8_probe_begin(&manual_probe, ascii_equal(argument, "LTSSM")
            ? BOOK8_RS780_LC_STATE0 : BOOK8_RS780_LINK_WIDTH, 0u);
    } else {
        uint32_t index, mask = 0u;
        if (!probe_hex(argument, &index) || (*extra != '\0' && !probe_hex(extra, &mask))) {
            console_write("PROBE1 ERROR expected PCIEPROBE <A2/A5..AF>|LTSSM|LINKWIDTH|LINKCAP|LINKSTATUS|STATUS|RETRY\n");
            return;
        }
        (void)book8_probe_begin(&manual_probe, index, mask);
    }
    char text[128];
    snprintf(text, sizeof(text), "PROBE1 state=%s id=%u submitted=%u poisoned=%u\n",
        manual_probe.phase == BOOK8_PROBE_IDLE ? "IDLE"
            : manual_probe.phase == BOOK8_PROBE_INTENT ? "INTENT" : "RESULT",
        manual_probe.id, manual_probe.sent ? 1u : 0u, manual_probe.poisoned ? 1u : 0u);
    console_write(text);
    if (manual_probe.phase == BOOK8_PROBE_IDLE) {
        snprintf(text, sizeof(text), "PROBE1 acknowledged LTSSM-L0=%u LINKCAP=%u LINKSTATUS=%u link-preflight=%u; GPU-MMIO blocked\n",
            manual_probe.ltssm_acknowledged ? 1u : 0u,
            manual_probe.link_cap_acknowledged ? 1u : 0u,
            manual_probe.link_status_acknowledged ? 1u : 0u,
            book8_probe_link_verified(&manual_probe) ? 1u : 0u);
        console_write(text);
        if (manual_probe.link_cap_acknowledged && manual_probe.link_status_acknowledged) {
            uint32_t status = manual_probe.link_status >> 16u;
            snprintf(text, sizeof(text),
                "PROBE1 max-speed-code=%u max-width=x%u speed-code=%u width=x%u training=%u DLL-active=%u DLL-reporting=%u\n",
                manual_probe.link_cap & 0xfu, (manual_probe.link_cap >> 4u) & 0x3fu,
                status & 0xfu, (status >> 4u) & 0x3fu, (status >> 11u) & 1u,
                (status >> 13u) & 1u, (manual_probe.link_cap >> 20u) & 1u);
            console_write(text);
        }
    }
    if (manual_probe.phase != BOOK8_PROBE_IDLE)
        console_write("PROBE1 verify collector record, then ACK matching id and phase; RETRY sends record only\n");
}

static void telemetry_command(char *argument)
{
    uint32_t target = 0u;
    if (!ascii_equal(argument, "OFF")
        && (!book8_net_parse_ip(argument, &target) || target == 0u)) {
        console_write("TELEMETRY: expected <target-ip> or OFF.\n");
        return;
    }
    if (target != 0u && !network_ready) {
        console_write("TELEMETRY: no supported NIC ready.\n");
        return;
    }
    enum book8_net_result result = net_telemetry_init(&kernel_net, target);
    if (result == BOOK8_NET_OK && target != 0u && tsc_hz == 0u) {
        pit_clock_start();
        (void)pit_clock_ticks();
        tsc_calibrate();
        if (tsc_hz == 0u) {
            enum book8_net_result stopped = net_telemetry_init(&kernel_net, 0u);
            console_write("TELEMETRY: clock calibration failed; stream stopped: ");
            console_write(book8_net_result_string(stopped));
            console_write("\n");
            telemetry_target = 0u;
            return;
        }
    }
    if (result == BOOK8_NET_OK) {
        telemetry_target = target;
        network_settings.telemetry = target;
        telemetry_last_tsc = read_tsc();
    }
    console_write("TELEMETRY: ");
    console_write(book8_net_result_string(result));
    console_write("\n");
}

static void network_status(void)
{
    console_write("MODE: ");
    console_write(network_settings.mode == BOOK8_NETWORK_DHCP
        ? book8_dhcp_state_string(dhcp_client.state) : "forced static");
    console_write(" DHCP rejected=");
    print_unsigned(dhcp_client.rejected);
    console_write(" lease-seconds=");
    print_unsigned(dhcp_client.lease_seconds);
    console_write("\n");
    console_write("BOOK8-NET: ");
    console_write(network_ready ? "polling stack ready\n" : "no supported NIC ready\n");
    console_write("IP=");
    network_print_ip(kernel_net.ip);
    console_write(" MASK=");
    network_print_ip(kernel_net.mask);
    console_write(" GATEWAY=");
    network_print_ip(kernel_net.gateway);
    console_write("\nRX/TX/invalid/ignored/RX-errors/TX-errors/ARP/echo/UDP=");
    const uint32_t values[] = {kernel_net.rx_frames, kernel_net.tx_frames,
        kernel_net.invalid_frames, kernel_net.ignored_frames, kernel_net.rx_errors,
        kernel_net.tx_errors, kernel_net.arp_replies, kernel_net.echo_replies,
        kernel_net.udp_received};
    for (unsigned i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (i != 0u) {
            console_putc('/');
        }
        print_unsigned(values[i]);
    }
    console_write("\n");
    console_write("TELEMETRY target=");
    network_print_ip(telemetry_target);
    console_write(" UDP=8080 interval=1000ms format=v2 (292 bytes) failures=");
    print_unsigned(telemetry_failures);
    console_write("\nCONFIG source=");
    console_write(network_config_source);
    console_write(" baked=" BOOK8_NET_DEFAULT_IP "/" BOOK8_NET_DEFAULT_MASK
        " gw=" BOOK8_NET_DEFAULT_GATEWAY " telemetry=" BOOK8_NET_DEFAULT_TELEMETRY);
    console_write("\nBUILD id=");
    print_unsigned(BOOK8_BUILD_ID);
    console_write(" relaunch=");
    print_unsigned(boot_relaunch_count);
    struct book8_push_stats push;
    book8_push_get_stats(&push);
    console_write(" PUSH ");
    console_write(push.listening ? "listening UDP=8081" : (BOOK8_PUSH ? "stopped" : "not built"));
    console_write(" accepted/rejected/challenges/busy=");
    print_unsigned(push.accepted);
    console_putc('/');
    print_unsigned(push.rejected);
    console_putc('/');
    print_unsigned(push.challenges);
    console_putc('/');
    print_unsigned(push.busy_replies);
    console_write("\n");
}

static void network_configure_command(char *argument)
{
    if (ascii_equal(argument, "DHCP")) {
        struct book8_network_settings settings = network_settings;
        settings.mode = BOOK8_NETWORK_DHCP;
        if (book8_network_settings_apply(&settings, false))
            console_write("NETCONFIG: DHCP discovery started\n");
        return;
    }
    if (ascii_equal(argument, "RENEW")) {
        (void)book8_network_settings_renew();
        return;
    }
    uint32_t values[3] = {0};
    if (!ascii_equal(argument, "OFF")) {
        for (unsigned i = 0u; i < 3u; ++i) {
            char *token = skip_spaces(argument);
            argument = token;
            while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
                ++argument;
            }
            if (*argument != '\0') {
                *argument++ = '\0';
            }
            if (!book8_net_parse_ip(token, &values[i])) {
                console_write("NETCONFIG: expected <ip> <mask> <gateway> or OFF.\n");
                return;
            }
        }
        if (*skip_spaces(argument) != '\0') {
            console_write("NETCONFIG: unexpected trailing argument.\n");
            return;
        }
    }
    if (!network_ready) {
        console_write("NETCONFIG: no supported NIC ready.\n");
        return;
    }
    struct book8_net validation = {0};
    enum book8_net_result result = book8_net_configure(&validation,
        values[0], values[1], values[2]);
    if (result == BOOK8_NET_OK)
        result = book8_dhcp_stop(&dhcp_client);
    if (result == BOOK8_NET_OK)
        result = book8_net_configure(&kernel_net,
        values[0], values[1], values[2]);
    console_write("NETCONFIG: ");
    console_write(book8_net_result_string(result));
    console_write("\n");
    if (result == BOOK8_NET_OK) {
        network_settings.mode = BOOK8_NETWORK_STATIC;
        network_settings.ip = values[0];
        network_settings.mask = values[1];
        network_settings.gateway = values[2];
        network_config_source = values[0] == 0u ? "none" : "NETCONFIG";
        if (values[0] == 0u && telemetry_target != 0u) {
            enum book8_net_result stopped = net_telemetry_init(&kernel_net, 0u);
            console_write("TELEMETRY: ");
            console_write(book8_net_result_string(stopped));
            console_write("\n");
            if (stopped == BOOK8_NET_OK) {
                telemetry_target = 0u;
            }
        }
        network_status();
    }
}

static bool netboot_overlap(uint64_t start, uint64_t length)
{
    uint64_t base = BOOK8_NETBOOT_ADDRESS;
    uint64_t end = base + BOOK8_TFTP_CAPACITY;
    return length != 0u && start < end
        && (length > UINT64_MAX - start || start + length > base);
}

static bool netboot_memory_available(void)
{
    if (netboot_info_address == 0u
        || (uint64_t)(uintptr_t)__kernel_end > BOOK8_NETBOOT_ADDRESS) {
        return false;
    }
    const uint32_t *info = (const uint32_t *)(uintptr_t)netboot_info_address;
    uint32_t size = info[0];
    if (size < 16u || size > 0x100000000ull - netboot_info_address
        || netboot_overlap(netboot_info_address, size)) {
        return false;
    }
    bool usable = false;
    for (uint32_t offset = 8u; offset <= size - 8u;) {
        const uint8_t *bytes = (const uint8_t *)info + offset;
        const struct multiboot_tag *tag = (const struct multiboot_tag *)bytes;
        if (tag->size < 8u || tag->size > size - offset) {
            return false;
        }
        if (tag->type == MULTIBOOT_TAG_END) {
            return usable;
        }
        if (tag->type == MULTIBOOT_TAG_MMAP && tag->size >= 16u) {
            uint32_t entry_size = *(const uint32_t *)(bytes + 8u);
            if (entry_size < 24u || (tag->size - 16u) % entry_size != 0u) {
                return false;
            }
            for (uint32_t pos = 16u; pos < tag->size; pos += entry_size) {
                uint64_t start = *(const uint64_t *)(bytes + pos);
                uint64_t length = *(const uint64_t *)(bytes + pos + 8u);
                uint32_t type = *(const uint32_t *)(bytes + pos + 16u);
                if (type != 1u && netboot_overlap(start, length)) {
                    return false;
                }
                if (type == 1u && start <= BOOK8_NETBOOT_ADDRESS
                    && length <= UINT64_MAX - start
                    && start + length >= BOOK8_NETBOOT_ADDRESS + BOOK8_TFTP_CAPACITY) {
                    usable = true;
                }
            }
        } else if (tag->type == MULTIBOOT_TAG_MODULE
            && tag->size >= sizeof(struct multiboot_tag_module)) {
            const struct multiboot_tag_module *module = (const void *)bytes;
            if (module->end < module->start
                || netboot_overlap(module->start, module->end - module->start)) {
                return false;
            }
        } else if (tag->type == MULTIBOOT_TAG_FRAMEBUFFER && tag->size >= 32u) {
            const struct multiboot_tag_framebuffer *fb = (const void *)bytes;
            if (netboot_overlap(fb->address, (uint64_t)fb->pitch * fb->height)) {
                return false;
            }
        }
        uint32_t step = (tag->size + 7u) & ~7u;
        if (step < tag->size || step > size - offset) {
            return false;
        }
        offset += step;
    }
    return false;
}

static uint64_t netboot_milliseconds(void *ctx)
{
    (void)ctx;
    uint64_t elapsed = read_tsc() - boot_tsc;
    return elapsed / tsc_hz * 1000u + elapsed % tsc_hz * 1000u / tsc_hz;
}

static void netboot_idle(void *ctx)
{
    (void)ctx;
    __asm__ volatile ("pause" ::: "memory");
}

static uint8_t netboot_stack[16384] __attribute__((aligned(16)));
static struct book8_warm_info warm_info;

static bool netboot_disable_dma(void)
{
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint8_t slot = 0u; slot < 32u; ++slot) {
            uint8_t count = 1u;
            for (uint8_t fn = 0u; fn < count; ++fn) {
                uint16_t vendor = (uint16_t)pci_read32((uint8_t)bus, slot, fn, 0u);
                if (vendor == 0xffffu || vendor == 0u) {
                    continue;
                }
                if (fn == 0u && (pci_read32((uint8_t)bus, slot, fn, 0x0cu)
                    & 0x800000u) != 0u) {
                    count = 8u;
                }
                uint32_t command = pci_read32((uint8_t)bus, slot, fn, 4u) & 0xffffu;
                driver_pci_write32(NULL, (uint8_t)bus, slot, fn, 4u,
                    (command & ~4u) | 0x400u);
                if ((pci_read32((uint8_t)bus, slot, fn, 4u) & 4u) != 0u) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void network_boot_command(char *argument)
{
    char *server_text = skip_spaces(argument);
    char *filename = server_text;
    while (*filename != '\0' && *filename != ' ' && *filename != '\t') {
        ++filename;
    }
    if (*filename != '\0') {
        *filename++ = '\0';
    }
    filename = skip_spaces(filename);
    uint32_t server;
    if (!book8_net_parse_ip(server_text, &server) || *filename == '\0') {
        console_write("NETBOOT: expected <tftp-ip> <warm-kernel-file>.\n");
        return;
    }
    if (gui_active) {
        console_write("NETBOOT: close the desktop and use the CLI for handoff.\n");
        return;
    }
    if (!network_ready) {
        console_write("NETBOOT: no supported NIC ready.\n");
        return;
    }
    if (kernel_net.ip == 0u) {
        console_write("NETBOOT: configure IPv4 with NETCONFIG first.\n");
        return;
    }
    if (!netboot_memory_available()) {
        console_write("NETBOOT: 68..76 MiB is not verified free usable RAM.\n");
        return;
    }
    if (tsc_hz == 0u) {
        pit_clock_start();
        (void)pit_clock_ticks();
        tsc_calibrate();
    }
    if (tsc_hz == 0u) {
        console_write("NETBOOT: clock calibration failed.\n");
        return;
    }
    const struct book8_tftp_clock clock = {
        .milliseconds = netboot_milliseconds, .idle = netboot_idle
    };
    enum book8_net_result result = tftp_client_init(&kernel_net, server,
        BOOK8_TFTP_CAPACITY, &clock);
    uint8_t *load = (uint8_t *)(uintptr_t)BOOK8_NETBOOT_ADDRESS;
    if (result == BOOK8_NET_OK) {
        console_write("NETBOOT: fetching trusted warm image via TFTP...\n");
        result = tftp_fetch_kernel(filename, load);
    }
    struct book8_warm_header header;
    if (result == BOOK8_NET_OK) {
        result = book8_netboot_validate(load, tftp_fetched_size(),
            BOOK8_TFTP_CAPACITY, &header);
    }
    if (result != BOOK8_NET_OK) {
        console_write("NETBOOT: ");
        console_write(book8_net_result_string(result));
        if (result == BOOK8_NET_REMOTE_ERROR) {
            console_write(" code=");
            print_unsigned(tftp_remote_error());
        }
        console_write("\n");
        return;
    }
    for (size_t i = header.image_bytes; i < header.memory_bytes; ++i) {
        load[i] = 0u;
    }
    warm_info = (struct book8_warm_info){
        .magic = BOOK8_WARM_MAGIC, .version = 1u,
        .load_address = BOOK8_NETBOOT_ADDRESS, .image_bytes = header.image_bytes,
        .memory_bytes = header.memory_bytes
    };
    __asm__ volatile ("mov %%cr3, %0" : "=r"(warm_info.cr3));
    console_write("NETBOOT: validated; disabling DMA/interrupts for warm handoff.\n");
    __asm__ volatile ("cli" ::: "memory");
    outb(0x21u, 0xffu);
    outb(0xa1u, 0xffu);
    rtl_nic.regs[RTL8168_CR] = 0u;
    (void)rtl_nic.regs[RTL8168_CR];
    if (!netboot_disable_dma()) {
        console_write("NETBOOT: failed to disable PCI DMA; halting without handoff.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    network_ready = false;
    __asm__ volatile ("mfence" ::: "memory");
    book8_netboot_jump(BOOK8_NETBOOT_ADDRESS + header.entry_offset,
        (uint64_t)(uintptr_t)(netboot_stack + sizeof(netboot_stack)), &warm_info);
}

/* ---- Baked network baseline, authenticated push agent, in-RAM relaunch ---- */
#define RELAUNCH_STAGE_CAPACITY (BOOK8_TFTP_CAPACITY - BOOK8_RELAUNCH_PLAN_BYTES)
#define RELAUNCH_PLAN_PHYSICAL (BOOK8_NETBOOT_ADDRESS + RELAUNCH_STAGE_CAPACITY)

static char push_message[BOOK8_PUSH_MESSAGE_MAX];
static uint64_t network_boot_tsc;

static bool network_calibrate_clock(void)
{
    if (tsc_hz == 0u) {
        pit_clock_start();
        (void)pit_clock_ticks();
        tsc_calibrate();
    }
    return tsc_hz != 0u;
}

/* Same rules as A:\BOOK8NET.CFG: nonzero IP, contiguous mask, valid dotted quads. */
static bool netcfg_valid(const struct book8_netcfg *config)
{
    char text[BOOK8_NETCFG_TEXT_MAX];
    struct book8_netcfg parsed;
    size_t length = book8_netcfg_format(config, text, sizeof(text));
    return length != 0u && book8_netcfg_parse(text, length, &parsed);
}

static bool network_baked_config(struct book8_netcfg *config)
{
    *config = (struct book8_netcfg){0};
    if (BOOK8_NET_DEFAULT_IP[0] == '\0') {
        return false;
    }
    return book8_net_parse_ip(BOOK8_NET_DEFAULT_IP, &config->ip)
        && book8_net_parse_ip(BOOK8_NET_DEFAULT_MASK, &config->mask)
        && (BOOK8_NET_DEFAULT_GATEWAY[0] == '\0'
            || book8_net_parse_ip(BOOK8_NET_DEFAULT_GATEWAY, &config->gateway))
        && (BOOK8_NET_DEFAULT_TELEMETRY[0] == '\0'
            || book8_net_parse_ip(BOOK8_NET_DEFAULT_TELEMETRY, &config->telemetry))
        && netcfg_valid(config);
}

static enum book8_net_result network_apply_config(const struct book8_netcfg *config,
    const char *source)
{
    enum book8_net_result result = book8_dhcp_stop(&dhcp_client);
    if (result != BOOK8_NET_OK)
        return result;
    result = book8_net_configure(&kernel_net, config->ip,
        config->mask, config->gateway);
    if (result != BOOK8_NET_OK) {
        return result;
    }
    network_config_source = source;
    network_settings.mode = BOOK8_NETWORK_STATIC;
    network_settings.ip = config->ip;
    network_settings.mask = config->mask;
    network_settings.gateway = config->gateway;
    network_settings.telemetry = config->telemetry;
    uint32_t target = config->telemetry;
    if (target != 0u && !network_calibrate_clock()) {
        console_write("BOOK8-NET: clock calibration failed; telemetry disabled.\n");
        target = 0u;
    }
    enum book8_net_result telemetry = net_telemetry_init(&kernel_net, target);
    if (telemetry == BOOK8_NET_OK) {
        telemetry_target = target;
        telemetry_last_tsc = read_tsc();
        telemetry_last_result = BOOK8_NET_OK;
    } else {
        console_write("BOOK8-NET: telemetry ");
        console_write(book8_net_result_string(telemetry));
        console_write("\n");
    }
    console_write("BOOK8-NET: config ");
    console_write(source);
    console_write(" IP=");
    network_print_ip(kernel_net.ip);
    console_write(" MASK=");
    network_print_ip(kernel_net.mask);
    console_write(" GATEWAY=");
    network_print_ip(kernel_net.gateway);
    console_write(" TELEMETRY=");
    network_print_ip(telemetry_target);
    console_write(telemetry_target != 0u ? ":8080\n" : "\n");
    return BOOK8_NET_OK;
}

static uint64_t network_milliseconds(void)
{
    uint64_t ticks = read_tsc() - boot_tsc;
    return tsc_hz == 0u ? 0u : ticks / tsc_hz * 1000u
        + ticks % tsc_hz * 1000u / tsc_hz;
}

static void network_dhcp_service(void)
{
    if (network_settings.mode != BOOK8_NETWORK_DHCP || tsc_hz == 0u)
        return;
    enum book8_net_result result = book8_dhcp_poll(&dhcp_client, network_milliseconds());
    if (result != BOOK8_NET_OK) {
        console_write("BOOK8-DHCP: ");
        console_write(book8_net_result_string(result));
        console_write("\n");
    }
    if (dhcp_generation == dhcp_client.generation)
        return;
    dhcp_generation = dhcp_client.generation;
    network_config_source = dhcp_client.fallback_active ? "DHCP timeout/static fallback"
        : kernel_net.ip != 0u ? "DHCP" : "DHCP waiting";
    result = net_telemetry_init(&kernel_net, 0u);
    if (result == BOOK8_NET_OK) {
        telemetry_target = 0u;
        result = net_telemetry_init(&kernel_net,
            kernel_net.ip != 0u ? network_settings.telemetry : 0u);
    }
    if (result == BOOK8_NET_OK)
        telemetry_target = kernel_net.ip != 0u ? network_settings.telemetry : 0u;
    else {
        console_write("BOOK8-DHCP: collector route unavailable: ");
        console_write(book8_net_result_string(result));
        console_write("\n");
    }
    char note[128];
    snprintf(note, sizeof(note), "DHCP %s ip=%08x mask=%08x gw=%08x dns=%08x seconds=%u",
        dhcp_client.fallback_active ? "fallback" : "lease", kernel_net.ip,
        kernel_net.mask, kernel_net.gateway, dhcp_client.dns,
        dhcp_client.lease_seconds);
    pcie_log(note);
    telemetry_last_tsc = 0u;
}

bool book8_network_settings_get(struct book8_network_status *status)
{
    if (status == NULL)
        return false;
    uint64_t now = tsc_hz != 0u ? network_milliseconds() : 0u;
    *status = (struct book8_network_status){.settings = network_settings,
        .assigned_ip = kernel_net.ip, .assigned_mask = kernel_net.mask,
        .assigned_gateway = kernel_net.gateway,
        .dns = network_settings.mode == BOOK8_NETWORK_STATIC
            ? network_settings.dns : dhcp_client.dns,
        .lease_seconds = dhcp_client.lease_seconds == UINT32_MAX ? UINT32_MAX
            : dhcp_client.expires > now ? (uint32_t)((dhcp_client.expires - now) / 1000u) : 0u,
        .state = network_suspended || cmdline_network_off ? "disabled"
            : dhcp_client.fallback_active ? "DHCP timeout/static fallback"
            : network_settings.mode == BOOK8_NETWORK_DHCP
            ? book8_dhcp_state_string(dhcp_client.state) : "forced static",
        .ready = network_ready && !network_suspended && !cmdline_network_off};
    return true;
}

bool book8_network_settings_apply(const struct book8_network_settings *settings, bool persist)
{
    if (!network_ready || !book8_network_settings_valid(settings) || cmdline_network_off) {
        console_write("BOOK8-NET: settings rejected (NIC unavailable, boot disabled or invalid values)\n");
        return false;
    }
    if (!network_calibrate_clock()) {
        console_write("BOOK8-NET: clock unavailable; settings rejected\n");
        return false;
    }
    if (persist) {
        uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
        if (!book8_network_settings_encode(settings, bytes)
            || !fs_write_file(BOOK8_NETWORK_SETTINGS_PATH, bytes, sizeof(bytes))) {
            console_write("BOOK8-NET: cannot save " BOOK8_NETWORK_SETTINGS_PATH "\n");
            return false;
        }
        static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
        char ip[16], mask[16], gateway[16], dns[16];
        registry_format_ipv4(settings->ip, ip, sizeof(ip));
        registry_format_ipv4(settings->mask, mask, sizeof(mask));
        registry_format_ipv4(settings->gateway, gateway, sizeof(gateway));
        registry_format_ipv4(settings->dns, dns, sizeof(dns));
        if (!reg_set_str(path, "NetMode",
                settings->mode == BOOK8_NETWORK_DHCP ? "DHCP" : "Static")
            || !reg_set_str(path, "StaticIP", ip)
            || !reg_set_str(path, "Netmask", mask)
            || !reg_set_str(path, "Gateway", gateway)
            || !reg_set_str(path, "DNS", dns) || !reg_save()) {
            console_write("BOOK8-NET: registry settings save failed\n");
            return false;
        }
    }
    if (book8_config_value(BOOK8_CFG_NETWORK) == 0u) {
        network_settings = *settings;
        console_write("BOOK8-NET: settings retained; network disabled in system settings\n");
        return true;
    }
    if (settings->mode == BOOK8_NETWORK_STATIC) {
        struct book8_netcfg config = {.ip = settings->ip, .mask = settings->mask,
            .gateway = settings->gateway, .telemetry = settings->telemetry};
        enum book8_net_result result = network_apply_config(&config, "forced static");
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-NET: static apply failed\n");
            return false;
        }
        dhcp_client.dns = settings->dns;
    } else {
        enum book8_net_result result = book8_dhcp_stop(&dhcp_client);
        if (result == BOOK8_NET_OK)
            result = net_telemetry_init(&kernel_net, 0u);
        if (result == BOOK8_NET_OK) {
            uint32_t xid = (uint32_t)read_tsc() ^ BOOK8_BUILD_ID
                ^ ((uint32_t)rtl_nic.mac[4] << 8u) ^ rtl_nic.mac[5];
            if (xid == 0u)
                xid = 1u;
            result = book8_dhcp_start(&dhcp_client, &kernel_net, network_milliseconds(),
                xid, settings->retry_ms, pcie_log);
            if (result == BOOK8_NET_OK)
                result = book8_dhcp_set_fallback(&dhcp_client,
                    settings->ip, settings->mask, settings->gateway);
        }
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-DHCP: start failed: ");
            console_write(book8_net_result_string(result));
            console_write("\n");
            return false;
        }
        telemetry_target = 0u;
        dhcp_generation = dhcp_client.generation;
        network_config_source = "DHCP waiting";
    }
    network_settings = *settings;
    return true;
}

static bool network_enabled_service(void)
{
    if (cmdline_network_off || book8_config_value(BOOK8_CFG_NETWORK) == 0u) {
        if (!network_suspended) {
            enum book8_net_result result = book8_dhcp_stop(&dhcp_client);
            if (result == BOOK8_NET_OK)
                result = book8_net_configure(&kernel_net, 0u, 0u, 0u);
            if (result == BOOK8_NET_OK)
                result = net_telemetry_init(&kernel_net, 0u);
            if (result != BOOK8_NET_OK) {
                console_write("BOOK8-NET: suspend failed: ");
                console_write(book8_net_result_string(result));
                console_write("\n");
                return false;
            }
            telemetry_target = 0u;
            network_suspended = true;
            network_config_source = "disabled";
        }
        return false;
    }
    if (network_suspended) {
        struct book8_network_settings settings = network_settings;
        if (!book8_network_settings_apply(&settings, false))
            return false;
        network_suspended = false;
    }
    return true;
}

bool book8_network_settings_renew(void)
{
    if (!network_ready || network_suspended || cmdline_network_off
        || book8_config_value(BOOK8_CFG_NETWORK) == 0u
        || network_settings.mode != BOOK8_NETWORK_DHCP) {
        console_write("BOOK8-DHCP: renew requires DHCP mode and ready NIC\n");
        return false;
    }
    enum book8_net_result result = book8_dhcp_renew(&dhcp_client, network_milliseconds());
    if (result != BOOK8_NET_OK) {
        console_write("BOOK8-DHCP: renew failed\n");
        return false;
    }
    return true;
}

static void network_early_telemetry(void)
{
    if (!network_ready || cmdline_network_off) {
        pcie_log("PCIE1 PULSE skipped: NIC unavailable or book8.net=off");
        return;
    }
    struct book8_netcfg config = {0};
    (void)network_baked_config(&config);
    network_settings.ip = config.ip;
    network_settings.mask = config.mask;
    network_settings.gateway = config.gateway;
    (void)book8_net_parse_ip(BOOK8_NET_DEFAULT_TELEMETRY, &network_settings.telemetry);
    if (!book8_network_settings_apply(&network_settings, false))
        pcie_log("PCIE1 PULSE waiting: DHCP start unavailable");
    uint32_t id = pci_read32(1u, 0u, 0u, 0u);
    uint32_t command = id == BOOK8_PCIE_TARGET_ID
        ? pci_read32(1u, 0u, 0u, 4u) : UINT32_MAX;
    char text[128];
    snprintf(text, sizeof(text),
        "PCIE1 PULSE id=%08x cmd=%08x lane=unknown", id, command);
    pcie_log(text);
    pcie_log("PCIE1 PULSE queued until DHCP lease or explicit static configuration");
}

static void msr_guard_test(void)
{
    interrupt_table_initialize();
    uint64_t before = 0u, after = 0u, invalid = 0x123456789abcdef0ull;
    uint64_t flags = irq_save();
    irq_restore(flags);
    bool first = book8_read_msr_safe(0xc0000080u, &before);
    /* Reserved x2APIC register zero faults even when arbitrary unknown MSRs
     * are emulated as zero by QEMU TCG. This is a read-only fault probe. */
    bool rejected = !book8_read_msr_safe(0x800u, &invalid);
    bool second = book8_read_msr_safe(0xc0000080u, &after);
    uint32_t memory = 0x12345678u, output = 0xabcdef01u;
    bool memory_ok = book8_read32_safe(&memory, &output) == 0u && output == memory
        && book8_write32_safe(&memory, 0x87654321u) == 0u && memory == 0x87654321u;
    /* Noncanonical addresses #GP; an absent identity-map address #PF.
     * No physical device is accessed by either fault injection. */
    volatile uint32_t *gp_address = (volatile uint32_t *)(uintptr_t)0x0100000000000000ull;
    volatile uint32_t *pf_address = (volatile uint32_t *)(uintptr_t)0x100000000ull;
    output = 0xabcdef01u;
    bool memory_faults = book8_read32_safe(gp_address, &output) == 13u
        && book8_write32_safe(gp_address, 0u) == 13u
        && book8_read32_safe(pf_address, &output) == 14u
        && book8_write32_safe(pf_address, 0u) == 14u
        && output == 0xabcdef01u;
    uint64_t final_flags = irq_save();
    irq_restore(final_flags);
    console_write(first && second && before == after && rejected && memory_ok && memory_faults
        && invalid == 0x123456789abcdef0ull && (flags & 0x200u) == (final_flags & 0x200u)
        ? "MSRGUARDTEST: PASS valid EFER read, #GP recovery, output/IF preserved\n"
        : "MSRGUARDTEST: FAIL safe MSR recovery or state preservation\n");
    console_write(memory_ok && memory_faults
        ? "MEMGUARDTEST: PASS RAM read/write, #GP/#PF read/write recovery, failed output preserved\n"
        : "MEMGUARDTEST: FAIL guarded memory accesses\n");
}

#if BOOK8_PUSH
static uint64_t push_milliseconds(void *ctx)
{
    return tsc_hz != 0u ? netboot_milliseconds(ctx) : 0u;
}

static void push_entropy(void *ctx, uint8_t out[BOOK8_SHA256_BYTES])
{
    (void)ctx;
    uint32_t a, b, c, d;
    uint64_t words[4] = {read_tsc(), network_boot_tsc,
        ((uint64_t)kernel_net.rx_frames << 32u) | kernel_net.udp_received, poll_tsc_total};
    cpu_id(1u, &a, &b, &c, &d);
    if ((c & (1u << 30u)) != 0u) {
        for (unsigned i = 0u; i < 10u; ++i) {
            uint64_t value;
            uint8_t ok;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) :: "cc");
            if (ok != 0u) {
                words[3] ^= value;
                break;
            }
        }
    }
    __builtin_memcpy(out, words, sizeof(words));
}
#endif

static void push_agent_start(void)
{
#if BOOK8_PUSH
    static const struct book8_push_platform platform = {
        .milliseconds = push_milliseconds, .entropy = push_entropy
    };
    if (!network_ready || !network_calibrate_clock()) {
        return;
    }
    enum book8_net_result result = book8_push_init(&kernel_net, book8_push_key,
        BOOK8_BUILD_ID, &platform);
    console_write("BOOK8-PUSH: update agent UDP 8081 ");
    console_write(book8_net_result_string(result));
    console_write("\n");
#endif
}

/* Polls until the RESULT datagram left (ARP may need a round trip). */
static void push_flush_result(uint32_t milliseconds)
{
    if (tsc_hz == 0u) {
        return;
    }
    uint64_t deadline = read_tsc() + tsc_hz / 1000u * milliseconds;
    struct book8_push_stats stats;
    do {
        (void)book8_net_poll(&kernel_net);
        (void)book8_push_service();
        book8_push_get_stats(&stats);
        __asm__ volatile ("pause");
    } while (stats.result_unsent && read_tsc() < deadline);
    /* Give the NIC time to put the final frame on the wire. */
    uint64_t drain = read_tsc() + tsc_hz / 20u;
    while (read_tsc() < drain) {
        (void)book8_net_poll(&kernel_net);
    }
}

static void push_finish(enum book8_push_status status, const char *message)
{
    console_write("BOOK8-PUSH: ");
    console_write(message);
    console_write("\n");
    telemetry_event(message);
    (void)book8_push_complete(status, message);
}

static bool push_valid_83(const char *name)
{
    size_t base = 0u, extension = 0u;
    bool dot = false;
    for (const char *c = name; *c != '\0'; ++c) {
        if (*c == '.') {
            if (dot || base == 0u) {
                return false;
            }
            dot = true;
            continue;
        }
        bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
            || *c == '_' || *c == '-';
        if (!ok) {
            return false;
        }
        if (dot ? ++extension > 3u : ++base > 8u) {
            return false;
        }
    }
    return base != 0u && (!dot || extension != 0u);
}

/* Fetches request->remote from the requester's TFTP server into the staging
 * window and verifies size and SHA-256. Returns NULL on success. */
static const char *push_fetch(const struct book8_push_request *request, size_t capacity)
{
    static const struct book8_tftp_clock clock = {
        .milliseconds = netboot_milliseconds, .idle = netboot_idle
    };
    uint8_t *load = (uint8_t *)(uintptr_t)BOOK8_NETBOOT_ADDRESS;
    if (request->size > capacity) {
        return "image larger than staging window";
    }
    if (!netboot_memory_available()) {
        return "68..76 MiB staging RAM not verified free";
    }
    enum book8_net_result result = tftp_client_init(&kernel_net, request->source_ip,
        capacity, &clock);
    if (result == BOOK8_NET_OK) {
        console_write("BOOK8-PUSH: TFTP fetch ");
        console_write(request->remote);
        console_write("\n");
        result = tftp_fetch_kernel(request->remote, load);
    }
    if (result != BOOK8_NET_OK) {
        snprintf(push_message, sizeof(push_message), "TFTP %s (remote error %u)",
            book8_net_result_string(result), (unsigned)tftp_remote_error());
        return push_message;
    }
    if (tftp_fetched_size() != request->size) {
        snprintf(push_message, sizeof(push_message), "size mismatch: got %u expected %u",
            (unsigned)tftp_fetched_size(), (unsigned)request->size);
        return push_message;
    }
    uint8_t digest[BOOK8_SHA256_BYTES];
    book8_sha256(load, request->size, digest);
    if (!book8_digest_equal(digest, request->sha256, sizeof(digest))) {
        return "SHA-256 mismatch";
    }
    return NULL;
}

static void relaunch_enter_now(void) __attribute__((noreturn));

static bool relaunch_prepare_staged(size_t bytes, struct book8_relaunch_info *info,
    const char **error)
{
    enum book8_relaunch_result result = book8_relaunch_prepare(
        (const uint8_t *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, bytes, BOOK8_NETBOOT_ADDRESS,
        (const uint8_t *)(uintptr_t)netboot_info_address,
        (uint8_t *)(uintptr_t)RELAUNCH_PLAN_PHYSICAL, RELAUNCH_PLAN_PHYSICAL, info);
    *error = book8_relaunch_result_string(result);
    return result == BOOK8_RELAUNCH_OK;
}

static void push_execute(const struct book8_push_request *request)
{
    switch (request->command) {
    case BOOK8_PUSH_STATUS: {
        uint64_t uptime = tsc_hz != 0u ? (read_tsc() - network_boot_tsc) / tsc_hz : 0u;
        snprintf(push_message, sizeof(push_message),
            "build %u up %us relaunch %u config %s telemetry-failures %u boot-update=1",
            (unsigned)BOOK8_BUILD_ID, (unsigned)uptime, (unsigned)boot_relaunch_count,
            network_config_source, (unsigned)telemetry_failures);
        (void)book8_push_complete(BOOK8_PUSH_OK, push_message);
        return;
    }
    case BOOK8_PUSH_CONFIG: {
        struct book8_netcfg config = {
            .ip = request->ip, .mask = request->mask,
            .gateway = request->gateway, .telemetry = request->telemetry
        };
        bool persist = (request->flags & BOOK8_PUSH_FLAG_PERSIST) != 0u;
        if (!netcfg_valid(&config)) {
            push_finish(BOOK8_PUSH_BAD_REQUEST, "push: invalid IPv4 configuration");
            return;
        }
        if (persist) {
            struct book8_network_settings saved = {.mode = BOOK8_NETWORK_STATIC,
                .ip = config.ip, .mask = config.mask, .gateway = config.gateway,
                .telemetry = config.telemetry, .retry_ms = network_settings.retry_ms};
            uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
            if (!book8_network_settings_encode(&saved, bytes)
                || !fs_write_file(BOOK8_NETWORK_SETTINGS_PATH, bytes, sizeof(bytes))) {
                push_finish(BOOK8_PUSH_FAILED, "push: cannot write A:\\BOOK8DHCP.CFG");
                return;
            }
        }
        push_finish(BOOK8_PUSH_ACCEPTED, persist
            ? "push: static config saved to A:\\BOOK8DHCP.CFG; applying"
            : "push: config applying (runtime only)");
        push_flush_result(1000u);
        enum book8_net_result result = network_apply_config(&config,
            persist ? BOOK8_NETCFG_PATH : "push");
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-PUSH: config apply ");
            console_write(book8_net_result_string(result));
            console_write("\n");
        }
        return;
    }
    case BOOK8_PUSH_FILE: {
        if (!push_valid_83(request->local)) {
            push_finish(BOOK8_PUSH_BAD_REQUEST,
                "push: destination must be an 8.3 upper-case name");
            return;
        }
        const char *error = push_fetch(request, BOOK8_TFTP_CAPACITY);
        char path[4u + BOOK8_PUSH_LOCAL_MAX];
        snprintf(path, sizeof(path), "A:\\%s", request->local);
        if (error == NULL && !fs_write_file(path,
            (const void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, request->size)) {
            error = "cannot write file on A: (is a FAT volume mounted?)";
        }
        if (error != NULL) {
            char text[BOOK8_PUSH_MESSAGE_MAX];
            snprintf(text, sizeof(text), "push: FILE failed: %s", error);
            push_finish(BOOK8_PUSH_FAILED, text);
            return;
        }
        snprintf(push_message, sizeof(push_message), "push: wrote %s (%u bytes, SHA-256 ok)",
            path, (unsigned)request->size);
        push_finish(BOOK8_PUSH_OK, push_message);
        return;
    }
    case BOOK8_PUSH_INSTALL_KERNEL: {
        const char *error = push_fetch(request, 1024u * 1024u);
        struct book8_relaunch_info info;
        if (error == NULL) {
            const char *validation_error;
            if (!relaunch_prepare_staged(request->size, &info, &validation_error))
                error = validation_error;
        }
        if (error == NULL && !fs_replace_boot_kernel(
            (const void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, request->size))
            error = "boot file replacement failed; inspect console; do not reboot";
        block_cache_invalidate(NULL);
        if (error != NULL) {
            snprintf(push_message, sizeof(push_message), "push: INSTALL_KERNEL failed: %s", error);
            push_finish(BOOK8_PUSH_FAILED, push_message);
            return;
        }
        push_finish(BOOK8_PUSH_OK, "push: A:\\boot\\book8.elf persisted and readback verified; reboot separately");
        return;
    }
    case BOOK8_PUSH_KERNEL: {
        struct book8_relaunch_info info;
        const char *error = push_fetch(request, RELAUNCH_STAGE_CAPACITY);
        if (error == NULL) {
            const char *prepare_error;
            if (!relaunch_prepare_staged(request->size, &info, &prepare_error)) {
                error = prepare_error;
            }
        }
        if (error != NULL) {
            char text[BOOK8_PUSH_MESSAGE_MAX];
            snprintf(text, sizeof(text), "push: KERNEL rejected: %s", error);
            push_finish(BOOK8_PUSH_FAILED, text);
            return;
        }
        snprintf(push_message, sizeof(push_message),
            "push: kernel verified (%u bytes); relaunch #%u entry 0x%x",
            (unsigned)request->size, (unsigned)info.relaunch_count, (unsigned)info.entry);
        push_finish(BOOK8_PUSH_ACCEPTED, push_message);
        push_flush_result(1000u);
        relaunch_enter_now();
    }
    case BOOK8_PUSH_REBOOT:
        push_finish(BOOK8_PUSH_ACCEPTED, "push: rebooting");
        push_flush_result(1000u);
        reboot_system();
        return;
    default:
        push_finish(BOOK8_PUSH_BAD_REQUEST, "push: unknown command");
        return;
    }
}

static void push_agent_service(void)
{
    struct book8_push_request request;
    if (push_executing) {
        return;
    }
    (void)book8_push_service();
    if (!book8_push_take(&request)) {
        return;
    }
    push_executing = true;
    console_write("BOOK8-PUSH: ");
    console_write(book8_push_command_name(request.command));
    console_write(" from ");
    network_print_ip(request.source_ip);
    console_write("\n");
    push_execute(&request);
    push_executing = false;
}

/* Clears bus mastering on every endpoint except display (framebuffer
 * scan-out); bridges keep forwarding. The new kernel re-enables what it uses. */
static bool relaunch_disable_dma(void)
{
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint8_t slot = 0u; slot < 32u; ++slot) {
            uint8_t count = 1u;
            for (uint8_t fn = 0u; fn < count; ++fn) {
                uint16_t vendor = (uint16_t)pci_read32((uint8_t)bus, slot, fn, 0u);
                if (vendor == 0xffffu || vendor == 0u) {
                    continue;
                }
                uint32_t header = (pci_read32((uint8_t)bus, slot, fn, 0x0cu) >> 16u) & 0xffu;
                if (fn == 0u && (header & 0x80u) != 0u) {
                    count = 8u;
                }
                uint8_t class_code = (uint8_t)(pci_read32((uint8_t)bus, slot, fn, 8u) >> 24u);
                if ((header & 0x7fu) != 0u || class_code == 0x03u || class_code == 0x06u) {
                    continue;
                }
                uint16_t command = pci_read16((uint8_t)bus, slot, fn, 4u);
                if ((command & 4u) == 0u) {
                    continue;
                }
                pci_write16((uint8_t)bus, slot, fn, 4u, (uint16_t)(command & ~4u));
                if ((pci_read16((uint8_t)bus, slot, fn, 4u) & 4u) != 0u) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void smp_quiesce_for_relaunch(void)
{
    if (!local_apic_ready || smp_topology.cpu_count <= 1u) {
        return;
    }
    uint64_t expected = 0u;
    for (uint32_t cpu = 1u; cpu < smp_topology.cpu_count; ++cpu) {
        expected |= 1ull << cpu;
    }
    __atomic_store_n(&smp_relaunch_requested, 1u, __ATOMIC_RELEASE);
    uint64_t pending = expected;
    uint64_t mask = 0u;
    uint64_t started = pit_clock_ticks();
    for (;;) {
        mask = __atomic_load_n(&smp_relaunch_halted_mask, __ATOMIC_ACQUIRE);
        pending = expected & ~mask;
        if (pending == 0u) {
            return;
        }
        if (pit_clock_ticks() - started >= PIT_HZ * 4u) {
            break;
        }
        for (uint32_t cpu = 1u; cpu < smp_topology.cpu_count; ++cpu) {
            if ((pending & (1ull << cpu)) != 0u) {
                (void)book8_apic_send_ipi(smp_topology.apic_ids[cpu],
                    SMP_SCHEDULE_IPI_VECTOR);
            }
        }
        __asm__ volatile ("pause");
    }
    if (pending != 0u) {
        char text[128];
        snprintf(text, sizeof(text),
            "BOOK8-RELAUNCH: AP quiesce timed out (requested=%u mask=0x%llx pending=0x%llx); halting.\n",
            (unsigned)__atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE),
            (unsigned long long)mask,
            (unsigned long long)pending);
        console_write(text);
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}

static void relaunch_enter_now(void)
{
    console_write("BOOK8-RELAUNCH: quiescing DMA/interrupts; entering new kernel\n");
    smp_quiesce_for_relaunch();
    __asm__ volatile ("cli" ::: "memory");
    outb(0x21u, 0xffu);
    outb(0xa1u, 0xffu);
    if (rtl_nic.regs != NULL) {
        rtl_nic.regs[RTL8168_CR] = 0u;
        (void)rtl_nic.regs[RTL8168_CR];
    }
    if (!relaunch_disable_dma()) {
        console_write("BOOK8-RELAUNCH: failed to disable PCI DMA; halting.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    network_ready = false;
    __asm__ volatile ("mfence" ::: "memory");
    book8_relaunch_enter((uint8_t *)(uintptr_t)RELAUNCH_PLAN_PHYSICAL, RELAUNCH_PLAN_PHYSICAL);
}

static void relaunch_command(char *argument)
{
    char path[96];
    char *name = skip_spaces(argument);
    if (*name == '\0') {
        console_write("RELAUNCH: expected <kernel.elf> on A: (Multiboot2 ELF64).\n");
        return;
    }
    if (name[0] != '\0' && name[1] == ':') {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "A:\\%s", name);
    }
    if (!netboot_memory_available()) {
        console_write("RELAUNCH: 68..76 MiB is not verified free usable RAM.\n");
        return;
    }
    uint32_t size = 0u;
    if (!fs_read_file(path, (void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS,
        RELAUNCH_STAGE_CAPACITY, &size) || size == 0u) {
        console_write("RELAUNCH: cannot read ");
        console_write(path);
        console_write(" (root files only, at most 8 MiB - 64 KiB).\n");
        return;
    }
    struct book8_relaunch_info info;
    const char *error;
    if (!relaunch_prepare_staged(size, &info, &error)) {
        console_write("RELAUNCH: ");
        console_write(error);
        console_write("\n");
        return;
    }
    char text[160];
    snprintf(text, sizeof(text),
        "RELAUNCH: %s %u bytes, %u segments 0x%x-0x%x, entry 0x%x, relaunch #%u\n",
        path, (unsigned)size, (unsigned)info.segments, (unsigned)info.image_low,
        (unsigned)info.image_high, (unsigned)info.entry, (unsigned)info.relaunch_count);
    console_write(text);
    relaunch_enter_now();
}

static void network_save_command(void)
{
    uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
    struct book8_network_settings saved = network_settings;
    if (saved.mode == BOOK8_NETWORK_STATIC)
        saved.telemetry = telemetry_target;
    if (!book8_network_settings_encode(&saved, bytes)
        || !fs_write_file(BOOK8_NETWORK_SETTINGS_PATH, bytes, sizeof(bytes))) {
        console_write("NETSAVE: cannot write " BOOK8_NETWORK_SETTINGS_PATH "\n");
        return;
    }
    console_write("NETSAVE: wrote mode/settings to " BOOK8_NETWORK_SETTINGS_PATH "\n");
}

/* Runs once after the filesystem is up: A:\BOOK8NET.CFG overrides the baked
 * build defaults; book8.net=off on the command line skips both. */
static void network_boot_configure(void)
{
    network_boot_tsc = read_tsc();
    if (boot_relaunch_count != 0u) {
        char text[64];
        snprintf(text, sizeof(text), "relaunched kernel #%u build %u",
            (unsigned)boot_relaunch_count, (unsigned)BOOK8_BUILD_ID);
        console_write("BOOK8-RELAUNCH: ");
        console_write(text);
        console_write("\n");
        telemetry_event(text);
    }
    if (!network_ready) {
        return;
    }
    push_agent_start();
    if (cmdline_network_off) {
        console_write("BOOK8-NET: book8.net=off; baked/file config skipped.\n");
        return;
    }
    {
        static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
        struct book8_network_settings saved = network_settings;
        char mode[BOOK8_REGISTRY_VALUE_MAX], text[BOOK8_REGISTRY_VALUE_MAX];
        bool have_registry = reg_get_str(path, "NetMode", mode, sizeof(mode));
        if (have_registry) {
            if (strcmp(mode, "DHCP") == 0) {
                saved.mode = BOOK8_NETWORK_DHCP;
            } else if (strcmp(mode, "Static") == 0) {
                saved.mode = BOOK8_NETWORK_STATIC;
            } else {
                console_write("BOOK8-NET: invalid registry mode; retaining DHCP defaults\n");
                return;
            }
            if (reg_get_str(path, "StaticIP", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.ip)) {
                console_write("BOOK8-NET: invalid registry StaticIP\n");
                return;
            }
            if (reg_get_str(path, "Netmask", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.mask)) {
                console_write("BOOK8-NET: invalid registry Netmask\n");
                return;
            }
            if (reg_get_str(path, "Gateway", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.gateway)) {
                console_write("BOOK8-NET: invalid registry Gateway\n");
                return;
            }
            if (reg_get_str(path, "DNS", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.dns)) {
                console_write("BOOK8-NET: invalid registry DNS\n");
                return;
            }
            if (!book8_network_settings_apply(&saved, false)) {
                console_write("BOOK8-NET: registry network settings failed to apply\n");
            }
            return;
        }
    }
    uint8_t settings_bytes[BOOK8_NETWORK_SETTINGS_BYTES + 1u];
    uint32_t settings_size = 0u;
    if (fs_read_file(BOOK8_NETWORK_SETTINGS_PATH, settings_bytes, sizeof(settings_bytes), &settings_size)) {
        struct book8_network_settings saved;
        if (book8_network_settings_decode(&saved, settings_bytes, settings_size)) {
            if (!book8_network_settings_apply(&saved, false))
                console_write("BOOK8-NET: saved network mode failed to apply\n");
            return;
        }
        console_write("BOOK8-NET: malformed " BOOK8_NETWORK_SETTINGS_PATH "; retaining DHCP defaults\n");
        return;
    }
    static char text[BOOK8_NETCFG_TEXT_MAX];
    struct book8_netcfg config;
    const char *source = NULL;
    uint32_t size = 0u;
    if (fs_read_file(BOOK8_NETCFG_PATH, text, sizeof(text), &size)) {
        if (book8_netcfg_parse(text, size, &config)) {
            source = BOOK8_NETCFG_PATH;
        } else {
            console_write("BOOK8-NET: ignoring malformed " BOOK8_NETCFG_PATH "\n");
        }
    }
    if (source == NULL) {
        console_write("BOOK8-NET: DHCP default; static fallback only after 30-second timeout\n");
        return;
    }
    struct book8_network_settings legacy = {.mode = BOOK8_NETWORK_DHCP,
        .ip = config.ip, .mask = config.mask, .gateway = config.gateway,
        .telemetry = config.telemetry, .retry_ms = network_settings.retry_ms};
    if (!book8_network_settings_apply(&legacy, false))
        console_write("BOOK8-NET: legacy DHCP fallback settings failed to apply\n");
    else
        console_write("BOOK8-NET: legacy static values loaded as DHCP timeout fallback only\n");
}

static void print_threads(void)
{
    struct sched_thread_info threads[SCHED_MAX_THREADS];
    uint32_t count = sched_snapshot(threads, SCHED_MAX_THREADS);

    console_write("TID  NAME                             STATE    CPU TICKS\n");
    for (uint32_t i = 0u; i < count && i < SCHED_MAX_THREADS; ++i) {
        print_unsigned(threads[i].tid);
        console_write("  ");
        console_write(threads[i].name);
        console_write("  ");
        console_write(sched_state_name(threads[i].state));
        console_write("  ");
        print_unsigned(threads[i].cpu_ticks);
        console_putc('\n');
    }
}

static void run_command(char *line)
{
    char *command = skip_spaces(line);
    char *argument = command;
    while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
        ++argument;
    }
    if (*argument != '\0') {
        *argument++ = '\0';
        argument = skip_spaces(argument);
    }

    if (*command == '\0' || ascii_equal(command, "REM")) {
        return;
    }
    if (ascii_equal(command, "REG")) {
        registry_command(argument);
        return;
    }
    if (fs_handle_command(command, argument)) {
        return;
    }
    if (ascii_equal(command, "HELP") || ascii_equal(command, "?")) {
        print_help();
    } else if (ascii_equal(command, "PCIDUMP") || ascii_equal(command, "LSPCI")) {
        pcidump_command();
    } else if (ascii_equal(command, "HWDIAG")) {
        hwdiag_command();
    } else if (ascii_equal(command, "NVINFO")) {
        nvinfo_command();
    } else if (ascii_equal(command, "PNPSCAN") || ascii_equal(command, "RESCAN")) {
        pnp_scan_command();
    } else if (ascii_equal(command, "MEMDUMP")) {
        memdump_command(argument);
    } else if (ascii_equal(command, "IOPORT")) {
        ioport_command(argument);
    } else if (ascii_equal(command, "MSRGUARDTEST")) {
        msr_guard_test();
    } else if (ascii_equal(command, "PCIEPROBE") || ascii_equal(command, "ACK")) {
        probe_command(argument, ascii_equal(command, "ACK"));
    } else if (ascii_equal(command, "VER")) {
        console_write("BOOK8 x86-64 experimental kernel 0.2\n");
    } else if (ascii_equal(command, "CLS")) {
        console_clear();
    } else if (ascii_equal(command, "ECHO")) {
        console_write(argument);
        console_putc('\n');
    } else if (ascii_equal(command, "DATE")) {
        print_date();
    } else if (ascii_equal(command, "TIME")) {
        print_time();
    } else if (ascii_equal(command, "USBSTAT")) {
        print_usb_poll_status();
    } else if (ascii_equal(command, "INPUTSTAT")) {
        print_input_status();
    } else if (ascii_equal(command, "THREADS")) {
        print_threads();
    } else if (ascii_equal(command, "NETSTAT")) {
        network_status();
    } else if (ascii_equal(command, "NETCONFIG")) {
        network_configure_command(argument);
    } else if (ascii_equal(command, "TELEMETRY")) {
        telemetry_command(argument);
    } else if (ascii_equal(command, "NETBOOT")) {
        network_boot_command(argument);
    } else if (ascii_equal(command, "NETSAVE")) {
        network_save_command();
    } else if (ascii_equal(command, "RELAUNCH")) {
        relaunch_command(argument);
    } else if (ascii_equal(command, "USBREG")) {
        console_write(book8_usb_diag_log());
    } else if (ascii_equal(command, "LIBTEST")) {
        int failed;
        stdio_set_output(console_putc);
        failed = libc_selftest(console_write);
        printf("libc self-test: %d checks, %d failed\n", libc_selftest_checks(), failed);
    } else if (ascii_equal(command, "RING3TEST")) {
        long status = syscall_ring3_smoke();
        if (status == 0) {
            console_write("RING3TEST: PASS\n");
        } else {
            stdio_set_output(console_putc);
            printf("RING3TEST: FAIL status=%d\n", (int)status);
        }
    } else if (ascii_equal(command, "SCHEDTEST")) {
        scheduler_test(false);
    } else if (ascii_equal(command, "SCHEDUSER")) {
        scheduler_test(true);
    } else if (ascii_equal(command, "SMPTEST")) {
        smp_scheduler_test();
    } else if (ascii_equal(command, "VMTEST")) {
        uint64_t flags = irq_save();
        bool passed = exec_address_space_test();
        irq_restore(flags);
        console_write(passed ? "VMTEST: PASS\n" : "VMTEST: FAIL\n");
    } else if (ascii_equal(command, "SWAPTEST")) {
        bool passed = vmm_swap_selftest();
        console_write(passed ? "SWAPTEST: PASS\n" : "SWAPTEST: FAIL\n");
    } else if (ascii_equal(command, "EXEC")) {
        run_exec(argument);
    } else if (ascii_equal(command, "SET")) {
        set_environment(argument);
    } else if (ascii_equal(command, "PATH")) {
        if (*argument != '\0') {
            copy_string(search_path, sizeof(search_path), argument);
        }
        console_write("PATH=");
        console_write(search_path);
        console_putc('\n');
    } else if (ascii_equal(command, "PROMPT")) {
        if (*argument != '\0') {
            copy_string(prompt_format, sizeof(prompt_format), argument);
        }
    } else if (ascii_equal(command, "CD") || ascii_equal(command, "CHDIR")) {
        if (*argument == '\0') {
            console_write(fs_current_path());
            console_putc('\n');
        } else {
            run_filesystem_command(command);
        }
    } else if (ascii_equal(command, "DISK")) {
        print_disk_inventory();
    } else if (ascii_equal(command, "INSTALL")) {
        install_command(argument);
    } else if (ascii_equal(command, "BOOK8GUI")) {
        gui_launch(false);
    } else if (ascii_equal(command, "VOL")) {
        console_write("No filesystem volume is mounted. Run DISK to inspect ATA devices.\n");
    } else if (ascii_equal(command, "READ")) {
        run_read_command(argument);
    } else if (ascii_equal(command, "WRITE")) {
        run_write_command(argument);
    } else if (ascii_equal(command, "DIR") || ascii_equal(command, "TYPE")
        || ascii_equal(command, "COPY") || ascii_equal(command, "DEL")
        || ascii_equal(command, "ERASE") || ascii_equal(command, "MD")
        || ascii_equal(command, "MKDIR") || ascii_equal(command, "RD")
        || ascii_equal(command, "RMDIR")) {
        run_filesystem_command(command);
    } else if (ascii_equal(command, "ATTRIB") || ascii_equal(command, "CHKDSK")
        || ascii_equal(command, "DEFRAG") || ascii_equal(command, "EDIT")
        || ascii_equal(command, "FDISK") || ascii_equal(command, "FORMAT")
        || ascii_equal(command, "MOVE") || ascii_equal(command, "SCANDISK")
        || ascii_equal(command, "SYS") || ascii_equal(command, "XCOPY")) {
        run_external_command(command);
    } else if (ascii_equal(command, "EXIT") || ascii_equal(command, "QUIT")
        || ascii_equal(command, "BYE")) {
        console_write("BOOK8 shutdown requested.\n");
        acpi_shutdown();
    } else if (ascii_equal(command, "HALT")) {
        console_write("BOOK8 halted.\n");
        acpi_shutdown();
    } else if (ascii_equal(command, "REBOOT") || ascii_equal(command, "RESET")) {
        console_write("BOOK8 reboot requested.\n");
        reboot_system();
    } else {
        console_write("Bad command or file name.\n");
    }
}

static void shell(void)
{
    char line[MAX_LINE];
    pit_clock_start();
    (void)pit_clock_ticks();
    if (tsc_hz == 0u)
        tsc_calibrate();
    interrupt_table_initialize();
    smp_runtime_initialize();
    if (autostart_desktop && installer_image == NULL
        && screen.pixels != NULL) {
        serial_write("BOOK8-BOOT: automatic desktop handoff\r\n");
        startup_quiet = true;
        console_clear();
        gui_launch(true);
        startup_quiet = false;
    }

    console_write("BOOK8 x86-64 command interpreter 0.2\n");
    console_write("IDE PIO + AHCI SATA. Type HELP for commands.\n");
    console_write(ps2_keyboard_controller_ready
        ? "PS/2 keyboard controller configured for polling.\n"
        : "PS/2 keyboard controller unavailable; serial input remains available.\n");
    console_write(ps2_mouse_available
        ? "PS/2 mouse ready.\n"
        : "PS/2 mouse unavailable.\n");
    if (book8_usb_keyboard_active()) {
        console_write("USB HID keyboard active.\n");
    } else {
        console_write(ps2_keyboard_controller_ready
            ? "USB HID keyboard unavailable; PS/2/serial fallback enabled.\n"
            : "USB HID keyboard unavailable; serial fallback enabled.\n");
    }
    console_write(installer_image != NULL
        ? "Installer payload loaded; use INSTALL to choose a target disk.\n"
        : "To install BOOK8, boot the BOOK8 Installer entry from the install ISO.\n");
    console_write("USB host controllers (total/UHCI/OHCI/EHCI/xHCI): ");
    print_unsigned(book8_usb_detected_controller_count());
    console_write("/");
    print_unsigned(book8_usb_uhci_controller_count());
    console_write("/");
    print_unsigned(book8_usb_ohci_controller_count());
    console_write("/");
    print_unsigned(book8_usb_ehci_controller_count());
    console_write("/");
    print_unsigned(book8_usb_xhci_controller_count());
    console_write(book8_usb_mouse_active()
        ? "; USB HID mouse active (UHCI/OHCI).\n"
        : "; USB HID mouse unavailable (no UHCI/OHCI boot mouse found; xHCI unsupported).\n");
    struct book8_usb_diagnostics usb_diagnostics;
    book8_usb_get_diagnostics(&usb_diagnostics);
    console_write("OHCI started/ports/connected/HID/hubs/reports/errors: ");
    print_unsigned(usb_diagnostics.ohci_started);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_connected_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_hid_devices);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_hub_devices);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_reports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_errors);
    console_write("; reset/control/ownership failures: ");
    print_unsigned(usb_diagnostics.ohci_reset_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_control_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_ownership_failures);
    console_write("; last CC/stage/retries: ");
    print_unsigned(usb_diagnostics.ohci_last_cc);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_last_stage);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_control_retries);
    console_write("; EHCI released to OHCI: ");
    print_unsigned(usb_diagnostics.ehci_released_to_ohci);
    console_putc('\n');
    console_write("USB enumeration (connected/reset/addressed/boot-HID/active): ");
    print_unsigned(usb_diagnostics.connected_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.reset_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.addressed_devices);
    console_putc('/');
    print_unsigned(usb_diagnostics.boot_hid_interfaces);
    console_putc('/');
    print_unsigned(usb_diagnostics.active_hid_devices);
    console_write("; UHCI running/schedule failures: ");
    print_unsigned(usb_diagnostics.uhci_running_controllers);
    console_putc('/');
    print_unsigned(usb_diagnostics.uhci_schedule_failures);
    console_write("; HID interrupt completions: ");
    print_unsigned(usb_diagnostics.uhci_interrupt_completions);
    console_write("; UHCI stalled HID schedules: ");
    print_unsigned(usb_diagnostics.uhci_interrupt_stalls);
    console_write("; EHCI legacy handoffs: ");
    print_unsigned(usb_diagnostics.ehci_legacy_handoffs);
    console_write("; handoff failures: ");
    print_unsigned(usb_diagnostics.ehci_legacy_handoff_failures);
    console_write("; handoffs skipped without UHCI: ");
    print_unsigned(usb_diagnostics.ehci_legacy_handoff_skipped);
    console_write("; EHCI powered/companion ports: ");
    print_unsigned(usb_diagnostics.ehci_powered_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ehci_companion_ports);
    console_write("; companion UHCI mapped/unmapped: ");
    print_unsigned(usb_diagnostics.ehci_uhci_companion_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ehci_unmapped_companion_ports);
    console_write("; companion mappings/static fallbacks: ");
    print_unsigned(usb_diagnostics.ehci_companion_mappings);
    console_putc('/');
    print_unsigned(usb_diagnostics.ehci_route_fallbacks);
    console_putc('\n');
    console_write("USB PCI command enable failures: ");
    print_unsigned(usb_diagnostics.pci_enable_failures);
    console_putc('\n');
    console_write("USB descriptor failures (device/config/HID-request/non-boot): ");
    print_unsigned(usb_diagnostics.device_descriptor_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.configuration_descriptor_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.hid_control_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.non_boot_hid_devices);
    console_putc('\n');

    bool diag_first_prompt = true;
    bool diag_first_key = true;
    for (;;) {
        __asm__ volatile ("sti" : : : "memory");
        uint32_t length = 0u;
        if (diag_first_prompt) {
            diag_marker("M1 shell loop top");
        }
        print_prompt();
        if (diag_first_prompt) {
            diag_marker("M2 prompt printed");
            diag_first_prompt = false;
        }
        for (;;) {
            char value = read_char();
            if (diag_first_key) {
                diag_marker("K first key");
                diag_first_key = false;
            }
            if (value == '\r' || value == '\n') {
                console_putc('\n');
                line[length] = '\0';
                break;
            }
            if ((value == '\b' || value == 0x7f) && length > 0u) {
                --length;
                console_putc('\b');
            } else if (value >= 0x20 && value <= 0x7e
                && length + 1u < sizeof(line)) {
                line[length++] = value;
                console_putc(value);
            }
        }
        run_command(line);
    }
}

static bool exec_drive_extent(uint32_t drive, struct fs_extent *extent)
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

long book8_console_write(const char *buf, unsigned long len)
{
    unsigned long i;

    for (i = 0; i < len; ++i) {
        console_putc(buf[i]); /* console_putc already mirrors to serial */
    }
    return (long)len;
}

void kernel_main(uint32_t magic, uint32_t info_address)
{
    book8_boot_log_begin(BOOK8_DIAG_BOOT_LOG != 0);
    serial_init();
    serial_write("BOOK8-BOOT: kernel_main entered\r\n");
    boot_tsc = read_tsc();
    netboot_info_address = info_address;
    syscall_init();
    screen.pixels = NULL;
    screen.pitch_bytes = 0u;
    screen.width = 0u;
    screen.height = 0u;
    screen.scanout_pitch_bytes = 0u;
    screen.scanout_width = 0u;
    screen.scanout_height = 0u;
    screen.bytes_per_pixel = 0u;
    screen.red_position = 0u;
    screen.red_mask_size = 0u;
    screen.green_position = 0u;
    screen.green_mask_size = 0u;
    screen.blue_position = 0u;
    screen.blue_mask_size = 0u;
    use_vga = false;
    cursor_x = 0u;
    cursor_y = 0u;
    vga_cursor = 0u;
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC || info_address == 0u) {
        console_write("BOOK8: invalid Multiboot2 boot handoff.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    const uint32_t *info = (const uint32_t *)(uintptr_t)info_address;
    uint32_t total_size = info[0];
    bool framebuffer_tag_seen = false;
    smp_trampoline_page_usable = false;
    smp_trampoline_page_conflict =
        (uint64_t)info_address < 0x9000u
        && (uint64_t)info_address + total_size > SMP_TRAMPOLINE_PHYS;
    autostart_desktop = true;
    initialization_complete = false;
    cmdline_network_off = false;
    boot_relaunch_count = 0u;
    startup_quiet = false;
    if (total_size < 16u) {
        console_write("BOOK8: invalid Multiboot2 information block.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)((const uint8_t *)info + offset);
        if (tag->type == MULTIBOOT_TAG_END || tag->size < sizeof(*tag)
            || tag->size > total_size - offset) {
            break;
        }
        if (tag->type == MULTIBOOT_TAG_CMDLINE
            && tag->size > sizeof(*tag)) {
            const char *cmdline = (const char *)tag + sizeof(*tag);
            uint32_t cmdline_size = tag->size - (uint32_t)sizeof(*tag);
            if (cmdline_has_option(cmdline, cmdline_size,
                "book8.autostart=0")) {
                autostart_desktop = false;
            }
            cmdline_network_off = cmdline_has_option(cmdline, cmdline_size,
                "book8.net=off");
            boot_relaunch_count = book8_relaunch_count(cmdline, cmdline_size);
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            break;
        }
        offset += next;
    }
    startup_quiet = autostart_desktop;

    pcie_bootstrap();
    acpi_discover((const uint8_t *)info, total_size);
    if (!acpi_s5_available) {
        acpi_discover_firmware();
    }
    book8_pcie_enumerate(&pcie);

    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)((const uint8_t *)info + offset);
        if (tag->type == MULTIBOOT_TAG_END || tag->size < sizeof(*tag)
            || tag->size > total_size - offset) {
            break;
        }
        if (tag->type == MULTIBOOT_TAG_MMAP && tag->size >= 16u) {
            const uint32_t *words = (const uint32_t *)tag;
            uint32_t entry_size = words[2];
            uint64_t total = 0u;
            for (uint32_t pos = 16u; entry_size >= 24u
                && pos + entry_size <= tag->size; pos += entry_size) {
                const uint8_t *entry = (const uint8_t *)tag + pos;
                const uint64_t *fields = (const uint64_t *)entry;
                uint64_t region_end = fields[0] + fields[1];
                if (*(const uint32_t *)(entry + 16u) == 1u
                    && fields[0] <= SMP_TRAMPOLINE_PHYS
                    && region_end >= SMP_TRAMPOLINE_PHYS + 4096u)
                    smp_trampoline_page_usable = true;
                if (control_memory_count < 12u) {
                    control_memory_map[control_memory_count].base = fields[0];
                    control_memory_map[control_memory_count].size = fields[1];
                    control_memory_map[control_memory_count].type =
                        *(const uint32_t *)(entry + 16u);
                    ++control_memory_count;
                }
                if (*(const uint32_t *)(entry + 16u) == 1u) {
                    total += fields[1] / 1024u;
                }
            }
            if (total != 0u) {
                memory_total_kb = total;
            }
        } else if (tag->type == MULTIBOOT_TAG_BASIC_MEMINFO
            && tag->size >= 16u && memory_total_kb == 0u) {
            const uint32_t *words = (const uint32_t *)tag;
            memory_total_kb = (uint64_t)words[2] + words[3] + 1024u;
        } else if (tag->type == MULTIBOOT_TAG_MODULE
            && tag->size >= sizeof(struct multiboot_tag_module)) {
            const struct multiboot_tag_module *module =
                (const struct multiboot_tag_module *)tag;
            if (module->start < SMP_TRAMPOLINE_PHYS + 4096u
                && module->end > SMP_TRAMPOLINE_PHYS)
                smp_trampoline_page_conflict = true;
            const char *name_end = module->command_line;
            const char *tag_end = (const char *)tag + tag->size;
            while (name_end < tag_end && *name_end != '\0') {
                ++name_end;
            }
            if (name_end < tag_end && installer_image == NULL
                && ascii_equal(module->command_line, "book8-installer")
                && module->end > module->start) {
                installer_image = (const uint8_t *)(uintptr_t)module->start;
                installer_image_size = module->end - module->start;
            }
            if (name_end < tag_end && application_image == NULL
                && ascii_equal(module->command_line, "book8-apps")
                && module->end > module->start) {
                uint64_t size = module->end - module->start;
                if ((size & 511u) == 0u && size <= 16u * 1024u * 1024u) {
                    application_image = (uint8_t *)(uintptr_t)module->start;
                    application_image_size = size;
                } else {
                    console_write("BOOK8-APPS: invalid application module size.\n");
                }
            }
        } else if (tag->type == MULTIBOOT_TAG_FRAMEBUFFER
            && tag->size >= MULTIBOOT_FRAMEBUFFER_RGB_TAG_SIZE) {
            framebuffer_tag_seen = true;
            const struct multiboot_tag_framebuffer *fb =
                (const struct multiboot_tag_framebuffer *)tag;
            uint8_t pixel_bytes =
                (uint8_t)((fb->bits_per_pixel + 7u) / 8u);
            if (fb->framebuffer_type == 1u
                && (fb->bits_per_pixel == 24u
                    || fb->bits_per_pixel == 32u)
                && (fb->address >> 32u) == 0u && fb->width != 0u
                && fb->height != 0u
                && (uint64_t)fb->pitch
                    >= (uint64_t)fb->width * pixel_bytes
                && (uint64_t)fb->pitch * fb->height
                    <= FRAMEBUFFER_MAX_BYTES
                && (uint64_t)fb->pitch * fb->height
                    <= 0x100000000ull - fb->address
                && fb->red_position < 32u && fb->green_position < 32u
                && fb->blue_position < 32u) {
                if (fb->red_mask_size == 0u || fb->red_mask_size > 8u
                    || fb->green_mask_size == 0u
                    || fb->green_mask_size > 8u
                    || fb->blue_mask_size == 0u
                    || fb->blue_mask_size > 8u
                    || fb->red_position + fb->red_mask_size
                        > fb->bits_per_pixel
                    || fb->green_position + fb->green_mask_size
                        > fb->bits_per_pixel
                    || fb->blue_position + fb->blue_mask_size
                        > fb->bits_per_pixel) {
                    screen.pixels = NULL;
                    use_vga = true;
                    break;
                }
                uint32_t red_mask =
                    ((1u << fb->red_mask_size) - 1u) << fb->red_position;
                uint32_t green_mask =
                    ((1u << fb->green_mask_size) - 1u) << fb->green_position;
                uint32_t blue_mask =
                    ((1u << fb->blue_mask_size) - 1u) << fb->blue_position;
                if ((red_mask & green_mask) != 0u
                    || (red_mask & blue_mask) != 0u
                    || (green_mask & blue_mask) != 0u) {
                    screen.pixels = NULL;
                    use_vga = true;
                    break;
                }
                /* boot.S identity maps physical 0..4 GiB; the validated range
                 * is therefore also the framebuffer's kernel virtual mapping. */
                screen.pixels = (volatile uint8_t *)(uintptr_t)fb->address;
                screen.pitch_bytes = fb->pitch;
                screen.width = fb->width;
                screen.height = fb->height;
                screen.scanout_pitch_bytes = fb->pitch;
                screen.scanout_width = fb->width;
                screen.scanout_height = fb->height;
                screen.bytes_per_pixel = pixel_bytes;
                screen.red_position = fb->red_position;
                screen.red_mask_size = fb->red_mask_size;
                screen.green_position = fb->green_position;
                screen.green_mask_size = fb->green_mask_size;
                screen.blue_position = fb->blue_position;
                screen.blue_mask_size = fb->blue_mask_size;
                use_vga = false;
                if (!fb_init(&fbdev, (void *)screen.pixels, screen.width,
                        screen.height, screen.pitch_bytes,
                        &(struct fb_format){
                            .bpp = fb->bits_per_pixel,
                            .red_pos = fb->red_position,
                            .red_size = fb->red_mask_size,
                            .green_pos = fb->green_position,
                            .green_size = fb->green_mask_size,
                            .blue_pos = fb->blue_position,
                            .blue_size = fb->blue_mask_size})) {
                    screen.pixels = NULL;
                    use_vga = true;
                } else if ((uint64_t)fbdev.pitch * fbdev.height
                    <= GUI_BACKBUFFER_BYTES) {
                    fb_set_backbuffer(&fbdev, gui_backbuffer);
                }
            } else {
                /* Some real BIOS/VBE implementations advertise a framebuffer but
                 * do not support the requested handshake or pixel layout.
                 * Falling back to VGA text avoids a hard lockup on unsupported
                 * mode negotiation. */
                screen.pixels = NULL;
                use_vga = true;
            }
            break;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            break;
        }
        offset += next;
    }

    smp_trampoline_page_usable &= !smp_trampoline_page_conflict;
    if (vmm_init(info))
        serial_write("BOOK8-VMM: kernel page arena ready\r\n");
    else
        serial_write("BOOK8-VMM: unavailable; kernel continues without arena\r\n");
    startup_quiet = autostart_desktop && installer_image == NULL
        && screen.pixels != NULL;
    char handoff[128];
    if (screen.pixels != NULL) {
        snprintf(handoff, sizeof(handoff),
            "BOOK8 framebuffer handoff: %ux%ux%u pitch=%u address=0x%08x",
            screen.width, screen.height, screen.bytes_per_pixel * 8u,
            screen.pitch_bytes, (uint32_t)(uintptr_t)screen.pixels);
    } else {
        snprintf(handoff, sizeof(handoff), "BOOK8 framebuffer handoff: %s",
            framebuffer_tag_seen ? "tag rejected; VGA fallback."
                : "tag missing; VGA fallback.");
    }
    if (startup_quiet) {
        serial_write(handoff);
        serial_write("\r\n");
    } else {
        console_write(handoff);
        console_write("\n");
    }

    if (screen.pixels == NULL) {
        use_vga = true;
        VGA_MEMORY[0] = 0x0f4fu;
        VGA_MEMORY[1] = 0x0f42u;
        VGA_MEMORY[2] = 0x0f41u;
        VGA_MEMORY[3] = 0x0f4du;
        VGA_MEMORY[4] = 0x0f45u;
        VGA_MEMORY[5] = 0x0f42u;
        VGA_MEMORY[6] = 0x0f41u;
        VGA_MEMORY[7] = 0x0f43u;
        VGA_MEMORY[8] = 0x0f4bu;
        VGA_MEMORY[9] = 0x0f3au;
        VGA_MEMORY[10] = 0x0f20u;
        VGA_MEMORY[11] = 0x0f56u;
        VGA_MEMORY[12] = 0x0f47u;
        VGA_MEMORY[13] = 0x0f41u;
        VGA_MEMORY[14] = 0x0f20u;
        VGA_MEMORY[15] = 0x0f20u;
        console_clear();
        console_write("BOOK8: VGA fallback active; framebuffer rejected or missing.\n");
    }
    if (screen.pixels != NULL) {
        (void)ps2_mouse_initialize();
    }
    discover_storage();
    network_early_telemetry();
    register_block_devices();
    swap_init(memory_total_kb * 1024u);
    if (swap_available()) {
        serial_write("BOOK8-SWAP: raw partition enabled, slots=");
        char swap_slots_text[24];
        snprintf(swap_slots_text, sizeof(swap_slots_text), "%u",
            swap_total_slots());
        serial_write(swap_slots_text);
        serial_write("\r\n");
    } else {
        serial_write("BOOK8-SWAP: no valid raw partition; swapping disabled\r\n");
    }
    ps2_keyboard_controller_ready = ps2_keyboard_initialize();
    pointer_x = screen.width != 0u ? (int32_t)(screen.width / 2u) : 0;
    pointer_y = screen.height != 0u ? (int32_t)(screen.height / 2u) : 0;
    pointer_buttons = 0u;
    usb_key_queue_head = 0u;
    usb_key_queue_tail = 0u;
    usb_previous_keyboard_report = (struct book8_usb_keyboard_report){0};
    usb_keyboard_leds = 0u;
    fs_initialize(filesystem_read_sector, filesystem_write_sector,
        filesystem_disk_size, filesystem_print, confirm_text);
    serial_write("BOOK8-BOOT: filesystem initialization complete\r\n");
    fs_set_drive_resolver(exec_drive_extent);
    registry_write_allowed = installer_image == NULL;
    reg_set_storage(fs_read_file, registry_write_file);
    serial_write("BOOK8-BOOT: registry load started\r\n");
    bool registry_loaded = reg_load();
    serial_write("BOOK8-BOOT: registry load finished\r\n");
    if (!registry_loaded) {
        console_write("REGISTRY: file unavailable or invalid; using defaults.\n");
    }
    registry_seed_defaults();
    uint64_t session_seed = pit_clock_ticks() ^ read_tsc()
        ^ (uint64_t)(uintptr_t)screen.pixels;
    if (!book8_session_initialize(session_seed)) {
        console_write("SESSION: initialization failed; desktop sign-in will remain locked.\n");
    }
    bool usb_host_module_ready = usb_host_module_load();
    bool rtl8168_module_ready = load_trusted_driver(
        "A:\\DRIVERS\\RTL8168.DRV", &rtl8168_driver) == 0;
    bool hda_module_ready = hda_module_load();
    (void)book8_pcie_bind_bus_drivers(&pcie, bus_drivers,
        sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    if (usb_host_module_ready) {
        book8_usb_register_bus_drivers(bus_drivers,
            sizeof(bus_drivers) / sizeof(bus_drivers[0]));
        book8_usb_initialize_from_pci(&pcie);
        serial_write("BOOK8-USB: module initialized\r\n");
    } else {
        serial_write("BOOK8-USB: module unavailable; USB input disabled\r\n");
    }
    if (rtl8168_module_ready) {
        bool rtl8168_found = false;
        for (size_t i = 0u; i < pcie.node_count; ++i) {
            const struct book8_pcie_node *node = &pcie.nodes[i];
            if (node->segment == 0u
                && node->bus_device.vendor_id == RTL8168_VENDOR_ID
                && node->bus_device.device_id == RTL8168_DEVICE_ID
                && (node->bus_device.class_code & 0xff0000u) == 0x020000u) {
                rtl8168_found = true;
                break;
            }
        }
        if (!rtl8168_found)
            serial_write("BOOK8-RTL8168: module loaded; no matching NIC found\r\n");
    } else {
        serial_write("BOOK8-RTL8168: module unavailable; network disabled\r\n");
    }
    if (hda_module_ready) {
        bool hda_found = false;
        for (size_t i = 0u; i < pcie.node_count; ++i) {
            const struct book8_pcie_node *node = &pcie.nodes[i];
            if (node->segment == 0u
                && node->bus_device.vendor_id == HDA_ATI_VENDOR_ID
                && node->bus_device.device_id == HDA_ATI_DEVICE_ID
                && (node->bus_device.class_code & 0xffff00u) == 0x040300u) {
                hda_found = true;
                break;
            }
        }
        if (!hda_found)
            serial_write("BOOK8-HDA: module loaded; no matching controller found\r\n");
        else if (!hda_controller.present)
            serial_write("BOOK8-HDA: matching controller initialization failed; audio disabled\r\n");
    } else {
        serial_write("BOOK8-HDA: module unavailable; audio disabled\r\n");
    }
    serial_write("BOOK8-BOOT: USB initialization complete\r\n");
    serial_write("BOOK8-BOOT: PCI registry synchronization started\r\n");
    bool pcie_registry_ok = book8_pcie_registry_sync(&pcie);
    serial_write("BOOK8-BOOT: PCI registry synchronization finished\r\n");
    bool usb_registry_ok = false;
    if (pcie_registry_ok) {
        serial_write("BOOK8-BOOT: USB registry synchronization started\r\n");
        usb_registry_ok = book8_usb_registry_sync();
        serial_write("BOOK8-BOOT: USB registry synchronization finished\r\n");
    }
    bool registry_saved = !registry_write_allowed;
    if (pcie_registry_ok && usb_registry_ok && registry_write_allowed) {
        serial_write("BOOK8-BOOT: registry save started\r\n");
        registry_saved = reg_save();
        serial_write("BOOK8-BOOT: registry save finished\r\n");
    }
    if (!pcie_registry_ok || !usb_registry_ok || !registry_saved) {
        console_write("REGISTRY: initial hardware sync/save failed.\n");
    } else if (!registry_write_allowed) {
        console_write("REGISTRY: persistent save deferred until INSTALL mounts the target root.\n");
    }
    serial_write("BOOK8-BOOT: registry initialization complete\r\n");
#if BOOK8_DIAG_BOOT_LOG
    if (!book8_boot_log_mount(application_image != NULL ? 1u : 0u)) {
        console_write("BOOK8-NVLOG: ERROR "); console_write(book8_boot_log_error()); console_write("\n");
    } else {
        console_write("BOOK8-NVLOG: writing initialization checkpoints to ");
        console_write(book8_boot_log_path()); console_write("\n");
    }
#endif
    network_boot_configure();
    if (pcie.target_present && !network_calibrate_clock()) {
        pcie_log("PCIE1 ERROR clock calibration unavailable; D0 transitions may fail");
    }
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: BEGIN PCIe target preparation\n");
#endif
    (void)book8_pcie_prepare_target(&pcie);
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: END PCIe target preparation; BEGIN NVIDIA probe\n");
#endif
    nv_gpu_init();
    record_nv_gpu_bars();
    if (!book8_pcie_registry_sync(&pcie) || !nv_registry_sync()
        || (registry_write_allowed && !reg_save())) {
        console_write("REGISTRY: post-GPU BAR sync/save failed.\n");
    }
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: END NVIDIA probe\n");
#endif
    hwdiag_init(&nv_gpu);
    initialization_complete = true;
    serial_write("BOOK8-BOOT: initialization complete; entering shell\r\n");
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: END initialization; entering shell\n");
    if (!book8_boot_log_finish()) {
        console_write("BOOK8-NVLOG: ERROR "); console_write(book8_boot_log_error()); console_write("\n");
    } else {
        console_write("BOOK8-NVLOG: saved "); console_write(book8_boot_log_path()); console_write("\n");
    }
#endif
    shell();
}
