#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../src/apps/book8_settings.h"
#include "../src/session.h"
#include "../src/apps/book8_ui.h"
#include "../src/apps/book8_paintbook.h"
#include "../include/syscall.h"
#include "../net/network_settings.h"

static struct book8gui_platform platform;
static struct WindowApp *settings_app;
static struct book8gui_rect list, track;
static uint32_t rows, stage, phase, launches, scan_bins;
static uint64_t now;
static bool pause_event, directory_failure, missing_metadata;
static char first_row[96], details_name[96], launched[256];
static uint32_t direct_menu_labels;
static uint32_t limit_fixture;
static bool static_menu_only;
static uint32_t program_policy_calls;
static uint32_t program_deletes;
static const char deep_name[] = "DEEP_DIRECTORY_LEVEL_00";
static char deep_binary[BOOK8GUI_PROGRAM_PATH_SIZE];

bool book8_network_settings_get(struct book8_network_status *s) { (void)s; return false; }
bool book8_network_settings_apply(const struct book8_network_settings *s, bool p)
{ (void)s; (void)p; return false; }
bool book8_network_settings_renew(void) { return false; }
static void command(char *s) { (void)s; }
static bool mouse(void) { return false; }
static void diagnostic(const char *s) { (void)s; }
static uint64_t clock_ticks(void) { return now; }
static void pump(void) { ++now; assert(now < 1000u); }
static uint32_t drives(void) { return 2u; }
static char letter(uint32_t i) { return (char)('A' + i); }
static bool readable(uint32_t i) { return i == 0u; }
static bool program_readable(uint32_t i)
{
    ++program_policy_calls;
    return i == 0u;
}
static bool metadata(const char *path, struct book8_file_info *info)
{
    if (missing_metadata && strstr(path, "APP12.ELF")) { return false; }
    *info = (struct book8_file_info){.allocated_bytes = 4096u, .allocation_valid = true};
    return true;
}
static bool directory(const char *path, book8_directory_entry_fn visit, void *ctx)
{
    assert(path[0] == 'A');
    if (static_menu_only) { return true; }
    if (limit_fixture != 0u) {
        const char *component = limit_fixture == 4u ? "D" : deep_name;
        if (!strcmp(path, "A:\\")) {
            if (limit_fixture <= 2u) { assert(visit("BIN", true, 0u, ctx)); }
            if (limit_fixture == 1u) {
                for (uint32_t i = 0u; i < 140u; ++i) {
                    char name[16];
                    snprintf(name, sizeof(name), "DIR%03u", i);
                    assert(visit(name, true, 0u, ctx));
                }
            }
            return limit_fixture == 2u || visit(component, true, 0u, ctx);
        }
        if (!strcmp(path, "A:\\BIN\\")) {
            uint32_t count = limit_fixture == 2u ? BOOK8GUI_MAX_PROGRAMS : 300u;
            for (uint32_t i = 0u; i < count; ++i) {
                char name[16];
                snprintf(name, sizeof(name), "APP%03u.ELF", i);
                assert(visit(name, false, 1u, ctx));
            }
        } else if (!strncmp(path + 3u, component, strlen(component))) {
            uint32_t depth = (uint32_t)((strlen(path) - 3u) / (strlen(component) + 1u));
            if (depth < (limit_fixture == 1u ? 16u : 40u)) {
                return visit(component, true, 0u, ctx);
            }
            snprintf(deep_binary, sizeof(deep_binary), "%sDEEP.ELF", path);
            return visit("DEEP.ELF", false, 1u, ctx);
        }
        return true;
    }
    if (!strcmp(path, "A:\\")) {
        assert(visit("boot", true, 0u, ctx));
        assert(visit("EFI", true, 0u, ctx));
        assert(visit("DRIVERS", true, 0u, ctx));
        assert(visit("MODULES", true, 0u, ctx));
        assert(visit("NVMAP.ELF", false, 1u, ctx));
        assert(visit("book8.elf", false, 1u, ctx));
        assert(visit("KERNEL.ELF", false, 1u, ctx));
        assert(visit("CAT.ELF", false, 1u, ctx));
        return visit("", false, 0u, ctx) && visit(".", true, 0u, ctx)
            && visit("..", true, 0u, ctx) && visit("BAD\\PATH.ELF", false, 1u, ctx)
            && visit("BIN", true, 0u, ctx);
    }
    assert(!strcmp(path, "A:\\BIN\\"));
    ++scan_bins;
    if (directory_failure) { return false; }
    static const char padded[] = "HEXVIEW.ELF\0ignored trailing storage";
    assert(visit(padded, false, 1u, ctx));
    assert(visit("hexview.elf", false, 1u, ctx)); /* Duplicate must not consume an index. */
    assert(visit("CODEEDIT.ELF", false, 1u, ctx));
    assert(visit("NETSHARE.ELF", false, 1u, ctx));
    assert(visit("PAINTBK.ELF", false, 1u, ctx));
    assert(visit("BROWSER.ELF", false, 1u, ctx));
    assert(visit("NVDIAG.ELF", false, 1u, ctx));
    assert(visit("NVMAP.ELF", false, 1u, ctx));
    for (uint32_t i = 0u; i < 70u; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "APP%02u.ELF", i);
        assert(visit(name, false, 1u, ctx));
    }
    return true;
}
static bool launch(const char *path)
{
    ++launches;
    snprintf(launched, sizeof(launched), "%s", path);
    return true;
}
static uint32_t session_uid(void)
{
    struct book8_session_info info;
    assert(book8_session_current(&info));
    return info.uid;
}
static uint64_t session_token(void)
{
    struct book8_session_info info;
    assert(book8_session_current(&info));
    return info.token;
}
static bool delete_program(const char *path)
{
    assert(strstr(path, "PAINTBK.ELF") || strstr(path, "BROWSER.ELF")
        || strstr(path, "NVDIAG.ELF"));
    ++program_deletes;
    return true;
}
static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    (void)color;
    assert(x <= platform.width && w <= platform.width - x);
    assert(y <= platform.height && h <= platform.height - y);
}
static void text(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg; (void)bg;
    assert(x < platform.width && y + 16u <= platform.height);
    if (x == 162u) {
        assert(strcmp(s, "Utilities >") != 0);
        if (!strcmp(s, "HexViewer")) { direct_menu_labels |= 1u; }
        if (!strcmp(s, "CodeEditor")) { direct_menu_labels |= 2u; }
        if (!strcmp(s, "NetShare")) { direct_menu_labels |= 4u; }
        if (!strcmp(s, "CAT")) { direct_menu_labels |= 8u; }
    }
    if (settings_app && x == list.x + 4u && y == list.y + 6u) {
        snprintf(first_row, sizeof(first_row), "%s", s);
    }
    if (settings_app && x == list.x + list.width + 10u && y == list.y + 4u) {
        snprintf(details_name, sizeof(details_name), "%s", s);
    }
}
static void mouse_event(struct book8gui_event *e, int32_t x, int32_t y, uint8_t b)
{
    *e = (struct book8gui_event){.type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = x, .mouse_y = y, .mouse_buttons = b};
}
static bool event(struct book8gui_event *e)
{
    if (pause_event) { pause_event = false; return false; }
    pause_event = true;
    if (static_menu_only) {
        static const char *const paths[] = {"A:\\BIN\\HEXVIEW.ELF",
            "A:\\BIN\\CODEEDIT.ELF", "A:\\BIN\\NETSHARE.ELF", "A:\\CAT.ELF"};
        uint32_t step = stage++;
        if (step < 24u) {
            uint32_t app = step / 6u, action = step % 6u;
            if (action == 0u && app != 0u) {
                assert(launches == app && !strcmp(launched, paths[app - 1u]));
            }
            int32_t x = action < 2u ? 20 : action < 4u ? 40 : 180;
            int32_t y = action < 2u ? (int32_t)platform.height - 18
                : action < 4u ? (int32_t)platform.height - 90
                : (int32_t)platform.height - 36 - 184
                    + (int32_t)(4u + app) * 20 + 10;
            mouse_event(e, x, y, action % 2u == 0u ? 1u : 0u);
        } else {
            assert(book8gui_program_count() == 5u);
            assert(direct_menu_labels == 15u && launches == 4u);
            assert(!strcmp(launched, paths[3]));
            uint32_t action = step - 24u;
            mouse_event(e, action < 2u ? 20 : action < 4u ? 40 : 200,
                (int32_t)platform.height - (action < 2u ? 18 : 50),
                action % 2u == 0u ? 1u : 0u);
        }
        return true;
    }
    if (phase == 0u && stage == 0u) {
        assert(book8gui_program_count() == 82u);
        for (uint32_t i = 5u; i < book8gui_program_count(); ++i) {
            struct book8gui_program info;
            assert(book8gui_program_info(i, &info));
            assert(strstr(info.path, "NVMAP") == NULL && strstr(info.path, "KERNEL") == NULL);
        }
        assert(book8gui_utility_installed(1u) && book8gui_utility_installed(2u)
            && book8gui_utility_installed(3u) && book8gui_utility_installed(4u)
            && book8gui_utility_installed(5u)
            && book8gui_utility_installed(BOOK8_GUI_NVDIAG));
        assert(book8gui_file_app(4u) == &book8_paintbook_class);
        assert(strstr(book8gui_program_status(), "metadata unavailable: 1"));
        assert(strstr(book8gui_program_status(), "unreadable drives skipped: 1"));
        settings_app = book8gui_launch_class(&book8_settings_class);
        assert(settings_app);
        settings_app->bounds = (struct book8gui_rect){20u, 20u,
            platform.width - 40u, platform.height - 60u};
        struct book8gui_rect c = ui_client(settings_app);
        uint32_t side = c.width >= 520u ? 220u : c.width / 3u;
        struct book8gui_rect content = {c.x + side + 8u, c.y + 46u,
            c.width > side + 8u ? c.width - side - 8u : 0u,
            c.height > 82u ? c.height - 82u : 0u};
        list = (struct book8gui_rect){content.x, content.y,
            content.width / 2u, content.height};
        track = ui_scroll_track(&list);
        rows = (list.height - 8u) / 22u;
    }
    *e = (struct book8gui_event){.type = BOOK8GUI_EVENT_KEY, .key = BOOK8GUI_KEY_PAGE_DOWN};
    if (phase == 0u) {
        switch (stage++) {
        case 0u: break;
        case 1u:
            break;
        case 2u:
            assert(!strcmp(first_row, "File Explorer"));
            mouse_event(e, (int32_t)track.x + 2, (int32_t)track.y + 1, 1u); break;
        case 3u:
            assert(!strcmp(first_row, "File Explorer"));
            mouse_event(e, (int32_t)list.x - 60, (int32_t)(track.y + track.height + 50u), 1u); break;
        case 4u: {
            char expected[16]; snprintf(expected, sizeof(expected), "APP%02u.ELF", 70u - rows);
            assert(!strcmp(first_row, expected));
            assert(!strcmp(details_name, "File Explorer"));
            mouse_event(e, (int32_t)list.x - 60, (int32_t)(track.y + track.height + 50u), 0u); break;
        }
        case 5u:
            mouse_event(e, (int32_t)list.x + 8, (int32_t)list.y + 10, 0u);
            e->wheel_delta = book8_wheel_detents(INT8_MAX); break;
        case 6u:
            assert(!strcmp(first_row, "File Explorer"));
            mouse_event(e, (int32_t)list.x + 8, (int32_t)list.y + 10, 0u);
            e->wheel_delta = book8_wheel_detents(INT8_MIN); break;
        case 7u:
            mouse_event(e, (int32_t)list.x + 8, (int32_t)(list.y + list.height - 1u), 1u); break;
        case 8u:
            assert(!strcmp(details_name, "File Explorer"));
            mouse_event(e, (int32_t)list.x + 8, (int32_t)(list.y + list.height - 1u), 0u); break;
        default:
            book8gui_end_task(settings_app->id); settings_app = NULL;
            phase = 1u; stage = 0u; pause_event = false;
            return false;
        }
    } else {
        uint32_t bottom = platform.height - BOOK8GUI_TASKBAR_HEIGHT;
        uint32_t visible = (bottom - 6u) / 20u;
        uint32_t menu_y = bottom - 2u - (visible * 20u + 4u);
        uint32_t menu_x = 152u, menu_width = 224u;
        struct book8gui_rect menu = {menu_x, menu_y, menu_width, visible * 20u + 4u};
        struct book8gui_rect bar = ui_scroll_track(&menu);
        switch (stage++) {
        case 0u: mouse_event(e, 20, (int32_t)platform.height - 18, 1u); break;
        case 1u: mouse_event(e, 20, (int32_t)platform.height - 18, 0u); break;
        case 2u: mouse_event(e, 40, (int32_t)platform.height - 90, 1u); break;
        case 3u: mouse_event(e, 40, (int32_t)platform.height - 90, 0u); break;
        case 4u:
            assert(direct_menu_labels == 15u);
            mouse_event(e, (int32_t)bar.x + 2, (int32_t)bar.y + 1, 1u); break;
        case 5u:
            mouse_event(e, (int32_t)bar.x + 50, (int32_t)(bar.y + bar.height + 50u), 1u); break;
        case 6u:
            mouse_event(e, (int32_t)bar.x + 50, (int32_t)(bar.y + bar.height + 50u), 0u); break;
        case 7u:
            mouse_event(e, 180, (int32_t)(menu_y + 2u + (visible - 1u) * 20u + 10u), 1u); break;
        case 8u:
            assert(launches == 1u && !strcmp(launched, "A:\\BIN\\APP69.ELF"));
            {
                struct book8gui_window_info windows[2];
                assert(book8gui_window_snapshot(windows, 2u) == 1u);
            }
            mouse_event(e, 180, 100, 0u); break;
        case 9u: mouse_event(e, 20, (int32_t)platform.height - 18, 1u); break;
        case 10u: mouse_event(e, 20, (int32_t)platform.height - 18, 0u); break;
        case 11u: mouse_event(e, 40, (int32_t)platform.height - 50, 1u); break;
        case 12u: mouse_event(e, 40, (int32_t)platform.height - 50, 0u); break;
        case 13u: mouse_event(e, 200, (int32_t)platform.height - 50, 1u); break;
        default:
            return false;
        }
    }
    return true;
}
int main(void)
{
    assert(book8_session_initialize(0x475549u));
    assert(book8_session_login("admin", "default"));
    assert(ui_wheel_scroll(0u, 80u, 10u, INT_MIN, 3u) == 70u);
    assert(ui_wheel_scroll(70u, 80u, 10u, INT_MAX, 3u) == 0u);
    platform = (struct book8gui_platform){.width = 640u, .height = 300u,
        .fill_rect = fill, .draw_text = text, .pump_input = pump, .poll_event = event,
        .usb_mouse_active = mouse, .run_command = command, .list_directory = directory,
        .file_info = metadata, .drive_count = drives, .drive_letter = letter,
        .drive_readable = readable, .program_drive_readable = program_readable,
        .diagnostic = diagnostic, .clock_ticks = clock_ticks,
        .session_uid = session_uid, .session_token = session_token,
        .clock_hz = 100u, .launch_program = launch,
        .delete_entry = delete_program};
    missing_metadata = true;
    assert(book8gui_run(&platform));
    platform.width = 800u; platform.height = 600u;
    now = stage = phase = launches = direct_menu_labels = 0u; pause_event = false;
    assert(book8gui_run(&platform));
    directory_failure = true;
    assert(!book8gui_refresh_programs());
    assert(strstr(book8gui_program_status(), "scan incomplete"));
    assert(strstr(book8gui_program_status(), "A:\\BIN\\"));
    assert(scan_bins >= 2u);
    limit_fixture = 1u;
    missing_metadata = false;
    assert(book8gui_refresh_programs());
    assert(book8gui_program_count() == 306u);
    assert(strlen(deep_binary) > 256u);
    struct book8gui_program deep;
    assert(book8gui_program_info(305u, &deep));
    assert(!strcmp(deep.path, deep_binary));
    assert(strstr(book8gui_program_status(), "Programs indexed: 306;"));
    limit_fixture = 2u;
    assert(!book8gui_refresh_programs());
    assert(book8gui_program_count() == BOOK8GUI_MAX_PROGRAMS);
    assert(strstr(book8gui_program_status(), "registry limit reached"));
    limit_fixture = 3u;
    assert(!book8gui_refresh_programs());
    assert(strstr(book8gui_program_status(), "scan incomplete"));
    limit_fixture = 4u;
    assert(!book8gui_refresh_programs());
    assert(strstr(book8gui_program_status(), "scan incomplete"));
    limit_fixture = 1u;
    program_policy_calls = 0u;
    assert(book8gui_refresh_programs());
    assert(program_policy_calls == 2u);
    assert(book8gui_program_count() == 306u);
    assert(strstr(book8gui_program_status(), "unreadable drives skipped: 1"));
    static_menu_only = true;
    now = stage = launches = direct_menu_labels = 0u; pause_event = false;
    assert(book8gui_run(&platform));
    static_menu_only = false;
    limit_fixture = 0u;
    directory_failure = false;
    assert(book8gui_refresh_programs());
    assert(book8gui_utility_installed(4u) && book8gui_utility_installed(5u)
        && book8gui_utility_installed(BOOK8_GUI_NVDIAG));
    assert(book8gui_remove_program("A:\\BIN\\PAINTBK.ELF"));
    assert(book8gui_remove_program("A:\\BIN\\BROWSER.ELF"));
    assert(book8gui_remove_program("A:\\BIN\\NVDIAG.ELF"));
    assert(program_deletes == 3u);
    assert(!book8gui_utility_installed(4u) && !book8gui_utility_installed(5u)
        && !book8gui_utility_installed(BOOK8_GUI_NVDIAG));
    puts("PASS: menus, app discovery/removal, ELF limits, metadata and scrollbar handling");
    return 0;
}
