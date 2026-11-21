#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/apps/book8_utilities.h"
#include "../include/syscall.h"

static uint8_t saved[65536];
static uint32_t saved_size, saves, clusters;
static char saved_path[256], first_hex[79];
static bool fail_save, fail_read, modal, saw_line_number;
void book8gui_invalidate(void) {}
void book8gui_set_modal(const struct WindowApp *app, bool active)
{ (void)app; modal = active; }

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    (void)color;
    assert(x <= 800u && w <= 800u - x);
    assert(y <= 600u && h <= 600u - y);
}
static void text(uint32_t x, uint32_t y, const char *value, uint32_t fg, uint32_t bg)
{
    (void)x; (void)fg; (void)bg;
    if (strcmp(value, "     1") == 0) { saw_line_number = true; }
    if (y == 101u && strlen(value) == 78u && value[8] == ':') {
        snprintf(first_hex, sizeof(first_hex), "%s", value);
    }
}
static bool info(const char *path, struct book8_file_info *out)
{
    *out = (struct book8_file_info){.size = strstr(path, "BIN") ? 512u : 6u};
    return true;
}
static bool read_file(const char *path, void *data, uint32_t capacity, uint32_t *size)
{
    if (fail_read) { memset(data, 0u, 6u); return false; }
    if (strstr(path, "BIN")) {
        assert(capacity >= 512u);
        for (uint32_t i = 0u; i < 512u; ++i) { ((uint8_t *)data)[i] = (uint8_t)i; }
        *size = 512u;
    } else { assert(capacity >= 6u); memcpy(data, "ab\ncd\n", 6u); *size = 6u; }
    return true;
}
static bool write_file(const char *path, const void *data, uint32_t size)
{
    ++saves;
    if (fail_save) { return false; }
    assert(size <= sizeof(saved));
    memcpy(saved, data, size);
    saved_size = size;
    snprintf(saved_path, sizeof(saved_path), "%s", path);
    return true;
}
static bool cluster(char drive, uint32_t number, void *data, uint32_t capacity, uint32_t *size)
{
    ++clusters;
    assert(capacity >= 512u);
    if (drive != 'A' || number != 2u) { return false; }
    memset(data, 0xff, 512u); *size = 512u;
    return true;
}
static void key(const struct book8gui_platform *p, struct WindowApp *app, uint32_t value)
{
    struct book8gui_event e = {.type = BOOK8GUI_EVENT_KEY, .key = value};
    assert(app->handle_event(p, app, &e));
}
static void click(const struct book8gui_platform *p, struct WindowApp *app, int32_t x, int32_t y)
{
    struct book8gui_event e = {.type = BOOK8GUI_EVENT_MOUSE, .mouse_x = x, .mouse_y = y, .mouse_buttons = 1u};
    assert(app->handle_event(p, app, &e));
    e.mouse_buttons = 0u; assert(app->handle_event(p, app, &e));
}

