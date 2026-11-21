#include "book8_settings.h"

#include <stddef.h>
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "../../net/network_settings.h"
#include "../registry.h"
#include "../session.h"

#include "book8_ui.h"

#define ROW_H 44u
#define READOUT_ROWS 64u
#define NETWORK_FIELDS 8u
#define NETWORK_RETRY_MIN 1000u
#define NETWORK_RETRY_MAX 16000u
#define NETWORK_RETRY_STEP 1000u
#define SETTINGS_CATEGORY_COUNT 8u
#define SETTINGS_NAV_ROW_H 54u

struct settings_state {
    enum book8_control_page page;
    uint32_t category;
    struct book8_system_config draft;
    struct book8_network_settings network_draft;
    struct book8_network_status network_status;
    struct book8gui_time time;
    uint32_t field, first, nav_first;
    int32_t dragging, scroll_grab;
    int32_t wheel_remainder[3];
    uint8_t previous_buttons;
    bool time_valid, time_dirty, network_status_valid;
    bool clock24h, clock24h_draft, ui_beep, ui_beep_draft;
    uint8_t network_octet;
    uint32_t program_selected, program_first;
    bool remove_confirm, program_drag;
    struct book8gui_program remove_target;
    struct book8_session_info profile_session;
    char profile_username[BOOK8_SESSION_NAME_MAX];
    char profile_display_name[BOOK8_SESSION_NAME_MAX];
    char profile_current_password[BOOK8_SESSION_PASSWORD_MAX + 1u];
    char profile_new_password[BOOK8_SESSION_PASSWORD_MAX + 1u];
    char profile_confirm_password[BOOK8_SESSION_PASSWORD_MAX + 1u];
    uint8_t profile_focus;
    char message[96], network_state[48];
};

static struct settings_state settings;

struct settings_layout {
    struct book8gui_rect header, sidebar, content, track, status, apply, reset, renew;
    uint32_t rows, total, fields, nav_rows;
};

static const char *const pages[SETTINGS_CATEGORY_COUNT] = {
    "Network & Internet", "Input & Devices", "Apps & Default Apps",
    "Display", "Sounds", "Storage", "System", "User Profile"
};
static const char *const subtitles[SETTINGS_CATEGORY_COUNT][2] = {
    {"rtl8168 / DHCP", "IP lease"},
    {"Keyboard, Mouse", "PCIe devices"},
    {"Installed apps", "File associations"},
    {"Theme, Overscan", "Display mode"},
    {"Volume", "UI audio"},
    {"A: FAT32", "4 GiB swap"},
    {"Date / Time", "Kernel / Admin"},
    {"Login & password", "UID 0 - Root"}
};
static const uint32_t category_colors[SETTINGS_CATEGORY_COUNT] = {
    0x1683e8u, 0x35a66fu, 0xe87525u, 0xe2a51bu,
    0x159b9au, 0x8051b8u, 0x64748bu, 0x3b82f6u
};
static const enum book8_control_page category_pages[SETTINGS_CATEGORY_COUNT] = {
    BOOK8_PANEL_NETWORK, BOOK8_PANEL_KEYBOARD, BOOK8_PANEL_PROGRAMS,
    BOOK8_PANEL_DISPLAY, BOOK8_PANEL_SOUND, BOOK8_PANEL_STORAGE,
    BOOK8_PANEL_TIME, BOOK8_PANEL_ADMIN
};
static const char *const labels[BOOK8_CFG_COUNT] = {
    "Timezone (fixed UTC offset)", "Sync manual changes to RTC",
    "Key repeat rate (keys/sec)", "Initial key delay",
    "Pointer speed (%)", "Double-click interval (ms)", "Swap left/right buttons",
    "Logical desktop resolution", "Presentation preference", "Accent color",
    "Dark desktop theme", "Adapter connection preference",
    "System volume (%)", "Master mute", "Alert frequency (Hz)"
};
static const uint32_t first_key[BOOK8_PANEL_COUNT] = {
    BOOK8_CFG_TIMEZONE, BOOK8_CFG_REPEAT_RATE, BOOK8_CFG_POINTER_SPEED,
    BOOK8_CFG_RESOLUTION, BOOK8_CFG_NETWORK, BOOK8_CFG_COUNT,
    BOOK8_CFG_VOLUME, BOOK8_CFG_COUNT, BOOK8_CFG_COUNT, BOOK8_CFG_COUNT
};
static const uint32_t key_count[BOOK8_PANEL_COUNT] = {2u, 2u, 3u, 4u, 1u, 0u, 3u, 0u, 0u, 0u};

static void message(const char *text)
{
    snprintf(settings.message, sizeof(settings.message), "%s", text);
}

static uint32_t ipv4_octet(uint32_t address, uint32_t octet)
{
    return (address >> ((3u - octet) * 8u)) & 0xffu;
}

static uint32_t ipv4_set_octet(uint32_t address, uint32_t octet, uint32_t value)
{
    uint32_t shift = (3u - octet) * 8u;
    uint32_t mask = 0xffu << shift;
    return (address & ~mask) | ((value & 0xffu) << shift);
}

static void ipv4_text(uint32_t address, char *text, uint32_t capacity)
{
    snprintf(text, capacity, "%u.%u.%u.%u", ipv4_octet(address, 0u),
        ipv4_octet(address, 1u), ipv4_octet(address, 2u),
        ipv4_octet(address, 3u));
}

static void network_refresh_status(void)
{
    struct book8_network_status status = {0};
    settings.network_status_valid = book8_network_settings_get(&status);
    if (!settings.network_status_valid) {
        settings.network_state[0] = '\0';
        return;
    }
    settings.network_status = status;
    snprintf(settings.network_state, sizeof(settings.network_state), "%s",
        status.state != NULL ? status.state : "unknown");
    settings.network_status.state = settings.network_state;
}

static void network_edit(uint32_t field, int32_t direction)
{
    if (field >= NETWORK_FIELDS) {
        return;
    }
    if (field == 0u) {
        settings.draft.values[BOOK8_CFG_NETWORK] =
            settings.draft.values[BOOK8_CFG_NETWORK] == 0u ? 1u : 0u;
        message("Unsaved network edits.");
        return;
    }
    --field;
    if (field == 0u) {
        settings.network_draft.mode = settings.network_draft.mode
                == BOOK8_NETWORK_DHCP
            ? BOOK8_NETWORK_STATIC : BOOK8_NETWORK_DHCP;
    } else if (field >= 1u && field <= 5u) {
        uint32_t *address = field == 1u ? &settings.network_draft.ip
            : field == 2u ? &settings.network_draft.mask
            : field == 3u ? &settings.network_draft.gateway
            : field == 4u ? &settings.network_draft.dns
            : &settings.network_draft.telemetry;
        uint32_t value = ipv4_octet(*address, settings.network_octet);
        value = direction < 0 ? (value == 0u ? 255u : value - 1u)
            : (value == 255u ? 0u : value + 1u);
        *address = ipv4_set_octet(*address, settings.network_octet, value);
    } else if (field == 6u) {
        int32_t retry = (int32_t)settings.network_draft.retry_ms
            + direction * (int32_t)NETWORK_RETRY_STEP;
        if (retry < (int32_t)NETWORK_RETRY_MIN) {
            retry = (int32_t)NETWORK_RETRY_MIN;
        }
        if (retry > (int32_t)NETWORK_RETRY_MAX) {
            retry = (int32_t)NETWORK_RETRY_MAX;
        }
        settings.network_draft.retry_ms = (uint32_t)retry;
    }
    message("Unsaved network edits.");
}

static void network_apply(const struct book8gui_platform *p, bool persist)
{
    if (!book8_network_settings_apply(&settings.network_draft, persist)) {
        message(persist
            ? "Network save failed; settings were not saved."
            : "Network apply failed; active settings unchanged.");
        return;
    }
    struct book8_system_config config = *book8_config_get();
    config.values[BOOK8_CFG_NETWORK] = settings.draft.values[BOOK8_CFG_NETWORK];
    if (persist && (p->save_config == NULL || !p->save_config(&config))) {
        message("Network profile saved; enable preference save failed.");
        return;
    }
    (void)book8_config_commit(&config);
    network_refresh_status();
    message(persist ? "Network settings applied and saved."
        : "Network settings applied (not saved).");
    book8gui_invalidate();
}

