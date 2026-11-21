#include "book8_paintbook.h"
#include "book8_image.h"
#include "book8_ui.h"

#include <stddef.h>
#include "../../include/stdio.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"

#define PAINT_FILE_CAPACITY (4u * 1024u * 1024u)
#define PAINT_PATH_CAPACITY 256u
#define PAINT_MENU_H 20u
#define PAINT_STATUS_H 20u
#define PAINT_MAX_BYTES (BOOK8_IMAGE_MAX_PIXELS * 4u)
#define PAINT_SAVE_CAPACITY (BOOK8_IMAGE_MAX_PIXELS * 3u + 54u)
#define MENU_NONE UINT32_MAX

enum paint_tool { TOOL_PENCIL, TOOL_ERASER, TOOL_LINE, TOOL_RECT, TOOL_FILL };
enum paint_dialog { DIALOG_NONE, DIALOG_OPEN, DIALOG_SAVE_AS };

struct paint_state {
    bool used, dirty, mouse_down, shape_started;
    uint8_t previous_buttons;
    enum paint_tool tool;
    enum paint_dialog dialog;
    uint32_t menu, zoom, color;
    uint32_t width, height, scroll_x, scroll_y;
    int32_t start_x, start_y, last_x, last_y;
    char path[PAINT_PATH_CAPACITY], field[PAINT_PATH_CAPACITY];
    char message[128], title[PAINT_PATH_CAPACITY + 32u];
    uint8_t *pixels;
};

struct paint_layout {
    struct book8gui_rect menu, toolbar, palette, canvas, status;
    struct book8gui_rect tools[5], colors[12];
};

static struct paint_state paint;
static uint8_t file_buffer[PAINT_FILE_CAPACITY];
static uint8_t save_buffer[PAINT_SAVE_CAPACITY];
static uint8_t undo_buffer[PAINT_MAX_BYTES];
static bool undo_valid;
static uint32_t undo_width, undo_height;

static const uint32_t palette[] = {
    0x000000u, 0xffffffu, 0x808080u, 0xc0c0c0u, 0x800000u, 0xff0000u,
    0x808000u, 0xffff00u, 0x008000u, 0x00ff00u, 0x000080u, 0x0000ffu
};
static const char *const tool_names[] = {
    "Pencil", "Eraser", "Line", "Rectangle", "Filled"
};
static const char *const menu_names[] = {"File", "Edit", "View", "Help"};

static uint32_t image_bytes(void)
{
    return paint.width * paint.height * 4u;
}

static void set_message(const char *text)
{
    snprintf(paint.message, sizeof(paint.message), "%s", text);
}

static void set_title(struct WindowApp *app)
{
    snprintf(paint.title, sizeof(paint.title), "PaintBook - %s%s",
        paint.path[0] != '\0' ? paint.path : "Untitled",
        paint.dirty ? " *" : "");
    app->title = paint.title;
}

static void layout(const struct WindowApp *app, struct paint_layout *v)
{
    struct book8gui_rect c = ui_client(app);
    v->menu = (struct book8gui_rect){c.x, c.y, c.width, PAINT_MENU_H};
    v->toolbar = (struct book8gui_rect){c.x, c.y + PAINT_MENU_H + 2u,
        c.width, 26u};
    v->palette = (struct book8gui_rect){c.x, c.y + PAINT_MENU_H + 30u,
        c.width, 22u};
    v->canvas = (struct book8gui_rect){c.x, c.y + PAINT_MENU_H + 56u,
        c.width, c.height > PAINT_MENU_H + 80u
            ? c.height - PAINT_MENU_H - 80u : 0u};
    v->status = (struct book8gui_rect){c.x, c.y + c.height - PAINT_STATUS_H,
        c.width, PAINT_STATUS_H};
    uint32_t x = c.x;
    for (uint32_t i = 0u; i < 5u; ++i) {
        v->tools[i] = (struct book8gui_rect){x, v->toolbar.y, 76u, 24u};
        x += 78u;
    }
    x = c.x;
    for (uint32_t i = 0u; i < 12u; ++i) {
        v->colors[i] = (struct book8gui_rect){x, v->palette.y, 22u, 20u};
        x += 24u;
    }
}

