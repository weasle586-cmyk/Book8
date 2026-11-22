#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../src/book8gui.h"
#include "../src/mouse_input.h"
#include "../src/apps/book8_ui.h"
#include "../src/apps/book8_editor.h"
#include "../src/apps/book8_player.h"
#include "../src/apps/book8_settings.h"
#include "../src/apps/book8_utilities.h"
#include "../src/registry.h"
#include "../src/session.h"
#include "../include/syscall.h"
#include "../net/network_settings.h"

#define WIDTH 640u
#define HEIGHT 360u
#define TRACE_SIZE 2048u
#define TASKBAR_COLOR 0xc0c0c0u

static uint32_t pixels[WIDTH * HEIGHT];
static uint32_t cursor_under[8u * 12u];
static uint32_t cursor_under_x, cursor_under_y;
static uint32_t cursor_under_width, cursor_under_height;
static uint32_t cursor_present_calls, full_desktop_redraws;
static uint32_t cursor_test_full_start, cursor_test_present_start;
static bool cursor_under_valid, cursor_test;
static struct book8gui_event events[128];
static uint64_t event_ticks[128];
static uint32_t event_count;
static uint32_t event_index;
static uint64_t current_ticks;
static uint32_t pump_count;
static bool spread_events, event_pause, saw_tx_light, saw_rx_light;
static bool many_files, saw_scrolled_first;
static bool saw_multi_scroll, saw_reverse_scroll;
static bool saw_initial_scroll_thumb, saw_moved_scroll_thumb;
static bool saw_binary_save_rejection;
static bool saw_properties_path, saw_properties_size, saw_properties_date;
static bool saw_maximized_bounds;
static unsigned editor_writes;
static char trace[TRACE_SIZE];
static uint32_t trace_length;
static unsigned network_get_calls, network_apply_calls, network_saved_calls;
static unsigned network_renew_calls;
static uint8_t host_audio_volume = 72u;
static bool host_audio_muted;
static unsigned host_audio_volume_calls, host_audio_mute_calls;
static unsigned editor_reads, command_calls;
static unsigned rename_calls, delete_calls;
static unsigned paste_calls;
static bool paste_was_cut;
static struct book8_network_settings host_network_settings = {
    BOOK8_NETWORK_DHCP, 0x0a010203u, 0xffffff00u, 0x0a010001u,
    0x08080808u, 0xc0000201u, 1000u
};
static bool host_network_renew_result = true;
static bool bounds_scenario;
static uint32_t live_width, live_height, display_queries;
static struct WindowApp *bounds_app, *other_app;
static int32_t wheel_sum[2];
static bool editor_scenario;
static bool editor_rows_seen[5];
static const struct book8gui_platform *editor_platform;
static bool programs_scenario, removed_hex, remove_fails;
static bool program_overflow, program_metadata_failure, program_changed_attributes;
static uint32_t program_deletes, program_launches;
static bool saw_program_name, saw_program_path, saw_program_size, saw_program_core;
static bool saw_remove_prompt;
static bool utility_scenario;

static uint32_t host_session_uid(void)
{
    struct book8_session_info info;
    assert(book8_session_current(&info));
    return info.uid;
}

static uint64_t host_session_token(void)
{
    struct book8_session_info info;
    assert(book8_session_current(&info));
    return info.token;
}

static bool launch_program(const char *path)
{
    assert(strcmp(path, "A:\\BIN\\IMGVIEW.ELF") == 0);
    ++program_launches;
    return true;
}

static bool probe_event(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event)
{
    (void)platform;
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        wheel_sum[app == bounds_app ? 0 : 1] += event->wheel_delta;
    }
    return true;
}

static void probe_render(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    (void)platform;
    (void)app;
}

static const struct book8gui_app_class probe_class = {
    .name = "Bounds probe", .default_width = 300u, .default_height = 200u,
    .render = probe_render, .handle_event = probe_event
};

static void display_size(uint32_t *width, uint32_t *height)
{
    ++display_queries;
    *width = live_width;
    *height = live_height;
}

static void check_bounds_stage(void)
{
    if (event_index == 0u) {
        bounds_app = book8gui_launch_class(&probe_class);
        other_app = book8gui_launch_class(&probe_class);
        assert(bounds_app != NULL && other_app != NULL);
        other_app->bounds = (struct book8gui_rect){400u, 40u, 200u, 200u};
    }
    if (event_index == 2u || event_index == 6u || event_index == 10u) {
        assert(bounds_app->maximized);
        assert(bounds_app->bounds.x == 0u && bounds_app->bounds.y == 0u);
        assert(bounds_app->bounds.width == live_width);
        assert(bounds_app->bounds.height == live_height - BOOK8GUI_TASKBAR_HEIGHT);
        if (event_index == 2u) {
            live_width = 1024u;
            live_height = 768u;
        } else if (event_index == 6u) {
            live_width = 640u;
            live_height = 300u;
        }
    }
    if (event_index == 4u || event_index == 8u || event_index == 12u) {
        assert(!bounds_app->maximized);
        assert(bounds_app->bounds.x == 40u && bounds_app->bounds.y == 40u);
        assert(bounds_app->bounds.width == 300u && bounds_app->bounds.height == 200u);
    }
    if (event_index == 17u) {
        assert(wheel_sum[0] == -120 && wheel_sum[1] == -120);
        assert(other_app->focused && !bounds_app->focused);
        assert(display_queries == 6u);
    }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t color)
{
    if (x == 0u && y == 0u && width == WIDTH && height == HEIGHT) {
        ++full_desktop_redraws;
    }
    if (x == 0u && y == 0u && width == WIDTH
        && height == HEIGHT - BOOK8GUI_TASKBAR_HEIGHT)
        saw_maximized_bounds = true;
    if (color == 0x00b8e8u && x == WIDTH - 88u - 52u + 1u
        && y == HEIGHT - BOOK8GUI_TASKBAR_HEIGHT + 16u)
        saw_tx_light = true;
    if (color == 0x00b8e8u && x == WIDTH - 88u - 52u + 8u
        && y == HEIGHT - BOOK8GUI_TASKBAR_HEIGHT + 6u)
        saw_rx_light = true;
    if (many_files && x == 324u && width == 10u && height == 52u
        && color == 0x808080u) {
        if (y == 122u)
            saw_initial_scroll_thumb = true;
        if (y > 122u)
            saw_moved_scroll_thumb = true;
    }
    uint32_t right = x + width > WIDTH ? WIDTH : x + width;
    uint32_t bottom = y + height > HEIGHT ? HEIGHT : y + height;
    if (x >= WIDTH || y >= HEIGHT) {
        return;
    }
    for (uint32_t row = y; row < bottom; ++row) {
        for (uint32_t column = x; column < right; ++column) {
            pixels[row * WIDTH + column] = color;
        }
    }
}