static void network_renew(void)
{
    if (!book8_network_settings_renew()) {
        message("Renew failed; check DHCP mode and network status.");
        return;
    }
    network_refresh_status();
    message("Network lease renewal requested.");
    book8gui_invalidate();
}

static void text_clipped(const struct book8gui_platform *p, uint32_t x,
    uint32_t y, uint32_t width, const char *text, uint32_t fg, uint32_t bg)
{
    char clipped[96];
    uint32_t n = 0u, max = width / 8u;
    while (text[n] != '\0' && n < max && n + 1u < sizeof(clipped)) {
        clipped[n] = text[n];
        ++n;
    }
    clipped[n] = '\0';
    p->draw_text(x, y, clipped, fg, bg);
}

static void button(const struct book8gui_platform *p,
    const struct book8gui_rect *rect, const char *text)
{
    ui_sunken(p, rect, UI_FACE);
    text_clipped(p, rect->x + 4u, rect->y + 3u,
        rect->width > 8u ? rect->width - 8u : 0u, text, UI_TEXT, UI_FACE);
}

static void draw_switch(const struct book8gui_platform *p,
    uint32_t x, uint32_t y, bool enabled)
{
    uint32_t track = enabled ? UI_SELECTED : UI_SHADOW;
    p->fill_rect(x, y + 4u, 42u, 18u, track);
    p->fill_rect(x + (enabled ? 25u : 3u), y + 6u, 14u, 14u,
        enabled ? 0xffffffu : UI_LIGHT);
}

static void action_button(const struct book8gui_platform *p,
    const struct book8gui_rect *rect, const char *text, bool highlighted)
{
    uint32_t background = highlighted ? UI_SELECTED : UI_FACE;
    uint32_t foreground = highlighted ? UI_SELECTED_TEXT : UI_TEXT;
    p->fill_rect(rect->x, rect->y, rect->width, rect->height, background);
    text_clipped(p, rect->x + 4u, rect->y + 5u,
        rect->width > 8u ? rect->width - 8u : 0u, text,
        foreground, background);
}

static uint32_t time_value(uint32_t field)
{
    switch (field) {
    case 0u: return settings.time.hour;
    case 1u: return settings.time.minute;
    case 2u: return settings.time.day;
    case 3u: return settings.time.month;
    default: return settings.time.year;
    }
}

static bool date_valid(void)
{
    static const uint32_t days[] = {31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u};
    uint32_t year = settings.time.year, month = settings.time.month;
    if (!settings.time_valid || year < 1980u || year > 2099u
        || month < 1u || month > 12u
        || settings.time.hour > 23u || settings.time.minute > 59u) {
        return false;
    }
    uint32_t limit = days[month - 1u];
    if (month == 2u && year % 4u == 0u
        && (year % 100u != 0u || year % 400u == 0u)) {
        ++limit;
    }
    return settings.time.day >= 1u && settings.time.day <= limit;
}

static void load_time(const struct book8gui_platform *p)
{
    settings.time_valid = p->read_time != NULL && p->read_time(&settings.time);
    if (!settings.time_valid) {
        settings.time = (struct book8gui_time){0u, 0u, 1u, 1u, 2026u};
        message("Clock read failed; date fields show defaults.");
    }
    settings.time_dirty = false;
}

static uint32_t field_count(void)
{
    if (settings.page == BOOK8_PANEL_NETWORK) {
        return NETWORK_FIELDS;
    }
    if (settings.category == 1u) {
        return 5u;
    }
    if (settings.page == BOOK8_PANEL_TIME) {
        return key_count[settings.page] + 6u;
    }
    if (settings.page == BOOK8_PANEL_SOUND) {
        return key_count[settings.page] + 2u;
    }
    return key_count[settings.page];
}

static bool config_key_for_row(uint32_t row, uint32_t *key)
{
    if (settings.category == 1u) {
        static const uint32_t input_keys[] = {
            BOOK8_CFG_REPEAT_RATE, BOOK8_CFG_KEY_DELAY,
            BOOK8_CFG_POINTER_SPEED, BOOK8_CFG_DOUBLE_CLICK,
            BOOK8_CFG_SWAP_BUTTONS
        };
        if (row >= sizeof(input_keys) / sizeof(input_keys[0])) { return false; }
        *key = input_keys[row];
        return true;
    }
    uint32_t local = row - (settings.page == BOOK8_PANEL_TIME ? 6u : 0u);
    if (settings.page == BOOK8_PANEL_TIME && row < 6u) { return false; }
    if (local >= key_count[settings.page]) { return false; }
    *key = first_key[settings.page] + local;
    return true;
}

static bool toggle_field(uint32_t row)
{
    uint32_t key;
    if (settings.page == BOOK8_PANEL_SOUND && row == 3u) { return true; }
    if (settings.page == BOOK8_PANEL_TIME && row == 5u) { return true; }
    return config_key_for_row(row, &key)
        && book8_config_ranges[key].max == 1u;
}

static bool readout(const struct book8gui_platform *p, uint32_t row,
    char *text, uint32_t capacity, uint32_t *bar)
{
    *bar = UINT32_MAX;
    if (settings.page == BOOK8_PANEL_NETWORK) {
        if (!settings.network_status_valid) {
            if (row != 0u) { return false; }
            snprintf(text, capacity, "Network status unavailable; check adapter.");
            return true;
        }
        const struct book8_network_status *status = &settings.network_status;
        char ip[16], mask[16], gateway[16], dns[16];
        switch (row) {
        case 0u:
            snprintf(text, capacity, "State: %s  %s  Lease: %u s",
                status->state != NULL ? status->state : "unknown",
                status->ready ? "Ready" : "Not ready", status->lease_seconds);
            return true;
        case 1u:
            ipv4_text(status->assigned_ip, ip, sizeof(ip));
            ipv4_text(status->assigned_mask, mask, sizeof(mask));
            snprintf(text, capacity, "Assigned IP: %s  Mask: %s", ip, mask);
            return true;
        case 2u:
            ipv4_text(status->assigned_gateway, gateway, sizeof(gateway));
            ipv4_text(status->dns, dns, sizeof(dns));
            snprintf(text, capacity, "Gateway: %s  DNS: %s", gateway, dns);
            return true;
        default:
            return false;
        }
    }
    if (p->control_readout != NULL) {
        return p->control_readout(settings.page, row, text, capacity, bar);
    }
    if (row != 0u) {
        return false;
    }
    if (settings.page == BOOK8_PANEL_DISPLAY) {
        snprintf(text, capacity, "Active logical canvas: %ux%u", p->width, p->height);
    } else if (settings.page == BOOK8_PANEL_ABOUT) {
        snprintf(text, capacity, "%s / x86-64", BOOK8_GUI_VERSION);
    } else {
        snprintf(text, capacity, "System defaults; no live sample.");
    }
    return true;
}

