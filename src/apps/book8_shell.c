#include "book8_shell.h"

#include <stddef.h>

#include "book8_ui.h"

#define SHELL_COMMAND_SIZE 160u
#define SHELL_OUTPUT_SIZE 4096u
#define SHELL_CHAR_WIDTH 8u
#define SHELL_CHAR_HEIGHT 16u
#define SHELL_INPUT_HEIGHT 22u
#define SHELL_TEXT 0x000000u
#define SHELL_FIELD 0xffffffu
#define SHELL_MUTED 0x404040u

struct shell_state {
    bool in_use;
    char command[SHELL_COMMAND_SIZE];
    uint32_t command_length;
    char output[SHELL_OUTPUT_SIZE];
    uint32_t output_length;
    uint32_t output_scroll;
    uint8_t previous_buttons;
    bool scroll_drag;
    int32_t scroll_grab;
    int32_t wheel_remainder;
};

static struct shell_state shell_pool[BOOK8GUI_MAX_INSTANCES_PER_APP];
/* Helpers act on the instance selected at the start of each callback. */
static struct shell_state *shell_current;
/* Instance whose command is running; receives kernel console output. */
static struct shell_state *shell_console;
#define shell (*shell_current)

static void append_output(char value)
{
    if (shell.output_length + 1u >= sizeof(shell.output)) {
        uint32_t discard = SHELL_OUTPUT_SIZE / 4u;
        while (discard < shell.output_length
            && shell.output[discard] != '\n') {
            ++discard;
        }
        if (discard < shell.output_length) {
            ++discard;
        }
        uint32_t remaining = shell.output_length - discard;
        for (uint32_t i = 0u; i < remaining; ++i) {
            shell.output[i] = shell.output[discard + i];
        }
        shell.output_length = remaining;
    }
    shell.output[shell.output_length++] = value;
    shell.output[shell.output_length] = '\0';
    book8gui_invalidate();
}

static void append_text(const char *text)
{
    while (*text != '\0') {
        append_output(*text++);
    }
}

static struct shell_state *console_target(void)
{
    if (shell_console != NULL) {
        return shell_console;
    }
    for (uint32_t i = 0u; i < BOOK8GUI_MAX_INSTANCES_PER_APP; ++i) {
        if (shell_pool[i].in_use) {
            return &shell_pool[i];
        }
    }
    return NULL;
}

static void clear_output(void)
{
    shell.output_length = 0u;
    shell.output[0] = '\0';
    shell.output_scroll = 0u;
    book8gui_invalidate();
}

void book8_shell_append_char(char value)
{
    struct shell_state *saved = shell_current;
    shell_current = console_target();
    if (shell_current != NULL) {
        append_output(value);
    }
    shell_current = saved;
}

void book8_shell_clear(void)
{
    struct shell_state *saved = shell_current;
    shell_current = console_target();
    if (shell_current != NULL) {
        clear_output();
    }
    shell_current = saved;
}

struct shell_layout {
    struct book8gui_rect frame;
    struct book8gui_rect track;
    struct book8gui_rect input;
    uint32_t text_x;
    uint32_t text_y;
    uint32_t columns;
    uint32_t rows;
    uint32_t total;
    uint32_t max_scroll;
};

/* Number of visual (wrapped) lines the output occupies at this width. */
static uint32_t count_lines(uint32_t columns)
{
    uint32_t lines = 1u;
    uint32_t column = 0u;
    for (uint32_t i = 0u; i < shell.output_length; ++i) {
        char value = shell.output[i];
        if (value == '\n') {
            ++lines;
            column = 0u;
        } else if (value == '\r') {
            column = 0u;
        } else {
            if (column >= columns) {
                ++lines;
                column = 0u;
            }
            ++column;
        }
    }
    return lines;
}

