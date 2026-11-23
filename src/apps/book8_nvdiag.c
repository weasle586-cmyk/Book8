#include "book8_nvdiag.h"

#include <stddef.h>
#include "../../include/hwdiag.h"
#include "../../include/stdio.h"
#include "../../drivers/nv_maxwell.h"

#include "book8_ui.h"

#define NVDIAG_TAB_H 22u
#define NVDIAG_ROW_H 18u
#define NVDIAG_STATUS_H 20u
#define NVDIAG_REFRESH_W 68u
#define NVDIAG_LINE_LEN 160u
#define NVDIAG_MAX_LINES 64u
#define NVDIAG_TAB_COUNT 8u

_Static_assert(NVDIAG_TAB_COUNT == 8u,
    "NV Diagnostic must expose exactly eight tabs");

enum nvdiag_tab {
    NVDIAG_GPU,
    NVDIAG_FIFO,
    NVDIAG_BAR0,
    NVDIAG_MEMORY,
    NVDIAG_HARDWARE,
    NVDIAG_FIRMWARE_PLATFORM,
    NVDIAG_FIRMWARE,
    NVDIAG_ACCEL
};

struct nvdiag_state {
    enum nvdiag_tab tab;
    uint32_t first;
    uint8_t previous_buttons;
    bool scroll_drag;
    int32_t scroll_grab;
    int32_t wheel_remainder;
    char status[64];
};

struct nvdiag_line {
    char text[NVDIAG_LINE_LEN];
    bool heading;
};

struct nvdiag_layout {
    struct book8gui_rect tabs[NVDIAG_TAB_COUNT];
    struct book8gui_rect frame;
    struct book8gui_rect track;
    struct book8gui_rect status;
    struct book8gui_rect refresh;
    uint32_t text_x;
    uint32_t text_y;
    uint32_t limit_x;
    uint32_t rows;
};

struct nvdiag_register {
    const char *name;
    uint32_t offset;
    uint32_t flag;
};

static const char *const tab_names[NVDIAG_TAB_COUNT] = {
    "GPU", "FIFO", "BAR0", "Memory", "Hardware",
    "Firmware Platform", "Firmware", "Accel"
};
/* 8 px glyphs, 8 px left inset and a small right gutter per tab label. */
static const uint32_t tab_widths[NVDIAG_TAB_COUNT] = {
    64u, 64u, 64u, 78u, 84u, 150u, 84u, 70u
};
static const struct nvdiag_register registers[] = {
    {"PMC_BOOT_0", 0x000000u, 0u},
    {"PMC_ENABLE", 0x000200u, BOOK8_NVDIAG_REG_PMC_ENABLE},
    {"PRAMIN_WINDOW", 0x001700u, 0u},
    {"PFIFO_INTR", 0x002100u, BOOK8_NVDIAG_REG_PFIFO_INTR},
    {"RUNLIST_PENDING", 0x002284u, BOOK8_NVDIAG_REG_RUNLIST_PENDING},
    {"ENGINE0_STATUS", 0x002640u, BOOK8_NVDIAG_REG_ENGINE_STATUS},
    {"PBDMA0_INTR", 0x040108u, BOOK8_NVDIAG_REG_PBDMA_INTR},
    {"CHANNEL0_INST", 0x800000u, BOOK8_NVDIAG_REG_CHAN_INST},
    {"CHANNEL0_CTRL", 0x800004u, BOOK8_NVDIAG_REG_CHAN_CTRL}
};

static void compute_layout(const struct WindowApp *app,
    struct nvdiag_layout *layout)
{
    struct book8gui_rect client = ui_client(app);
    uint32_t x = client.x;
    for (uint32_t i = 0u; i < NVDIAG_TAB_COUNT; ++i) {
        layout->tabs[i] = (struct book8gui_rect){
            x, client.y, tab_widths[i], NVDIAG_TAB_H
        };
        x += tab_widths[i] + 2u;
    }
    layout->status = (struct book8gui_rect){
        client.x, client.y + (client.height > NVDIAG_STATUS_H
            ? client.height - NVDIAG_STATUS_H : 0u),
        client.width, client.height < NVDIAG_STATUS_H
            ? client.height : NVDIAG_STATUS_H
    };
    uint32_t frame_y = client.y + NVDIAG_TAB_H + 2u;
    uint32_t frame_bottom = layout->status.y > 2u
        ? layout->status.y - 2u : frame_y;
    layout->frame = (struct book8gui_rect){
        client.x, frame_y, client.width,
        frame_bottom > frame_y ? frame_bottom - frame_y : 0u
    };
    layout->track = ui_scroll_track(&layout->frame);
    layout->text_x = layout->frame.x + UI_PAD + 4u;
    layout->text_y = layout->frame.y + UI_PAD + 1u;
    layout->limit_x = layout->frame.x + layout->frame.width
        - UI_SCROLL_W - 4u;
    layout->rows = layout->frame.height > 2u * UI_PAD
        ? (layout->frame.height - 2u * UI_PAD) / NVDIAG_ROW_H : 0u;
    layout->refresh = (struct book8gui_rect){
        layout->status.x + (layout->status.width > NVDIAG_REFRESH_W
            ? layout->status.width - NVDIAG_REFRESH_W : 0u),
        layout->status.y, NVDIAG_REFRESH_W, layout->status.height
    };
}