static void layout_for(const struct book8gui_platform *p,
    const struct WindowApp *app, struct settings_layout *l)
{
    struct book8gui_rect c = ui_client(app);
    uint32_t side = c.width >= 520u ? 220u : c.width / 3u;
    uint32_t header_height = 44u;
    uint32_t body = c.height > 82u ? c.height - 82u : 0u;
    l->header = (struct book8gui_rect){c.x + side + 8u, c.y,
        c.width > side + 8u ? c.width - side - 8u : 0u, header_height};
    l->sidebar = (struct book8gui_rect){c.x, c.y + header_height + 2u,
        side, body};
    l->content = (struct book8gui_rect){c.x + side + 8u,
        c.y + header_height + 2u,
        c.width > side + 8u ? c.width - side - 8u : 0u, body};
    l->track = ui_scroll_track(&l->content);
    l->status = (struct book8gui_rect){c.x + side + 8u,
        c.y + header_height + body + 8u,
        c.width > side + 8u ? c.width - side - 8u : 0u, 20u};
    uint32_t reset_width = 56u;
    uint32_t apply_width = l->header.width > reset_width + 20u
        ? l->header.width - reset_width - 20u : 90u;
    l->apply = (struct book8gui_rect){l->header.x + l->header.width
        - apply_width - reset_width - 8u, l->header.y + 8u,
        apply_width, 26u};
    l->reset = (struct book8gui_rect){l->apply.x + apply_width + 4u,
        l->apply.y, reset_width, 26u};
    uint32_t button_width = l->content.width > 212u ? 64u
        : (l->content.width > 8u ? (l->content.width - 8u) / 3u : 0u);
    if (settings.page == BOOK8_PANEL_NETWORK) {
        l->apply.x = l->header.x + l->header.width
            - button_width * 3u - 12u;
        l->apply.width = button_width;
        l->reset = (struct book8gui_rect){l->apply.x + button_width + 4u,
            l->apply.y, button_width, 24u};
        l->renew = (struct book8gui_rect){l->reset.x + button_width + 4u,
            l->apply.y, button_width, 24u};
    } else {
        l->renew = (struct book8gui_rect){0u, 0u, 0u, 0u};
    }
    l->nav_rows = body > 4u ? (body - 4u) / SETTINGS_NAV_ROW_H : 0u;
    uint32_t nav_max = SETTINGS_CATEGORY_COUNT > l->nav_rows
        ? SETTINGS_CATEGORY_COUNT - l->nav_rows : 0u;
    if (settings.nav_first > nav_max) { settings.nav_first = nav_max; }
    l->rows = body > 8u ? (body - 8u) / ROW_H : 0u;
    l->fields = field_count();
    l->total = l->fields;
    char text[96];
    uint32_t bar;
    for (uint32_t i = 0u; i < READOUT_ROWS && readout(p, i, text, sizeof(text), &bar); ++i) {
        ++l->total;
    }
    uint32_t max = l->total > l->rows ? l->total - l->rows : 0u;
    if (settings.first > max) {
        settings.first = max;
    }
}

static bool slider(enum book8_config_key key)
{
    return key == BOOK8_CFG_REPEAT_RATE || key == BOOK8_CFG_POINTER_SPEED
        || key == BOOK8_CFG_DOUBLE_CLICK || key == BOOK8_CFG_VOLUME
        || key == BOOK8_CFG_TONE;
}

static void value_text(enum book8_config_key key, char *out, uint32_t size)
{
    uint32_t value = settings.draft.values[key];
    static const char *const resolutions[] = {"800x600", "1024x768", "1440x900"};
    static const char *const accents[] = {"Blue", "Cyan", "Teal", "Green",
        "Orange", "Red", "Magenta", "Purple"};
    if (key == BOOK8_CFG_TIMEZONE) {
        int32_t minutes = (int32_t)value * 15 - 720;
        uint32_t magnitude = (uint32_t)(minutes < 0 ? -minutes : minutes);
        snprintf(out, size, "UTC%c%02u:%02u", minutes < 0 ? '-' : '+',
            magnitude / 60u, magnitude % 60u);
    } else if (key == BOOK8_CFG_RESOLUTION) {
        snprintf(out, size, "%s", resolutions[value]);
    } else if (key == BOOK8_CFG_ACCENT) {
        snprintf(out, size, "%s", accents[value]);
    } else if (key == BOOK8_CFG_PRESENT_MODE) {
        snprintf(out, size, "%s", value == 0u ? "Buffered copy" : "Vsync request");
    } else if (key == BOOK8_CFG_KEY_DELAY) {
        snprintf(out, size, "%u ms", (value + 1u) * 250u);
    } else if (book8_config_ranges[key].max == 1u) {
        bool enabled = value != 0u;
        snprintf(out, size, "[%c] %s", enabled ? 'X' : ' ',
            enabled ? "On" : "Off");
    } else {
        snprintf(out, size, "%u", value);
    }
}

static void change(uint32_t field, int32_t direction)
{
    if (settings.page == BOOK8_PANEL_NETWORK) {
        network_edit(field, direction);
        return;
    }
    if (settings.page == BOOK8_PANEL_SOUND && field == 3u) {
        settings.ui_beep_draft = !settings.ui_beep_draft;
        message("Unsaved UI beep setting.");
        return;
    }
    if (settings.page == BOOK8_PANEL_TIME && field == 5u) {
        settings.clock24h_draft = !settings.clock24h_draft;
        message("Unsaved clock-format edit.");
        return;
    }
    if (settings.page == BOOK8_PANEL_TIME && field < 5u) {
        static const uint32_t low[] = {0u, 0u, 1u, 1u, 1980u};
        static const uint32_t high[] = {23u, 59u, 31u, 12u, 2099u};
        int32_t value = (int32_t)time_value(field) + direction;
        if (value < (int32_t)low[field]) { value = (int32_t)high[field]; }
        if (value > (int32_t)high[field]) { value = (int32_t)low[field]; }
        switch (field) {
        case 0u: settings.time.hour = (uint8_t)value; break;
        case 1u: settings.time.minute = (uint8_t)value; break;
        case 2u: settings.time.day = (uint8_t)value; break;
        case 3u: settings.time.month = (uint8_t)value; break;
        default: settings.time.year = (uint16_t)value; break;
        }
        settings.time_dirty = true;
        settings.time_valid = true;
    } else {
        uint32_t key;
        if (!config_key_for_row(field, &key)) { return; }
        const struct book8_config_range *range = &book8_config_ranges[key];
        int32_t value = (int32_t)settings.draft.values[key]
            + direction * (int32_t)range->step;
        if (value < (int32_t)range->min) { value = (int32_t)range->min; }
        if (value > (int32_t)range->max) { value = (int32_t)range->max; }
        settings.draft.values[key] = (uint32_t)value;
    }
    message("Unsaved changes. Enter or Apply saves all pages.");
}

static void apply(const struct book8gui_platform *p)
{
    if (settings.page == BOOK8_PANEL_NETWORK) {
        network_apply(p, true);
        return;
    }
    if (settings.time_dirty && !date_valid()) {
        message("Invalid calendar date; nothing saved.");
        return;
    }
    if (settings.draft.values[BOOK8_CFG_RESOLUTION]
        != book8_config_value(BOOK8_CFG_RESOLUTION)) {
        if (settings.time_dirty && p->set_display_mode != NULL) {
            message("Save clock edits before previewing a display mode.");
            return;
        }
        enum book8_display_request result = book8gui_preview_display(&settings.draft);
        if (result == BOOK8_DISPLAY_STARTED) {
            message("Display preview; confirm in the modal dialog.");
            return;
        }
        if (result == BOOK8_DISPLAY_FAILED) {
            message(book8gui_display_status());
            return;
        }
    }
    if (!reg_set_u32("HKLM\\SOFTWARE\\BOOK8\\Settings", "Clock24H",
            settings.clock24h_draft ? 1u : 0u)
        || !reg_set_u32("HKLM\\SOFTWARE\\BOOK8\\Settings", "UiBeep",
            settings.ui_beep_draft ? 1u : 0u)) {
        message("Could not update Settings values in the registry.");
        return;
    }
    if (p->save_config == NULL || !p->save_config(&settings.draft)) {
        message("Save failed; active preferences unchanged.");
        return;
    }
    (void)book8_config_commit(&settings.draft);
    if (settings.time_dirty) {
        if (p->set_time == NULL || !p->set_time(&settings.time)) {
            message("Preferences saved; clock update failed.");
            return;
        }
        settings.time_dirty = false;
        if (p->diagnostic != NULL) {
            char text[80];
            snprintf(text, sizeof(text), "BOOK8-SETTINGS: time set %04u-%02u-%02u %02u:%02u",
                settings.time.year, settings.time.month, settings.time.day,
                settings.time.hour, settings.time.minute);
            p->diagnostic(text);
        }
        settings.clock24h = settings.clock24h_draft;
        settings.ui_beep = settings.ui_beep_draft;
    }
    message(settings.page == BOOK8_PANEL_DISPLAY
        ? "Preferences saved; active canvas shown below."
        : "Saved to A:\\BOOK8.CFG.");
    book8gui_invalidate();
}

static void draw_network_ip(const struct book8gui_platform *p, uint32_t x,
    uint32_t y, uint32_t width, uint32_t address)
{
    uint32_t start = x + (width > 128u ? (width - 128u) / 2u : 0u);
    for (uint32_t octet = 0u; octet < 4u; ++octet) {
        char value[4];
        snprintf(value, sizeof(value), "%u", ipv4_octet(address, octet));
        uint32_t value_x = start + octet * 32u;
        bool selected = octet == settings.network_octet;
        p->draw_text(value_x, y, value,
            selected ? UI_SELECTED_TEXT : UI_TEXT,
            selected ? UI_SELECTED : UI_FIELD);
        if (octet != 3u) {
            p->draw_text(value_x + 24u, y, ".", UI_TEXT, UI_FIELD);
        }
    }
}