static void compute_layout(const struct WindowApp *app,
    struct shell_layout *layout)
{
    struct book8gui_rect client = ui_client(app);
    uint32_t bottom = app->bounds.y + app->bounds.height - 4u;
    layout->input = (struct book8gui_rect){
        client.x, bottom - SHELL_INPUT_HEIGHT, client.width,
        SHELL_INPUT_HEIGHT
    };
    layout->frame = (struct book8gui_rect){
        client.x, client.y, client.width,
        layout->input.y > client.y + 2u ? layout->input.y - 2u - client.y : 0u
    };
    layout->track = ui_scroll_track(&layout->frame);
    layout->text_x = layout->frame.x + UI_PAD;
    layout->text_y = layout->frame.y + UI_PAD;
    uint32_t width = layout->frame.width > 2u * UI_PAD + UI_SCROLL_W
        ? layout->frame.width - 2u * UI_PAD - UI_SCROLL_W : 0u;
    uint32_t height = layout->frame.height > 2u * UI_PAD
        ? layout->frame.height - 2u * UI_PAD : 0u;
    layout->columns = width / SHELL_CHAR_WIDTH;
    layout->rows = height / SHELL_CHAR_HEIGHT;
    layout->total = layout->columns != 0u ? count_lines(layout->columns) : 1u;
    layout->max_scroll = layout->total > layout->rows
        ? layout->total - layout->rows : 0u;
    if (shell.output_scroll > layout->max_scroll) {
        shell.output_scroll = layout->max_scroll;
    }
}

/* Positive lines scroll back toward older output. */
static void adjust_scroll(const struct WindowApp *app, int32_t delta)
{
    struct shell_layout layout;
    compute_layout(app, &layout);
    int32_t next = (int32_t)shell.output_scroll + delta;
    if (next < 0) {
        next = 0;
    }
    shell.output_scroll = (uint32_t)next > layout.max_scroll
        ? layout.max_scroll : (uint32_t)next;
}

static void render_output(const struct book8gui_platform *platform,
    const struct shell_layout *layout)
{
    if (layout->columns == 0u || layout->rows == 0u) {
        return;
    }
    uint32_t first = layout->max_scroll - shell.output_scroll;
    uint32_t line = 0u;
    uint32_t column = 0u;
    for (uint32_t i = 0u; i < shell.output_length; ++i) {
        char value = shell.output[i];
        if (value == '\n') {
            ++line;
            column = 0u;
            continue;
        }
        if (value == '\r') {
            column = 0u;
            continue;
        }
        if (column >= layout->columns) {
            ++line;
            column = 0u;
        }
        if (line >= first + layout->rows) {
            break;
        }
        if (line >= first) {
            char character[2] = {value, '\0'};
            platform->draw_text(layout->text_x + column * SHELL_CHAR_WIDTH,
                layout->text_y + (line - first) * SHELL_CHAR_HEIGHT,
                character, SHELL_TEXT, SHELL_FIELD);
        }
        ++column;
    }
}

static void render(const struct book8gui_platform *platform,
    struct WindowApp *app)
{
    shell_current = app->state;
    struct shell_layout layout;
    compute_layout(app, &layout);
    ui_sunken(platform, &layout.frame, SHELL_FIELD);
    render_output(platform, &layout);
    ui_scrollbar(platform, &layout.track, layout.total, layout.rows,
        layout.max_scroll - shell.output_scroll);

    ui_sunken(platform, &layout.input, SHELL_FIELD);
    uint32_t x = layout.input.x;
    uint32_t y = layout.input.y + 3u;
    platform->draw_text(x + 6u, y, "A:\\>", SHELL_MUTED, SHELL_FIELD);

    uint32_t max_chars = layout.input.width > 66u
        ? (layout.input.width - 66u) / SHELL_CHAR_WIDTH : 0u;
    uint32_t start = shell.command_length > max_chars
        ? shell.command_length - max_chars : 0u;
    uint32_t count = shell.command_length - start;
    char visible[SHELL_COMMAND_SIZE];
    for (uint32_t i = 0u; i < count; ++i) {
        visible[i] = shell.command[start + i];
    }
    visible[count] = '\0';
    platform->draw_text(x + 46u, y, visible, SHELL_TEXT, SHELL_FIELD);
    if (app->focused) {
        platform->fill_rect(x + 46u + count * SHELL_CHAR_WIDTH, y, 2u, 14u,
            SHELL_TEXT);
    }
}