static void add_line(struct nvdiag_line *lines, uint32_t *count,
    bool heading, const char *text)
{
    if (*count >= NVDIAG_MAX_LINES) {
        return;
    }
    snprintf(lines[*count].text, sizeof(lines[*count].text), "%s", text);
    lines[*count].heading = heading;
    ++*count;
}

/* Wraps on word boundaries so long specification rows stay inside the pane. */
static void add_wrapped_line(struct nvdiag_line *lines, uint32_t *count,
    bool heading, const char *text, uint32_t wrap_cols)
{
    uint32_t limit = wrap_cols;
    if (limit == 0u || limit >= NVDIAG_LINE_LEN - 1u) {
        add_line(lines, count, heading, text);
        return;
    }
    uint32_t start = 0u;
    bool first_chunk = true;
    while (text[start] != '\0') {
        while (text[start] == ' ') {
            ++start;
        }
        if (text[start] == '\0') {
            break;
        }
        uint32_t end = start;
        uint32_t last_space = start;
        while (text[end] != '\0' && end - start < limit) {
            if (text[end] == ' ') {
                last_space = end;
            }
            ++end;
        }
        uint32_t split = text[end] == '\0' ? end
            : (last_space > start ? last_space : end);
        char chunk[NVDIAG_LINE_LEN];
        uint32_t n = 0u;
        for (uint32_t i = start; i < split && n + 1u < sizeof(chunk); ++i) {
            chunk[n++] = text[i];
        }
        chunk[n] = '\0';
        add_line(lines, count, heading && first_chunk, chunk);
        first_chunk = false;
        start = split;
    }
}

static const char *device_name(uint32_t pci_id)
{
    uint16_t device_id = (uint16_t)(pci_id >> 16u);
    return device_id == NV_DEVICE_GTX970 ? "NVIDIA GeForce GTX 970"
        : device_id == NV_DEVICE_GTX980 ? "NVIDIA GeForce GTX 980"
        : "NVIDIA display adapter";
}

static const char *platform_state_text(uint8_t state)
{
    return state == BOOK8_PLATFORM_ON ? "detected/enabled"
        : state == BOOK8_PLATFORM_OFF ? "not active" : "not reported";
}

static const char *platform_bool_text(uint8_t state)
{
    return state == BOOK8_PLATFORM_ON ? "yes"
        : state == BOOK8_PLATFORM_OFF ? "no" : "unknown";
}

static uint32_t pcie_generation(uint32_t link_status)
{
    return link_status & 0xfu;
}

static uint32_t pcie_lanes(uint32_t link_status)
{
    return (link_status >> 4u) & 0x3fu;
}