static void draw_network_field(const struct book8gui_platform *p,
    uint32_t field, uint32_t row, uint32_t x, uint32_t width, uint32_t y)
{
    static const char *const names[NETWORK_FIELDS] = {
        "Adapter connection preference", "Address mode", "Static IPv4 address", "Subnet mask",
        "Gateway", "DNS server", "Collector IPv4 address", "Retry interval (ms)"
    };
    text_clipped(p, x, y, width, names[field],
        row == settings.field ? UI_SELECTED : UI_TEXT, UI_FIELD);
    struct book8gui_rect minus = {x, y + 20u, 20u, 22u};
    struct book8gui_rect plus = {x + width - 20u, y + 20u, 20u, 22u};
    button(p, &minus, "-");
    button(p, &plus, "+");
    if (field == 0u) {
        char value[24];
        value_text(BOOK8_CFG_NETWORK, value, sizeof(value));
        text_clipped(p, x + 24u, y + 20u, width - 48u, value, UI_TEXT, UI_FIELD);
        return;
    }
    --field;
    if (field == 0u) {
        text_clipped(p, x + 24u, y + 20u, width - 48u,
            settings.network_draft.mode == BOOK8_NETWORK_DHCP
                ? "DHCP" : "Static", UI_TEXT, UI_FIELD);
    } else if (field <= 5u) {
        uint32_t address = field == 1u ? settings.network_draft.ip
            : field == 2u ? settings.network_draft.mask
            : field == 3u ? settings.network_draft.gateway
            : field == 4u ? settings.network_draft.dns
            : settings.network_draft.telemetry;
        draw_network_ip(p, x + 24u, y + 20u, width - 48u, address);
    } else {
        char value[24];
        snprintf(value, sizeof(value), "%u", settings.network_draft.retry_ms);
        text_clipped(p, x + 24u, y + 20u, width - 48u, value, UI_TEXT, UI_FIELD);
    }
}

static void select_page(uint32_t page);

struct programs_layout {
    struct book8gui_rect list, track, details, confirmation, yes, no;
    uint32_t rows;
};

static void programs_layout(const struct book8gui_platform *p,
    const struct settings_layout *l, struct programs_layout *out)
{
    uint32_t left = l->content.width / 2u;
    out->list = (struct book8gui_rect){l->content.x, l->content.y, left,
        l->content.height};
    out->details = (struct book8gui_rect){l->content.x + left + 4u,
        l->content.y, l->content.width > left + 4u ? l->content.width - left - 4u : 0u,
        l->content.height};
    out->track = ui_scroll_track(&out->list);
    out->rows = out->list.height > 8u ? (out->list.height - 8u) / 22u : 0u;
    uint32_t width = p->width < 560u ? p->width - 16u : 544u;
    out->confirmation = (struct book8gui_rect){(p->width - width) / 2u,
        (p->height - BOOK8GUI_TASKBAR_HEIGHT - 176u) / 2u, width, 176u};
    out->yes = (struct book8gui_rect){out->confirmation.x + 16u,
        out->confirmation.y + 140u, 64u, 24u};
    out->no = out->yes;
    out->no.x += 72u;
}

static uint32_t wrapped_text(const struct book8gui_platform *p,
    const struct book8gui_rect *box, uint32_t y, const char *text, uint32_t color)
{
    uint32_t cols = box->width > 12u ? (box->width - 12u) / 8u : 0u;
    if (cols > 95u) { cols = 95u; }
    while (*text != '\0' && cols != 0u && y + 16u <= box->y + box->height) {
        char line[96];
        uint32_t n = 0u;
        while (n < cols && text[n] != '\0') { line[n] = text[n]; ++n; }
        line[n] = '\0';
        p->draw_text(box->x + 6u, y, line, color, UI_FACE);
        text += n;
        y += 16u;
    }
    return y;
}

static void programs_render(const struct book8gui_platform *p,
    const struct settings_layout *l)
{
    struct programs_layout view;
    programs_layout(p, l, &view);
    uint32_t count = book8gui_program_count();
    if (settings.program_selected >= count) { settings.program_selected = count != 0u ? count - 1u : 0u; }
    settings.program_first = ui_wheel_scroll(settings.program_first, count, view.rows, 0, 1u);
    ui_sunken(p, &view.list, UI_FIELD);
    ui_sunken(p, &view.details, UI_FACE);
    for (uint32_t row = 0u; row < view.rows; ++row) {
        uint32_t index = settings.program_first + row;
        struct book8gui_program info;
        if (!book8gui_program_info(index, &info)) { break; }
        uint32_t y = view.list.y + 4u + row * 22u;
        bool selected = index == settings.program_selected;
        uint32_t bg = selected ? UI_SELECTED : UI_FIELD;
        uint32_t width = view.list.width > 18u ? view.list.width - 18u : 0u;
        p->fill_rect(view.list.x + 2u, y, width, 20u, bg);
        text_clipped(p, view.list.x + 4u, y + 2u,
            width > 4u ? width - 4u : 0u, info.name,
            selected ? UI_SELECTED_TEXT : UI_TEXT, bg);
    }
    ui_scrollbar(p, &view.track, count, view.rows, settings.program_first);
    struct book8gui_program info;
    bool removable = false;
    if (book8gui_program_info(settings.program_selected, &info)) {
        uint32_t y = view.details.y + 4u;
        y = wrapped_text(p, &view.details, y, info.name, UI_TEXT);
        y = wrapped_text(p, &view.details, y + 4u, info.path, UI_TEXT);
        char size[64];
        if (info.allocation_valid) {
            snprintf(size, sizeof(size), "On disk: %llu bytes", (unsigned long long)info.allocated_bytes);
        } else {
            snprintf(size, sizeof(size), "On disk: unavailable");
        }
        y = wrapped_text(p, &view.details, y + 4u, size, UI_TEXT);
        (void)wrapped_text(p, &view.details, y + 4u,
            info.system_component ? "System Core - System Component (read-only)" : "User Installed",
            info.system_component ? UI_MUTED : UI_TEXT);
        removable = !info.system_component && p->delete_entry != NULL;
    }
    button(p, &l->apply, "Remove");
    if (!removable) {
        text_clipped(p, l->apply.x + 4u, l->apply.y + 3u,
            l->apply.width - 8u, "Remove", UI_MUTED, UI_FACE);
    }
    button(p, &l->reset, "Refresh");
    ui_sunken(p, &l->status, UI_FACE);
    text_clipped(p, l->status.x + 4u, l->status.y + 2u, l->status.width - 8u,
        settings.message[0] != '\0' ? settings.message
            : "Select a program. System Components cannot be removed.", UI_TEXT, UI_FACE);
    if (settings.remove_confirm) {
        ui_sunken(p, &view.confirmation, UI_FACE);
        char prompt[256];
        snprintf(prompt, sizeof(prompt), "Are you sure you want to remove %s from BOOK8OS?",
            settings.remove_target.name);
        struct book8gui_rect text = view.confirmation;
        text.height = text.height > 36u ? text.height - 36u : 0u;
        (void)wrapped_text(p, &text, text.y + 6u, prompt, UI_TEXT);
        button(p, &view.yes, "Yes");
        button(p, &view.no, "No");
    }
}

static void programs_confirm(const struct book8gui_platform *p, struct WindowApp *app, bool yes)
{
    (void)p;
    settings.remove_confirm = false;
    book8gui_set_modal(app, false);
    if (yes) {
        (void)book8gui_remove_program(settings.remove_target.path);
        message(book8gui_program_status());
    } else {
        message("Removal cancelled.");
    }
}

static void programs_remove(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (!book8gui_program_info(settings.program_selected, &settings.remove_target)
        || settings.remove_target.system_component || p->delete_entry == NULL) {
        message("System Component: removal is disabled.");
        return;
    }
    settings.remove_confirm = true;
    book8gui_set_modal(app, true);
}