static bool contains(const struct book8gui_rect *r, int32_t x, int32_t y)
{
    return ui_contains(r, x, y);
}

static void clipped_text(const struct book8gui_platform *p, uint32_t x,
    uint32_t y, uint32_t width, const char *text, uint32_t fg, uint32_t bg)
{
    char line[160];
    uint32_t n = 0u;
    while (text[n] != '\0' && n + 1u < sizeof(line) && n < width / 8u) {
        line[n] = text[n];
        ++n;
    }
    line[n] = '\0';
    p->draw_text(x, y, line, fg, bg);
}

static void button(const struct book8gui_platform *p,
    const struct book8gui_rect *r, const char *text, bool selected)
{
    ui_sunken(p, r, selected ? UI_SELECTED : UI_FACE);
    clipped_text(p, r->x + 4u, r->y + 4u, r->width - 8u, text,
        selected ? UI_SELECTED_TEXT : UI_TEXT,
        selected ? UI_SELECTED : UI_FACE);
}

static void preserve_undo(void)
{
    uint32_t bytes = image_bytes();
    if (paint.pixels != NULL && bytes <= sizeof(undo_buffer)) {
        memcpy(undo_buffer, paint.pixels, bytes);
        undo_width = paint.width;
        undo_height = paint.height;
        undo_valid = true;
    }
}

static void changed(struct WindowApp *app, const char *message)
{
    paint.dirty = true;
    set_message(message);
    set_title(app);
    book8gui_invalidate();
}

static bool path_copy(char *out, uint32_t capacity, const char *path)
{
    uint32_t n = 0u;
    while (n + 1u < capacity && path[n] != '\0') {
        out[n] = path[n];
        ++n;
    }
    out[n] = '\0';
    return path[n] == '\0';
}

static bool load_image(const struct book8gui_platform *p,
    struct WindowApp *app, const char *path)
{
    uint32_t size = 0u;
    if (p->read_file == NULL
        || !p->read_file(path, file_buffer, sizeof(file_buffer), &size)) {
        set_message("Open failed: file unreadable or larger than 4 MiB.");
        return false;
    }
    book8_image_release();
    struct book8_decoded_image image;
    if (!book8_image_decode(file_buffer, size, &image)) {
        paint.pixels = NULL;
        set_message("Unsupported or invalid image (BMP, JPEG, GIF, PNG).");
        return false;
    }
    paint.pixels = image.rgba;
    paint.width = image.width;
    paint.height = image.height;
    paint.scroll_x = paint.scroll_y = 0u;
    if (!path_copy(paint.path, sizeof(paint.path), path)) {
        paint.path[0] = '\0';
    }
    paint.dirty = false;
    undo_valid = false;
    snprintf(paint.message, sizeof(paint.message), "%ux%u image loaded",
        paint.width, paint.height);
    set_title(app);
    book8gui_invalidate();
    return true;
}

static bool new_image(struct WindowApp *app, uint32_t width, uint32_t height)
{
    book8_image_release();
    paint.pixels = book8_image_new(width, height, 0xffffffu);
    if (paint.pixels == NULL) {
        paint.width = paint.height = 0u;
        set_message("New image failed: canvas allocation limit reached.");
        return false;
    }
    paint.width = width;
    paint.height = height;
    paint.path[0] = '\0';
    paint.scroll_x = paint.scroll_y = 0u;
    paint.dirty = false;
    undo_valid = false;
    set_message("New 640x480 canvas.");
    if (app != NULL) {
        set_title(app);
    }
    book8gui_invalidate();
    return true;
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u);
    p[3] = (uint8_t)(value >> 24u);
}

