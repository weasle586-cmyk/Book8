#include "book8gui.h"

#include <stddef.h>
#include "../include/stdio.h"

#include "apps/book8_editor.h"
#include "apps/book8_explorer.h"
#include "apps/book8_settings.h"
#include "apps/book8_shell.h"
#include "apps/book8_taskmgr.h"
#include "apps/book8_utilities.h"
#include "apps/book8_netshare.h"
#include "apps/book8_paintbook.h"
#include "apps/book8_browser.h"
#include "apps/book8_player.h"
#include "apps/book8_nvdiag.h"
#include "apps/book8_login.h"
#include "apps/book8_ui.h"
#include "../include/syscall.h"
#include "registry.h"
#include "session.h"
#define DESKTOP_BACKGROUND (book8_config_value(BOOK8_CFG_DARK) ? 0x202020u : 0x008080u)
#define DESKTOP_TITLE (book8_accent_colors[book8_config_value(BOOK8_CFG_ACCENT)])
#define DESKTOP_TITLE_TEXT 0xffffffu
#define DESKTOP_WINDOW (book8_config_value(BOOK8_CFG_DARK) ? 0x303030u : 0xc0c0c0u)
#define DESKTOP_TEXT (book8_config_value(BOOK8_CFG_DARK) ? 0xf0f0f0u : 0x000000u)
#define DESKTOP_LIGHT 0xffffffu
#define DESKTOP_SHADOW 0x808080u
#define DESKTOP_EDGE 0x000000u
#define DESKTOP_ACTIVE 0x000080u
#define DESKTOP_TASKBAR_HEIGHT BOOK8GUI_TASKBAR_HEIGHT
#define DESKTOP_TOP_HEIGHT 32u
#define DESKTOP_TITLEBAR_HEIGHT 26u
#define DESKTOP_START_WIDTH 92u
#define DESKTOP_TASK_WIDTH 126u
#define DESKTOP_STATUS_WIDTH 160u
#define DESKTOP_AUDIO_POPUP_WIDTH 132u
#define DESKTOP_AUDIO_POPUP_HEIGHT 164u
#define DESKTOP_CLOSE_SIZE 18u
#define DESKTOP_MENU_ROW 20u
#define DESKTOP_MENU_WIDTH 150u
#define DESKTOP_MENU_X 4u
#define DESKTOP_NO_SUBMENU 0xffu
#define DESKTOP_RESIZE_BORDER 5u
#define DESKTOP_MIN_WIDTH 240u
#define DESKTOP_MIN_HEIGHT 160u

/* Frozen UI geometry. QEMU tests derive click coordinates from these values
 * (tests/ui_geometry.py); a layout change must update that module too. */
_Static_assert(BOOK8GUI_TASKBAR_HEIGHT == 36u,
    "taskbar height is frozen at 36px");
_Static_assert(DESKTOP_TOP_HEIGHT == 32u,
    "desktop top height is frozen at 32px");
_Static_assert(DESKTOP_START_WIDTH == 92u,
    "start button width is frozen at 92px");
_Static_assert(DESKTOP_MENU_X == 4u, "menu x is frozen at 4px");
_Static_assert(DESKTOP_MENU_WIDTH == 150u, "menu width is frozen at 150px");
_Static_assert(DESKTOP_MENU_ROW == 20u, "menu row height is frozen at 20px");
_Static_assert(BOOK8GUI_MAX_PROGRAMS == BOOK8_MAX_PROGRAMS,
    "GUI program capacity is frozen to BOOK8_MAX_PROGRAMS");

#define DRAG_NONE 0u
#define DRAG_MOVE 0x10u
#define DRAG_LEFT 0x01u
#define DRAG_RIGHT 0x02u
#define DRAG_TOP 0x04u
#define DRAG_BOTTOM 0x08u

static const struct book8gui_menu_item programs_menu[] = {
    {"File Explorer", BOOK8GUI_MENU_ITEM, &book8_explorer_class, NULL, 0u, NULL},
    {"Command Shell", BOOK8GUI_MENU_ITEM, &book8_shell_class, NULL, 0u, NULL},
    {"NoteEditor", BOOK8GUI_MENU_ITEM, &book8_editor_class, NULL, 0u, NULL},
    {"Task Manager", BOOK8GUI_MENU_ITEM, &book8_taskmgr_class, NULL, 0u, NULL},
    {"HexViewer", BOOK8GUI_MENU_ITEM, NULL, NULL, 0u, "A:\\BIN\\HEXVIEW.ELF"},
    {"CodeEditor", BOOK8GUI_MENU_ITEM, NULL, NULL, 0u, "A:\\BIN\\CODEEDIT.ELF"},
    {"NetShare", BOOK8GUI_MENU_ITEM, NULL, NULL, 0u, "A:\\BIN\\NETSHARE.ELF"},
    {"CAT", BOOK8GUI_MENU_ITEM, NULL, NULL, 0u, "A:\\CAT.ELF"},
    {"BOOK8 Player", BOOK8GUI_MENU_ITEM, &book8_player_class, NULL, 0u, NULL}
};

#define PROGRAMS_MENU_COUNT (sizeof(programs_menu) / sizeof(programs_menu[0]))

static const struct book8gui_menu_item power_menu[] = {
    {"Shutdown", BOOK8GUI_MENU_SHUTDOWN, NULL, NULL, 0u, NULL},
    {"Reboot", BOOK8GUI_MENU_REBOOT, NULL, NULL, 0u, NULL},
    {"Exit to Prompt", BOOK8GUI_MENU_EXIT, NULL, NULL, 0u, NULL}
};

static const struct book8gui_menu_item start_menu[] = {
    {"Programs", BOOK8GUI_MENU_SUBMENU, NULL, programs_menu,
        PROGRAMS_MENU_COUNT, NULL},
    {"Settings", BOOK8GUI_MENU_ITEM, &book8_settings_class, NULL, 0u, NULL},
    {"Shutdown", BOOK8GUI_MENU_SUBMENU, NULL, power_menu,
        sizeof(power_menu) / sizeof(power_menu[0]), NULL}
};

#define START_MENU_COUNT (sizeof(start_menu) / sizeof(start_menu[0]))
_Static_assert(START_MENU_COUNT == 3u,
    "root menu is frozen at Programs/Settings/Shutdown (3 rows)");

struct desktop_state {
    const struct book8gui_platform *platform;
    struct WindowApp apps[BOOK8GUI_MAX_WINDOWS];
    uint32_t app_count;
    uint32_t next_id;
    uint32_t launch_count;
    uint32_t focused_id;
    uint32_t modal_id;
    uint8_t open_submenu;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t cursor_saved_x;
    int32_t cursor_saved_y;
    uint8_t mouse_buttons;
    bool launcher_open;
    bool menu_drag;
    int32_t menu_grab;
    int32_t menu_wheel_remainder;
    bool context_open;
    uint32_t context_x, context_y;
    bool dirty;
    bool cursor_dirty;
    bool cursor_saved;
    bool close_requested;
    uint32_t status_flags;
    uint32_t network_tx, network_rx;
    uint64_t tx_until, rx_until;
    bool network_valid, tx_lit, rx_lit;
    bool audio_available, audio_muted, audio_popup_open, audio_dragging;
    uint8_t audio_volume;
    struct book8gui_time time;
    bool time_valid;
    uint32_t drag_id;
    uint32_t close_request_id;
    uint8_t drag_mode;
    int32_t drag_origin_x;
    int32_t drag_origin_y;
    struct book8gui_rect drag_bounds;
    uint64_t busy_ticks;
    uint64_t sample_start;
    uint64_t session_start;
    uint32_t desktop_cpu_permille;
    uint64_t last_disk_read;
    uint64_t last_disk_write;
    uint64_t last_disk_busy;
    struct book8gui_telemetry telemetry;
};

static struct desktop_state desktop;
static struct book8gui_platform active_platform;
static struct book8gui_program registered_programs[BOOK8GUI_MAX_PROGRAMS];
static uint32_t registered_count, programs_first;
static bool registry_programs_active;
static char programs_status[128];
static void program_report(const char *text);
static bool path_equal(const char *a, const char *b);
static bool set_mouse_position(int32_t x, int32_t y);
static void draw_pointer(void);
static uint32_t program_entries_start(void)
{
    return registry_programs_active ? 0u : 5u;
}

static struct book8gui_rect audio_icon_rect(void);
static struct book8gui_rect audio_popup_rect(void);
static struct book8gui_rect audio_slider_rect(
    const struct book8gui_rect *popup);
static struct book8gui_rect audio_mute_rect(
    const struct book8gui_rect *popup);
static void draw_audio_icon(void);
static void draw_audio_popup(void);
static struct { uint32_t kind; char path[256]; } pending_utility;

bool book8gui_decode_wheel_message(struct book8gui_event *event,
    uint32_t message, uintptr_t wparam, int32_t x, int32_t y)
{
    if (message != BOOK8_WM_MOUSEWHEEL) { return false; }
    *event = (struct book8gui_event){.type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = x, .mouse_y = y, .wheel_delta = book8_wheel_wparam(wparam),
        .mouse_buttons = (uint8_t)((wparam & 3u) | ((wparam & 0x10u) >> 2u)
            | ((wparam & 0x60u) >> 2u))};
    return true;
}

static const char *utility_path(uint32_t kind)
{
    return kind == BOOK8_GUI_HEXVIEW ? "A:\\BIN\\HEXVIEW.ELF"
        : kind == BOOK8_GUI_CODEEDIT ? "A:\\BIN\\CODEEDIT.ELF"
        : kind == BOOK8_GUI_NETSHARE ? "A:\\BIN\\NETSHARE.ELF"
        : kind == BOOK8_GUI_PAINTBOOK ? "A:\\BIN\\PAINTBK.ELF"
        : kind == BOOK8_GUI_BROWSER ? "A:\\BIN\\BROWSER.ELF"
        : kind == BOOK8_GUI_NVDIAG ? "A:\\BIN\\NVDIAG.ELF"
        : kind == BOOK8_GUI_PLAYER ? "A:\\BIN\\PLAYER.ELF" : NULL;
}

long book8gui_queue_utility(uint32_t kind, const char *path)
{
    if (utility_path(kind) == NULL || path == NULL) { return -BOOK8_EINVAL; }
    if (kind == BOOK8_GUI_NETSHARE && path[0] != '\0') { return -BOOK8_EINVAL; }
    if (pending_utility.kind != 0u) { return -BOOK8_EAGAIN; }
    uint32_t n = 0u;
    while (n < sizeof(pending_utility.path) && path[n] != '\0') { ++n; }
    if (n == sizeof(pending_utility.path)) { return -BOOK8_EINVAL; }
    snprintf(pending_utility.path, sizeof(pending_utility.path), "%s", path);
    pending_utility.kind = kind;
    return 0;
}

bool book8gui_utility_pending(void) { return pending_utility.kind != 0u; }
void book8gui_cancel_pending_utility(void) { pending_utility.kind = 0u; }

bool book8gui_utility_installed(uint32_t kind)
{
    const char *path = utility_path(kind);
    if (path == NULL) { return false; }
    for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
        if (path_equal(registered_programs[i].path, path)) { return true; }
    }
    return false;
}

bool book8gui_open_utility(uint32_t kind, const char *document)
{
    if (desktop.platform->launch_utility == NULL || !book8gui_utility_installed(kind)) {
        program_report("GUI utility is not installed or the ELF launcher is unavailable.");
        return false;
    }
    return desktop.platform->launch_utility(kind, document);
}

bool book8gui_open_pending_utility(void)
{
    uint32_t kind = pending_utility.kind;
    if (kind == 0u) { return true; }
    pending_utility.kind = 0u;
    const struct book8gui_app_class *cls = kind == BOOK8_GUI_HEXVIEW
        ? &book8_hexview_class : kind == BOOK8_GUI_CODEEDIT
        ? &book8_codeedit_class : kind == BOOK8_GUI_NETSHARE
        ? &book8_netshare_class : kind == BOOK8_GUI_PAINTBOOK
        ? &book8_paintbook_class : kind == BOOK8_GUI_BROWSER
        ? &book8_browser_class : kind == BOOK8_GUI_NVDIAG
        ? &book8_nvdiag_class : &book8_player_class;
    struct WindowApp *app = book8gui_launch_class(cls);
    if (app == NULL) { program_report("Utility window limit reached."); return false; }
    if (pending_utility.path[0] != '\0') {
        if (cls->open_path == NULL) { program_report("This utility does not accept document paths."); return false; }
        return cls->open_path(desktop.platform, app, pending_utility.path);
    }
    return true;
}

static bool path_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') { ca -= 'a' - 'A'; }
        if (cb >= 'a' && cb <= 'z') { cb -= 'a' - 'A'; }
        if (ca == '/') { ca = '\\'; }
        if (cb == '/') { cb = '\\'; }
        if (ca != cb) { return false; }
    }
    return *a == *b;
}

static bool static_program_path(const char *path)
{
    for (uint32_t i = 0u; i < PROGRAMS_MENU_COUNT; ++i) {
        if (programs_menu[i].executable_path != NULL
            && path_equal(path, programs_menu[i].executable_path)) { return true; }
    }
    return false;
}

static uint32_t programs_total(void)
{
    if (registry_programs_active) {
        uint32_t count = 0u;
        for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
            if (!registered_programs[i].hidden) { ++count; }
        }
        return count;
    }
    uint32_t count = PROGRAMS_MENU_COUNT;
    for (uint32_t i = 5u; i < registered_count; ++i) {
        if (!static_program_path(registered_programs[i].path)) { ++count; }
    }
    return count;
}

/* Discovered applications follow the fixed rows without duplicate targets. */
static uint32_t menu_program_index(uint32_t row)
{
    if (registry_programs_active) {
        for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
            if (registered_programs[i].hidden) { continue; }
            if (row == 0u) { return i; }
            --row;
        }
        return registered_count;
    }
    row -= PROGRAMS_MENU_COUNT;
    for (uint32_t i = 5u; i < registered_count; ++i) {
        if (static_program_path(registered_programs[i].path)) { continue; }
        if (row == 0u) { return i; }
        --row;
    }
    return registered_count;
}

static void program_report(const char *text)
{
    snprintf(programs_status, sizeof(programs_status), "%s", text);
    if (desktop.platform->diagnostic != NULL) {
        desktop.platform->diagnostic(text);
        desktop.platform->diagnostic("\n");
    }
}

static bool system_directory(const char *path)
{
    static const char *const protected[] = {
        "\\BOOT\\", "\\DRIVERS\\", "\\SYSTEM\\", "\\EFI\\", "\\MODULES\\", "\\KERNEL\\"
    };
    for (uint32_t i = 0u; i < sizeof(protected) / sizeof(protected[0]); ++i) {
        char prefix[16];
        uint32_t n = 0u;
        while (protected[i][n] != '\0') { ++n; }
        uint32_t j = 0u;
        while (path[j] != '\0' && j < n) { prefix[j] = path[j]; ++j; }
        prefix[j] = '\0';
        if (path_equal(prefix, protected[i])) { return true; }
    }
    return false;
}

