#include "book8_utilities.h"
#include "book8_ui.h"
#include "../../include/stdio.h"

#define HEX_CAPACITY (1024u * 1024u)
#define CODE_CAPACITY 65536u
#define NO_POSITION UINT32_MAX

enum utility_dialog { DIALOG_NONE, DIALOG_OPEN, DIALOG_SAVE_AS, DIALOG_CLUSTER };
struct utility_state {
    bool used, hex, dirty, dragging;
    uint8_t previous_buttons;
    int32_t scroll_grab;
    int32_t wheel_remainder;
    uint32_t length, cursor, first, horizontal, preferred_column;
    bool preferred_valid;
    enum utility_dialog dialog;
    char path[256], field[256], title[280], message[128];
    uint8_t data[HEX_CAPACITY];
};
static struct utility_state states[BOOK8GUI_MAX_INSTANCES_PER_APP];
static uint8_t read_buffer[HEX_CAPACITY];

struct utility_layout {
    struct book8gui_rect frame, track, open, save, save_as, cluster, status;
    uint32_t rows, columns, text_x;
};

static void layout(const struct WindowApp *app, const struct utility_state *s,
    struct utility_layout *v)
{
    struct book8gui_rect client = ui_client(app);
    v->open = (struct book8gui_rect){client.x, client.y, 56u, 24u};
    v->save = (struct book8gui_rect){client.x + 60u, client.y, 56u, 24u};
    v->save_as = (struct book8gui_rect){client.x + 120u, client.y, 80u, 24u};
    v->cluster = (struct book8gui_rect){client.x + 60u, client.y, 112u, 24u};
    v->frame = (struct book8gui_rect){client.x, client.y + 28u, client.width,
        client.height > 52u ? client.height - 52u : 0u};
    v->status = (struct book8gui_rect){client.x, client.y + client.height - 20u,
        client.width, 20u};
    v->track = ui_scroll_track(&v->frame);
    v->rows = v->frame.height > 6u ? (v->frame.height - 6u) / 16u : 0u;
    v->text_x = v->frame.x + (s->hex ? 3u : 59u);
    uint32_t margin = s->hex ? 6u : 62u;
    v->columns = v->frame.width > UI_SCROLL_W + margin
        ? (v->frame.width - UI_SCROLL_W - margin) / 8u : 0u;
    if (v->columns > 127u) { v->columns = 127u; }
}

static void notify(const struct book8gui_platform *p, struct utility_state *s,
    const char *message)
{
    snprintf(s->message, sizeof(s->message), "%s", message);
    if (p->diagnostic != NULL) {
        p->diagnostic(message);
        p->diagnostic("\n");
    }
    book8gui_invalidate();
}

static void title(struct WindowApp *app, struct utility_state *s)
{
    if (s->hex) {
        snprintf(s->title, sizeof(s->title), "Hex Sector Viewer");
    } else {
        snprintf(s->title, sizeof(s->title), "Code Editor - %s%s",
            s->path[0] != '\0' ? s->path : "Untitled", s->dirty ? " *" : "");
    }
    app->title = s->title;
}

static uint32_t line_start(const struct utility_state *s, uint32_t pos)
{
    while (pos != 0u && s->data[pos - 1u] != '\n') { --pos; }
    return pos;
}

static uint32_t line_end(const struct utility_state *s, uint32_t pos)
{
    while (pos < s->length && s->data[pos] != '\n') { ++pos; }
    return pos;
}

static uint32_t total_lines(const struct utility_state *s)
{
    if (s->hex) { return (s->length + 15u) / 16u; }
    uint32_t count = 1u;
    for (uint32_t i = 0u; i < s->length; ++i) {
        if (s->data[i] == '\n') { ++count; }
    }
    return count;
}

static uint32_t row_start(const struct utility_state *s, uint32_t row)
{
    uint32_t pos = 0u;
    while (row != 0u && pos < s->length) {
        pos = line_end(s, pos);
        if (pos == s->length) { return NO_POSITION; }
        ++pos;
        --row;
    }
    return row == 0u ? pos : NO_POSITION;
}