static void build_lines(const struct book8gui_platform *platform,
    enum nvdiag_tab tab, const struct book8_nvdiag_snapshot *s,
    bool snapshot_ok, struct nvdiag_line *lines, uint32_t *count,
    uint32_t wrap_cols)
{
    char text[NVDIAG_LINE_LEN];
    *count = 0u;
    if (!snapshot_ok) {
        add_line(lines, count, true, "GPU telemetry is unavailable.");
        return;
    }
    if ((s->flags & BOOK8_NVDIAG_PRESENT) == 0u) {
        add_line(lines, count, true, "No supported NVIDIA GPU detected.");
        add_line(lines, count, false,
            "The desktop remains on software rendering.");
        return;
    }

    switch (tab) {
    case NVDIAG_GPU:
        add_line(lines, count, true, "GPU STATUS");
        snprintf(text, sizeof(text), "Device: %s", device_name(s->pci_id));
        add_line(lines, count, false, text);
        add_line(lines, count, false, (s->flags & BOOK8_NVDIAG_MAPPED) != 0u
            ? "BAR0 mapping and GM204 identity verified."
            : "PCI device detected; BAR0 is not mapped.");
        if ((s->flags & BOOK8_NVDIAG_PCI_CONFIG) != 0u) {
            snprintf(text, sizeof(text), "PCI address: 0000:%02x:%02x.%u",
                (s->bdf >> 8u) & 0xffu, (s->bdf >> 3u) & 0x1fu,
                s->bdf & 7u);
            add_line(lines, count, false, text);
            snprintf(text, sizeof(text), "PCI ID: %08x",
                s->pci_id);
            add_line(lines, count, false, text);
            snprintf(text, sizeof(text), "Subsystem: %04x:%04x",
                s->subsystem_id >> 16u, s->subsystem_id & 0xffffu);
            add_line(lines, count, false, text);
            snprintf(text, sizeof(text), "PCI command: MSE=%u  BME=%u",
                (s->pci_command >> 1u) & 1u,
                (s->pci_command >> 2u) & 1u);
            add_line(lines, count, false, text);
            snprintf(text, sizeof(text), "PCI status: %04x",
                s->pci_status);
            add_line(lines, count, false, text);
        }
        snprintf(text, sizeof(text), "Architecture: Maxwell GM204 (0x%03x)",
            s->chipset);
        add_line(lines, count, false, text);
        if ((s->flags & BOOK8_NVDIAG_ACCEL_ACTIVE) != 0u) {
            add_line(lines, count, false,
                "Copy-engine acceleration active; NV902D is not initialized.");
        } else if ((s->flags & BOOK8_NVDIAG_ACCEL_FAILED) != 0u) {
            add_line(lines, count, false,
                "Copy engine disabled after a fence timeout; using software.");
        } else if ((s->flags & BOOK8_NVDIAG_CHANNEL_BOUND) != 0u) {
            add_line(lines, count, false,
                "PFIFO channel active; copy engine not verified.");
        } else if ((s->flags & BOOK8_NVDIAG_MAPPED) == 0u) {
            add_line(lines, count, false,
                "2D acceleration unavailable; GPU registers not mapped.");
        } else if ((s->flags & BOOK8_NVDIAG_CE_FOUND) != 0u) {
            add_line(lines, count, false,
                "Copy engine found but not verified; software rendering active.");
        } else {
            add_line(lines, count, false,
                "2D acceleration unavailable; no channel bound.");
        }
        if ((s->flags & BOOK8_NVDIAG_SCANOUT_VALID) != 0u) {
            snprintf(text, sizeof(text), "Scanout: %ux%u, pitch %u bytes",
                s->scanout_width, s->scanout_height, s->scanout_pitch);
            add_line(lines, count, false, text);
        }
        break;

    case NVDIAG_FIFO:
        add_line(lines, count, true, "FIFO CHANNEL");
        add_line(lines, count, false,
            (s->flags & BOOK8_NVDIAG_CHANNEL_BOUND) != 0u
                ? "Channel: bound" : "Channel: not bound");
        add_line(lines, count, false,
            (s->flags & BOOK8_NVDIAG_RUNLIST_COMMITTED) != 0u
                ? "Runlist: committed" : "Runlist: not committed");
        add_line(lines, count, false,
            (s->flags & BOOK8_NVDIAG_RING_ACKED) != 0u
                ? "Pushbuffer ring: acknowledged" : "Pushbuffer ring: not acknowledged");
        if (s->gpfifo_entries != 0u) {
            snprintf(text, sizeof(text),
                "GPFIFO GET/PUT: %u / %u of %u entries",
                s->gp_get, s->gp_put, s->gpfifo_entries);
        } else {
            snprintf(text, sizeof(text), "GPFIFO: not initialized");
        }
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "Pushbuffer words: %u / %u",
            s->pushbuf_used, s->pushbuf_words);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "Pushbuffer overflows: %u",
            s->pushbuf_overflows);
        add_line(lines, count, false, text);
        if ((s->register_flags & BOOK8_NVDIAG_REG_CHAN_CTRL) != 0u) {
            snprintf(text, sizeof(text), "Channel control: %08x",
                s->channel_ctrl);
            add_line(lines, count, false, text);
        }
        if ((s->register_flags & BOOK8_NVDIAG_REG_RUNLIST_PENDING) != 0u) {
            snprintf(text, sizeof(text), "Runlist pending: %08x",
                s->runlist_pending);
            add_line(lines, count, false, text);
        }
        if ((s->register_flags & BOOK8_NVDIAG_REG_PFIFO_INTR) != 0u) {
            snprintf(text, sizeof(text), "PFIFO interrupt: %08x",
                s->pfifo_intr);
            add_line(lines, count, false, text);
        }
        if ((s->register_flags & BOOK8_NVDIAG_REG_PBDMA_INTR) != 0u) {
            snprintf(text, sizeof(text), "PBDMA0 interrupt: %08x",
                s->pbdma_intr);
            add_line(lines, count, false, text);
        }
        if ((s->register_flags & BOOK8_NVDIAG_REG_ENGINE_STATUS) != 0u) {
            snprintf(text, sizeof(text), "Engine 0 status: %08x",
                s->engine_status);
            add_line(lines, count, false, text);
        }
        break;

    case NVDIAG_BAR0:
        add_line(lines, count, true, "BAR0 REGISTER OFFSETS");
        for (uint32_t i = 0u; i < sizeof(registers) / sizeof(registers[0]);
            ++i) {
            const struct nvdiag_register *reg = &registers[i];
            uint32_t value = 0u;
            bool valid = false;
            if (reg->flag == 0u) {
                if (reg->offset == 0u
                    && (s->flags & BOOK8_NVDIAG_MAPPED) != 0u) {
                    value = s->boot0;
                    valid = true;
                }
            } else if ((s->register_flags & reg->flag) != 0u) {
                valid = true;
                switch (reg->flag) {
                case BOOK8_NVDIAG_REG_PMC_ENABLE: value = s->pmc_enable; break;
                case BOOK8_NVDIAG_REG_PFIFO_INTR: value = s->pfifo_intr; break;
                case BOOK8_NVDIAG_REG_RUNLIST_PENDING: value = s->runlist_pending; break;
                case BOOK8_NVDIAG_REG_ENGINE_STATUS: value = s->engine_status; break;
                case BOOK8_NVDIAG_REG_PBDMA_INTR: value = s->pbdma_intr; break;
                case BOOK8_NVDIAG_REG_CHAN_INST: value = s->channel_inst; break;
                case BOOK8_NVDIAG_REG_CHAN_CTRL: value = s->channel_ctrl; break;
                default: valid = false; break;
                }
            }
            if (valid) {
                snprintf(text, sizeof(text), "%s  %06x  %08x",
                    reg->name, reg->offset, value);
            } else {
                snprintf(text, sizeof(text), "%s  %06x  not sampled",
                    reg->name, reg->offset);
            }
            add_line(lines, count, false, text);
        }
        add_line(lines, count, false,
            "Only explicitly allowlisted, read-only offsets are sampled.");
        break;

    case NVDIAG_MEMORY:
        add_line(lines, count, true, "VRAM APERTURE AND PARTITIONS");
        snprintf(text, sizeof(text), "BAR1 aperture: %016llx, %llu MiB",
            (unsigned long long)s->bar1_phys,
            (unsigned long long)(s->bar1_size / (1024u * 1024u)));
        add_line(lines, count, false, text);
        if ((s->flags & BOOK8_NVDIAG_SCANOUT_VALID) != 0u) {
            uint64_t bytes = (uint64_t)s->scanout_pitch
                * s->scanout_height;
            snprintf(text, sizeof(text),
                "Scanout VRAM offset: %016llx (%llu bytes)",
                (unsigned long long)s->scanout_vram_offset,
                (unsigned long long)bytes);
            add_line(lines, count, false, text);
            snprintf(text, sizeof(text), "Scanout geometry: %ux%u, pitch %u",
                s->scanout_width, s->scanout_height, s->scanout_pitch);
            add_line(lines, count, false, text);
        } else {
            add_line(lines, count, false, "Scanout VRAM partition: unavailable.");
        }
        if ((s->flags & BOOK8_NVDIAG_CHANNEL_BOUND) != 0u) {
            snprintf(text, sizeof(text), "Channel VRAM base: %016llx",
                (unsigned long long)s->vram_base);
            add_line(lines, count, false, text);
        } else {
            add_line(lines, count, false,
                "Channel VRAM partition: not allocated.");
        }
        add_line(lines, count, false,
            "BAR1 size is an aperture, not total installed VRAM.");
        add_line(lines, count, false,
            "Memory-controller telemetry: not implemented by this driver.");
        break;

    case NVDIAG_HARDWARE: {
        struct book8_platform_info pi = {0};
        struct book8gui_sysinfo si = {0};
        bool have_pi = platform->platform_info != NULL
            && platform->platform_info(&pi);
        bool have_si = platform->system_info != NULL
            && platform->system_info(&si);

        add_wrapped_line(lines, count, true, "HARDWARE INVENTORY (live)",
            wrap_cols);
        snprintf(text, sizeof(text), "GPU: %s", device_name(s->pci_id));
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if ((s->flags & BOOK8_NVDIAG_PCI_CONFIG) != 0u) {
            snprintf(text, sizeof(text),
                "PCI: %02x:%02x.%u  ID %08x  subsystem %04x:%04x",
                (s->bdf >> 8u) & 0xffu, (s->bdf >> 3u) & 0x1fu,
                s->bdf & 7u, s->pci_id,
                s->subsystem_id >> 16u, s->subsystem_id & 0xffffu);
            add_wrapped_line(lines, count, false, text, wrap_cols);
        }
        snprintf(text, sizeof(text), "BAR0: 0x%llx + %llu MiB",
            (unsigned long long)s->bar0_phys,
            (unsigned long long)(s->bar0_size / (1024u * 1024u)));
        add_wrapped_line(lines, count, false, text, wrap_cols);
        snprintf(text, sizeof(text), "BAR1: 0x%llx + %llu MiB",
            (unsigned long long)s->bar1_phys,
            (unsigned long long)(s->bar1_size / (1024u * 1024u)));
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if (s->detected_vram_mb != 0u) {
            snprintf(text, sizeof(text),
                "VRAM from PFB_CSTATUS: %u MiB", s->detected_vram_mb);
            add_wrapped_line(lines, count, false, text, wrap_cols);
        } else {
            add_wrapped_line(lines, count, false,
                "VRAM: PFB_CSTATUS not sampled", wrap_cols);
        }
        snprintf(text, sizeof(text),
            "Chipset 0x%03x, revision 0x%u, foundry 0x%u", s->chipset,
            (unsigned)s->chip_revision, (unsigned)s->foundry_id);
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if ((s->flags & BOOK8_NVDIAG_SCANOUT_VALID) != 0u) {
            snprintf(text, sizeof(text),
                "Display: %ux%u, pitch %u (boot scanout; EDID not read)",
                s->scanout_width, s->scanout_height, s->scanout_pitch);
        } else {
            snprintf(text, sizeof(text), "%s",
                "Display: no adopted scanout; EDID not read");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if ((s->flags & BOOK8_NVDIAG_PCI_EXPRESS) != 0u) {
            snprintf(text, sizeof(text),
                "GPU PCIe link: Gen%u x%u (max Gen%u x%u)",
                (unsigned)pcie_generation(s->pcie_link_status),
                (unsigned)pcie_lanes(s->pcie_link_status),
                (unsigned)pcie_generation(s->pcie_link_cap),
                (unsigned)pcie_lanes(s->pcie_link_cap));
            add_wrapped_line(lines, count, false, text, wrap_cols);
        }

        if (have_si && si.cpu_name[0u] != '\0') {
            snprintf(text, sizeof(text), "CPU: %.48s", si.cpu_name);
            add_wrapped_line(lines, count, false, text, wrap_cols);
            snprintf(text, sizeof(text),
                "CPU ID: family %02xh model %02xh stepping %u; %u threads, %u MHz",
                (unsigned)pi.cpu_family, (unsigned)pi.cpu_model_id,
                (unsigned)pi.cpu_stepping, si.cpu_threads, si.cpu_mhz);
        } else {
            snprintf(text, sizeof(text), "%s",
                "CPU: brand string unavailable (CPUID not supported)");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if (have_pi && (pi.flags & BOOK8_PLATFORM_SMBIOS_VALID) != 0u) {
            snprintf(text, sizeof(text), "Motherboard: %s %s",
                pi.board_vendor[0u] != '\0' ? pi.board_vendor : "unknown",
                pi.board_product[0u] != '\0' ? pi.board_product : "unknown");
            add_wrapped_line(lines, count, false, text, wrap_cols);
            snprintf(text, sizeof(text), "BIOS: %s %s, released %s",
                pi.bios_vendor[0u] != '\0' ? pi.bios_vendor : "unknown",
                pi.bios_version[0u] != '\0' ? pi.bios_version : "unknown",
                pi.bios_date[0u] != '\0' ? pi.bios_date : "unknown");
        } else {
            snprintf(text, sizeof(text), "%s",
                "Motherboard/BIOS: no SMBIOS table reported");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if (have_si) {
            snprintf(text, sizeof(text), "Physical RAM: %llu MiB",
                (unsigned long long)(si.memory_total_kb / 1024u));
            add_wrapped_line(lines, count, false, text, wrap_cols);
        }
        if (have_pi && (pi.flags & BOOK8_PLATFORM_DIMMS_VALID) != 0u) {
            snprintf(text, sizeof(text),
                "DIMMs: %u of %u slots populated, %u MiB; %s",
                (unsigned)pi.dimm_count, (unsigned)pi.dimm_slots,
                (unsigned)pi.dimm_installed_mb, pi.memory_layout);
        } else {
            snprintf(text, sizeof(text), "%s",
                "DIMM layout: not reported by SMBIOS");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);
        if (have_si && si.disk_count != 0u) {
            snprintf(text, sizeof(text),
                "Storage: %.40s; %u device(s), %llu MiB",
                si.disk_label[0u] != '\0' ? si.disk_label : "unknown model",
                si.disk_count,
                (unsigned long long)(si.disk_total_kb / 1024u));
        } else {
            snprintf(text, sizeof(text), "%s",
                "Storage: no disk detected");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);
        add_wrapped_line(lines, count, false,
            "Serial console: COM1 at 0x3f8, 115200 8N1 (kernel setting)",
            wrap_cols);
        if (have_pi) {
            snprintf(text, sizeof(text), "Firmware interface: %s",
                pi.boot_mode);
            add_wrapped_line(lines, count, false, text, wrap_cols);
        }
        break;
    }

    case NVDIAG_FIRMWARE_PLATFORM: {
        struct book8_platform_info pi = {0};
        bool have_pi = platform->platform_info != NULL
            && platform->platform_info(&pi);
        const char *boot_mode = have_pi && pi.boot_mode[0u] != '\0'
            ? pi.boot_mode : "not detected";

        add_wrapped_line(lines, count, true,
            "FIRMWARE AND PLATFORM STATE (live)", wrap_cols);
        snprintf(text, sizeof(text), "FW-1 Boot mode: %s", boot_mode);
        add_wrapped_line(lines, count, false, text, wrap_cols);

        snprintf(text, sizeof(text), "FW-2 Above 4G decoding: %s%s",
            platform_state_text(pi.above_4g),
            pi.above_4g == BOOK8_PLATFORM_ON
                ? " (GPU BAR placed above 4 GiB)"
                : pi.above_4g == BOOK8_PLATFORM_OFF
                    ? " (GPU BARs below 4 GiB)" : "");
        add_wrapped_line(lines, count, false, text, wrap_cols);

        snprintf(text, sizeof(text),
            "FW-3 Resizable BAR / SAM: %s (BAR1 %llu MiB)",
            platform_state_text(pi.resizable_bar),
            (unsigned long long)(s->bar1_size / (1024u * 1024u)));
        add_wrapped_line(lines, count, false, text, wrap_cols);

        if ((pi.flags & BOOK8_PLATFORM_ACPI_VALID) != 0u) {
            snprintf(text, sizeof(text),
                "FW-4 IOMMU: %s (ACPI %s)",
                platform_state_text(pi.iommu),
                pi.iommu == BOOK8_PLATFORM_ON
                    ? "DMAR/IVRS present" : "no DMAR/IVRS table");
        } else {
            snprintf(text, sizeof(text), "%s",
                "FW-4 IOMMU: not reported (ACPI tables unavailable)");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);

        snprintf(text, sizeof(text),
            "FW-5 Fast Boot: %s (firmware setting not exposed to OS)",
            platform_state_text(pi.fast_boot));
        add_wrapped_line(lines, count, false, text, wrap_cols);

        snprintf(text, sizeof(text),
            "FW-6 Video handoff: CSM %s, GOP %s",
            platform_bool_text(pi.csm_video),
            platform_bool_text(pi.gop_video));
        add_wrapped_line(lines, count, false, text, wrap_cols);

        if (pi.secure_boot == BOOK8_PLATFORM_OFF) {
            snprintf(text, sizeof(text), "%s",
                "FW-7 Secure Boot: not available (Legacy/CSM boot)");
        } else if (pi.secure_boot == BOOK8_PLATFORM_ON) {
            snprintf(text, sizeof(text), "%s",
                "FW-7 Secure Boot: enabled (UEFI variable)");
        } else {
            snprintf(text, sizeof(text), "%s",
                "FW-7 Secure Boot: not reported (UEFI variables not read)");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);

        if ((s->flags & BOOK8_NVDIAG_PCI_EXPRESS) != 0u) {
            snprintf(text, sizeof(text),
                "FW-8 PCIe link: Gen%u x%u (max Gen%u x%u), %u GT/s cap",
                (unsigned)pcie_generation(s->pcie_link_status),
                (unsigned)pcie_lanes(s->pcie_link_status),
                (unsigned)pcie_generation(s->pcie_link_cap),
                (unsigned)pcie_lanes(s->pcie_link_cap),
                (unsigned)(pcie_generation(s->pcie_link_cap) == 1u ? 2u
                    : pcie_generation(s->pcie_link_cap) == 2u ? 5u
                    : pcie_generation(s->pcie_link_cap) == 3u ? 8u : 16u));
        } else {
            snprintf(text, sizeof(text), "%s",
                "FW-8 PCIe link: not sampled (no PCIe capability)");
        }
        add_wrapped_line(lines, count, false, text, wrap_cols);

        snprintf(text, sizeof(text),
            "VBIOS ROM BAR: %08x (%s); image not parsed",
            s->rom_bar,
            (s->rom_bar & 1u) != 0u ? "enabled" : "disabled");
        add_wrapped_line(lines, count, false, text, wrap_cols);
        break;
    }

    case NVDIAG_FIRMWARE: {
        struct book8_platform_info pi = {0};
        bool have_pi = platform->platform_info != NULL
            && platform->platform_info(&pi);
        add_line(lines, count, true, "FIRMWARE AND ARCHITECTURE");
        snprintf(text, sizeof(text), "Architecture: Maxwell GM204, ID 0x%03x",
            s->chipset);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text),
            "VBIOS ROM BAR: %08x (%s); image not parsed by BOOK8",
            s->rom_bar, (s->rom_bar & 1u) != 0u ? "enabled" : "disabled");
        add_line(lines, count, false, text);
        add_line(lines, count, false,
            "ACR firmware: no loader active; firmware not loaded by BOOK8.");
        add_line(lines, count, false,
            (s->flags & BOOK8_NVDIAG_SCANOUT_VALID) != 0u
                ? "Display scanout was inherited from the boot firmware."
                : "Firmware display scanout was not adopted.");
        if (have_pi && pi.boot_mode[0u] != '\0') {
            snprintf(text, sizeof(text), "Firmware interface: %s",
                pi.boot_mode);
            add_line(lines, count, false, text);
        }
        if (have_pi && (pi.flags & BOOK8_PLATFORM_SMBIOS_VALID) != 0u
            && pi.bios_version[0u] != '\0') {
            snprintf(text, sizeof(text), "System BIOS: %.40s",
                pi.bios_version);
            add_line(lines, count, false, text);
        }
        add_line(lines, count, false,
            "No firmware/register writes are performed by this utility.");
        break;
    }

    case NVDIAG_ACCEL:
        add_line(lines, count, true, "COPY ENGINE ACCELERATION");
        if ((s->flags & BOOK8_NVDIAG_ACCEL_ACTIVE) != 0u) {
            add_line(lines, count, false, "State: active");
        } else if ((s->flags & BOOK8_NVDIAG_ACCEL_FAILED) != 0u) {
            add_line(lines, count, false,
                "State: disabled after fence timeout");
        } else if ((s->flags & BOOK8_NVDIAG_MAPPED) == 0u) {
            add_line(lines, count, false,
                "State: not probed (GPU registers not mapped)");
        } else if ((s->flags & BOOK8_NVDIAG_CE_FOUND) != 0u) {
            add_line(lines, count, false,
                "State: copy engine found but not active (software rendering)");
        } else if ((s->flags & BOOK8_NVDIAG_CHANNEL_BOUND) == 0u) {
            add_line(lines, count, false,
                "State: channel not bound (software rendering)");
        } else {
            add_line(lines, count, false,
                "State: not active (software rendering)");
        }
        if ((s->flags & BOOK8_NVDIAG_CE_FOUND) != 0u) {
            snprintf(text, sizeof(text),
                "CE engine %u, runlist %u; %u of %u PTOP devices",
                s->ce_engine, s->ce_runlist, s->ce_count, s->ptop_devices);
        } else {
            snprintf(text, sizeof(text), "Copy engine: not discovered");
        }
        add_line(lines, count, false, text);
        add_line(lines, count, false,
            (s->flags & BOOK8_NVDIAG_CE_VERIFIED) != 0u
                ? "CE self-test: passed" : "CE self-test: not passed");
        snprintf(text, sizeof(text), "Minimum GPU rectangle: %u pixels",
            s->accel_min_pixels);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "GPU fills: %llu",
            (unsigned long long)s->accel_fills);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "GPU copies: %llu",
            (unsigned long long)s->accel_copies);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "Small rectangles on CPU: %llu",
            (unsigned long long)s->accel_small_skips);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "Unsupported fallbacks: %llu",
            (unsigned long long)s->accel_unsupported);
        add_line(lines, count, false, text);
        snprintf(text, sizeof(text), "Fence timeouts: %llu",
            (unsigned long long)s->accel_fence_timeouts);
        add_line(lines, count, false, text);
        break;
    }
}

