#include "book8_taskmgr.h"

#include <stddef.h>

#include "book8_ui.h"

#define TM_ROW_H 18u
#define TM_TAB_H 22u
#define TM_TAB1_W 112u
#define TM_TAB2_W 128u
#define TM_BUTTON_W 88u
#define TM_BUTTON_H 22u
#define TM_FOOTER_H 28u
#define TM_MAX_ROWS 16u
#define TM_MAX_THREADS 16u
#define TM_PERF_LINES 18u
#define TM_LINE_LEN 72u

#define TM_COL_NAME 0u
#define TM_COL_PID 168u
#define TM_COL_CPU 216u
#define TM_COL_MEM 280u
#define TM_COL_DISK 360u

enum tm_tab {
    TM_TAB_PROCESSES,
    TM_TAB_PERFORMANCE
};

struct tm_state {
    enum tm_tab tab;
    bool has_selection;
    uint32_t selected_id;
    uint32_t first;
    uint8_t previous_buttons;
    bool scroll_drag;
    int32_t scroll_grab;
    int32_t wheel_remainder;
    char message[32];
};

static struct tm_state tm;

struct tm_layout {
    struct book8gui_rect tab1;
    struct book8gui_rect tab2;
    struct book8gui_rect frame;
    struct book8gui_rect track;
    struct book8gui_rect button;
    struct book8gui_rect status;
    uint32_t text_x;
    uint32_t text_y;
    uint32_t limit_x;
    uint32_t rows;
    uint32_t total;
};

struct tm_buf {
    char *text;
    size_t length;
    size_t capacity;
};

static void buf_str(struct tm_buf *b, const char *s)
{
    while (*s != '\0' && b->length + 1u < b->capacity) {
        b->text[b->length++] = *s++;
    }
    b->text[b->length] = '\0';
}

static void buf_u64(struct tm_buf *b, uint64_t value, uint32_t min_digits)
{
    char digits[21];
    uint32_t n = 0u;
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && n < sizeof(digits));
    while (n < min_digits && n < sizeof(digits)) {
        digits[n++] = '0';
    }
    while (n != 0u && b->length + 1u < b->capacity) {
        b->text[b->length++] = digits[--n];
    }
    b->text[b->length] = '\0';
}

/* Prints tenths: 123 -> "12.3". */
static void buf_tenths(struct tm_buf *b, uint64_t tenths)
{
    buf_u64(b, tenths / 10u, 1u);
    buf_str(b, ".");
    buf_u64(b, tenths % 10u, 1u);
}

static void buf_percent(struct tm_buf *b, uint32_t permille)
{
    buf_tenths(b, permille > 1000u ? 1000u : permille);
    buf_str(b, "%");
}

static void buf_memory(struct tm_buf *b, uint64_t kb)
{
    if (kb < 1024u) {
        buf_u64(b, kb, 1u);
        buf_str(b, " KB");
    } else {
        buf_tenths(b, kb * 10u / 1024u);
        buf_str(b, " MB");
    }
}

static void buf_rate(struct tm_buf *b, uint64_t bytes_per_second)
{
    buf_tenths(b, bytes_per_second * 10u / 1024u);
    buf_str(b, " KB/s");
}

static void buf_clock(struct tm_buf *b, uint64_t seconds)
{
    buf_u64(b, seconds / 3600u, 2u);
    buf_str(b, ":");
    buf_u64(b, seconds / 60u % 60u, 2u);
    buf_str(b, ":");
    buf_u64(b, seconds % 60u, 2u);
}

static void set_message(const char *text)
{
    size_t i = 0u;
    for (; text[i] != '\0' && i + 1u < sizeof(tm.message); ++i) {
        tm.message[i] = text[i];
    }
    tm.message[i] = '\0';
}

