#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/apps/book8_nvdiag.h"
#include "../include/hwdiag.h"
#include "../drivers/nv_maxwell.h"

#define EXPECTED_TAB_COUNT 8u
#define MAX_SAVES 16u
#define SAVE_CONTENT_SIZE 16384u

static char rendered[32768];
static char compact_rendered_text[32768];
static bool telemetry_available = true;
static bool writes_succeed = true;
static char saved_paths[MAX_SAVES][40];
static char saved_contents[MAX_SAVES][SAVE_CONTENT_SIZE];
static uint32_t saved_count;

static const uint32_t expected_tab_widths[EXPECTED_TAB_COUNT] = {
    64u, 64u, 64u, 78u, 84u, 150u, 84u, 70u
};
static const char *const expected_tab_names[EXPECTED_TAB_COUNT] = {
    "GPU", "FIFO", "BAR0", "Memory", "Hardware",
    "Firmware Platform", "Firmware", "Accel"
};

void book8gui_invalidate(void)
{
}

static void fill(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t color)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

static void draw_text(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background)
{
    (void)x;
    (void)y;
    (void)foreground;
    (void)background;
    assert(strlen(text) <= 96u);
    size_t used = strlen(rendered);
    if (used + strlen(text) + 2u < sizeof(rendered)) {
        snprintf(rendered + used, sizeof(rendered) - used, "%s\n", text);
    }
}

static bool write_file(const char *path, const void *data, uint32_t size)
{
    if (!writes_succeed || saved_count >= MAX_SAVES) {
        return false;
    }
    snprintf(saved_paths[saved_count], sizeof(saved_paths[0]), "%s", path);
    uint32_t length = size;
    if (length >= sizeof(saved_contents[0])) {
        length = (uint32_t)sizeof(saved_contents[0]) - 1u;
    }
    memcpy(saved_contents[saved_count], data, length);
    saved_contents[saved_count][length] = '\0';
    ++saved_count;
    return true;
}

static bool snapshot(struct book8_nvdiag_snapshot *out)
{
    if (!telemetry_available) {
        return false;
    }
    *out = (struct book8_nvdiag_snapshot){
        .flags = BOOK8_NVDIAG_PRESENT | BOOK8_NVDIAG_MAPPED
            | BOOK8_NVDIAG_PCI_CONFIG | BOOK8_NVDIAG_MMIO_READABLE
            | BOOK8_NVDIAG_SCANOUT_VALID | BOOK8_NVDIAG_CE_FOUND
            | BOOK8_NVDIAG_CE_VERIFIED | BOOK8_NVDIAG_ACCEL_ACTIVE
            | BOOK8_NVDIAG_PCI_EXPRESS,
        .register_flags = BOOK8_NVDIAG_REG_PMC_ENABLE
            | BOOK8_NVDIAG_REG_PFIFO_INTR,
        .pci_id = (NV_DEVICE_GTX970 << 16u) | NV_VENDOR_ID,
        .bdf = (1u << 8u) | (0u << 3u),
        .pci_command = 6u,
        .pci_status = 0x10u,
        .subsystem_id = 0x19803842u,
        .boot0 = 0x124020a1u,
        .chipset = 0x124u,
        .bar0_phys = 0xfd000000u,
        .bar0_size = 0x01000000u,
        .bar1_phys = 0xd0000000u,
        .bar1_size = 0x10000000u,
        .pmc_enable = 0x100u,
        .pfifo_intr = 0u,
        .accel_min_pixels = 4096u,
        .ce_engine = 1u,
        .ce_runlist = 2u,
        .ce_count = 1u,
        .ptop_devices = 3u,
        .accel_fills = 17u,
        .accel_copies = 9u,
        .accel_small_skips = 1234u,
        .accel_unsupported = 2u,
        .accel_fence_timeouts = 1u,
        .scanout_vram_offset = 0x3f00000u,
        .scanout_width = 800u,
        .scanout_height = 600u,
        .scanout_pitch = 3200u,
        .chip_revision = 4u,
        .foundry_id = 0u,
        .detected_vram_mb = 4096u,
        .pdisp_head_mask = 1u,
        .pdisp_active_head = 0u,
        .pdisp_htotal = 800u,
        .pdisp_vtotal = 600u,
        .pcie_link_cap = 0x103u,   /* Gen3, x16 */
        .pcie_link_status = 0x103u,
        .rom_bar = 0xfd000000u     /* disabled */
    };
    return true;
}