static bool infrastructure_binary(const char *name)
{
    static const char *const excluded[] = {
        "NVMAP.ELF", "BOOK8.ELF", "KERNEL.ELF", "BOOK8GUI.ELF", "SHELL.ELF"
    };
    for (uint32_t i = 0u; i < sizeof(excluded) / sizeof(excluded[0]); ++i) {
        if (path_equal(name, excluded[i])) { return true; }
    }
    return false;
}

#define PROGRAM_SCAN_MAX_DIRECTORIES 512u
#define PROGRAM_SCAN_MAX_DEPTH 32u

struct program_scan {
    char directories[PROGRAM_SCAN_MAX_DIRECTORIES][BOOK8GUI_PROGRAM_PATH_SIZE];
    uint8_t depths[PROGRAM_SCAN_MAX_DIRECTORIES];
    uint32_t count, current;
    bool incomplete;
    uint32_t invalid_names, unavailable_metadata, skipped_drives;
    char failed_directory[BOOK8GUI_PROGRAM_PATH_SIZE];
    char failure_reason[48];
};
static struct program_scan program_scan;

static bool scan_program(const char *name, bool directory, uint32_t size,
    void *context)
{
    (void)size;
    struct program_scan *scan = context;
    if (name == NULL || name[0] == '\0') { return true; }
    if (path_equal(name, ".") || path_equal(name, "..")) { return true; }
    uint32_t length = 0u;
    while (length < 256u && name[length] != '\0') {
        unsigned char ch = (unsigned char)name[length];
        if (ch < 32u || ch == '\\' || ch == '/' || ch == ':' || ch == '*'
            || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ++scan->invalid_names;
            return true;
        }
        ++length;
    }
    if (length == 256u) { ++scan->invalid_names; return true; }
    char path[BOOK8GUI_PROGRAM_PATH_SIZE];
    int n = snprintf(path, sizeof(path), "%s%s%s",
        scan->directories[scan->current], name, directory ? "\\" : "");
    if (n < 0 || (uint32_t)n >= sizeof(path)) {
        scan->incomplete = true;
        return true;
    }
    if (directory) {
        if (system_directory(path + 2u)) { return true; }
        if (scan->count == PROGRAM_SCAN_MAX_DIRECTORIES
            || scan->depths[scan->current] >= PROGRAM_SCAN_MAX_DEPTH) {
            scan->incomplete = true;
        } else {
            snprintf(scan->directories[scan->count], sizeof(scan->directories[scan->count]), "%s", path);
            scan->depths[scan->count++] = scan->depths[scan->current] + 1u;
        }
        return true;
    }
    if (length < 5u || !path_equal(name + length - 4u, ".ELF")) { return true; }
    if (infrastructure_binary(name)) { return true; }
    for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
        if (path_equal(registered_programs[i].path, path)) { return true; }
    }
    if (registered_count == BOOK8GUI_MAX_PROGRAMS) {
        scan->incomplete = true;
        return true;
    }
    struct book8gui_program *entry = &registered_programs[registered_count++];
    *entry = (struct book8gui_program){0};
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    if (path_equal(path, utility_path(BOOK8_GUI_HEXVIEW))) {
        snprintf(entry->name, sizeof(entry->name), "Hex Viewer");
    } else if (path_equal(path, utility_path(BOOK8_GUI_CODEEDIT))) {
        snprintf(entry->name, sizeof(entry->name), "Code Editor");
    } else if (path_equal(path, utility_path(BOOK8_GUI_NETSHARE))) {
        snprintf(entry->name, sizeof(entry->name), "Network Share Browser");
    } else if (path_equal(path, utility_path(BOOK8_GUI_PAINTBOOK))) {
        snprintf(entry->name, sizeof(entry->name), "PaintBook");
    } else if (path_equal(path, utility_path(BOOK8_GUI_BROWSER))) {
        snprintf(entry->name, sizeof(entry->name), "Web Browser");
    } else if (path_equal(path, utility_path(BOOK8_GUI_PLAYER))) {
        snprintf(entry->name, sizeof(entry->name), "BOOK8 Player");
    } else if (path_equal(path, utility_path(BOOK8_GUI_NVDIAG))) {
        snprintf(entry->name, sizeof(entry->name), "NV Diagnostic");
    } else if (path_equal(name, "EXPLORER.ELF")) {
        snprintf(entry->name, sizeof(entry->name), "File Explorer");
    } else if (path_equal(name, "TEXTEDIT.ELF") || path_equal(name, "EDITOR.ELF")) {
        snprintf(entry->name, sizeof(entry->name), "Text Editor");
    } else if (path_equal(name, "IMGVIEW.ELF")) {
        snprintf(entry->name, sizeof(entry->name), "Image Viewer");
    }
    return true;
}

static const struct book8gui_app_class *program_class(const char *name)
{
    if (path_equal(name, "explorer")) { return &book8_explorer_class; }
    if (path_equal(name, "shell")) { return &book8_shell_class; }
    if (path_equal(name, "editor")) { return &book8_editor_class; }
    if (path_equal(name, "taskmgr")) { return &book8_taskmgr_class; }
    if (path_equal(name, "hexview")) { return &book8_hexview_class; }
    if (path_equal(name, "codeedit")) { return &book8_codeedit_class; }
    if (path_equal(name, "netshare")) { return &book8_netshare_class; }
    if (path_equal(name, "paintbook") || path_equal(name, "paint")) { return &book8_paintbook_class; }
    if (path_equal(name, "browser")) { return &book8_browser_class; }
    if (path_equal(name, "player")) { return &book8_player_class; }
    if (path_equal(name, "nvdiag")) { return &book8_nvdiag_class; }
    return NULL;
}

const struct book8gui_app_class *book8gui_app_class_by_name(const char *name)
{
    return name != NULL ? program_class(name) : NULL;
}

struct registry_program_context {
    bool incomplete;
};

static bool import_registry_program(const char *key, void *context)
{
    struct registry_program_context *state = context;
    char registry_path[BOOK8_REGISTRY_KEY_MAX];
    char name[sizeof(registered_programs[0].name)];
    char path[BOOK8GUI_PROGRAM_PATH_SIZE];
    char app_class[BOOK8_REGISTRY_VALUE_MAX];
    uint32_t hidden = 0u, system_component = 0u;
    int length = snprintf(registry_path, sizeof(registry_path),
        "HKLM\\SOFTWARE\\BOOK8\\Programs\\%s", key);
    if (length < 0 || (size_t)length >= sizeof(registry_path)
        || !reg_get_str(registry_path, "Name", name, sizeof(name))) {
        return true;
    }
    path[0] = '\0';
    app_class[0] = '\0';
    (void)reg_get_str(registry_path, "Path", path, sizeof(path));
    (void)reg_get_str(registry_path, "AppClass", app_class,
        sizeof(app_class));
    (void)reg_get_u32(registry_path, "Hidden", &hidden);
    (void)reg_get_u32(registry_path, "SystemComponent", &system_component);
    if (name[0] == '\0'
        || (path[0] == '\0' && program_class(app_class) == NULL)) {
        return true;
    }
    if (registered_count == BOOK8GUI_MAX_PROGRAMS) {
        state->incomplete = true;
        return false;
    }
    struct book8gui_program *entry = &registered_programs[registered_count++];
    *entry = (struct book8gui_program){0};
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    snprintf(entry->registry_key, sizeof(entry->registry_key), "%s",
        registry_path);
    entry->launch_class = program_class(app_class);
    entry->hidden = hidden != 0u;
    entry->system_component = system_component != 0u || entry->path[0] == '\0';
    if (entry->path[0] != '\0' && desktop.platform->file_info != NULL) {
        struct book8_file_info info = {0};
        if (!desktop.platform->file_info(entry->path, &info)
            || info.directory) {
            --registered_count;
            return true;
        }
        entry->allocated_bytes = info.allocated_bytes;
        entry->allocation_valid = info.allocation_valid;
    }
    return true;
}

static char program_fold(char value)
{
    return value >= 'a' && value <= 'z'
        ? (char)(value - ('a' - 'A')) : value;
}

static bool program_before(const struct book8gui_program *left,
    const struct book8gui_program *right)
{
    size_t i = 0u;
    while (left->name[i] != '\0' && right->name[i] != '\0') {
        char a = program_fold(left->name[i]);
        char b = program_fold(right->name[i]);
        if (a != b) {
            return a < b;
        }
        ++i;
    }
    if (left->name[i] != right->name[i]) {
        return left->name[i] == '\0';
    }
    return false;
}

static void sort_programs(void)
{
    uint32_t first = program_entries_start();
    for (uint32_t i = first + 1u; i < registered_count; ++i) {
        struct book8gui_program current = registered_programs[i];
        uint32_t position = i;
        while (position > first
            && program_before(&current, &registered_programs[position - 1u])) {
            registered_programs[position] = registered_programs[position - 1u];
            --position;
        }
        registered_programs[position] = current;
    }
}

bool book8gui_refresh_programs(void)
{
    static const char *const names[] = {
        "File Explorer", "Command Shell", "NoteEditor", "Task Manager", "System Settings"
    };
    registered_count = 0u;
    registry_programs_active = false;
    programs_first = 0u;
    desktop.menu_drag = false;
    desktop.menu_wheel_remainder = 0;
    programs_status[0] = '\0';
    struct registry_program_context registry_context = {0};
    if (reg_enum_keys("HKLM\\SOFTWARE\\BOOK8\\Programs",
            import_registry_program, &registry_context) != 0u) {
        registry_programs_active = true;
    } else {
        struct book8_file_info core_info = {0};
        bool core_metadata = desktop.platform->file_info != NULL
            && desktop.platform->file_info("A:\\BOOT\\BOOK8.ELF", &core_info);
        for (uint32_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
            struct book8gui_program *entry = &registered_programs[registered_count++];
            *entry = (struct book8gui_program){.system_component = true};
            snprintf(entry->name, sizeof(entry->name), "%s", names[i]);
            snprintf(entry->path, sizeof(entry->path), "A:\\BOOT\\BOOK8.ELF");
            if (core_metadata) {
                entry->allocated_bytes = core_info.allocated_bytes;
                entry->allocation_valid = core_info.allocation_valid;
            }
        }
    }
    if (registry_programs_active) {
        sort_programs();
    }
    if (desktop.platform->launch_program == NULL) {
        book8gui_invalidate();
        return true;
    }
    program_scan = (struct program_scan){0};
    uint32_t drives = desktop.platform->drive_count != NULL
        ? desktop.platform->drive_count() : 1u;
    if (drives > 26u) { drives = 26u; program_scan.incomplete = true; }
    for (uint32_t i = 0u; i < drives; ++i) {
        if (desktop.platform->program_drive_readable != NULL
            && !desktop.platform->program_drive_readable(i)) {
            ++program_scan.skipped_drives;
            continue;
        }
        if (desktop.platform->drive_readable != NULL
            && !desktop.platform->drive_readable(i)) {
            ++program_scan.skipped_drives;
            continue;
        }
        char letter = desktop.platform->drive_letter != NULL
            ? desktop.platform->drive_letter(i) : 'A';
        if (letter >= 'a' && letter <= 'z') { letter -= 'a' - 'A'; }
        if (letter < 'A' || letter > 'Z') { ++program_scan.invalid_names; continue; }
        snprintf(program_scan.directories[program_scan.count], sizeof(program_scan.directories[program_scan.count]),
            "%c:\\", letter);
        ++program_scan.count;
    }
    while (program_scan.current < program_scan.count) {
        if (!desktop.platform->list_directory(
            program_scan.directories[program_scan.current], scan_program, &program_scan)) {
            program_scan.incomplete = true;
            if (program_scan.failed_directory[0] == '\0') {
                snprintf(program_scan.failed_directory, sizeof(program_scan.failed_directory),
                    "%s", program_scan.directories[program_scan.current]);
                const char *reason = desktop.platform->last_error_text != NULL
                    ? desktop.platform->last_error_text() : NULL;
                snprintf(program_scan.failure_reason, sizeof(program_scan.failure_reason),
                    "%s", reason != NULL ? reason : "directory unreadable");
            }
        }
        ++program_scan.current;
    }
    /* Directory iteration shares the filesystem sector buffer with file_info. */
    for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
        struct book8gui_program *entry = &registered_programs[i];
        if (entry->path[0] == '\0') { continue; }
        struct book8_file_info info = {0};
        if (desktop.platform->file_info != NULL
            && desktop.platform->file_info(entry->path, &info)) {
            entry->allocated_bytes = info.allocated_bytes;
            entry->allocation_valid = info.allocation_valid;
            entry->system_component |= info.system || info.read_only;
        } else {
            entry->system_component |= desktop.platform->file_info != NULL;
            ++program_scan.unavailable_metadata;
        }
    }
    if (registry_programs_active) {
        sort_programs();
    }
    /* Publish the actual Programs menu rows so QEMU and host tests can click
     * by label instead of by a hardcoded index. Adding or removing an ELF
     * changes these rows, so the diagnostic must follow the same iteration
     * order as menu_program_index(). */
    if (desktop.platform->diagnostic != NULL) {
        char row_line[192];
        uint32_t row = 0u;
        if (registry_programs_active) {
            /* Registry mode replaces the static menu: rows are all non-hidden
             * registry entries, in the order used by menu_program_index(). */
            for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
                const struct book8gui_program *entry = &registered_programs[i];
                if (entry->hidden) { continue; }
                snprintf(row_line, sizeof(row_line), "PROGRAM-MENU-ROW: %u|%.64s|%s",
                         row, entry->name, entry->path);
                desktop.platform->diagnostic(row_line);
                desktop.platform->diagnostic("\n");
                ++row;
            }
        } else {
            for (; row < PROGRAMS_MENU_COUNT; ++row) {
                snprintf(row_line, sizeof(row_line), "PROGRAM-MENU-ROW: %u|%s|%s",
                         row, programs_menu[row].label,
                         programs_menu[row].executable_path != NULL
                             ? programs_menu[row].executable_path : "");
                desktop.platform->diagnostic(row_line);
                desktop.platform->diagnostic("\n");
            }
            for (uint32_t i = 5u; i < registered_count; ++i) {
                const struct book8gui_program *entry = &registered_programs[i];
                if (static_program_path(entry->path)) { continue; }
                snprintf(row_line, sizeof(row_line), "PROGRAM-MENU-ROW: %u|%.64s|%s",
                         row, entry->name, entry->path);
                desktop.platform->diagnostic(row_line);
                desktop.platform->diagnostic("\n");
                ++row;
            }
        }
    }
    if (program_scan.incomplete) {
        char report[128];
        if (program_scan.failed_directory[0] != '\0') {
            snprintf(report, sizeof(report), "Programs scan incomplete: %.64s: %.24s",
                program_scan.failed_directory, program_scan.failure_reason);
        } else {
            snprintf(report, sizeof(report), "Programs scan incomplete: path, directory depth or registry limit reached.");
        }
        program_report(report);
    } else {
        char report[128];
        snprintf(report, sizeof(report),
            "Programs indexed: %u; metadata unavailable: %u; invalid names skipped: %u; unreadable drives skipped: %u.",
            registered_count, program_scan.unavailable_metadata, program_scan.invalid_names,
            program_scan.skipped_drives);
        program_report(report);
    }
    book8gui_invalidate();
    return !program_scan.incomplete;
}