static bool programs_event(const struct book8gui_platform *p, struct WindowApp *app,
    const struct book8gui_event *event, const struct settings_layout *l)
{
    struct programs_layout view;
    programs_layout(p, l, &view);
    uint32_t count = book8gui_program_count();
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        bool down = (event->mouse_buttons & 1u) != 0u;
        bool pressed = down && (settings.previous_buttons & 1u) == 0u;
        settings.previous_buttons = event->mouse_buttons;
        if (!down) { settings.program_drag = false; }
        if (settings.remove_confirm) {
            if (pressed && ui_contains(&view.yes, event->mouse_x, event->mouse_y)) {
                programs_confirm(p, app, true);
            } else if (pressed && ui_contains(&view.no, event->mouse_x, event->mouse_y)) {
                programs_confirm(p, app, false);
            }
        } else if (down && settings.program_drag) {
            settings.program_first = ui_scrollbar_drag_position(&view.track, count,
                view.rows, event->mouse_y, settings.scroll_grab);
        } else if (ui_contains(&l->sidebar, event->mouse_x, event->mouse_y)) {
            settings.nav_first = ui_wheel_scroll_delta(settings.nav_first, SETTINGS_CATEGORY_COUNT,
                l->nav_rows, event->wheel_delta, 1u, &settings.wheel_remainder[0]);
            if (pressed && event->mouse_y >= (int32_t)(l->sidebar.y + 4u)) {
                uint32_t page = settings.nav_first
                    + ((uint32_t)event->mouse_y - l->sidebar.y - 4u)
                        / SETTINGS_NAV_ROW_H;
                if (page < SETTINGS_CATEGORY_COUNT) { select_page(page); }
            }
        } else {
            if (ui_contains(&view.list, event->mouse_x, event->mouse_y)) {
                settings.program_first = ui_wheel_scroll_delta(settings.program_first, count,
                    view.rows, event->wheel_delta, 1u, &settings.wheel_remainder[2]);
                if (pressed && ui_contains(&view.track, event->mouse_x, event->mouse_y)) {
                    settings.program_drag = count > view.rows;
                    settings.scroll_grab = ui_scrollbar_grab(&view.track, count,
                        view.rows, settings.program_first, event->mouse_y);
                } else if (pressed && event->mouse_y >= (int32_t)(view.list.y + 4u)) {
                    uint32_t row = ((uint32_t)event->mouse_y - view.list.y - 4u) / 22u;
                    uint32_t index = settings.program_first + row;
                    if (row < view.rows && index < count) { settings.program_selected = index; }
                }
            }
            if (down && settings.program_drag) {
                settings.program_first = ui_scrollbar_drag_position(&view.track, count,
                    view.rows, event->mouse_y, settings.scroll_grab);
            }
            if (pressed && ui_contains(&l->apply, event->mouse_x, event->mouse_y)) {
                programs_remove(p, app);
            } else if (pressed && ui_contains(&l->reset, event->mouse_x, event->mouse_y)) {
                (void)book8gui_refresh_programs();
                message(book8gui_program_status());
            }
        }
    } else if (settings.remove_confirm) {
        if (event->key == BOOK8GUI_KEY_ENTER || event->key == 'y' || event->key == 'Y') {
            programs_confirm(p, app, true);
        } else if (event->key == BOOK8GUI_KEY_ESCAPE || event->key == 'n' || event->key == 'N') {
            programs_confirm(p, app, false);
        }
    } else {
        switch (event->key) {
        case BOOK8GUI_KEY_PAGE_UP:
            select_page((settings.category + SETTINGS_CATEGORY_COUNT - 1u)
                % SETTINGS_CATEGORY_COUNT); break;
        case BOOK8GUI_KEY_PAGE_DOWN:
            select_page((settings.category + 1u) % SETTINGS_CATEGORY_COUNT); break;
        case BOOK8GUI_KEY_UP:
            if (settings.program_selected != 0u) { --settings.program_selected; } break;
        case BOOK8GUI_KEY_DOWN:
            if (settings.program_selected + 1u < count) { ++settings.program_selected; } break;
        case BOOK8GUI_KEY_HOME: settings.program_selected = 0u; break;
        case BOOK8GUI_KEY_END: settings.program_selected = count != 0u ? count - 1u : 0u; break;
        case BOOK8GUI_KEY_ENTER:
        case BOOK8GUI_KEY_DELETE: programs_remove(p, app); break;
        case 'r': case 'R':
            (void)book8gui_refresh_programs();
            message(book8gui_program_status()); break;
        default: return false;
        }
        if (settings.program_selected < settings.program_first) {
            settings.program_first = settings.program_selected;
        } else if (view.rows != 0u && settings.program_selected >= settings.program_first + view.rows) {
            settings.program_first = settings.program_selected - view.rows + 1u;
        }
    }
    book8gui_invalidate();
    return true;
}

static void profile_load(void)
{
    memset(&settings.profile_session, 0, sizeof(settings.profile_session));
    memset(settings.profile_current_password, 0,
        sizeof(settings.profile_current_password));
    memset(settings.profile_new_password, 0,
        sizeof(settings.profile_new_password));
    memset(settings.profile_confirm_password, 0,
        sizeof(settings.profile_confirm_password));
    if (!book8_session_current(&settings.profile_session)
        || !book8_session_profile(settings.profile_username,
            sizeof(settings.profile_username), settings.profile_display_name,
            sizeof(settings.profile_display_name))) {
        message("No authenticated session is available.");
        return;
    }
    settings.profile_focus = 0u;
    message("The built-in admin account is permanently UID 0 / root.");
}

static void profile_render(const struct book8gui_platform *p,
    const struct settings_layout *l)
{
    static const char *const labels[] = {
        "Login username", "Display name", "Current password", "New password",
        "Confirm new password"
    };
    const char *values[] = {
        settings.profile_username, settings.profile_display_name,
        settings.profile_current_password, settings.profile_new_password,
        settings.profile_confirm_password
    };
    uint32_t x = l->content.x + 12u;
    uint32_t width = l->content.width > 24u ? l->content.width - 24u : 0u;
    uint32_t y = l->content.y + 10u;
    text_clipped(p, x, y, width, "Primary administrator profile",
        UI_TEXT, UI_FIELD);
    for (uint32_t i = 0u; i < 5u; ++i) {
        uint32_t row_y = y + 28u + i * 58u;
        text_clipped(p, x, row_y, width, labels[i], UI_MUTED, UI_FIELD);
        struct book8gui_rect field = {x, row_y + 18u, width, 28u};
        ui_sunken(p, &field, UI_FIELD);
        char masked[BOOK8_SESSION_PASSWORD_MAX + 1u];
        const char *text = values[i];
        if (i >= 2u) {
            uint32_t n = 0u;
            while (text[n] != '\0' && n + 1u < sizeof(masked)) {
                masked[n] = '*';
                ++n;
            }
            masked[n] = '\0';
            text = masked;
        }
        text_clipped(p, field.x + 6u, field.y + 6u,
            field.width > 12u ? field.width - 12u : 0u, text,
            settings.profile_focus == i ? UI_SELECTED : UI_TEXT, UI_FIELD);
    }
    text_clipped(p, x, y + 324u, width,
        "The admin account always retains UID 0 and root access.",
        UI_MUTED, UI_FIELD);
}

static bool profile_save(void)
{
    if (strcmp(settings.profile_new_password,
            settings.profile_confirm_password) != 0) {
        message("New password confirmation does not match.");
        return false;
    }
    if (!book8_session_update_profile(settings.profile_session.uid,
            settings.profile_session.token, settings.profile_current_password,
            settings.profile_username, settings.profile_display_name,
            settings.profile_new_password)) {
        message("Profile update failed; verify current password and fields.");
        memset(settings.profile_current_password, 0,
            sizeof(settings.profile_current_password));
        memset(settings.profile_new_password, 0,
            sizeof(settings.profile_new_password));
        memset(settings.profile_confirm_password, 0,
            sizeof(settings.profile_confirm_password));
        return false;
    }
    memset(settings.profile_current_password, 0,
        sizeof(settings.profile_current_password));
    memset(settings.profile_new_password, 0,
        sizeof(settings.profile_new_password));
    memset(settings.profile_confirm_password, 0,
        sizeof(settings.profile_confirm_password));
    (void)book8_session_current(&settings.profile_session);
    message("User profile saved to A:\\BOOK8REG.INI.");
    return true;
}