static bool cursor_save_under(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    assert(width <= 8u && height <= 12u && x + width <= WIDTH
        && y + height <= HEIGHT);
    cursor_under_x = x;
    cursor_under_y = y;
    cursor_under_width = width;
    cursor_under_height = height;
    for (uint32_t row = 0u; row < height; ++row) {
        for (uint32_t column = 0u; column < width; ++column) {
            cursor_under[row * width + column] =
                pixels[(y + row) * WIDTH + x + column];
        }
    }
    cursor_under_valid = true;
    return true;
}

static bool cursor_restore_under(void)
{
    assert(cursor_under_valid);
    for (uint32_t row = 0u; row < cursor_under_height; ++row) {
        for (uint32_t column = 0u; column < cursor_under_width; ++column) {
            pixels[(cursor_under_y + row) * WIDTH + cursor_under_x + column] =
                cursor_under[row * cursor_under_width + column];
        }
    }
    cursor_under_valid = false;
    return true;
}

static void present_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    assert(width != 0u && height != 0u && x + width <= WIDTH
        && y + height <= HEIGHT);
    ++cursor_present_calls;
}

static void draw_text(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background)
{
    if (programs_scenario) {
        if (strcmp(text, "Hex Viewer") == 0) { saw_program_name = true; }
        if (strcmp(text, "A:\\BIN\\HEXVIEW.ELF") == 0) { saw_program_path = true; }
        if (strcmp(text, "On disk: 4096 bytes") == 0) { saw_program_size = true; }
        if (strstr(text, "System Core") != NULL) { saw_program_core = true; }
        if (strstr(text, "Are you sure you want") != NULL) { saw_remove_prompt = true; }
    }
    if (editor_scenario && x == 47u && y == 93u
        && event_index >= 1u && event_index <= 5u) {
        static const char *const first_lines[] = {
            "LINE06", "LINE03", "LINE00", "LINE74", "LINE74"
        };
        assert(strncmp(text, first_lines[event_index - 1u], 6u) == 0);
        editor_rows_seen[event_index - 1u] = true;
    }
    if (many_files && x == 50u && y == 124u && strcmp(text, "ITEM01") == 0)
        saw_scrolled_first = true;
    if (many_files && x == 50u && y == 124u && strcmp(text, "ITEM04") == 0)
        saw_multi_scroll = true;
    if (many_files && x == 50u && y == 124u && strcmp(text, "ITEM02") == 0)
        saw_reverse_scroll = true;
    if (strstr(text, "Binary files cannot be saved") != NULL)
        saw_binary_save_rejection = true;
    if (strcmp(text, "A:\\HELLO.ELF") == 0)
        saw_properties_path = true;
    if (strcmp(text, "On disk: 512 bytes") == 0)
        saw_properties_size = true;
    if (strstr(text, "Created: 2026-11-07 13:25:18") != NULL)
        saw_properties_date = true;
    (void)foreground;
    (void)background;
}

static void pump_input(void)
{
    ++pump_count;
}

static bool poll_event(struct book8gui_event *event)
{
    if (spread_events && event_pause) {
        event_pause = false;
        return false;
    }
    if (cursor_test && event_index == 1u) {
        assert(full_desktop_redraws == cursor_test_full_start + 1u);
        assert(cursor_present_calls == cursor_test_present_start + 1u);
    }
    if (cursor_test && event_index == 2u) {
        assert(full_desktop_redraws == cursor_test_full_start + 1u);
        assert(cursor_present_calls == cursor_test_present_start + 3u);
    }
    if (event_index == event_count) {
        fprintf(stderr, "Host GUI exhausted events without exiting (programs=%d utility=%d editor=%d):\n%s\n",
            programs_scenario, utility_scenario, editor_scenario, trace);
        assert(event_index < event_count);
        return false;
    }
    if (bounds_scenario) {
        check_bounds_stage();
    }
    if (editor_scenario && event_index == 0u) {
        struct WindowApp *app = book8gui_launch_class(&book8_editor_class);
        assert(app != NULL);
        app->bounds = (struct book8gui_rect){40u, 40u, 300u, 200u};
        assert(app->cls->open_path(editor_platform, app, "A:\\SCROLL.TXT"));
        app->render(editor_platform, app);
    }
    if (utility_scenario && event_index == 0u) {
        assert(book8gui_queue_utility(BOOK8_GUI_HEXVIEW, "") == 0);
        assert(book8gui_open_pending_utility() && !book8gui_utility_pending());
        assert(!book8gui_remove_program("A:\\BIN\\HEXVIEW.ELF"));
        struct book8gui_window_info utility[2];
        assert(book8gui_window_snapshot(utility, 2u) == 2u);
        book8gui_end_task(utility[1].id);
    }
    if (utility_scenario && event_index == 1u) {
        assert(book8gui_remove_program("A:\\BIN\\HEXVIEW.ELF"));
        assert(!book8gui_utility_installed(BOOK8_GUI_HEXVIEW));
        assert(book8gui_file_app(2u) == NULL);
    }
    if (programs_scenario && !utility_scenario && event_index == 0u) {
        assert(book8gui_program_count() == 8u);
        assert(book8gui_launch_class(&book8_settings_class) != NULL);
        assert(!book8gui_remove_program("A:\\BOOT\\BOOK8.ELF"));
        assert(!book8gui_remove_program("A:\\BIN\\SHELL.ELF"));
        assert(!book8gui_remove_program("A:\\BIN\\READONLY.ELF"));
        assert(program_deletes == 0u);
    }
    event_pause = spread_events;
    current_ticks = event_ticks[event_index];
    *event = events[event_index++];
    return true;
}