uint32_t book8gui_program_count(void) { return registered_count; }
const char *book8gui_program_status(void) { return programs_status; }

bool book8gui_program_info(uint32_t index, struct book8gui_program *out)
{
    if (index >= registered_count || out == NULL) { return false; }
    *out = registered_programs[index];
    return true;
}

bool book8gui_remove_program(const char *path)
{
    if (path == NULL) {
        program_report("Remove refused: missing executable path.");
        return false;
    }
    for (uint32_t i = program_entries_start(); i < registered_count; ++i) {
        struct book8gui_program *entry = &registered_programs[i];
        if (!path_equal(path, entry->path)) { continue; }
        for (uint32_t j = 0u; j < desktop.app_count; ++j) {
            const struct book8gui_app_class *cls = desktop.apps[j].cls;
            if ((path_equal(path, utility_path(BOOK8_GUI_HEXVIEW)) && cls == &book8_hexview_class)
                || (path_equal(path, utility_path(BOOK8_GUI_CODEEDIT)) && cls == &book8_codeedit_class)
                || (path_equal(path, utility_path(BOOK8_GUI_NETSHARE)) && cls == &book8_netshare_class)
                || (path_equal(path, utility_path(BOOK8_GUI_PAINTBOOK)) && cls == &book8_paintbook_class)
                || (path_equal(path, utility_path(BOOK8_GUI_BROWSER)) && cls == &book8_browser_class)
                || (path_equal(path, utility_path(BOOK8_GUI_PLAYER)) && cls == &book8_player_class)
                || (path_equal(path, utility_path(BOOK8_GUI_NVDIAG)) && cls == &book8_nvdiag_class)) {
                program_report("Close this utility's windows before removing its executable.");
                return false;
            }
        }
        struct book8_file_info info;
        if (entry->system_component || desktop.platform->file_info == NULL
            || !desktop.platform->file_info(path, &info)
            || info.directory || info.system || info.read_only) {
            program_report("Remove refused: System Component or unreadable file attributes.");
            return false;
        }
        if (desktop.platform->delete_entry == NULL
            || !desktop.platform->delete_entry(path)) {
            program_report(desktop.platform->last_error_text != NULL
                ? desktop.platform->last_error_text() : "Remove failed: executable was not deleted.");
            return false;
        }
        bool registry_update_failed = false;
        if (entry->registry_key[0] != '\0') {
            registry_update_failed = !reg_delete_tree(entry->registry_key)
                || !reg_save();
        }
        for (uint32_t j = i; j + 1u < registered_count; ++j) {
            registered_programs[j] = registered_programs[j + 1u];
        }
        --registered_count;
        programs_first = 0u;
        program_report(registry_update_failed
            ? "Program removed, but the registry shortcut could not be saved."
            : "Program removed; Programs shortcut unregistered.");
        book8gui_invalidate();
        return true;
    }
    program_report("Remove refused: program is not a registered user application.");
    return false;
}
static struct {
    char source[512];
    char status[128];
    bool cut;
} clipboard;
static struct {
    bool active, restore_failed;
    uint32_t old_width, old_height, seconds;
    uint64_t started;
    uint8_t previous_buttons;
    struct book8_system_config proposed;
    struct book8gui_rect bounds[BOOK8GUI_MAX_WINDOWS];
    struct book8gui_rect restored[BOOK8GUI_MAX_WINDOWS];
    char status[96];
} display_preview;

const char *book8gui_display_status(void)
{
    return display_preview.status;
}

static void display_report(const char *text)
{
    snprintf(display_preview.status, sizeof(display_preview.status), "%s", text);
    if (desktop.platform->diagnostic != NULL) {
        desktop.platform->diagnostic(text);
    }
}

static uint64_t ticks_now(void)
{
    return desktop.platform->clock_ticks != NULL
        ? desktop.platform->clock_ticks() : 0u;
}

static struct book8gui_rect confirmation_box(void)
{
    uint32_t width = desktop.platform->width < 560u
        ? desktop.platform->width - 16u : 544u;
    return (struct book8gui_rect){
        (desktop.platform->width - width) / 2u,
        (desktop.platform->height - 140u) / 2u, width, 140u
    };
}

static struct book8gui_rect confirmation_button(bool keep)
{
    struct book8gui_rect box = confirmation_box();
    return (struct book8gui_rect){box.x + (keep ? 16u : box.width - 156u),
        box.y + 96u, 140u, 28u};
}

static void draw_display_confirmation(void)
{
    if (!display_preview.active) { return; }
    struct book8gui_rect box = confirmation_box();
    desktop.platform->fill_rect(box.x, box.y, box.width, box.height, DESKTOP_EDGE);
    desktop.platform->fill_rect(box.x + 2u, box.y + 2u, box.width - 4u,
        box.height - 4u, DESKTOP_WINDOW);
    char text[96];
    snprintf(text, sizeof(text), "Reverting in [%02u] seconds.", display_preview.seconds);
    desktop.platform->draw_text(box.x + 16u, box.y + 16u,
        "Keep this display setting?", DESKTOP_TEXT, DESKTOP_WINDOW);
    desktop.platform->draw_text(box.x + 16u, box.y + 40u,
        display_preview.restore_failed ? "Restore failed. Enter retries restore." : text,
        DESKTOP_TEXT, DESKTOP_WINDOW);
    for (uint32_t i = 0u; i < 2u; ++i) {
        struct book8gui_rect b = confirmation_button(i == 0u);
        desktop.platform->fill_rect(b.x, b.y, b.width, b.height, DESKTOP_SHADOW);
        desktop.platform->draw_text(b.x + 8u, b.y + 6u,
            i == 0u ? (display_preview.restore_failed ? "Retry Restore" : "Keep Changes") : "Revert",
            DESKTOP_TITLE_TEXT, DESKTOP_SHADOW);
    }
}

static void fit_display_rect(struct book8gui_rect *rect, uint32_t width,
    uint32_t height)
{
    uint32_t work_height = height - DESKTOP_TOP_HEIGHT - DESKTOP_TASKBAR_HEIGHT;
    if (rect->width > width) { rect->width = width; }
    if (rect->height > work_height) { rect->height = work_height; }
    if (rect->x + rect->width > width) { rect->x = width - rect->width; }
    if (rect->y < DESKTOP_TOP_HEIGHT) { rect->y = DESKTOP_TOP_HEIGHT; }
    if (rect->y + rect->height > height - DESKTOP_TASKBAR_HEIGHT) {
        rect->y = height - DESKTOP_TASKBAR_HEIGHT - rect->height;
    }
}

static struct book8gui_rect maximized_bounds(void)
{
    return (struct book8gui_rect){0u, 0u, desktop.platform->width,
        desktop.platform->height - DESKTOP_TASKBAR_HEIGHT};
}

static bool refresh_display_size(void)
{
    if (desktop.platform->display_size == NULL) {
        return true;
    }
    uint32_t width, height;
    desktop.platform->display_size(&width, &height);
    if (width < 640u || height < 300u || width > INT32_MAX || height > INT32_MAX) {
        display_report("BOOK8-DISPLAY: invalid active screen dimensions.");
        return false;
    }
    if (width == active_platform.width && height == active_platform.height) {
        return true;
    }
    active_platform.width = width;
    active_platform.height = height;
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        struct WindowApp *app = &desktop.apps[i];
        fit_display_rect(&app->restore_bounds, width, height);
        if (app->maximized) {
            app->bounds = maximized_bounds();
        } else {
            fit_display_rect(&app->bounds, width, height);
        }
    }
    desktop.drag_mode = DRAG_NONE;
    book8gui_invalidate();
    return true;
}

enum book8_display_request book8gui_preview_display(
    const struct book8_system_config *config)
{
    if (desktop.platform == NULL || desktop.platform->set_display_mode == NULL) {
        return BOOK8_DISPLAY_UNAVAILABLE;
    }
    if (display_preview.active || !book8_config_valid(config)
        || desktop.platform->clock_ticks == NULL || desktop.platform->clock_hz == 0u
        || desktop.platform->save_config == NULL) {
        display_report("BOOK8-DISPLAY: preview requires timer and config storage.");
        return BOOK8_DISPLAY_FAILED;
    }
    static const uint32_t widths[] = {800u, 1024u, 1440u};
    static const uint32_t heights[] = {600u, 768u, 900u};
    uint32_t index = config->values[BOOK8_CFG_RESOLUTION];
    display_preview.old_width = desktop.platform->width;
    display_preview.old_height = desktop.platform->height;
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        display_preview.bounds[i] = desktop.apps[i].bounds;
        display_preview.restored[i] = desktop.apps[i].restore_bounds;
    }
    if (!desktop.platform->set_display_mode(widths[index], heights[index])) {
        display_report("BOOK8-DISPLAY: mode change failed; original mode unchanged.");
        return BOOK8_DISPLAY_FAILED;
    }
    active_platform.width = widths[index];
    active_platform.height = heights[index];
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        struct WindowApp *app = &desktop.apps[i];
        fit_display_rect(&app->bounds, widths[index], heights[index]);
        fit_display_rect(&app->restore_bounds, widths[index], heights[index]);
        if (app->maximized) {
            app->bounds = maximized_bounds();
        }
    }
    if (desktop.mouse_x >= (int32_t)widths[index]) { desktop.mouse_x = (int32_t)widths[index] - 1; }
    if (desktop.mouse_y >= (int32_t)heights[index]) { desktop.mouse_y = (int32_t)heights[index] - 1; }
    desktop.drag_mode = DRAG_NONE;
    desktop.launcher_open = false;
    display_preview.proposed = *config;
    display_preview.started = ticks_now();
    display_preview.seconds = 30u;
    display_preview.previous_buttons = desktop.mouse_buttons;
    display_preview.restore_failed = false;
    display_preview.active = true;
    display_preview.status[0] = '\0';
    desktop.dirty = true;
    desktop.cursor_dirty = false;
    desktop.cursor_saved = false;
    return BOOK8_DISPLAY_STARTED;
}

static void revert_display(void)
{
    if (!desktop.platform->set_display_mode(display_preview.old_width,
        display_preview.old_height)) {
        display_preview.restore_failed = true;
        display_preview.seconds = 0u;
        display_report("BOOK8-DISPLAY: restore failed; retry required.");
        desktop.dirty = true;
        return;
    }
    active_platform.width = display_preview.old_width;
    active_platform.height = display_preview.old_height;
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        desktop.apps[i].bounds = display_preview.bounds[i];
        desktop.apps[i].restore_bounds = display_preview.restored[i];
    }
    display_preview.active = false;
    if (display_preview.restore_failed) {
        display_report("BOOK8-DISPLAY: previous mode restored.");
        display_preview.restore_failed = false;
    }
    desktop.dirty = true;
}

static void confirm_display(void)
{
    if (display_preview.restore_failed) {
        revert_display();
        return;
    }
    if (!desktop.platform->save_config(&display_preview.proposed)) {
        display_report("BOOK8-DISPLAY: config save failed; reverting.");
        revert_display();
        return;
    }
    (void)book8_config_commit(&display_preview.proposed);
    display_preview.active = false;
    display_report("BOOK8-DISPLAY: changes kept and saved.");
    desktop.dirty = true;
}

static bool display_timer(void)
{
    if (!display_preview.active || display_preview.restore_failed) { return false; }
    uint64_t elapsed = ticks_now() - display_preview.started;
    uint64_t limit = (uint64_t)desktop.platform->clock_hz * 30u;
    if (elapsed >= limit) {
        display_report("BOOK8-DISPLAY: confirmation timed out; reverting.");
        revert_display();
        return true;
    }
    uint32_t remaining = 30u - (uint32_t)(elapsed / desktop.platform->clock_hz);
    if (remaining != display_preview.seconds) {
        display_preview.seconds = remaining;
        desktop.dirty = true;
    }
    return false;
}

static void display_event(const struct book8gui_event *event)
{
    if (event->type == BOOK8GUI_EVENT_KEY) {
        if (event->key == BOOK8GUI_KEY_ENTER) { confirm_display(); }
        else if (event->key == BOOK8GUI_KEY_ESCAPE) {
            display_report("BOOK8-DISPLAY: changes cancelled; reverting.");
            revert_display();
        }
        return;
    }
    bool pressed = (event->mouse_buttons & 1u) != 0u
        && (display_preview.previous_buttons & 1u) == 0u;
    display_preview.previous_buttons = event->mouse_buttons;
    (void)set_mouse_position(event->mouse_x, event->mouse_y);
    bool buttons_changed = desktop.mouse_buttons != event->mouse_buttons;
    desktop.mouse_buttons = event->mouse_buttons;
    if (buttons_changed) {
        book8gui_invalidate();
    }
    if (!pressed) { return; }
    for (uint32_t i = 0u; i < 2u; ++i) {
        struct book8gui_rect b = confirmation_button(i == 0u);
        if (event->mouse_x >= (int32_t)b.x && event->mouse_y >= (int32_t)b.y
            && event->mouse_x < (int32_t)(b.x + b.width)
            && event->mouse_y < (int32_t)(b.y + b.height)) {
            if (i == 0u) { confirm_display(); }
            else {
                display_report("BOOK8-DISPLAY: changes cancelled; reverting.");
                revert_display();
            }
            break;
        }
    }
}

static uint32_t append_unsigned(char *buffer, uint32_t offset,
    uint32_t value)
{
    char digits[10];
    uint32_t count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count != 0u) {
        buffer[offset++] = digits[--count];
    }
    return offset;
}

static bool inside(const struct book8gui_rect *rect, int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && (uint32_t)x >= rect->x
        && (uint32_t)y >= rect->y
        && (uint32_t)x - rect->x < rect->width
        && (uint32_t)y - rect->y < rect->height;
}

static bool set_mouse_position(int32_t x, int32_t y)
{
    if (x < 0) {
        x = 0;
    } else if ((uint32_t)x >= desktop.platform->width) {
        x = (int32_t)(desktop.platform->width - 1u);
    }
    if (y < 0) {
        y = 0;
    } else if ((uint32_t)y >= desktop.platform->height) {
        y = (int32_t)(desktop.platform->height - 1u);
    }
    bool moved = desktop.mouse_x != x || desktop.mouse_y != y;
    if (moved) {
        desktop.cursor_dirty = true;
        desktop.mouse_x = x;
        desktop.mouse_y = y;
    }
    return moved;
}

static struct book8gui_rect pointer_bounds(int32_t x, int32_t y)
{
    uint32_t left = (uint32_t)x;
    uint32_t top = (uint32_t)y;
    uint32_t width = desktop.platform->width - left;
    uint32_t height = desktop.platform->height - top;