static bool system_info(struct book8gui_sysinfo *info)
{
    *info = (struct book8gui_sysinfo){
        .cpu_name = "AMD FX(tm)-8350 Eight-Core Processor",
        .cpu_threads = 8u,
        .cpu_mhz = 4000u,
        .memory_total_kb = 16u * 1024u * 1024u,
        .disk_count = 2u,
        .disk_total_kb = 512u * 1024u * 1024u,
        .disk_label = "Samsung SSD 850"
    };
    return true;
}

static bool platform_info(struct book8_platform_info *info)
{
    *info = (struct book8_platform_info){
        .flags = BOOK8_PLATFORM_SMBIOS_VALID | BOOK8_PLATFORM_ACPI_VALID
            | BOOK8_PLATFORM_DIMMS_VALID,
        .boot_mode = "Legacy BIOS/CSM (Multiboot2)",
        .board_vendor = "ASUSTeK",
        .board_product = "M5A97 R2.0",
        .bios_vendor = "American Megatrends",
        .bios_version = "2403",
        .bios_date = "03/12/2014",
        .cpu_family = 0x1fu,
        .cpu_model_id = 2u,
        .cpu_stepping = 0u,
        .dimm_count = 2u,
        .dimm_slots = 4u,
        .dimm_installed_mb = 16384u,
        .memory_layout = "DIMM_A1=8192 MB, DIMM_B1=8192 MB",
        .above_4g = BOOK8_PLATFORM_OFF,
        .resizable_bar = BOOK8_PLATFORM_OFF,
        .iommu = BOOK8_PLATFORM_OFF,
        .secure_boot = BOOK8_PLATFORM_OFF,
        .csm_video = BOOK8_PLATFORM_ON,
        .gop_video = BOOK8_PLATFORM_OFF,
        .fast_boot = BOOK8_PLATFORM_UNKNOWN
    };
    return true;
}

static void render_text(struct WindowApp *app,
    const struct book8gui_platform *platform)
{
    rendered[0] = '\0';
    book8_nvdiag_class.render(platform, app);
}

/* Rejoins wrapped viewport rows with spaces for exact-content assertions. */
static const char *compact_rendered(void)
{
    size_t n = 0u;
    for (size_t i = 0u; rendered[i] != '\0' && n + 1u < sizeof(compact_rendered_text); ++i) {
        compact_rendered_text[n++] = rendered[i] == '\n' ? ' ' : rendered[i];
    }
    compact_rendered_text[n] = '\0';
    return compact_rendered_text;
}

static void click_tab(struct WindowApp *app,
    const struct book8gui_platform *platform, uint32_t index)
{
    assert(index < EXPECTED_TAB_COUNT);
    uint32_t x = app->bounds.x + 4u; /* ui_client() left inset. */
    for (uint32_t i = 0u; i < index; ++i) {
        x += expected_tab_widths[i] + 2u;
    }
    x += expected_tab_widths[index] / 2u;
    struct book8gui_event event = {
        .type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = (int32_t)x,
        .mouse_y = (int32_t)(app->bounds.y + 35u),
        .mouse_buttons = 1u
    };
    assert(book8_nvdiag_class.handle_event(platform, app, &event));
    event.mouse_buttons = 0u;
    assert(book8_nvdiag_class.handle_event(platform, app, &event));
}

static void send_key(struct WindowApp *app,
    const struct book8gui_platform *platform, uint32_t key)
{
    struct book8gui_event event = {
        .type = BOOK8GUI_EVENT_KEY,
        .key = key
    };
    assert(book8_nvdiag_class.handle_event(platform, app, &event));
}

static void assert_hardware_content(const char *text)
{
    assert(strstr(text, "HARDWARE INVENTORY (live)") != NULL);
    assert(strstr(text, "GPU: NVIDIA GeForce GTX 970") != NULL);
    assert(strstr(text, "PCI: 01:00.0  ID 13c210de") != NULL);
    assert(strstr(text, "BAR0: 0xfd000000 + 16 MiB") != NULL);
    assert(strstr(text, "BAR1: 0xd0000000 + 256 MiB") != NULL);
    assert(strstr(text, "VRAM from PFB_CSTATUS: 4096 MiB") != NULL);
    assert(strstr(text, "Chipset 0x124, revision 0x4, foundry 0x0") != NULL);
    assert(strstr(text, "Display: 800x600, pitch 3200") != NULL);
    assert(strstr(text, "GPU PCIe link: Gen3 x16") != NULL);
    assert(strstr(text, "CPU: AMD FX(tm)-8350 Eight-Core Processor") != NULL);
    assert(strstr(text, "family 1fh model 02h stepping 0; 8 threads, 4000 MHz") != NULL);
    assert(strstr(text, "Motherboard: ASUSTeK M5A97 R2.0") != NULL);
    assert(strstr(text, "BIOS: American Megatrends 2403, released 03/12/2014") != NULL);
    assert(strstr(text, "Physical RAM: 16384 MiB") != NULL);
    assert(strstr(text, "DIMMs: 2 of 4 slots populated, 16384 MiB") != NULL);
    assert(strstr(text, "Storage: Samsung SSD 850; 2 device(s)") != NULL);
    assert(strstr(text, "Serial console: COM1 at 0x3f8, 115200 8N1") != NULL);
    assert(strstr(text, "Firmware interface: Legacy BIOS/CSM (Multiboot2)") != NULL);
    assert(strstr(text, "Minimum target") == NULL);
    assert(strstr(text, "Hardware inventory to record") == NULL);
}