static void compute_layout(const struct WindowApp *app,
    struct tm_layout *layout)
{
    struct book8gui_rect client = ui_client(app);
    bool processes = tm.tab == TM_TAB_PROCESSES;
    layout->tab1 = (struct book8gui_rect){client.x, client.y, TM_TAB1_W,
        TM_TAB_H};
    layout->tab2 = (struct book8gui_rect){client.x + TM_TAB1_W + 2u,
        client.y, TM_TAB2_W, TM_TAB_H};
    layout->status = (struct book8gui_rect){
        client.x, client.y + client.height - UI_STATUS_H, client.width,
        client.height >= UI_STATUS_H ? UI_STATUS_H : 0u
    };
    uint32_t used = TM_TAB_H + 2u + UI_STATUS_H + 2u
        + (processes ? TM_FOOTER_H : 0u);
    layout->frame = (struct book8gui_rect){
        client.x, client.y + TM_TAB_H + 2u, client.width,
        client.height > used ? client.height - used : 0u
    };
    layout->track = ui_scroll_track(&layout->frame);
    layout->button = (struct book8gui_rect){
        client.x + (client.width > TM_BUTTON_W + 4u
            ? client.width - TM_BUTTON_W - 4u : 0u),
        layout->frame.y + layout->frame.height + 3u, TM_BUTTON_W,
        TM_BUTTON_H
    };
    layout->text_x = layout->frame.x + UI_PAD + 4u;
    layout->text_y = layout->frame.y + UI_PAD
        + (processes ? TM_ROW_H : 0u);
    layout->limit_x = layout->frame.x + layout->frame.width
        - UI_SCROLL_W - 4u;
    uint32_t inner = layout->frame.height > 2u * UI_PAD
        ? layout->frame.height - 2u * UI_PAD : 0u;
    if (processes) {
        inner = inner > TM_ROW_H ? inner - TM_ROW_H : 0u;
    }
    layout->rows = inner / TM_ROW_H;
}

static void draw_clipped(const struct book8gui_platform *platform, uint32_t x,
    uint32_t y, const char *text, uint32_t max_chars, uint32_t limit_x,
    uint32_t fg, uint32_t bg)
{
    if (x >= limit_x) {
        return;
    }
    uint32_t fit = (limit_x - x) / 8u;
    if (max_chars > fit) {
        max_chars = fit;
    }
    char clipped[TM_LINE_LEN];
    uint32_t n = 0u;
    while (text[n] != '\0' && n < max_chars && n + 1u < sizeof(clipped)) {
        clipped[n] = text[n];
        ++n;
    }
    clipped[n] = '\0';
    platform->draw_text(x, y, clipped, fg, bg);
}

static void draw_button(const struct book8gui_platform *platform,
    const struct book8gui_rect *r, const char *label, bool enabled)
{
    platform->fill_rect(r->x, r->y, r->width, r->height, UI_SHADOW);
    platform->fill_rect(r->x + 1u, r->y + 1u, r->width - 2u, r->height - 2u,
        UI_FACE);
    uint32_t length = 0u;
    while (label[length] != '\0') {
        ++length;
    }
    platform->draw_text(r->x + (r->width > length * 8u
        ? (r->width - length * 8u) / 2u : 0u), r->y + 3u, label,
        enabled ? UI_TEXT : UI_SHADOW, UI_FACE);
}

static void draw_tab(const struct book8gui_platform *platform,
    const struct book8gui_rect *r, const char *label, bool active)
{
    uint32_t bg = active ? UI_SELECTED : UI_FACE;
    platform->fill_rect(r->x, r->y, r->width, r->height, UI_SHADOW);
    platform->fill_rect(r->x + 1u, r->y + 1u, r->width - 2u, r->height - 1u,
        bg);
    uint32_t length = 0u;
    while (label[length] != '\0') {
        ++length;
    }
    platform->draw_text(r->x + (r->width > length * 8u
        ? (r->width - length * 8u) / 2u : 0u), r->y + 3u, label,
        active ? UI_SELECTED_TEXT : UI_TEXT, bg);
}

static uint32_t selected_index(const struct book8gui_window_info *list,
    uint32_t count)
{
    for (uint32_t i = 0u; tm.has_selection && i < count; ++i) {
        if (list[i].id == tm.selected_id) {
            return i;
        }
    }
    return count;
}

static void clamp_first(const struct tm_layout *layout)
{
    uint32_t max_first = layout->total > layout->rows
        ? layout->total - layout->rows : 0u;
    if (tm.first > max_first) {
        tm.first = max_first;
    }
}