static bool save_bmp(const struct book8gui_platform *p,
    struct WindowApp *app, const char *path)
{
    if (paint.pixels == NULL || p->write_file == NULL) {
        set_message("Save failed: no canvas or writable filesystem.");
        return false;
    }
    uint32_t row = (paint.width * 3u + 3u) & ~3u;
    uint32_t size = 54u + row * paint.height;
    if (size > sizeof(save_buffer)) {
        set_message("Save failed: image exceeds BMP size limit.");
        return false;
    }
    memset(save_buffer, 0, 54u);
    save_buffer[0] = 'B';
    save_buffer[1] = 'M';
    write_u32(save_buffer + 2u, size);
    write_u32(save_buffer + 10u, 54u);
    write_u32(save_buffer + 14u, 40u);
    write_u32(save_buffer + 18u, paint.width);
    write_u32(save_buffer + 22u, paint.height);
    write_u16(save_buffer + 26u, 1u);
    write_u16(save_buffer + 28u, 24u);
    write_u32(save_buffer + 34u, row * paint.height);
    for (uint32_t y = 0u; y < paint.height; ++y) {
        uint8_t *dst = save_buffer + 54u + (paint.height - 1u - y) * row;
        const uint8_t *src = paint.pixels + (uint64_t)y * paint.width * 4u;
        for (uint32_t x = 0u; x < paint.width; ++x) {
            dst[x * 3u] = src[x * 4u + 2u];
            dst[x * 3u + 1u] = src[x * 4u + 1u];
            dst[x * 3u + 2u] = src[x * 4u];
        }
    }
    if (!p->write_file(path, save_buffer, size)) {
        set_message("Save failed: the destination was not written.");
        return false;
    }
    (void)path_copy(paint.path, sizeof(paint.path), path);
    paint.dirty = false;
    snprintf(paint.message, sizeof(paint.message), "Saved %s as BMP", path);
    set_title(app);
    book8gui_invalidate();
    return true;
}

static void save_current(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    const char *path = paint.path;
    uint32_t n = (uint32_t)strlen(path);
    bool bmp = n >= 4u && path[n - 4u] == '.'
        && (path[n - 3u] == 'b' || path[n - 3u] == 'B')
        && (path[n - 2u] == 'm' || path[n - 2u] == 'M')
        && (path[n - 1u] == 'p' || path[n - 1u] == 'P');
    if (!bmp) {
        path = "A:\\PAINTBOOK.BMP";
    }
    (void)save_bmp(p, app, path);
}

static bool image_point(const struct paint_layout *v, int32_t x, int32_t y,
    int32_t *ix, int32_t *iy)
{
    if (!contains(&v->canvas, x, y) || paint.pixels == NULL) {
        return false;
    }
    int64_t px = ((int64_t)x - v->canvas.x - 2) / paint.zoom + paint.scroll_x;
    int64_t py = ((int64_t)y - v->canvas.y - 2) / paint.zoom + paint.scroll_y;
    if (px < 0 || py < 0 || px >= paint.width || py >= paint.height) {
        return false;
    }
    *ix = (int32_t)px;
    *iy = (int32_t)py;
    return true;
}

static void put_pixel(int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= paint.width
        || (uint32_t)y >= paint.height) {
        return;
    }
    uint8_t *p = paint.pixels + ((uint64_t)(uint32_t)y * paint.width
        + (uint32_t)x) * 4u;
    p[0] = (uint8_t)(color >> 16u);
    p[1] = (uint8_t)(color >> 8u);
    p[2] = (uint8_t)color;
    p[3] = 0xffu;
}