    return (struct book8gui_rect){
        .x = left,
        .y = top,
        .width = width < 8u ? width : 8u,
        .height = height < 12u ? height : 12u
    };
}

static void draw_cursor(void)
{
    if (desktop.mouse_x < 0 || desktop.mouse_y < 0) {
        desktop.cursor_saved = false;
        return;
    }

    struct book8gui_rect bounds = pointer_bounds(desktop.mouse_x,
        desktop.mouse_y);
    desktop.cursor_saved = desktop.platform->cursor_save_under != NULL
        && desktop.platform->cursor_save_under(bounds.x, bounds.y,
            bounds.width, bounds.height);
    if (desktop.cursor_saved) {
        desktop.cursor_saved_x = desktop.mouse_x;
        desktop.cursor_saved_y = desktop.mouse_y;
    }
    draw_pointer();
}

static bool update_cursor(void)
{
    if (desktop.platform->cursor_restore_under == NULL
        || desktop.platform->cursor_save_under == NULL
        || desktop.platform->present_rect == NULL) {
        return false;
    }

    bool had_previous = desktop.cursor_saved;
    struct book8gui_rect previous = {0};
    if (had_previous) {
        previous = pointer_bounds(desktop.cursor_saved_x,
            desktop.cursor_saved_y);
        if (!desktop.platform->cursor_restore_under()) {
            return false;
        }
    }
    desktop.cursor_saved = false;
    if (desktop.mouse_x >= 0 && desktop.mouse_y >= 0) {
        struct book8gui_rect current = pointer_bounds(desktop.mouse_x,
            desktop.mouse_y);
        desktop.cursor_saved = desktop.platform->cursor_save_under(
            current.x, current.y, current.width, current.height);
        if (!desktop.cursor_saved) {
            return false;
        }
        desktop.cursor_saved_x = desktop.mouse_x;
        desktop.cursor_saved_y = desktop.mouse_y;
        draw_pointer();

        if (!had_previous) {
            desktop.platform->present_rect(current.x, current.y,
                current.width, current.height);
            return true;
        }
        desktop.platform->present_rect(previous.x, previous.y,
            previous.width, previous.height);
        desktop.platform->present_rect(current.x, current.y,
            current.width, current.height);
    } else if (had_previous) {
        desktop.platform->present_rect(previous.x, previous.y,
            previous.width, previous.height);
    }
    return true;
}

void book8gui_invalidate(void)
{
    desktop.dirty = true;
}

static struct WindowApp *find_app(uint32_t id)
{
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        if (desktop.apps[i].id == id) {
            return &desktop.apps[i];
        }
    }
    return NULL;
}

static void report_registration(const struct WindowApp *app)
{
    char message[128] = "BOOK8-DESKTOP: WINDOW id=";
    uint32_t offset = append_unsigned(message, 25u, app->id);
    const char middle[] = " title=\"";
    for (uint32_t i = 0u; i < sizeof(middle) - 1u; ++i) {
        message[offset++] = middle[i];
    }
    for (const char *title = app->title; *title != '\0' && offset < 110u; ++title) {
        message[offset++] = *title;
    }
    const char suffix[] = "\" registered\n";
    for (uint32_t i = 0u; i < sizeof(suffix); ++i) {
        message[offset++] = suffix[i];
    }
    desktop.platform->diagnostic(message);
}

static void report_focus(const struct WindowApp *app)
{
    char message[128] = "BOOK8-DESKTOP: focus id=";
    uint32_t offset = append_unsigned(message, 24u, app->id);
    const char middle[] = " title=\"";
    for (uint32_t i = 0u; i < sizeof(middle) - 1u; ++i) {
        message[offset++] = middle[i];
    }
    for (const char *title = app->title; *title != '\0' && offset < 124u; ++title) {
        message[offset++] = *title;
    }
    const char suffix[] = "\"\n";
    for (uint32_t i = 0u; i < sizeof(suffix); ++i) {
        message[offset++] = suffix[i];
    }
    desktop.platform->diagnostic(message);
}

static void report_window_message(const char *prefix, uint32_t id,
    const char *title)
{
    char message[128];
    uint32_t offset = 0u;
    while (*prefix != '\0') {
        message[offset++] = *prefix++;
    }
    offset = append_unsigned(message, offset, id);
    const char middle[] = " title=\"";
    for (uint32_t i = 0u; i < sizeof(middle) - 1u; ++i) {
        message[offset++] = middle[i];
    }
    for (uint32_t i = 0u; title[i] != '\0' && offset < 120u; ++i) {
        message[offset++] = title[i];
    }
    message[offset++] = '"';
    message[offset++] = '\n';
    message[offset] = '\0';
    desktop.platform->diagnostic(message);
}

static void report_input_route(const struct WindowApp *app,
    const struct book8gui_event *event)
{
    char message[112] = "BOOK8-DESKTOP: input routed id=";
    uint32_t offset = append_unsigned(message, 31u, app->id);
    const char type[] = " type=mouse-click x=";
    for (uint32_t i = 0u; i < sizeof(type) - 1u; ++i) {
        message[offset++] = type[i];
    }
    offset = append_unsigned(message, offset, (uint32_t)event->mouse_x);
    const char y_label[] = " y=";
    for (uint32_t i = 0u; i < sizeof(y_label) - 1u; ++i) {
        message[offset++] = y_label[i];
    }
    offset = append_unsigned(message, offset, (uint32_t)event->mouse_y);
    const char suffix[] = "\n";
    for (uint32_t i = 0u; i < sizeof(suffix); ++i) {
        message[offset++] = suffix[i];
    }
    desktop.platform->diagnostic(message);
}

static void report_taskbar_click(const struct book8gui_event *event)
{
    char message[80] = "BOOK8-DESKTOP: taskbar click x=";
    uint32_t offset = append_unsigned(message, 31u,
        (uint32_t)event->mouse_x);
    const char y_label[] = " y=";
    for (uint32_t i = 0u; i < sizeof(y_label) - 1u; ++i) {
        message[offset++] = y_label[i];
    }
    offset = append_unsigned(message, offset, (uint32_t)event->mouse_y);
    message[offset++] = '\n';
    message[offset] = '\0';
    desktop.platform->diagnostic(message);
}

static void set_focus(struct WindowApp *app)
{
    if (app == NULL || !app->visible || desktop.focused_id == app->id) {
        return;
    }
    desktop.focused_id = app->id;
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        desktop.apps[i].focused = &desktop.apps[i] == app;
    }
    book8gui_invalidate();
    if (desktop.platform->diagnostic != NULL) {
        report_focus(app);
    }
}

bool book8gui_clipboard_set(const char *path, bool cut)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    size_t i = 0u;
    while (path[i] != '\0' && i + 1u < sizeof(clipboard.source)) {
        clipboard.source[i] = path[i];
        ++i;
    }
    if (path[i] != '\0') {
        clipboard.source[0] = '\0';
        snprintf(clipboard.status, sizeof(clipboard.status), "Path is too long.");
        return false;
    }
    clipboard.source[i] = '\0';
    clipboard.cut = cut;
    snprintf(clipboard.status, sizeof(clipboard.status),
        "%s selected.", cut ? "Cut" : "Copy");
    return true;
}

bool book8gui_clipboard_paste(const char *directory)
{
    if (clipboard.source[0] == '\0') {
        snprintf(clipboard.status, sizeof(clipboard.status), "Clipboard is empty.");
        return false;
    }
    if (desktop.platform->copy_to_directory == NULL) {
        snprintf(clipboard.status, sizeof(clipboard.status), "Paste is unavailable.");
        return false;
    }
    if (!desktop.platform->copy_to_directory(clipboard.source, directory, clipboard.cut)) {
        snprintf(clipboard.status, sizeof(clipboard.status), "%s",
            desktop.platform->last_error_text != NULL
                ? desktop.platform->last_error_text() : "Paste failed.");
        return false;
    }
    if (clipboard.cut)
        clipboard.source[0] = '\0';
    snprintf(clipboard.status, sizeof(clipboard.status), "Paste complete.");
    return true;
}

const char *book8gui_clipboard_status(void)
{
    return clipboard.status;
}

void book8gui_set_modal(const struct WindowApp *app, bool modal)
{
    if (app == NULL || find_app(app->id) != app) {
        return;
    }
    if (modal) {
        desktop.modal_id = app->id;
        desktop.launcher_open = false;
        desktop.audio_popup_open = false;
        desktop.audio_dragging = false;
        set_focus(find_app(app->id));
    } else if (desktop.modal_id == app->id) {
        desktop.modal_id = 0u;
    }
    book8gui_invalidate();
}

static void draw_bevel(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t face)
{
    if (width < 4u || height < 4u) {
        return;
    }
    desktop.platform->fill_rect(x, y, width, height, DESKTOP_EDGE);
    desktop.platform->fill_rect(x + 1u, y + 1u, width - 2u,
        height - 2u, face);
    desktop.platform->fill_rect(x + 1u, y + 1u, width - 3u,
        1u, DESKTOP_LIGHT);
    desktop.platform->fill_rect(x + 1u, y + 1u, 1u,
        height - 3u, DESKTOP_LIGHT);
    desktop.platform->fill_rect(x + 2u, y + height - 2u, width - 3u,
        1u, DESKTOP_SHADOW);
    desktop.platform->fill_rect(x + width - 2u, y + 2u, 1u,
        height - 3u, DESKTOP_SHADOW);
}

static struct book8gui_rect close_rect(const struct WindowApp *app)
{
    return (struct book8gui_rect){
        app->bounds.x + app->bounds.width - DESKTOP_CLOSE_SIZE - 6u,
        app->bounds.y + 6u, DESKTOP_CLOSE_SIZE, DESKTOP_CLOSE_SIZE
    };
}

static struct book8gui_rect maximize_rect(const struct WindowApp *app)
{
    struct book8gui_rect close = close_rect(app);
    close.x -= DESKTOP_CLOSE_SIZE + 2u;
    return close;
}

static struct book8gui_rect minimize_rect(const struct WindowApp *app)
{
    struct book8gui_rect maximize = maximize_rect(app);
    maximize.x -= DESKTOP_CLOSE_SIZE + 2u;
    return maximize;
}

static void draw_box_glyph(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    desktop.platform->fill_rect(x, y, width, 2u, DESKTOP_TEXT);
    desktop.platform->fill_rect(x, y + height - 1u, width, 1u,
        DESKTOP_TEXT);
    desktop.platform->fill_rect(x, y, 1u, height, DESKTOP_TEXT);
    desktop.platform->fill_rect(x + width - 1u, y, 1u, height,
        DESKTOP_TEXT);
}

static void draw_window(struct WindowApp *app)
{
    uint32_t color = app->focused ? DESKTOP_TITLE : DESKTOP_SHADOW;
    draw_bevel(app->bounds.x, app->bounds.y,
        app->bounds.width, app->bounds.height, DESKTOP_WINDOW);
    desktop.platform->fill_rect(app->bounds.x + 2u, app->bounds.y + 2u,
        app->bounds.width - 4u, DESKTOP_TITLEBAR_HEIGHT, color);
    char title_text[128];
    uint32_t title_columns = app->bounds.width > 80u ? (app->bounds.width - 80u) / 8u : 0u;
    uint32_t title_length = 0u;
    while (title_length < title_columns && title_length < sizeof(title_text) - 1u
        && app->title[title_length] != '\0') {
        title_text[title_length] = app->title[title_length];
        ++title_length;
    }
    title_text[title_length] = '\0';
    desktop.platform->draw_text(app->bounds.x + 8u, app->bounds.y + 7u,
        title_text, DESKTOP_TITLE_TEXT, color);
    struct book8gui_rect close = close_rect(app);
    draw_bevel(close.x, close.y, close.width, close.height, DESKTOP_WINDOW);
    desktop.platform->draw_text(close.x + 5u, close.y + 1u, "x",
        DESKTOP_TEXT, DESKTOP_WINDOW);
    struct book8gui_rect maximize = maximize_rect(app);
    draw_bevel(maximize.x, maximize.y, maximize.width, maximize.height,
        DESKTOP_WINDOW);
    if (app->maximized) {
        draw_box_glyph(maximize.x + 7u, maximize.y + 4u, 7u, 6u);
        draw_box_glyph(maximize.x + 4u, maximize.y + 7u, 7u, 7u);
    } else {
        draw_box_glyph(maximize.x + 4u, maximize.y + 4u, 10u, 10u);
    }
    struct book8gui_rect minimize = minimize_rect(app);
    draw_bevel(minimize.x, minimize.y, minimize.width, minimize.height,
        DESKTOP_WINDOW);
    desktop.platform->fill_rect(minimize.x + 4u, minimize.y + 12u, 10u, 2u,
        DESKTOP_TEXT);
    uint64_t render_start = ticks_now();
    app->render(desktop.platform, app);
    app->cpu_ticks += ticks_now() - render_start;
}

static void draw_task_button(uint32_t x, uint32_t y, uint32_t width,
    const char *label, bool active)
{
    uint32_t face = active ? DESKTOP_ACTIVE : DESKTOP_WINDOW;
    uint32_t foreground = active ? DESKTOP_TITLE_TEXT : DESKTOP_TEXT;
    draw_bevel(x, y + 2u, width, DESKTOP_TASKBAR_HEIGHT - 4u, face);
    desktop.platform->draw_text(x + 6u, y + (DESKTOP_TASKBAR_HEIGHT - 16u) / 2u, label,
        foreground, face);
}

/* Shared by drawing and hit-testing so they cannot drift apart. */
static bool task_slot(uint32_t index, uint32_t *x, uint32_t *width)
{
    uint32_t task_x = DESKTOP_START_WIDTH + 12u;
    uint32_t available = desktop.platform->width
        > DESKTOP_STATUS_WIDTH + task_x
        ? desktop.platform->width - DESKTOP_STATUS_WIDTH - task_x : 0u;
    uint32_t button_width = desktop.app_count == 0u ? 0u
        : available / desktop.app_count;
    if (button_width > DESKTOP_TASK_WIDTH) {
        button_width = DESKTOP_TASK_WIDTH;
    }
    if (button_width < 72u) {
        button_width = 72u;
    }
    *x = task_x + index * (button_width + 4u);
    *width = button_width;
    return desktop.platform->width > DESKTOP_STATUS_WIDTH
        && *x + button_width <= desktop.platform->width
            - DESKTOP_STATUS_WIDTH;
}

static uint32_t menu_top(void)
{
    return desktop.platform->height - DESKTOP_TASKBAR_HEIGHT;
}

static uint32_t menu_visible_rows(uint32_t level)
{
    uint32_t count = level == 0u ? (uint32_t)START_MENU_COUNT
        : desktop.open_submenu == 0u
        ? programs_total() : start_menu[desktop.open_submenu].child_count;
    uint32_t capacity = (menu_top() - 6u) / DESKTOP_MENU_ROW;
    return count < capacity ? count : capacity;
}