static void fill_performance(char lines[][TM_LINE_LEN], uint32_t *count)
{
    struct book8gui_telemetry t;
    book8gui_get_telemetry(&t);
    uint32_t n = 0u;
    struct tm_buf b;
#define LINE() do { b = (struct tm_buf){lines[n], 0u, TM_LINE_LEN}; \
    lines[n][0] = '\0'; ++n; } while (0)
    LINE(); buf_str(&b, "CPU");
    LINE(); buf_str(&b, "  Name: ");
    buf_str(&b, t.sys_valid ? t.sys.cpu_name : "Unknown");
    LINE(); buf_str(&b, "  Utilization: "); buf_percent(&b, t.cpu_permille);
    LINE(); buf_str(&b, "  Speed: ");
    if (t.sys_valid && t.sys.cpu_mhz != 0u) {
        buf_u64(&b, t.sys.cpu_mhz, 1u);
        buf_str(&b, " MHz");
    } else {
        buf_str(&b, "Unknown");
    }
    LINE(); buf_str(&b, "  Logical processors: ");
    buf_u64(&b, t.sys_valid && t.sys.cpu_threads != 0u
        ? t.sys.cpu_threads : 1u, 1u);
    LINE(); buf_str(&b, "  Core 0: "); buf_percent(&b, t.cpu_permille);
    LINE(); buf_str(&b, "Memory");
    LINE(); buf_str(&b, "  In use: ");
    if (t.sys_valid && t.sys.memory_total_kb != 0u) {
        buf_memory(&b, t.sys.memory_used_kb);
        buf_str(&b, " / ");
        buf_memory(&b, t.sys.memory_total_kb);
        buf_str(&b, " - ");
        buf_u64(&b, t.sys.memory_used_kb * 100u / t.sys.memory_total_kb,
            1u);
        buf_str(&b, "%");
    } else {
        buf_str(&b, "Unknown");
    }
    LINE(); buf_str(&b, "Disk");
    LINE(); buf_str(&b, "  Utilization: ");
    buf_percent(&b, t.disk_busy_permille);
    LINE(); buf_str(&b, "  Read: "); buf_rate(&b, t.disk_read_bps);
    buf_str(&b, "  Write: "); buf_rate(&b, t.disk_write_bps);
    LINE(); buf_str(&b, "  Drive: ");
    if (t.sys_valid && t.sys.disk_count != 0u) {
        buf_str(&b, t.sys.disk_label);
        buf_str(&b, " (");
        buf_memory(&b, t.sys.disk_total_kb);
        buf_str(&b, ")");
    } else {
        buf_str(&b, "None detected");
    }
    LINE(); buf_str(&b, "System");
    LINE(); buf_str(&b, "  Up time: "); buf_clock(&b, t.uptime_seconds);
    LINE(); buf_str(&b, "  Processes: "); buf_u64(&b, t.task_count, 1u);
#undef LINE
    *count = n;
}

static void format_status(char *out, size_t size,
    const struct book8gui_window_info *list, uint32_t count,
    uint32_t thread_count)
{
    struct book8gui_telemetry t;
    book8gui_get_telemetry(&t);
    struct tm_buf b = {out, 0u, size};
    out[0] = '\0';
    uint64_t used = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        used += list[i].memory_kb;
    }
    buf_str(&b, "Tasks: ");
    buf_u64(&b, t.task_count, 1u);
    buf_str(&b, "  Memory: ");
    if (t.sys_valid && t.sys.memory_total_kb != 0u) {
        buf_memory(&b, t.sys.memory_used_kb);
        buf_str(&b, " / ");
        buf_memory(&b, t.sys.memory_total_kb);
    } else {
        buf_memory(&b, used);
        buf_str(&b, " allocated");
    }
    buf_str(&b, "  Threads: ");
    buf_u64(&b, thread_count, 1u);
    if (tm.message[0] != '\0') {
        buf_str(&b, "  ");
        buf_str(&b, tm.message);
    }
}