static void line_pixels(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    uint32_t color)
{
    int32_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    for (;;) {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) { break; }
        int32_t twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static void draw_shape(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t color = paint.tool == TOOL_ERASER ? 0xffffffu : paint.color;
    if (paint.tool == TOOL_PENCIL || paint.tool == TOOL_ERASER
        || paint.tool == TOOL_LINE) {
        line_pixels(x0, y0, x1, y1, color);
        return;
    }
    int32_t left = x0 < x1 ? x0 : x1, right = x0 > x1 ? x0 : x1;
    int32_t top = y0 < y1 ? y0 : y1, bottom = y0 > y1 ? y0 : y1;
    if (paint.tool == TOOL_FILL) {
        for (int32_t y = top; y <= bottom; ++y) {
            line_pixels(left, y, right, y, color);
        }
    } else {
        line_pixels(left, top, right, top, color);
        line_pixels(left, bottom, right, bottom, color);
        line_pixels(left, top, left, bottom, color);
        line_pixels(right, top, right, bottom, color);
    }
}

static void draw_image(const struct book8gui_platform *p,
    const struct paint_layout *v)
{
    ui_sunken(p, &v->canvas, UI_FIELD);
    if (paint.pixels == NULL || v->canvas.width < 5u || v->canvas.height < 5u) {
        clipped_text(p, v->canvas.x + 8u, v->canvas.y + 8u,
            v->canvas.width > 16u ? v->canvas.width - 16u : 0u,
            "Use File > New or Open to begin.", UI_MUTED, UI_FIELD);
        return;
    }
    uint32_t max_w = (v->canvas.width - 4u) / paint.zoom;
    uint32_t max_h = (v->canvas.height - 4u) / paint.zoom;
    uint32_t width = paint.width - paint.scroll_x;
    uint32_t height = paint.height - paint.scroll_y;
    if (width > max_w) { width = max_w; }
    if (height > max_h) { height = max_h; }
    if (p->draw_rgba != NULL) {
        const uint8_t *pixels = paint.pixels
            + ((uint64_t)paint.scroll_y * paint.width + paint.scroll_x) * 4u;
        p->draw_rgba(v->canvas.x + 2u, v->canvas.y + 2u, width, height,
            pixels, paint.width * 4u, paint.zoom);
        return;
    }
    for (uint32_t y = 0u; y < height; ++y) {
        const uint8_t *row = paint.pixels
            + ((uint64_t)(y + paint.scroll_y) * paint.width
                + paint.scroll_x) * 4u;
        uint32_t run = 0u;
        while (run < width) {
            const uint8_t *pixel = row + run * 4u;
            uint32_t alpha = pixel[3];
            uint32_t rgb = ((pixel[0] * alpha + 255u * (255u - alpha)) / 255u) << 16u
                | ((pixel[1] * alpha + 255u * (255u - alpha)) / 255u) << 8u
                | ((pixel[2] * alpha + 255u * (255u - alpha)) / 255u);
            uint32_t end = run + 1u;
            while (end < width) {
                const uint8_t *next = row + end * 4u;
                if (next[0] != pixel[0] || next[1] != pixel[1]
                    || next[2] != pixel[2] || next[3] != pixel[3]) {
                    break;
                }
                ++end;
            }
            p->fill_rect(v->canvas.x + 2u + run * paint.zoom,
                v->canvas.y + 2u + y * paint.zoom,
                (end - run) * paint.zoom, paint.zoom, rgb);
            run = end;
        }
    }
}

static uint32_t menu_item_count(uint32_t menu)
{
    static const uint8_t counts[] = {5u, 2u, 3u, 1u};
    return menu < sizeof(counts) ? counts[menu] : 0u;
}

static const char *menu_item_label(uint32_t menu, uint32_t item)
{
    static const char *const file[] = {"New", "Open...", "Save", "Save As...", "Exit"};
    static const char *const edit[] = {"Undo", "Clear"};
    static const char *const view[] = {"Zoom In", "Zoom Out", "Actual Size"};
    static const char *const help[] = {"About PaintBook"};
    const char *const *items = menu == 0u ? file : menu == 1u ? edit
        : menu == 2u ? view : help;
    return item < menu_item_count(menu) ? items[item] : "";
}

static void open_dialog(enum paint_dialog dialog)
{
    paint.dialog = dialog;
    snprintf(paint.field, sizeof(paint.field), "%s",
        dialog == DIALOG_SAVE_AS ? "A:\\PAINTBOOK.BMP" : "");
    paint.menu = MENU_NONE;
}

static void menu_action(const struct book8gui_platform *p,
    struct WindowApp *app, uint32_t menu, uint32_t item)
{
    paint.menu = MENU_NONE;
    if (menu == 0u) {
        if (item == 0u) { (void)new_image(app, 640u, 480u); }
        else if (item == 1u) { open_dialog(DIALOG_OPEN); }
        else if (item == 2u) { save_current(p, app); }
        else if (item == 3u) { open_dialog(DIALOG_SAVE_AS); }
        else if (item == 4u) { book8gui_request_close(app); }
    } else if (menu == 1u) {
        if (item == 0u && undo_valid && undo_width == paint.width
            && undo_height == paint.height) {
            memcpy(paint.pixels, undo_buffer, image_bytes());
            undo_valid = false;
            changed(app, "Undo complete.");
        } else if (item == 1u && paint.pixels != NULL) {
            preserve_undo();
            for (uint32_t i = 0u; i < paint.width * paint.height; ++i) {
                put_pixel((int32_t)(i % paint.width),
                    (int32_t)(i / paint.width), 0xffffffu);
            }
            changed(app, "Canvas cleared.");
        }
    } else if (menu == 2u) {
        if (item == 0u && paint.zoom < 4u) { ++paint.zoom; }
        else if (item == 1u && paint.zoom > 1u) { --paint.zoom; }
        else if (item == 2u) { paint.zoom = 1u; }
        book8gui_invalidate();
    } else if (menu == 3u) {
        set_message("PaintBook: BMP, JPEG, GIF and PNG viewer with BMP drawing export.");
    }
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (app->title == book8_paintbook_class.name) {
        set_title(app);
    }
    struct paint_layout v;
    layout(app, &v);
    for (uint32_t i = 0u; i < 4u; ++i) {
        uint32_t x = v.menu.x + 4u + i * 48u;
        struct book8gui_rect r = {x, v.menu.y, i == 3u ? 56u : 48u, 20u};
        button(p, &r, menu_names[i], paint.menu == i);
    }
    for (uint32_t i = 0u; i < 5u; ++i) {
        button(p, &v.tools[i], tool_names[i], paint.tool == i);
    }
    for (uint32_t i = 0u; i < 12u; ++i) {
        p->fill_rect(v.colors[i].x, v.colors[i].y,
            v.colors[i].width, v.colors[i].height, palette[i]);
        if (paint.color == palette[i]) {
            p->fill_rect(v.colors[i].x, v.colors[i].y, v.colors[i].width, 2u, UI_SELECTED);
            p->fill_rect(v.colors[i].x, v.colors[i].y + v.colors[i].height - 2u,
                v.colors[i].width, 2u, UI_SELECTED);
        }
    }
    draw_image(p, &v);
    ui_sunken(p, &v.status, UI_FACE);
    char status[160];
    snprintf(status, sizeof(status), "%s  %ux%u  %ux zoom",
        paint.message[0] != '\0' ? paint.message : "Ready",
        paint.width, paint.height, paint.zoom);
    clipped_text(p, v.status.x + 4u, v.status.y + 2u,
        v.status.width - 8u, status, UI_TEXT, UI_FACE);
    if (paint.menu != MENU_NONE) {
        uint32_t count = menu_item_count(paint.menu);
        uint32_t x = v.menu.x + paint.menu * 48u + 4u;
        uint32_t width = paint.menu == 0u ? 112u : 144u;
        struct book8gui_rect box = {x, v.menu.y + 20u, width, count * 22u + 4u};
        ui_sunken(p, &box, UI_FACE);
        for (uint32_t i = 0u; i < count; ++i) {
            p->draw_text(x + 8u, box.y + 4u + i * 22u,
                menu_item_label(paint.menu, i), UI_TEXT, UI_FACE);
        }
    }
    if (paint.dialog != DIALOG_NONE) {
        struct book8gui_rect box = {
            (p->width - 440u) / 2u,
            (p->height - BOOK8GUI_TASKBAR_HEIGHT - 144u) / 2u,
            440u, 144u
        };
        ui_sunken(p, &box, UI_FACE);
        p->draw_text(box.x + 8u, box.y + 8u,
            paint.dialog == DIALOG_OPEN
                ? "Open image: enter an absolute FAT path."
                : "Save as BMP: enter an absolute FAT path.",
            UI_TEXT, UI_FACE);
        struct book8gui_rect field = {box.x + 8u, box.y + 36u, box.width - 16u, 24u};
        ui_sunken(p, &field, UI_FIELD);
        clipped_text(p, field.x + 4u, field.y + 4u, field.width - 8u,
            paint.field, UI_TEXT, UI_FIELD);
        struct book8gui_rect ok = {box.x + box.width - 144u, box.y + 104u, 64u, 24u};
        struct book8gui_rect cancel = {box.x + box.width - 72u, box.y + 104u, 64u, 24u};
        button(p, &ok, "OK", false);
        button(p, &cancel, "Cancel", false);
    }
}

static void dialog_submit(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    enum paint_dialog dialog = paint.dialog;
    paint.dialog = DIALOG_NONE;
    if (dialog == DIALOG_OPEN) {
        (void)load_image(p, app, paint.field);
    } else if (dialog == DIALOG_SAVE_AS) {
        (void)save_bmp(p, app, paint.field);
    }
}

static bool handle_event(const struct book8gui_platform *p,
    struct WindowApp *app, const struct book8gui_event *event)
{
    struct paint_layout v;
    layout(app, &v);
    if (event->type == BOOK8GUI_EVENT_KEY) {
        uint32_t key = event->key;
        if (paint.dialog != DIALOG_NONE) {
            uint32_t n = (uint32_t)strlen(paint.field);
            if (key == BOOK8GUI_KEY_ESCAPE) {
                paint.dialog = DIALOG_NONE;
            } else if (key == BOOK8GUI_KEY_ENTER) {
                dialog_submit(p, app);
            } else if (key == BOOK8GUI_KEY_BACKSPACE && n != 0u) {
                paint.field[n - 1u] = '\0';
            } else if ((key & BOOK8GUI_KEY_MASK) >= 32u
                && (key & BOOK8GUI_KEY_MASK) < 127u && n + 1u < sizeof(paint.field)) {
                paint.field[n] = (char)(key & BOOK8GUI_KEY_MASK);
                paint.field[n + 1u] = '\0';
            }
            book8gui_invalidate();
            return true;
        }
        if ((key & BOOK8GUI_KEY_CTRL) != 0u) {
            uint32_t ch = key & BOOK8GUI_KEY_MASK;
            if (ch == 'o') { open_dialog(DIALOG_OPEN); }
            else if (ch == 's') { save_current(p, app); }
            else if (ch == 'n') { (void)new_image(app, 640u, 480u); }
            else if (ch == 'z' && undo_valid
                && undo_width == paint.width && undo_height == paint.height) {
                memcpy(paint.pixels, undo_buffer, image_bytes());
                undo_valid = false;
                changed(app, "Undo complete.");
            }
            return true;
        }
        if (key == BOOK8GUI_KEY_ESCAPE && paint.menu != MENU_NONE) {
            paint.menu = MENU_NONE;
            book8gui_invalidate();
            return true;
        }
        return false;
    }

    bool down = (event->mouse_buttons & 1u) != 0u;
    bool pressed = down && (paint.previous_buttons & 1u) == 0u;
    bool released = !down && (paint.previous_buttons & 1u) != 0u;
    paint.previous_buttons = event->mouse_buttons;
    if (paint.dialog != DIALOG_NONE) {
        struct book8gui_rect box = {(p->width - 440u) / 2u,
            (p->height - BOOK8GUI_TASKBAR_HEIGHT - 144u) / 2u, 440u, 144u};
        struct book8gui_rect ok = {box.x + box.width - 144u, box.y + 104u, 64u, 24u};
        struct book8gui_rect cancel = {box.x + box.width - 72u, box.y + 104u, 64u, 24u};
        if (pressed && contains(&ok, event->mouse_x, event->mouse_y)) {
            dialog_submit(p, app);
        } else if (pressed && contains(&cancel, event->mouse_x, event->mouse_y)) {
            paint.dialog = DIALOG_NONE;
        }
        book8gui_invalidate();
        return true;
    }
    if (pressed && contains(&v.menu, event->mouse_x, event->mouse_y)) {
        uint32_t index = event->mouse_x >= (int32_t)v.menu.x + 148
            ? 3u : ((uint32_t)event->mouse_x - v.menu.x - 4u) / 48u;
        if (index < 4u) {
            paint.menu = paint.menu == index ? MENU_NONE : index;
        }
        book8gui_invalidate();
        return true;
    }
    if (paint.menu != MENU_NONE) {
        uint32_t x = v.menu.x + paint.menu * 48u + 4u;
        uint32_t width = paint.menu == 0u ? 112u : 144u;
        struct book8gui_rect box = {x, v.menu.y + 20u, width,
            menu_item_count(paint.menu) * 22u + 4u};
        if (pressed && contains(&box, event->mouse_x, event->mouse_y)) {
            uint32_t item = ((uint32_t)event->mouse_y - box.y - 2u) / 22u;
            menu_action(p, app, paint.menu, item);
        } else if (pressed) {
            paint.menu = MENU_NONE;
        }
        book8gui_invalidate();
        return true;
    }
    if (pressed) {
        for (uint32_t i = 0u; i < 5u; ++i) {
            if (contains(&v.tools[i], event->mouse_x, event->mouse_y)) {
                paint.tool = (enum paint_tool)i;
                book8gui_invalidate();
                return true;
            }
        }
        for (uint32_t i = 0u; i < 12u; ++i) {
            if (contains(&v.colors[i], event->mouse_x, event->mouse_y)) {
                paint.color = palette[i];
                book8gui_invalidate();
                return true;
            }
        }
    }
    if (event->wheel_delta != 0 && contains(&v.canvas,
            event->mouse_x, event->mouse_y)) {
        int32_t direction = event->wheel_delta > 0 ? -1 : 1;
        if (paint.height > 0u) {
            int64_t next = (int64_t)paint.scroll_y + direction * 3;
            uint32_t visible = v.canvas.height > 4u
                ? (v.canvas.height - 4u) / paint.zoom : 0u;
            uint32_t maximum = paint.height > visible ? paint.height - visible : 0u;
            paint.scroll_y = next < 0 ? 0u
                : (uint32_t)next > maximum ? maximum : (uint32_t)next;
        }
        book8gui_invalidate();
        return true;
    }
    int32_t ix, iy;
    bool on_image = image_point(&v, event->mouse_x, event->mouse_y, &ix, &iy);
    if (pressed && on_image) {
        preserve_undo();
        paint.mouse_down = true;
        paint.shape_started = true;
        paint.start_x = paint.last_x = ix;
        paint.start_y = paint.last_y = iy;
        if (paint.tool == TOOL_PENCIL || paint.tool == TOOL_ERASER) {
            draw_shape(ix, iy, ix, iy);
            changed(app, "Drawing...");
        }
    } else if (down && paint.mouse_down && on_image
        && (paint.tool == TOOL_PENCIL || paint.tool == TOOL_ERASER)) {
        draw_shape(paint.last_x, paint.last_y, ix, iy);
        paint.last_x = ix;
        paint.last_y = iy;
        changed(app, "Drawing...");
    } else if (released && paint.mouse_down) {
        if (paint.tool != TOOL_PENCIL && paint.tool != TOOL_ERASER
            && paint.shape_started) {
            if (!on_image) {
                ix = paint.last_x;
                iy = paint.last_y;
            }
            draw_shape(paint.start_x, paint.start_y, ix, iy);
            changed(app, "Shape drawn.");
        }
        paint.mouse_down = false;
        paint.shape_started = false;
    }
    return on_image || paint.mouse_down;
}

static void *create(const struct book8gui_platform *p)
{
    (void)p;
    if (paint.used) {
        return NULL;
    }
    paint = (struct paint_state){.used = true, .zoom = 1u,
        .color = 0x000000u, .menu = MENU_NONE, .tool = TOOL_PENCIL};
    undo_valid = false;
    if (new_image(NULL, 640u, 480u)) {
        paint.dirty = false;
    }
    return &paint;
}

static void destroy(void *state)
{
    (void)state;
    book8_image_release();
    undo_valid = false;
    paint = (struct paint_state){0};
}

static bool open_path(const struct book8gui_platform *p,
    struct WindowApp *app, const char *path)
{
    return load_image(p, app, path);
}

const struct book8gui_app_class book8_paintbook_class = {
    .name = "PaintBook",
    .default_width = 760u,
    .default_height = 520u,
    .single_instance = true,
    .wants_all_keys = true,
    .state_size = sizeof(struct paint_state),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event,
    .open_path = open_path
};
