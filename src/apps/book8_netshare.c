#include "book8_netshare.h"
#include "book8_ui.h"
#include "book8_explorer.h"
#include "../../net/smb.h"
#include "../../net/ntlm.h"
#include "../../include/syscall.h"
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "../session.h"

struct browser {
    bool used, alert, saved, local_view, remember, guest, startup_error, connecting;
    uint8_t buttons, focus, dragging;
    uint8_t dialog, dialog_focus, profile;
    uint32_t session_uid;
    uint64_t session_token;
    int32_t scroll_grab;
    int32_t wheel_remainder[2];
    char ip[16], share[64], local[256], saved_path[512], message[160];
    char username[64], domain[64], password[128], input[256], details[9][320];
    struct book8_share_entry locals[BOOK8_SHARE_ENTRIES];
    uint32_t local_count;
    uint32_t selected, first[2];
    struct book8_share client;
};
static struct browser browser;
#define PROFILE_PATH "A:\\NETSHARE.CFG"
#define PROFILE_COUNT 8u
enum { LOGIN = 1, PROFILES, RENAME, DELETE_LOCAL, NEW_FOLDER, PROPERTIES };
struct profile {
    char ip[16], share[64], local[256], username[64], domain[64];
    uint8_t guest;
};
struct profiles {
    char magic[8];
    uint32_t count;
    struct profile items[PROFILE_COUNT];
};
static struct profiles profiles;
static char passwords[PROFILE_COUNT][128];
static bool profiles_loaded;
static void refresh_local(const struct book8gui_platform *, struct WindowApp *);
static bool local_path(char *, uint32_t, const char *);
static void show_dialog(struct WindowApp *, uint8_t);
static void render_dialog(const struct book8gui_platform *);
static bool dialog_event(const struct book8gui_platform *, struct WindowApp *, const struct book8gui_event *, bool);
static bool remote_ready(void)
{ return browser.client.state == SHARE_READY || browser.client.state == SHARE_DOWNLOADED || browser.client.state == SHARE_UPLOADED; }
static bool session_allowed(void)
{
    return book8_session_authorize(browser.session_uid, browser.session_token,
        BOOK8_SESSION_NETSHARE | BOOK8_SESSION_READ | BOOK8_SESSION_WRITE);
}
static uint32_t count(void) { return browser.local_view ? browser.local_count : browser.client.count; }
static struct book8_share_entry *entries(void) { return browser.local_view ? browser.locals : browser.client.entries; }
struct layout {
    struct book8gui_rect ip, share, local, connect, cancel, up, download, edit, explorer;
    struct book8gui_rect actions[7];
    struct book8gui_rect pane[2], track[2], status;
    uint32_t rows;
};
static uint64_t milliseconds(const struct book8gui_platform *p)
{ return p->clock_ticks && p->clock_hz ? p->clock_ticks() * 1000u / p->clock_hz : 0u; }
static bool layout(const struct WindowApp *app, struct layout *v)
{
    struct book8gui_rect c = ui_client(app);
    if (c.width < 300u || c.height < 220u) { return false; }
    uint32_t field = (c.width - 164u) / 2u;
    v->ip = (struct book8gui_rect){c.x, c.y + 16u, field, 24u};
    v->share = (struct book8gui_rect){c.x + field + 8u, c.y + 16u, field, 24u};
    v->connect = (struct book8gui_rect){c.x + c.width - 148u, c.y + 16u, 76u, 24u};
    v->cancel = (struct book8gui_rect){c.x + c.width - 68u, c.y + 16u, 68u, 24u};
    v->local = (struct book8gui_rect){c.x + 48u, c.y + 44u, c.width - 48u, 24u};
    v->up = (struct book8gui_rect){c.x, c.y + 72u, 40u, 24u};
    v->download = (struct book8gui_rect){c.x + 44u, c.y + 72u, 144u, 24u};
    v->edit = (struct book8gui_rect){c.x + 192u, c.y + 72u, 72u, 24u};
    v->explorer = (struct book8gui_rect){c.x + 268u, c.y + 72u, c.width - 268u, 24u};
    for (uint32_t i = 0; i < 7u; ++i) {
        uint32_t start = c.width * i / 7u, end = c.width * (i + 1u) / 7u;
        v->actions[i] = (struct book8gui_rect){c.x + start, c.y + 100u, end - start - 2u, 24u};
    }
    uint32_t left = c.width / 3u;
    v->pane[0] = (struct book8gui_rect){c.x, c.y + 144u, left, c.height - 168u};
    v->pane[1] = (struct book8gui_rect){c.x + left + 4u, c.y + 144u, c.width - left - 4u, c.height - 168u};
    for (uint32_t i = 0u; i < 2u; ++i) { v->track[i] = ui_scroll_track(&v->pane[i]); }
    v->rows = (v->pane[0].height - 6u) / 18u;
    v->status = (struct book8gui_rect){c.x, c.y + c.height - 20u, c.width, 20u};
    return true;
}
static void text(const struct book8gui_platform *p, uint32_t x, uint32_t y,
    uint32_t width, const char *value, uint32_t fg, uint32_t bg)
{
    char clipped[128]; uint32_t n = 0u;
    while (value[n] && n < sizeof(clipped) - 1u && n < width / 8u) { clipped[n] = value[n]; ++n; }
    clipped[n] = '\0'; p->draw_text(x, y, clipped, fg, bg);
}
static void button(const struct book8gui_platform *p, struct book8gui_rect r, const char *label, bool enabled)
{
    ui_sunken(p, &r, UI_FACE);
    text(p, r.x + 3u, r.y + 4u, r.width - 6u, label, enabled ? UI_TEXT : UI_MUTED, UI_FACE);
}
static void alert(const struct book8gui_platform *p, struct WindowApp *app, const char *message)
{
    snprintf(browser.message, sizeof(browser.message), "%s", message);
    app->visible = true; app->minimized = false;
    browser.alert = true; book8gui_set_modal(app, true);
    if (p->diagnostic) { p->diagnostic("NETSHARE: "); p->diagnostic(message); p->diagnostic("\n"); }
    book8gui_invalidate();
}
static struct book8gui_rect alert_box(const struct book8gui_platform *p)
{ return (struct book8gui_rect){(p->width - 520u) / 2u, (p->height - 36u - 128u) / 2u, 520u, 128u}; }
static void render_alert(const struct book8gui_platform *p)
{
    if (!browser.alert) { return; }
    struct book8gui_rect box = alert_box(p); ui_sunken(p, &box, UI_FACE);
    text(p, box.x + 8u, box.y + 8u, box.width - 16u, "Network Share Browser", UI_TEXT, UI_FACE);
    for (uint32_t row = 0u; row < 3u; ++row) {
        uint32_t offset = row * 62u;
        if (offset >= strlen(browser.message)) { break; }
        text(p, box.x + 8u, box.y + 30u + row * 18u, 496u, browser.message + offset, UI_TEXT, UI_FACE);
    }
    button(p, (struct book8gui_rect){box.x + 8u, box.y + 96u, 64u, 24u}, "OK", true);
}
static uint32_t visible_rows(const struct layout *v, uint32_t pane)
{ return pane == 1u && browser.saved && !browser.local_view && v->rows > 2u ? v->rows - 2u : v->rows; }
static uint32_t pane_count(uint32_t pane)
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < (browser.local_view ? browser.local_count : browser.client.count); ++i) {
        if (entries()[i].directory == (pane == 0u)) { ++count; }
    }
    return count;
}
static uint32_t entry_at(uint32_t pane, uint32_t row)
{
    for (uint32_t i = 0u; i < count(); ++i) {
        if (entries()[i].directory == (pane == 0u) && row-- == 0u) { return i; }
    }
    return UINT32_MAX;
}
static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct layout v;
    if (!layout(app, &v)) {
        struct book8gui_rect c = ui_client(app);
        text(p, c.x, c.y, c.width, "Enlarge window to use Share Browser.", UI_TEXT, UI_FACE);
        render_dialog(p);
        render_alert(p);
        return;
    }
    text(p, v.ip.x, v.ip.y - 16u, v.ip.width, "Host IPv4", UI_TEXT, UI_FACE);
    text(p, v.share.x, v.share.y - 16u, v.share.width, "SMB share", UI_TEXT, UI_FACE);
    struct book8gui_rect fields[] = {v.ip, v.share, v.local};
    const char *values[] = {browser.ip, browser.share, browser.local};
    for (uint32_t i = 0u; i < 3u; ++i) {
        ui_sunken(p, &fields[i], UI_FIELD);
        uint32_t length = (uint32_t)strlen(values[i]), columns = (fields[i].width - 6u) / 8u;
        text(p, fields[i].x + 3u, fields[i].y + 4u, fields[i].width - 6u,
            values[i] + (length > columns ? length - columns : 0u), UI_TEXT, UI_FIELD);
        if (browser.focus == i && browser.client.state != SHARE_BUSY && columns) {
            uint32_t caret = length < columns ? length : columns - 1u;
            p->fill_rect(fields[i].x + 3u + caret * 8u, fields[i].y + 4u, 2u, 16u, UI_TEXT);
        }
    }
    text(p, v.local.x - 48u, v.local.y + 4u, 48u, "Local:", UI_TEXT, UI_FACE);
    bool ready = remote_ready();
    button(p, v.connect, "Connect", browser.client.state != SHARE_BUSY);
    button(p, v.cancel, "Cancel", browser.client.state == SHARE_BUSY);
    button(p, v.up, "Up", browser.local_view || (ready && browser.client.path[0]));
    button(p, v.download, "Download to Local", !browser.local_view && ready && browser.selected < browser.client.count);
    bool local_file = browser.local_view && browser.selected < browser.local_count && !browser.locals[browser.selected].directory;
    button(p, v.edit, "Edit", (browser.saved || local_file) && book8gui_utility_installed(BOOK8_GUI_CODEEDIT));
    button(p, v.explorer, "Explorer", browser.saved || browser.local_view);
    const char *labels[] = {browser.local_view ? "Remote" : "Local", "Upload", "Properties", "Rename", "Delete", "New folder", "Profiles"};
    bool selected = browser.selected < count(), idle = browser.client.state != SHARE_BUSY;
    for (uint32_t i = 0; i < 7u; ++i) {
        bool enabled = idle && (i == 0u || i == 6u || (i == 5u && browser.local_view)
            || (selected && (i == 2u || (browser.local_view && (i != 1u || ready)))));
        button(p, v.actions[i], labels[i], enabled);
    }
    text(p, v.pane[0].x, v.pane[0].y - 16u, v.pane[0].width, "Directories", UI_TEXT, UI_FACE);
    text(p, v.pane[1].x, v.pane[1].y - 16u, v.pane[1].width, browser.local_view ? "Local files / bytes" : "Remote files / bytes", UI_TEXT, UI_FACE);
    for (uint32_t pane = 0u; pane < 2u; ++pane) {
        ui_sunken(p, &v.pane[pane], UI_FIELD);
        uint32_t count = pane_count(pane);
        uint32_t rows = visible_rows(&v, pane);
        browser.first[pane] = ui_wheel_scroll(browser.first[pane], count, rows, 0, 1u);
        for (uint32_t row = 0u; row < rows; ++row) {
            uint32_t index = entry_at(pane, browser.first[pane] + row);
            if (index == UINT32_MAX) { break; }
            const struct book8_share_entry *entry = &entries()[index];
            char label[320];
            if (pane == 0u) { snprintf(label, sizeof(label), "%s", entry->name); }
            else { snprintf(label, sizeof(label), "%llu  %s", (unsigned long long)entry->size, entry->name); }
            uint32_t y = v.pane[pane].y + 3u + row * 18u;
            uint32_t color = index == browser.selected ? UI_SELECTED : UI_FIELD;
            p->fill_rect(v.pane[pane].x + 2u, y, v.pane[pane].width - 16u, 18u, color);
            text(p, v.pane[pane].x + 3u, y + 1u, v.pane[pane].width - 18u, label,
                index == browser.selected ? UI_SELECTED_TEXT : UI_TEXT, color);
        }
        ui_scrollbar(p, &v.track[pane], count, rows, browser.first[pane]);
    }
    if (browser.saved && !browser.local_view && v.rows > 2u) {
        char preview[65];
        uint32_t count = browser.client.downloaded < 64u ? browser.client.downloaded : 64u;
        for (uint32_t i = 0u; i < count; ++i) {
            uint8_t c = browser.client.data[i]; preview[i] = c >= 32u && c < 127u ? (char)c : '.';
        }
        preview[count] = '\0';
        uint32_t y = v.pane[1].y + 3u + (v.rows - 2u) * 18u;
        text(p, v.pane[1].x + 3u, y, v.pane[1].width - 18u, "Downloaded preview:", UI_MUTED, UI_FIELD);
        text(p, v.pane[1].x + 3u, y + 18u, v.pane[1].width - 18u, preview, UI_TEXT, UI_FIELD);
    }
    char status[384];
    if (browser.client.state == SHARE_BUSY) {
        snprintf(status, sizeof(status), browser.client.uploading ? "Uploading %u / %u bytes..." : browser.client.downloading ? "Downloading %u / %u bytes..." : "Connecting / listing SMB2 share...",
            browser.client.downloaded, browser.client.expected);
    } else if (browser.message[0]) { snprintf(status, sizeof(status), "%s", browser.message); }
    else if (browser.local_view) { snprintf(status, sizeof(status), "Local: %s (%u entries)", browser.local, browser.local_count); }
    else if (ready) { snprintf(status, sizeof(status), "%s\\%s (%u entries)", browser.client.share, browser.client.path, browser.client.count); }
    else { snprintf(status, sizeof(status), "SMB2 / TCP 445. Connect using Guest or a server login."); }
    ui_sunken(p, &v.status, UI_FACE);
    text(p, v.status.x + 3u, v.status.y + 2u, v.status.width - 6u, status, UI_TEXT, UI_FACE);
    render_dialog(p);
    render_alert(p);
}
static void start_connection(const struct book8gui_platform *p, struct WindowApp *app)
{
    uint32_t ip;
    if (!book8_net_parse_ip(browser.ip, &ip)) { alert(p, app, "Invalid IPv4 address; enter four decimal octets."); return; }
    struct book8_net *net = p->network ? p->network() : NULL;
    if (!net || !net->ip) { alert(p, app, "Network offline: enable networking and obtain an IPv4 address."); return; }
    browser.saved = false; browser.message[0] = '\0'; browser.local_view = false;
    browser.selected = UINT32_MAX; browser.first[0] = browser.first[1] = 0u;
    browser.wheel_remainder[0] = browser.wheel_remainder[1] = 0;
    bool ok = browser.guest ? book8_share_connect(&browser.client, net, ip, browser.share, milliseconds(p))
        : book8_share_connect_auth(&browser.client, net, ip, browser.share, browser.username, browser.domain, browser.password, milliseconds(p));
    browser.connecting = ok;
    if (!ok) { alert(p, app, browser.client.error); }
    if (!browser.remember) { book8_ntlm_clear(browser.password, sizeof(browser.password)); }
}
static void connect(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.client.state == SHARE_BUSY) { return; }
    uint32_t ip;
    if (!book8_net_parse_ip(browser.ip, &ip)) { alert(p, app, "Invalid IPv4 address; enter four decimal octets."); return; }
    struct book8_net *net = p->network ? p->network() : NULL;
    if (!net || !net->ip) { alert(p, app, "Network offline: enable networking and obtain an IPv4 address."); return; }
    if (browser.profile < profiles.count) {
        const struct profile *profile = &profiles.items[browser.profile];
        if (!strcmp(profile->ip, browser.ip) && !strcmp(profile->share, browser.share)) {
            if (profile->guest) { browser.guest = true; start_connection(p, app); return; }
            snprintf(browser.username, sizeof(browser.username), "%s", profile->username);
            snprintf(browser.domain, sizeof(browser.domain), "%s", profile->domain);
            if (passwords[browser.profile][0]) {
                snprintf(browser.password, sizeof(browser.password), "%s", passwords[browser.profile]);
                browser.guest = false; start_connection(p, app); return;
            }
        }
    }
    book8_ntlm_clear(browser.password, sizeof(browser.password));
    browser.message[0] = '\0'; show_dialog(app, LOGIN);
}
static void tick(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (!session_allowed()) { return; }
    enum book8_share_state old = browser.client.state;
    uint32_t bytes = browser.client.downloaded;
    if (browser.startup_error) {
        char message[160]; snprintf(message, sizeof(message), "%s", browser.message);
        browser.startup_error = false; alert(p, app, message);
    }
    if (old == SHARE_IDLE || old == SHARE_FAILED) { return; }
    if (!p->network || p->network() != browser.client.tcp.net) {
        book8_share_close(&browser.client); alert(p, app, "Network went offline; reconnect when ready."); return;
    }
    book8_share_poll(&browser.client, milliseconds(p));
    if (old == SHARE_BUSY && browser.client.state == SHARE_READY && p->diagnostic) {
        char message[80];
        snprintf(message, sizeof(message), "NETSHARE: listed %u entries\n", browser.client.count);
        p->diagnostic(message);
    }
    if (browser.connecting && old == SHARE_BUSY && browser.client.state == SHARE_READY && !browser.guest && browser.remember
        && browser.profile < profiles.count && !strcmp(profiles.items[browser.profile].ip, browser.ip)
        && !strcmp(profiles.items[browser.profile].share, browser.share)
        && !strcmp(profiles.items[browser.profile].username, browser.username)
        && !strcmp(profiles.items[browser.profile].domain, browser.domain)) {
        snprintf(passwords[browser.profile], sizeof(passwords[0]), "%s", browser.password);
    }
    if (browser.client.state != SHARE_BUSY) { browser.connecting = false; }
    if (browser.client.state == SHARE_FAILED && old != SHARE_FAILED) {
        if (browser.client.auth_failed) {
            book8_ntlm_clear(browser.password, sizeof(browser.password));
            if (browser.profile < profiles.count) { book8_ntlm_clear(passwords[browser.profile], sizeof(passwords[0])); }
            snprintf(browser.message, sizeof(browser.message), "%s", browser.client.error); show_dialog(app, LOGIN);
            if (p->diagnostic) { p->diagnostic("NETSHARE: Login failed; retry prompt\n"); }
        } else { alert(p, app, browser.client.error); }
    }
    if (old == SHARE_BUSY && browser.client.state == SHARE_UPLOADED) {
        if (p->diagnostic) { p->diagnostic("NETSHARE: upload saved\n"); }
        char path[256]; snprintf(path, sizeof(path), "%s", browser.client.path);
        if (!book8_share_directory(&browser.client, path, milliseconds(p))) { alert(p, app, browser.client.error); }
        else {
            browser.selected = UINT32_MAX; browser.first[0] = browser.first[1] = 0u;
            snprintf(browser.message, sizeof(browser.message), "Upload complete. Refreshing remote folder.");
        }
    }
    if (old == SHARE_BUSY && browser.client.state == SHARE_DOWNLOADED) {
        uint32_t length = (uint32_t)strlen(browser.local);
        char directory[256];
        snprintf(directory, sizeof(directory), "%s", browser.local);
        while (length > 3u && (directory[length - 1u] == '\\' || directory[length - 1u] == '/')) { directory[--length] = '\0'; }
        if (length < 3u || directory[1] != ':' || (directory[2] != '\\' && directory[2] != '/')) {
            alert(p, app, "Download directory must be an absolute local FAT drive path."); return;
        }
        const char *name = browser.client.entries[browser.selected].name;
        int n = snprintf(browser.saved_path, sizeof(browser.saved_path), "%s\\%s", directory, name);
        if (n < 0 || (uint32_t)n >= sizeof(browser.saved_path)) { alert(p, app, "Local download path is too long."); return; }
        if (!p->make_directory || !p->create_file || !p->make_directory(directory)
            || !p->create_file(browser.saved_path, browser.client.data, browser.client.downloaded)) {
            alert(p, app, p->last_error_text ? p->last_error_text() : "FAT download failed; existing files are never overwritten."); return;
        }
        browser.saved = true;
        snprintf(browser.message, sizeof(browser.message), "Downloaded %u bytes. Use Edit or Explorer to open locally.", browser.client.downloaded);
        if (p->diagnostic) { p->diagnostic("NETSHARE: download saved\n"); }
    }
    if (old == SHARE_BUSY || old != browser.client.state || bytes != browser.client.downloaded) { book8gui_invalidate(); }
}
static void navigate(const struct book8gui_platform *p, struct WindowApp *app, const char *path)
{
    if (!book8_share_directory(&browser.client, path, milliseconds(p))) {
        alert(p, app, browser.client.error[0] ? browser.client.error : "Directory operation is unavailable."); return;
    }
    browser.selected = UINT32_MAX; browser.first[0] = browser.first[1] = 0u;
    browser.wheel_remainder[0] = browser.wheel_remainder[1] = 0;
    browser.message[0] = '\0';
}
static bool local_path(char *path, uint32_t capacity, const char *name)
{
    uint32_t length = (uint32_t)strlen(browser.local);
    if (length < 3u || browser.local[1] != ':' || (browser.local[2] != '\\' && browser.local[2] != '/')) { return false; }
    while (length > 3u && (browser.local[length - 1u] == '\\' || browser.local[length - 1u] == '/')) { --length; }
    int n = snprintf(path, capacity, "%.*s%s%s", (int)length, browser.local,
        length == 3u ? "" : "\\", name);
    return n >= 0 && (uint32_t)n < capacity;
}
static const char *disk_error(const struct book8gui_platform *p)
{ return p->last_error_text ? p->last_error_text() : "Local filesystem operation failed or is unavailable."; }
static bool collect_local(const char *name, bool directory, uint32_t size, void *context)
{
    (void)context;
    if (!strcmp(name, ".") || !strcmp(name, "..")) { return true; }
    if (browser.local_count == BOOK8_SHARE_ENTRIES) { ++browser.local_count; return false; }
    struct book8_share_entry *e = &browser.locals[browser.local_count++];
    memset(e, 0, sizeof(*e)); snprintf(e->name, sizeof(e->name), "%s", name);
    e->directory = directory; e->size = size; return true;
}
static void refresh_local(const struct book8gui_platform *p, struct WindowApp *app)
{
    char path[512]; browser.local_count = 0u; browser.selected = UINT32_MAX;
    browser.first[0] = browser.first[1] = 0u; browser.wheel_remainder[0] = browser.wheel_remainder[1] = 0;
    if (!local_path(path, sizeof(path), "")) { alert(p, app, "Local folder must be an absolute drive path (for example A:\\DOWNLOADS)."); return; }
    bool ok = p->list_directory && p->list_directory(path, collect_local, NULL);
    if (browser.local_count > BOOK8_SHARE_ENTRIES) {
        browser.local_count = 0u; alert(p, app, "Local folder exceeds 128 entries. Choose a smaller folder.");
    } else if (!ok) { browser.local_count = 0u; alert(p, app, disk_error(p)); }
    else { browser.message[0] = '\0'; }
}
static void show_dialog(struct WindowApp *app, uint8_t kind)
{
    browser.dialog = kind; browser.dialog_focus = 0u;
    app->visible = true; app->minimized = false; book8gui_set_modal(app, true); book8gui_invalidate();
}
static struct book8gui_rect dialog_box(const struct book8gui_platform *p)
{
    uint32_t width = p->width < 536u ? p->width - 16u : 520u;
    uint32_t height = p->height < 352u ? p->height - 52u : 300u;
    return (struct book8gui_rect){(p->width - width) / 2u, (p->height - 36u - height) / 2u, width, height};
}
static struct book8gui_rect dialog_field(struct book8gui_rect box, uint32_t row)
{ return (struct book8gui_rect){box.x + 96u, box.y + 54u + row * 34u, box.width - 108u, 24u}; }
static struct book8gui_rect dialog_button(struct book8gui_rect box, uint32_t i)
{
    uint32_t width = (box.width - 20u) / 4u;
    return (struct book8gui_rect){box.x + 8u + i * (width + 2u), box.y + box.height - 32u, width, 24u};
}
static void render_dialog(const struct book8gui_platform *p)
{
    if (!browser.dialog) { return; }
    struct book8gui_rect box = dialog_box(p); ui_sunken(p, &box, UI_FACE);
    const char *title = browser.dialog == LOGIN ? "Server login" : browser.dialog == PROFILES ? "Server profiles (passwords stay in memory)"
        : browser.dialog == RENAME ? "Rename local entry" : browser.dialog == DELETE_LOCAL ? "Delete local entry?" : browser.dialog == NEW_FOLDER ? "Create local folder" : "File properties";
    text(p, box.x + 8u, box.y + 8u, box.width - 16u, title, UI_TEXT, UI_FACE);
    text(p, box.x + 8u, box.y + 30u, box.width - 16u, browser.message, UI_MUTED, UI_FACE);
    if (browser.dialog == LOGIN || browser.dialog == RENAME || browser.dialog == NEW_FOLDER) {
        const char *labels[] = {browser.dialog == LOGIN ? "Username:" : "Name:", "Domain:", "Password:"};
        const char *values[] = {browser.dialog == LOGIN ? browser.username : browser.input, browser.domain, browser.password};
        for (uint32_t i = 0; i < (browser.dialog == LOGIN ? 3u : 1u); ++i) {
            struct book8gui_rect field = dialog_field(box, i); ui_sunken(p, &field, UI_FIELD);
            text(p, box.x + 8u, field.y + 4u, 88u, labels[i], UI_TEXT, UI_FACE);
            char masked[128]; const char *value = values[i]; uint32_t n = (uint32_t)strlen(value);
            if (i == 2u) { memset(masked, '*', n); masked[n] = '\0'; value = masked; }
            uint32_t columns = (field.width - 6u) / 8u;
            text(p, field.x + 3u, field.y + 4u, field.width - 6u, value + (n > columns ? n - columns : 0u), UI_TEXT, UI_FIELD);
            if (browser.dialog_focus == i && columns) {
                p->fill_rect(field.x + 3u + (n < columns ? n : columns - 1u) * 8u, field.y + 4u, 2u, 16u, UI_TEXT);
            }
        }
        if (browser.dialog == LOGIN) {
            text(p, box.x + 8u, box.y + 164u, box.width - 16u,
                browser.remember ? "[x] Remember password for this session" : "[ ] Transient login (forget password after connect)", UI_TEXT, UI_FACE);
            text(p, box.x + 8u, box.y + 190u, box.width - 16u, "Passwords are never saved to disk.", UI_MUTED, UI_FACE);
        }
    } else if (browser.dialog == PROFILES) {
        for (uint32_t i = 0; i < profiles.count; ++i) {
            char label[256]; snprintf(label, sizeof(label), "%u  %s / %s (%s)", i + 1u, profiles.items[i].ip,
                profiles.items[i].share, profiles.items[i].guest ? "Guest" : profiles.items[i].username);
            text(p, box.x + 8u, box.y + 54u + i * 18u, box.width - 16u, label,
                i == browser.profile ? UI_SELECTED_TEXT : UI_TEXT, i == browser.profile ? UI_SELECTED : UI_FACE);
        }
        if (!profiles.count) { text(p, box.x + 8u, box.y + 54u, box.width - 16u, "No profiles. Save the current server fields.", UI_TEXT, UI_FACE); }
    } else if (browser.dialog == PROPERTIES) {
        for (uint32_t i = 0; i < 9u; ++i) { text(p, box.x + 8u, box.y + 54u + i * 18u, box.width - 16u, browser.details[i], UI_TEXT, UI_FACE); }
    } else { text(p, box.x + 8u, box.y + 64u, box.width - 16u, browser.input, UI_TEXT, UI_FACE); }
    const char *buttons[] = {browser.dialog == PROFILES ? "Load" : browser.dialog == LOGIN ? "Login" : browser.dialog == PROPERTIES ? "OK" : "Confirm",
        browser.dialog == PROFILES ? "Save current" : browser.dialog == LOGIN ? "Guest" : "",
        browser.dialog == PROFILES ? "Remove" : "", "Cancel"};
    for (uint32_t i = 0; i < 4u; ++i) { if (buttons[i][0]) { button(p, dialog_button(box, i), buttons[i], true); } }
}
static bool valid_name(const char *name)
{
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) { return false; }
    for (const char *c = name; *c; ++c) {
        if ((uint8_t)*c < 32u || (uint8_t)*c >= 127u || strchr("\\/:*?\"<>|", *c)) { return false; }
    }
    uint32_t n = (uint32_t)strlen(name); return name[n - 1u] != '.' && name[n - 1u] != ' ';
}
static bool profile_valid(const struct profile *v)
{
    if (!memchr(v->ip, 0, sizeof(v->ip)) || !memchr(v->share, 0, sizeof(v->share))
        || !memchr(v->local, 0, sizeof(v->local)) || !memchr(v->username, 0, sizeof(v->username))
        || !memchr(v->domain, 0, sizeof(v->domain)) || v->guest > 1u) { return false; }
    uint32_t ip;
    const char *texts[] = {v->ip, v->share, v->local, v->username, v->domain};
    for (uint32_t i = 0; i < 5u; ++i) {
        for (const char *c = texts[i]; *c; ++c) { if ((uint8_t)*c < 32u || (uint8_t)*c >= 127u) { return false; } }
    }
    return book8_net_parse_ip(v->ip, &ip) && valid_name(v->share)
        && strlen(v->local) >= 3u && v->local[1] == ':' && (v->local[2] == '\\' || v->local[2] == '/');
}
static bool save_profiles(const struct book8gui_platform *p, struct WindowApp *app, const struct profiles *next)
{
    if (!p->write_file || !p->write_file(PROFILE_PATH, next, sizeof(*next))) { alert(p, app, disk_error(p)); return false; }
    profiles = *next;
    if (p->diagnostic) { p->diagnostic("NETSHARE: profiles saved without passwords\n"); }
    return true;
}
static bool leap(uint32_t year) { return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u); }
static void remote_time(char *out, uint32_t capacity, const char *label, uint64_t value)
{
    if (!value || value >= 2650467744000000000ull) { snprintf(out, capacity, "%s: unavailable", label); return; }
    uint64_t seconds = value / 10000000u;
    uint32_t days = (uint32_t)(seconds / 86400u), year = 1601u, month = 1u;
    while (days >= (leap(year) ? 366u : 365u)) { days -= leap(year) ? 366u : 365u; ++year; }
    static const uint8_t months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    while (days >= months[month - 1u] + (uint32_t)(month == 2u && leap(year))) {
        days -= months[month - 1u] + (uint32_t)(month == 2u && leap(year)); ++month;
    }
    uint32_t time = (uint32_t)(seconds % 86400u);
    snprintf(out, capacity, "%s: %04u-%02u-%02u %02u:%02u:%02u UTC", label, year, month, days + 1u,
        time / 3600u, (time / 60u) % 60u, time % 60u);
}
static void properties_dialog(const struct book8gui_platform *p, struct WindowApp *app)
{
    const struct book8_share_entry *entry = &entries()[browser.selected];
    memset(browser.details, 0, sizeof(browser.details));
    snprintf(browser.details[0], sizeof(browser.details[0]), "Name: %s", entry->name);
    snprintf(browser.details[1], sizeof(browser.details[1]), "Type: %s (%s)", entry->directory ? "Folder" : "File", browser.local_view ? "Local" : "SMB host");
    snprintf(browser.details[2], 128u, "Size: %llu bytes", (unsigned long long)entry->size);
    if (browser.local_view) {
        char path[512]; struct book8_file_info info;
        if (!local_path(path, sizeof(path), entry->name)) { alert(p, app, "Local path is too long or invalid."); return; }
        if (!p->file_info || !p->file_info(path, &info)) { alert(p, app, disk_error(p)); return; }
        snprintf(browser.details[2], sizeof(browser.details[2]), "Size: %llu bytes", (unsigned long long)info.size);
        snprintf(browser.details[3], 128u, info.allocation_valid ? "Allocated: %llu bytes" : "Allocated: unavailable", (unsigned long long)info.allocated_bytes);
        snprintf(browser.details[4], 128u, "Attributes: %s%s%s", info.read_only ? "read-only " : "writable ", info.system ? "system " : "", info.directory ? "directory" : "file");
        if (info.creation_valid) {
            snprintf(browser.details[5], 128u, "Created: %04u-%02u-%02u %02u:%02u:%02u%s",
                info.year, info.month, info.day, info.hour, info.minute, info.second, info.creation_utc ? " UTC" : " local");
        } else { snprintf(browser.details[5], 128u, "Created: unavailable"); }
        snprintf(browser.details[6], sizeof(browser.details[6]), "Folder: %s", browser.local);
    } else {
        snprintf(browser.details[3], 128u, "Allocated: %llu bytes", (unsigned long long)entry->allocated);
        snprintf(browser.details[4], 128u, "Attributes: 0x%08x%s", entry->attributes, entry->attributes & 1u ? " (read-only)" : "");
        remote_time(browser.details[5], sizeof(browser.details[5]), "Created", entry->created);
        remote_time(browser.details[6], sizeof(browser.details[6]), "Modified", entry->modified);
        snprintf(browser.details[7], 128u, "Server: %u.%u.%u.%u / %s", browser.client.tcp.peer >> 24u,
            (browser.client.tcp.peer >> 16u) & 255u, (browser.client.tcp.peer >> 8u) & 255u, browser.client.tcp.peer & 255u, browser.client.share);
        snprintf(browser.details[8], sizeof(browser.details[8]), "Folder: %s", browser.client.path[0] ? browser.client.path : "\\");
    }
    browser.message[0] = '\0'; show_dialog(app, PROPERTIES);
}
static bool dialog_event(const struct book8gui_platform *p, struct WindowApp *app, const struct book8gui_event *e, bool pressed)
{
    struct book8gui_rect box = dialog_box(p); uint32_t key = e->key & BOOK8GUI_KEY_MASK;
    uint32_t action = UINT32_MAX;
    if (e->type == BOOK8GUI_EVENT_KEY && key == BOOK8GUI_KEY_ESCAPE) { action = 3u; }
    else if (e->type == BOOK8GUI_EVENT_KEY && key == BOOK8GUI_KEY_ENTER) { action = 0u; }
    if (pressed) {
        for (uint32_t i = 0; i < 4u; ++i) { struct book8gui_rect r = dialog_button(box, i); if (ui_contains(&r, e->mouse_x, e->mouse_y)) { action = i; } }
        if (browser.dialog == LOGIN) {
            struct book8gui_rect remember = {box.x + 8u, box.y + 160u, box.width - 16u, 24u};
            if (ui_contains(&remember, e->mouse_x, e->mouse_y)) {
                browser.remember = !browser.remember;
                if (!browser.remember && browser.profile < profiles.count) { book8_ntlm_clear(passwords[browser.profile], sizeof(passwords[0])); }
            }
        }
    }
    if (action == 3u || (action == 0u && browser.dialog == PROPERTIES)) {
        if (browser.dialog == LOGIN) { book8_ntlm_clear(browser.password, sizeof(browser.password)); }
        browser.dialog = 0u; browser.message[0] = '\0'; book8gui_set_modal(app, false);
        return true;
    }
    if (browser.dialog == PROFILES) {
        if (pressed && e->mouse_y >= (int32_t)box.y + 54 && e->mouse_y < (int32_t)box.y + 54 + (int32_t)profiles.count * 18
            && e->mouse_x >= (int32_t)box.x && e->mouse_x < (int32_t)(box.x + box.width)) {
            browser.profile = (uint8_t)((e->mouse_y - (int32_t)box.y - 54) / 18);
        }
        if (e->type == BOOK8GUI_EVENT_KEY && profiles.count) {
            if (key == BOOK8GUI_KEY_UP) { browser.profile = browser.profile < profiles.count && browser.profile ? browser.profile - 1u : 0u; }
            if (key == BOOK8GUI_KEY_DOWN) { browser.profile = browser.profile + 1u < profiles.count ? browser.profile + 1u : (uint8_t)(profiles.count - 1u); }
        }
        if (action == 0u && browser.profile < profiles.count) {
            book8_share_close(&browser.client); browser.saved = false;
            const struct profile *v = &profiles.items[browser.profile];
            snprintf(browser.ip, sizeof(browser.ip), "%s", v->ip); snprintf(browser.share, sizeof(browser.share), "%s", v->share);
            snprintf(browser.local, sizeof(browser.local), "%s", v->local); snprintf(browser.username, sizeof(browser.username), "%s", v->username);
            snprintf(browser.domain, sizeof(browser.domain), "%s", v->domain); browser.guest = v->guest != 0u;
            book8_ntlm_clear(browser.password, sizeof(browser.password));
            browser.dialog = 0u; book8gui_set_modal(app, false); browser.selected = UINT32_MAX; browser.local_count = 0u;
            snprintf(browser.message, sizeof(browser.message), "Profile loaded. Connect to open this server.");
        } else if (action == 1u) {
            struct profiles next = profiles; memcpy(next.magic, "B8NSH01", 8u);
            uint32_t index = next.count;
            for (uint32_t i = 0; i < next.count; ++i) {
                if (!strcmp(next.items[i].ip, browser.ip) && !strcmp(next.items[i].share, browser.share)
                    && !strcmp(next.items[i].username, browser.username) && !strcmp(next.items[i].domain, browser.domain)) { index = i; break; }
            }
            if (index == PROFILE_COUNT) { alert(p, app, "All eight profile slots are used. Remove a profile first."); return true; }
            struct profile *v = &next.items[index]; memset(v, 0, sizeof(*v));
            snprintf(v->ip, sizeof(v->ip), "%s", browser.ip); snprintf(v->share, sizeof(v->share), "%s", browser.share);
            snprintf(v->local, sizeof(v->local), "%s", browser.local); snprintf(v->username, sizeof(v->username), "%s", browser.username);
            snprintf(v->domain, sizeof(v->domain), "%s", browser.domain); v->guest = browser.guest;
            if (!profile_valid(v)) { alert(p, app, "Enter a valid server IPv4, share and absolute local folder before saving."); return true; }
            if (index == next.count) { ++next.count; }
            if (save_profiles(p, app, &next)) {
                browser.profile = (uint8_t)index;
                if (browser.remember && browser.client.authenticated && !browser.guest
                    && !strcmp(browser.client.username, browser.username) && !strcmp(browser.client.domain, browser.domain)
                    && !strcmp(browser.client.share, browser.share)) {
                    uint32_t ip;
                    if (!book8_net_parse_ip(browser.ip, &ip) || ip != browser.client.tcp.peer) {
                        book8_ntlm_clear(passwords[index], sizeof(passwords[0]));
                    } else { snprintf(passwords[index], sizeof(passwords[0]), "%s", browser.password); }
                }
                snprintf(browser.message, sizeof(browser.message), "Profile saved. Password is not written to disk.");
            }
        } else if (action == 2u && browser.profile < profiles.count) {
            struct profiles next = profiles; uint32_t index = browser.profile;
            memmove(&next.items[index], &next.items[index + 1u], (next.count - index - 1u) * sizeof(next.items[0]));
            memset(&next.items[--next.count], 0, sizeof(next.items[0]));
            if (save_profiles(p, app, &next)) {
                memmove(passwords[index], passwords[index + 1u], (PROFILE_COUNT - index - 1u) * sizeof(passwords[0]));
                book8_ntlm_clear(passwords[PROFILE_COUNT - 1u], sizeof(passwords[0]));
                browser.profile = profiles.count ? 0u : UINT8_MAX;
            }
        }
        return true;
    }
    if (browser.dialog == LOGIN && (action == 0u || action == 1u)) {
        if (!action && !browser.username[0]) { snprintf(browser.message, sizeof(browser.message), "Enter a username or choose Guest."); return true; }
        browser.guest = action == 1u;
        if (browser.guest) {
            book8_ntlm_clear(browser.password, sizeof(browser.password)); browser.username[0] = browser.domain[0] = '\0';
        }
        if (!browser.remember && browser.profile < profiles.count) { book8_ntlm_clear(passwords[browser.profile], sizeof(passwords[0])); }
        browser.dialog = 0u; book8gui_set_modal(app, false); start_connection(p, app); return true;
    }
    if (!action && (browser.dialog == RENAME || browser.dialog == DELETE_LOCAL || browser.dialog == NEW_FOLDER)) {
        char path[512]; bool ok;
        if (browser.dialog != DELETE_LOCAL && !valid_name(browser.input)) { snprintf(browser.message, sizeof(browser.message), "Enter a valid filename, without path separators."); return true; }
        const char *name = browser.dialog == NEW_FOLDER ? browser.input : browser.selected < browser.local_count ? browser.locals[browser.selected].name : NULL;
        if (!name || !local_path(path, sizeof(path), name)) { alert(p, app, "Local selection/path is invalid or too long."); return true; }
        ok = browser.dialog == RENAME ? (p->rename_entry && p->rename_entry(path, browser.input))
            : browser.dialog == DELETE_LOCAL ? (p->delete_entry && p->delete_entry(path))
            : (p->make_directory && p->make_directory(path));
        if (!ok) { alert(p, app, disk_error(p)); return true; }
        if (p->diagnostic) {
            p->diagnostic(browser.dialog == RENAME ? "NETSHARE: local entry renamed\n"
                : browser.dialog == DELETE_LOCAL ? "NETSHARE: local entry deleted\n" : "NETSHARE: local folder created\n");
        }
        browser.dialog = 0u; book8gui_set_modal(app, false); refresh_local(p, app); return true;
    }
    if (browser.dialog == LOGIN || browser.dialog == RENAME || browser.dialog == NEW_FOLDER) {
        uint32_t fields = browser.dialog == LOGIN ? 3u : 1u;
        if (pressed) {
            for (uint32_t i = 0; i < fields; ++i) { struct book8gui_rect field = dialog_field(box, i); if (ui_contains(&field, e->mouse_x, e->mouse_y)) { browser.dialog_focus = (uint8_t)i; } }
        }
        if (e->type == BOOK8GUI_EVENT_KEY) {
            if (key == BOOK8GUI_KEY_TAB) { browser.dialog_focus = (uint8_t)((browser.dialog_focus + 1u) % fields); }
            else if (!(e->key & BOOK8GUI_KEY_CTRL)) {
                char *value = browser.dialog != LOGIN ? browser.input : browser.dialog_focus == 0u ? browser.username : browser.dialog_focus == 1u ? browser.domain : browser.password;
                uint32_t capacity = browser.dialog != LOGIN ? sizeof(browser.input) : browser.dialog_focus == 2u ? sizeof(browser.password) : sizeof(browser.username);
                uint32_t n = (uint32_t)strlen(value);
                if (key == BOOK8GUI_KEY_BACKSPACE) { if (n) { value[n - 1u] = '\0'; } }
                else if (key >= 32u && key < 127u) {
                    if (n + 1u >= capacity) { snprintf(browser.message, sizeof(browser.message), "Input field is full."); }
                    else { value[n] = (char)key; value[n + 1u] = '\0'; }
                }
            }
        }
    }
    return true;
}
static bool event(const struct book8gui_platform *p, struct WindowApp *app, const struct book8gui_event *e)
{
    if (!session_allowed()) {
        alert(p, app, "NetShare requires an authenticated administrator session.");
        return true;
    }
    bool down = e->type == BOOK8GUI_EVENT_MOUSE && (e->mouse_buttons & 1u);
    bool pressed = down && !(browser.buttons & 1u);
    if (e->type == BOOK8GUI_EVENT_MOUSE) { browser.buttons = e->mouse_buttons; if (!down) { browser.dragging = 0u; } }
    uint32_t key = e->key & BOOK8GUI_KEY_MASK;
    if (browser.alert) {
        struct book8gui_rect box = alert_box(p);
        struct book8gui_rect ok = {box.x + 8u, box.y + 96u, 64u, 24u};
        if ((e->type == BOOK8GUI_EVENT_KEY && (key == BOOK8GUI_KEY_ENTER || key == BOOK8GUI_KEY_ESCAPE))
            || (pressed && ui_contains(&ok, e->mouse_x, e->mouse_y))) {
            browser.alert = false; book8gui_set_modal(app, browser.dialog != 0u);
        }
        book8gui_invalidate(); return true;
    }
    if (browser.dialog) { bool handled = dialog_event(p, app, e, pressed); book8gui_invalidate(); return handled; }
    struct layout v;
    if (!layout(app, &v)) { return false; }
    if (e->type == BOOK8GUI_EVENT_KEY) {
        if (key == BOOK8GUI_KEY_TAB) { browser.focus = browser.focus >= 3u ? 0u : (uint8_t)((browser.focus + 1u) % 3u); }
        else if ((key == BOOK8GUI_KEY_UP || key == BOOK8GUI_KEY_DOWN) && browser.client.state != SHARE_BUSY && count()) {
            browser.focus = 3u;
            if (browser.selected >= count()) { browser.selected = 0u; }
            else if (key == BOOK8GUI_KEY_DOWN && browser.selected + 1u < count()) { ++browser.selected; }
            else if (key == BOOK8GUI_KEY_UP && browser.selected) { --browser.selected; }
        }
        else if (key == BOOK8GUI_KEY_ENTER) {
            if (browser.focus == 3u && browser.client.state != SHARE_BUSY && browser.selected < count() && entries()[browser.selected].directory) {
                char path[256]; const char *name = entries()[browser.selected].name;
                if (browser.local_view) {
                    if (!local_path(path, sizeof(path), name)) { alert(p, app, "Local path is too long or invalid."); }
                    else { snprintf(browser.local, sizeof(browser.local), "%s", path); refresh_local(p, app); }
                } else {
                    int n = snprintf(path, sizeof(path), "%s%s%s", browser.client.path, browser.client.path[0] ? "\\" : "", name);
                    if (n < 0 || (uint32_t)n >= sizeof(path)) { alert(p, app, "Remote path is too long."); }
                    else { navigate(p, app, path); }
                }
            } else if (browser.focus == 2u && browser.client.state != SHARE_BUSY) { browser.local_view = true; refresh_local(p, app); }
            else if (browser.focus != 3u) { connect(p, app); }
        }
        else if (browser.focus < 3u && browser.client.state != SHARE_BUSY && !(e->key & BOOK8GUI_KEY_CTRL)) {
            char *field = browser.focus == 0u ? browser.ip : browser.focus == 1u ? browser.share : browser.local;
            uint32_t capacity = browser.focus == 0u ? sizeof(browser.ip) : browser.focus == 1u ? sizeof(browser.share) : sizeof(browser.local);
            uint32_t n = (uint32_t)strlen(field);
            if (key == BOOK8GUI_KEY_BACKSPACE) {
                if (n) { field[n - 1u] = '\0'; }
                if (browser.focus == 2u) { browser.selected = UINT32_MAX; browser.local_count = 0u; browser.saved = false; }
            }
            else if (key >= 32u && key < 127u) {
                if (n + 1u >= capacity) { alert(p, app, "Input field is full."); }
                else {
                    field[n] = (char)key; field[n + 1u] = '\0';
                    if (browser.focus == 2u) { browser.selected = UINT32_MAX; browser.local_count = 0u; browser.saved = false; }
                }
            } else { return false; }
        }
    } else if (e->type == BOOK8GUI_EVENT_MOUSE) {
        bool ready = remote_ready(), idle = browser.client.state != SHARE_BUSY;
        struct book8gui_rect fields[] = {v.ip, v.share, v.local};
        for (uint32_t i = 0u; i < 3u; ++i) { if (pressed && ui_contains(&fields[i], e->mouse_x, e->mouse_y)) { browser.focus = (uint8_t)i; } }
        if (pressed && ui_contains(&v.connect, e->mouse_x, e->mouse_y)) { connect(p, app); }
        else if (pressed && ui_contains(&v.cancel, e->mouse_x, e->mouse_y) && browser.client.state == SHARE_BUSY) {
            bool upload = browser.client.uploading; book8_share_close(&browser.client);
            snprintf(browser.message, sizeof(browser.message), upload ? "Upload cancelled. A partial remote file may remain; check before retrying." : "Cancelled. Reconnect to browse again.");
        } else if (pressed && idle && browser.local_view && ui_contains(&v.up, e->mouse_x, e->mouse_y)) {
            uint32_t length = (uint32_t)strlen(browser.local);
            while (length > 3u && (browser.local[length - 1u] == '\\' || browser.local[length - 1u] == '/')) { browser.local[--length] = '\0'; }
            while (length > 3u && browser.local[length - 1u] != '\\' && browser.local[length - 1u] != '/') { browser.local[--length] = '\0'; }
            refresh_local(p, app);
        } else if (pressed && ready && !browser.local_view && ui_contains(&v.up, e->mouse_x, e->mouse_y) && browser.client.path[0]) {
            char parent[256]; snprintf(parent, sizeof(parent), "%s", browser.client.path);
            char *last = strrchr(parent, '\\'); if (last) { *last = '\0'; } else { parent[0] = '\0'; }
            navigate(p, app, parent);
        } else if (pressed && ready && !browser.local_view && ui_contains(&v.download, e->mouse_x, e->mouse_y) && browser.selected < browser.client.count) {
            browser.saved = false; browser.message[0] = '\0';
            if (!book8_share_download(&browser.client, browser.selected, milliseconds(p))) { alert(p, app, browser.client.error); }
        } else if (pressed && idle && ui_contains(&v.edit, e->mouse_x, e->mouse_y)
            && (browser.saved || (browser.local_view && browser.selected < browser.local_count && !browser.locals[browser.selected].directory))) {
            char path[512]; const char *document = browser.saved_path;
            if (browser.local_view) {
                if (!local_path(path, sizeof(path), browser.locals[browser.selected].name)) { alert(p, app, "Local path is too long or invalid."); return true; }
                document = path;
            }
            if (!book8gui_open_utility(BOOK8_GUI_CODEEDIT, document)) { alert(p, app, "Code Editor is unavailable, or this file cannot be opened as text."); }
        } else if (pressed && idle && (browser.saved || browser.local_view) && ui_contains(&v.explorer, e->mouse_x, e->mouse_y)) {
            struct WindowApp *explorer = book8gui_launch_class(&book8_explorer_class);
            if (!explorer || !book8_explorer_open_directory(p, explorer, browser.local)) { alert(p, app, "Unable to open the download directory in Explorer."); }
        }
        for (uint32_t i = 0; i < 7u && pressed && idle; ++i) {
            if (!ui_contains(&v.actions[i], e->mouse_x, e->mouse_y)) { continue; }
            if (i == 0u) {
                browser.local_view = !browser.local_view; browser.saved = false; browser.selected = UINT32_MAX;
                browser.first[0] = browser.first[1] = 0u; browser.message[0] = '\0';
                if (browser.local_view) { refresh_local(p, app); }
            } else if (i == 6u) { browser.message[0] = '\0'; show_dialog(app, PROFILES); }
            else if (i == 5u && browser.local_view) { browser.input[0] = browser.message[0] = '\0'; show_dialog(app, NEW_FOLDER); }
            else if (browser.selected < count()) {
                if (i == 2u) { properties_dialog(p, app); }
                else if (browser.local_view && i == 1u) {
                    char path[512]; uint32_t size;
                    const struct book8_share_entry *entry = &browser.locals[browser.selected];
                    if (!ready || entry->directory) { alert(p, app, "Connect and select a local file to upload."); }
                    else if (!local_path(path, sizeof(path), entry->name)) { alert(p, app, "Local path is too long or invalid."); }
                    else if (!p->read_file || !p->read_file(path, browser.client.data, sizeof(browser.client.data), &size)) { alert(p, app, disk_error(p)); }
                    else if (!book8_share_upload(&browser.client, entry->name, browser.client.data, size, milliseconds(p))) { alert(p, app, browser.client.error); }
                    else { browser.saved = false; browser.message[0] = '\0'; }
                } else if (browser.local_view && (i == 3u || i == 4u)) {
                    snprintf(browser.input, sizeof(browser.input), "%s", browser.locals[browser.selected].name);
                    browser.message[0] = '\0'; show_dialog(app, i == 3u ? RENAME : DELETE_LOCAL);
                }
            }
        }
        for (uint32_t pane = 0u; pane < 2u && (browser.local_view || ready) && idle; ++pane) {
            uint32_t rows = visible_rows(&v, pane);
            if (ui_contains(&v.pane[pane], e->mouse_x, e->mouse_y)) {
                browser.first[pane] = ui_wheel_scroll_delta(browser.first[pane], pane_count(pane), rows,
                    e->wheel_delta, 3u, &browser.wheel_remainder[pane]);
                if (pressed && ui_contains(&v.track[pane], e->mouse_x, e->mouse_y)) {
                    browser.dragging = pane_count(pane) > rows ? (uint8_t)(pane + 1u) : 0u;
                    browser.scroll_grab = ui_scrollbar_grab(&v.track[pane],
                        pane_count(pane), rows, browser.first[pane], e->mouse_y);
                }
                else if (pressed && e->mouse_y >= (int32_t)v.pane[pane].y + 3) {
                    uint32_t row = ((uint32_t)e->mouse_y - v.pane[pane].y - 3u) / 18u;
                    uint32_t index = entry_at(pane, browser.first[pane] + row);
                    if (row < rows && index != UINT32_MAX) {
                        browser.focus = 3u;
                        if (pane == 0u) {
                            if (browser.local_view) {
                                char path[256];
                                if (!local_path(path, sizeof(path), browser.locals[index].name)) { alert(p, app, "Local path is too long or invalid."); }
                                else { snprintf(browser.local, sizeof(browser.local), "%s", path); refresh_local(p, app); }
                                continue;
                            }
                            char path[512]; int n = snprintf(path, sizeof(path), "%s%s%s", browser.client.path,
                                browser.client.path[0] ? "\\" : "", browser.client.entries[index].name);
                            if (n < 0 || n >= 256) { alert(p, app, "Remote directory path is too long."); }
                            else { navigate(p, app, path); }
                        } else { browser.selected = index; }
                    }
                }
            }
            if (down && browser.dragging == pane + 1u) {
                browser.first[pane] = ui_scrollbar_drag_position(&v.track[pane],
                    pane_count(pane), rows, e->mouse_y, browser.scroll_grab);
            }
        }
    }
    book8gui_invalidate(); return true;
}
static void *create(const struct book8gui_platform *p)
{
    if (browser.used) { return NULL; }
    memset(&browser, 0u, sizeof(browser)); browser.used = true;
    browser.session_uid = p->session_uid != NULL
        ? p->session_uid() : UINT32_MAX;
    browser.session_token = p->session_token != NULL
        ? p->session_token() : 0u;
    if (!session_allowed()) {
        memset(&browser, 0u, sizeof(browser));
        return NULL;
    }
    struct book8_net *net = p->network ? p->network() : NULL;
    uint32_t gateway = net ? net->gateway : 0u;
    snprintf(browser.ip, sizeof(browser.ip), "%u.%u.%u.%u", gateway >> 24u,
        (gateway >> 16u) & 255u, (gateway >> 8u) & 255u, gateway & 255u);
    snprintf(browser.share, sizeof(browser.share), "public");
    snprintf(browser.local, sizeof(browser.local), "A:\\DOWNLOADS\\");
    browser.selected = UINT32_MAX; browser.profile = UINT8_MAX; browser.remember = true;
    if (!profiles_loaded) {
        profiles_loaded = true;
        struct book8_file_info info;
        if (p->file_info && p->file_info(PROFILE_PATH, &info)) {
            struct profiles loaded; uint32_t size;
            bool ok = p->read_file && p->read_file(PROFILE_PATH, &loaded, sizeof(loaded), &size)
                && size == sizeof(loaded) && !memcmp(loaded.magic, "B8NSH01", 8u) && loaded.count <= PROFILE_COUNT;
            if (ok) { for (uint32_t i = 0; i < loaded.count; ++i) { if (!profile_valid(&loaded.items[i])) { ok = false; break; } } }
            if (ok) { profiles = loaded; }
            else { browser.startup_error = true; snprintf(browser.message, sizeof(browser.message), "Cannot load server profiles: file is unreadable or invalid."); }
        } else if (p->file_info && p->last_error_text && strcmp(p->last_error_text(), "Directory not found.")) {
            browser.startup_error = true; snprintf(browser.message, sizeof(browser.message), "Cannot access server profiles: %s", disk_error(p));
        }
    }
    return &browser;
}
static void destroy(void *state)
{
    struct browser *s = state; book8_share_close(&s->client); book8_ntlm_clear(s, sizeof(*s));
}
const struct book8gui_app_class book8_netshare_class = {
    .name = "Local Network Share Browser", .default_width = 720u, .default_height = 460u,
    .single_instance = true, .wants_all_keys = true, .state_size = sizeof(struct browser),
    .create = create, .destroy = destroy, .render = render, .handle_event = event, .tick = tick
};