static uint32_t column_at(const struct utility_state *s, uint32_t start, uint32_t pos)
{
    uint32_t col = 0u;
    for (uint32_t i = start; i < pos; ++i) {
        col += s->data[i] == '\r' ? 0u : s->data[i] == '\t' ? 4u - col % 4u : 1u;
    }
    return col;
}

static uint32_t at_column(const struct utility_state *s, uint32_t start, uint32_t col)
{
    uint32_t end = line_end(s, start), pos = start, visual = 0u;
    while (pos < end) {
        uint32_t next = visual + (s->data[pos] == '\r' ? 0u
            : s->data[pos] == '\t' ? 4u - visual % 4u : 1u);
        if (next > col) { break; }
        visual = next;
        ++pos;
    }
    return pos;
}

static void follow_cursor(struct utility_state *s, const struct utility_layout *v)
{
    uint32_t row = 0u;
    for (uint32_t i = 0u; i < s->cursor; ++i) {
        if (s->data[i] == '\n') { ++row; }
    }
    if (row < s->first) { s->first = row; }
    if (v->rows != 0u && row >= s->first + v->rows) { s->first = row - v->rows + 1u; }
    uint32_t col = column_at(s, line_start(s, s->cursor), s->cursor);
    if (col < s->horizontal) { s->horizontal = col; }
    if (v->columns != 0u && col >= s->horizontal + v->columns) {
        s->horizontal = col - v->columns + 1u;
    }
}

void book8_hex_format(uint32_t offset, const uint8_t *data, uint32_t size,
    char line[79])
{
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0u; i < 78u; ++i) { line[i] = ' '; }
    line[78] = '\0';
    for (uint32_t i = 0u; i < 8u; ++i) {
        line[i] = hex[(offset >> ((7u - i) * 4u)) & 15u];
    }
    line[8] = ':';
    line[60] = '|';
    line[77] = '|';
    for (uint32_t i = 0u; i < 16u && i < size; ++i) {
        uint8_t byte = data[i];
        line[10u + i * 3u] = hex[byte >> 4u];
        line[11u + i * 3u] = hex[byte & 15u];
        line[61u + i] = byte >= 32u && byte <= 126u ? (char)byte : '.';
    }
}

static void clipped(const struct book8gui_platform *p, uint32_t x, uint32_t y,
    uint32_t width, const char *text, uint32_t fg, uint32_t bg)
{
    char line[128];
    uint32_t n = 0u;
    while (text[n] != '\0' && n < width / 8u && n < 127u) { line[n] = text[n]; ++n; }
    line[n] = '\0';
    p->draw_text(x, y, line, fg, bg);
}

static void button(const struct book8gui_platform *p, const struct book8gui_rect *b,
    const char *text)
{
    ui_sunken(p, b, UI_FACE);
    clipped(p, b->x + 4u, b->y + 4u, b->width - 8u, text, UI_TEXT, UI_FACE);
}