/* level 0 is the Start panel; level 1 is the open Programs submenu. */
static struct book8gui_rect menu_panel_rect(uint32_t level)
{
    uint32_t root_height = (uint32_t)START_MENU_COUNT * DESKTOP_MENU_ROW
        + 4u;
    uint32_t root_y = menu_top() - 2u - root_height;
    if (level == 0u) {
        return (struct book8gui_rect){
            DESKTOP_MENU_X, root_y, DESKTOP_MENU_WIDTH, root_height
        };
    }
    uint32_t count = menu_visible_rows(1u);
    uint32_t height = count * DESKTOP_MENU_ROW + 4u;
    uint32_t y = root_y + 2u + desktop.open_submenu * DESKTOP_MENU_ROW;
    if (y + height > menu_top() - 2u) {
        y = menu_top() - 2u - height;
    }
    return (struct book8gui_rect){
        DESKTOP_MENU_X + DESKTOP_MENU_WIDTH - 2u, y,
        desktop.open_submenu == 0u ? 224u : DESKTOP_MENU_WIDTH,
        height
    };
}

static struct book8gui_rect menu_row_rect(uint32_t level, uint32_t index)
{
    struct book8gui_rect panel = menu_panel_rect(level);
    return (struct book8gui_rect){
        panel.x + 2u, panel.y + 2u + index * DESKTOP_MENU_ROW,
        panel.width - 4u, DESKTOP_MENU_ROW
    };
}

static void draw_menu_panel(uint32_t level,
    const struct book8gui_menu_item *items, uint32_t count)
{
    struct book8gui_rect panel = menu_panel_rect(level);
    draw_bevel(panel.x, panel.y, panel.width, panel.height, DESKTOP_WINDOW);
    for (uint32_t i = 0u; i < count; ++i) {
        struct book8gui_rect row = menu_row_rect(level, i);
        bool active = level == 0u && desktop.open_submenu == i;
        uint32_t face = active ? DESKTOP_ACTIVE : DESKTOP_WINDOW;
        uint32_t fore = active ? DESKTOP_TITLE_TEXT : DESKTOP_TEXT;
        desktop.platform->fill_rect(row.x, row.y, row.width, row.height,
            face);
        const char *label = items != NULL ? items[i].label : "";
        if (level == 1u && desktop.open_submenu == 0u) {
            uint32_t row_index = programs_first + i;
            if (registry_programs_active) {
                uint32_t program_index = menu_program_index(row_index);
                label = program_index < registered_count
                    ? registered_programs[program_index].name : "";
            } else {
                label = row_index < PROGRAMS_MENU_COUNT
                    ? programs_menu[row_index].label
                    : registered_programs[menu_program_index(row_index)].name;
            }
        }
        char clipped[32];
        uint32_t columns = (row.width - 16u
            - (level == 1u && desktop.open_submenu == 0u ? UI_SCROLL_W : 0u)) / 8u;
        uint32_t n = 0u;
        while (n < columns && n + 1u < sizeof(clipped) && label[n] != '\0') {
            clipped[n] = label[n]; ++n;
        }
        clipped[n] = '\0';
        desktop.platform->draw_text(row.x + 8u, row.y + 2u, clipped,
            fore, face);
        if (items != NULL && items[i].kind == BOOK8GUI_MENU_SUBMENU) {
            desktop.platform->draw_text(row.x + row.width - 16u, row.y + 2u,
                ">", fore, face);
        }
    }
}

static void draw_launcher(void)
{
    if (desktop.open_submenu == 0u) {
        programs_first = ui_wheel_scroll(programs_first, programs_total(),
            menu_visible_rows(1u), 0, 1u);
    }
    draw_menu_panel(0u, start_menu, (uint32_t)START_MENU_COUNT);
    if (desktop.open_submenu != DESKTOP_NO_SUBMENU) {
        const struct book8gui_menu_item *parent =
            &start_menu[desktop.open_submenu];
        draw_menu_panel(1u, desktop.open_submenu == 0u ? NULL : parent->children,
            menu_visible_rows(1u));
        if (desktop.open_submenu == 0u) {
            struct book8gui_rect panel = menu_panel_rect(1u);
            struct book8gui_rect track = ui_scroll_track(&panel);
            if (programs_total() > menu_visible_rows(1u)) {
                ui_scrollbar(desktop.platform, &track, programs_total(),
                    menu_visible_rows(1u), programs_first);
            }
        }
    }
}

static void poll_audio_status(void)
{
    uint8_t volume = 0u;
    bool muted = true;
    bool available = desktop.platform->audio_state != NULL
        && desktop.platform->audio_state(&volume, &muted);
    if (available && volume > 100u) {
        if (desktop.platform->diagnostic != NULL) {
            desktop.platform->diagnostic(
                "BOOK8-DESKTOP: audio state returned invalid volume\n");
        }
        available = false;
    }
    if (!available) {
        volume = 0u;
        muted = true;
    }
    if (available != desktop.audio_available
        || volume != desktop.audio_volume || muted != desktop.audio_muted) {
        desktop.audio_available = available;
        desktop.audio_volume = volume;
        desktop.audio_muted = muted;
        book8gui_invalidate();
    }
}

static void set_audio_volume(uint8_t volume)
{
    if (!desktop.audio_available || volume > 100u
        || desktop.platform->audio_set_volume == NULL) {
        return;
    }
    if (!desktop.platform->audio_set_volume(volume)) {
        if (desktop.platform->diagnostic != NULL) {
            desktop.platform->diagnostic(
                "BOOK8-DESKTOP: audio volume change failed\n");
        }
        poll_audio_status();
        return;
    }
    poll_audio_status();
}

static void set_audio_mute(bool muted)
{
    if (!desktop.audio_available || desktop.platform->audio_set_mute == NULL) {
        return;
    }
    if (!desktop.platform->audio_set_mute(muted)) {
        if (desktop.platform->diagnostic != NULL) {
            desktop.platform->diagnostic(
                "BOOK8-DESKTOP: audio mute change failed\n");
        }
        poll_audio_status();
        return;
    }
    poll_audio_status();
}

static uint8_t audio_volume_at_y(const struct book8gui_rect *track,
    int32_t y)
{
    int32_t top = (int32_t)track->y;
    int32_t bottom = top + (int32_t)track->height - 1;
    int32_t clamped = y < top ? top : (y > bottom ? bottom : y);
    return (uint8_t)(((bottom - clamped) * 100) / (bottom - top));
}

static bool handle_audio_popup_event(const struct book8gui_event *event)
{
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool pressed = down && (desktop.mouse_buttons & 1u) == 0u;
    struct book8gui_rect icon = audio_icon_rect();
    struct book8gui_rect popup = audio_popup_rect();
    int32_t x = event->mouse_x;
    int32_t y = event->mouse_y;

    if (desktop.audio_dragging) {
        struct book8gui_rect track = audio_slider_rect(&popup);
        set_audio_volume(audio_volume_at_y(&track, y));
        if (!down)
            desktop.audio_dragging = false;
        return true;
    }
    if (inside(&icon, x, y)) {
        if (pressed) {
            desktop.audio_popup_open = !desktop.audio_popup_open;
            desktop.audio_dragging = false;
            if (desktop.audio_popup_open) {
                desktop.launcher_open = false;
                desktop.open_submenu = DESKTOP_NO_SUBMENU;
                desktop.context_open = false;
            }
            book8gui_invalidate();
        }
        return true;
    }
    if (!desktop.audio_popup_open)
        return false;
    if (inside(&popup, x, y)) {
        if (pressed && desktop.audio_available) {
            struct book8gui_rect track = audio_slider_rect(&popup);
            struct book8gui_rect slider_hit = {
                track.x - 5u, track.y - 5u, track.width + 10u,
                track.height + 10u
            };
            struct book8gui_rect mute = audio_mute_rect(&popup);
            if (inside(&slider_hit, x, y)) {
                desktop.audio_dragging = true;
                set_audio_volume(audio_volume_at_y(&track, y));
            } else if (inside(&mute, x, y)) {
                set_audio_mute(!desktop.audio_muted);
            }
        }
        return true;
    }
    if (pressed) {
        desktop.audio_popup_open = false;
        desktop.audio_dragging = false;
        book8gui_invalidate();
    }
    return false;
}

/* Samples RTC and device flags; only a change requests a redraw. */
static void poll_taskbar_status(void)
{
    uint32_t flags = desktop.platform->status_flags != NULL
        ? desktop.platform->status_flags() : 0u;
    if (flags != desktop.status_flags) {
        desktop.status_flags = flags;
        book8gui_invalidate();
    }
    struct book8gui_time time;
    if (desktop.platform->read_time != NULL
        && desktop.platform->read_time(&time)) {
        if (!desktop.time_valid || time.minute != desktop.time.minute
            || time.hour != desktop.time.hour
            || time.day != desktop.time.day
            || time.month != desktop.time.month
            || time.year != desktop.time.year) {
            book8gui_invalidate();
        }
        desktop.time = time;
        desktop.time_valid = true;
    }
    poll_audio_status();
}

static void poll_network_activity(uint64_t now)
{
    uint32_t tx, rx;
    bool valid = desktop.platform->network_activity != NULL
        && desktop.platform->network_activity(&tx, &rx);
    if (valid && desktop.network_valid) {
        uint64_t duration = desktop.platform->clock_hz / 5u;
        if (duration == 0u)
            duration = 1u;
        if (tx != desktop.network_tx)
            desktop.tx_until = now + duration;
        if (rx != desktop.network_rx)
            desktop.rx_until = now + duration;
    }
    if (valid) {
        desktop.network_tx = tx;
        desktop.network_rx = rx;
    } else {
        desktop.tx_until = desktop.rx_until = 0u;
    }
    bool tx_lit = valid && now < desktop.tx_until;
    bool rx_lit = valid && now < desktop.rx_until;
    if (tx_lit != desktop.tx_lit || rx_lit != desktop.rx_lit
        || valid != desktop.network_valid)
        book8gui_invalidate();
    desktop.network_valid = valid;
    desktop.tx_lit = tx_lit;
    desktop.rx_lit = rx_lit;
}

static void draw_network_terminal(uint32_t x, uint32_t y, bool lit)
{
    desktop.platform->fill_rect(x, y, 13u, 10u, DESKTOP_EDGE);
    desktop.platform->fill_rect(x + 1u, y + 1u, 11u, 7u,
        lit ? 0x00b8e8u : DESKTOP_SHADOW);
    desktop.platform->fill_rect(x + 5u, y + 10u, 3u, 2u, DESKTOP_EDGE);
    desktop.platform->fill_rect(x + 3u, y + 12u, 7u, 1u, DESKTOP_EDGE);
}

static void draw_taskbar(void)
{
    uint32_t width = desktop.platform->width;
    uint32_t top = desktop.platform->height - DESKTOP_TASKBAR_HEIGHT;
    desktop.platform->fill_rect(0u, top, width, DESKTOP_TASKBAR_HEIGHT,
        DESKTOP_WINDOW);
    desktop.platform->fill_rect(0u, top, width, 1u, DESKTOP_LIGHT);
    draw_task_button(4u, top, DESKTOP_START_WIDTH, "START",
        desktop.launcher_open);

    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        uint32_t x;
        uint32_t button_width;
        if (!task_slot(i, &x, &button_width)) {
            break;
        }
        draw_task_button(x, top, button_width, desktop.apps[i].title,
            desktop.apps[i].focused);
    }

    uint32_t clock_x = width > 96u ? width - 88u : 0u;
    uint32_t network_x = clock_x > 52u ? clock_x - 52u : 0u;
    draw_network_terminal(network_x + 7u, top + 5u, desktop.rx_lit);
    draw_network_terminal(network_x, top + 15u, desktop.tx_lit);
    draw_audio_icon();
    if (desktop.time_valid) {
        char line[12];
        uint32_t n = 0u;
        uint32_t clock24h = 0u;
        (void)reg_get_u32("HKLM\\SOFTWARE\\BOOK8\\Settings", "Clock24H",
            &clock24h);
        if (clock24h != 0u) {
            n = append_unsigned(line, n, desktop.time.hour);
        } else {
            uint32_t hour12 = desktop.time.hour % 12u;
            if (hour12 == 0u) { hour12 = 12u; }
            n = append_unsigned(line, n, hour12);
        }
        line[n++] = ':';
        line[n++] = (char)('0' + desktop.time.minute / 10u % 10u);
        line[n++] = (char)('0' + desktop.time.minute % 10u);
        if (clock24h == 0u) {
            line[n++] = ' ';
            line[n++] = desktop.time.hour >= 12u ? 'P' : 'A';
            line[n++] = 'M';
        }
        line[n] = '\0';
        desktop.platform->draw_text(clock_x, top + 2u, line,
            DESKTOP_TEXT, DESKTOP_WINDOW);
        n = append_unsigned(line, 0u, desktop.time.month);
        line[n++] = '/';
        n = append_unsigned(line, n, desktop.time.day);
        line[n++] = '/';
        line[n] = '\0';
        char date[16];
        for (uint32_t i = 0u; i <= n; ++i) {
            date[i] = line[i];
        }
        uint32_t d = append_unsigned(date, n, desktop.time.year);
        date[d] = '\0';
        desktop.platform->draw_text(clock_x, top + 18u, date,
            DESKTOP_TEXT, DESKTOP_WINDOW);
    }
}

static struct book8gui_rect audio_icon_rect(void)
{
    uint32_t clock_x = desktop.platform->width > 96u
        ? desktop.platform->width - 88u : 0u;
    uint32_t x = clock_x > 24u ? clock_x - 24u : 0u;
    uint32_t y = desktop.platform->height - DESKTOP_TASKBAR_HEIGHT + 9u;
    return (struct book8gui_rect){x, y, 18u, 18u};
}

static struct book8gui_rect audio_popup_rect(void)
{
    struct book8gui_rect icon = audio_icon_rect();
    uint32_t top = desktop.platform->height - DESKTOP_TASKBAR_HEIGHT;
    uint32_t x = icon.x + icon.width / 2u - DESKTOP_AUDIO_POPUP_WIDTH / 2u;
    if (x + DESKTOP_AUDIO_POPUP_WIDTH > desktop.platform->width) {
        x = desktop.platform->width - DESKTOP_AUDIO_POPUP_WIDTH;
    }
    uint32_t y = top > DESKTOP_AUDIO_POPUP_HEIGHT + 2u
        ? top - DESKTOP_AUDIO_POPUP_HEIGHT - 2u : 0u;
    return (struct book8gui_rect){x, y, DESKTOP_AUDIO_POPUP_WIDTH,
        DESKTOP_AUDIO_POPUP_HEIGHT};
}

static struct book8gui_rect audio_slider_rect(
    const struct book8gui_rect *popup)
{
    return (struct book8gui_rect){popup->x + 56u, popup->y + 36u, 14u, 86u};
}

static struct book8gui_rect audio_mute_rect(
    const struct book8gui_rect *popup)
{
    return (struct book8gui_rect){popup->x + 12u, popup->y + 134u, 108u, 22u};
}