static void render_processes(const struct book8gui_platform *platform,
    const struct tm_layout *layout,
    const struct book8gui_window_info *list, uint32_t count,
    const struct book8gui_thread_info *threads, uint32_t thread_count)
{
    uint32_t hx = layout->frame.x + UI_PAD + 4u;
    uint32_t hy = layout->frame.y + UI_PAD;
    platform->fill_rect(layout->frame.x + 1u, hy + TM_ROW_H - 2u,
        layout->frame.width > UI_SCROLL_W + 4u
            ? layout->frame.width - UI_SCROLL_W - 4u : 0u, 1u, UI_SHADOW);
    draw_clipped(platform, hx + TM_COL_NAME, hy + 1u, "Name", 20u,
        layout->limit_x, UI_MUTED, UI_FIELD);
    draw_clipped(platform, hx + TM_COL_PID, hy + 1u, "PID", 5u,
        layout->limit_x, UI_MUTED, UI_FIELD);
    draw_clipped(platform, hx + TM_COL_CPU, hy + 1u, "CPU", 7u,
        layout->limit_x, UI_MUTED, UI_FIELD);
    draw_clipped(platform, hx + TM_COL_MEM, hy + 1u, "Memory", 10u,
        layout->limit_x, UI_MUTED, UI_FIELD);
    draw_clipped(platform, hx + TM_COL_DISK, hy + 1u, "Disk", 12u,
        layout->limit_x, UI_MUTED, UI_FIELD);
    uint32_t selected = selected_index(list, count);
    for (uint32_t r = 0u; r < layout->rows; ++r) {
        uint32_t index = tm.first + r;
        if (index >= count + thread_count) {
            break;
        }
        uint32_t y = layout->text_y + r * TM_ROW_H;
        bool on = index < count && index == selected;
        uint32_t bg = on ? UI_SELECTED : UI_FIELD;
        uint32_t fg = on ? UI_SELECTED_TEXT : UI_TEXT;
        if (on) {
            platform->fill_rect(layout->frame.x + 2u, y,
                layout->limit_x > layout->frame.x + 2u
                    ? layout->limit_x - layout->frame.x - 2u : 0u,
                TM_ROW_H, UI_SELECTED);
        }
        char cell[TM_LINE_LEN];
        struct tm_buf b = {cell, 0u, sizeof(cell)};
        cell[0] = '\0';
        const struct book8gui_window_info *w = index < count
            ? &list[index] : NULL;
        const struct book8gui_thread_info *thread = index >= count
            ? &threads[index - count] : NULL;
        buf_str(&b, w != NULL ? w->title : thread->name);
        if (w != NULL && w->minimized)
            buf_str(&b, " (min)");
        draw_clipped(platform, hx + TM_COL_NAME, y + 1u, cell, 20u,
            layout->limit_x, fg, bg);
        b.length = 0u; cell[0] = '\0';
        buf_u64(&b, w != NULL ? w->id : thread->tid, 1u);
        draw_clipped(platform, hx + TM_COL_PID, y + 1u, cell, 8u,
            layout->limit_x, fg, bg);
        b.length = 0u; cell[0] = '\0';
        if (w != NULL)
            buf_percent(&b, w->cpu_permille);
        else
            buf_str(&b, thread->state);
        draw_clipped(platform, hx + TM_COL_CPU, y + 1u, cell, 9u,
            layout->limit_x, fg, bg);
        b.length = 0u; cell[0] = '\0';
        if (w != NULL)
            buf_memory(&b, w->memory_kb);
        else
            buf_u64(&b, thread->cpu_ticks, 1u);
        draw_clipped(platform, hx + TM_COL_MEM, y + 1u, cell, 10u,
            layout->limit_x, fg, bg);
        b.length = 0u; cell[0] = '\0';
        if (w == NULL) {
            buf_str(&b, "ticks");
        } else if (w->disk_read_bps == 0u && w->disk_write_bps == 0u) {
            buf_str(&b, "Idle");
        } else {
            buf_rate(&b, (uint64_t)w->disk_read_bps + w->disk_write_bps);
        }
        draw_clipped(platform, hx + TM_COL_DISK, y + 1u, cell, 14u,
            layout->limit_x, fg, bg);
    }
}

