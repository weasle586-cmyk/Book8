#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/apps/book8_nvdiag.h"
#include "../include/hwdiag.h"
#include "../drivers/nv_maxwell.h"

static char rendered[8192];
static bool telemetry_available = true;

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
    size_t used = strlen(rendered);
    if (used + strlen(text) + 2u < sizeof(rendered)) {
        snprintf(rendered + used, sizeof(rendered) - used, "%s\n", text);
    }
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
            | BOOK8_NVDIAG_CE_VERIFIED | BOOK8_NVDIAG_ACCEL_ACTIVE,
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
        .scanout_pitch = 3200u
    };
    return true;
}

static void render_text(struct WindowApp *app,
    const struct book8gui_platform *platform)
{
    rendered[0] = '\0';
    book8_nvdiag_class.render(platform, app);
}

int main(void)
{
    struct book8gui_platform platform = {
        .width = 800u,
        .height = 600u,
        .fill_rect = fill,
        .draw_text = draw_text,
        .nvdiag_snapshot = snapshot
    };
    struct WindowApp app = {
        .title = book8_nvdiag_class.name,
        .bounds = {20u, 20u, 700u, 480u},
        .visible = true
    };
    app.state = book8_nvdiag_class.create(&platform);
    assert(app.state != NULL);

    render_text(&app, &platform);
    assert(strstr(rendered, "NVIDIA GeForce GTX 970"));
    assert(strstr(rendered, "MSE=1  BME=1"));
    assert(strstr(rendered, "Copy-engine acceleration active"));

    struct book8gui_event event = {
        .type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = 170,
        .mouse_y = 55,
        .mouse_buttons = 1u
    };
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    render_text(&app, &platform);
    assert(strstr(rendered, "PMC_BOOT_0"));
    assert(strstr(rendered, "124020a1"));

    event.mouse_buttons = 0u;
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    event.mouse_x = 250;
    event.mouse_buttons = 1u;
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    render_text(&app, &platform);
    assert(strstr(rendered, "Memory-controller telemetry: not implemented"));

    event.mouse_buttons = 0u;
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    event.mouse_x = 330;
    event.mouse_buttons = 1u;
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    render_text(&app, &platform);
    assert(strstr(rendered, "ACR firmware: no loader active"));

    event.mouse_buttons = 0u;
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    event.mouse_x = 420;
    event.mouse_buttons = 1u;
    assert(book8_nvdiag_class.handle_event(&platform, &app, &event));
    render_text(&app, &platform);
    assert(strstr(rendered, "COPY ENGINE ACCELERATION"));
    assert(strstr(rendered, "State: active"));
    assert(strstr(rendered, "CE engine 1, runlist 2; 1 of 3 PTOP devices"));
    assert(strstr(rendered, "CE self-test: passed"));
    assert(strstr(rendered, "Minimum GPU rectangle: 4096 pixels"));
    assert(strstr(rendered, "GPU fills: 17"));
    assert(strstr(rendered, "GPU copies: 9"));
    assert(strstr(rendered, "Small rectangles on CPU: 1234"));
    assert(strstr(rendered, "Unsupported fallbacks: 2"));
    assert(strstr(rendered, "Fence timeouts: 1"));

    telemetry_available = false;
    render_text(&app, &platform);
    assert(strstr(rendered, "GPU telemetry is unavailable."));
    puts("PASS: NV Diagnostic telemetry tabs and unavailable-state rendering");
    return 0;
}
