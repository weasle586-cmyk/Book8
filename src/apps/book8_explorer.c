#include "book8_explorer.h"
#include "book8_utilities.h"
#include "../../include/syscall.h"

#include <stddef.h>
#include "../../include/stdio.h"
#include "../../include/string.h"

#include "book8_editor.h"
#include "book8_ui.h"
#include "../registry.h"
#define EXPLORER_MAX_FILES 128u
#define EXPLORER_NAME_SIZE 256u
#define EXPLORER_PATH_SIZE 512u
#define EXPLORER_CHAR_HEIGHT 16u
#define EXPLORER_ROW_HEIGHT 18u
#define EXPLORER_TEXT 0x000000u
#define EXPLORER_MUTED 0x404040u
#define EXPLORER_FIELD 0xffffffu
#define EXPLORER_SELECTED 0x000080u
#define EXPLORER_SELECTED_TEXT 0xffffffu
#define EXPLORER_DRIVE_WIDTH 36u
#define CONTEXT_ROW 18u
#define CONTEXT_ROWS 8u
#define CONTEXT_HEIGHT (CONTEXT_ROW * CONTEXT_ROWS + 4u)

struct explorer_file {
    char name[EXPLORER_NAME_SIZE];
    bool directory;
    uint32_t size;
};

struct explorer_state {
    bool in_use;
    struct explorer_file files[EXPLORER_MAX_FILES];
    uint32_t file_count;
    uint32_t selected_file;
    uint32_t file_scroll;
    uint32_t h_scroll;
    bool has_selection;
    bool last_click_valid;
    uint8_t previous_buttons;
    bool scroll_drag;
    int32_t scroll_grab;
    int32_t wheel_remainder[2];
    bool context_open;
    bool open_with;
    uint8_t action_dialog;
    char rename_name[EXPLORER_NAME_SIZE];
    bool properties_open;
    uint32_t properties_first;
    struct book8_file_info properties;
    char properties_path[EXPLORER_PATH_SIZE];
    uint32_t context_x, context_y;
    uint32_t last_click_file;
    uint64_t last_click_ticks;
    char last_click_path[EXPLORER_PATH_SIZE];
    char path[EXPLORER_PATH_SIZE];
    char status[EXPLORER_NAME_SIZE];
};

static struct explorer_state explorer_pool[BOOK8GUI_MAX_INSTANCES_PER_APP];
/* Helpers act on the instance selected at the start of each callback. */
static struct explorer_state *explorer_current;
#define explorer (*explorer_current)