static bool network_activity(uint32_t *tx, uint32_t *rx)
{
    *tx = event_index >= 3u ? 1u : 0u;
    *rx = event_index >= 6u ? 1u : 0u;
    return true;
}

static bool usb_mouse_active(void)
{
    return false;
}

static void run_command(char *command)
{
    (void)command;
    ++command_calls;
}

static bool read_file(const char *path, void *destination, uint32_t capacity,
    uint32_t *size)
{
    ++editor_reads;
    if (editor_scenario && strcmp(path, "A:\\SCROLL.TXT") == 0) {
        assert(capacity >= 560u);
        for (uint32_t i = 0u; i < 80u; ++i) {
            char line[8];
            snprintf(line, sizeof(line), "LINE%02u\n", i);
            memcpy((char *)destination + i * 7u, line, 7u);
        }
        *size = 560u;
        return true;
    }
    if (strcmp(path, "A:\\HELLO.ELF") != 0 || capacity < 4u)
        return false;
    memcpy(destination, "t\0st", 4u);
    *size = 4u;
    return true;
}

static bool write_file(const char *path, const void *data, uint32_t size)
{
    (void)path;
    (void)data;
    (void)size;
    ++editor_writes;
    return true;
}

static bool file_info(const char *path, struct book8_file_info *info)
{
    if (programs_scenario) {
        if (program_metadata_failure && strcmp(path, "A:\\BIN\\IMGVIEW.ELF") == 0) {
            return false;
        }
        *info = (struct book8_file_info){.size = 1200u, .allocated_bytes = 4096u,
            .allocation_valid = true};
        if (strcmp(path, "A:\\BIN\\READONLY.ELF") == 0) { info->read_only = true; }
        if (program_changed_attributes && strcmp(path, "A:\\BIN\\IMGVIEW.ELF") == 0) {
            info->system = true;
        }
        return true;
    }
    if (strcmp(path, "A:\\HELLO.ELF") != 0)
        return false;
    *info = (struct book8_file_info){
        .size = 4u, .allocated_bytes = 512u, .allocation_valid = true,
        .creation_valid = true,
        .year = 2026u, .month = 11u, .day = 7u,
        .hour = 13u, .minute = 25u, .second = 18u
    };
    return true;
}

static bool rename_entry(const char *path, const char *name)
{
    if (strcmp(path, "A:\\HELLO.ELF") != 0 || strcmp(name, "HELLO.ELF") != 0)
        return false;
    ++rename_calls;
    return true;
}

static bool delete_entry(const char *path)
{
    if (programs_scenario) {
        assert(strcmp(path, "A:\\BIN\\HEXVIEW.ELF") == 0);
        ++program_deletes;
        if (remove_fails) { remove_fails = false; return false; }
        removed_hex = true;
        return true;
    }
    if (strcmp(path, "A:\\HELLO.ELF") != 0)
        return false;
    ++delete_calls;
    return true;
}

static bool copy_to_directory(const char *path, const char *directory, bool cut)
{
    if (strcmp(path, "A:\\HELLO.ELF") != 0 || strcmp(directory, "A:\\") != 0)
        return false;
    ++paste_calls;
    paste_was_cut = cut;
    return true;
}

static unsigned shutdown_calls;
static unsigned reboot_calls;

static void do_shutdown(void)
{
    ++shutdown_calls;
}

static uint32_t host_flags(void)
{
    return BOOK8GUI_STATUS_KEYBOARD | BOOK8GUI_STATUS_MOUSE;
}

static bool host_audio_state(uint8_t *volume, bool *muted)
{
    *volume = host_audio_volume;
    *muted = host_audio_muted;
    return true;
}

static bool host_audio_set_volume(uint8_t volume)
{
    if (volume > 100u)
        return false;
    host_audio_volume = volume;
    ++host_audio_volume_calls;
    return true;
}

static bool host_audio_set_mute(bool muted)
{
    host_audio_muted = muted;
    ++host_audio_mute_calls;
    return true;
}

static bool host_time(struct book8gui_time *time)
{
    *time = (struct book8gui_time){13u, 54u, 5u, 11u, 2026u};
    return true;
}

static bool host_set_time(const struct book8gui_time *time)
{
    return time->year >= 1980u;
}

static bool host_save_config(const struct book8_system_config *config)
{
    return book8_config_valid(config);
}

bool book8_network_settings_get(struct book8_network_status *status)
{
    ++network_get_calls;
    *status = (struct book8_network_status){
        .settings = host_network_settings,
        .assigned_ip = 0x0a010203u,
        .assigned_mask = 0xffffff00u,
        .assigned_gateway = 0x0a010001u,
        .dns = 0x08080808u,
        .lease_seconds = 3600u,
        .state = "bound",
        .ready = true
    };
    return true;
}