static struct book8gui_rect dialog_box(const struct book8gui_platform *p)
{
    uint32_t w = p->width < 560u ? p->width - 16u : 544u;
    return (struct book8gui_rect){(p->width - w) / 2u,
        (p->height - BOOK8GUI_TASKBAR_HEIGHT - 156u) / 2u, w, 156u};
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct utility_state *s = app->state;
    struct utility_layout v;
    layout(app, s, &v);
    s->first = ui_wheel_scroll(s->first, total_lines(s), v.rows, 0, 1u);
    if (s->hex) {
        uint32_t maximum = v.columns < 78u ? 78u - v.columns : 0u;
        if (s->horizontal > maximum) { s->horizontal = maximum; }
    }
    button(p, &v.open, "Open");
    if (s->hex) { button(p, &v.cluster, "FAT Cluster"); }
    else { button(p, &v.save, "Save"); button(p, &v.save_as, "Save As"); }
    ui_sunken(p, &v.frame, UI_FIELD);
    uint32_t pos = s->hex ? s->first * 16u : row_start(s, s->first);
    for (uint32_t row = 0u; row < v.rows && pos != NO_POSITION; ++row) {
        uint32_t y = v.frame.y + 3u + row * 16u;
        char text[128];
        if (s->hex) {
            if (pos >= s->length) { break; }
            char hex[79];
            book8_hex_format(pos, s->data + pos, s->length - pos, hex);
            clipped(p, v.text_x, y, v.columns * 8u, hex + s->horizontal, UI_TEXT, UI_FIELD);
            pos += 16u;
        } else {
            char number[16];
            snprintf(number, sizeof(number), "%6u", s->first + row + 1u);
            p->fill_rect(v.frame.x + 2u, y, 54u, 16u, UI_FACE);
            p->draw_text(v.frame.x + 3u, y, number, UI_MUTED, UI_FACE);
            for (uint32_t i = 0u; i < v.columns; ++i) { text[i] = ' '; }
            text[v.columns] = '\0';
            uint32_t end = line_end(s, pos), col = 0u;
            for (uint32_t i = pos; i < end; ++i) {
                uint32_t cells = s->data[i] == '\r' ? 0u : s->data[i] == '\t' ? 4u - col % 4u : 1u;
                for (uint32_t k = 0u; k < cells; ++k) {
                    if (col + k >= s->horizontal && col + k - s->horizontal < v.columns) {
                        uint8_t ch = s->data[i];
                        text[col + k - s->horizontal] = ch == '\t' ? ' '
                            : ch >= 32u && ch <= 126u ? (char)ch : '.';
                    }
                }
                col += cells;
            }
            p->draw_text(v.text_x, y, text, UI_TEXT, UI_FIELD);
            if (s->cursor >= pos && s->cursor <= end) {
                uint32_t caret = column_at(s, pos, s->cursor);
                if (caret >= s->horizontal && caret - s->horizontal < v.columns) {
                    p->fill_rect(v.text_x + (caret - s->horizontal) * 8u, y, 2u, 16u, UI_TEXT);
                }
            }
            pos = end < s->length ? end + 1u : NO_POSITION;
        }
    }
    ui_scrollbar(p, &v.track, total_lines(s), v.rows, s->first);
    ui_sunken(p, &v.status, UI_FACE);
    clipped(p, v.status.x + 4u, v.status.y + 2u, v.status.width - 8u,
        s->message[0] != '\0' ? s->message : s->path[0] != '\0' ? s->path
            : s->hex ? "Read-only: Open file or FAT Cluster." : "Ctrl+S Save; Ctrl+Shift+S Save As; Ctrl+O Open.",
        UI_TEXT, UI_FACE);
    if (s->dialog != DIALOG_NONE) {
        struct book8gui_rect box = dialog_box(p);
        ui_sunken(p, &box, UI_FACE);
        const char *prompt = s->dialog == DIALOG_SAVE_AS ? "Save As: absolute path (parent must exist)"
            : s->dialog == DIALOG_CLUSTER ? "Read FAT cluster: drive:number (e.g. A:2)"
            : "Open: absolute file path";
        p->draw_text(box.x + 8u, box.y + 8u, prompt, UI_TEXT, UI_FACE);
        struct book8gui_rect field = {box.x + 8u, box.y + 36u, box.width - 16u, 24u};
        ui_sunken(p, &field, UI_FIELD);
        uint32_t length = 0u;
        while (s->field[length] != '\0') { ++length; }
        uint32_t cols = (field.width - 8u) / 8u;
        uint32_t first = length > cols ? length - cols : 0u;
        clipped(p, field.x + 4u, field.y + 4u, field.width - 8u,
            s->field + first, UI_TEXT, UI_FIELD);
        clipped(p, box.x + 8u, box.y + 72u, box.width - 16u, s->message, UI_TEXT, UI_FACE);
        struct book8gui_rect ok = {box.x + 8u, box.y + 120u, 64u, 24u};
        struct book8gui_rect cancel = {box.x + 80u, box.y + 120u, 80u, 24u};
        button(p, &ok, "OK");
        button(p, &cancel, "Cancel");
    }
}