static bool profile_event(const struct book8gui_event *event,
    const struct settings_layout *l)
{
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        bool pressed = (event->mouse_buttons & 1u) != 0u
            && (settings.previous_buttons & 1u) == 0u;
        settings.previous_buttons = event->mouse_buttons;
        if (!pressed) { return true; }
        if (ui_contains(&l->sidebar, event->mouse_x, event->mouse_y)
            && event->mouse_y >= (int32_t)(l->sidebar.y + 4u)) {
            uint32_t page = settings.nav_first
                + ((uint32_t)event->mouse_y - l->sidebar.y - 4u)
                    / SETTINGS_NAV_ROW_H;
            if (page < SETTINGS_CATEGORY_COUNT) { select_page(page); }
        } else if (ui_contains(&l->apply, event->mouse_x, event->mouse_y)) {
            (void)profile_save();
        } else if (ui_contains(&l->reset, event->mouse_x, event->mouse_y)) {
            profile_load();
        } else if (ui_contains(&l->content, event->mouse_x, event->mouse_y)) {
            uint32_t y = l->content.y + 10u + 28u;
            int32_t offset = event->mouse_y - (int32_t)y;
            if (offset >= 0) {
                uint32_t row = (uint32_t)offset / 58u;
                if (row < 5u && (uint32_t)offset % 58u >= 18u) {
                    settings.profile_focus = (uint8_t)row;
                }
            }
        }
        book8gui_invalidate();
        return true;
    }
    if (event->type != BOOK8GUI_EVENT_KEY) { return true; }
    uint32_t key = event->key & BOOK8GUI_KEY_MASK;
    if (key == BOOK8GUI_KEY_PAGE_UP) {
        select_page((settings.category + SETTINGS_CATEGORY_COUNT - 1u)
            % SETTINGS_CATEGORY_COUNT);
    } else if (key == BOOK8GUI_KEY_PAGE_DOWN) {
        select_page((settings.category + 1u) % SETTINGS_CATEGORY_COUNT);
    } else if (key == BOOK8GUI_KEY_TAB) {
        settings.profile_focus = (uint8_t)((settings.profile_focus + 1u) % 5u);
    } else if (key == BOOK8GUI_KEY_ENTER) {
        if (settings.profile_focus == 4u) { (void)profile_save(); }
        else { ++settings.profile_focus; }
    } else if (key == BOOK8GUI_KEY_BACKSPACE) {
        char *value = settings.profile_focus == 0u ? settings.profile_username
            : settings.profile_focus == 1u ? settings.profile_display_name
            : settings.profile_focus == 2u ? settings.profile_current_password
            : settings.profile_focus == 3u ? settings.profile_new_password
            : settings.profile_confirm_password;
        uint32_t length = 0u;
        while (value[length] != '\0') { ++length; }
        if (length != 0u) { value[length - 1u] = '\0'; }
    } else if (key >= 32u && key <= 126u) {
        char *value = settings.profile_focus == 0u ? settings.profile_username
            : settings.profile_focus == 1u ? settings.profile_display_name
            : settings.profile_focus == 2u ? settings.profile_current_password
            : settings.profile_focus == 3u ? settings.profile_new_password
            : settings.profile_confirm_password;
        uint32_t capacity = settings.profile_focus < 2u
            ? BOOK8_SESSION_NAME_MAX : BOOK8_SESSION_PASSWORD_MAX + 1u;
        uint32_t length = 0u;
        while (length + 1u < capacity && value[length] != '\0') { ++length; }
        if (length + 1u < capacity) {
            value[length] = (char)key;
            value[length + 1u] = '\0';
        }
    } else {
        return false;
    }
    book8gui_invalidate();
    return true;
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (settings.page == BOOK8_PANEL_NETWORK) {
        network_refresh_status();
    }
    struct settings_layout l;
    layout_for(p, app, &l);
    ui_sunken(p, &l.sidebar, UI_FACE);
    for (uint32_t r = 0u; r < l.nav_rows; ++r) {
        uint32_t i = settings.nav_first + r;
        if (i >= SETTINGS_CATEGORY_COUNT) { break; }
        uint32_t y = l.sidebar.y + 4u + r * SETTINGS_NAV_ROW_H;
        if (y + SETTINGS_NAV_ROW_H > l.sidebar.y + l.sidebar.height) { break; }
        bool active = i == settings.category;
        uint32_t row_x = l.sidebar.x + 3u;
        uint32_t row_width = l.sidebar.width - 6u;
        p->fill_rect(row_x, y, row_width, SETTINGS_NAV_ROW_H - 3u,
            active ? (book8_config_value(BOOK8_CFG_DARK) ? 0x383838u : 0xe9f3ffu)
                : UI_FACE);
        uint32_t badge_x = row_x + 6u, badge_y = y + 7u;
        static const uint8_t badge_widths[20] = {
            8u, 12u, 14u, 16u, 18u, 18u, 20u, 20u, 20u, 20u,
            20u, 20u, 20u, 18u, 18u, 16u, 14u, 12u, 8u, 4u
        };
        for (uint32_t line = 0u; line < 20u; ++line) {
            uint32_t inset = (20u - badge_widths[line]) / 2u;
            p->fill_rect(badge_x + inset, badge_y + line,
                badge_widths[line], 1u, category_colors[i]);
        }
        char icon[2] = {pages[i][0], '\0'};
        p->draw_text(badge_x + 6u, badge_y + 2u, icon, 0xffffffu,
            category_colors[i]);
        uint32_t title_x = badge_x + 28u;
        uint32_t text_width = row_width > 42u ? row_width - 42u : 0u;
        text_clipped(p, title_x, y + 3u, text_width, pages[i],
            UI_TEXT, active ? (book8_config_value(BOOK8_CFG_DARK)
                ? 0x383838u : 0xe9f3ffu) : UI_FACE);
        text_clipped(p, title_x, y + 20u, text_width, subtitles[i][0],
            UI_MUTED, UI_FACE);
        text_clipped(p, title_x, y + 35u, text_width, subtitles[i][1],
            UI_MUTED, UI_FACE);
    }
    ui_sunken(p, &l.header, UI_FACE);
    text_clipped(p, l.header.x + 6u, l.header.y + 12u,
        l.header.width > 180u ? l.header.width - 180u : 0u,
        pages[settings.category], UI_TEXT, UI_FACE);
    bool dirty = settings.time_dirty;
    dirty = dirty || settings.clock24h != settings.clock24h_draft
        || settings.ui_beep != settings.ui_beep_draft;
    if (settings.page == BOOK8_PANEL_NETWORK) {
        dirty = dirty
            || settings.network_draft.mode != settings.network_status.settings.mode
            || settings.network_draft.ip != settings.network_status.settings.ip
            || settings.network_draft.mask != settings.network_status.settings.mask
            || settings.network_draft.gateway != settings.network_status.settings.gateway
            || settings.network_draft.dns != settings.network_status.settings.dns
            || settings.network_draft.telemetry != settings.network_status.settings.telemetry
            || settings.network_draft.retry_ms != settings.network_status.settings.retry_ms;
    }
    for (uint32_t key = 0u; key < BOOK8_CFG_COUNT; ++key) {
        if (settings.draft.values[key] != book8_config_value((enum book8_config_key)key)) {
            dirty = true;
            break;
        }
    }
    action_button(p, &l.apply, settings.page == BOOK8_PANEL_PROGRAMS
        ? "Remove" : settings.page == BOOK8_PANEL_ADMIN
            ? "Save Profile" : "Apply & Save", dirty);
    button(p, &l.reset, settings.page == BOOK8_PANEL_PROGRAMS
        ? "Refresh" : "Reset");
    if (settings.page == BOOK8_PANEL_PROGRAMS) {
        programs_render(p, &l);
        return;
    }
    if (settings.page == BOOK8_PANEL_ADMIN) {
        profile_render(p, &l);
        ui_sunken(p, &l.status, UI_FACE);
        text_clipped(p, l.status.x + 4u, l.status.y + 2u,
            l.status.width > 8u ? l.status.width - 8u : 0u,
            settings.message, UI_TEXT, UI_FACE);
        return;
    }
    ui_sunken(p, &l.content, UI_FIELD);
    uint32_t x = l.content.x + 4u;
    uint32_t width = l.content.width > 24u ? l.content.width - 24u : 0u;
    for (uint32_t i = 0u; i < l.rows; ++i) {
        uint32_t row = settings.first + i;
        if (row >= l.total || width < 64u) { break; }
        uint32_t y = l.content.y + 4u + i * ROW_H;
        uint32_t color = row == settings.field ? UI_SELECTED : UI_TEXT;
        char text[96];
        const char *label = NULL;
        uint32_t bar = UINT32_MAX;
        int32_t key = -1;
        if (settings.page == BOOK8_PANEL_NETWORK) {
            if (row >= l.fields) {
                if (readout(p, row - l.fields, text, sizeof(text), &bar)) {
                    text_clipped(p, x, y + 2u, width, text, UI_MUTED, UI_FIELD);
                }
            } else {
                draw_network_field(p, row, row, x, width, y);
            }
            continue;
        }
        if (row >= l.fields) {
            if (!readout(p, row - l.fields, text, sizeof(text), &bar)) { continue; }
            text_clipped(p, x, y + 2u, width, text, UI_MUTED, UI_FIELD);
            if (bar <= 1000u) {
                p->fill_rect(x, y + 24u, width, 10u, UI_FACE);
                p->fill_rect(x, y + 24u, (uint32_t)((uint64_t)width * bar / 1000u),
                    10u, book8_accent_colors[settings.draft.values[BOOK8_CFG_ACCENT]]);
            }
            continue;
        }
        if (settings.page == BOOK8_PANEL_TIME && row < 5u) {
            static const char *const names[] = {"Hour", "Minute", "Day", "Month", "Year"};
            label = names[row];
            snprintf(text, sizeof(text), "%u", time_value(row));
        } else if (settings.page == BOOK8_PANEL_TIME && row == 5u) {
            label = "Clock format";
            snprintf(text, sizeof(text), "%s",
                settings.clock24h_draft ? "24-hour" : "12-hour");
        } else if (settings.page == BOOK8_PANEL_SOUND && row == 3u) {
            label = "UI beep";
            snprintf(text, sizeof(text), "%s", settings.ui_beep_draft ? "On" : "Off");
        } else if (settings.page == BOOK8_PANEL_SOUND && row == 4u) {
            label = "Test alert tone";
            snprintf(text, sizeof(text), "Enter to play");
        } else {
            uint32_t config_key;
            if (!config_key_for_row(row, &config_key)) {
                struct book8gui_rect tone = {x, y + 4u, width, 24u};
                button(p, &tone, "Test alert tone");
                continue;
            }
            key = (int32_t)config_key;
            label = labels[key];
            value_text((enum book8_config_key)key, text, sizeof(text));
        }
        struct book8gui_rect minus = {x, y + 10u, 20u, 24u};
        struct book8gui_rect plus = {x + width - 20u, y + 10u, 20u, 24u};
        p->fill_rect(minus.x, minus.y, minus.width, minus.height, UI_FACE);
        p->fill_rect(plus.x, plus.y, plus.width, plus.height, UI_FACE);
        p->draw_text(minus.x + 6u, minus.y + 4u, "-", UI_TEXT, UI_FACE);
        p->draw_text(plus.x + 6u, plus.y + 4u, "+", UI_TEXT, UI_FACE);
        text_clipped(p, x + 28u, y + 6u,
            width > 136u ? width - 136u : 0u, label, color, UI_FIELD);
        if ((key >= 0 && book8_config_ranges[key].max == 1u)
            || (settings.page == BOOK8_PANEL_TIME && row == 5u)
            || (settings.page == BOOK8_PANEL_SOUND && row == 3u)) {
            uint32_t value_x = x + width - 94u;
            text_clipped(p, value_x, y + 6u, 42u, text,
                UI_SELECTED, UI_FIELD);
            draw_switch(p, x + width - 50u, y + 2u,
                settings.page == BOOK8_PANEL_TIME && row == 5u
                    ? settings.clock24h_draft
                    : settings.page == BOOK8_PANEL_SOUND && row == 3u
                        ? settings.ui_beep_draft
                        : settings.draft.values[key] != 0u);
        } else {
            text_clipped(p, x + width - 88u, y + 6u, 62u, text,
                UI_SELECTED, UI_FIELD);
            if (key >= 0 && slider((enum book8_config_key)key)) {
                const struct book8_config_range *range = &book8_config_ranges[key];
                uint32_t span = width > 148u ? width - 148u : 0u;
                uint32_t filled = span == 0u ? 0u
                    : (uint32_t)((uint64_t)span
                        * (settings.draft.values[key] - range->min)
                        / (range->max - range->min));
                p->fill_rect(x + 28u, y + 34u, span, 3u, UI_SHADOW);
                p->fill_rect(x + 28u, y + 34u, filled, 3u, UI_SELECTED);
            }
        }
        p->fill_rect(x + 4u, y + ROW_H - 1u,
            width > 8u ? width - 8u : 0u, 1u,
            book8_config_value(BOOK8_CFG_DARK) ? 0x484848u : 0xe4e7ebu);
    }
    ui_scrollbar(p, &l.track, l.total, l.rows, settings.first);
    if (settings.page == BOOK8_PANEL_NETWORK) {
        button(p, &l.renew, "Renew");
    }
    ui_sunken(p, &l.status, UI_FACE);
    const char *hint = settings.message[0] != '\0' ? settings.message
        : "PgUp/PgDn category; Tab field; Left/Right edit; Enter save.";
    if (settings.page == BOOK8_PANEL_NETWORK && settings.message[0] == '\0') {
        hint = "Select an octet; Left/Right edits. Apply, Save, or Renew.";
    }
    if (settings.page == BOOK8_PANEL_DISPLAY && book8gui_display_status()[0] != '\0') {
        hint = book8gui_display_status();
    }
    text_clipped(p, l.status.x + 4u, l.status.y + 2u,
        l.status.width > 8u ? l.status.width - 8u : 0u, hint, UI_TEXT, UI_FACE);
}