static bool handle_event(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event)
{
    shell_current = app->state;
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        struct shell_layout layout;
        compute_layout(app, &layout);
        bool down = (event->mouse_buttons & 1u) != 0u;
        bool pressed = down && (shell.previous_buttons & 1u) == 0u;
        shell.previous_buttons = event->mouse_buttons;
        if (!down) {
            shell.scroll_drag = false;
        }
        if (event->wheel_delta != 0
            && event->mouse_x >= (int32_t)app->bounds.x
            && event->mouse_y >= (int32_t)app->bounds.y
            && (uint32_t)event->mouse_x
                < app->bounds.x + app->bounds.width
            && (uint32_t)event->mouse_y
                < app->bounds.y + app->bounds.height) {
            adjust_scroll(app, ui_wheel_steps(event->wheel_delta, &shell.wheel_remainder));
            book8gui_invalidate();
        }
        if (pressed && layout.max_scroll != 0u
            && ui_contains(&layout.track, event->mouse_x, event->mouse_y)) {
            shell.scroll_drag = true;
            shell.scroll_grab = ui_scrollbar_grab(&layout.track, layout.total,
                layout.rows, layout.max_scroll - shell.output_scroll, event->mouse_y);
        }
        if (down && shell.scroll_drag) {
            uint32_t first = ui_scrollbar_drag_position(&layout.track,
                layout.total, layout.rows, event->mouse_y, shell.scroll_grab);
            shell.output_scroll = layout.max_scroll - first;
            book8gui_invalidate();
        }
        return app->visible;
    }
    if (event->key == BOOK8GUI_KEY_UP) {
        adjust_scroll(app, 1);
        book8gui_invalidate();
    } else if (event->key == BOOK8GUI_KEY_DOWN) {
        adjust_scroll(app, -1);
        book8gui_invalidate();
    } else if (event->key == BOOK8GUI_KEY_ENTER) {
        if (shell.command_length != 0u) {
            shell.command[shell.command_length] = '\0';
            append_text("> ");
            append_text(shell.command);
            append_output('\n');
            shell_console = shell_current;
            platform->run_command(shell.command);
            shell_console = NULL;
            shell.command_length = 0u;
            shell.command[0] = '\0';
            shell.output_scroll = 0u;
            book8gui_invalidate();
        }
    } else if (event->key == BOOK8GUI_KEY_BACKSPACE
        || event->key == BOOK8GUI_KEY_DELETE) {
        if (shell.command_length != 0u) {
            shell.command[--shell.command_length] = '\0';
            book8gui_invalidate();
        }
    } else if (event->key >= 0x20u && event->key <= 0x7eu
        && shell.command_length + 1u < sizeof(shell.command)) {
        shell.command[shell.command_length++] = (char)event->key;
        shell.command[shell.command_length] = '\0';
        book8gui_invalidate();
    }
    (void)app;
    return true;
}

static void *create(const struct book8gui_platform *platform)
{
    (void)platform;
    for (uint32_t i = 0u; i < BOOK8GUI_MAX_INSTANCES_PER_APP; ++i) {
        if (shell_pool[i].in_use) {
            continue;
        }
        shell_current = &shell_pool[i];
        shell.in_use = true;
        shell.command_length = 0u;
        shell.command[0] = '\0';
        shell.output_length = 0u;
        shell.output[0] = '\0';
        shell.output_scroll = 0u;
        shell.previous_buttons = 0u;
        shell.scroll_drag = false;
        shell.wheel_remainder = 0;
        append_text("BOOK8 Command Shell ready. Type HELP for commands.\n");
        return shell_current;
    }
    return NULL;
}

bool book8_shell_is_open(void)
{
    for (uint32_t i = 0u; i < BOOK8GUI_MAX_INSTANCES_PER_APP; ++i) {
        if (shell_pool[i].in_use) {
            return true;
        }
    }
    return false;
}

static void destroy(void *state)
{
    struct shell_state *instance = state;
    if (instance != NULL) {
        instance->in_use = false;
        if (shell_console == instance) {
            shell_console = NULL;
        }
    }
}

const struct book8gui_app_class book8_shell_class = {
    .name = "Command Shell",
    .default_width = 300u,
    .default_height = 300u,
    .single_instance = false,
    .state_size = sizeof(struct shell_state),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event
};