static void render(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    struct tm_layout layout;
    struct book8gui_window_info list[TM_MAX_ROWS];
    struct book8gui_thread_info threads[TM_MAX_THREADS];
    char perf[TM_PERF_LINES][TM_LINE_LEN];
    uint32_t count = book8gui_window_snapshot(list, TM_MAX_ROWS);
    uint32_t thread_count = platform->thread_snapshot != NULL
        ? platform->thread_snapshot(threads, TM_MAX_THREADS) : 0u;
    if (thread_count > TM_MAX_THREADS)
        thread_count = TM_MAX_THREADS;
    uint32_t perf_count = 0u;
    compute_layout(app, &layout);
    bool processes = tm.tab == TM_TAB_PROCESSES;
    if (!processes) {
        fill_performance(perf, &perf_count);
    }
    layout.total = processes ? count + thread_count : perf_count;
    clamp_first(&layout);

    draw_tab(platform, &layout.tab1, "[ Processes ]", processes);
    draw_tab(platform, &layout.tab2, "[ Performance ]", !processes);
    ui_sunken(platform, &layout.frame, UI_FIELD);
    if (processes) {
        render_processes(platform, &layout, list, count, threads,
            thread_count);
        bool can_end = tm.has_selection && tm.selected_id != 0u
            && selected_index(list, count) < count;
        draw_button(platform, &layout.button, "End Task", can_end);
    } else {
        for (uint32_t r = 0u; r < layout.rows; ++r) {
            if (tm.first + r >= perf_count) {
                break;
            }
            const char *line = perf[tm.first + r];
            bool heading = line[0] != ' ';
            draw_clipped(platform, layout.text_x,
                layout.text_y + r * TM_ROW_H + 1u, line, TM_LINE_LEN,
                layout.limit_x, heading ? UI_SELECTED : UI_TEXT, UI_FIELD);
        }
    }
    ui_scrollbar(platform, &layout.track, layout.total, layout.rows,
        tm.first);

    char status[96];
    format_status(status, sizeof(status), list, count, thread_count);
    ui_sunken(platform, &layout.status, UI_FACE);
    draw_clipped(platform, layout.status.x + 6u, layout.status.y + 2u, status,
        TM_LINE_LEN, layout.status.x + layout.status.width - 6u, UI_TEXT,
        UI_FACE);
}

static void end_selected(const struct book8gui_window_info *list,
    uint32_t count)
{
    if (selected_index(list, count) >= count) {
        set_message("Select a task first.");
    } else if (tm.selected_id == 0u) {
        set_message("Desktop cannot be ended.");
    } else {
        book8gui_end_task(tm.selected_id);
        tm.has_selection = false;
        set_message("");
    }
    book8gui_invalidate();
}

static void move_selection(const struct tm_layout *layout,
    const struct book8gui_window_info *list, uint32_t count, int32_t delta)
{
    uint32_t index = selected_index(list, count);
    if (index >= count) {
        index = delta > 0 ? 0u : count - 1u;
    } else if (delta < 0 && index > 0u) {
        --index;
    } else if (delta > 0 && index + 1u < count) {
        ++index;
    }
    tm.has_selection = true;
    tm.selected_id = list[index].id;
    if (index < tm.first) {
        tm.first = index;
    } else if (layout->rows != 0u && index >= tm.first + layout->rows) {
        tm.first = index + 1u - layout->rows;
    }
    set_message("");
}