static void draw_audio_icon(void)
{
    struct book8gui_rect icon = audio_icon_rect();
    uint32_t x = icon.x + 1u;
    uint32_t y = icon.y + 2u;
    uint32_t color = DESKTOP_TEXT;
    desktop.platform->fill_rect(x, y + 5u, 4u, 8u, color);
    desktop.platform->fill_rect(x + 4u, y + 2u, 5u, 14u, color);
    if (!desktop.audio_available || desktop.audio_muted
        || desktop.audio_volume == 0u) {
        uint32_t mute = 0x800000u;
        desktop.platform->fill_rect(x + 11u, y + 3u, 2u, 2u, mute);
        desktop.platform->fill_rect(x + 13u, y + 5u, 2u, 2u, mute);
        desktop.platform->fill_rect(x + 15u, y + 7u, 2u, 2u, mute);
        desktop.platform->fill_rect(x + 13u, y + 9u, 2u, 2u, mute);
        desktop.platform->fill_rect(x + 11u, y + 11u, 2u, 2u, mute);
    } else {
        desktop.platform->fill_rect(x + 11u, y + 5u, 2u, 8u, color);
        desktop.platform->fill_rect(x + 14u, y + 2u, 2u, 14u, color);
    }
}

static void draw_audio_popup(void)
{
    if (!desktop.audio_popup_open)
        return;
    struct book8gui_rect popup = audio_popup_rect();
    draw_bevel(popup.x, popup.y, popup.width, popup.height, DESKTOP_WINDOW);
    desktop.platform->draw_text(popup.x + 10u, popup.y + 8u, "Volume",
        DESKTOP_TEXT, DESKTOP_WINDOW);
    if (desktop.audio_available) {
        struct book8gui_rect track = audio_slider_rect(&popup);
        draw_bevel(track.x, track.y, track.width, track.height, DESKTOP_SHADOW);
        uint32_t inner_height = track.height - 4u;
        uint32_t fill_height = inner_height * desktop.audio_volume / 100u;
        uint32_t fill_y = track.y + 2u + inner_height - fill_height;
        if (fill_height != 0u) {
            desktop.platform->fill_rect(track.x + 2u, fill_y,
                track.width - 4u, fill_height, DESKTOP_ACTIVE);
        }
        uint32_t knob_y = track.y + 2u + inner_height
            - inner_height * desktop.audio_volume / 100u - 4u;
        draw_bevel(track.x - 5u, knob_y, 24u, 9u, DESKTOP_WINDOW);
        char percent[5];
        uint32_t n = append_unsigned(percent, 0u, desktop.audio_volume);
        percent[n++] = '%';
        percent[n] = '\0';
        desktop.platform->draw_text(popup.x + 82u, popup.y + 67u, percent,
            DESKTOP_TEXT, DESKTOP_WINDOW);
        struct book8gui_rect mute = audio_mute_rect(&popup);
        draw_bevel(mute.x, mute.y, mute.width, mute.height, DESKTOP_WINDOW);
        desktop.platform->fill_rect(mute.x + 8u, mute.y + 5u, 12u, 12u,
            DESKTOP_EDGE);
        desktop.platform->fill_rect(mute.x + 9u, mute.y + 6u, 10u, 10u,
            DESKTOP_LIGHT);
        if (desktop.audio_muted) {
            desktop.platform->fill_rect(mute.x + 11u, mute.y + 8u, 6u, 6u,
                DESKTOP_TEXT);
        }
        desktop.platform->draw_text(mute.x + 27u, mute.y + 4u, "Mute",
            DESKTOP_TEXT, DESKTOP_WINDOW);
    } else {
        desktop.platform->draw_text(popup.x + 10u, popup.y + 62u,
            "Audio unavailable", DESKTOP_TEXT, DESKTOP_WINDOW);
    }
}

static void draw_pointer(void)
{
    if (desktop.mouse_x < 0 || desktop.mouse_y < 0) {
        return;
    }
    static const uint8_t arrow_width[12] =
        {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 5u, 4u, 3u, 2u};
    for (uint32_t row = 0u; row < 12u; ++row) {
        uint32_t y = (uint32_t)desktop.mouse_y + row;
        if (y >= desktop.platform->height) {
            break;
        }
        uint32_t width = arrow_width[row];
        desktop.platform->fill_rect((uint32_t)desktop.mouse_x, y,
            width, 1u, DESKTOP_EDGE);
        if (row >= 2u && row < 8u && width > 2u) {
            desktop.platform->fill_rect((uint32_t)desktop.mouse_x + 1u,
                y, width - 2u, 1u, DESKTOP_LIGHT);
        }
    }
}

static void render_desktop(void)
{
    uint32_t width = desktop.platform->width;
    uint32_t height = desktop.platform->height;
    desktop.platform->fill_rect(0u, 0u, width, height, DESKTOP_BACKGROUND);
    desktop.platform->draw_text(12u, 8u, "BOOK8 DESKTOP",
        DESKTOP_TITLE_TEXT, DESKTOP_BACKGROUND);
    if (clipboard.status[0] != '\0')
        desktop.platform->draw_text(12u, DESKTOP_TOP_HEIGHT,
            clipboard.status, DESKTOP_TITLE_TEXT, DESKTOP_BACKGROUND);

    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        if (!desktop.apps[i].focused && desktop.apps[i].visible) {
            draw_window(&desktop.apps[i]);
        }
    }
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        if (desktop.apps[i].focused && desktop.apps[i].visible) {
            draw_window(&desktop.apps[i]);
        }
    }
    draw_taskbar();
    if (desktop.launcher_open) {
        draw_launcher();
    }
    if (desktop.context_open) {
        draw_bevel(desktop.context_x, desktop.context_y, 150u, 24u, DESKTOP_WINDOW);
        desktop.platform->draw_text(desktop.context_x + 5u, desktop.context_y + 4u,
            "Paste to A:\\", DESKTOP_TEXT, DESKTOP_WINDOW);
    }
    draw_audio_popup();
    draw_display_confirmation();
    draw_cursor();
}

static bool hit_taskbar(uint32_t x, uint32_t y)
{
    return y >= desktop.platform->height - DESKTOP_TASKBAR_HEIGHT
        && x < desktop.platform->width;
}

static struct WindowApp *window_at(int32_t x, int32_t y)
{
    struct WindowApp *focused = find_app(desktop.focused_id);
    if (focused != NULL && focused->visible && inside(&focused->bounds, x, y)) {
        return focused;
    }
    for (uint32_t i = desktop.app_count; i != 0u; --i) {
        struct WindowApp *app = &desktop.apps[i - 1u];
        if (app->visible && inside(&app->bounds, x, y)) {
            return app;
        }
    }
    return NULL;
}

static struct WindowApp *find_class_instance(
    const struct book8gui_app_class *cls, uint32_t *count)
{
    struct WindowApp *first = NULL;
    *count = 0u;
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        if (desktop.apps[i].cls == cls) {
            if (first == NULL) {
                first = &desktop.apps[i];
            }
            ++*count;
        }
    }
    return first;
}

static void restore_window(struct WindowApp *app)
{
    if (app->minimized) {
        app->minimized = false;
        app->visible = true;
        if (desktop.platform->diagnostic != NULL) {
            report_window_message("BOOK8-DESKTOP: WINDOW restored id=",
                app->id, app->title);
        }
        book8gui_invalidate();
    }
}

static void minimize_window(struct WindowApp *app)
{
    app->minimized = true;
    app->visible = false;
    if (app->id == desktop.focused_id) {
        app->focused = false;
        desktop.focused_id = 0u;
        for (uint32_t i = desktop.app_count; i != 0u; --i) {
            if (desktop.apps[i - 1u].visible) {
                set_focus(&desktop.apps[i - 1u]);
                break;
            }
        }
    }
    if (desktop.platform->diagnostic != NULL) {
        report_window_message("BOOK8-DESKTOP: WINDOW minimized id=", app->id,
            app->title);
    }
    book8gui_invalidate();
}

static void toggle_maximize(struct WindowApp *app)
{
    if (!refresh_display_size()) {
        return;
    }
    if (app->maximized) {
        app->bounds = app->restore_bounds;
        app->maximized = false;
    } else {
        app->restore_bounds = app->bounds;
        app->bounds = maximized_bounds();
        app->maximized = true;
    }
    if (desktop.platform->diagnostic != NULL) {
        report_window_message(app->maximized
            ? "BOOK8-DESKTOP: WINDOW maximized id="
            : "BOOK8-DESKTOP: WINDOW restored id=", app->id, app->title);
    }
    book8gui_invalidate();
}

static struct WindowApp *launch_app(const struct book8gui_app_class *cls,
    const struct book8gui_rect *bounds)
{
    uint32_t instances;
    struct WindowApp *existing = find_class_instance(cls, &instances);
    if (cls->single_instance && existing != NULL) {
        restore_window(existing);
        set_focus(existing);
        return existing;
    }
    if (desktop.app_count >= BOOK8GUI_MAX_WINDOWS
        || instances >= BOOK8GUI_MAX_INSTANCES_PER_APP) {
        if (desktop.platform->diagnostic != NULL) {
            report_window_message("BOOK8-DESKTOP: launch refused id=", 0u,
                cls->name);
        }
        return NULL;
    }
    void *state = NULL;
    if (cls->create != NULL) {
        state = cls->create(desktop.platform);
        if (state == NULL) {
            if (desktop.platform->diagnostic != NULL) {
                report_window_message("BOOK8-DESKTOP: launch failed id=", 0u,
                    cls->name);
            }
            return NULL;
        }
    }

    struct book8gui_rect rect;
    if (bounds != NULL) {
        rect = *bounds;
    } else {
        uint32_t work_bottom = desktop.platform->height
            - DESKTOP_TASKBAR_HEIGHT;
        uint32_t step = (desktop.launch_count++ % 6u) * 24u;
        rect.width = cls->default_width;
        rect.height = cls->default_height;
        if (rect.width > desktop.platform->width - 8u) {
            rect.width = desktop.platform->width - 8u;
        }
        if (rect.height > work_bottom - DESKTOP_TOP_HEIGHT - 8u) {
            rect.height = work_bottom - DESKTOP_TOP_HEIGHT - 8u;
        }
        rect.x = 40u + step;
        rect.y = DESKTOP_TOP_HEIGHT + 8u + step;
        if (rect.x + rect.width > desktop.platform->width) {
            rect.x = desktop.platform->width - rect.width;
        }
        if (rect.y + rect.height > work_bottom) {
            rect.y = work_bottom - rect.height;
        }
    }

    struct WindowApp *app = &desktop.apps[desktop.app_count++];
    app->id = desktop.next_id++;
    app->cls = cls;
    app->title = cls->name;
    app->bounds = rect;
    app->visible = true;
    app->focused = false;
    app->minimized = false;
    app->maximized = false;
    app->restore_bounds = rect;
    app->cpu_ticks = 0u;
    app->disk_read_acc = 0u;
    app->disk_write_acc = 0u;
    app->cpu_permille = 0u;
    app->disk_read_bps = 0u;
    app->disk_write_bps = 0u;
    app->state = state;
    app->render = cls->render;
    app->handle_event = cls->handle_event;
    if (desktop.platform->diagnostic != NULL) {
        report_registration(app);
    }
    book8gui_invalidate();
    return app;
}

static void close_window(struct WindowApp *app)
{
    uint32_t index = (uint32_t)(app - desktop.apps);
    bool was_focused = app->id == desktop.focused_id;
    if (desktop.modal_id == app->id) {
        desktop.modal_id = 0u;
    }
    if (desktop.platform->diagnostic != NULL) {
        report_window_message("BOOK8-DESKTOP: WINDOW closed id=", app->id,
            app->title);
    }
    if (app->cls != NULL && app->cls->destroy != NULL) {
        app->cls->destroy(app->state);
    }
    for (uint32_t i = index; i + 1u < desktop.app_count; ++i) {
        desktop.apps[i] = desktop.apps[i + 1u];
    }
    --desktop.app_count;
    if (was_focused) {
        desktop.focused_id = 0u;
        if (desktop.app_count != 0u) {
            set_focus(&desktop.apps[desktop.app_count - 1u]);
        }
    }
    book8gui_invalidate();
}

struct WindowApp *book8gui_launch_class(const struct book8gui_app_class *cls)
{
    struct WindowApp *app = launch_app(cls, NULL);
    set_focus(app);
    return app;
}

const struct book8gui_app_class *book8gui_file_app(uint32_t index)
{
    for (uint32_t i = 0u; i < sizeof(programs_menu) / sizeof(programs_menu[0]); ++i) {
        const struct book8gui_app_class *cls = programs_menu[i].launch;
        if (cls != NULL && cls->open_path != NULL) {
            if (index == 0u) {
                return cls;
            }
            --index;
        }
    }
    if (book8gui_utility_installed(BOOK8_GUI_CODEEDIT)) {
        if (index == 0u) { return &book8_codeedit_class; }
        --index;
    }
    if (book8gui_utility_installed(BOOK8_GUI_HEXVIEW)) {
        if (index == 0u) { return &book8_hexview_class; }
        --index;
    }
    if (book8gui_utility_installed(BOOK8_GUI_PAINTBOOK)) {
        if (index == 0u) { return &book8_paintbook_class; }
        --index;
    }
    return NULL;
}

void book8gui_request_close(const struct WindowApp *app)
{
    desktop.close_request_id = app->id;
}

void book8gui_end_task(uint32_t id)
{
    if (id != 0u && find_app(id) != NULL) {
        desktop.close_request_id = id;
    }
}

void book8gui_note_disk(const struct WindowApp *app, uint32_t read_bytes,
    uint32_t write_bytes)
{
    struct WindowApp *target = find_app(app->id);
    if (target == NULL) {
        return;
    }
    target->disk_read_acc += read_bytes;
    target->disk_write_acc += write_bytes;
}

static uint32_t permille_of(uint64_t part, uint64_t whole)
{
    if (whole == 0u) {
        return 0u;
    }
    uint64_t value = part * 1000u / whole;
    return value > 1000u ? 1000u : (uint32_t)value;
}

static uint32_t rate_of(uint64_t amount, uint64_t elapsed)
{
    if (elapsed == 0u) {
        return 0u;
    }
    uint64_t value = amount * desktop.platform->clock_hz / elapsed;
    return value > 0xffffffffu ? 0xffffffffu : (uint32_t)value;
}

