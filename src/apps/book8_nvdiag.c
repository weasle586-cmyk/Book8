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
#define NVDIAG_LINE_LEN 96u
#define NVDIAG_MAX_LINES 20u
#define NVDIAG_TAB_COUNT 6u

enum nvdiag_tab {
    NVDIAG_GPU,
    NVDIAG_FIFO,
    NVDIAG_BAR0,
    NVDIAG_MEMORY,
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
    "GPU", "FIFO", "BAR0", "Memory", "Firmware", "Accel"
};
static const uint32_t tab_widths[NVDIAG_TAB_COUNT] = {
    70u, 70u, 70u, 82u, 86u, 72u
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

static const char *device_name(uint32_t pci_id)
{
    uint16_t device_id = (uint16_t)(pci_id >> 16u);
    return device_id == NV_DEVICE_GTX970 ? "NVIDIA GeForce GTX 970"
        : device_id == NV_DEVICE_GTX980 ? "NVIDIA GeForce GTX 980"
        : "NVIDIA display adapter";
}

static void build_lines(enum nvdiag_tab tab,
    const struct book8_nvdiag_snapshot *s, bool snapshot_ok,
    struct nvdiag_line *lines, uint32_t *count)
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
        } else {
            add_line(lines, count, false,
                "2D acceleration unavailable; software rendering active.");
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

    case NVDIAG_FIRMWARE:
        add_line(lines, count, true, "FIRMWARE AND ARCHITECTURE");
        snprintf(text, sizeof(text), "Architecture: Maxwell GM204, ID 0x%03x",
            s->chipset);
        add_line(lines, count, false, text);
        add_line(lines, count, false,
            "VBIOS image: not parsed by BOOK8.");
        add_line(lines, count, false,
            "ACR firmware: no loader active; firmware not loaded by BOOK8.");
        add_line(lines, count, false,
            (s->flags & BOOK8_NVDIAG_SCANOUT_VALID) != 0u
                ? "Display scanout was inherited from the boot firmware."
                : "Firmware display scanout was not adopted.");
        add_line(lines, count, false,
            "No firmware/register writes are performed by this utility.");
        break;

    case NVDIAG_ACCEL:
        add_line(lines, count, true, "COPY ENGINE ACCELERATION");
        if ((s->flags & BOOK8_NVDIAG_ACCEL_ACTIVE) != 0u) {
            add_line(lines, count, false, "State: active");
        } else if ((s->flags & BOOK8_NVDIAG_ACCEL_FAILED) != 0u) {
            add_line(lines, count, false,
                "State: disabled after fence timeout");
        } else {
            add_line(lines, count, false, "State: inactive (software rendering)");
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
    build_lines(((struct nvdiag_state *)app->state)->tab, &snapshot,
        snapshot_ok, lines, &count);
    struct nvdiag_state *state = app->state;
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
    build_lines(state->tab, &snapshot, snapshot_ok, lines, &count);
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
    switch (event->key & BOOK8GUI_KEY_MASK) {
    case BOOK8GUI_KEY_F5:
        snprintf(state->status, sizeof(state->status),
            "Live snapshot refreshed.");
        break;
    case BOOK8GUI_KEY_UP:
    case BOOK8GUI_KEY_DOWN:
    case BOOK8GUI_KEY_PAGE_UP:
    case BOOK8GUI_KEY_PAGE_DOWN: {
        int32_t direction = (event->key & BOOK8GUI_KEY_MASK)
            == BOOK8GUI_KEY_UP
            || (event->key & BOOK8GUI_KEY_MASK) == BOOK8GUI_KEY_PAGE_UP
            ? -1 : 1;
        uint32_t amount = (event->key & BOOK8GUI_KEY_MASK)
            == BOOK8GUI_KEY_PAGE_UP
            || (event->key & BOOK8GUI_KEY_MASK) == BOOK8GUI_KEY_PAGE_DOWN
            ? (layout.rows != 0u ? layout.rows : 1u) : 1u;
        int64_t next = (int64_t)state->first + direction * (int64_t)amount;
        state->first = next < 0 ? 0u
            : (uint32_t)next > max_first ? max_first : (uint32_t)next;
        break;
    }
    case BOOK8GUI_KEY_LEFT:
    case BOOK8GUI_KEY_RIGHT:
        state->tab = (enum nvdiag_tab)((state->tab
            + (event->key == BOOK8GUI_KEY_RIGHT ? 1u : NVDIAG_TAB_COUNT - 1u))
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