static bool handle_event(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event)
{
    (void)platform;
    struct tm_layout layout;
    struct book8gui_window_info list[TM_MAX_ROWS];
    struct book8gui_thread_info threads[TM_MAX_THREADS];
    uint32_t count = book8gui_window_snapshot(list, TM_MAX_ROWS);
    uint32_t thread_count = platform->thread_snapshot != NULL
        ? platform->thread_snapshot(threads, TM_MAX_THREADS) : 0u;
    if (thread_count > TM_MAX_THREADS)
        thread_count = TM_MAX_THREADS;
    compute_layout(app, &layout);
    bool processes = tm.tab == TM_TAB_PROCESSES;
    layout.total = processes ? count + thread_count : TM_PERF_LINES;
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        bool down = (event->mouse_buttons & 1u) != 0u;
        bool pressed = down && (tm.previous_buttons & 1u) == 0u;
        tm.previous_buttons = event->mouse_buttons;
        if (!down) {
            tm.scroll_drag = false;
        }
        if (event->wheel_delta != 0
            && event->mouse_x >= (int32_t)app->bounds.x
            && event->mouse_y >= (int32_t)app->bounds.y
            && (uint32_t)event->mouse_x < app->bounds.x + app->bounds.width
            && (uint32_t)event->mouse_y < app->bounds.y + app->bounds.height) {
            tm.first = ui_wheel_scroll_delta(tm.first, layout.total, layout.rows,
                event->wheel_delta, 1u, &tm.wheel_remainder);
            book8gui_invalidate();
        }
        if (pressed) {
            int32_t mx = event->mouse_x;
            int32_t my = event->mouse_y;
            if (ui_contains(&layout.tab1, mx, my)
                || ui_contains(&layout.tab2, mx, my)) {
                tm.tab = ui_contains(&layout.tab1, mx, my)
                    ? TM_TAB_PROCESSES : TM_TAB_PERFORMANCE;
                tm.first = 0u;
                set_message("");
                book8gui_invalidate();
            } else if (processes && ui_contains(&layout.button, mx, my)) {
                end_selected(list, count);
            } else if (ui_contains(&layout.track, mx, my)) {
                tm.scroll_drag = layout.total > layout.rows;
                tm.scroll_grab = ui_scrollbar_grab(&layout.track, layout.total,
                    layout.rows, tm.first, my);
            } else if (processes && ui_contains(&layout.frame, mx, my)
                && my >= (int32_t)layout.text_y) {
                uint32_t index = tm.first
                    + ((uint32_t)my - layout.text_y) / TM_ROW_H;
                if (index < count) {
                    tm.has_selection = true;
                    tm.selected_id = list[index].id;
                    set_message("");
                }
                book8gui_invalidate();
            }
        }
        if (down && tm.scroll_drag) {
            tm.first = ui_scrollbar_drag_position(&layout.track, layout.total,
                layout.rows, event->mouse_y, tm.scroll_grab);
            book8gui_invalidate();
        }
        return app->visible;
    }
    switch (event->key) {
    case BOOK8GUI_KEY_LEFT:
    case BOOK8GUI_KEY_RIGHT:
        tm.tab = event->key == BOOK8GUI_KEY_LEFT
            ? TM_TAB_PROCESSES : TM_TAB_PERFORMANCE;
        tm.first = 0u;
        set_message("");
        break;
    case BOOK8GUI_KEY_UP:
    case BOOK8GUI_KEY_DOWN:
        if (processes && count != 0u) {
            move_selection(&layout, list, count,
                event->key == BOOK8GUI_KEY_UP ? -1 : 1);
        } else {
            tm.first = ui_wheel_scroll(tm.first, layout.total, layout.rows,
                event->key == BOOK8GUI_KEY_UP ? 1 : -1, 1u);
        }
        break;
    case BOOK8GUI_KEY_PAGE_UP:
    case BOOK8GUI_KEY_PAGE_DOWN:
        tm.first = ui_wheel_scroll(tm.first, layout.total, layout.rows,
            event->key == BOOK8GUI_KEY_PAGE_UP ? 1 : -1,
            layout.rows != 0u ? layout.rows : 1u);
        break;
    case BOOK8GUI_KEY_DELETE:
        if (processes) {
            end_selected(list, count);
        }
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
    tm.tab = TM_TAB_PROCESSES;
    tm.has_selection = false;
    tm.selected_id = 0u;
    tm.first = 0u;
    tm.previous_buttons = 0u;
    tm.scroll_drag = false;
    tm.wheel_remainder = 0;
    tm.message[0] = '\0';
    return &tm;
}

const struct book8gui_app_class book8_taskmgr_class = {
    .name = "Task Manager",
    .default_width = 560u,
    .default_height = 360u,
    .single_instance = true,
    .refresh_1hz = true,
    .state_size = sizeof(struct tm_state),
    .create = create,
    .destroy = NULL,
    .render = render,
    .handle_event = handle_event
};