static void draw_clipped(const struct book8gui_platform *platform,
    uint32_t x, uint32_t y, const char *text, uint32_t limit_x,
    uint32_t foreground, uint32_t background)
{
    if (x >= limit_x) {
        return;
    }
    uint32_t fit = (limit_x - x) / 8u;
    char clipped[NVDIAG_LINE_LEN];
    uint32_t n = 0u;
    while (text[n] != '\0' && n < fit && n + 1u < sizeof(clipped)) {
        clipped[n] = text[n];
        ++n;
    }
    clipped[n] = '\0';
    platform->draw_text(x, y, clipped, foreground, background);
}

static void draw_tab(const struct book8gui_platform *platform,
    const struct book8gui_rect *rect, const char *text, bool selected)
{
    uint32_t face = selected ? UI_SELECTED : UI_FACE;
    uint32_t foreground = selected ? UI_SELECTED_TEXT : UI_TEXT;
    platform->fill_rect(rect->x, rect->y, rect->width, rect->height,
        UI_SHADOW);
    platform->fill_rect(rect->x + 1u, rect->y + 1u, rect->width - 2u,
        rect->height - 2u, face);
    platform->draw_text(rect->x + 8u, rect->y + 3u, text, foreground, face);
}

/* Writes one complete tab (without viewport wrapping) through the GUI hook. */
static bool write_tab_to_file(const struct book8gui_platform *platform,
    enum nvdiag_tab tab, const char *path, uint32_t wrap_cols)
{
    if (platform->write_file == NULL) {
        return false;
    }
    struct book8_nvdiag_snapshot snapshot = {0};
    bool snapshot_ok = platform->nvdiag_snapshot != NULL
        && platform->nvdiag_snapshot(&snapshot);
    struct nvdiag_line lines[NVDIAG_MAX_LINES];
    uint32_t count = 0u;
    build_lines(platform, tab, &snapshot, snapshot_ok, lines, &count,
        wrap_cols);