static bool open_path(const struct book8gui_platform *p, struct WindowApp *app, const char *path)
{
    struct utility_state *s = app->state;
    uint32_t path_length = 0u;
    if (path == NULL) { notify(p, s, "Missing file path."); return false; }
    while (path_length < sizeof(s->path) && path[path_length] != '\0') { ++path_length; }
    if (path_length >= sizeof(s->path)) { notify(p, s, "File path is too long."); return false; }
    if (s->dirty) { notify(p, s, "Save changes before opening another file."); return false; }
    struct book8_file_info info;
    uint32_t limit = s->hex ? HEX_CAPACITY : CODE_CAPACITY;
    if (p->file_info == NULL || !p->file_info(path, &info) || info.directory || info.size > limit) {
        notify(p, s, "Cannot open file: unavailable, directory or buffer limit exceeded.");
        return false;
    }
    uint32_t size;
    if (p->read_file == NULL || !p->read_file(path, read_buffer, limit, &size) || size > limit) {
        notify(p, s, "File read failed.");
        return false;
    }
    if (!s->hex) {
        for (uint32_t i = 0u; i < size; ++i) {
            uint8_t ch = read_buffer[i];
            if (ch == 0u || (ch < 32u && ch != '\r' && ch != '\n' && ch != '\t')) {
                notify(p, s, "Binary input rejected: use Hex Viewer.");
                return false;
            }
            for (uint32_t i = 0u; i < size; ++i) { s->data[i] = read_buffer[i]; }
        }
    }
    s->length = size;
    s->cursor = s->first = s->horizontal = 0u;
    s->preferred_valid = false;
    char new_path[256];
    snprintf(new_path, sizeof(new_path), "%s", path);
    snprintf(s->path, sizeof(s->path), "%s", new_path);
    s->message[0] = '\0';
    title(app, s);
    book8gui_invalidate();
    return true;
}

static void show_dialog(struct WindowApp *app, struct utility_state *s, enum utility_dialog dialog)
{
    s->dialog = dialog;
    snprintf(s->field, sizeof(s->field), "%s", dialog == DIALOG_CLUSTER ? "A:2" : s->path);
    s->message[0] = '\0';
    book8gui_set_modal(app, true);
    book8gui_invalidate();
}

static bool save(const struct book8gui_platform *p, struct WindowApp *app, const char *path)
{
    struct utility_state *s = app->state;
    uint32_t length = 0u;
    while (path[length] != '\0' && length < sizeof(s->path)) { ++length; }
    if (length < 4u || length >= sizeof(s->path) || path[1] != ':'
        || (path[2] != '\\' && path[2] != '/')) {
        notify(p, s, "Save requires an absolute drive path, e.g. A:\\SRC\\TEST.C.");
        return false;
    }
    char new_path[256];
    snprintf(new_path, sizeof(new_path), "%s", path);
    if (p->write_file == NULL || !p->write_file(new_path, s->data, s->length)) {
        notify(p, s, p->last_error_text != NULL ? p->last_error_text() : "Save failed; buffer retained.");
        return false;
    }
    snprintf(s->path, sizeof(s->path), "%s", new_path);
    s->dirty = false;
    title(app, s);
    notify(p, s, "Code Editor: file saved.");
    return true;
}