bool book8_network_settings_apply(const struct book8_network_settings *settings,
    bool persist)
{
    ++network_apply_calls;
    if (persist) {
        ++network_saved_calls;
    }
    host_network_settings = *settings;
    return true;
}

bool book8_network_settings_renew(void)
{
    ++network_renew_calls;
    return host_network_renew_result;
}

static bool host_sysinfo(struct book8gui_sysinfo *info)
{
    *info = (struct book8gui_sysinfo){0};
    strcpy(info->cpu_name, "Host Test CPU");
    info->cpu_threads = 4u;
    info->cpu_mhz = 3000u;
    info->memory_total_kb = 524288u;
    info->memory_used_kb = 4096u;
    info->uptime_seconds = 3723u;
    info->disk_count = 1u;
    info->disk_total_kb = 16384u;
    strcpy(info->disk_label, "HOST DISK");
    return true;
}

static void do_reboot(void)
{
    ++reboot_calls;
}

static bool list_directory(const char *path,
    book8_directory_entry_fn visitor, void *context)
{
    if (programs_scenario) {
        if (program_overflow) {
            for (uint32_t i = 0u; i < BOOK8GUI_MAX_PROGRAMS + 10u; ++i) {
                char name[16];
                snprintf(name, sizeof(name), "APP%02u.ELF", i);
                if (!visitor(name, false, 1u, context)) { return false; }
            }
            return true;
        }
        if (strcmp(path, "A:\\") == 0) {
            return visitor("BIN", true, 0u, context)
                && visitor("BOOT", true, 0u, context);
        }
        if (strcmp(path, "A:\\BIN\\") == 0) {
            return (removed_hex || visitor("HEXVIEW.ELF", false, 1200u, context))
                && visitor("IMGVIEW.ELF", false, 1200u, context)
                && visitor("SHELL.ELF", false, 1200u, context)
                && visitor("READONLY.ELF", false, 1200u, context);
        }
        if (strcmp(path, "A:\\BOOT\\") == 0) {
            return visitor("BOOK8.ELF", false, 1200u, context);
        }
        return false;
    }
    if (strcmp(path, "A:\\") == 0) {
        if (many_files) {
            for (unsigned i = 0u; i < 30u; ++i) {
                char name[] = "ITEM00";
                name[4] = (char)('0' + i / 10u);
                name[5] = (char)('0' + i % 10u);
                if (!visitor(name, false, 1u, context))
                    return false;
            }
            return true;
        }
        return visitor("DATA", true, 0u, context)
            && visitor("HELLO.ELF", false, 1u, context);
    }
    if (strcmp(path, "A:\\DATA\\") == 0) {
        return visitor("NOTE.TXT", false, 1u, context);
    }
    return false;
}

static void diagnostic(const char *message)
{
    while (*message != '\0' && trace_length + 1u < sizeof(trace)) {
        trace[trace_length++] = *message++;
    }
    trace[trace_length] = '\0';
}

static uint64_t clock_ticks(void)
{
    return current_ticks;
}

static void add_mouse(int32_t x, int32_t y, uint8_t buttons, uint64_t ticks)
{
    struct book8gui_event *event = &events[event_count];
    event->type = BOOK8GUI_EVENT_MOUSE;
    event->key = 0u;
    event->mouse_x = x;
    event->mouse_y = y;
    event->wheel_delta = 0;
    event->mouse_buttons = buttons;
    event_ticks[event_count++] = ticks;
}

static void add_key(uint32_t key, uint64_t ticks)
{
    struct book8gui_event *event = &events[event_count];
    event->type = BOOK8GUI_EVENT_KEY;
    event->key = key;
    event->mouse_x = 0;
    event->mouse_y = 0;
    event->wheel_delta = 0;
    event->mouse_buttons = 0u;
    event_ticks[event_count++] = ticks;
}

static bool contains(const char *text)
{
    return strstr(trace, text) != NULL;
}

static void add_click(int32_t x, int32_t y, uint64_t *ticks)
{
    add_mouse(x, y, 1u, (*ticks)++);
    add_mouse(x, y, 0u, (*ticks)++);
}