    char content[NVDIAG_MAX_LINES * (NVDIAG_LINE_LEN + 2u) + 128u];
    uint32_t used = 0u;
    int written = snprintf(content, sizeof(content),
        "NV Diagnostic - %s\r\n", tab_names[tab]);
    if (written < 0 || (uint32_t)written >= sizeof(content)) {
        return false;
    }
    used = (uint32_t)written;
    for (uint32_t i = 0u; i < count; ++i) {
        written = snprintf(content + used, sizeof(content) - used,
            "%s\r\n", lines[i].text);
        if (written < 0 || (uint32_t)written >= sizeof(content) - used) {
            return false;
        }
        used += (uint32_t)written;
    }
    return platform->write_file(path, content, used);
}

static void save_current_tab(const struct book8gui_platform *platform,
    struct nvdiag_state *state)
{
    bool ok = write_tab_to_file(platform, state->tab,
        "A:\\NVDIAG_SAVE.TXT", NVDIAG_LINE_LEN - 1u);
    snprintf(state->status, sizeof(state->status),
        ok ? "Saved A:\\NVDIAG_SAVE.TXT" : "USB save failed");
}

/* Shift+F6 never reuses the F6 filename, so earlier saves are not overwritten. */
static void save_all_tabs(const struct book8gui_platform *platform,
    struct nvdiag_state *state)
{
    for (uint32_t i = 0u; i < NVDIAG_TAB_COUNT; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "A:\\NVDIAG_TAB%u.TXT", (unsigned)i);
        if (!write_tab_to_file(platform, (enum nvdiag_tab)i, path,
                NVDIAG_LINE_LEN - 1u)) {
            snprintf(state->status, sizeof(state->status),
                "USB export failed at tab %u", (unsigned)i);
            return;
        }
    }
    snprintf(state->status, sizeof(state->status),
        "Exported 8 tabs to A:\\NVDIAG_TAB*.TXT");
}