int main(void)
{
    struct book8gui_platform p = {.width = 800u, .height = 600u, .fill_rect = fill,
        .draw_text = text, .read_file = read_file, .file_info = info, .write_file = write_file,
        .read_cluster = cluster};
    struct WindowApp code = {.cls = &book8_codeedit_class, .bounds = {40u, 40u, 700u, 420u},
        .handle_event = book8_codeedit_class.handle_event};
    code.state = code.cls->create(&p);
    assert(code.state && code.cls->open_path(&p, &code, "A:\\SRC\\TEST.C"));
    code.cls->render(&p, &code);
    assert(saw_line_number);
    key(&p, &code, BOOK8GUI_KEY_END);
    key(&p, &code, BOOK8GUI_KEY_TAB);
    key(&p, &code, 'X');
    key(&p, &code, BOOK8GUI_KEY_LEFT);
    key(&p, &code, BOOK8GUI_KEY_DELETE);
    key(&p, &code, BOOK8GUI_KEY_BACKSPACE);
    key(&p, &code, BOOK8GUI_KEY_ENTER);
    key(&p, &code, 'q');
    key(&p, &code, BOOK8GUI_KEY_UP);
    key(&p, &code, BOOK8GUI_KEY_DOWN);
    assert(!code.cls->can_close(&p, &code));
    assert(!code.cls->open_path(&p, &code, "A:\\OTHER.C"));
    fail_save = true;
    key(&p, &code, 's' | BOOK8GUI_KEY_CTRL);
    assert(!code.cls->can_close(&p, &code));
    fail_save = false;
    key(&p, &code, 's' | BOOK8GUI_KEY_CTRL);
    assert(saved_size == 8u && memcmp(saved, "ab\nq\ncd\n", 8u) == 0);
    assert(strcmp(saved_path, "A:\\SRC\\TEST.C") == 0 && saves == 2u);
    assert(code.cls->can_close(&p, &code));
    key(&p, &code, 'S' | BOOK8GUI_KEY_CTRL | BOOK8GUI_KEY_SHIFT);
    assert(modal);
    for (uint32_t i = 0u; i < 30u; ++i) { key(&p, &code, BOOK8GUI_KEY_BACKSPACE); }
    const char *path = "A:\\SYS\\CONFIG.CFG";
    for (uint32_t i = 0u; path[i] != '\0'; ++i) { key(&p, &code, (uint32_t)path[i]); }
    key(&p, &code, BOOK8GUI_KEY_ENTER);
    assert(!modal && strcmp(saved_path, path) == 0 && strstr(code.title, path));
    key(&p, &code, 'o' | BOOK8GUI_KEY_CTRL);
    key(&p, &code, BOOK8GUI_KEY_ESCAPE);
    assert(!modal);
    fail_read = true;
    assert(!code.cls->open_path(&p, &code, "A:\\OTHER.C"));
    fail_read = false;
    assert(!code.cls->open_path(&p, &code, "A:\\BIN\\DATA.BIN"));
    key(&p, &code, 's' | BOOK8GUI_KEY_CTRL);
    assert(saved_size == 8u && memcmp(saved, "ab\nq\ncd\n", 8u) == 0);
    assert(strcmp(saved_path, path) == 0);

    struct WindowApp hex = {.cls = &book8_hexview_class, .bounds = {40u, 40u, 700u, 420u},
        .handle_event = book8_hexview_class.handle_event};
    hex.state = hex.cls->create(&p);
    assert(hex.state && hex.cls->open_path(&p, &hex, "A:\\BIN\\DATA.BIN"));
    char line[79];
    const uint8_t bytes[] = {0u, 31u, 32u, 'A', 126u, 127u, 255u};
    book8_hex_format(0x12345678u, bytes, sizeof(bytes), line);
    assert(strncmp(line, "12345678: 00 1F 20 41 7E 7F FF", 29u) == 0);
    assert(strncmp(line + 61u, ".. A~..", 7u) == 0 && line[77] == '|');
    hex.cls->render(&p, &hex);
    assert(strncmp(first_hex, "00000000:", 9u) == 0);
    struct book8gui_event wheel = {.type = BOOK8GUI_EVENT_MOUSE, .mouse_x = 100, .mouse_y = 120, .wheel_delta = -60};
    assert(hex.handle_event(&p, &hex, &wheel)); hex.cls->render(&p, &hex);
    assert(strncmp(first_hex, "00000000:", 9u) == 0);
    assert(hex.handle_event(&p, &hex, &wheel)); hex.cls->render(&p, &hex);
    assert(strncmp(first_hex, "00000030:", 9u) == 0);
    wheel.wheel_delta = -120;
    assert(hex.handle_event(&p, &hex, &wheel)); hex.cls->render(&p, &hex);
    assert(strncmp(first_hex, "00000060:", 9u) == 0);
    wheel.wheel_delta = 240;
    assert(hex.handle_event(&p, &hex, &wheel)); hex.cls->render(&p, &hex);
    assert(strncmp(first_hex, "00000000:", 9u) == 0);
    uint32_t writes = saves;
    struct book8gui_event typing = {.type = BOOK8GUI_EVENT_KEY, .key = 'Z'};
    assert(!hex.handle_event(&p, &hex, &typing) && saves == writes);
    click(&p, &hex, 120, 80);
    assert(modal);
    key(&p, &hex, BOOK8GUI_KEY_ENTER);
    assert(!modal && clusters == 1u);
    hex.cls->render(&p, &hex);
    assert(strstr(first_hex, "FF FF FF") && strstr(first_hex, "................"));
    click(&p, &hex, 120, 80);
    key(&p, &hex, BOOK8GUI_KEY_BACKSPACE);
    key(&p, &hex, '1');
    key(&p, &hex, BOOK8GUI_KEY_ENTER);
    assert(modal);
    key(&p, &hex, BOOK8GUI_KEY_ESCAPE);
    hex.cls->render(&p, &hex);
    assert(strstr(first_hex, "FF FF FF"));
    code.cls->destroy(code.state); hex.cls->destroy(hex.state);
    puts("PASS: utility hex formatting, wheel, raw cluster, pure editing, Save/Save As and errors");
    return 0;
}
