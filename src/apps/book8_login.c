#include "book8_login.h"

#include <stddef.h>
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "../session.h"
#include "book8_ui.h"

struct login_state {
    char username[BOOK8_SESSION_NAME_MAX];
    char password[BOOK8_SESSION_PASSWORD_MAX + 1u];
    char message[72];
    uint8_t focus;
};

static struct login_state login;

static void clipped(const struct book8gui_platform *p, uint32_t x,
    uint32_t y, uint32_t width, const char *text, uint32_t color,
    uint32_t background)
{
    char line[96];
    uint32_t n = 0u, max = width / 8u;
    while (text[n] != '\0' && n < max && n + 1u < sizeof(line)) {
        line[n] = text[n];
        ++n;
    }
    line[n] = '\0';
    p->draw_text(x, y, line, color, background);
}

static struct book8gui_rect login_box(const struct WindowApp *app,
    uint32_t row)
{
    struct book8gui_rect client = ui_client(app);
    return (struct book8gui_rect){client.x + 12u, client.y + 82u + row * 48u,
        client.width - 24u, 32u};
}

static void *create(const struct book8gui_platform *p)
{
    (void)p;
    login = (struct login_state){0};
    char display[BOOK8_SESSION_NAME_MAX];
    if (!book8_session_profile(login.username, sizeof(login.username),
            display, sizeof(display))) {
        snprintf(login.username, sizeof(login.username), "admin");
    }
    login.focus = 1u;
    snprintf(login.message, sizeof(login.message), "Enter your BOOK8 account password.");
    return &login;
}

static void destroy(void *state)
{
    if (state != NULL) { memset(state, 0, sizeof(login)); }
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct book8gui_rect client = ui_client(app);
    p->fill_rect(client.x, client.y, client.width, client.height, UI_FACE);
    clipped(p, client.x + 12u, client.y + 12u, client.width - 24u,
        "Sign in to BOOK8", UI_TEXT, UI_FACE);
    clipped(p, client.x + 12u, client.y + 36u, client.width - 24u,
        "Primary administrator account", UI_MUTED, UI_FACE);
    for (uint32_t row = 0u; row < 2u; ++row) {
        struct book8gui_rect box = login_box(app, row);
        const char *label = row == 0u ? "Username" : "Password";
        ui_sunken(p, &box, UI_FIELD);
        clipped(p, box.x + 6u, box.y + 8u, 76u, label,
            login.focus == row ? UI_SELECTED : UI_MUTED, UI_FIELD);
        char value[BOOK8_SESSION_PASSWORD_MAX + 1u];
        const char *source = row == 0u ? login.username : login.password;
        uint32_t n = 0u;
        while (source[n] != '\0' && n < sizeof(value) - 1u) {
            value[n] = row == 0u ? source[n] : '*';
            ++n;
        }
        value[n] = '\0';
        clipped(p, box.x + 88u, box.y + 8u, box.width - 96u,
            value, UI_TEXT, UI_FIELD);
    }
    struct book8gui_rect submit = {client.x + 12u, client.y + 190u,
        client.width - 24u, 30u};
    ui_sunken(p, &submit, UI_SELECTED);
    clipped(p, submit.x + 8u, submit.y + 7u, submit.width - 16u,
        "Sign In", UI_SELECTED_TEXT, UI_SELECTED);
    clipped(p, client.x + 12u, client.y + 234u, client.width - 24u,
        login.message, UI_MUTED, UI_FACE);
}

static bool submit(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (!book8_session_login(login.username, login.password)) {
        snprintf(login.message, sizeof(login.message),
            "Sign-in failed. Check username and password.");
        memset(login.password, 0, sizeof(login.password));
        book8gui_invalidate();
        return false;
    }
    memset(login.password, 0, sizeof(login.password));
    if (p->diagnostic != NULL) {
        p->diagnostic("BOOK8-SESSION: login accepted uid=0\n");
    }
    (void)book8gui_refresh_programs();
    book8gui_set_modal(app, false);
    book8gui_request_close(app);
    return true;
}

static bool handle_event(const struct book8gui_platform *p,
    struct WindowApp *app, const struct book8gui_event *event)
{
    if (event->type == BOOK8GUI_EVENT_MOUSE) {
        if ((event->mouse_buttons & 1u) == 0u) { return true; }
        for (uint32_t row = 0u; row < 2u; ++row) {
            struct book8gui_rect box = login_box(app, row);
            if (ui_contains(&box, event->mouse_x, event->mouse_y)) {
                login.focus = (uint8_t)row;
                book8gui_invalidate();
                return true;
            }
        }
        struct book8gui_rect client = ui_client(app);
        struct book8gui_rect submit_box = {client.x + 12u, client.y + 190u,
            client.width - 24u, 30u};
        if (ui_contains(&submit_box, event->mouse_x, event->mouse_y)) {
            (void)submit(p, app);
        }
        return true;
    }
    if (event->type != BOOK8GUI_EVENT_KEY) { return true; }
    uint32_t key = event->key & BOOK8GUI_KEY_MASK;
    if (key == BOOK8GUI_KEY_TAB) {
        login.focus ^= 1u;
    } else if (key == BOOK8GUI_KEY_ENTER) {
        (void)submit(p, app);
    } else if (key == BOOK8GUI_KEY_BACKSPACE) {
        char *value = login.focus == 0u ? login.username : login.password;
        uint32_t length = 0u;
        while (value[length] != '\0') { ++length; }
        if (length != 0u) { value[length - 1u] = '\0'; }
    } else if (key >= 32u && key <= 126u) {
        char *value = login.focus == 0u ? login.username : login.password;
        uint32_t capacity = login.focus == 0u
            ? sizeof(login.username) : sizeof(login.password);
        uint32_t length = 0u;
        while (length + 1u < capacity && value[length] != '\0') { ++length; }
        if (length + 1u < capacity) {
            value[length] = (char)key;
            value[length + 1u] = '\0';
        }
    }
    book8gui_invalidate();
    return true;
}

const struct book8gui_app_class book8_login_class = {
    .name = "BOOK8 Sign In",
    .default_width = 420u,
    .default_height = 320u,
    .single_instance = true,
    .wants_all_keys = true,
    .state_size = sizeof(struct login_state),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event
};