static void render(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    struct nvdiag_layout layout;
    struct nvdiag_line lines[NVDIAG_MAX_LINES];
    struct book8_nvdiag_snapshot snapshot = {0};
    uint32_t count = 0u;
    bool snapshot_ok = platform->nvdiag_snapshot != NULL
        && platform->nvdiag_snapshot(&snapshot);
    compute_layout(app, &layout);
    struct nvdiag_state *state = app->state;
    uint32_t wrap_cols = layout.limit_x > layout.text_x
        ? (layout.limit_x - layout.text_x) / 8u : 1u;
    build_lines(platform, state->tab, &snapshot, snapshot_ok, lines, &count,
        wrap_cols);
    uint32_t max_first = count > layout.rows ? count - layout.rows : 0u;
    if (state->first > max_first) {
        state->first = max_first;
    }

    for (uint32_t i = 0u; i < NVDIAG_TAB_COUNT; ++i) {
        draw_tab(platform, &layout.tabs[i], tab_names[i], state->tab == i);
    }
    ui_sunken(platform, &layout.frame, UI_FIELD);
    for (uint32_t row = 0u; row < layout.rows; ++row) {
        uint32_t index = state->first + row;
        if (index >= count) {
            break;
        }
        draw_clipped(platform, layout.text_x,
            layout.text_y + row * NVDIAG_ROW_H, lines[index].text,
            layout.limit_x, lines[index].heading ? UI_SELECTED : UI_TEXT,
            UI_FIELD);
    }
    ui_scrollbar(platform, &layout.track, count, layout.rows, state->first);
    ui_sunken(platform, &layout.status, UI_FACE);
    draw_clipped(platform, layout.status.x + 5u,
        layout.status.y + 2u, state->status,
        layout.refresh.x - 4u, UI_TEXT, UI_FACE);
    ui_sunken(platform, &layout.refresh, UI_FACE);
    platform->draw_text(layout.refresh.x + 5u, layout.refresh.y + 2u,
        "Refresh", UI_TEXT, UI_FACE);
}