/* Runs once per second: turns the busy/disk accumulators into rates. */
static void sample_telemetry(uint64_t now)
{
    const struct book8gui_platform *platform = desktop.platform;
    uint64_t elapsed = now - desktop.sample_start;
    if (elapsed == 0u || platform->clock_hz == 0u) {
        return;
    }
    struct book8gui_telemetry *t = &desktop.telemetry;
    uint64_t apps_busy = 0u;
    bool refresh = false;
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        struct WindowApp *app = &desktop.apps[i];
        apps_busy += app->cpu_ticks;
        app->cpu_permille = permille_of(app->cpu_ticks, elapsed);
        app->disk_read_bps = rate_of(app->disk_read_acc, elapsed);
        app->disk_write_bps = rate_of(app->disk_write_acc, elapsed);
        app->cpu_ticks = 0u;
        app->disk_read_acc = 0u;
        app->disk_write_acc = 0u;
        if (app->cls != NULL && app->cls->refresh_1hz) {
            refresh = true;
        }
    }
    t->cpu_permille = permille_of(desktop.busy_ticks, elapsed);
    desktop.desktop_cpu_permille = desktop.busy_ticks > apps_busy
        ? permille_of(desktop.busy_ticks - apps_busy, elapsed) : 0u;
    desktop.busy_ticks = 0u;
    t->task_count = desktop.app_count;
    t->disk_busy_permille = 0u;
    t->disk_read_bps = 0u;
    t->disk_write_bps = 0u;
    t->sys_valid = platform->system_info != NULL
        && platform->system_info(&t->sys);
    if (t->sys_valid) {
        t->disk_read_bps = rate_of(
            t->sys.disk_read_bytes - desktop.last_disk_read, elapsed);
        t->disk_write_bps = rate_of(
            t->sys.disk_write_bytes - desktop.last_disk_write, elapsed);
        t->disk_busy_permille = permille_of(
            t->sys.disk_busy_ticks - desktop.last_disk_busy, elapsed);
        desktop.last_disk_read = t->sys.disk_read_bytes;
        desktop.last_disk_write = t->sys.disk_write_bytes;
        desktop.last_disk_busy = t->sys.disk_busy_ticks;
        t->uptime_seconds = t->sys.uptime_seconds;
    } else {
        t->uptime_seconds = (now - desktop.session_start)
            / platform->clock_hz;
    }
    desktop.sample_start = now;
    if (refresh) {
        book8gui_invalidate();
    }
}

void book8gui_get_telemetry(struct book8gui_telemetry *out)
{
    *out = desktop.telemetry;
    out->task_count = desktop.app_count;
}

uint32_t book8gui_window_snapshot(struct book8gui_window_info *out,
    uint32_t max)
{
    uint32_t count = 0u;
    if (max == 0u) {
        return 0u;
    }
    out[count++] = (struct book8gui_window_info){
        0u, "BOOK8 Desktop", desktop.desktop_cpu_permille,
        desktop.platform->width * desktop.platform->height * 4u / 1024u,
        0u, 0u, false
    };
    for (uint32_t i = 0u; i < desktop.app_count && count < max; ++i) {
        const struct WindowApp *app = &desktop.apps[i];
        uint32_t size = app->cls != NULL ? app->cls->state_size : 0u;
        out[count++] = (struct book8gui_window_info){
            app->id, app->title, app->cpu_permille,
            size / 1024u + 1u, app->disk_read_bps, app->disk_write_bps,
            app->minimized
        };
    }
    return count;
}

static void apply_close_request(void)
{
    if (desktop.close_request_id == 0u) {
        return;
    }
    struct WindowApp *app = find_app(desktop.close_request_id);
    desktop.close_request_id = 0u;
    if (app != NULL) {
        close_window(app);
    }
}

static void destroy_all_windows(void)
{
    while (desktop.app_count != 0u) {
        struct WindowApp *app = &desktop.apps[desktop.app_count - 1u];
        if (app->cls != NULL && app->cls->destroy != NULL) {
            app->cls->destroy(app->state);
        }
        --desktop.app_count;
    }
    desktop.focused_id = 0u;
}

static void handle_taskbar(const struct book8gui_event *event)
{
    uint32_t top = desktop.platform->height - DESKTOP_TASKBAR_HEIGHT;
    bool pressed = (event->mouse_buttons & 1u) != 0u
        && (desktop.mouse_buttons & 1u) == 0u;
    if (!pressed || event->mouse_y < (int32_t)top) {
        return;
    }
    if (desktop.platform->diagnostic != NULL) {
        report_taskbar_click(event);
    }
    uint32_t x = (uint32_t)event->mouse_x;
    if (x < DESKTOP_START_WIDTH + 4u) {
        desktop.launcher_open = !desktop.launcher_open;
        desktop.open_submenu = DESKTOP_NO_SUBMENU;
        book8gui_invalidate();
        return;
    }
    for (uint32_t i = 0u; i < desktop.app_count; ++i) {
        uint32_t button_x;
        uint32_t button_width;
        if (!task_slot(i, &button_x, &button_width)) {
            break;
        }
        if (x >= button_x && x < button_x + button_width) {
            struct WindowApp *app = &desktop.apps[i];
            desktop.launcher_open = false;
            if (app->minimized) {
                restore_window(app);
                set_focus(app);
            } else if (app->id == desktop.focused_id) {
                minimize_window(app);
            } else {
                set_focus(app);
            }
            return;
        }
    }
}

static void close_launcher(void)
{
    desktop.launcher_open = false;
    desktop.open_submenu = DESKTOP_NO_SUBMENU;
    desktop.menu_drag = false;
    desktop.menu_wheel_remainder = 0;
    book8gui_invalidate();
}

/* Returns true when the item was handled as a power action. */
static bool run_power_item(const struct book8gui_menu_item *item)
{
    if (item->kind == BOOK8GUI_MENU_SHUTDOWN) {
        close_launcher();
        if (desktop.platform->shutdown != NULL) {
            desktop.platform->shutdown();
        }
        return true;
    }
    if (item->kind == BOOK8GUI_MENU_REBOOT) {
        close_launcher();
        if (desktop.platform->reboot != NULL) {
            desktop.platform->reboot();
        }
        return true;
    }
    if (item->kind == BOOK8GUI_MENU_EXIT) {
        close_launcher();
        desktop.close_requested = true;
        return true;
    }
    return false;
}

/* Returns true when the press was consumed by the Start menu. */
static bool handle_launcher_click(const struct book8gui_event *event)
{
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool pressed = down
        && (desktop.mouse_buttons & 1u) == 0u;
    if (!desktop.launcher_open) {
        return false;
    }
    int32_t x = event->mouse_x;
    int32_t y = event->mouse_y;

    if (desktop.menu_drag) {
        if (down) {
            struct book8gui_rect panel = menu_panel_rect(1u);
            struct book8gui_rect track = ui_scroll_track(&panel);
            programs_first = ui_scrollbar_drag_position(&track, programs_total(),
                menu_visible_rows(1u), y, desktop.menu_grab);
        } else { desktop.menu_drag = false; }
        return true;
    }
    if (desktop.open_submenu == 0u) {
        struct book8gui_rect panel = menu_panel_rect(1u);
        struct book8gui_rect track = ui_scroll_track(&panel);
        if (programs_total() > menu_visible_rows(1u) && inside(&track, x, y)) {
            if (pressed) {
                desktop.menu_drag = true;
                desktop.menu_grab = ui_scrollbar_grab(&track, programs_total(),
                    menu_visible_rows(1u), programs_first, y);
                programs_first = ui_scrollbar_drag_position(&track, programs_total(),
                    menu_visible_rows(1u), y, desktop.menu_grab);
            }
            return true;
        }
    }
    if (!pressed) {
        struct book8gui_rect panel = menu_panel_rect(0u);
        if (inside(&panel, x, y)) { return true; }
        if (desktop.open_submenu != DESKTOP_NO_SUBMENU) {
            panel = menu_panel_rect(1u);
            if (inside(&panel, x, y)) { return true; }
        }
        return false;
    }
    if (desktop.open_submenu != DESKTOP_NO_SUBMENU) {
        const struct book8gui_menu_item *parent =
            &start_menu[desktop.open_submenu];
        for (uint32_t i = 0u; i < menu_visible_rows(1u); ++i) {
            struct book8gui_rect row = menu_row_rect(1u, i);
            if (inside(&row, x, y)) {
                if (desktop.open_submenu != 0u && run_power_item(&parent->children[i])) {
                    return true;
                }
                uint32_t index = desktop.open_submenu == 0u ? programs_first + i : i;
                const char *executable = NULL;
                const struct book8gui_app_class *launch_class = NULL;
                if (desktop.open_submenu == 0u) {
                    if (registry_programs_active) {
                        uint32_t program_index = menu_program_index(index);
                        if (program_index < registered_count) {
                            const struct book8gui_program *program =
                                &registered_programs[program_index];
                            if (program->launch_class != NULL) {
                                struct WindowApp *app =
                                    launch_app(program->launch_class, NULL);
                                close_launcher();
                                set_focus(app);
                                return true;
                            }
                            executable = program->path;
                        }
                    } else {
                        if (index < PROGRAMS_MENU_COUNT) {
                            launch_class = programs_menu[index].launch;
                            executable = programs_menu[index].executable_path;
                        } else {
                            executable = registered_programs[
                                menu_program_index(index)].path;
                        }
                    }
                    if (launch_class != NULL) {
                        struct WindowApp *app = launch_app(launch_class, NULL);
                        close_launcher();
                        set_focus(app);
                        return true;
                    }
                }
                if (executable != NULL && executable[0] != '\0') {
                    char path[BOOK8GUI_PROGRAM_PATH_SIZE];
                    snprintf(path, sizeof(path), "%s", executable);
                    close_launcher();
                    if (desktop.platform->launch_program == NULL) {
                        program_report("Program launch failed: ELF launcher is unavailable.");
                    } else {
                        (void)desktop.platform->launch_program(path);
                    }
                    book8gui_invalidate();
                    return true;
                }
                if (desktop.open_submenu != 0u) {
                    struct WindowApp *app =
                        launch_app(parent->children[index].launch, NULL);
                    close_launcher();
                    set_focus(app);
                    return true;
                }
                program_report("Program has no registered launch target.");
                return true;
            }
        }
    }
    for (uint32_t i = 0u; i < START_MENU_COUNT; ++i) {
        struct book8gui_rect row = menu_row_rect(0u, i);
        if (!inside(&row, x, y)) {
            continue;
        }
        if (start_menu[i].kind == BOOK8GUI_MENU_SUBMENU) {
            desktop.open_submenu = desktop.open_submenu == i
                ? DESKTOP_NO_SUBMENU : (uint8_t)i;
            book8gui_invalidate();
        } else if (!run_power_item(&start_menu[i])) {
            struct WindowApp *app = launch_app(start_menu[i].launch, NULL);
            close_launcher();
            set_focus(app);
        }
        return true;
    }

    /* Start button itself toggles the menu closed. */
    if (y >= (int32_t)menu_top() && x < (int32_t)(DESKTOP_START_WIDTH + 4u)) {
        close_launcher();
        return true;
    }
    struct book8gui_rect panel = menu_panel_rect(0u);
    if (inside(&panel, x, y)) {
        return true;
    }
    if (desktop.open_submenu != DESKTOP_NO_SUBMENU) {
        panel = menu_panel_rect(1u);
        if (inside(&panel, x, y)) {
            return true;
        }
    }
    close_launcher();
    return false;
}

static void dispatch_mouse_to_focused(const struct book8gui_event *event)
{
    struct WindowApp *focused = find_app(desktop.focused_id);
    if (focused != NULL && focused->handle_event != NULL) {
        (void)focused->handle_event(desktop.platform, focused, event);
    }
}

static int32_t clamp_i32(int32_t value, int32_t low, int32_t high)
{
    if (high < low) {
        high = low;
    }
    return value < low ? low : (value > high ? high : value);
}

static uint8_t resize_edges_at(const struct WindowApp *app, int32_t x,
    int32_t y)
{
    int32_t left = (int32_t)app->bounds.x;
    int32_t top = (int32_t)app->bounds.y;
    int32_t right = left + (int32_t)app->bounds.width;
    int32_t bottom = top + (int32_t)app->bounds.height;
    int32_t border = (int32_t)DESKTOP_RESIZE_BORDER;
    uint8_t edges = DRAG_NONE;
    if (x < left || x >= right || y < top || y >= bottom) {
        return DRAG_NONE;
    }
    if (x < left + border) {
        edges |= DRAG_LEFT;
    } else if (x >= right - border) {
        edges |= DRAG_RIGHT;
    }
    if (y < top + border) {
        edges |= DRAG_TOP;
    } else if (y >= bottom - border) {
        edges |= DRAG_BOTTOM;
    }
    return edges;
}

static bool in_title_bar(const struct WindowApp *app, int32_t x, int32_t y)
{
    struct book8gui_rect bar = {
        app->bounds.x + 2u, app->bounds.y + 2u,
        app->bounds.width - 4u, DESKTOP_TITLEBAR_HEIGHT
    };
    return inside(&bar, x, y);
}

static void begin_drag(const struct WindowApp *app, uint8_t mode)
{
    desktop.drag_id = app->id;
    desktop.drag_mode = mode;
    desktop.drag_origin_x = desktop.mouse_x;
    desktop.drag_origin_y = desktop.mouse_y;
    desktop.drag_bounds = app->bounds;
}

static void update_drag(const struct book8gui_event *event)
{
    struct WindowApp *app = find_app(desktop.drag_id);
    if (app == NULL || (event->mouse_buttons & 1u) == 0u) {
        desktop.drag_mode = DRAG_NONE;
        return;
    }
    int32_t dx = desktop.mouse_x - desktop.drag_origin_x;
    int32_t dy = desktop.mouse_y - desktop.drag_origin_y;
    int32_t screen_w = (int32_t)desktop.platform->width;
    int32_t work_top = (int32_t)DESKTOP_TOP_HEIGHT;
    int32_t work_bottom = (int32_t)(desktop.platform->height
        - DESKTOP_TASKBAR_HEIGHT);
    int32_t left = (int32_t)desktop.drag_bounds.x;
    int32_t top = (int32_t)desktop.drag_bounds.y;
    int32_t right = left + (int32_t)desktop.drag_bounds.width;
    int32_t bottom = top + (int32_t)desktop.drag_bounds.height;

    if (desktop.drag_mode == DRAG_MOVE) {
        int32_t width = right - left;
        int32_t height = bottom - top;
        left = clamp_i32(left + dx, 0, screen_w - width);
        top = clamp_i32(top + dy, work_top, work_bottom - height);
        right = left + width;
        bottom = top + height;
    } else {
        if ((desktop.drag_mode & DRAG_LEFT) != 0u) {
            left = clamp_i32(left + dx, 0,
                right - (int32_t)DESKTOP_MIN_WIDTH);
        }
        if ((desktop.drag_mode & DRAG_RIGHT) != 0u) {
            right = clamp_i32(right + dx,
                left + (int32_t)DESKTOP_MIN_WIDTH, screen_w);
        }
        if ((desktop.drag_mode & DRAG_TOP) != 0u) {
            top = clamp_i32(top + dy, work_top,
                bottom - (int32_t)DESKTOP_MIN_HEIGHT);
        }
        if ((desktop.drag_mode & DRAG_BOTTOM) != 0u) {
            bottom = clamp_i32(bottom + dy,
                top + (int32_t)DESKTOP_MIN_HEIGHT, work_bottom);
        }
    }
    app->bounds.x = (uint32_t)left;
    app->bounds.y = (uint32_t)top;
    app->bounds.width = (uint32_t)(right - left);
    app->bounds.height = (uint32_t)(bottom - top);
}