static size_t string_length(const char *text)
{
    size_t length = 0u;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0u) {
        return;
    }
    size_t i = 0u;
    while (i + 1u < capacity && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static bool same_text(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool same_letter(char left, char right)
{
    if (left >= 'a' && left <= 'z') {
        left = (char)(left - 'a' + 'A');
    }
    if (right >= 'a' && right <= 'z') {
        right = (char)(right - 'a' + 'A');
    }
    return left == right;
}

/* Platforms without a drive registry expose a single A: drive. */
static uint32_t drive_total_count(const struct book8gui_platform *platform)
{
    return platform->drive_count != NULL ? platform->drive_count() : 1u;
}

static char drive_letter_at(const struct book8gui_platform *platform,
    uint32_t index)
{
    return platform->drive_letter != NULL
        ? platform->drive_letter(index) : 'A';
}

static bool collect_file(const char *name, bool directory, uint32_t size,
    void *context)
{
    struct explorer_state *state = context;
    if (state->file_count == EXPLORER_MAX_FILES) {
        return false;
    }
    struct explorer_file *file = &state->files[state->file_count++];
    copy_text(file->name, sizeof(file->name), name);
    file->directory = directory;
    file->size = size;
    return true;
}

static void set_status(const char *message)
{
    copy_text(explorer.status, sizeof(explorer.status), message);
    book8gui_invalidate();
}

static bool refresh(const struct book8gui_platform *platform)
{
    explorer.file_count = 0u;
    explorer.has_selection = false;
    explorer.file_scroll = 0u;
    explorer.h_scroll = 0u;
    explorer.last_click_valid = false;
    explorer.context_open = false;
    explorer.previous_buttons = 0u;
    if (!platform->list_directory(explorer.path, collect_file, &explorer)) {
        explorer.file_count = 0u;
        set_status(platform->last_error_text != NULL
            ? platform->last_error_text()
            : "Directory unavailable or unsupported.");
        return false;
    }
    explorer.status[0] = '\0';
    if (platform->diagnostic != NULL) {
        char message[EXPLORER_PATH_SIZE + 32u] =
            "BOOK8-EXPLORER: path=";
        size_t offset = sizeof("BOOK8-EXPLORER: path=") - 1u;
        for (size_t i = 0u; explorer.path[i] != '\0'; ++i) {
            message[offset++] = explorer.path[i];
        }
        message[offset++] = '\n';
        message[offset] = '\0';
        platform->diagnostic(message);
    }
    book8gui_invalidate();
    return true;
}

struct explorer_layout {
    struct book8gui_rect client;
    struct book8gui_rect path_field;
    struct book8gui_rect frame;
    struct book8gui_rect track;
    uint32_t drive_y;
    uint32_t path_y;
    uint32_t status_y;
    uint32_t list_x;
    uint32_t list_y;
    uint32_t list_width;
    uint32_t rows;
};

/* Shared by drawing and hit-testing so they cannot drift apart. */
static void compute_layout(const struct WindowApp *app,
    struct explorer_layout *layout)
{
    layout->client = ui_client(app);
    uint32_t bottom = app->bounds.y + app->bounds.height - 4u;
    layout->status_y = bottom - UI_STATUS_H;
    layout->drive_y = layout->client.y + 2u;
    layout->path_y = layout->client.y + 26u;
    uint32_t frame_y = layout->client.y + 50u;
    layout->path_field = (struct book8gui_rect){
        layout->client.x + 40u, layout->path_y,
        layout->client.width > 40u ? layout->client.width - 40u : 0u, 20u
    };
    layout->frame = (struct book8gui_rect){
        layout->client.x, frame_y, layout->client.width,
        layout->status_y > frame_y ? layout->status_y - frame_y : 0u
    };
    layout->track = ui_scroll_track(&layout->frame);
    layout->list_x = layout->frame.x + UI_PAD;
    layout->list_y = layout->frame.y + UI_PAD;
    layout->list_width = layout->frame.width > 2u * UI_PAD + UI_SCROLL_W
        ? layout->frame.width - 2u * UI_PAD - UI_SCROLL_W : 0u;
    layout->rows = layout->frame.height > 2u * UI_PAD
        ? (layout->frame.height - 2u * UI_PAD) / EXPLORER_ROW_HEIGHT : 0u;
}

static uint32_t name_columns(const struct explorer_layout *layout)
{
    return layout->list_width > 54u ? (layout->list_width - 54u) / 8u : 0u;
}

static uint32_t path_columns(const struct explorer_layout *layout)
{
    return layout->path_field.width > 64u
        ? (layout->path_field.width - 64u) / 8u : 0u;
}

static uint32_t max_h_scroll(const struct explorer_layout *layout)
{
    uint32_t max_scroll = 0u;
    uint32_t columns = name_columns(layout);
    for (uint32_t i = 0u; i < explorer.file_count; ++i) {
        uint32_t length = (uint32_t)string_length(explorer.files[i].name);
        if (length > columns && length - columns > max_scroll) {
            max_scroll = length - columns;
        }
    }
    uint32_t path_length = (uint32_t)string_length(explorer.path);
    columns = path_columns(layout);
    if (path_length > columns && path_length - columns > max_scroll) {
        max_scroll = path_length - columns;
    }
    return max_scroll;
}

static bool selected_path(char *path, size_t capacity)
{
    if (!explorer.has_selection || explorer.selected_file >= explorer.file_count)
        return false;
    size_t prefix = string_length(explorer.path);
    const char *name = explorer.files[explorer.selected_file].name;
    size_t length = string_length(name);
    if (prefix + length + 1u > capacity)
        return false;
    copy_text(path, capacity, explorer.path);
    for (size_t i = 0u; i <= length; ++i)
        path[prefix + i] = name[i];
    return true;
}

static void draw_clipped(const struct book8gui_platform *platform,
    uint32_t x, uint32_t y, const char *text, uint32_t scroll,
    uint32_t columns, uint32_t foreground, uint32_t background);

static void draw_properties(const struct book8gui_platform *platform,
    const struct explorer_layout *layout)
{
    const struct book8gui_rect *box = &layout->client;
    uint32_t columns = box->width > 20u ? (box->width - 20u) / 8u : 1u;
    if (columns > 120u)
        columns = 120u;
    uint32_t path_length = (uint32_t)string_length(explorer.properties_path);
    uint32_t path_rows = (path_length + columns - 1u) / columns;
    if (path_rows == 0u)
        path_rows = 1u;
    uint32_t total = path_rows + 7u;
    uint32_t rows = box->height > 48u ? (box->height - 48u) / 18u : 0u;
    if (explorer.properties_first > (total > rows ? total - rows : 0u))
        explorer.properties_first = total > rows ? total - rows : 0u;
    platform->fill_rect(box->x, box->y, box->width, box->height, UI_FACE);
    platform->draw_text(box->x + 5u, box->y + 3u, "Properties (Esc closes)",
        UI_TEXT, UI_FACE);
    platform->draw_text(box->x + box->width - 30u,
        box->y + box->height - 20u, "OK", UI_TEXT, UI_FACE);
    for (uint32_t row = 0u; row < rows; ++row) {
        uint32_t index = row + explorer.properties_first;
        if (index >= total)
            break;
        char line[160];
        if (index < path_rows) {
            uint32_t offset = index * columns;
            uint32_t count = 0u;
            while (count < columns && explorer.properties_path[offset + count] != '\0') {
                line[count] = explorer.properties_path[offset + count];
                ++count;
            }
            line[count] = '\0';
        } else {
            uint32_t field = index - path_rows;
            const struct book8_file_info *info = &explorer.properties;
            const char *name = explorer.files[explorer.selected_file].name;
            const char *extension = "";
            for (const char *p = name; *p != '\0'; ++p)
                if (*p == '.')
                    extension = p;
            switch (field) {
            case 0u:
                snprintf(line, sizeof(line), "Type: %s", info->directory
                    ? "Folder" : (*extension != '\0' ? extension : "(none)"));
                break;
            case 1u:
                snprintf(line, sizeof(line), "Size: %llu bytes",
                    (unsigned long long)info->size);
                break;
            case 2u:
                if (info->allocation_valid)
                    snprintf(line, sizeof(line), "On disk: %llu bytes",
                        (unsigned long long)info->allocated_bytes);
                else
                    copy_text(line, sizeof(line), "On disk: unavailable");
                break;
            case 3u:
                if (info->creation_valid)
                    snprintf(line, sizeof(line), "Created: %04u-%02u-%02u %02u:%02u:%02u",
                        info->year, info->month, info->day, info->hour,
                        info->minute, info->second);
                else
                    copy_text(line, sizeof(line), "Created: unavailable");
                break;
            case 4u:
                snprintf(line, sizeof(line), "Time basis: %s", info->creation_valid
                    ? (info->creation_utc ? "UTC" : "FAT local") : "unavailable");
                break;
            case 5u:
                snprintf(line, sizeof(line), "Read-only: %s",
                    info->read_only ? "Yes" : "No");
                break;
            default:
                snprintf(line, sizeof(line), "System: %s", info->system ? "Yes" : "No");
                break;
            }
        }
        draw_clipped(platform, box->x + 5u, box->y + 24u + row * 18u,
            line, 0u, columns, UI_TEXT, UI_FACE);
    }
}

/* Draws the window of text starting at scroll, marking clipped text with '>'. */
static void draw_clipped(const struct book8gui_platform *platform,
    uint32_t x, uint32_t y, const char *text, uint32_t scroll,
    uint32_t columns, uint32_t foreground, uint32_t background)
{
    char clipped[EXPLORER_PATH_SIZE];
    uint32_t length = (uint32_t)string_length(text);
    uint32_t count = 0u;
    if (columns >= sizeof(clipped)) {
        columns = sizeof(clipped) - 1u;
    }
    while (scroll + count < length && count < columns) {
        clipped[count] = text[scroll + count];
        ++count;
    }
    if (scroll + count < length && count > 0u) {
        clipped[count - 1u] = '>';
    }
    clipped[count] = '\0';
    platform->draw_text(x, y, clipped, foreground, background);
}

static void render(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    explorer_current = app->state;
    struct explorer_layout layout;
    compute_layout(app, &layout);
    uint32_t left = layout.client.x;
    uint32_t name_chars = name_columns(&layout);
    uint32_t max_file_scroll = explorer.file_count > layout.rows
        ? explorer.file_count - layout.rows : 0u;
    if (explorer.file_scroll > max_file_scroll) {
        explorer.file_scroll = max_file_scroll;
    }
    uint32_t max_scroll = max_h_scroll(&layout);
    if (explorer.h_scroll > max_scroll) {
        explorer.h_scroll = max_scroll;
    }

    uint32_t drives = drive_total_count(platform);
    for (uint32_t i = 0u; i < drives; ++i) {
        char label[3] = {drive_letter_at(platform, i), ':', '\0'};
        bool current = same_letter(label[0], explorer.path[0]);
        uint32_t background = current ? EXPLORER_SELECTED : EXPLORER_FIELD;
        platform->fill_rect(left + i * EXPLORER_DRIVE_WIDTH,
            layout.drive_y, EXPLORER_DRIVE_WIDTH - 2u, 20u, background);
        platform->draw_text(left + i * EXPLORER_DRIVE_WIDTH + 6u,
            layout.drive_y + 2u, label,
            current ? EXPLORER_SELECTED_TEXT : EXPLORER_TEXT, background);
    }

    platform->draw_text(left + 2u, layout.path_y + 2u, "[UP]", EXPLORER_TEXT,
        UI_FACE);
    ui_sunken(platform, &layout.path_field, EXPLORER_FIELD);
    draw_clipped(platform, layout.path_field.x + 4u, layout.path_y + 2u,
        explorer.path, explorer.h_scroll, path_columns(&layout),
        EXPLORER_TEXT, EXPLORER_FIELD);
    if (max_scroll != 0u) {
        platform->draw_text(layout.path_field.x + layout.path_field.width
            - 52u, layout.path_y + 2u, "[<][>]", EXPLORER_TEXT,
            EXPLORER_FIELD);
    }

    ui_sunken(platform, &layout.frame, EXPLORER_FIELD);
    for (uint32_t row = 0u; row < layout.rows; ++row) {
        uint32_t index = explorer.file_scroll + row;
        uint32_t row_y = layout.list_y + row * EXPLORER_ROW_HEIGHT;
        if (index >= explorer.file_count) {
            break;
        }
        bool selected = explorer.has_selection
            && explorer.selected_file == index;
        uint32_t background = selected ? EXPLORER_SELECTED : EXPLORER_FIELD;
        uint32_t foreground = selected
            ? EXPLORER_SELECTED_TEXT : EXPLORER_TEXT;
        platform->fill_rect(layout.list_x, row_y, layout.list_width,
            EXPLORER_ROW_HEIGHT, background);
        draw_clipped(platform, layout.list_x + 3u, row_y + 1u,
            explorer.files[index].name, explorer.h_scroll, name_chars,
            foreground, background);
        if (explorer.files[index].directory && layout.list_width > 48u) {
            platform->draw_text(layout.list_x + layout.list_width - 44u,
                row_y + 1u, "<DIR>", foreground, background);
        }
    }
    ui_scrollbar(platform, &layout.track, explorer.file_count, layout.rows,
        explorer.file_scroll);

    struct book8gui_rect status = {
        layout.client.x, layout.status_y, layout.client.width, UI_STATUS_H
    };
    ui_sunken(platform, &status, UI_FACE);
    draw_clipped(platform, status.x + 6u, status.y + 2u,
        explorer.status[0] != '\0' ? explorer.status
            : "Single click selects; double-click opens a directory.",
        0u, status.width > 12u ? (status.width - 12u) / 8u : 0u,
        EXPLORER_MUTED, UI_FACE);
    if (explorer.context_open) {
        static const char *const labels[] = {
            "Open", "Delete", "Rename", "Properties", "Cut", "Copy",
            "Paste", "Open With >"
        };
        platform->fill_rect(explorer.context_x, explorer.context_y,
            180u, CONTEXT_HEIGHT, UI_SHADOW);
        platform->fill_rect(explorer.context_x + 1u, explorer.context_y + 1u,
            178u, CONTEXT_HEIGHT - 2u, UI_FACE);
        for (uint32_t i = 0u; i < CONTEXT_ROWS; ++i) {
            const struct book8gui_app_class *cls = explorer.open_with
                ? book8gui_file_app(i) : NULL;
            const char *label = explorer.open_with
                ? (cls != NULL ? cls->name : "Back") : labels[i];
            platform->draw_text(explorer.context_x + 5u,
                explorer.context_y + 3u + i * CONTEXT_ROW,
                label, UI_TEXT, UI_FACE);
            if (explorer.open_with && cls == NULL)
                break;
        }
    }
    if (explorer.properties_open)
        draw_properties(platform, &layout);
    if (explorer.action_dialog != 0u) {
        platform->fill_rect(layout.client.x, layout.client.y,
            layout.client.width, layout.client.height, UI_FACE);
        platform->draw_text(layout.client.x + 5u, layout.client.y + 4u,
            explorer.action_dialog == 1u ? "Rename (8.3 name):" : "Delete item and contents?",
            UI_TEXT, UI_FACE);
        if (explorer.action_dialog == 1u)
            draw_clipped(platform, layout.client.x + 5u, layout.client.y + 28u,
                explorer.rename_name, 0u, (layout.client.width - 10u) / 8u,
                UI_TEXT, UI_FACE);
        platform->draw_text(layout.client.x + 5u, layout.client.y + 52u,
            "Enter confirms; Esc cancels", UI_TEXT, UI_FACE);
        platform->draw_text(layout.client.x + 5u, layout.client.y + 80u,
            "[ OK ]       [ Cancel ]", UI_TEXT, UI_FACE);
    }
}

static bool parent_directory(void)
{
    size_t length = string_length(explorer.path);
    while (length > 3u && (explorer.path[length - 1u] == '\\'
            || explorer.path[length - 1u] == '/')) {
        explorer.path[--length] = '\0';
    }
    while (length > 3u && explorer.path[length - 1u] != '\\'
        && explorer.path[length - 1u] != '/') {
        explorer.path[--length] = '\0';
    }
    if (length < 3u) {
        copy_text(explorer.path, sizeof(explorer.path), "A:\\");
    } else {
        explorer.path[length] = '\0';
    }
    return true;
}

static bool child_path(const char *name)
{
    size_t path_length = string_length(explorer.path);
    size_t name_length = string_length(name);
    bool needs_separator = path_length != 0u
        && explorer.path[path_length - 1u] != '\\'
        && explorer.path[path_length - 1u] != '/';
    if (path_length + (needs_separator ? 1u : 0u) + name_length + 2u
        > sizeof(explorer.path)) {
        return false;
    }
    if (needs_separator) {
        explorer.path[path_length++] = '\\';
    }
    for (size_t i = 0u; i < name_length; ++i) {
        explorer.path[path_length++] = name[i];
    }
    explorer.path[path_length++] = '\\';
    explorer.path[path_length] = '\0';
    return true;
}

static bool enter_directory(const struct book8gui_platform *platform,
    const char *name)
{
    char previous_path[EXPLORER_PATH_SIZE];
    copy_text(previous_path, sizeof(previous_path), explorer.path);
    if (!child_path(name)) {
        set_status("Directory path is too long.");
        return false;
    }
    if (!refresh(platform)) {
        const char *reason = platform->last_error_text != NULL
            ? platform->last_error_text()
            : "Directory unavailable or unsupported.";
        char message[EXPLORER_NAME_SIZE];
        copy_text(message, sizeof(message), reason);
        copy_text(explorer.path, sizeof(explorer.path), previous_path);
        (void)refresh(platform);
        set_status(message);
        return false;
    }
    return true;
}

static uint32_t visible_row_count(const struct WindowApp *app)
{
    struct explorer_layout layout;
    compute_layout(app, &layout);
    return layout.rows;
}

static bool open_file_with(const struct book8gui_platform *platform,
    const struct book8gui_app_class *cls)
{
    if (!explorer.has_selection
        || explorer.selected_file >= explorer.file_count
        || explorer.files[explorer.selected_file].directory) {
        return false;
    }
    char path[EXPLORER_PATH_SIZE];
    if (!selected_path(path, sizeof(path))) {
        set_status("Selected filename is too long.");
        return false;
    }
    if (cls == &book8_codeedit_class || cls == &book8_hexview_class) {
        bool ok = book8gui_open_utility(cls == &book8_codeedit_class
            ? BOOK8_GUI_CODEEDIT : BOOK8_GUI_HEXVIEW, path);
        if (!ok) { set_status("Utility launch failed; check installation and file path."); }
        return ok;
    }
    if (cls->open_path == NULL) {
        set_status("Associated application cannot open this file type.");
        return false;
    }
    struct WindowApp *target = book8gui_launch_class(cls);
    if (target == NULL) {
        set_status("Unable to launch application.");
        return false;
    }
    return cls->open_path(platform, target, path);
}

static bool open_file(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    if (explorer.has_selection
        && explorer.selected_file < explorer.file_count) {
        const char *name = explorer.files[explorer.selected_file].name;
        const char *dot = NULL;
        for (uint32_t i = 0u; name[i] != '\0'; ++i) {
            if (name[i] == '.') { dot = name + i; }
        }
        if (dot != NULL) {
            char extension[BOOK8_REGISTRY_NAME_MAX];
            uint32_t i = 0u;
            while (dot[i] != '\0' && i + 1u < sizeof(extension)) {
                char ch = dot[i];
                extension[i] = ch >= 'a' && ch <= 'z'
                    ? (char)(ch - ('a' - 'A')) : ch;
                ++i;
            }
            extension[i] = '\0';
            char app_class[BOOK8_REGISTRY_VALUE_MAX];
            if (reg_get_str("HKLM\\SOFTWARE\\BOOK8\\FileAssoc", extension,
                    app_class, sizeof(app_class))) {
                const struct book8gui_app_class *cls =
                    book8gui_app_class_by_name(app_class);
                if (cls != NULL) {
                    return open_file_with(platform, cls);
                }
            }
        }
    }
    explorer.context_x = app->bounds.x + 8u;
    explorer.context_y = app->bounds.y + 36u;
    explorer.context_open = true;
    explorer.open_with = true;
    set_status("No usable file association; choose Open With.");
    book8gui_invalidate();
    return false;
}

static bool handle_event(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event)
{
    explorer_current = app->state;
    if (explorer.action_dialog != 0u) {
        struct book8gui_event translated;
        if (event->type == BOOK8GUI_EVENT_MOUSE) {
            bool click = (event->mouse_buttons & 1u) != 0u
                && (explorer.previous_buttons & 1u) == 0u;
            explorer.previous_buttons = event->mouse_buttons;
            struct explorer_layout layout;
            compute_layout(app, &layout);
            int32_t x = event->mouse_x - (int32_t)layout.client.x;
            int32_t y = event->mouse_y - (int32_t)layout.client.y;
            if (!click || y < 78 || y >= 98
                || !((x >= 5 && x < 53) || (x >= 109 && x < 189)))
                return true;
            translated = *event;
            translated.type = BOOK8GUI_EVENT_KEY;
            translated.key = x < 53 ? BOOK8GUI_KEY_ENTER : BOOK8GUI_KEY_ESCAPE;
            event = &translated;
        }
        if (event->key == BOOK8GUI_KEY_ESCAPE) {
            explorer.action_dialog = 0u;
            book8gui_set_modal(app, false);
        } else if (event->key == BOOK8GUI_KEY_ENTER) {
            char path[EXPLORER_PATH_SIZE];
            bool rename = explorer.action_dialog == 1u;
            bool available = rename ? platform->rename_entry != NULL
                : platform->delete_entry != NULL;
            bool ok = false;
            if (available && selected_path(path, sizeof(path)))
                ok = rename ? platform->rename_entry(path, explorer.rename_name)
                    : platform->delete_entry(path);
            explorer.action_dialog = 0u;
            book8gui_set_modal(app, false);
            if (ok) {
                (void)refresh(platform);
                set_status(rename ? "Renamed." : "Deleted.");
            } else {
                set_status(available && platform->last_error_text != NULL
                    ? platform->last_error_text() : "Operation unavailable.");
            }
        } else if (explorer.action_dialog == 1u) {
            size_t len = string_length(explorer.rename_name);
            if (event->key == BOOK8GUI_KEY_BACKSPACE && len != 0u)
                explorer.rename_name[len - 1u] = '\0';
            else if (event->key >= 32u && event->key < 127u
                && len + 1u < sizeof(explorer.rename_name)) {
                explorer.rename_name[len] = (char)event->key;
                explorer.rename_name[len + 1u] = '\0';
            }
        }
        book8gui_invalidate();
        return true;
    }
    if (explorer.properties_open) {
        struct explorer_layout layout;
        compute_layout(app, &layout);
        bool click = event->type == BOOK8GUI_EVENT_MOUSE
            && (event->mouse_buttons & 1u) != 0u
            && (explorer.previous_buttons & 1u) == 0u;
        if (event->type == BOOK8GUI_EVENT_MOUSE)
            explorer.previous_buttons = event->mouse_buttons;
        if (event->type == BOOK8GUI_EVENT_KEY
            && (event->key == BOOK8GUI_KEY_ESCAPE || event->key == BOOK8GUI_KEY_ENTER)) {
            explorer.properties_open = false;
            book8gui_set_modal(app, false);
        } else if (click && event->mouse_x >= (int32_t)(layout.client.x
                + layout.client.width - 44u)
            && event->mouse_y >= (int32_t)(layout.client.y
                + layout.client.height - 24u)
            && ui_contains(&layout.client, event->mouse_x, event->mouse_y)) {
            explorer.properties_open = false;
            book8gui_set_modal(app, false);
        } else if (event->type == BOOK8GUI_EVENT_MOUSE && event->wheel_delta != 0) {
            explorer.properties_first = ui_wheel_scroll_delta(explorer.properties_first,
                UINT32_MAX, 1u, event->wheel_delta, 1u, &explorer.wheel_remainder[1]);
        } else if (event->type == BOOK8GUI_EVENT_KEY && event->key == BOOK8GUI_KEY_DOWN) {
            ++explorer.properties_first;
        } else if ((event->type == BOOK8GUI_EVENT_KEY && event->key == BOOK8GUI_KEY_UP)
            && explorer.properties_first != 0u) {
            --explorer.properties_first;
        }
        book8gui_invalidate();
        return true;
    }
    if (event->type != BOOK8GUI_EVENT_MOUSE) {
        if (event->key == BOOK8GUI_KEY_ESCAPE && explorer.context_open) {
            explorer.context_open = false;
            book8gui_invalidate();
            return true;
        }
        if (event->key == BOOK8GUI_KEY_UP
            && explorer.has_selection && explorer.selected_file > 0u) {
            --explorer.selected_file;
            if (explorer.selected_file < explorer.file_scroll) {
                explorer.file_scroll = explorer.selected_file;
            }
            book8gui_invalidate();
        } else if (event->key == BOOK8GUI_KEY_DOWN
            && explorer.selected_file + 1u < explorer.file_count) {
            ++explorer.selected_file;
            explorer.has_selection = true;
            uint32_t visible = visible_row_count(app);
            if (explorer.selected_file >= explorer.file_scroll + visible) {
                explorer.file_scroll = explorer.selected_file - visible + 1u;
            }
            book8gui_invalidate();
        } else if (event->key == BOOK8GUI_KEY_ENTER) {
            if (explorer.has_selection
                && explorer.selected_file < explorer.file_count
                && explorer.files[explorer.selected_file].directory) {
                (void)enter_directory(platform,
                    explorer.files[explorer.selected_file].name);
            } else {
                (void)open_file(platform, app);
            }
        }
        return true;
    }

    if (event->wheel_delta != 0
        && event->mouse_x >= (int32_t)app->bounds.x
        && event->mouse_y >= (int32_t)app->bounds.y
        && (uint32_t)event->mouse_x < app->bounds.x + app->bounds.width
        && (uint32_t)event->mouse_y < app->bounds.y + app->bounds.height) {
        explorer.file_scroll = ui_wheel_scroll_delta(explorer.file_scroll,
            explorer.file_count, visible_row_count(app), event->wheel_delta,
            1u, &explorer.wheel_remainder[0]);
        book8gui_invalidate();
    }

    struct explorer_layout layout;
    compute_layout(app, &layout);
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool pressed = down && (explorer.previous_buttons & 1u) == 0u;
    bool right_pressed = (event->mouse_buttons & 2u) != 0u
        && (explorer.previous_buttons & 2u) == 0u;
    explorer.previous_buttons = event->mouse_buttons;
    if (right_pressed) {
        explorer.context_open = false;
        if (event->mouse_x >= (int32_t)layout.list_x
            && event->mouse_x < (int32_t)(layout.list_x + layout.list_width)
            && event->mouse_y >= (int32_t)layout.list_y
            && event->mouse_y < (int32_t)(layout.list_y
                + layout.rows * EXPLORER_ROW_HEIGHT)) {
            uint32_t index = explorer.file_scroll
                + ((uint32_t)event->mouse_y - layout.list_y) / EXPLORER_ROW_HEIGHT;
            if (index < explorer.file_count) {
                explorer.selected_file = index;
                explorer.has_selection = true;
                explorer.last_click_valid = false;
            } else {
                explorer.has_selection = false;
            }
            {
                uint32_t right = app->bounds.x + app->bounds.width - 4u;
                uint32_t bottom = app->bounds.y + app->bounds.height - 4u;
                explorer.context_x = (uint32_t)event->mouse_x + 180u <= right
                    ? (uint32_t)event->mouse_x : right - 180u;
                explorer.context_y = (uint32_t)event->mouse_y + CONTEXT_HEIGHT <= bottom
                    ? (uint32_t)event->mouse_y : bottom - CONTEXT_HEIGHT;
                explorer.context_open = true;
                explorer.open_with = false;
            }
        }
        book8gui_invalidate();
        return true;
    }
    if (pressed && explorer.context_open) {
        bool inside_menu = event->mouse_x >= (int32_t)explorer.context_x
            && event->mouse_x < (int32_t)(explorer.context_x + 180u)
            && event->mouse_y > (int32_t)explorer.context_y
            && event->mouse_y < (int32_t)(explorer.context_y + CONTEXT_HEIGHT);
        uint32_t row = inside_menu
            ? ((uint32_t)event->mouse_y - explorer.context_y - 1u) / CONTEXT_ROW
            : CONTEXT_ROWS;
        const struct book8gui_app_class *chosen = explorer.open_with
            && row < CONTEXT_ROWS ? book8gui_file_app(row) : NULL;
        bool open = !explorer.open_with && row == 0u;
        bool properties = !explorer.open_with && row == 3u && explorer.has_selection;
        if (explorer.open_with && chosen == NULL && row < CONTEXT_ROWS) {
            explorer.open_with = false;
            book8gui_invalidate();
            return true;
        }
        if (!explorer.open_with && row == 7u && explorer.has_selection
            && !explorer.files[explorer.selected_file].directory) {
            explorer.open_with = true;
            book8gui_invalidate();
            return true;
        }
        explorer.context_open = false;
        if (chosen != NULL && explorer.has_selection)
            (void)open_file_with(platform, chosen);
        if (open && explorer.has_selection) {
            if (explorer.files[explorer.selected_file].directory)
                (void)enter_directory(platform,
                    explorer.files[explorer.selected_file].name);
            else
                (void)open_file(platform, app);
        }
        if (properties && selected_path(explorer.properties_path,
                sizeof(explorer.properties_path))) {
            if (platform->file_info == NULL)
                set_status("File properties are unavailable.");
            else if (!platform->file_info(explorer.properties_path, &explorer.properties))
                set_status(platform->last_error_text != NULL
                    ? platform->last_error_text() : "Cannot read file properties.");
            else {
                explorer.properties_first = 0u;
                explorer.properties_open = true;
                book8gui_set_modal(app, true);
            }
        }
        if (!explorer.open_with && explorer.has_selection && (row == 1u || row == 2u)) {
            explorer.action_dialog = row == 2u ? 1u : 2u;
            book8gui_set_modal(app, true);
            copy_text(explorer.rename_name, sizeof(explorer.rename_name),
                explorer.files[explorer.selected_file].name);
        } else if (!explorer.open_with && (row == 4u || row == 5u)
            && explorer.has_selection) {
            char path[EXPLORER_PATH_SIZE];
            if (selected_path(path, sizeof(path))
                && book8gui_clipboard_set(path, row == 4u))
                set_status(book8gui_clipboard_status());
            else
                set_status("Unable to select clipboard item.");
        } else if (!explorer.open_with && row == 6u) {
            bool ok = book8gui_clipboard_paste(explorer.path);
            if (ok)
                (void)refresh(platform);
            set_status(book8gui_clipboard_status());
        }
        explorer.open_with = false;
        book8gui_invalidate();
        return true;
    }
    if (!down) {
        explorer.scroll_drag = false;
    }
    if (down && explorer.scroll_drag) {
        explorer.file_scroll = ui_scrollbar_drag_position(&layout.track,
            explorer.file_count, layout.rows, event->mouse_y, explorer.scroll_grab);
        book8gui_invalidate();
        return true;
    }
    if (!pressed) {
        return true;
    }
    if (ui_contains(&layout.track, event->mouse_x, event->mouse_y)) {
        if (explorer.file_count > layout.rows) {
            explorer.scroll_drag = true;
            explorer.scroll_grab = ui_scrollbar_grab(&layout.track,
                explorer.file_count, layout.rows, explorer.file_scroll, event->mouse_y);
            explorer.file_scroll = ui_scrollbar_drag_position(&layout.track,
                explorer.file_count, layout.rows, event->mouse_y, explorer.scroll_grab);
            book8gui_invalidate();
        }
        return true;
    }

    uint32_t left = layout.client.x;
    uint32_t drive_y = layout.drive_y;
    uint32_t path_y = layout.path_y;
    uint32_t list_y = layout.list_y;
    uint32_t visible = layout.rows;
    uint32_t drives = drive_total_count(platform);
    if (event->mouse_y >= (int32_t)drive_y
        && event->mouse_y < (int32_t)(drive_y + 20u)
        && event->mouse_x >= (int32_t)left
        && (uint32_t)event->mouse_x - left < drives * EXPLORER_DRIVE_WIDTH) {
        uint32_t index = ((uint32_t)event->mouse_x - left)
            / EXPLORER_DRIVE_WIDTH;
        explorer.path[0] = drive_letter_at(platform, index);
        explorer.path[1] = ':';
        explorer.path[2] = '\\';
        explorer.path[3] = '\0';
        (void)refresh(platform);
        return true;
    }
    if (event->mouse_x >= (int32_t)left
        && event->mouse_x < (int32_t)(left + 36u)
        && event->mouse_y >= (int32_t)path_y
        && event->mouse_y < (int32_t)(path_y + 22u)) {
        parent_directory();
        (void)refresh(platform);
        return true;
    }
    uint32_t scroll_left = layout.path_field.x + layout.path_field.width
        - 52u;
    if (event->mouse_x >= (int32_t)scroll_left
        && event->mouse_x < (int32_t)(scroll_left + 48u)
        && event->mouse_y >= (int32_t)path_y
        && event->mouse_y < (int32_t)(path_y + 22u)) {
        uint32_t max_scroll = max_h_scroll(&layout);
        uint32_t step = 8u;
        if (max_scroll == 0u) {
            return true;
        }
        if (event->mouse_x < (int32_t)(scroll_left + 24u)) {
            explorer.h_scroll = explorer.h_scroll > step
                ? explorer.h_scroll - step : 0u;
        } else {
            explorer.h_scroll = explorer.h_scroll + step < max_scroll
                ? explorer.h_scroll + step : max_scroll;
        }
        book8gui_invalidate();
        return true;
    }

    if (event->mouse_x < (int32_t)layout.list_x
        || event->mouse_x >= (int32_t)(layout.list_x + layout.list_width)
        || event->mouse_y < (int32_t)list_y
        || event->mouse_y >= (int32_t)(list_y
            + visible * EXPLORER_ROW_HEIGHT)) {
        return true;
    }

    uint32_t index = explorer.file_scroll
        + ((uint32_t)event->mouse_y - list_y) / EXPLORER_ROW_HEIGHT;
    if (index >= explorer.file_count) {
        return true;
    }
    uint64_t now = platform->clock_ticks != NULL
        ? platform->clock_ticks() : 0u;
    uint64_t threshold = (uint64_t)platform->clock_hz
        * book8_config_value(BOOK8_CFG_DOUBLE_CLICK) / 1000u;
    bool double_click = platform->clock_ticks != NULL
        && explorer.last_click_valid
        && explorer.last_click_file == index
        && same_text(explorer.last_click_path, explorer.path)
        && now >= explorer.last_click_ticks
        && now - explorer.last_click_ticks <= threshold;
    explorer.selected_file = index;
    explorer.has_selection = true;
    explorer.last_click_valid = true;
    explorer.last_click_file = index;
    explorer.last_click_ticks = now;
    copy_text(explorer.last_click_path, sizeof(explorer.last_click_path),
        explorer.path);
    set_status("");

    if (double_click) {
        explorer.last_click_valid = false;
        if (explorer.files[index].directory) {
            char child_name[EXPLORER_NAME_SIZE];
            copy_text(child_name, sizeof(child_name), explorer.files[index].name);
            (void)enter_directory(platform, child_name);
        } else {
            (void)open_file(platform, app);
        }
    }
    book8gui_invalidate();
    return true;
}

static void *create(const struct book8gui_platform *platform)
{
    for (uint32_t i = 0u; i < BOOK8GUI_MAX_INSTANCES_PER_APP; ++i) {
        if (explorer_pool[i].in_use) {
            continue;
        }
        explorer_current = &explorer_pool[i];
        *explorer_current = (struct explorer_state){0};
        explorer.in_use = true;
        explorer.file_count = 0u;
        explorer.selected_file = 0u;
        explorer.file_scroll = 0u;
        explorer.has_selection = false;
        explorer.last_click_valid = false;
        explorer.previous_buttons = 0u;
        explorer.properties_open = false;
        explorer.action_dialog = 0u;
        explorer.open_with = false;
        copy_text(explorer.path, sizeof(explorer.path), "A:\\");
        explorer.status[0] = '\0';
        (void)refresh(platform);
        return explorer_current;
    }
    return NULL;
}

static void destroy(void *state)
{
    struct explorer_state *instance = state;
    if (instance != NULL) {
        instance->in_use = false;
    }

}

bool book8_explorer_open_directory(const struct book8gui_platform *platform,
    struct WindowApp *app, const char *path)
{
    explorer_current = app->state;
    if (path == NULL || strlen(path) >= sizeof(explorer.path)) {
        set_status("Directory path is too long."); return false;
    }
    copy_text(explorer.path, sizeof(explorer.path), path);
    explorer.file_scroll = 0u; explorer.has_selection = false;
    return refresh(platform);
}

const struct book8gui_app_class book8_explorer_class = {
    .name = "File Explorer",
    .default_width = 300u,
    .default_height = 300u,
    .single_instance = false,
    .state_size = sizeof(struct explorer_state),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event
};