static bool handle_event(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event)
{
    struct nvdiag_state *state = app->state;
    struct nvdiag_layout layout;
    struct nvdiag_line lines[NVDIAG_MAX_LINES];
    struct book8_nvdiag_snapshot snapshot = {0};
    uint32_t count = 0u;
    bool snapshot_ok = platform->nvdiag_snapshot != NULL
        && platform->nvdiag_snapshot(&snapshot);
    compute_layout(app, &layout);
    uint32_t wrap_cols = layout.limit_x > layout.text_x
        ? (layout.limit_x - layout.text_x) / 8u : 1u;
    build_lines(platform, state->tab, &snapshot, snapshot_ok, lines, &count,
        wrap_cols);
    uint32_t max_first = count > layout.rows ? count - layout.rows : 0u;

    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        bool down = (event->mouse_buttons & 1u) != 0u;
        bool pressed = down && (state->previous_buttons & 1u) == 0u;
        state->previous_buttons = event->mouse_buttons;
        if (!down) {
            state->scroll_drag = false;
        }
        if (event->wheel_delta != 0
            && event->mouse_x >= (int32_t)app->bounds.x
            && event->mouse_y >= (int32_t)app->bounds.y
            && (uint32_t)event->mouse_x < app->bounds.x + app->bounds.width
            && (uint32_t)event->mouse_y < app->bounds.y + app->bounds.height) {
            state->first = ui_wheel_scroll_delta(state->first, count,
                layout.rows, event->wheel_delta, 1u,
                &state->wheel_remainder);
            book8gui_invalidate();
        }
        if (pressed) {
            for (uint32_t i = 0u; i < NVDIAG_TAB_COUNT; ++i) {
                if (ui_contains(&layout.tabs[i], event->mouse_x,
                    event->mouse_y)) {
                    state->tab = (enum nvdiag_tab)i;
                    state->first = 0u;
                    book8gui_invalidate();
                    return true;
                }
            }
            if (ui_contains(&layout.refresh, event->mouse_x,
                event->mouse_y)) {
                snprintf(state->status, sizeof(state->status),
                    "Live snapshot refreshed.");
                book8gui_invalidate();
                return true;
            }
            if (ui_contains(&layout.track, event->mouse_x,
                event->mouse_y)) {
                state->scroll_drag = count > layout.rows;
                state->scroll_grab = ui_scrollbar_grab(&layout.track, count,
                    layout.rows, state->first, event->mouse_y);
            } else if (ui_contains(&layout.frame, event->mouse_x,
                event->mouse_y) && event->mouse_y >= (int32_t)layout.text_y) {
                uint32_t row = state->first
                    + ((uint32_t)event->mouse_y - layout.text_y)
                        / NVDIAG_ROW_H;
                if (row < count) {
                    book8gui_invalidate();
                }
            }
        }
        if (down && state->scroll_drag) {
            state->first = ui_scrollbar_drag_position(&layout.track, count,
                layout.rows, event->mouse_y, state->scroll_grab);
            book8gui_invalidate();
        }
        return app->visible;
    }
    uint32_t key = event->key & BOOK8GUI_KEY_MASK;
    bool shift_pressed = (event->key & BOOK8GUI_KEY_SHIFT) != 0u;
    switch (key) {
    case BOOK8GUI_KEY_F5:
        snprintf(state->status, sizeof(state->status),
            "Live snapshot refreshed.");
        break;
    case BOOK8GUI_KEY_F6:
        if (shift_pressed) {
            save_all_tabs(platform, state);
        } else {
            save_current_tab(platform, state);
        }
        break;
    case BOOK8GUI_KEY_UP:
    case BOOK8GUI_KEY_DOWN:
    case BOOK8GUI_KEY_PAGE_UP:
    case BOOK8GUI_KEY_PAGE_DOWN: {
        int32_t direction = key == BOOK8GUI_KEY_UP
            || key == BOOK8GUI_KEY_PAGE_UP ? -1 : 1;
        uint32_t amount = key == BOOK8GUI_KEY_PAGE_UP
            || key == BOOK8GUI_KEY_PAGE_DOWN
            ? (layout.rows != 0u ? layout.rows : 1u) : 1u;
        int64_t next = (int64_t)state->first + direction * (int64_t)amount;
        state->first = next < 0 ? 0u
            : (uint32_t)next > max_first ? max_first : (uint32_t)next;
        break;
    }
    case BOOK8GUI_KEY_LEFT:
    case BOOK8GUI_KEY_RIGHT:
        state->tab = (enum nvdiag_tab)((state->tab
            + (key == BOOK8GUI_KEY_RIGHT ? 1u : NVDIAG_TAB_COUNT - 1u))
            % NVDIAG_TAB_COUNT);
        state->first = 0u;
        break;
    default:
        return true;
    }
    book8gui_invalidate();
    return true;
}

static void *create(const struct book8gui_platform *platform)
{
    (void)platform;
    static struct nvdiag_state state;
    state = (struct nvdiag_state){
        .tab = NVDIAG_GPU,
        .status = "Live telemetry; refreshes once per second."
    };
    return &state;
}

const struct book8gui_app_class book8_nvdiag_class = {
    .name = "NV Diagnostic",
    .default_width = 700u,
    .default_height = 480u,
    .single_instance = true,
    .state_size = sizeof(struct nvdiag_state),
    .refresh_1hz = true,
    .create = create,
    .destroy = NULL,
    .render = render,
    .handle_event = handle_event
};