static void handle_mouse(const struct book8gui_event *event)
{
    bool moved = set_mouse_position(event->mouse_x, event->mouse_y);
    bool buttons_changed = desktop.mouse_buttons != event->mouse_buttons;
    if (buttons_changed || event->wheel_delta != 0) {
        book8gui_invalidate();
    }

    if (desktop.launcher_open && desktop.open_submenu == 0u
        && event->wheel_delta != 0) {
        struct book8gui_rect panel = menu_panel_rect(1u);
        if (inside(&panel, desktop.mouse_x, desktop.mouse_y)) {
            programs_first = ui_wheel_scroll_delta(programs_first, programs_total(),
                menu_visible_rows(1u), event->wheel_delta, 1u, &desktop.menu_wheel_remainder);
            desktop.mouse_buttons = event->mouse_buttons;
            return;
        }
    }
    if (desktop.drag_mode != DRAG_NONE) {
        update_drag(event);
        if (moved) {
            book8gui_invalidate();
        }
        desktop.mouse_buttons = event->mouse_buttons;
        return;
    }

    if (handle_audio_popup_event(event)) {
        desktop.mouse_buttons = event->mouse_buttons;
        return;
    }
    if (desktop.launcher_open && handle_launcher_click(event)) {
        desktop.mouse_buttons = event->mouse_buttons;
        return;
    }
    if (hit_taskbar((uint32_t)desktop.mouse_x,
        (uint32_t)desktop.mouse_y)) {
        handle_taskbar(event);
        if (event->wheel_delta == 0) {
            dispatch_mouse_to_focused(event);
        }
        desktop.mouse_buttons = event->mouse_buttons;
        return;
    }

    bool pressed = (event->mouse_buttons & 1u) != 0u
        && (desktop.mouse_buttons & 1u) == 0u;
    struct WindowApp *hit = window_at(desktop.mouse_x, desktop.mouse_y);
    bool right_pressed = (event->mouse_buttons & 2u) != 0u
        && (desktop.mouse_buttons & 2u) == 0u;
    if (desktop.context_open && pressed) {
        bool paste = desktop.mouse_x >= (int32_t)desktop.context_x
            && desktop.mouse_x < (int32_t)(desktop.context_x + 150u)
            && desktop.mouse_y >= (int32_t)desktop.context_y
            && desktop.mouse_y < (int32_t)(desktop.context_y + 24u);
        desktop.context_open = false;
        if (paste) {
            (void)book8gui_clipboard_paste("A:\\");
            if (desktop.platform->diagnostic != NULL) {
                desktop.platform->diagnostic(book8gui_clipboard_status());
                desktop.platform->diagnostic("\n");
            }
        }
        desktop.mouse_buttons = event->mouse_buttons;
        return;
    }
    if (right_pressed && hit == NULL) {
        uint32_t x = (uint32_t)desktop.mouse_x;
        uint32_t y = (uint32_t)desktop.mouse_y;
        desktop.context_x = x + 150u <= desktop.platform->width
            ? x : desktop.platform->width - 150u;
        uint32_t bottom = desktop.platform->height - DESKTOP_TASKBAR_HEIGHT;
        desktop.context_y = y + 24u <= bottom ? y : bottom - 24u;
        desktop.context_open = true;
        desktop.launcher_open = false;
        desktop.mouse_buttons = event->mouse_buttons;
        return;
    }
    if (right_pressed && hit != NULL)
        set_focus(hit);
    if (pressed && hit != NULL) {
        struct book8gui_rect close = close_rect(hit);
        if (inside(&close, desktop.mouse_x, desktop.mouse_y)) {
            if (hit->cls->can_close == NULL || hit->cls->can_close(desktop.platform, hit)) {
                close_window(hit);
            }
            desktop.mouse_buttons = event->mouse_buttons;
            return;
        }
        struct book8gui_rect maximize = maximize_rect(hit);
        if (inside(&maximize, desktop.mouse_x, desktop.mouse_y)) {
            toggle_maximize(hit);
            desktop.mouse_buttons = event->mouse_buttons;
            return;
        }
        struct book8gui_rect minimize = minimize_rect(hit);
        if (inside(&minimize, desktop.mouse_x, desktop.mouse_y)) {
            minimize_window(hit);
            desktop.mouse_buttons = event->mouse_buttons;
            return;
        }
        set_focus(hit);
        uint8_t edges = hit->maximized ? DRAG_NONE
            : resize_edges_at(hit, desktop.mouse_x, desktop.mouse_y);
        if (edges != DRAG_NONE) {
            begin_drag(hit, edges);
            desktop.mouse_buttons = event->mouse_buttons;
            return;
        }
        if (!hit->maximized
            && in_title_bar(hit, desktop.mouse_x, desktop.mouse_y)) {
            begin_drag(hit, DRAG_MOVE);
            desktop.mouse_buttons = event->mouse_buttons;
            return;
        }
        if (desktop.platform->diagnostic != NULL) {
            report_input_route(hit, event);
        }
    }
    if (event->wheel_delta != 0) {
        if (hit != NULL && hit->handle_event != NULL) {
            (void)hit->handle_event(desktop.platform, hit, event);
        }
    } else {
        dispatch_mouse_to_focused(event);
    }
    desktop.mouse_buttons = event->mouse_buttons;
}

static void handle_event_inner(const struct book8gui_event *event)
{
    if (event->type == BOOK8GUI_EVENT_KEY
        && event->key == (BOOK8GUI_KEY_TAB | BOOK8GUI_KEY_SHIFT)) {
        for (uint32_t i = 0u; i < desktop.app_count; ++i) {
            if (desktop.apps[i].focused) {
                for (uint32_t step = 1u; step < desktop.app_count; ++step) {
                    struct WindowApp *next =
                        &desktop.apps[(i + step) % desktop.app_count];
                    if (next->visible) {
                        set_focus(next);
                        return;
                    }
                }
            }
        }
        return;
    }
    if (desktop.modal_id != 0u) {
        struct WindowApp *owner = find_app(desktop.modal_id);
        if (owner != NULL && owner->handle_event != NULL) {
            if (event->type == BOOK8GUI_EVENT_MOUSE) {
                bool buttons_changed =
                    desktop.mouse_buttons != event->mouse_buttons;
                (void)set_mouse_position(event->mouse_x, event->mouse_y);
                desktop.mouse_buttons = event->mouse_buttons;
                if (buttons_changed || event->wheel_delta != 0) {
                    book8gui_invalidate();
                }
            }
            (void)owner->handle_event(desktop.platform, owner, event);
        }
        return;
    }
    if (event->type == BOOK8GUI_EVENT_KEY
        && event->key == (BOOK8GUI_KEY_ESCAPE | BOOK8GUI_KEY_CTRL
            | BOOK8GUI_KEY_ALT)) {
        desktop.launcher_open = false;
        desktop.context_open = false;
        clipboard.source[0] = '\0';
        clipboard.status[0] = '\0';
        struct WindowApp *manager = book8gui_launch_class(
            &book8_taskmgr_class);
        if (manager != NULL && desktop.platform->diagnostic != NULL) {
            report_window_message("BOOK8-DESKTOP: hotkey Task Manager id=",
                manager->id, manager->title);
        }
        return;
    }
    if (event->type == BOOK8GUI_EVENT_KEY
        && event->key == BOOK8GUI_KEY_ESCAPE && desktop.audio_popup_open) {
        desktop.audio_popup_open = false;
        desktop.audio_dragging = false;
        book8gui_invalidate();
        return;
    }
    if (event->type == BOOK8GUI_EVENT_KEY) {
        struct WindowApp *owner = find_app(desktop.focused_id);
        if (owner != NULL && owner->cls != NULL
            && owner->cls->wants_all_keys && owner->handle_event != NULL) {
            if (owner->handle_event(desktop.platform, owner, event)) {
                return;
            }
            if (event->key != BOOK8GUI_KEY_ESCAPE
                && event->key != BOOK8GUI_KEY_TAB) {
                return;
            }
        }
    }
    if (event->type == BOOK8GUI_EVENT_KEY
        && event->key == BOOK8GUI_KEY_TAB) {
        for (uint32_t i = 0u; i < desktop.app_count; ++i) {
            if (desktop.apps[i].focused) {
                for (uint32_t step = 1u; step < desktop.app_count; ++step) {
                    struct WindowApp *next =
                        &desktop.apps[(i + step) % desktop.app_count];
                    if (next->visible) {
                        set_focus(next);
                        break;
                    }
                }
                return;
            }
        }
    }
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        handle_mouse(event);
        return;
    }
    struct WindowApp *focused = find_app(desktop.focused_id);
    if (focused != NULL && focused->handle_event != NULL) {
        (void)focused->handle_event(desktop.platform, focused, event);
    }
}

static void handle_event(const struct book8gui_event *event)
{
    uint32_t owner_id = desktop.focused_id;
    uint64_t started = ticks_now();
    handle_event_inner(event);
    struct WindowApp *owner = find_app(owner_id);
    if (owner != NULL) {
        owner->cpu_ticks += ticks_now() - started;
    }
    apply_close_request();
}

static void initialize_windows(void)
{
    if (book8_session_login_required()) {
        struct WindowApp *login = launch_app(&book8_login_class, NULL);
        if (login != NULL) { book8gui_set_modal(login, true); }
    }
    if (desktop.platform->diagnostic != NULL) {
        desktop.platform->diagnostic(
            "BOOK8-DESKTOP: TASKBAR height=36 reserved\n");
    }
}

bool book8gui_run(const struct book8gui_platform *platform)
{
    if (platform == NULL || platform->width < 640u
        || platform->height < 300u || platform->fill_rect == NULL
        || platform->draw_text == NULL || platform->poll_event == NULL
        || platform->usb_mouse_active == NULL
        || platform->run_command == NULL || platform->list_directory == NULL) {
        return false;
    }
    active_platform = *platform;
    desktop.platform = &active_platform;
    platform = desktop.platform;
    display_preview.active = false;
    display_preview.status[0] = '\0';
    desktop.app_count = 0u;
    desktop.next_id = 1u;
    desktop.launch_count = 0u;
    desktop.focused_id = 0u;
    desktop.modal_id = 0u;
    desktop.open_submenu = DESKTOP_NO_SUBMENU;
    desktop.mouse_x = -1;
    desktop.mouse_y = -1;
    desktop.cursor_saved_x = -1;
    desktop.cursor_saved_y = -1;
    desktop.mouse_buttons = 0u;
    desktop.launcher_open = false;
    desktop.dirty = true;
    desktop.close_requested = false;
    desktop.close_request_id = 0u;
    desktop.status_flags = 0u;
    desktop.network_valid = desktop.tx_lit = desktop.rx_lit = false;
    desktop.network_tx = desktop.network_rx = 0u;
    desktop.tx_until = desktop.rx_until = 0u;
    desktop.audio_available = false;
    desktop.audio_muted = true;
    desktop.audio_popup_open = false;
    desktop.audio_dragging = false;
    desktop.audio_volume = 0u;
    desktop.time_valid = false;
    desktop.busy_ticks = 0u;
    desktop.desktop_cpu_permille = 0u;
    desktop.last_disk_read = 0u;
    desktop.last_disk_write = 0u;
    desktop.last_disk_busy = 0u;
    desktop.session_start = ticks_now();
    if (!book8_session_login_required()) {
        (void)book8gui_refresh_programs();
    }
    (void)book8gui_open_pending_utility();
    desktop.sample_start = desktop.session_start;
    desktop.telemetry = (struct book8gui_telemetry){0};
    poll_taskbar_status();
    initialize_windows();
    render_desktop();
    desktop.dirty = false;
    if (platform->present != NULL) {
        platform->present();
    }

    bool paced = platform->clock_ticks != NULL
        && platform->clock_hz >= BOOK8GUI_FRAME_HZ;
    uint64_t frame_period = paced
        ? platform->clock_hz / BOOK8GUI_FRAME_HZ : 0u;
    uint64_t input_period = platform->clock_hz / BOOK8GUI_INPUT_HZ;
    if (input_period == 0u) {
        input_period = 1u;
    }
    uint64_t now = platform->clock_ticks != NULL
        ? platform->clock_ticks() : 0u;
    uint64_t next_frame = paced ? now + frame_period : 0u;
    uint64_t next_input = now;
    uint64_t next_status = now + platform->clock_hz;

    for (;;) {
        now = platform->clock_ticks != NULL
            ? platform->clock_ticks() : 0u;
        if (platform->pump_input != NULL
            && (!paced || now >= next_input)) {
            platform->pump_input();
            next_input = paced ? now + input_period : 0u;
        }
        if (paced && now < next_frame) {
            __asm__ volatile ("pause");
            continue;
        }
        if (paced) {
            next_frame = now - next_frame > frame_period * 4u
                ? now + frame_period : next_frame + frame_period;
        }
        poll_network_activity(now);
        if (platform->clock_ticks != NULL && platform->clock_hz != 0u
            && now >= next_status) {
            next_status = now + platform->clock_hz;
            poll_taskbar_status();
            sample_telemetry(now);
        }
        uint64_t work_start = now;
        bool preview_expired = display_timer();
        bool worked = desktop.dirty;
        for (uint32_t handled = 0u;
            handled < BOOK8GUI_MAX_EVENTS_PER_FRAME; ++handled) {
            struct book8gui_event event;
            if (!platform->poll_event(&event)) {
                break;
            }
            worked = true;
            bool was_preview = display_preview.active;
            bool timed_out = display_timer();
            if (preview_expired) {
                continue;
            } else if (was_preview) {
                if (!timed_out && display_preview.active) { display_event(&event); }
            } else {
                handle_event(&event);
            }
            if (desktop.close_requested) {
                destroy_all_windows();
                return true;
            }
        }
        for (uint32_t i = 0u; i < desktop.app_count; ++i) {
            if (desktop.apps[i].cls->tick != NULL) {
                desktop.apps[i].cls->tick(platform, &desktop.apps[i]);
            }
        }
        if (platform->media_service != NULL) {
            platform->media_service();
        }
        if (desktop.dirty) {
            desktop.dirty = false;
            render_desktop();
            desktop.cursor_dirty = false;
            if (platform->present != NULL) {
                platform->present();
            }
            worked = true;
        } else if (desktop.cursor_dirty) {
            desktop.cursor_dirty = false;
            if (!update_cursor()) {
                render_desktop();
                if (platform->present != NULL) {
                    platform->present();
                }
            }
            worked = true;
        }
        if (worked) {
            desktop.busy_ticks += ticks_now() - work_start;
        }
    }
}

void book8gui_console_char(char value)
{
    book8_shell_append_char(value);
}

void book8gui_console_clear(void)
{
    book8_shell_clear();
}