static void assert_firmware_matrix_content(const char *text)
{
    assert(strstr(text, "FIRMWARE AND PLATFORM STATE (live)") != NULL);
    assert(strstr(text, "FW-1 Boot mode: Legacy BIOS/CSM (Multiboot2)") != NULL);
    assert(strstr(text, "FW-2 Above 4G decoding: not active") != NULL);
    assert(strstr(text, "FW-3 Resizable BAR / SAM: not active (BAR1 256 MiB)") != NULL);
    assert(strstr(text, "FW-4 IOMMU: not active (ACPI no DMAR/IVRS table)") != NULL);
    assert(strstr(text, "FW-5 Fast Boot: not reported") != NULL);
    assert(strstr(text, "FW-6 Video handoff: CSM yes, GOP no") != NULL);
    assert(strstr(text, "FW-7 Secure Boot: not available (Legacy/CSM boot)") != NULL);
    assert(strstr(text, "FW-8 PCIe link: Gen3 x16 (max Gen3 x16), 8 GT/s cap") != NULL);
    assert(strstr(text, "VBIOS ROM BAR: fd000000 (disabled)") != NULL);
}

int main(void)
{
    uint32_t width_total = 0u;
    for (uint32_t i = 0u; i < EXPECTED_TAB_COUNT; ++i) {
        width_total += expected_tab_widths[i];
        assert(strlen(expected_tab_names[i]) * 8u + 8u
            <= expected_tab_widths[i]);
    }
    width_total += 2u * (EXPECTED_TAB_COUNT - 1u);
    assert(width_total == 672u);
    assert(width_total <= 700u);

    struct book8gui_platform platform = {
        .width = 800u,
        .height = 600u,
        .fill_rect = fill,
        .draw_text = draw_text,
        .nvdiag_snapshot = snapshot,
        .system_info = system_info,
        .platform_info = platform_info,
        .write_file = write_file
    };
    struct WindowApp app = {
        .title = book8_nvdiag_class.name,
        .bounds = {20u, 20u, 700u, 480u},
        .visible = true
    };
    app.state = book8_nvdiag_class.create(&platform);
    assert(app.state != NULL);

    render_text(&app, &platform);
    assert(strstr(rendered, "NVIDIA GeForce GTX 970") != NULL);
    assert(strstr(rendered, "MSE=1  BME=1") != NULL);
    assert(strstr(rendered, "Copy-engine acceleration active") != NULL);
    for (uint32_t i = 0u; i < EXPECTED_TAB_COUNT; ++i) {
        assert(strstr(rendered, expected_tab_names[i]) != NULL);
    }

    click_tab(&app, &platform, 2u);
    render_text(&app, &platform);
    assert(strstr(rendered, "PMC_BOOT_0") != NULL);
    assert(strstr(rendered, "124020a1") != NULL);

    click_tab(&app, &platform, 3u);
    render_text(&app, &platform);
    assert(strstr(rendered, "Memory-controller telemetry: not implemented") != NULL);

    click_tab(&app, &platform, 6u);
    render_text(&app, &platform);
    assert(strstr(rendered, "ACR firmware: no loader active") != NULL);
    assert(strstr(rendered, "VBIOS ROM BAR: fd000000 (disabled)") != NULL);
    assert(strstr(rendered, "Firmware interface: Legacy BIOS/CSM") != NULL);
    assert(strstr(rendered, "System BIOS: 2403") != NULL);

    click_tab(&app, &platform, 7u);
    render_text(&app, &platform);
    assert(strstr(rendered, "COPY ENGINE ACCELERATION") != NULL);
    assert(strstr(rendered, "State: active") != NULL);
    assert(strstr(rendered, "CE engine 1, runlist 2; 1 of 3 PTOP devices") != NULL);
    assert(strstr(rendered, "CE self-test: passed") != NULL);
    assert(strstr(rendered, "Minimum GPU rectangle: 4096 pixels") != NULL);
    assert(strstr(rendered, "GPU fills: 17") != NULL);
    assert(strstr(rendered, "GPU copies: 9") != NULL);
    assert(strstr(rendered, "Small rectangles on CPU: 1234") != NULL);
    assert(strstr(rendered, "Unsupported fallbacks: 2") != NULL);
    assert(strstr(rendered, "Fence timeouts: 1") != NULL);

    /* Right from the last tab wraps to index 0; Left wraps back to index 7. */
    send_key(&app, &platform, BOOK8GUI_KEY_RIGHT);
    render_text(&app, &platform);
    assert(strstr(rendered, "NVIDIA GeForce GTX 970") != NULL);
    send_key(&app, &platform, BOOK8GUI_KEY_LEFT);
    render_text(&app, &platform);
    assert(strstr(rendered, "COPY ENGINE ACCELERATION") != NULL);

    click_tab(&app, &platform, 4u);
    render_text(&app, &platform);
    assert(strstr(rendered, "HARDWARE INVENTORY (live)") != NULL);
    assert(strstr(rendered, "GPU: NVIDIA GeForce GTX 970") != NULL);
    assert(strstr(rendered, "VRAM from PFB_CSTATUS: 4096 MiB") != NULL);
    assert(strstr(rendered, "Motherboard: ASUSTeK M5A97 R2.0") != NULL);
    assert(strstr(rendered, "CPU: AMD FX(tm)-8350") != NULL);
    send_key(&app, &platform, BOOK8GUI_KEY_PAGE_DOWN);
    render_text(&app, &platform);
    assert(strstr(rendered, "Storage: Samsung SSD 850") != NULL
        || strstr(rendered, "Serial console: COM1") != NULL);

    click_tab(&app, &platform, 5u);
    render_text(&app, &platform);
    assert_firmware_matrix_content(compact_rendered());

    /* F6 saves the selected tab without changing it. */
    click_tab(&app, &platform, 4u);
    render_text(&app, &platform);
    saved_count = 0u;
    send_key(&app, &platform, BOOK8GUI_KEY_F6);
    assert(saved_count == 1u);
    assert(strcmp(saved_paths[0], "A:\\NVDIAG_SAVE.TXT") == 0);
    assert_hardware_content(saved_contents[0]);
    render_text(&app, &platform);
    assert(strstr(rendered, "HARDWARE") != NULL);
    assert(strstr(rendered, "Saved A:\\NVDIAG_SAVE.TXT") != NULL);

    /* Shift+F6 exports all tabs to distinct files and restores the tab. */
    send_key(&app, &platform, BOOK8GUI_KEY_F6 | BOOK8GUI_KEY_SHIFT);
    assert(saved_count == 9u);
    for (uint32_t i = 0u; i < EXPECTED_TAB_COUNT; ++i) {
        char expected_path[40];
        snprintf(expected_path, sizeof(expected_path),
            "A:\\NVDIAG_TAB%u.TXT", (unsigned)i);
        assert(strcmp(saved_paths[i + 1u], expected_path) == 0);
        assert(strstr(saved_contents[i + 1u], expected_tab_names[i]) != NULL
            || strstr(saved_contents[i + 1u], "NV Diagnostic") != NULL);
    }
    assert_hardware_content(saved_contents[5u]);
    assert_firmware_matrix_content(saved_contents[6u]);
    assert(strstr(saved_contents[1u], "GPU STATUS") != NULL);
    assert(strstr(saved_contents[2u], "FIFO CHANNEL") != NULL);
    assert(strstr(saved_contents[3u], "BAR0 REGISTER OFFSETS") != NULL);
    assert(strstr(saved_contents[4u], "VRAM APERTURE AND PARTITIONS") != NULL);
    assert(strstr(saved_contents[7u], "FIRMWARE AND ARCHITECTURE") != NULL);
    assert(strstr(saved_contents[8u], "COPY ENGINE ACCELERATION") != NULL);
    render_text(&app, &platform);
    assert(strstr(rendered, "HARDWARE") != NULL);
    assert(strstr(rendered, "Exported 8 tabs") != NULL);

    writes_succeed = false;
    send_key(&app, &platform, BOOK8GUI_KEY_F6);
    render_text(&app, &platform);
    assert(strstr(rendered, "USB save failed") != NULL);

    telemetry_available = false;
    render_text(&app, &platform);
    assert(strstr(rendered, "GPU telemetry is unavailable.") != NULL);
    puts("PASS: NV Diagnostic 8 tabs, USB export and unavailable-state rendering");
    return 0;
}