static void select_page(uint32_t page)
{
    if (page >= SETTINGS_CATEGORY_COUNT) { return; }
    settings.category = page;
    settings.page = category_pages[page];
    settings.first = settings.field = 0u;
    settings.dragging = -1;
    settings.nav_first = page;
    if (settings.page == BOOK8_PANEL_PROGRAMS) {
        (void)book8gui_refresh_programs();
        settings.program_selected = settings.program_first = 0u;
        message(book8gui_program_status());
    } else if (settings.page == BOOK8_PANEL_ADMIN) {
        profile_load();
    }
}

static void adjust_slider(const struct settings_layout *l, uint32_t field, int32_t mx)
{
    uint32_t key;
    if (!config_key_for_row(field, &key) || l->content.width <= 72u) { return; }
    if (!slider((enum book8_config_key)key)) { return; }
    const struct book8_config_range *r = &book8_config_ranges[key];
    uint32_t span = l->content.width - 72u;
    int32_t offset = mx - (int32_t)(l->content.x + 28u);
    uint32_t clamped = offset < 0 ? 0u
        : (uint32_t)offset > span ? span : (uint32_t)offset;
    uint32_t value = r->min + (uint32_t)((uint64_t)(r->max - r->min) * clamped / span);
    settings.draft.values[key] = r->min + (value - r->min) / r->step * r->step;
    message("Unsaved changes. Enter or Apply saves all pages.");
}