static void dialog_accept(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct utility_state *s = app->state;
    bool ok = false;
    if (s->dialog == DIALOG_OPEN) { ok = open_path(p, app, s->field); }
    else if (s->dialog == DIALOG_SAVE_AS) { ok = save(p, app, s->field); }
    else {
        uint32_t cluster = 0u, i = 2u;
        bool valid = s->field[0] >= 'A' && s->field[0] <= 'Z' && s->field[1] == ':'
            && s->field[2] >= '0' && s->field[2] <= '9';
        while (valid && s->field[i] != '\0') {
            char ch = s->field[i++];
            if (ch < '0' || ch > '9' || cluster > (UINT32_MAX - (uint32_t)(ch - '0')) / 10u) {
                valid = false;
            } else { cluster = cluster * 10u + (uint32_t)(ch - '0'); }
        }
        uint32_t size;
        if (valid && p->read_cluster != NULL
            && p->read_cluster(s->field[0], cluster, read_buffer, HEX_CAPACITY, &size)
            && size <= HEX_CAPACITY) {
            for (uint32_t j = 0u; j < size; ++j) { s->data[j] = read_buffer[j]; }
            s->length = size;
            s->first = s->horizontal = 0u;
            snprintf(s->path, sizeof(s->path), "FAT %c: cluster %u", s->field[0], cluster);
            s->message[0] = '\0';
            ok = true;
        } else { notify(p, s, "Cluster read failed: expected mounted FAT drive and valid data cluster."); }
    }
    if (ok) { s->dialog = DIALOG_NONE; book8gui_set_modal(app, false); }
}

