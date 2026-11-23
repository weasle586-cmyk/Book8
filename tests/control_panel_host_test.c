#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/apps/book8_settings.h"
#include "../src/session.h"
#include "../net/network_settings.h"

static uint64_t now;
static uint32_t saves, changes, restores, scenario, stage;
static uint32_t exit_stage;
static uint64_t exit_ready;
static uint32_t live_width, live_height;
static bool save_failure, change_failure, restore_failure;
static uint8_t persisted[BOOK8_CONFIG_BYTES];
static char drawn[16384];
static uint32_t drawn_size;
static struct book8gui_platform platform;
static struct WindowApp *preview_app;
static struct book8_network_settings network_settings = {
    BOOK8_NETWORK_DHCP, 0x0a010203u, 0xffffff00u, 0x0a010001u,
    0x08080808u, 0xc0000201u, 1000u
};

bool book8_network_settings_get(struct book8_network_status *status)
{
    *status = (struct book8_network_status){
        .settings = network_settings,
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

bool book8_network_settings_apply(const struct book8_network_settings *settings,
    bool persist)
{
    (void)persist;
    network_settings = *settings;
    return true;
}

bool book8_network_settings_renew(void)
{
    return true;
}

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    (void)color;
    assert(x <= live_width && y <= live_height);
    assert(w <= live_width - x && h <= live_height - y);
}

static void text(uint32_t x, uint32_t y, const char *line, uint32_t fg, uint32_t bg)
{
    (void)x; (void)y; (void)fg; (void)bg;
    size_t n = strlen(line);
    if (drawn_size + n + 2u < sizeof(drawn)) {
        memcpy(drawn + drawn_size, line, n);
        drawn_size += (uint32_t)n;
        drawn[drawn_size++] = '\n';
        drawn[drawn_size] = '\0';
    }
}

static bool save(const struct book8_system_config *config)
{
    ++saves;
    return !save_failure && book8_config_encode(config, persisted);
}

static bool mode(uint32_t width, uint32_t height)
{
    if (width == 800u && height == 600u) {
        ++restores;
        if (restore_failure) { restore_failure = false; return false; }
    } else {
        ++changes;
        if (change_failure) { return false; }
    }
    live_width = width;
    live_height = height;
    return true;
}

static bool mouse_active(void) { return false; }
static bool directory(const char *path, book8_directory_entry_fn visitor, void *ctx)
{
    (void)path; (void)visitor; (void)ctx;
    return true;
}
static void command(char *line) { (void)line; }
static void pump(void) { now += 100u; assert(now < 40000u); }
static uint64_t ticks(void) { return now; }
static void diagnostic(const char *line) { (void)line; }

static bool event(struct book8gui_event *out)
{
    *out = (struct book8gui_event){.type = BOOK8GUI_EVENT_KEY};
    if (stage == 0u) {
        preview_app = book8gui_launch_class(&book8_settings_class);
        assert(preview_app != NULL);
        preview_app->maximized = true;
        preview_app->bounds = (struct book8gui_rect){0u, 0u, 800u, 564u};
        preview_app->restore_bounds = (struct book8gui_rect){180u, 100u, 620u, 450u};
        struct book8_system_config proposed = *book8_config_get();
        proposed.values[BOOK8_CFG_RESOLUTION] = 1u;
        enum book8_display_request result = book8gui_preview_display(&proposed);
        assert(result == (change_failure ? BOOK8_DISPLAY_FAILED : BOOK8_DISPLAY_STARTED));
        if (!change_failure) {
            assert(preview_app->bounds.width == 1024u);
            assert(preview_app->bounds.y == 0u);
            assert(preview_app->bounds.height == 732u);
        }
        stage = 1u;
        return false;
    }
    if (change_failure || (changes != 0u
        && (restores != 0u || saves != 0u))) {
        if (strstr(book8gui_display_status(), "restore failed") != NULL) {
            out->key = BOOK8GUI_KEY_ENTER;
        } else {
            if (live_width == 800u) {
                assert(preview_app->bounds.width == 800u);
                assert(preview_app->bounds.y == 0u);
                assert(preview_app->bounds.height == 564u);
                assert(preview_app->restore_bounds.x == 180u);
                assert(preview_app->restore_bounds.y == 100u);
            }
            static const int32_t x[] = {20, 20, 20, 40, 40, 200, 200};
            if (exit_ready == 0u) {
                exit_ready = now + 200u;
                return false;
            }
            if (now < exit_ready)
                return false;
            uint32_t index = exit_stage++;
            assert(index < 7u);
            out->type = BOOK8GUI_EVENT_MOUSE;
            out->mouse_x = x[index];
            out->mouse_y = index < 3u ? (int32_t)live_height - 20
                : (int32_t)live_height - 50;
            out->mouse_buttons = (index & 1u) != 0u ? 1u : 0u;
        }
        return true;
    }
    if (stage == 1u && ((scenario != 0u && now >= 29100u)
        || (scenario == 0u && now >= 30200u))) {
        stage = 2u;
        if (scenario == 2u) { out->key = BOOK8GUI_KEY_ESCAPE; }
        else if (scenario == 3u) {
            out->type = BOOK8GUI_EVENT_MOUSE;
            out->mouse_x = (int32_t)((1024u - 544u) / 2u + 30u);
            out->mouse_y = (int32_t)((768u - 140u) / 2u + 108u);
            out->mouse_buttons = 1u;
        } else { out->key = BOOK8GUI_KEY_ENTER; }
        return true;
    }
    return false;
}

static void test_display(uint32_t which, bool fail_save, bool fail_change, bool fail_restore)
{
    struct book8_system_config defaults;
    book8_config_defaults(&defaults);
    assert(book8_config_commit(&defaults));
    scenario = which; save_failure = fail_save; change_failure = fail_change;
    restore_failure = fail_restore;
    now = saves = changes = restores = stage = drawn_size = 0u;
    exit_stage = 0u;
    exit_ready = 0u;
    drawn[0] = '\0';
    live_width = 800u; live_height = 600u;
    platform = (struct book8gui_platform){
        .width = 800u, .height = 600u, .fill_rect = fill, .draw_text = text,
        .pump_input = pump, .poll_event = event, .usb_mouse_active = mouse_active,
        .run_command = command, .diagnostic = diagnostic,
        .clock_ticks = ticks, .clock_hz = 1000u,
        .save_config = save, .set_display_mode = mode,
        .list_directory = directory,
        .session_uid = session_uid, .session_token = session_token
    };
    assert(book8gui_run(&platform));
    if (which == 0u || which == 2u || fail_save || fail_change) {
        assert(book8_config_value(BOOK8_CFG_RESOLUTION) == 1u);
        assert(live_width == 800u && live_height == 600u);
    } else {
        struct book8_system_config loaded;
        assert(book8_config_value(BOOK8_CFG_RESOLUTION) == 1u);
        assert(book8_config_decode(&loaded, persisted, sizeof(persisted)));
        assert(loaded.values[BOOK8_CFG_RESOLUTION] == 1u);
    }
    if (which == 0u || which == 2u || fail_change) { assert(saves == 0u); }
    if (which == 0u && !fail_change) { assert(now >= 30100u); }
    if (!fail_change) {
        assert(strstr(drawn, "Keep this display setting?") != NULL);
        assert(strstr(drawn, "Reverting in [30] seconds.") != NULL);
    }
}

static void key(struct WindowApp *app, uint32_t value)
{
    struct book8gui_event e = {.type = BOOK8GUI_EVENT_KEY, .key = value};
    assert(book8_settings_class.handle_event(&platform, app, &e));
}

static void pointer(struct WindowApp *app, int32_t x, int32_t y, uint8_t buttons)
{
    struct book8gui_event e = {.type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = x, .mouse_y = y, .mouse_buttons = buttons};
    assert(book8_settings_class.handle_event(&platform, app, &e));
}

static void test_panel(void)
{
    drawn_size = 0u;
    drawn[0] = '\0';
    struct book8_system_config defaults, loaded;
    book8_config_defaults(&defaults);
    assert(book8_config_commit(&defaults));
    platform.set_display_mode = NULL;
    /* A fresh desktop is needed so its platform also has no modesetting hook. */
    platform.width = live_width = 800u;
    platform.height = live_height = 600u;
    struct WindowApp app = {.bounds = {20u, 40u, 620u, 450u}, .visible = true};
    app.state = book8_settings_class.create(&platform);
    /* Eight categories exist; a full lap returns to Network, then one more
     * PAGE_DOWN selects Input & Devices for the repeat-rate edit. */
    for (uint32_t page = 0u; page < 8u; ++page) {
        book8_settings_class.render(&platform, &app);
        key(&app, BOOK8GUI_KEY_PAGE_DOWN);
    }
    key(&app, BOOK8GUI_KEY_PAGE_DOWN);
    key(&app, BOOK8GUI_KEY_RIGHT);
    save_failure = false;
    key(&app, BOOK8GUI_KEY_ENTER);
    assert(book8_config_value(BOOK8_CFG_REPEAT_RATE) == 26u);
    assert(book8_config_decode(&loaded, persisted, sizeof(persisted)));
    assert(loaded.values[BOOK8_CFG_REPEAT_RATE] == 26u);
    save_failure = true;
    key(&app, BOOK8GUI_KEY_RIGHT);
    key(&app, BOOK8GUI_KEY_ENTER);
    assert(book8_config_value(BOOK8_CFG_REPEAT_RATE) == 26u);
    book8_settings_class.render(&platform, &app);
    assert(strstr(drawn, "Save failed") != NULL);
    key(&app, BOOK8GUI_KEY_DOWN);
    key(&app, BOOK8GUI_KEY_DOWN);
    pointer(&app, 400, 210, 1u);
    pointer(&app, 592, 210, 1u);
    pointer(&app, 592, 210, 0u);
    assert(book8_config_value(BOOK8_CFG_POINTER_SPEED) == 100u);
    save_failure = false;
    key(&app, BOOK8GUI_KEY_ENTER);
    assert(book8_config_value(BOOK8_CFG_POINTER_SPEED) == 300u);
    key(&app, BOOK8GUI_KEY_DOWN);
    key(&app, BOOK8GUI_KEY_DOWN);
    key(&app, BOOK8GUI_KEY_RIGHT);
    assert(book8_config_value(BOOK8_CFG_SWAP_BUTTONS) == 0u);
    pointer(&app, 300, 90, 1u);
    pointer(&app, 300, 90, 0u);
    assert(book8_config_value(BOOK8_CFG_SWAP_BUTTONS) == 1u);
    key(&app, BOOK8GUI_KEY_PAGE_DOWN);
    key(&app, BOOK8GUI_KEY_PAGE_DOWN);
    key(&app, BOOK8GUI_KEY_DOWN);
    key(&app, BOOK8GUI_KEY_DOWN);
    key(&app, BOOK8GUI_KEY_DOWN);
    key(&app, BOOK8GUI_KEY_RIGHT);
    pointer(&app, 300, 90, 1u);
    pointer(&app, 300, 90, 0u);
    assert(book8_config_value(BOOK8_CFG_DARK) == 1u);
    /* From Display, five PAGE_DOWN presses wrap to Network. */
    for (uint32_t page = 0u; page < 5u; ++page) {
        key(&app, BOOK8GUI_KEY_PAGE_DOWN);
    }
    book8_settings_class.render(&platform, &app);
    assert(strstr(drawn, "Adapter connection preference") != NULL);
    struct book8_network_settings original_network = network_settings;
    key(&app, BOOK8GUI_KEY_LEFT);
    assert(book8_config_value(BOOK8_CFG_NETWORK) == 1u);
    key(&app, BOOK8GUI_KEY_ENTER);
    assert(book8_config_value(BOOK8_CFG_NETWORK) == 0u);
    assert(book8_config_decode(&loaded, persisted, sizeof(persisted)));
    assert(loaded.values[BOOK8_CFG_NETWORK] == 0u);
    assert(memcmp(&network_settings, &original_network, sizeof(network_settings)) == 0);
    pointer(&app, 260, 140, 1u);
    pointer(&app, 260, 140, 0u);
    save_failure = true;
    key(&app, BOOK8GUI_KEY_ENTER);
    assert(book8_config_value(BOOK8_CFG_NETWORK) == 0u);
    book8_settings_class.render(&platform, &app);
    assert(strstr(drawn, "Network profile saved; enable preference") != NULL);
    save_failure = false;
    key(&app, BOOK8GUI_KEY_ENTER);
    assert(book8_config_value(BOOK8_CFG_NETWORK) == 1u);
    assert(book8_config_decode(&loaded, persisted, sizeof(persisted)));
    assert(loaded.values[BOOK8_CFG_NETWORK] == 1u);
    assert(memcmp(&network_settings, &original_network, sizeof(network_settings)) == 0);
    /* F1 opens the About box from any category without adding a ninth category. */
    drawn_size = 0u;
    drawn[0] = '\0';
    key(&app, BOOK8GUI_KEY_F1);
    book8_settings_class.render(&platform, &app);
    assert(strstr(drawn, "About BOOK8") != NULL);
    assert(strstr(drawn, BOOK8_GUI_VERSION) != NULL);
    assert(strstr(drawn, "A:\\DOC\\README.MD") != NULL);
    assert(strstr(drawn, "A:\\DOC\\DEVREAD.MD") != NULL);
    assert(strstr(drawn, "A:\\DOC\\KERNELNO.MD") != NULL);
    key(&app, BOOK8GUI_KEY_ESCAPE);
    drawn_size = 0u;
    drawn[0] = '\0';
    book8_settings_class.render(&platform, &app);
    assert(strstr(drawn, "A:\\DOC\\README.MD") == NULL);
    /* Resizing and scrolling must not draw outside even the minimum window. */
    app.bounds = (struct book8gui_rect){0u, 0u, 240u, 160u};
    for (uint32_t page = 0u; page < 8u; ++page) {
        book8_settings_class.render(&platform, &app);
        key(&app, BOOK8GUI_KEY_PAGE_DOWN);
    }
}

static void test_config(void)
{
    struct book8_system_config original, copy;
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    book8_config_defaults(&original);
    assert(book8_config_encode(&original, bytes));
    assert(book8_config_decode(&copy, bytes, sizeof(bytes)));
    assert(memcmp(&original, &copy, sizeof(copy)) == 0);
    assert(!book8_config_decode(&copy, bytes, sizeof(bytes) - 1u));
    bytes[4] = 2u;
    assert(!book8_config_decode(&copy, bytes, sizeof(bytes)));
    assert(book8_config_encode(&original, bytes));
    bytes[20] ^= 1u;
    assert(!book8_config_decode(&copy, bytes, sizeof(bytes)));
    original.values[BOOK8_CFG_RESOLUTION] = 3u;
    assert(!book8_config_commit(&original));
    assert(!book8_config_encode(&original, bytes));
    book8_config_defaults(&original);
    original.values[BOOK8_CFG_SWAP_BUTTONS] = 1u;
    original.values[BOOK8_CFG_POINTER_SPEED] = 25u;
    assert(book8_config_commit(&original));
    assert(book8_pointer_buttons(5u) == 6u);
    int32_t remainder = 0;
    for (uint32_t i = 0u; i < 3u; ++i) { assert(book8_pointer_delta(1, &remainder) == 0); }
    assert(book8_pointer_delta(1, &remainder) == 1);
    assert(book8_pointer_delta(-4, &remainder) == -1);
}

int main(void)
{
    assert(book8_session_initialize(0x43504e4cu));
    assert(book8_session_login("admin", "default"));
    test_config();
    test_display(0u, false, false, false); /* timeout, no save */
    test_display(1u, false, false, false); /* Enter before deadline */
    test_display(2u, false, false, false); /* Escape rolls back */
    test_display(3u, false, false, false); /* Keep Changes click */
    test_display(1u, true, false, false);  /* storage failure rolls back */
    test_display(0u, false, true, false);  /* failed switch: no dialog/save */
    test_display(0u, false, false, true);  /* failed rollback can be retried */
    test_panel();
    puts("control panel host test: PASS");
    return 0;
}
