#include "book8_editor.h"
#include "book8_ui.h"

#include <stddef.h>
#include <stdint.h>

/*
 * NoteEditor: a Notepad-style editor. Text lives in one flat buffer; the
 * viewport is a list of "visual lines" (logical lines, optionally word
 * wrapped) identified by the offset where each one starts. Everything drawn
 * is clipped to the window by construction (columns/rows come from the
 * window bounds).
 */

#define ED_CAPACITY 65536u
#define ED_NAME 128u
#define ED_FIELD 120u
#define ED_MAX_COLS 256u
#define ED_MAX_FILES 48u
#define ED_NOTE 80u
#define ED_NONE UINT32_MAX
#define ED_MENU_BAR 20u
#define ED_STATUS_BAR 20u
#define ED_SCROLL_W 12u
#define ED_PAD 3u
#define ED_ITEM_H 18u
#define ED_SEP_H 8u
#define ED_ZOOM_MAX 3u
#define ED_MSG_LINES 10u

#define C_FACE 0xc0c0c0u
#define C_LIGHT 0xffffffu
#define C_SHADOW 0x808080u
#define C_EDGE 0x000000u
#define C_FIELD 0xffffffu
#define C_TEXT 0x000000u
#define C_SEL 0x000080u
#define C_SEL_TEXT 0xffffffu
#define C_DISABLED 0x808080u

enum edit_kind {
    EDIT_NONE,
    EDIT_TYPING,
    EDIT_DELETING,
    EDIT_OTHER
};

enum pending {
    PEND_NONE,
    PEND_NEW,
    PEND_OPEN,
    PEND_EXIT
};

enum dialog_kind {
    DLG_NONE,
    DLG_OPEN,
    DLG_SAVE_AS,
    DLG_FIND,
    DLG_REPLACE,
    DLG_GOTO,
    DLG_FONT,
    DLG_CONFIRM,
    DLG_MESSAGE
};

enum action {
    ACT_NONE,
    ACT_NEW,
    ACT_NEW_WINDOW,
    ACT_OPEN,
    ACT_SAVE,
    ACT_SAVE_AS,
    ACT_EXIT,
    ACT_UNDO,
    ACT_CUT,
    ACT_COPY,
    ACT_PASTE,
    ACT_DELETE,
    ACT_FIND,
    ACT_FIND_NEXT,
    ACT_FIND_PREV,
    ACT_REPLACE,
    ACT_GOTO,
    ACT_SELECT_ALL,
    ACT_TIME_DATE,
    ACT_WORD_WRAP,
    ACT_FONT,
    ACT_SUB_ZOOM,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_ZOOM_RESET,
    ACT_STATUS_BAR,
    ACT_HELP,
    ACT_FEEDBACK,
    ACT_ABOUT
};

enum button_id {
    BTN_OK = 1,
    BTN_CANCEL,
    BTN_FIND_NEXT,
    BTN_REPLACE,
    BTN_REPLACE_ALL,
    BTN_SAVE,
    BTN_DONT_SAVE
};

struct menu_entry {
    const char *label;
    const char *shortcut;
    uint8_t action;
};

struct menu_def {
    const char *title;
    const struct menu_entry *entries;
    uint8_t count;
};

#define ENTRY_COUNT(table) ((uint8_t)(sizeof(table) / sizeof((table)[0])))

static const struct menu_entry file_menu[] = {
    {"New", "Ctrl+N", ACT_NEW},
    {"New Window", "Ctrl+Shift+N", ACT_NEW_WINDOW},
    {"Open...", "Ctrl+O", ACT_OPEN},
    {"Save", "Ctrl+S", ACT_SAVE},
    {"Save As...", "Ctrl+Shift+S", ACT_SAVE_AS},
    {NULL, NULL, ACT_NONE},
    {"Exit", NULL, ACT_EXIT}
};

static const struct menu_entry edit_menu[] = {
    {"Undo", "Ctrl+Z", ACT_UNDO},
    {NULL, NULL, ACT_NONE},
    {"Cut", "Ctrl+X", ACT_CUT},
    {"Copy", "Ctrl+C", ACT_COPY},
    {"Paste", "Ctrl+V", ACT_PASTE},
    {"Delete", "Del", ACT_DELETE},
    {NULL, NULL, ACT_NONE},
    {"Find...", "Ctrl+F", ACT_FIND},
    {"Find Next", "F3", ACT_FIND_NEXT},
    {"Find Previous", "Shift+F3", ACT_FIND_PREV},
    {"Replace...", "Ctrl+H", ACT_REPLACE},
    {"Go To...", "Ctrl+G", ACT_GOTO},
    {NULL, NULL, ACT_NONE},
    {"Select All", "Ctrl+A", ACT_SELECT_ALL},
    {"Time/Date", "F5", ACT_TIME_DATE}
};

static const struct menu_entry format_menu[] = {
    {"Word Wrap", NULL, ACT_WORD_WRAP},
    {"Font...", NULL, ACT_FONT}
};

static const struct menu_entry view_menu[] = {
    {"Zoom", ">", ACT_SUB_ZOOM},
    {"Status Bar", NULL, ACT_STATUS_BAR}
};

static const struct menu_entry help_menu[] = {
    {"View Help", "F1", ACT_HELP},
    {"Send Feedback", NULL, ACT_FEEDBACK},
    {NULL, NULL, ACT_NONE},
    {"About NoteEditor", NULL, ACT_ABOUT}
};

static const struct menu_entry zoom_menu[] = {
    {"Zoom In", "Ctrl+Plus", ACT_ZOOM_IN},
    {"Zoom Out", "Ctrl+Minus", ACT_ZOOM_OUT},
    {"Restore Default Zoom", "Ctrl+0", ACT_ZOOM_RESET}
};

static const struct menu_def menus[] = {
    {"File", file_menu, ENTRY_COUNT(file_menu)},
    {"Edit", edit_menu, ENTRY_COUNT(edit_menu)},
    {"Format", format_menu, ENTRY_COUNT(format_menu)},
    {"View", view_menu, ENTRY_COUNT(view_menu)},
    {"Help", help_menu, ENTRY_COUNT(help_menu)}
};

#define MENU_COUNT ((int32_t)(sizeof(menus) / sizeof(menus[0])))

static const struct menu_def zoom_def = {
    "Zoom", zoom_menu, ENTRY_COUNT(zoom_menu)
};

struct editor_state {
    bool in_use;
    char text[ED_CAPACITY];
    uint32_t length;
    uint32_t cursor;
    uint32_t anchor;
    uint32_t top;
    uint32_t hscroll;
    uint32_t want_col;
    bool want_valid;
    bool follow;

    char undo_text[ED_CAPACITY];
    uint32_t undo_length;
    uint32_t undo_cursor;
    uint32_t undo_anchor;
    bool undo_valid;
    enum edit_kind last_edit;

    bool dirty;
    bool has_path;
    bool binary_document;
    char path[ED_NAME];
    char note[ED_NOTE];

    bool word_wrap;
    bool status_bar;
    bool bold;
    uint32_t zoom;

    int32_t menu_open;
    int32_t menu_sel;
    bool sub_open;
    int32_t sub_sel;

    uint8_t previous_buttons;
    bool selecting;
    bool scroll_drag;
    int32_t wheel_remainder[2];

    enum dialog_kind dialog;
    char field[2][ED_FIELD];
    uint32_t active_field;
    uint32_t choice;
    bool match_case;
    char find[ED_FIELD];
    char replace[ED_FIELD];
    const char *message_title;
    const char *message_lines[ED_MSG_LINES];
    uint32_t message_count;
    char message_dynamic[ED_NAME + 48u];
    enum pending confirm_pending;
    enum pending after_save;

    char drive;
    char files[ED_MAX_FILES][ED_NAME];
    uint32_t file_count;
    uint32_t list_scroll;
    uint32_t list_sel;
    bool list_has_sel;
    uint64_t last_click_ticks;
    uint32_t last_click_index;
    bool last_click_valid;
};