static bool event(const struct book8gui_platform *p, struct WindowApp *app,
    const struct book8gui_event *e)
{
    struct utility_state *s = app->state;
    struct utility_layout v;
    layout(app, s, &v);
    bool down = e->type == BOOK8GUI_EVENT_MOUSE && (e->mouse_buttons & 1u) != 0u;
    bool pressed = down && (s->previous_buttons & 1u) == 0u;
    if (e->type == BOOK8GUI_EVENT_MOUSE) {
        s->previous_buttons = e->mouse_buttons;
        if (!down) { s->dragging = false; }
    }
    uint32_t key = e->key & BOOK8GUI_KEY_MASK;
    if ((e->key & BOOK8GUI_KEY_CTRL) != 0u && key >= 'A' && key <= 'Z') {
        key += 'a' - 'A';
    }
    if (s->dialog != DIALOG_NONE) {
        if (e->type == BOOK8GUI_EVENT_KEY) {
            uint32_t n = 0u;
            while (s->field[n] != '\0') { ++n; }
            if (key == BOOK8GUI_KEY_ESCAPE) { s->dialog = DIALOG_NONE; book8gui_set_modal(app, false); }
            else if (key == BOOK8GUI_KEY_ENTER) { dialog_accept(p, app); }
            else if (key == BOOK8GUI_KEY_BACKSPACE && n != 0u) { s->field[n - 1u] = '\0'; }
            else if (key >= 32u && key <= 126u && (e->key & BOOK8GUI_KEY_CTRL) == 0u) {
                if (n + 1u < sizeof(s->field)) { s->field[n] = (char)key; s->field[n + 1u] = '\0'; }
                else { notify(p, s, "Path input is full."); }
            }
        } else if (pressed) {
            struct book8gui_rect box = dialog_box(p);
            struct book8gui_rect ok = {box.x + 8u, box.y + 120u, 64u, 24u};
            struct book8gui_rect cancel = {box.x + 80u, box.y + 120u, 80u, 24u};
            if (ui_contains(&ok, e->mouse_x, e->mouse_y)) { dialog_accept(p, app); }
            if (ui_contains(&cancel, e->mouse_x, e->mouse_y)) {
                s->dialog = DIALOG_NONE; book8gui_set_modal(app, false);
            }
        }
        book8gui_invalidate();
        return true;
    }
    if (e->type == BOOK8GUI_EVENT_MOUSE) {
        if (ui_contains(&v.frame, e->mouse_x, e->mouse_y)) {
            s->first = ui_wheel_scroll_delta(s->first, total_lines(s), v.rows,
                e->wheel_delta, 3u, &s->wheel_remainder);
            if (pressed && ui_contains(&v.track, e->mouse_x, e->mouse_y)) {
                s->dragging = total_lines(s) > v.rows;
                s->scroll_grab = ui_scrollbar_grab(&v.track, total_lines(s),
                    v.rows, s->first, e->mouse_y);
            }
            else if (pressed && !s->hex && e->mouse_x >= (int32_t)v.text_x
                && e->mouse_y >= (int32_t)v.frame.y + 3) {
                uint32_t row = ((uint32_t)e->mouse_y - v.frame.y - 3u) / 16u;
                uint32_t pos = row_start(s, s->first + row);
                if (row < v.rows && pos != NO_POSITION) {
                    s->cursor = at_column(s, pos, s->horizontal + ((uint32_t)e->mouse_x - v.text_x) / 8u);
                    s->preferred_valid = false;
                }
            }
        }
        if (down && s->dragging) {
            s->first = ui_scrollbar_drag_position(&v.track, total_lines(s), v.rows,
                e->mouse_y, s->scroll_grab);
        }
        if (pressed && ui_contains(&v.open, e->mouse_x, e->mouse_y)) { show_dialog(app, s, DIALOG_OPEN); }
        else if (pressed && s->hex && ui_contains(&v.cluster, e->mouse_x, e->mouse_y)) {
            show_dialog(app, s, DIALOG_CLUSTER);
        } else if (pressed && !s->hex && ui_contains(&v.save_as, e->mouse_x, e->mouse_y)) {
            show_dialog(app, s, DIALOG_SAVE_AS);
        } else if (pressed && !s->hex && ui_contains(&v.save, e->mouse_x, e->mouse_y)) {
            if (s->path[0] == '\0') { show_dialog(app, s, DIALOG_SAVE_AS); }
            else { (void)save(p, app, s->path); }
        }
    } else if ((e->key & BOOK8GUI_KEY_CTRL) != 0u) {
        if (key == 'o') { show_dialog(app, s, DIALOG_OPEN); }
        else if (key == 's' && !s->hex) {
            if ((e->key & BOOK8GUI_KEY_SHIFT) != 0u || s->path[0] == '\0') {
                show_dialog(app, s, DIALOG_SAVE_AS);
            } else { (void)save(p, app, s->path); }
        } else { return false; }
    } else if (s->hex) {
        int32_t wheel = key == BOOK8GUI_KEY_UP ? 1 : key == BOOK8GUI_KEY_DOWN ? -1
            : key == BOOK8GUI_KEY_PAGE_UP ? (int32_t)v.rows
            : key == BOOK8GUI_KEY_PAGE_DOWN ? -(int32_t)v.rows : 0;
        if (key == BOOK8GUI_KEY_LEFT || key == BOOK8GUI_KEY_RIGHT) {
            uint32_t maximum = v.columns < 78u ? 78u - v.columns : 0u;
            if (key == BOOK8GUI_KEY_LEFT && s->horizontal != 0u) { --s->horizontal; }
            if (key == BOOK8GUI_KEY_RIGHT && s->horizontal < maximum) { ++s->horizontal; }
        }
        else if (key == BOOK8GUI_KEY_HOME) { s->first = 0u; }
        else if (key == BOOK8GUI_KEY_END) { s->first = total_lines(s); }
        else if (wheel != 0) { s->first = ui_wheel_scroll(s->first, total_lines(s), v.rows, wheel, 1u); }
        else { return false; }
    } else {
        uint32_t old = s->cursor;
        bool changed = false;
        if (key == BOOK8GUI_KEY_LEFT && s->cursor != 0u) { --s->cursor; }
        else if (key == BOOK8GUI_KEY_RIGHT && s->cursor < s->length) { ++s->cursor; }
        else if (key == BOOK8GUI_KEY_HOME) { s->cursor = line_start(s, s->cursor); }
        else if (key == BOOK8GUI_KEY_END) { s->cursor = line_end(s, s->cursor); }
        else if (key == BOOK8GUI_KEY_UP || key == BOOK8GUI_KEY_DOWN
            || key == BOOK8GUI_KEY_PAGE_UP || key == BOOK8GUI_KEY_PAGE_DOWN) {
            uint32_t start = line_start(s, s->cursor);
            if (!s->preferred_valid) {
                s->preferred_column = column_at(s, start, s->cursor);
                s->preferred_valid = true;
            }
            uint32_t steps = key == BOOK8GUI_KEY_PAGE_UP || key == BOOK8GUI_KEY_PAGE_DOWN ? v.rows : 1u;
            while (steps-- != 0u) {
                if (key == BOOK8GUI_KEY_UP || key == BOOK8GUI_KEY_PAGE_UP) {
                    if (start == 0u) { break; }
                    start = line_start(s, start - 1u);
                } else {
                    uint32_t end = line_end(s, start);
                    if (end == s->length) { break; }
                    start = end + 1u;
                }
            }
            s->cursor = at_column(s, start, s->preferred_column);
        } else if (key == BOOK8GUI_KEY_BACKSPACE || key == BOOK8GUI_KEY_DELETE) {
            uint32_t pos = s->cursor;
            if (key == BOOK8GUI_KEY_BACKSPACE && pos != 0u) { --pos; --s->cursor; }
            else if (key == BOOK8GUI_KEY_BACKSPACE) { pos = s->length; }
            if (pos < s->length) {
                for (uint32_t i = pos; i + 1u < s->length; ++i) { s->data[i] = s->data[i + 1u]; }
                --s->length; changed = true;
            }
        } else if (key == BOOK8GUI_KEY_ENTER || key == BOOK8GUI_KEY_TAB || (key >= 32u && key <= 126u)) {
            if (s->length == CODE_CAPACITY) { notify(p, s, "Text buffer full (64 KiB); save before continuing."); return true; }
            for (uint32_t i = s->length; i > s->cursor; --i) { s->data[i] = s->data[i - 1u]; }
            s->data[s->cursor++] = key == BOOK8GUI_KEY_ENTER ? '\n' : key == BOOK8GUI_KEY_TAB ? '\t' : (uint8_t)key;
            ++s->length; changed = true;
        } else { return false; }
        if (key != BOOK8GUI_KEY_UP && key != BOOK8GUI_KEY_DOWN
            && key != BOOK8GUI_KEY_PAGE_UP && key != BOOK8GUI_KEY_PAGE_DOWN) { s->preferred_valid = false; }
        if (changed) { s->dirty = true; s->message[0] = '\0'; title(app, s); }
        if (changed || old != s->cursor) { follow_cursor(s, &v); }
    }
    book8gui_invalidate();
    return true;
}