static bool handle_event(const struct book8gui_platform *p, struct WindowApp *app,
    const struct book8gui_event *event)
{
    struct settings_layout l;
    layout_for(p, app, &l);
    if (settings.page == BOOK8_PANEL_PROGRAMS) {
        return programs_event(p, app, event, &l);
    }
    if (settings.page == BOOK8_PANEL_ADMIN) {
        return profile_event(event, &l);
    }
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        bool down = (event->mouse_buttons & 1u) != 0u;
        bool pressed = down && (settings.previous_buttons & 1u) == 0u;
        settings.previous_buttons = event->mouse_buttons;
        if (!down) { settings.dragging = -1; }
        if (event->wheel_delta != 0 && ui_contains(&l.content, event->mouse_x, event->mouse_y)) {
            settings.first = ui_wheel_scroll_delta(settings.first, l.total, l.rows,
                event->wheel_delta, 1u, &settings.wheel_remainder[1]);
        }
        if (event->wheel_delta != 0 && ui_contains(&l.sidebar, event->mouse_x, event->mouse_y)) {
            settings.nav_first = ui_wheel_scroll_delta(settings.nav_first,
                SETTINGS_CATEGORY_COUNT,
                l.nav_rows, event->wheel_delta, 1u, &settings.wheel_remainder[0]);
        }
        if (pressed && ui_contains(&l.sidebar, event->mouse_x, event->mouse_y)) {
            if (event->mouse_y >= (int32_t)(l.sidebar.y + 4u)) {
                uint32_t page = settings.nav_first
                    + ((uint32_t)event->mouse_y - l.sidebar.y - 4u)
                        / SETTINGS_NAV_ROW_H;
                if (page < SETTINGS_CATEGORY_COUNT) { select_page(page); }
            }
        } else if (pressed && ui_contains(&l.apply, event->mouse_x, event->mouse_y)) {
            if (settings.page == BOOK8_PANEL_NETWORK) {
                network_apply(p, true);
            } else {
                apply(p);
            }
        } else if (pressed && ui_contains(&l.reset, event->mouse_x, event->mouse_y)) {
            if (settings.page == BOOK8_PANEL_NETWORK) {
                settings.network_draft = settings.network_status.settings;
                settings.draft = *book8_config_get();
                message("Unsaved network edits discarded.");
            } else {
                settings.draft = *book8_config_get();
                settings.clock24h_draft = settings.clock24h;
                load_time(p);
                message("Unsaved edits discarded.");
            }
        } else if (pressed && settings.page == BOOK8_PANEL_NETWORK
            && ui_contains(&l.renew, event->mouse_x, event->mouse_y)) {
            network_renew();
        } else if (pressed && ui_contains(&l.track, event->mouse_x, event->mouse_y)) {
            settings.dragging = l.total > l.rows ? -2 : -1;
            settings.scroll_grab = ui_scrollbar_grab(&l.track, l.total, l.rows,
                settings.first, event->mouse_y);
        } else if (pressed && ui_contains(&l.content, event->mouse_x, event->mouse_y)
            && event->mouse_y >= (int32_t)(l.content.y + 4u)) {
            uint32_t visual = ((uint32_t)event->mouse_y - l.content.y - 4u) / ROW_H;
            uint32_t row = settings.first + visual;
            if (visual < l.rows && row < l.fields) {
                settings.field = row;
                uint32_t local_y = ((uint32_t)event->mouse_y - l.content.y - 4u) % ROW_H;
                if (settings.page == BOOK8_PANEL_NETWORK) {
                    if (local_y >= 20u && row == 0u
                        && event->mouse_x >= (int32_t)(l.content.x + 24u)
                        && event->mouse_x < (int32_t)(l.content.x + l.content.width - 40u)) {
                        network_edit(row, 1);
                    } else if (local_y >= 20u && row >= 2u && row <= 6u
                        && event->mouse_x >= (int32_t)(l.content.x + 24u)
                        && event->mouse_x < (int32_t)(l.content.x + l.content.width - 40u)) {
                        uint32_t width = l.content.width > 72u
                            ? l.content.width - 72u : 0u;
                        uint32_t x = l.content.x + 4u;
                        uint32_t start = x + 24u
                            + (width > 128u ? (width - 128u) / 2u : 0u);
                        int32_t offset = event->mouse_x - (int32_t)start;
                        uint32_t octet = offset <= 0 ? 0u : (uint32_t)offset / 32u;
                        settings.network_octet = octet > 3u ? 3u : (uint8_t)octet;
                    } else if (local_y >= 20u
                        && event->mouse_x < (int32_t)(l.content.x + 24u)) {
                        network_edit(row, -1);
                    } else if (local_y >= 20u
                        && event->mouse_x >= (int32_t)(l.content.x + l.content.width - 40u)) {
                        network_edit(row, 1);
                    }
                } else if (settings.page == BOOK8_PANEL_SOUND && row == 4u) {
                    if (p->test_tone == NULL || !p->test_tone(settings.draft.values[BOOK8_CFG_TONE])) {
                        message("Alert not played (muted or no speaker hook).");
                    } else { message("PC speaker test started."); }
                } else if (settings.page == BOOK8_PANEL_SOUND && row == 3u) {
                    change(row, 1);
                } else if (settings.page == BOOK8_PANEL_TIME && row == 5u) {
                    change(row, 1);
                } else {
                    if (event->mouse_x < (int32_t)(l.content.x + 24u)) { change(row, -1); }
                    else if (event->mouse_x >= (int32_t)(l.content.x + l.content.width - 40u)) { change(row, 1); }
                    else {
                        uint32_t key;
                        if (config_key_for_row(row, &key)) {
                            if (slider((enum book8_config_key)key)) {
                                settings.dragging = (int32_t)row;
                            } else if (book8_config_ranges[key].max == 1u) {
                                change(row, settings.draft.values[key] == 0u ? 1 : -1);
                            }
                        }
                    }
                }
            }
        }
        if (down && settings.dragging == -2) {
            settings.first = ui_scrollbar_drag_position(&l.track, l.total, l.rows,
                event->mouse_y, settings.scroll_grab);
        } else if (down && settings.dragging >= 0) {
            adjust_slider(&l, (uint32_t)settings.dragging, event->mouse_x);
        }
    } else {
        switch (event->key) {
        case BOOK8GUI_KEY_PAGE_UP:
            select_page((settings.category + SETTINGS_CATEGORY_COUNT - 1u)
                % SETTINGS_CATEGORY_COUNT); break;
        case BOOK8GUI_KEY_PAGE_DOWN:
            select_page((settings.category + 1u) % SETTINGS_CATEGORY_COUNT); break;
        case BOOK8GUI_KEY_UP:
            if (settings.field != 0u) { --settings.field; } break;
        case BOOK8GUI_KEY_DOWN:
        case BOOK8GUI_KEY_TAB:
            if (l.total != 0u) { settings.field = (settings.field + 1u) % l.total; } break;
        case BOOK8GUI_KEY_LEFT: change(settings.field, -1); break;
        case BOOK8GUI_KEY_RIGHT: change(settings.field, 1); break;
        case BOOK8GUI_KEY_ENTER:
            if (settings.page == BOOK8_PANEL_NETWORK) {
                network_apply(p, true);
            } else if (settings.page == BOOK8_PANEL_SOUND && settings.field == 4u) {
                if (p->test_tone == NULL || !p->test_tone(settings.draft.values[BOOK8_CFG_TONE])) {
                    message("Alert not played (muted or no speaker hook).");
                } else { message("PC speaker test started."); }
            } else if (toggle_field(settings.field)) {
                change(settings.field, 1);
            } else { apply(p); }
            break;
        case 'r':
        case 'R':
            if (settings.page != BOOK8_PANEL_NETWORK) { return false; }
            network_renew();
            break;
        default: return false;
        }
        if (settings.field < settings.first) { settings.first = settings.field; }
        if (l.rows != 0u && settings.field >= settings.first + l.rows) {
            settings.first = settings.field - l.rows + 1u;
        }
    }
    book8gui_invalidate();
    return true;
}

static void *create(const struct book8gui_platform *p)
{
    settings = (struct settings_state){0};
    settings.dragging = -1;
    settings.page = BOOK8_PANEL_NETWORK;
    settings.draft = *book8_config_get();
    settings.network_draft = (struct book8_network_settings){
        .mode = BOOK8_NETWORK_DHCP,
        .retry_ms = NETWORK_RETRY_MIN
    };
    network_refresh_status();
    if (settings.network_status_valid) {
        settings.network_draft = settings.network_status.settings;
    } else {
        message("Network settings unavailable; check adapter status.");
    }
    uint32_t clock_format = 0u;
    if (reg_get_u32("HKLM\\SOFTWARE\\BOOK8\\Settings", "Clock24H",
            &clock_format) && clock_format <= 1u) {
        settings.clock24h = clock_format != 0u;
    }
    settings.clock24h_draft = settings.clock24h;
    load_time(p);
    return &settings;
}

const struct book8gui_app_class book8_settings_class = {
    .name = "System Settings",
    .default_width = 760u,
    .default_height = 520u,
    .single_instance = true,
    .wants_all_keys = true,
    .state_size = sizeof(struct settings_state),
    .refresh_1hz = true,
    .create = create,
    .render = render,
    .handle_event = handle_event
};