static struct editor_state editor_pool[BOOK8GUI_MAX_INSTANCES_PER_APP];
static char clipboard[ED_CAPACITY];
static uint32_t clipboard_length;
static char io_buffer[ED_CAPACITY];

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t text_length(const char *text)
{
    uint32_t length = 0u;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void copy_text(char *destination, uint32_t capacity,
    const char *source)
{
    if (capacity == 0u) {
        return;
    }
    uint32_t i = 0u;
    while (i + 1u < capacity && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static char lower_letter(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static bool is_word_char(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9') || value == '_';
}

static uint32_t append_number(char *buffer, uint32_t offset, uint32_t value)
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

static uint32_t append_text(char *buffer, uint32_t offset, const char *text)
{
    while (*text != '\0') {
        buffer[offset++] = *text++;
    }
    return offset;
}

static bool in_rect(const struct book8gui_rect *rect, int32_t x, int32_t y)
{
    return x >= (int32_t)rect->x && y >= (int32_t)rect->y
        && x < (int32_t)(rect->x + rect->width)
        && y < (int32_t)(rect->y + rect->height);
}

static void set_note(struct editor_state *s, const char *note)
{
    copy_text(s->note, sizeof(s->note), note);
    book8gui_invalidate();
}

static void diagnostic_event(const struct book8gui_platform *platform,
    const char *verb, const char *path, uint32_t bytes)
{
    if (platform->diagnostic == NULL) {
        return;
    }
    char message[ED_NAME + 64u];
    uint32_t offset = append_text(message, 0u, "BOOK8-NOTEEDITOR: ");
    offset = append_text(message, offset, verb);
    offset = append_text(message, offset, " path=");
    for (uint32_t i = 0u; path[i] != '\0' && offset + 24u < sizeof(message);
        ++i) {
        message[offset++] = path[i];
    }
    offset = append_text(message, offset, " bytes=");
    offset = append_number(message, offset, bytes);
    message[offset++] = '\n';
    message[offset] = '\0';
    platform->diagnostic(message);
}

/* Draws at most max_px worth of characters so nothing leaves its area. */
static void put_text(const struct book8gui_platform *platform, uint32_t x,
    uint32_t y, const char *text, uint32_t foreground, uint32_t background,
    uint32_t max_px)
{
    char buffer[ED_MAX_COLS];
    uint32_t limit = max_px / 8u;
    uint32_t count = 0u;
    if (limit >= sizeof(buffer)) {
        limit = sizeof(buffer) - 1u;
    }
    while (text[count] != '\0' && count < limit) {
        buffer[count] = text[count];
        ++count;
    }
    buffer[count] = '\0';
    if (count != 0u) {
        platform->draw_text(x, y, buffer, foreground, background);
    }
}

static void draw_panel(const struct book8gui_platform *platform, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height)
{
    if (width < 4u || height < 4u) {
        return;
    }
    platform->fill_rect(x, y, width, height, C_EDGE);
    platform->fill_rect(x + 1u, y + 1u, width - 2u, height - 2u, C_FACE);
    platform->fill_rect(x + 1u, y + 1u, width - 3u, 1u, C_LIGHT);
    platform->fill_rect(x + 1u, y + 1u, 1u, height - 3u, C_LIGHT);
    platform->fill_rect(x + 2u, y + height - 2u, width - 3u, 1u, C_SHADOW);
    platform->fill_rect(x + width - 2u, y + 2u, 1u, height - 3u, C_SHADOW);
}

static void draw_sunken(const struct book8gui_platform *platform, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height, uint32_t face)
{
    if (width < 4u || height < 4u) {
        return;
    }
    platform->fill_rect(x, y, width, height, C_SHADOW);
    platform->fill_rect(x + 1u, y + 1u, width - 2u, height - 2u, face);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

struct view {
    uint32_t menu_x;
    uint32_t menu_y;
    uint32_t menu_w;
    uint32_t frame_x;
    uint32_t frame_y;
    uint32_t frame_w;
    uint32_t frame_h;
    uint32_t status_y;
    uint32_t scale;
    uint32_t cell_w;
    uint32_t cell_h;
    uint32_t cols;
    uint32_t rows;
};

static void compute_view(const struct book8gui_platform *platform,
    const struct WindowApp *app, const struct editor_state *s,
    struct view *view)
{
    uint32_t bottom = app->bounds.y + app->bounds.height - 4u;
    view->menu_x = app->bounds.x + 4u;
    view->menu_y = app->bounds.y + 30u;
    view->menu_w = app->bounds.width > 8u ? app->bounds.width - 8u : 0u;
    view->frame_x = view->menu_x;
    view->frame_y = view->menu_y + ED_MENU_BAR;
    view->frame_w = view->menu_w;
    view->status_y = s->status_bar ? bottom - ED_STATUS_BAR : bottom;
    view->frame_h = view->status_y > view->frame_y
        ? view->status_y - view->frame_y : 0u;
    view->scale = platform->draw_text_scaled != NULL ? s->zoom : 1u;
    view->cell_w = 8u * view->scale;
    view->cell_h = 16u * view->scale;
    uint32_t inner_w = view->frame_w > ED_SCROLL_W + 2u * ED_PAD
        ? view->frame_w - ED_SCROLL_W - 2u * ED_PAD : 0u;
    view->cols = inner_w / view->cell_w;
    if (view->cols == 0u) {
        view->cols = 1u;
    }
    if (view->cols > ED_MAX_COLS - 1u) {
        view->cols = ED_MAX_COLS - 1u;
    }
    view->rows = view->frame_h > 2u * ED_PAD
        ? (view->frame_h - 2u * ED_PAD) / view->cell_h : 0u;
}

/* ------------------------------------------------------------------ */
/* Text geometry                                                       */
/* ------------------------------------------------------------------ */

static uint32_t cell_width(char value)
{
    return value == '\t' ? 4u : 1u;
}

static uint32_t line_start(const struct editor_state *s, uint32_t position)
{
    while (position > 0u && s->text[position - 1u] != '\n') {
        --position;
    }
    return position;
}

static uint32_t line_end(const struct editor_state *s, uint32_t position)
{
    while (position < s->length && s->text[position] != '\n') {
        ++position;
    }
    return position;
}

/* End (exclusive) of the visual line that begins at start. */
static uint32_t break_after(const struct editor_state *s, uint32_t start,
    uint32_t end, uint32_t cols)
{
    if (!s->word_wrap || cols == 0u) {
        return end;
    }
    uint32_t width = 0u;
    uint32_t last_space = ED_NONE;
    for (uint32_t i = start; i < end; ++i) {
        uint32_t cell = cell_width(s->text[i]);
        if (width + cell > cols && i > start) {
            return last_space != ED_NONE ? last_space + 1u : i;
        }
        width += cell;
        if (s->text[i] == ' ') {
            last_space = i;
        }
    }
    return end;
}

static uint32_t visual_start(const struct editor_state *s, uint32_t position,
    uint32_t cols)
{
    uint32_t start = line_start(s, position);
    uint32_t end = line_end(s, start);
    for (;;) {
        uint32_t stop = break_after(s, start, end, cols);
        if (stop >= end || position < stop) {
            return start;
        }
        start = stop;
    }
}

static uint32_t visual_end(const struct editor_state *s, uint32_t start,
    uint32_t cols)
{
    return break_after(s, start, line_end(s, start), cols);
}

static uint32_t next_visual_start(const struct editor_state *s,
    uint32_t start, uint32_t cols)
{
    uint32_t end = line_end(s, start);
    uint32_t stop = break_after(s, start, end, cols);
    if (stop < end) {
        return stop;
    }
    return end < s->length ? end + 1u : ED_NONE;
}

static uint32_t prev_visual_start(const struct editor_state *s,
    uint32_t start, uint32_t cols)
{
    return start == 0u ? ED_NONE : visual_start(s, start - 1u, cols);
}

static uint32_t column_of(const struct editor_state *s, uint32_t start,
    uint32_t position)
{
    uint32_t column = 0u;
    for (uint32_t i = start; i < position && i < s->length; ++i) {
        column += cell_width(s->text[i]);
    }
    return column;
}

static uint32_t offset_at_column(const struct editor_state *s, uint32_t start,
    uint32_t column, uint32_t cols)
{
    uint32_t end = line_end(s, start);
    uint32_t stop = break_after(s, start, end, cols);
    uint32_t limit = stop < end ? stop - 1u : stop;
    uint32_t i = start;
    uint32_t accumulated = 0u;
    while (i < limit) {
        uint32_t cell = cell_width(s->text[i]);
        if (accumulated + cell > column) {
            break;
        }
        accumulated += cell;
        ++i;
    }
    return i;
}

static bool has_selection(const struct editor_state *s)
{
    return s->cursor != s->anchor;
}

static uint32_t selection_start(const struct editor_state *s)
{
    return s->cursor < s->anchor ? s->cursor : s->anchor;
}

static uint32_t selection_end(const struct editor_state *s)
{
    return s->cursor < s->anchor ? s->anchor : s->cursor;
}

static void scroll_lines(struct editor_state *s, uint32_t cols,
    int32_t lines)
{
    while (lines < 0) {
        uint32_t previous = prev_visual_start(s, s->top, cols);
        if (previous == ED_NONE) {
            break;
        }
        s->top = previous;
        ++lines;
    }
    while (lines > 0) {
        uint32_t next = next_visual_start(s, s->top, cols);
        if (next == ED_NONE) {
            break;
        }
        s->top = next;
        --lines;
    }
}

/* Re-validates top/hscroll after edits and, when asked, follows the caret. */
static void settle_view(struct editor_state *s, const struct view *view)
{
    if (s->top > s->length) {
        s->top = s->length;
    }
    s->top = visual_start(s, s->top, view->cols);
    if (s->cursor > s->length) {
        s->cursor = s->length;
    }
    if (s->anchor > s->length) {
        s->anchor = s->length;
    }
    if (!s->word_wrap) {
        if (!s->follow) {
            return;
        }
    } else {
        s->hscroll = 0u;
    }
    if (!s->follow) {
        return;
    }
    s->follow = false;
    uint32_t caret_line = visual_start(s, s->cursor, view->cols);
    if (caret_line < s->top) {
        s->top = caret_line;
    } else if (view->rows != 0u) {
        uint32_t rows_down = 0u;
        uint32_t line = s->top;
        while (line < caret_line && line != ED_NONE) {
            line = next_visual_start(s, line, view->cols);
            ++rows_down;
        }
        while (rows_down >= view->rows) {
            uint32_t next = next_visual_start(s, s->top, view->cols);
            if (next == ED_NONE) {
                break;
            }
            s->top = next;
            --rows_down;
        }
    }
    if (!s->word_wrap) {
        uint32_t column = column_of(s, line_start(s, s->cursor), s->cursor);
        if (column < s->hscroll) {
            s->hscroll = column;
        } else if (column >= s->hscroll + view->cols) {
            s->hscroll = column - view->cols + 1u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Editing primitives                                                  */
/* ------------------------------------------------------------------ */

static void take_snapshot(struct editor_state *s)
{
    for (uint32_t i = 0u; i < s->length; ++i) {
        s->undo_text[i] = s->text[i];
    }
    s->undo_length = s->length;
    s->undo_cursor = s->cursor;
    s->undo_anchor = s->anchor;
    s->undo_valid = true;
}

static bool edit_replace(struct editor_state *s, uint32_t start,
    uint32_t end, const char *data, uint32_t count, enum edit_kind kind)
{
    uint32_t removed = end - start;
    if (s->length - removed + count >= ED_CAPACITY) {
        set_note(s, "Document is full (64 KB limit).");
        return false;
    }
    if (kind == EDIT_OTHER || !s->undo_valid || s->last_edit != kind) {
        take_snapshot(s);
    }
    s->last_edit = kind;
    uint32_t tail = s->length - end;
    if (count > removed) {
        for (uint32_t i = tail; i > 0u; --i) {
            s->text[start + count + i - 1u] = s->text[end + i - 1u];
        }
    } else if (count < removed) {
        for (uint32_t i = 0u; i < tail; ++i) {
            s->text[start + count + i] = s->text[end + i];
        }
    }
    for (uint32_t i = 0u; i < count; ++i) {
        s->text[start + i] = data[i];
    }
    s->length = s->length - removed + count;
    s->cursor = start + count;
    s->anchor = s->cursor;
    s->dirty = true;
    s->want_valid = false;
    s->follow = true;
    s->note[0] = '\0';
    book8gui_invalidate();
    return true;
}

static void move_cursor(struct editor_state *s, uint32_t position,
    bool extend)
{
    s->cursor = position > s->length ? s->length : position;
    if (!extend) {
        s->anchor = s->cursor;
    }
    s->follow = true;
    s->last_edit = EDIT_NONE;
    s->note[0] = '\0';
    book8gui_invalidate();
}

static void insert_char(struct editor_state *s, char value)
{
    if (has_selection(s)) {
        (void)edit_replace(s, selection_start(s), selection_end(s), &value,
            1u, EDIT_OTHER);
    } else {
        (void)edit_replace(s, s->cursor, s->cursor, &value, 1u,
            EDIT_TYPING);
    }
}

static void delete_backward(struct editor_state *s)
{
    if (has_selection(s)) {
        (void)edit_replace(s, selection_start(s), selection_end(s), NULL,
            0u, EDIT_OTHER);
    } else if (s->cursor > 0u) {
        (void)edit_replace(s, s->cursor - 1u, s->cursor, NULL, 0u,
            EDIT_DELETING);
    }
}

static void delete_forward(struct editor_state *s)
{
    if (has_selection(s)) {
        (void)edit_replace(s, selection_start(s), selection_end(s), NULL,
            0u, EDIT_OTHER);
    } else if (s->cursor < s->length) {
        (void)edit_replace(s, s->cursor, s->cursor + 1u, NULL, 0u,
            EDIT_DELETING);
    }
}

static void undo_edit(struct editor_state *s)
{
    if (!s->undo_valid) {
        return;
    }
    uint32_t count = s->length > s->undo_length ? s->length : s->undo_length;
    for (uint32_t i = 0u; i < count; ++i) {
        char swap = s->text[i];
        s->text[i] = s->undo_text[i];
        s->undo_text[i] = swap;
    }
    uint32_t value = s->length;
    s->length = s->undo_length;
    s->undo_length = value;
    value = s->cursor;
    s->cursor = s->undo_cursor;
    s->undo_cursor = value;
    value = s->anchor;
    s->anchor = s->undo_anchor;
    s->undo_anchor = value;
    s->dirty = true;
    s->last_edit = EDIT_NONE;
    s->want_valid = false;
    s->follow = true;
    book8gui_invalidate();
}

static void copy_selection(struct editor_state *s)
{
    if (!has_selection(s)) {
        return;
    }
    uint32_t start = selection_start(s);
    clipboard_length = selection_end(s) - start;
    for (uint32_t i = 0u; i < clipboard_length; ++i) {
        clipboard[i] = s->text[start + i];
    }
}

static void paste_clipboard(struct editor_state *s)
{
    if (clipboard_length == 0u) {
        return;
    }
    (void)edit_replace(s, selection_start(s), selection_end(s), clipboard,
        clipboard_length, EDIT_OTHER);
}

static void move_vertical(struct editor_state *s, const struct view *view,
    int32_t lines, bool extend)
{
    uint32_t start = visual_start(s, s->cursor, view->cols);
    uint32_t column = s->want_valid ? s->want_col
        : column_of(s, start, s->cursor);
    uint32_t target = start;
    while (lines < 0) {
        uint32_t previous = prev_visual_start(s, target, view->cols);
        if (previous == ED_NONE) {
            break;
        }
        target = previous;
        ++lines;
    }
    while (lines > 0) {
        uint32_t next = next_visual_start(s, target, view->cols);
        if (next == ED_NONE) {
            break;
        }
        target = next;
        --lines;
    }
    uint32_t position;
    if (target == start && lines > 0) {
        position = s->length;
    } else {
        position = offset_at_column(s, target, column, view->cols);
    }
    move_cursor(s, position, extend);
    s->want_col = column;
    s->want_valid = true;
}

static uint32_t word_left(const struct editor_state *s, uint32_t position)
{
    while (position > 0u && !is_word_char(s->text[position - 1u])) {
        --position;
    }
    while (position > 0u && is_word_char(s->text[position - 1u])) {
        --position;
    }
    return position;
}

static uint32_t word_right(const struct editor_state *s, uint32_t position)
{
    while (position < s->length && !is_word_char(s->text[position])) {
        ++position;
    }
    while (position < s->length && is_word_char(s->text[position])) {
        ++position;
    }
    return position;
}

/* ------------------------------------------------------------------ */
/* Dialog state helpers                                                */
/* ------------------------------------------------------------------ */

static void close_dialog(struct editor_state *s)
{
    s->dialog = DLG_NONE;
    book8gui_invalidate();
}

static void show_message(struct editor_state *s, const char *title,
    const char *first, const char *second)
{
    s->dialog = DLG_MESSAGE;
    s->message_title = title;
    s->message_count = 0u;
    if (first != NULL) {
        s->message_lines[s->message_count++] = first;
    }
    if (second != NULL) {
        copy_text(s->message_dynamic, sizeof(s->message_dynamic), second);
        s->message_lines[s->message_count++] = s->message_dynamic;
    }
    book8gui_invalidate();
}

static void show_static_message(struct editor_state *s, const char *title,
    const char *const *lines, uint32_t count)
{
    s->dialog = DLG_MESSAGE;
    s->message_title = title;
    s->message_count = count < ED_MSG_LINES ? count : ED_MSG_LINES;
    for (uint32_t i = 0u; i < s->message_count; ++i) {
        s->message_lines[i] = lines[i];
    }
    book8gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static void reset_document(struct editor_state *s)
{
    s->length = 0u;
    s->cursor = 0u;
    s->anchor = 0u;
    s->top = 0u;
    s->wheel_remainder[0] = s->wheel_remainder[1] = 0;
    s->hscroll = 0u;
    s->want_valid = false;
    s->follow = true;
    s->undo_valid = false;
    s->last_edit = EDIT_NONE;
    s->dirty = false;
    s->has_path = false;
    s->binary_document = false;
    s->path[0] = '\0';
    s->note[0] = '\0';
    book8gui_invalidate();
}

static bool save_to(const struct book8gui_platform *platform,
    const struct WindowApp *app, struct editor_state *s, const char *path)
{
    if (s->binary_document) {
        show_message(s, "Save", "Binary files cannot be saved by NoteEditor.",
            NULL);
        return false;
    }
    if (platform->write_file == NULL) {
        show_message(s, "Save", "File access is not available.", NULL);
        return false;
    }
    if (!platform->write_file(path, s->text, s->length)) {
        show_message(s, "Save", "Could not save the file:", path);
        return false;
    }
    book8gui_note_disk(app, 0u, s->length);
    copy_text(s->path, sizeof(s->path), path);
    s->has_path = true;
    s->dirty = false;
    set_note(s, "Saved.");
    diagnostic_event(platform, "saved", path, s->length);
    return true;
}

static bool open_path(const struct book8gui_platform *platform,
    const struct WindowApp *app, struct editor_state *s, const char *path)
{
    uint32_t size = 0u;
    if (platform->read_file == NULL) {
        show_message(s, "Open", "File access is not available.", NULL);
        return false;
    }
    if (!platform->read_file(path, io_buffer, ED_CAPACITY - 1u, &size)) {
        show_message(s, "Open",
            "Cannot open the file (missing, over 64 KB, or unreadable):",
            path);
        return false;
    }
    uint32_t count = 0u;
    bool binary = false;
    book8gui_note_disk(app, size, 0u);
    for (uint32_t i = 0u; i < size; ++i) {
        if (io_buffer[i] == 0) {
            binary = true;
            break;
        }
    }
    for (uint32_t i = 0u; i < size; ++i) {
        if (binary || io_buffer[i] != '\r') {
            s->text[count++] = io_buffer[i];
        }
    }
    reset_document(s);
    s->length = count;
    s->binary_document = binary;
    copy_text(s->path, sizeof(s->path), path);
    s->has_path = true;
    if (binary)
        set_note(s, "Binary file: saving disabled.");
    diagnostic_event(platform, "opened", path, count);
    return true;
}

bool book8_editor_open_path(const struct book8gui_platform *platform,
    struct WindowApp *app, const char *path)
{
    if (platform == NULL || app == NULL || app->cls != &book8_editor_class
        || app->state == NULL || path == NULL) {
        return false;
    }
    struct editor_state *state = app->state;
    if (text_length(path) >= sizeof(state->path)) {
        show_message(state, "Open", "File path is too long:", path);
        return false;
    }
    bool opened = open_path(platform, app, state, path);
    book8gui_invalidate();
    return opened;
}

struct list_context {
    struct editor_state *state;
};

static bool collect_file(const char *name, bool directory, uint32_t size,
    void *context)
{
    struct list_context *list = context;
    (void)size;
    if (directory || list->state->file_count >= ED_MAX_FILES
        || text_length(name) >= ED_NAME) {
        return true;
    }
    copy_text(list->state->files[list->state->file_count++], ED_NAME, name);
    return true;
}

static void refresh_file_list(const struct book8gui_platform *platform,
    struct editor_state *s)
{
    char root[4] = {s->drive, ':', '\\', '\0'};
    struct list_context context = {s};
    s->file_count = 0u;
    s->list_scroll = 0u;
    s->list_has_sel = false;
    s->last_click_valid = false;
    if (platform->list_directory != NULL) {
        (void)platform->list_directory(root, collect_file, &context);
    }
    book8gui_invalidate();
}

static void open_file_dialog(const struct book8gui_platform *platform,
    struct editor_state *s, bool save)
{
    s->dialog = save ? DLG_SAVE_AS : DLG_OPEN;
    s->active_field = 0u;
    s->field[0][0] = '\0';
    s->drive = 'A';
    if (s->has_path) {
        const char *name = s->path;
        if (name[0] != '\0' && name[1] == ':') {
            s->drive = name[0] >= 'a' && name[0] <= 'z'
                ? (char)(name[0] - 'a' + 'A') : name[0];
            name += 2;
        }
        while (*name == '\\' || *name == '/') {
            ++name;
        }
        if (save) {
            copy_text(s->field[0], ED_FIELD, name);
        }
    }
    refresh_file_list(platform, s);
}

static void perform_pending(const struct book8gui_platform *platform,
    const struct WindowApp *app, struct editor_state *s,
    enum pending pending)
{
    switch (pending) {
    case PEND_NEW:
        reset_document(s);
        break;
    case PEND_OPEN:
        open_file_dialog(platform, s, false);
        break;
    case PEND_EXIT:
        book8gui_request_close(app);
        break;
    default:
        break;
    }
}

/* Runs pending now, or after the user decides what to do with changes. */
static void guard_changes(const struct book8gui_platform *platform,
    const struct WindowApp *app, struct editor_state *s,
    enum pending pending)
{
    if (!s->dirty) {
        perform_pending(platform, app, s, pending);
        return;
    }
    s->dialog = DLG_CONFIRM;
    s->confirm_pending = pending;
    s->message_title = "NoteEditor";
    s->message_count = 2u;
    s->message_lines[0] = "Do you want to save changes to:";
    s->message_lines[1] = s->has_path ? s->path : "Untitled";
    book8gui_invalidate();
}

static void commit_file_dialog(const struct book8gui_platform *platform,
    const struct WindowApp *app, struct editor_state *s)
{
    char path[ED_FIELD + 4u];
    const char *name = s->field[0];
    uint32_t offset = 0u;
    if (name[0] == '\0') {
        return;
    }
    if (name[1] != ':') {
        path[offset++] = s->drive;
        path[offset++] = ':';
    }
    for (uint32_t i = 0u; name[i] != '\0'; ++i) {
        path[offset++] = name[i];
    }
    path[offset] = '\0';
    bool save = s->dialog == DLG_SAVE_AS;
    enum pending after = s->after_save;
    close_dialog(s);
    if (save) {
        s->after_save = PEND_NONE;
        if (save_to(platform, app, s, path) && after != PEND_NONE) {
            perform_pending(platform, app, s, after);
        }
    } else {
        (void)open_path(platform, app, s, path);
    }
}

static void save_document(const struct book8gui_platform *platform,
    const struct WindowApp *app, struct editor_state *s,
    enum pending after)
{
    if (s->has_path) {
        if (save_to(platform, app, s, s->path) && after != PEND_NONE) {
            perform_pending(platform, app, s, after);
        }
    } else {
        open_file_dialog(platform, s, true);
        s->after_save = after;
    }
}

/* ------------------------------------------------------------------ */
/* Find / replace / go to                                              */
/* ------------------------------------------------------------------ */

static bool matches_at(const struct editor_state *s, uint32_t position,
    const char *needle, uint32_t count)
{
    for (uint32_t i = 0u; i < count; ++i) {
        char left = s->text[position + i];
        char right = needle[i];
        if (!s->match_case) {
            left = lower_letter(left);
            right = lower_letter(right);
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

static void show_not_found(struct editor_state *s)
{
    char line[ED_FIELD + 16u];
    uint32_t offset = append_text(line, 0u, "Cannot find \"");
    for (uint32_t i = 0u; s->find[i] != '\0' && offset + 4u < sizeof(line);
        ++i) {
        line[offset++] = s->find[i];
    }
    line[offset++] = '"';
    line[offset] = '\0';
    show_message(s, "Find", line, NULL);
}

static bool find_text(struct editor_state *s, bool forward)
{
    uint32_t count = text_length(s->find);
    if (count == 0u || count > s->length) {
        show_not_found(s);
        return false;
    }
    uint32_t last = s->length - count;
    uint32_t found = ED_NONE;
    if (forward) {
        uint32_t from = selection_end(s);
        for (uint32_t i = from; i <= last && found == ED_NONE; ++i) {
            if (matches_at(s, i, s->find, count)) {
                found = i;
            }
        }
        for (uint32_t i = 0u; i < from && i <= last && found == ED_NONE;
            ++i) {
            if (matches_at(s, i, s->find, count)) {
                found = i;
            }
        }
    } else {
        uint32_t from = selection_start(s);
        for (uint32_t i = from; i > 0u && found == ED_NONE; --i) {
            if (i - 1u <= last && matches_at(s, i - 1u, s->find, count)) {
                found = i - 1u;
            }
        }
        for (uint32_t i = last + 1u; i > from && found == ED_NONE; --i) {
            if (matches_at(s, i - 1u, s->find, count)) {
                found = i - 1u;
            }
        }
    }
    if (found == ED_NONE) {
        show_not_found(s);
        return false;
    }
    s->anchor = found;
    move_cursor(s, found + count, true);
    return true;
}

static void replace_current(struct editor_state *s)
{
    uint32_t count = text_length(s->find);
    if (count != 0u && has_selection(s) && selection_end(s) - selection_start(s)
        == count && matches_at(s, selection_start(s), s->find, count)) {
        if (!edit_replace(s, selection_start(s), selection_end(s),
            s->replace, text_length(s->replace), EDIT_OTHER)) {
            return;
        }
    }
    (void)find_text(s, true);
}

static void replace_all(struct editor_state *s)
{
    uint32_t count = text_length(s->find);
    uint32_t replacement = text_length(s->replace);
    uint32_t replaced = 0u;
    uint32_t position = 0u;
    if (count == 0u) {
        show_not_found(s);
        return;
    }
    s->undo_valid = false;
    while (position + count <= s->length) {
        if (!matches_at(s, position, s->find, count)) {
            ++position;
            continue;
        }
        if (!edit_replace(s, position, position + count, s->replace,
            replacement, replaced == 0u ? EDIT_OTHER : EDIT_TYPING)) {
            break;
        }
        position += replacement;
        ++replaced;
    }
    if (replaced == 0u) {
        show_not_found(s);
    } else {
        char note[ED_NOTE];
        uint32_t offset = append_number(note, 0u, replaced);
        offset = append_text(note, offset, " occurrence(s) replaced.");
        note[offset] = '\0';
        set_note(s, note);
    }
}

static void go_to_line(struct editor_state *s)
{
    uint32_t line = 0u;
    for (uint32_t i = 0u; s->field[0][i] != '\0'; ++i) {
        if (line > 100000000u) {
            break;
        }
        line = line * 10u + (uint32_t)(s->field[0][i] - '0');
    }
    if (line == 0u) {
        line = 1u;
    }
    uint32_t position = 0u;
    uint32_t current = 1u;
    while (current < line) {
        uint32_t end = line_end(s, position);
        if (end >= s->length) {
            close_dialog(s);
            show_message(s, "Go To Line",
                "The line number is beyond the total", "number of lines.");
            return;
        }
        position = end + 1u;
        ++current;
    }
    close_dialog(s);
    move_cursor(s, position, false);
}

static void insert_time_date(const struct book8gui_platform *platform,
    struct editor_state *s)
{
    struct book8gui_time now;
    if (platform->read_time == NULL || !platform->read_time(&now)) {
        set_note(s, "The clock is unavailable.");
        return;
    }
    char text[40];
    uint32_t offset = 0u;
    uint32_t hour = now.hour % 12u == 0u ? 12u : now.hour % 12u;
    offset = append_number(text, offset, hour);
    text[offset++] = ':';
    text[offset++] = (char)('0' + now.minute / 10u);
    text[offset++] = (char)('0' + now.minute % 10u);
    offset = append_text(text, offset, now.hour >= 12u ? " PM " : " AM ");
    offset = append_number(text, offset, now.month);
    text[offset++] = '/';
    offset = append_number(text, offset, now.day);
    text[offset++] = '/';
    offset = append_number(text, offset, now.year);
    (void)edit_replace(s, selection_start(s), selection_end(s), text, offset,
        EDIT_OTHER);
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
/* ------------------------------------------------------------------ */

static bool action_enabled(const struct book8gui_platform *platform,
    const struct editor_state *s, uint8_t action)
{
    switch (action) {
    case ACT_UNDO:
        return s->undo_valid;
    case ACT_CUT:
    case ACT_COPY:
    case ACT_DELETE:
        return has_selection(s);
    case ACT_PASTE:
        return clipboard_length != 0u;
    case ACT_ZOOM_IN:
        return platform->draw_text_scaled != NULL && s->zoom < ED_ZOOM_MAX;
    case ACT_ZOOM_OUT:
        return platform->draw_text_scaled != NULL && s->zoom > 1u;
    case ACT_ZOOM_RESET:
        return platform->draw_text_scaled != NULL && s->zoom != 1u;
    default:
        return true;
    }
}

static bool action_checked(const struct editor_state *s, uint8_t action)
{
    return (action == ACT_WORD_WRAP && s->word_wrap)
        || (action == ACT_STATUS_BAR && s->status_bar);
}

static uint32_t entry_height(const struct menu_entry *entry)
{
    return entry->label == NULL ? ED_SEP_H : ED_ITEM_H;
}

static uint32_t header_x(const struct view *view, int32_t menu)
{
    uint32_t x = view->menu_x;
    for (int32_t i = 0; i < menu; ++i) {
        x += (text_length(menus[i].title) + 2u) * 8u;
    }
    return x;
}

static void panel_geometry(const struct WindowApp *app,
    const struct menu_def *def, uint32_t x, uint32_t y,
    struct book8gui_rect *rect)
{
    uint32_t label_max = 0u;
    uint32_t shortcut_max = 0u;
    uint32_t height = 4u;
    for (uint32_t i = 0u; i < def->count; ++i) {
        const struct menu_entry *entry = &def->entries[i];
        height += entry_height(entry);
        if (entry->label != NULL && text_length(entry->label) > label_max) {
            label_max = text_length(entry->label);
        }
        if (entry->shortcut != NULL
            && text_length(entry->shortcut) > shortcut_max) {
            shortcut_max = text_length(entry->shortcut);
        }
    }
    uint32_t width = 24u + label_max * 8u + 8u
        + (shortcut_max != 0u ? 16u + shortcut_max * 8u : 0u);
    uint32_t right = app->bounds.x + app->bounds.width;
    if (x + width > right) {
        x = right > width ? right - width : 0u;
    }
    *rect = (struct book8gui_rect){x, y, width, height};
}

static void main_panel(const struct view *view, const struct WindowApp *app,
    int32_t menu, struct book8gui_rect *rect)
{
    panel_geometry(app, &menus[menu], header_x(view, menu),
        view->menu_y + ED_MENU_BAR, rect);
}

static uint32_t entry_top(const struct menu_def *def,
    const struct book8gui_rect *panel, uint32_t index)
{
    uint32_t y = panel->y + 2u;
    for (uint32_t i = 0u; i < index; ++i) {
        y += entry_height(&def->entries[i]);
    }
    return y;
}

static void sub_panel(const struct view *view, const struct WindowApp *app,
    const struct editor_state *s, struct book8gui_rect *rect)
{
    struct book8gui_rect parent;
    main_panel(view, app, s->menu_open, &parent);
    uint32_t row = 0u;
    for (uint32_t i = 0u; i < menus[s->menu_open].count; ++i) {
        if (menus[s->menu_open].entries[i].action == ACT_SUB_ZOOM) {
            row = i;
        }
    }
    uint32_t y = entry_top(&menus[s->menu_open], &parent, row);
    panel_geometry(app, &zoom_def, parent.x + parent.width - 2u, y, rect);
    uint32_t right = app->bounds.x + app->bounds.width;
    if (rect->x + rect->width > right) {
        rect->x = parent.x > rect->width ? parent.x - rect->width + 2u : 0u;
    }
}

static int32_t entry_at(const struct menu_def *def,
    const struct book8gui_rect *panel, int32_t x, int32_t y)
{
    if (!in_rect(panel, x, y)) {
        return -1;
    }
    for (uint32_t i = 0u; i < def->count; ++i) {
        uint32_t top = entry_top(def, panel, i);
        if (y >= (int32_t)top
            && y < (int32_t)(top + entry_height(&def->entries[i]))) {
            return def->entries[i].label != NULL ? (int32_t)i : -1;
        }
    }
    return -1;
}

static int32_t header_at(const struct view *view, int32_t x, int32_t y)
{
    if (y < (int32_t)view->menu_y
        || y >= (int32_t)(view->menu_y + ED_MENU_BAR)) {
        return -1;
    }
    for (int32_t i = 0; i < MENU_COUNT; ++i) {
        int32_t left = (int32_t)header_x(view, i);
        int32_t width = (int32_t)((text_length(menus[i].title) + 2u) * 8u);
        if (x >= left && x < left + width) {
            return i;
        }
    }
    return -1;
}

static void close_menus(struct editor_state *s)
{
    s->menu_open = -1;
    s->menu_sel = -1;
    s->sub_open = false;
    s->sub_sel = -1;
    book8gui_invalidate();
}

static void open_menu(struct editor_state *s, int32_t menu)
{
    s->menu_open = menu;
    s->menu_sel = -1;
    s->sub_open = false;
    s->sub_sel = -1;
    book8gui_invalidate();
}

static const char *const help_lines[] = {
    "File:  Ctrl+N New        Ctrl+O Open",
    "       Ctrl+S Save       Ctrl+Shift+S Save As",
    "       Ctrl+Shift+N New Window",
    "Edit:  Ctrl+Z Undo      Ctrl+X/C/V Cut/Copy/Paste",
    "       Ctrl+F Find       F3 / Shift+F3 Next/Prev",
    "       Ctrl+H Replace    Ctrl+G Go To",
    "       Ctrl+A Select All F5 Time/Date",
    "View:  Ctrl+Plus/Minus/0 Zoom",
    "Mouse: click, drag to select, wheel scrolls"
};

static const char *const feedback_lines[] = {
    "BOOK8 has no mail or network client yet.",
    "Please report problems and ideas to the",
    "BOOK8 project maintainers."
};

static void run_action(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, uint8_t action)
{
    struct view view;
    compute_view(platform, app, s, &view);
    close_menus(s);
    s->last_click_valid = false;
    switch (action) {
    case ACT_NEW:
        guard_changes(platform, app, s, PEND_NEW);
        break;
    case ACT_NEW_WINDOW:
        if (book8gui_launch_class(&book8_editor_class) == NULL) {
            show_message(s, "New Window",
                "Cannot open another NoteEditor window.", NULL);
        }
        break;
    case ACT_OPEN:
        guard_changes(platform, app, s, PEND_OPEN);
        break;
    case ACT_SAVE:
        save_document(platform, app, s, PEND_NONE);
        break;
    case ACT_SAVE_AS:
        open_file_dialog(platform, s, true);
        s->after_save = PEND_NONE;
        break;
    case ACT_EXIT:
        guard_changes(platform, app, s, PEND_EXIT);
        break;
    case ACT_UNDO:
        undo_edit(s);
        break;
    case ACT_CUT:
        copy_selection(s);
        delete_backward(s);
        break;
    case ACT_COPY:
        copy_selection(s);
        break;
    case ACT_PASTE:
        paste_clipboard(s);
        break;
    case ACT_DELETE:
        delete_forward(s);
        break;
    case ACT_FIND:
        s->dialog = DLG_FIND;
        s->active_field = 0u;
        copy_text(s->field[0], ED_FIELD, s->find);
        break;
    case ACT_FIND_NEXT:
    case ACT_FIND_PREV:
        if (s->find[0] == '\0') {
            s->dialog = DLG_FIND;
            s->active_field = 0u;
            s->field[0][0] = '\0';
        } else {
            (void)find_text(s, action == ACT_FIND_NEXT);
        }
        break;
    case ACT_REPLACE:
        s->dialog = DLG_REPLACE;
        s->active_field = 0u;
        copy_text(s->field[0], ED_FIELD, s->find);
        copy_text(s->field[1], ED_FIELD, s->replace);
        break;
    case ACT_GOTO:
        s->dialog = DLG_GOTO;
        s->active_field = 0u;
        s->field[0][0] = '\0';
        break;
    case ACT_SELECT_ALL:
        s->anchor = 0u;
        move_cursor(s, s->length, true);
        break;
    case ACT_TIME_DATE:
        insert_time_date(platform, s);
        break;
    case ACT_WORD_WRAP:
        s->word_wrap = !s->word_wrap;
        s->hscroll = 0u;
        s->follow = true;
        break;
    case ACT_FONT:
        s->dialog = DLG_FONT;
        s->choice = s->bold ? 1u : 0u;
        break;
    case ACT_ZOOM_IN:
        if (action_enabled(platform, s, action)) {
            ++s->zoom;
            s->follow = true;
        }
        break;
    case ACT_ZOOM_OUT:
        if (action_enabled(platform, s, action)) {
            --s->zoom;
            s->follow = true;
        }
        break;
    case ACT_ZOOM_RESET:
        s->zoom = 1u;
        s->follow = true;
        break;
    case ACT_STATUS_BAR:
        s->status_bar = !s->status_bar;
        s->follow = true;
        break;
    case ACT_HELP:
        show_static_message(s, "NoteEditor Help", help_lines,
            (uint32_t)(sizeof(help_lines) / sizeof(help_lines[0])));
        break;
    case ACT_FEEDBACK:
        show_static_message(s, "Send Feedback", feedback_lines,
            (uint32_t)(sizeof(feedback_lines) / sizeof(feedback_lines[0])));
        break;
    case ACT_ABOUT: {
        static const char *about[4];
        about[0] = "NoteEditor 1.0";
        about[1] = "Text editor for the BOOK8 desktop.";
        about[2] = "Buffer: 64 KB, files in the root directory.";
        about[3] = "Font: built-in 8x16 bitmap (regular/bold).";
        show_static_message(s, "About NoteEditor", about, 4u);
        break;
    }
    default:
        break;
    }
    book8gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Dialog layout                                                       */
/* ------------------------------------------------------------------ */

struct button_slot {
    struct book8gui_rect rect;
    const char *label;
    uint8_t id;
};

struct dialog_layout {
    struct book8gui_rect box;
    struct book8gui_rect field[2];
    uint32_t label_y[2];
    const char *label[2];
    uint32_t field_count;
    struct book8gui_rect check;
    bool has_check;
    struct book8gui_rect drives;
    uint32_t drive_count;
    struct book8gui_rect list;
    uint32_t list_rows;
    struct book8gui_rect choice[2];
    bool has_choice;
    uint32_t text_y;
    struct button_slot buttons[4];
    uint32_t button_count;
};

static uint32_t drive_total(const struct book8gui_platform *platform)
{
    return platform->drive_count != NULL ? platform->drive_count() : 1u;
}

static char drive_at(const struct book8gui_platform *platform,
    uint32_t index)
{
    return platform->drive_letter != NULL
        ? platform->drive_letter(index) : 'A';
}

static void layout_dialog(const struct book8gui_platform *platform,
    const struct WindowApp *app, const struct editor_state *s,
    struct dialog_layout *l)
{
    struct button_slot wanted[4];
    uint32_t wanted_count = 0u;
    uint32_t width = app->bounds.width > 16u ? app->bounds.width - 16u : 0u;
    uint32_t max_width = s->dialog == DLG_MESSAGE ? 400u : 340u;
    if (width > max_width) {
        width = max_width;
    }
    *l = (struct dialog_layout){0};
    l->box.x = app->bounds.x + (app->bounds.width - width) / 2u;
    l->box.y = app->bounds.y + 54u;
    l->box.width = width;
    uint32_t x = l->box.x + 10u;
    uint32_t inner = width > 20u ? width - 20u : 0u;
    uint32_t y = l->box.y + 28u;
    uint32_t available = app->bounds.y + app->bounds.height - 4u
        > l->box.y ? app->bounds.y + app->bounds.height - 4u - l->box.y : 0u;

    switch (s->dialog) {
    case DLG_OPEN:
    case DLG_SAVE_AS:
        l->label[0] = "File name:";
        l->label_y[0] = y;
        l->field[0] = (struct book8gui_rect){x, y + 16u, inner, 20u};
        l->field_count = 1u;
        y += 42u;
        l->drives = (struct book8gui_rect){x, y, inner, 20u};
        l->drive_count = drive_total(platform);
        if (l->drive_count > inner / 36u) {
            l->drive_count = inner / 36u;
        }
        y += 24u;
        l->list_rows = available > 200u ? (available - 200u) / 16u : 0u;
        if (l->list_rows > 8u) {
            l->list_rows = 8u;
        }
        if (l->list_rows != 0u) {
            l->list = (struct book8gui_rect){x, y, inner,
                l->list_rows * 16u + 4u};
            y += l->list_rows * 16u + 8u;
        }
        wanted[wanted_count++] = (struct button_slot){
            {0}, s->dialog == DLG_OPEN ? "Open" : "Save", BTN_OK};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Cancel", BTN_CANCEL};
        break;
    case DLG_FIND:
        l->label[0] = "Find what:";
        l->label_y[0] = y;
        l->field[0] = (struct book8gui_rect){x, y + 16u, inner, 20u};
        l->field_count = 1u;
        y += 42u;
        l->check = (struct book8gui_rect){x, y, inner, 18u};
        l->has_check = true;
        y += 24u;
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Find Next", BTN_FIND_NEXT};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Cancel", BTN_CANCEL};
        break;
    case DLG_REPLACE:
        l->label[0] = "Find what:";
        l->label_y[0] = y;
        l->field[0] = (struct book8gui_rect){x, y + 16u, inner, 20u};
        y += 42u;
        l->label[1] = "Replace with:";
        l->label_y[1] = y;
        l->field[1] = (struct book8gui_rect){x, y + 16u, inner, 20u};
        l->field_count = 2u;
        y += 42u;
        l->check = (struct book8gui_rect){x, y, inner, 18u};
        l->has_check = true;
        y += 24u;
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Find Next", BTN_FIND_NEXT};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Replace", BTN_REPLACE};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Replace All", BTN_REPLACE_ALL};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Cancel", BTN_CANCEL};
        break;
    case DLG_GOTO:
        l->label[0] = "Line number:";
        l->label_y[0] = y;
        l->field[0] = (struct book8gui_rect){x, y + 16u, inner, 20u};
        l->field_count = 1u;
        y += 42u;
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Go To", BTN_OK};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Cancel", BTN_CANCEL};
        break;
    case DLG_FONT:
        l->has_choice = true;
        l->choice[0] = (struct book8gui_rect){x, y, inner, 18u};
        l->choice[1] = (struct book8gui_rect){x, y + 20u, inner, 18u};
        y += 44u;
        l->text_y = y;
        y += 24u;
        wanted[wanted_count++] = (struct button_slot){{0}, "OK", BTN_OK};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Cancel", BTN_CANCEL};
        break;
    case DLG_CONFIRM:
        l->text_y = y;
        y += 16u * s->message_count + 10u;
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Save", BTN_SAVE};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Don't Save", BTN_DONT_SAVE};
        wanted[wanted_count++] = (struct button_slot){
            {0}, "Cancel", BTN_CANCEL};
        break;
    case DLG_MESSAGE:
        l->text_y = y;
        y += 16u * s->message_count + 10u;
        wanted[wanted_count++] = (struct button_slot){{0}, "OK", BTN_OK};
        break;
    default:
        break;
    }

    uint32_t bx = x;
    uint32_t by = y;
    for (uint32_t i = 0u; i < wanted_count; ++i) {
        uint32_t button_width = text_length(wanted[i].label) * 8u + 24u;
        if (bx != x && bx + button_width > x + inner) {
            bx = x;
            by += 28u;
        }
        l->buttons[i] = wanted[i];
        l->buttons[i].rect = (struct book8gui_rect){
            bx, by, button_width, 22u};
        bx += button_width + 8u;
    }
    l->button_count = wanted_count;
    l->box.height = by + 22u + 10u - l->box.y;
}

static const char *dialog_title(const struct editor_state *s)
{
    switch (s->dialog) {
    case DLG_OPEN: return "Open";
    case DLG_SAVE_AS: return "Save As";
    case DLG_FIND: return "Find";
    case DLG_REPLACE: return "Replace";
    case DLG_GOTO: return "Go To Line";
    case DLG_FONT: return "Font";
    default: return s->message_title != NULL ? s->message_title : "NoteEditor";
    }
}

static void draw_button(const struct book8gui_platform *platform,
    const struct button_slot *button)
{
    draw_panel(platform, button->rect.x, button->rect.y, button->rect.width,
        button->rect.height);
    put_text(platform, button->rect.x + 12u, button->rect.y + 3u,
        button->label, C_TEXT, C_FACE, button->rect.width - 12u);
}

static void draw_field(const struct book8gui_platform *platform,
    const struct book8gui_rect *rect, const char *text, bool active)
{
    draw_sunken(platform, rect->x, rect->y, rect->width, rect->height,
        C_FIELD);
    uint32_t columns = rect->width > 12u ? (rect->width - 12u) / 8u : 0u;
    uint32_t length = text_length(text);
    uint32_t skip = length + 1u > columns && columns != 0u
        ? length + 1u - columns : 0u;
    char shown[ED_FIELD + 2u];
    uint32_t count = 0u;
    for (uint32_t i = skip; i < length && count < sizeof(shown) - 2u; ++i) {
        shown[count++] = text[i];
    }
    if (active) {
        shown[count++] = '_';
    }
    shown[count] = '\0';
    put_text(platform, rect->x + 4u, rect->y + 2u, shown, C_TEXT, C_FIELD,
        rect->width - 8u);
}

static void render_dialog(const struct book8gui_platform *platform,
    const struct WindowApp *app, const struct editor_state *s)
{
    struct dialog_layout l;
    layout_dialog(platform, app, s, &l);
    draw_panel(platform, l.box.x, l.box.y, l.box.width, l.box.height);
    platform->fill_rect(l.box.x + 3u, l.box.y + 3u, l.box.width - 6u, 20u,
        C_SEL);
    put_text(platform, l.box.x + 8u, l.box.y + 5u, dialog_title(s),
        C_SEL_TEXT, C_SEL, l.box.width - 16u);

    for (uint32_t i = 0u; i < l.field_count; ++i) {
        put_text(platform, l.field[i].x, l.label_y[i], l.label[i], C_TEXT,
            C_FACE, l.field[i].width);
        draw_field(platform, &l.field[i], s->field[i],
            s->active_field == i);
    }
    if (l.drive_count != 0u) {
        for (uint32_t i = 0u; i < l.drive_count; ++i) {
            char letter[3] = {drive_at(platform, i), ':', '\0'};
            bool current = letter[0] == s->drive;
            uint32_t face = current ? C_SEL : C_FIELD;
            platform->fill_rect(l.drives.x + i * 36u, l.drives.y, 34u, 20u,
                face);
            put_text(platform, l.drives.x + i * 36u + 8u, l.drives.y + 2u,
                letter, current ? C_SEL_TEXT : C_TEXT, face, 24u);
        }
    }
    if (l.list_rows != 0u) {
        draw_sunken(platform, l.list.x, l.list.y, l.list.width,
            l.list.height, C_FIELD);
        for (uint32_t row = 0u; row < l.list_rows; ++row) {
            uint32_t index = s->list_scroll + row;
            if (index >= s->file_count) {
                break;
            }
            bool selected = s->list_has_sel && s->list_sel == index;
            uint32_t face = selected ? C_SEL : C_FIELD;
            platform->fill_rect(l.list.x + 2u, l.list.y + 2u + row * 16u,
                l.list.width - 4u, 16u, face);
            put_text(platform, l.list.x + 4u, l.list.y + 2u + row * 16u,
                s->files[index], selected ? C_SEL_TEXT : C_TEXT, face,
                l.list.width - 8u);
        }
    }
    if (l.has_check) {
        draw_sunken(platform, l.check.x, l.check.y + 1u, 14u, 14u, C_FIELD);
        if (s->match_case) {
            put_text(platform, l.check.x + 3u, l.check.y, "x", C_TEXT,
                C_FIELD, 8u);
        }
        put_text(platform, l.check.x + 22u, l.check.y, "Match case",
            C_TEXT, C_FACE, l.check.width - 22u);
    }
    if (l.has_choice) {
        static const char *const names[2] = {"Regular", "Bold"};
        for (uint32_t i = 0u; i < 2u; ++i) {
            bool selected = s->choice == i;
            uint32_t face = selected ? C_SEL : C_FACE;
            platform->fill_rect(l.choice[i].x, l.choice[i].y,
                l.choice[i].width, l.choice[i].height, face);
            put_text(platform, l.choice[i].x + 6u, l.choice[i].y + 1u,
                names[i], selected ? C_SEL_TEXT : C_TEXT, face,
                l.choice[i].width - 12u);
        }
        draw_sunken(platform, l.choice[0].x, l.text_y, l.choice[0].width,
            22u, C_FIELD);
        const char *sample = "AaBbYyZz 0123";
        if (s->choice == 1u && platform->draw_text_scaled != NULL) {
            platform->draw_text_scaled(l.choice[0].x + 4u, l.text_y + 3u,
                sample, C_TEXT, C_FIELD, 1u, true);
        } else {
            put_text(platform, l.choice[0].x + 4u, l.text_y + 3u, sample,
                C_TEXT, C_FIELD, l.choice[0].width - 8u);
        }
    }
    if (s->dialog == DLG_CONFIRM || s->dialog == DLG_MESSAGE) {
        for (uint32_t i = 0u; i < s->message_count; ++i) {
            put_text(platform, l.box.x + 10u, l.text_y + i * 16u,
                s->message_lines[i], C_TEXT, C_FACE, l.box.width - 20u);
        }
    }
    for (uint32_t i = 0u; i < l.button_count; ++i) {
        draw_button(platform, &l.buttons[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void draw_cells(const struct book8gui_platform *platform,
    const struct view *view, const struct editor_state *s, uint32_t x,
    uint32_t y, const char *text, uint32_t foreground, uint32_t background)
{
    if (platform->draw_text_scaled != NULL && (view->scale != 1u || s->bold)) {
        platform->draw_text_scaled(x, y, text, foreground, background,
            view->scale, s->bold);
    } else {
        platform->draw_text(x, y, text, foreground, background);
    }
}

static void render_line(const struct book8gui_platform *platform,
    const struct view *view, const struct editor_state *s, uint32_t start,
    uint32_t row, uint32_t caret_line)
{
    char cells[ED_MAX_COLS];
    uint8_t selected[ED_MAX_COLS];
    uint32_t end = visual_end(s, start, view->cols);
    uint32_t sel_start = selection_start(s);
    uint32_t sel_end = selection_end(s);
    uint32_t column = 0u;
    for (uint32_t i = 0u; i < view->cols; ++i) {
        cells[i] = ' ';
        selected[i] = 0u;
    }
    for (uint32_t i = start; i < end; ++i) {
        char value = s->text[i];
        uint32_t cell = cell_width(value);
        bool chosen = i >= sel_start && i < sel_end;
        for (uint32_t k = 0u; k < cell; ++k) {
            uint32_t absolute = column + k;
            if (absolute >= s->hscroll
                && absolute - s->hscroll < view->cols) {
                uint32_t slot = absolute - s->hscroll;
                cells[slot] = value >= 0x20 && value <= 0x7e ? value
                    : (value == '\t' ? ' ' : '?');
                selected[slot] = chosen ? 1u : 0u;
            }
        }
        column += cell;
    }
    if (end < s->length && s->text[end] == '\n' && end >= sel_start
        && end < sel_end && column >= s->hscroll
        && column - s->hscroll < view->cols) {
        selected[column - s->hscroll] = 1u;
    }

    uint32_t x0 = view->frame_x + ED_PAD;
    uint32_t y0 = view->frame_y + ED_PAD + row * view->cell_h;
    uint32_t slot = 0u;
    while (slot < view->cols) {
        uint32_t run = slot;
        char run_text[ED_MAX_COLS];
        bool blank = true;
        while (run < view->cols && selected[run] == selected[slot]) {
            run_text[run - slot] = cells[run];
            if (cells[run] != ' ') {
                blank = false;
            }
            ++run;
        }
        run_text[run - slot] = '\0';
        if (selected[slot] != 0u || !blank) {
            draw_cells(platform, view, s, x0 + slot * view->cell_w, y0,
                run_text, selected[slot] != 0u ? C_SEL_TEXT : C_TEXT,
                selected[slot] != 0u ? C_SEL : C_FIELD);
        }
        slot = run;
    }

    if (caret_line == start) {
        uint32_t caret = column_of(s, start, s->cursor);
        if (caret >= s->hscroll && caret - s->hscroll < view->cols) {
            platform->fill_rect(x0 + (caret - s->hscroll) * view->cell_w,
                y0, 2u, view->cell_h, C_TEXT);
        }
    }
}

static void render_scrollbar(const struct book8gui_platform *platform,
    const struct view *view, const struct editor_state *s)
{
    uint32_t x = view->frame_x + view->frame_w - ED_SCROLL_W - 1u;
    uint32_t track = view->frame_h > 4u ? view->frame_h - 4u : 0u;
    if (track < 12u) {
        return;
    }
    platform->fill_rect(x, view->frame_y + 2u, ED_SCROLL_W, track, C_FACE);
    uint32_t thumb = 16u;
    uint32_t span = track - thumb;
    uint32_t offset = s->length != 0u
        ? (uint32_t)((uint64_t)s->top * span / s->length) : 0u;
    platform->fill_rect(x + 1u, view->frame_y + 2u + offset, ED_SCROLL_W - 2u,
        thumb, C_SHADOW);
}

static void render_menu_bar(const struct book8gui_platform *platform,
    const struct view *view, const struct editor_state *s)
{
    platform->fill_rect(view->menu_x, view->menu_y, view->menu_w,
        ED_MENU_BAR, C_FACE);
    for (int32_t i = 0; i < MENU_COUNT; ++i) {
        uint32_t x = header_x(view, i);
        uint32_t width = (text_length(menus[i].title) + 2u) * 8u;
        bool open = s->menu_open == i;
        uint32_t face = open ? C_SEL : C_FACE;
        if (x + width > view->menu_x + view->menu_w) {
            break;
        }
        platform->fill_rect(x, view->menu_y + 1u, width, ED_MENU_BAR - 2u,
            face);
        put_text(platform, x + 8u, view->menu_y + 2u, menus[i].title,
            open ? C_SEL_TEXT : C_TEXT, face, width - 8u);
    }
}

static void render_menu_panel(const struct book8gui_platform *platform,
    const struct editor_state *s, const struct menu_def *def,
    const struct book8gui_rect *panel, int32_t selected)
{
    draw_panel(platform, panel->x, panel->y, panel->width, panel->height);
    for (uint32_t i = 0u; i < def->count; ++i) {
        const struct menu_entry *entry = &def->entries[i];
        uint32_t y = entry_top(def, panel, i);
        if (entry->label == NULL) {
            platform->fill_rect(panel->x + 4u, y + 3u, panel->width - 8u, 1u,
                C_SHADOW);
            platform->fill_rect(panel->x + 4u, y + 4u, panel->width - 8u, 1u,
                C_LIGHT);
            continue;
        }
        bool enabled = action_enabled(platform, s, entry->action);
        bool hot = selected == (int32_t)i && enabled;
        uint32_t face = hot ? C_SEL : C_FACE;
        uint32_t color = !enabled ? C_DISABLED : (hot ? C_SEL_TEXT : C_TEXT);
        platform->fill_rect(panel->x + 2u, y, panel->width - 4u, ED_ITEM_H,
            face);
        if (action_checked(s, entry->action)) {
            put_text(platform, panel->x + 6u, y + 1u, "x", color, face, 8u);
        }
        put_text(platform, panel->x + 22u, y + 1u, entry->label, color, face,
            panel->width - 24u);
        if (entry->shortcut != NULL) {
            uint32_t length = text_length(entry->shortcut);
            put_text(platform, panel->x + panel->width - 8u - length * 8u,
                y + 1u, entry->shortcut, color, face, length * 8u);
        }
    }
}

static void render_status(const struct book8gui_platform *platform,
    const struct view *view, const struct editor_state *s)
{
    char line[ED_MAX_COLS];
    uint32_t row = 1u;
    uint32_t counted = 0u;
    for (uint32_t i = 0u; i < s->cursor && i < s->length; ++i) {
        if (s->text[i] == '\n') {
            ++row;
            counted = i + 1u;
        }
    }
    uint32_t offset = append_text(line, 0u, "Ln ");
    offset = append_number(line, offset, row);
    offset = append_text(line, offset, ", Col ");
    offset = append_number(line, offset,
        column_of(s, counted, s->cursor) + 1u);
    offset = append_text(line, offset, " | ");
    offset = append_number(line, offset,
        (platform->draw_text_scaled != NULL ? s->zoom : 1u) * 100u);
    offset = append_text(line, offset, "% | ");
    if (s->note[0] != '\0') {
        offset = append_text(line, offset, s->note);
    } else {
        offset = append_text(line, offset, s->has_path ? s->path : "Untitled");
        if (s->dirty) {
            line[offset++] = '*';
        }
    }
    line[offset] = '\0';
    draw_sunken(platform, view->menu_x, view->status_y, view->menu_w,
        ED_STATUS_BAR, C_FACE);
    put_text(platform, view->menu_x + 6u, view->status_y + 2u, line, C_TEXT,
        C_FACE, view->menu_w - 12u);
}

static void render(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    struct editor_state *s = app->state;
    struct view view;
    compute_view(platform, app, s, &view);
    settle_view(s, &view);

    draw_sunken(platform, view.frame_x, view.frame_y, view.frame_w,
        view.frame_h, C_FIELD);
    uint32_t caret_line = visual_start(s, s->cursor, view.cols);
    uint32_t line = s->top;
    for (uint32_t row = 0u; row < view.rows && line != ED_NONE; ++row) {
        render_line(platform, &view, s, line, row, caret_line);
        line = next_visual_start(s, line, view.cols);
    }
    render_scrollbar(platform, &view, s);
    if (s->status_bar) {
        render_status(platform, &view, s);
    }
    render_menu_bar(platform, &view, s);
    if (s->menu_open >= 0) {
        struct book8gui_rect panel;
        main_panel(&view, app, s->menu_open, &panel);
        render_menu_panel(platform, s, &menus[s->menu_open], &panel,
            s->menu_sel);
        if (s->sub_open) {
            struct book8gui_rect sub;
            sub_panel(&view, app, s, &sub);
            render_menu_panel(platform, s, &zoom_def, &sub, s->sub_sel);
        }
    }
    if (s->dialog != DLG_NONE) {
        render_dialog(platform, app, s);
    }
}

/* ------------------------------------------------------------------ */
/* Dialog input                                                        */
/* ------------------------------------------------------------------ */

static void apply_button(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, uint8_t id)
{
    switch (id) {
    case BTN_CANCEL:
        s->after_save = PEND_NONE;
        close_dialog(s);
        break;
    case BTN_OK:
        if (s->dialog == DLG_OPEN || s->dialog == DLG_SAVE_AS) {
            commit_file_dialog(platform, app, s);
        } else if (s->dialog == DLG_GOTO) {
            go_to_line(s);
        } else if (s->dialog == DLG_FONT) {
            s->bold = s->choice == 1u;
            close_dialog(s);
        } else {
            close_dialog(s);
        }
        break;
    case BTN_FIND_NEXT:
        copy_text(s->find, ED_FIELD, s->field[0]);
        if (s->dialog == DLG_FIND) {
            close_dialog(s);
        }
        (void)find_text(s, true);
        break;
    case BTN_REPLACE:
        copy_text(s->find, ED_FIELD, s->field[0]);
        copy_text(s->replace, ED_FIELD, s->field[1]);
        replace_current(s);
        break;
    case BTN_REPLACE_ALL:
        copy_text(s->find, ED_FIELD, s->field[0]);
        copy_text(s->replace, ED_FIELD, s->field[1]);
        close_dialog(s);
        replace_all(s);
        break;
    case BTN_SAVE: {
        enum pending pending = s->confirm_pending;
        close_dialog(s);
        save_document(platform, app, s, pending);
        break;
    }
    case BTN_DONT_SAVE: {
        enum pending pending = s->confirm_pending;
        close_dialog(s);
        s->dirty = false;
        perform_pending(platform, app, s, pending);
        break;
    }
    default:
        break;
    }
    book8gui_invalidate();
}

static void dialog_primary(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s)
{
    switch (s->dialog) {
    case DLG_FIND:
    case DLG_REPLACE:
        apply_button(platform, app, s, BTN_FIND_NEXT);
        break;
    case DLG_CONFIRM:
        apply_button(platform, app, s, BTN_SAVE);
        break;
    default:
        apply_button(platform, app, s, BTN_OK);
        break;
    }
}

static void select_list_entry(struct editor_state *s, uint32_t index)
{
    s->list_sel = index;
    s->list_has_sel = true;
    copy_text(s->field[0], ED_FIELD, s->files[index]);
    book8gui_invalidate();
}

static bool dialog_key(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, uint32_t key)
{
    uint32_t base = key & BOOK8GUI_KEY_MASK;
    bool ctrl = (key & BOOK8GUI_KEY_CTRL) != 0u;
    char *field = s->field[s->active_field < 2u ? s->active_field : 0u];
    uint32_t length = text_length(field);
    bool text_dialog = s->dialog == DLG_OPEN || s->dialog == DLG_SAVE_AS
        || s->dialog == DLG_FIND || s->dialog == DLG_REPLACE
        || s->dialog == DLG_GOTO;

    if (base == BOOK8GUI_KEY_ESCAPE) {
        apply_button(platform, app, s, BTN_CANCEL);
    } else if (base == BOOK8GUI_KEY_ENTER) {
        dialog_primary(platform, app, s);
    } else if (base == BOOK8GUI_KEY_TAB && s->dialog == DLG_REPLACE) {
        s->active_field ^= 1u;
    } else if (s->dialog == DLG_FONT
        && (base == BOOK8GUI_KEY_UP || base == BOOK8GUI_KEY_DOWN)) {
        s->choice = base == BOOK8GUI_KEY_UP ? 0u : 1u;
    } else if ((s->dialog == DLG_OPEN || s->dialog == DLG_SAVE_AS)
        && (base == BOOK8GUI_KEY_UP || base == BOOK8GUI_KEY_DOWN)
        && s->file_count != 0u) {
        uint32_t index = !s->list_has_sel ? 0u
            : (base == BOOK8GUI_KEY_UP
                ? (s->list_sel > 0u ? s->list_sel - 1u : 0u)
                : (s->list_sel + 1u < s->file_count
                    ? s->list_sel + 1u : s->list_sel));
        select_list_entry(s, index);
        if (index < s->list_scroll) {
            s->list_scroll = index;
        } else if (index >= s->list_scroll + 8u) {
            s->list_scroll = index - 7u;
        }
    } else if (text_dialog && base == BOOK8GUI_KEY_BACKSPACE && length != 0u) {
        field[length - 1u] = '\0';
    } else if (text_dialog && ctrl && base == 'v') {
        for (uint32_t i = 0u; i < clipboard_length && length + 1u < ED_FIELD
            && clipboard[i] != '\n'; ++i) {
            field[length++] = clipboard[i];
        }
        field[length] = '\0';
    } else if (text_dialog && !ctrl && base >= 0x20u && base <= 0x7eu
        && length + 1u < ED_FIELD
        && (s->dialog != DLG_GOTO || (base >= '0' && base <= '9'))) {
        field[length] = (char)base;
        field[length + 1u] = '\0';
    }
    book8gui_invalidate();
    return true;
}

static void dialog_click(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, int32_t x, int32_t y)
{
    struct dialog_layout l;
    layout_dialog(platform, app, s, &l);
    for (uint32_t i = 0u; i < l.button_count; ++i) {
        if (in_rect(&l.buttons[i].rect, x, y)) {
            apply_button(platform, app, s, l.buttons[i].id);
            return;
        }
    }
    for (uint32_t i = 0u; i < l.field_count; ++i) {
        if (in_rect(&l.field[i], x, y)) {
            s->active_field = i;
            book8gui_invalidate();
            return;
        }
    }
    if (l.has_check && in_rect(&l.check, x, y)) {
        s->match_case = !s->match_case;
        book8gui_invalidate();
        return;
    }
    if (l.has_choice) {
        for (uint32_t i = 0u; i < 2u; ++i) {
            if (in_rect(&l.choice[i], x, y)) {
                s->choice = i;
                book8gui_invalidate();
                return;
            }
        }
    }
    if (l.drive_count != 0u && in_rect(&l.drives, x, y)) {
        uint32_t index = ((uint32_t)x - l.drives.x) / 36u;
        if (index < l.drive_count) {
            s->drive = drive_at(platform, index);
            refresh_file_list(platform, s);
        }
        return;
    }
    if (l.list_rows != 0u && in_rect(&l.list, x, y)) {
        uint32_t index = s->list_scroll
            + ((uint32_t)y - l.list.y - 2u) / 16u;
        if (index >= s->file_count) {
            return;
        }
        uint64_t now = platform->clock_ticks != NULL
            ? platform->clock_ticks() : 0u;
        uint64_t threshold = (uint64_t)platform->clock_hz * 400u / 1000u;
        bool again = platform->clock_ticks != NULL && s->last_click_valid
            && s->last_click_index == index && now >= s->last_click_ticks
            && now - s->last_click_ticks <= threshold;
        select_list_entry(s, index);
        s->last_click_index = index;
        s->last_click_ticks = now;
        s->last_click_valid = !again;
        if (again) {
            commit_file_dialog(platform, app, s);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

static bool handle_shortcut(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, uint32_t base, bool shift)
{
    uint8_t action = ACT_NONE;
    switch (base) {
    case 'n': action = shift ? ACT_NEW_WINDOW : ACT_NEW; break;
    case 'o': action = ACT_OPEN; break;
    case 's': action = shift ? ACT_SAVE_AS : ACT_SAVE; break;
    case 'z': action = ACT_UNDO; break;
    case 'x': action = ACT_CUT; break;
    case 'c': action = ACT_COPY; break;
    case 'v': action = ACT_PASTE; break;
    case 'a': action = ACT_SELECT_ALL; break;
    case 'f': action = ACT_FIND; break;
    case 'h': action = ACT_REPLACE; break;
    case 'g': action = ACT_GOTO; break;
    case '+':
    case '=': action = ACT_ZOOM_IN; break;
    case '-':
    case '_': action = ACT_ZOOM_OUT; break;
    case '0': action = ACT_ZOOM_RESET; break;
    default: break;
    }
    if (action == ACT_NONE) {
        return false;
    }
    if (action_enabled(platform, s, action)) {
        run_action(platform, app, s, action);
    }
    return true;
}

static bool menu_key(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, uint32_t base)
{
    const struct menu_def *def = s->sub_open ? &zoom_def
        : &menus[s->menu_open];
    int32_t *selection = s->sub_open ? &s->sub_sel : &s->menu_sel;
    int32_t count = (int32_t)def->count;
    if (base == BOOK8GUI_KEY_ESCAPE) {
        if (s->sub_open) {
            s->sub_open = false;
        } else {
            close_menus(s);
        }
    } else if (base == BOOK8GUI_KEY_LEFT && s->sub_open) {
        s->sub_open = false;
    } else if (base == BOOK8GUI_KEY_LEFT || base == BOOK8GUI_KEY_RIGHT) {
        bool enter = base == BOOK8GUI_KEY_RIGHT
            && s->menu_sel >= 0
            && menus[s->menu_open].entries[s->menu_sel].action
                == ACT_SUB_ZOOM;
        if (enter) {
            s->sub_open = true;
            s->sub_sel = 0;
        } else {
            int32_t next = s->menu_open
                + (base == BOOK8GUI_KEY_LEFT ? -1 : 1);
            open_menu(s, (next + MENU_COUNT) % MENU_COUNT);
        }
    } else if (base == BOOK8GUI_KEY_UP || base == BOOK8GUI_KEY_DOWN) {
        int32_t step = base == BOOK8GUI_KEY_UP ? -1 : 1;
        int32_t index = *selection;
        for (int32_t tries = 0; tries < count; ++tries) {
            index = (index + step + count) % count;
            if (index < 0) {
                index += count;
            }
            if (def->entries[index].label != NULL) {
                break;
            }
        }
        *selection = index;
    } else if (base == BOOK8GUI_KEY_ENTER && *selection >= 0) {
        uint8_t action = def->entries[*selection].action;
        if (action == ACT_SUB_ZOOM) {
            s->sub_open = true;
            s->sub_sel = 0;
        } else if (action_enabled(platform, s, action)) {
            run_action(platform, app, s, action);
        }
    }
    book8gui_invalidate();
    return true;
}

static bool handle_key(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, uint32_t key)
{
    uint32_t base = key & BOOK8GUI_KEY_MASK;
    bool ctrl = (key & BOOK8GUI_KEY_CTRL) != 0u;
    bool shift = (key & BOOK8GUI_KEY_SHIFT) != 0u;
    struct view view;

    if (s->dialog != DLG_NONE) {
        return dialog_key(platform, app, s, key);
    }
    if (s->menu_open >= 0) {
        return menu_key(platform, app, s, base);
    }
    compute_view(platform, app, s, &view);
    if (base == BOOK8GUI_KEY_F10) {
        open_menu(s, 0);
        return true;
    }
    if (base == BOOK8GUI_KEY_F1) {
        run_action(platform, app, s, ACT_HELP);
        return true;
    }
    if (base == BOOK8GUI_KEY_F3) {
        run_action(platform, app, s, shift ? ACT_FIND_PREV : ACT_FIND_NEXT);
        return true;
    }
    if (base == BOOK8GUI_KEY_F5) {
        run_action(platform, app, s, ACT_TIME_DATE);
        return true;
    }
    if (ctrl && base >= 0x20u && base <= 0x7eu) {
        (void)handle_shortcut(platform, app, s, base, shift);
        return true;
    }

    uint32_t position = s->cursor;
    switch (base) {
    case BOOK8GUI_KEY_LEFT:
        if (!shift && has_selection(s)) {
            position = selection_start(s);
        } else if (ctrl) {
            position = word_left(s, s->cursor);
        } else if (s->cursor > 0u) {
            position = s->cursor - 1u;
        }
        move_cursor(s, position, shift);
        break;
    case BOOK8GUI_KEY_RIGHT:
        if (!shift && has_selection(s)) {
            position = selection_end(s);
        } else if (ctrl) {
            position = word_right(s, s->cursor);
        } else if (s->cursor < s->length) {
            position = s->cursor + 1u;
        }
        move_cursor(s, position, shift);
        break;
    case BOOK8GUI_KEY_UP:
        move_vertical(s, &view, -1, shift);
        break;
    case BOOK8GUI_KEY_DOWN:
        move_vertical(s, &view, 1, shift);
        break;
    case BOOK8GUI_KEY_PAGE_UP:
        move_vertical(s, &view, -(int32_t)(view.rows > 1u ? view.rows - 1u
            : 1u), shift);
        break;
    case BOOK8GUI_KEY_PAGE_DOWN:
        move_vertical(s, &view, (int32_t)(view.rows > 1u ? view.rows - 1u
            : 1u), shift);
        break;
    case BOOK8GUI_KEY_HOME:
        move_cursor(s, ctrl ? 0u : visual_start(s, s->cursor, view.cols),
            shift);
        break;
    case BOOK8GUI_KEY_END:
        if (ctrl) {
            position = s->length;
        } else {
            uint32_t start = visual_start(s, s->cursor, view.cols);
            uint32_t end = visual_end(s, start, view.cols);
            position = end < line_end(s, start) && end > start
                ? end - 1u : end;
        }
        move_cursor(s, position, shift);
        break;
    case BOOK8GUI_KEY_BACKSPACE:
        delete_backward(s);
        break;
    case BOOK8GUI_KEY_DELETE:
        delete_forward(s);
        break;
    case BOOK8GUI_KEY_ENTER:
        insert_char(s, '\n');
        break;
    case BOOK8GUI_KEY_TAB:
        insert_char(s, '\t');
        break;
    case BOOK8GUI_KEY_ESCAPE:
        break;
    default:
        if (base >= 0x20u && base <= 0x7eu) {
            insert_char(s, (char)base);
        }
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

static uint32_t position_at(const struct view *view,
    const struct editor_state *s, int32_t x, int32_t y)
{
    int32_t top = (int32_t)(view->frame_y + ED_PAD);
    int32_t left = (int32_t)(view->frame_x + ED_PAD);
    int32_t row = y < top ? 0 : (y - top) / (int32_t)view->cell_h;
    int32_t column = x < left ? 0
        : (x - left + (int32_t)view->cell_w / 2) / (int32_t)view->cell_w;
    uint32_t line = s->top;
    for (int32_t i = 0; i < row; ++i) {
        uint32_t next = next_visual_start(s, line, view->cols);
        if (next == ED_NONE) {
            break;
        }
        line = next;
    }
    return offset_at_column(s, line, (uint32_t)column + s->hscroll,
        view->cols);
}

static void scrollbar_drag(struct editor_state *s, const struct view *view,
    int32_t y)
{
    uint32_t track = view->frame_h > 4u ? view->frame_h - 4u : 1u;
    int32_t relative = y - (int32_t)view->frame_y - 2;
    uint32_t clamped = relative < 0 ? 0u
        : ((uint32_t)relative > track ? track : (uint32_t)relative);
    uint32_t target = (uint32_t)((uint64_t)s->length * clamped / track);
    s->top = visual_start(s, target, view->cols);
    book8gui_invalidate();
}

static void menu_mouse(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s, const struct view *view,
    int32_t x, int32_t y, bool pressed)
{
    int32_t header = header_at(view, x, y);
    if (header >= 0) {
        if (pressed && header == s->menu_open) {
            close_menus(s);
        } else if (header != s->menu_open) {
            open_menu(s, header);
        }
        return;
    }
    if (s->sub_open) {
        struct book8gui_rect sub;
        sub_panel(view, app, s, &sub);
        int32_t index = entry_at(&zoom_def, &sub, x, y);
        if (index >= 0) {
            if (s->sub_sel != index) {
                s->sub_sel = index;
                book8gui_invalidate();
            }
            if (pressed && action_enabled(platform, s,
                zoom_menu[index].action)) {
                run_action(platform, app, s, zoom_menu[index].action);
            }
            return;
        }
    }
    struct book8gui_rect panel;
    main_panel(view, app, s->menu_open, &panel);
    int32_t index = entry_at(&menus[s->menu_open], &panel, x, y);
    if (index >= 0) {
        uint8_t action = menus[s->menu_open].entries[index].action;
        if (s->menu_sel != index) {
            s->menu_sel = index;
            s->sub_open = action == ACT_SUB_ZOOM;
            s->sub_sel = -1;
            book8gui_invalidate();
        }
        if (pressed) {
            if (action == ACT_SUB_ZOOM) {
                s->sub_open = true;
            } else if (action_enabled(platform, s, action)) {
                run_action(platform, app, s, action);
            }
        }
        return;
    }
    if (in_rect(&panel, x, y)) {
        return;
    }
    if (pressed) {
        close_menus(s);
    }
}

static void handle_mouse(const struct book8gui_platform *platform,
    struct WindowApp *app, struct editor_state *s,
    const struct book8gui_event *event)
{
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool was_down = (s->previous_buttons & 1u) != 0u;
    bool pressed = down && !was_down;
    int32_t x = event->mouse_x;
    int32_t y = event->mouse_y;
    struct book8gui_rect bounds = app->bounds;
    bool inside = in_rect(&bounds, x, y);
    struct view view;
    s->previous_buttons = event->mouse_buttons;
    compute_view(platform, app, s, &view);
    if (!down) {
        s->selecting = false;
        s->scroll_drag = false;
    }

    if (event->wheel_delta != 0 && inside) {
        if ((s->dialog == DLG_OPEN || s->dialog == DLG_SAVE_AS)
            && s->file_count != 0u) {
            s->list_scroll = ui_wheel_scroll_delta(s->list_scroll, s->file_count,
                1u, event->wheel_delta, 1u, &s->wheel_remainder[1]);
        } else if (s->dialog == DLG_NONE && s->menu_open < 0
            && x >= (int32_t)view.frame_x && y >= (int32_t)view.frame_y
            && x < (int32_t)(view.frame_x + view.frame_w)
            && y < (int32_t)(view.frame_y + view.frame_h)) {
            scroll_lines(s, view.cols, -ui_wheel_steps(event->wheel_delta, &s->wheel_remainder[0]) * 3);
            uint32_t last_top = visual_start(s, s->length, view.cols);
            for (uint32_t row = 1u; row < view.rows; ++row) {
                uint32_t previous = prev_visual_start(s, last_top, view.cols);
                if (previous == ED_NONE) {
                    break;
                }
                last_top = previous;
            }
            if (s->top > last_top) {
                s->top = last_top;
            }
        }
        book8gui_invalidate();
    }

    if (s->dialog != DLG_NONE) {
        if (pressed && inside) {
            dialog_click(platform, app, s, x, y);
        }
        return;
    }
    if (s->menu_open >= 0) {
        if (inside || pressed) {
            menu_mouse(platform, app, s, &view, x, y, pressed);
        }
        return;
    }
    if (pressed && !inside) {
        return;
    }
    if (pressed) {
        int32_t header = header_at(&view, x, y);
        struct book8gui_rect frame = {view.frame_x, view.frame_y,
            view.frame_w, view.frame_h};
        if (header >= 0) {
            open_menu(s, header);
            return;
        }
        if (in_rect(&frame, x, y)) {
            s->last_edit = EDIT_NONE;
            if (x >= (int32_t)(view.frame_x + view.frame_w - ED_SCROLL_W - 2u)) {
                s->scroll_drag = true;
                scrollbar_drag(s, &view, y);
            } else {
                move_cursor(s, position_at(&view, s, x, y), false);
                s->selecting = true;
            }
        }
        return;
    }
    if (down && s->scroll_drag) {
        scrollbar_drag(s, &view, y);
    } else if (down && s->selecting) {
        if (y < (int32_t)(view.frame_y + ED_PAD)) {
            scroll_lines(s, view.cols, -1);
        } else if (y >= (int32_t)(view.frame_y + view.frame_h - ED_PAD)) {
            scroll_lines(s, view.cols, 1);
        }
        uint32_t position = position_at(&view, s, x, y);
        s->cursor = position;
        s->follow = false;
        book8gui_invalidate();
    }
}

static bool handle_event(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event)
{
    struct editor_state *s = app->state;
    if (s == NULL) {
        return true;
    }
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        handle_mouse(platform, app, s, event);
        return true;
    }
    return handle_key(platform, app, s, event->key);
}

static void *create(const struct book8gui_platform *platform)
{
    (void)platform;
    for (uint32_t i = 0u; i < BOOK8GUI_MAX_INSTANCES_PER_APP; ++i) {
        struct editor_state *s = &editor_pool[i];
        if (s->in_use) {
            continue;
        }
        s->in_use = true;
        reset_document(s);
        s->word_wrap = false;
        s->status_bar = true;
        s->bold = false;
        s->zoom = 1u;
        s->menu_open = -1;
        s->menu_sel = -1;
        s->sub_open = false;
        s->sub_sel = -1;
        s->previous_buttons = 0u;
        s->selecting = false;
        s->scroll_drag = false;
        s->dialog = DLG_NONE;
        s->match_case = false;
        s->find[0] = '\0';
        s->replace[0] = '\0';
        s->field[0][0] = '\0';
        s->field[1][0] = '\0';
        s->after_save = PEND_NONE;
        s->drive = 'A';
        s->file_count = 0u;
        return s;
    }
    return NULL;
}

static void destroy(void *state)
{
    struct editor_state *s = state;
    if (s != NULL) {
        s->in_use = false;
    }
}

const struct book8gui_app_class book8_editor_class = {
    .open_path = book8_editor_open_path,
    .name = "NoteEditor",
    .default_width = 560u,
    .default_height = 360u,
    .single_instance = false,
    .wants_all_keys = true,
    .state_size = sizeof(struct editor_state),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event
};