int main(void)
{
    assert(book8_ps2_wheel(0x01u, 3u) == -120);
    assert(book8_ps2_wheel(0xffu, 3u) == 120);
    assert(book8_ps2_wheel(0x10u, 3u) == -1920);
    assert(book8_ps2_wheel(0xf0u, 3u) == 1920);
    assert(book8_ps2_wheel(0x80u, 3u) == 15360);
    assert(book8_ps2_wheel(0x31u, 4u) == -120);
    assert(book8_ps2_wheel(0x3fu, 4u) == 120);
    assert(book8_ps2_wheel(0x08u, 4u) == 960);
    struct book8gui_event message = {.type = BOOK8GUI_EVENT_KEY, .key = 'x'};
    assert(!book8gui_decode_wheel_message(&message, BOOK8_WM_MOUSEHWHEEL,
        120u << 16u, 50, 150) && message.type == BOOK8GUI_EVENT_KEY);
    assert(book8gui_decode_wheel_message(&message, BOOK8_WM_MOUSEWHEEL,
        (uintptr_t)(uint16_t)-240 << 16u | 0x73u, -20, 30));
    assert(message.wheel_delta == -240 && message.mouse_x == -20
        && message.mouse_y == 30 && message.mouse_buttons == 31u);
    assert(book8_wheel_wparam((uintptr_t)0x123400780000ull) == 120);
    assert(book8_wheel_wparam(0x80000000u) == INT16_MIN);
    assert(book8_wheel_detents(INT32_MAX) == 32760);
    assert(book8_wheel_detents(INT32_MIN) == -32760);
    int32_t remainder = 0;
    assert(ui_wheel_scroll_delta(10u, 30u, 10u, 60, 3u, &remainder) == 10u);
    assert(remainder == 60);
    assert(ui_wheel_scroll_delta(10u, 30u, 10u, 60, 3u, &remainder) == 7u);
    assert(remainder == 0);
    assert(ui_wheel_scroll(5u, 30u, 10u, -3, 1u) == 8u);
    assert(ui_wheel_scroll(5u, 30u, 10u, 2, 1u) == 3u);
    assert(ui_wheel_scroll(5u, 30u, 10u, -128, 1u) == 20u);
    assert(ui_wheel_scroll(5u, 30u, 10u, 127, 1u) == 0u);
    assert(ui_wheel_scroll(0u, 2u, 10u, -3, 1u) == 0u);
    const uint32_t explorer_row_x = 60u;
    const uint32_t explorer_row_y = 40u + 30u + 50u + 3u + 8u;
    const uint32_t task_x = 110u;
    const uint32_t taskbar_y = HEIGHT - BOOK8GUI_TASKBAR_HEIGHT + 8u;
    const int32_t start_x = 20;
    const int32_t start_y = (int32_t)taskbar_y;

    /* The desktop boots empty: Start > Programs > File Explorer first. */
    uint64_t ticks = 10u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 150, &ticks);
    add_mouse(explorer_row_x, explorer_row_y, 1u, ticks);
    add_mouse(explorer_row_x, explorer_row_y, 0u, ticks + 1u);
    add_mouse(explorer_row_x, explorer_row_y, 1u, ticks + 5u);
    add_mouse(explorer_row_x, explorer_row_y, 0u, ticks + 6u);
    ticks += 20u;
    /* Taskbar button minimizes then restores; maximize toggles twice. */
    add_click(task_x, taskbar_y, &ticks);
    add_click(task_x, taskbar_y, &ticks);
    add_click(305, 55, &ticks);
    add_click((int32_t)WIDTH - 35, 15, &ticks);

    /* Start > Programs > NoteEditor, Command Shell, then Settings twice. */
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 190, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 170, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 290, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 290, &ticks);
    /* Open System and advance the hour before applying the clock. */
    for (unsigned i = 0u; i < 6u; ++i) {
        add_key(BOOK8GUI_KEY_PAGE_DOWN, ticks++);
    }
    add_key(BOOK8GUI_KEY_RIGHT, ticks++);
    add_key(BOOK8GUI_KEY_ENTER, ticks++);
    /* Configure a static network profile, preserving its values across modes. */
    for (unsigned i = 0u; i < 6u; ++i) {
        add_key(BOOK8GUI_KEY_PAGE_UP, ticks++);
    }
    add_key(BOOK8GUI_KEY_LEFT, ticks++);  /* Disable adapter */
    add_key(BOOK8GUI_KEY_DOWN, ticks++);  /* Address mode */
    add_key(BOOK8GUI_KEY_RIGHT, ticks++); /* DHCP -> Static */
    add_key(BOOK8GUI_KEY_DOWN, ticks++);  /* Static IPv4 */
    add_key(BOOK8GUI_KEY_RIGHT, ticks++); /* first octet */
    for (unsigned i = 0u; i < 4u; ++i) {
        add_key(BOOK8GUI_KEY_DOWN, ticks++);
    }
    add_key(BOOK8GUI_KEY_RIGHT, ticks++); /* collector first octet */
    add_key(BOOK8GUI_KEY_DOWN, ticks++);  /* retry interval */
    add_key(BOOK8GUI_KEY_RIGHT, ticks++);
    for (unsigned i = 0u; i < 6u; ++i) {
        add_key(BOOK8GUI_KEY_UP, ticks++);
    }
    add_key(BOOK8GUI_KEY_LEFT, ticks++);  /* Static -> DHCP */
    add_key(BOOK8GUI_KEY_RIGHT, ticks++); /* DHCP -> Static */
    add_key('r', ticks++);                /* Renew */
    add_key(BOOK8GUI_KEY_ENTER, ticks++); /* Apply & Save */
    add_click(620, 47, &ticks);
    /* Task Manager from Start > Programs, then focused again by hotkey. */
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 210, &ticks);
    add_key(BOOK8GUI_KEY_ESCAPE | BOOK8GUI_KEY_CTRL | BOOK8GUI_KEY_ALT,
        ticks++);
    add_key(BOOK8GUI_KEY_RIGHT, ticks++);
    add_key(BOOK8GUI_KEY_LEFT, ticks++);
    /* Select File Explorer (the desktop row comes first) and End Task. */
    add_key(BOOK8GUI_KEY_DOWN, ticks++);
    add_key(BOOK8GUI_KEY_DOWN, ticks++);
    add_key(BOOK8GUI_KEY_DELETE, ticks++);
    ticks += 100u;
    /* Tray mixer opens, tracks a vertical drag, toggles mute, then passes an
     * outside press through to the Start button. */
    add_click(537, 342, &ticks);
    add_mouse(533, 194, 1u, ticks++);
    add_mouse(533, 236, 1u, ticks++);
    add_mouse(533, 236, 0u, ticks++);
    add_click(491, 303, &ticks);
    add_click(start_x, start_y, &ticks);
    /* Start > Shutdown > Reboot, then Start > Shutdown > Shutdown. */
    add_click(40, 310, &ticks);
    add_click(200, 290, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 270, &ticks);
    add_key(BOOK8GUI_KEY_ESCAPE, ticks++);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);

    struct book8gui_platform platform = {
        .width = WIDTH,
        .height = HEIGHT,
        .fill_rect = fill_rect,
        .draw_text = draw_text,
        .pump_input = pump_input,
        .poll_event = poll_event,
        .usb_mouse_active = usb_mouse_active,
        .run_command = run_command,
        .shutdown = do_shutdown,
        .reboot = do_reboot,
        .status_flags = host_flags,
        .audio_state = host_audio_state,
        .audio_set_volume = host_audio_set_volume,
        .audio_set_mute = host_audio_set_mute,
        .read_time = host_time,
        .set_time = host_set_time,
        .save_config = host_save_config,
        .system_info = host_sysinfo,
        .session_uid = host_session_uid,
        .session_token = host_session_token,
        .read_file = read_file,
        .write_file = write_file,
        .list_directory = list_directory,
        .file_info = file_info,
        .rename_entry = rename_entry,
        .delete_entry = delete_entry,
        .copy_to_directory = copy_to_directory,
        .diagnostic = diagnostic,
        .clock_ticks = clock_ticks,
        .clock_hz = 50u,
        .present = NULL
    };

    if (!book8gui_run(&platform)) {
        assert(book8_session_initialize(0x424f4f4b38u));
        assert(book8_session_login("admin", "default"));
        fprintf(stderr, "FAIL: desktop did not start\n");
        return 1;
    }
    if (host_audio_volume != 50u || !host_audio_muted
        || host_audio_volume_calls < 2u || host_audio_mute_calls != 1u) {
        fprintf(stderr, "FAIL: tray volume drag or mute control\n");
        return 1;
    }
    if (!contains("BOOK8-DESKTOP: WINDOW id=1 title=\"File Explorer\" registered\n")
        || !contains("BOOK8-DESKTOP: WINDOW id=2 title=\"NoteEditor\" registered\n")
        || !contains("BOOK8-DESKTOP: WINDOW id=3 title=\"Command Shell\" registered\n")
        || !contains("BOOK8-DESKTOP: WINDOW id=4 title=\"System Settings\" registered\n")) {
        fprintf(stderr, "FAIL: desktop app registry diagnostics missing\n");
        fwrite(trace, 1u, trace_length, stderr);
        return 1;
    }
    if (strstr(trace, "WINDOW id=1 title=\"File Explorer\" registered\n")
        < strstr(trace, "TASKBAR height=36 reserved\n")) {
        fprintf(stderr, "FAIL: desktop did not boot empty\n");
        return 1;
    }
    if (!contains("BOOK8-EXPLORER: path=A:\\DATA\\\n")) {
        fprintf(stderr, "FAIL: tick-based directory double-click did not navigate\n");
        return 1;
    }
    if (!contains("WINDOW minimized id=1")
        || !contains("WINDOW maximized id=1")
        || !contains("WINDOW restored id=1")) {
        fprintf(stderr, "FAIL: minimize/maximize/restore missing\n");
        return 1;
    }
    if (!contains("BOOK8-SETTINGS: time set 2026-11-05 14:54")) {
        fprintf(stderr, "FAIL: Settings did not set the clock\n");
        return 1;
    }
    if (network_get_calls == 0u || network_apply_calls != 1u
        || network_saved_calls != 1u || network_renew_calls != 1u
        || host_network_settings.mode != BOOK8_NETWORK_STATIC
        || host_network_settings.ip != 0x0b010203u
        || host_network_settings.telemetry != 0xc1000201u
        || host_network_settings.retry_ms != 2000u
        || book8_config_value(BOOK8_CFG_NETWORK) != 0u) {
        fprintf(stderr, "FAIL: network settings edits/apply/save/renew (get=%u apply=%u save=%u renew=%u mode=%u ip=%08x collector=%08x retry=%u)\n",
            network_get_calls, network_apply_calls, network_saved_calls,
            network_renew_calls, host_network_settings.mode,
            host_network_settings.ip, host_network_settings.telemetry,
            host_network_settings.retry_ms);
        return 1;
    }
    if (strstr(trace, "id=5 title=\"System Settings\"") != NULL) {
        fprintf(stderr, "FAIL: single-instance Settings opened twice\n");
        return 1;
    }
    if (!contains("BOOK8-DESKTOP: WINDOW id=5 title=\"Task Manager\" registered\n")
        || !contains("BOOK8-DESKTOP: hotkey Task Manager id=5")
        || strstr(trace, "id=6 title=\"Task Manager\"") != NULL) {
        fprintf(stderr, "FAIL: Task Manager launch/hotkey/single-instance\n");
        return 1;
    }
    if (!contains("BOOK8-DESKTOP: WINDOW closed id=1 title=\"File Explorer\"\n")) {
        fprintf(stderr, "FAIL: Task Manager End Task did not close the window\n");
        return 1;
    }
    if (!contains("BOOK8-DESKTOP: WINDOW closed id=4 title=\"System Settings\"\n")) {
        fprintf(stderr, "FAIL: close button did not close the window\n");
        return 1;
    }
    if (shutdown_calls != 1u || reboot_calls != 1u) {
        fprintf(stderr, "FAIL: power menu actions (shutdown=%u reboot=%u)\n",
            shutdown_calls, reboot_calls);
        return 1;
    }
    if (pixels[(HEIGHT - 1u) * WIDTH + WIDTH - 1u] != TASKBAR_COLOR) {
        fprintf(stderr, "FAIL: taskbar does not cover the bottom screen row\n");
        return 1;
    }
    if (pump_count == 0u) {
        fprintf(stderr, "FAIL: input pump was not serviced\n");
        return 1;
    }
    event_count = event_index = 0u;
    current_ticks = 0u;
    spread_events = true;
    event_pause = false;
    platform.cursor_save_under = cursor_save_under;
    platform.cursor_restore_under = cursor_restore_under;
    platform.present_rect = present_rect;
    cursor_test = true;
    cursor_test_full_start = full_desktop_redraws;
    cursor_test_present_start = cursor_present_calls;
    ticks = 1u;
    add_mouse(300, 120, 0u, ticks++);
    add_mouse(306, 126, 0u, ticks++);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    assert(book8gui_run(&platform));
    cursor_test = false;
    platform.cursor_save_under = NULL;
    platform.cursor_restore_under = NULL;
    platform.present_rect = NULL;
    spread_events = true;
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    platform.network_activity = network_activity;
    trace_length = 0u;
    trace[0] = '\0';
    assert(reg_set_str("HKLM\\SOFTWARE\\BOOK8\\FileAssoc", ".ELF", "editor"));
    ticks = 1u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 150, &ticks);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 2u, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 0u, ticks++);
    add_click(explorer_row_x + 5, explorer_row_y + 18u + 3u * 18u + 4u, &ticks);
    add_click(start_x, start_y, &ticks); /* Modal blocks the taskbar. */
    add_key(BOOK8GUI_KEY_TAB, ticks++);
    add_key(BOOK8GUI_KEY_ESCAPE, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 2u, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 0u, ticks++);
    add_click(explorer_row_x + 5, explorer_row_y + 18u + 4u, &ticks);
    add_key(BOOK8GUI_KEY_CTRL | 's', ticks++);
    add_key(BOOK8GUI_KEY_ESCAPE, ticks++);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    if (!book8gui_run(&platform) || editor_reads != 1u || editor_writes != 0u
        || !saw_binary_save_rejection || command_calls != 0u
        || !saw_properties_path || !saw_properties_size || !saw_properties_date
        || !saw_tx_light || !saw_rx_light
        || !contains("BOOK8-NOTEEDITOR: opened path=A:\\HELLO.ELF")) {
        fprintf(stderr, "FAIL: context/editor: reads=%u writes=%u binary=%d commands=%u props=%d/%d/%d activity=%d/%d\n",
            editor_reads, editor_writes, saw_binary_save_rejection, command_calls,
            saw_properties_path, saw_properties_size, saw_properties_date,
            saw_tx_light, saw_rx_light);
        fprintf(stderr, "%s\n", trace);
        return 1;
    }
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    editor_reads = 0u;
    saw_maximized_bounds = false;
    ticks = 1u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 150, &ticks);
    add_click(300, 50, &ticks);
    add_click(602, 42, &ticks);
    add_mouse(explorer_row_x, explorer_row_y - 14u, 2u, ticks++);
    add_mouse(explorer_row_x, explorer_row_y - 14u, 0u, ticks++);
    add_click(explorer_row_x + 5, explorer_row_y - 14u + 7u * 18u + 4u, &ticks);
    add_click(explorer_row_x + 5, explorer_row_y - 14u + 4u, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    if (!book8gui_run(&platform) || !saw_maximized_bounds || editor_reads != 1u
        || book8gui_file_app(0u) == NULL
        || book8gui_file_app(1u) != &book8_player_class
        || book8gui_file_app(2u) != NULL) {
        fprintf(stderr, "FAIL: maximize bounds or Open With registry\n%s\n", trace);
        return 1;
    }
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    ticks = 1u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 150, &ticks);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 2u, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 0u, ticks++);
    add_click(explorer_row_x + 5, explorer_row_y + 18u + 2u * 18u + 4u, &ticks);
    add_click(start_x, start_y, &ticks);
    add_key(BOOK8GUI_KEY_ENTER, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 2u, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 0u, ticks++);
    add_click(explorer_row_x + 5, explorer_row_y + 18u + 18u + 4u, &ticks);
    add_click(50, 151, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    if (!book8gui_run(&platform) || rename_calls != 1u || delete_calls != 1u) {
        fprintf(stderr, "FAIL: modal rename/delete actions: %u/%u\n%s\n",
            rename_calls, delete_calls, trace);
        return 1;
    }
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    ticks = 1u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 150, &ticks);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 2u, ticks++);
    add_mouse(explorer_row_x, explorer_row_y + 18u, 0u, ticks++);
    add_click(explorer_row_x + 5, explorer_row_y + 18u + 4u * 18u + 4u, &ticks);
    add_mouse(500, 100, 2u, ticks++);
    add_mouse(500, 100, 0u, ticks++);
    add_click(505, 105, &ticks);
    add_mouse(500, 100, 2u, ticks++);
    add_mouse(500, 100, 0u, ticks++);
    add_click(505, 105, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    if (!book8gui_run(&platform) || paste_calls != 1u || !paste_was_cut) {
        fprintf(stderr, "FAIL: desktop cut/paste clipboard: %u\n%s\n", paste_calls, trace);
        return 1;
    }
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    many_files = true;
    ticks = 1u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 150, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_click(200, 170, &ticks);
    add_mouse(50, 150, 0u, ticks++);
    events[event_count - 1u].wheel_delta = -120;
    add_mouse(50, 150, 0u, ticks++);
    assert(book8gui_decode_wheel_message(&events[event_count - 1u], BOOK8_WM_MOUSEWHEEL,
        (uintptr_t)(uint16_t)-360 << 16u, 50, 150));
    add_mouse(50, 150, 0u, ticks++);
    assert(book8gui_decode_wheel_message(&events[event_count - 1u], BOOK8_WM_MOUSEWHEEL,
        240u << 16u, 50, 150));
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    if (!book8gui_run(&platform) || !saw_scrolled_first
        || !saw_multi_scroll || !saw_reverse_scroll
        || !saw_initial_scroll_thumb || !saw_moved_scroll_thumb) {
        fprintf(stderr, "FAIL: wheel view/thumb movement: %d/%d/%d\n",
            saw_scrolled_first, saw_initial_scroll_thumb, saw_moved_scroll_thumb);
        return 1;
    }
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    bounds_scenario = true;
    live_width = 800u;
    live_height = 600u;
    platform.display_size = display_size;
    ticks = 1u;
    add_click(305, 55, &ticks);
    add_click(765, 15, &ticks);
    add_click(305, 55, &ticks);
    add_click(989, 15, &ticks);
    add_click(305, 55, &ticks);
    add_click(605, 15, &ticks);
    add_mouse(50, 150, 0u, ticks++);
    events[event_count - 1u].wheel_delta = -360;
    add_mouse(50, 150, 0u, ticks++);
    events[event_count - 1u].wheel_delta = 240;
    add_mouse(500, 150, 0u, ticks++);
    events[event_count - 1u].wheel_delta = -120;
    add_mouse(10, 150, 0u, ticks++);
    events[event_count - 1u].wheel_delta = -600;
    add_mouse(20, 280, 0u, ticks++);
    events[event_count - 1u].wheel_delta = -600;
    add_click(20, 280, &ticks);
    add_click(40, 250, &ticks);
    add_click(200, 250, &ticks);
    assert(book8gui_run(&platform));
    bounds_scenario = false;
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    editor_scenario = true;
    platform.display_size = NULL;
    editor_platform = &platform;
    ticks = 1u;
    const int8_t editor_wheels[] = {-2, 1, 127, -128, 1};
    for (uint32_t i = 0u; i < 5u; ++i) {
        add_mouse(50, i == 4u ? 75 : 110, 0u, ticks++);
        events[event_count - 1u].wheel_delta = book8_wheel_detents(editor_wheels[i]);
    }
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    assert(book8gui_run(&platform));
    for (uint32_t i = 0u; i < 5u; ++i) {
        assert(editor_rows_seen[i]);
    }
    editor_scenario = false;
    event_count = event_index = 0u;
    current_ticks = 0u;
    event_pause = false;
    programs_scenario = true;
    remove_fails = true;
    platform.launch_program = launch_program;
    ticks = 1u;
    add_key(BOOK8GUI_KEY_PAGE_DOWN, ticks++);
    add_key(BOOK8GUI_KEY_PAGE_DOWN, ticks++);
    add_key(BOOK8GUI_KEY_DELETE, ticks++);
    for (uint32_t i = 0u; i < 5u; ++i) { add_key(BOOK8GUI_KEY_DOWN, ticks++); }
    add_key(BOOK8GUI_KEY_ESCAPE, ticks++);
    add_key(BOOK8GUI_KEY_DELETE, ticks++);
    add_key(BOOK8GUI_KEY_ENTER, ticks++);
    add_key(BOOK8GUI_KEY_DELETE, ticks++);
    add_key(BOOK8GUI_KEY_ENTER, ticks++);
    add_click(start_x, start_y, &ticks);
    add_click(40, 270, &ticks);
    add_mouse(200, 290, 0u, ticks++);
    events[event_count - 1u].wheel_delta = -480;
    add_click(200, 290, &ticks);
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    assert(book8gui_run(&platform));
    assert(program_launches == 1u && program_deletes == 2u);
    assert(saw_program_name && saw_program_path && saw_program_size && saw_program_core && saw_remove_prompt);
    assert(book8gui_program_count() == 7u);
    struct book8gui_program installed;
    assert(book8gui_program_info(5u, &installed));
    assert(strcmp(installed.path, "A:\\BIN\\IMGVIEW.ELF") == 0);
    program_changed_attributes = true;
    assert(!book8gui_remove_program(installed.path));
    assert(program_deletes == 2u && book8gui_program_count() == 7u);
    program_changed_attributes = false;
    program_metadata_failure = true;
    assert(book8gui_refresh_programs());
    assert(book8gui_program_info(5u, &installed) && installed.system_component);
    assert(strstr(book8gui_program_status(), "metadata unavailable: 1") != NULL);
    assert(!book8gui_remove_program(installed.path));
    assert(!book8gui_remove_program(NULL));
    program_metadata_failure = false;
    program_overflow = true;
    assert(!book8gui_refresh_programs());
    assert(book8gui_program_count() == BOOK8GUI_MAX_PROGRAMS);
    assert(strstr(book8gui_program_status(), "incomplete") != NULL);
    program_overflow = false;
    assert(book8gui_refresh_programs());
    removed_hex = false;
    assert(book8gui_refresh_programs());
    assert(book8gui_utility_installed(BOOK8_GUI_HEXVIEW));
    assert(book8gui_file_app(2u) == &book8_hexview_class);
    assert(book8gui_queue_utility(99u, "") == -BOOK8_EINVAL);
    assert(book8gui_queue_utility(BOOK8_GUI_HEXVIEW, NULL) == -BOOK8_EINVAL);
    char overlong_path[257];
    memset(overlong_path, 'X', 256u); overlong_path[256] = '\0';
    assert(book8gui_queue_utility(BOOK8_GUI_HEXVIEW, overlong_path) == -BOOK8_EINVAL);
    assert(book8gui_queue_utility(BOOK8_GUI_HEXVIEW, "") == 0);
    assert(book8gui_queue_utility(BOOK8_GUI_HEXVIEW, "") == -BOOK8_EAGAIN);
    book8gui_cancel_pending_utility();
    assert(!book8gui_utility_pending());
    utility_scenario = true;
    event_count = event_index = 0u; current_ticks = 0u; event_pause = false;
    ticks = 1u;
    add_click(start_x, start_y, &ticks);
    add_click(40, 310, &ticks);
    add_click(200, 310, &ticks);
    assert(book8gui_run(&platform));
    assert(!book8gui_utility_installed(BOOK8_GUI_HEXVIEW));
    assert(book8gui_file_app(2u) == NULL);
    utility_scenario = false;
    programs_scenario = false;
    printf("book8gui host test: PASS\n");
    return 0;
}