static void *create_state(bool hex)
{
    for (uint32_t i = 0u; i < BOOK8GUI_MAX_INSTANCES_PER_APP; ++i) {
        if (!states[i].used) {
            struct utility_state *s = &states[i];
            s->used = true; s->hex = hex; s->dirty = s->dragging = false;
            s->wheel_remainder = 0;
            s->previous_buttons = 0u;
            s->length = s->cursor = s->first = s->horizontal = 0u;
            s->preferred_valid = false; s->dialog = DIALOG_NONE;
            s->path[0] = s->field[0] = s->message[0] = '\0';
            return s;
        }
    }
    return NULL;
}
static void *hex_create(const struct book8gui_platform *p) { (void)p; return create_state(true); }
static void *code_create(const struct book8gui_platform *p) { (void)p; return create_state(false); }
static void destroy(void *state) { ((struct utility_state *)state)->used = false; }
static bool can_close(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct utility_state *s = app->state;
    if (s->dirty) {
        notify(p, s, "Unsaved text: save before closing (Task Manager can force termination).");
        return false;
    }
    return true;
}

const struct book8gui_app_class book8_hexview_class = {
    .name = "Hex Sector Viewer", .default_width = 680u, .default_height = 420u,
    .wants_all_keys = true, .state_size = sizeof(struct utility_state),
    .create = hex_create, .destroy = destroy, .render = render, .handle_event = event,
    .open_path = open_path
};
const struct book8gui_app_class book8_codeedit_class = {
    .name = "Code Editor", .default_width = 640u, .default_height = 420u,
    .wants_all_keys = true, .state_size = sizeof(struct utility_state),
    .create = code_create, .destroy = destroy, .render = render, .handle_event = event,
    .open_path = open_path, .can_close = can_close
};
