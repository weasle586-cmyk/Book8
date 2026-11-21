#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/apps/book8_netshare.h"
#include "../src/apps/book8_explorer.h"
#include "../src/session.h"
#include "../net/smb.h"
#include "../include/syscall.h"
static struct book8_net net;
static bool online, modal, fail_write, gateway_seen, bad_login, masked_seen, created_seen;
static uint32_t writes, opens, directories, uploads, renames, deletes, folders, logins, profile_writes;
static uint8_t profile_bytes[4096];
static uint32_t profile_size;
static const char *last_error = "Directory not found.";
static char destination[512];
static uint64_t ticks;
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
void book8gui_invalidate(void) {}
void book8gui_set_modal(const struct WindowApp *app, bool value) { (void)app; modal = value; }
bool book8gui_utility_installed(uint32_t kind) { return kind == BOOK8_GUI_CODEEDIT; }
bool book8gui_open_utility(uint32_t kind, const char *path)
{ assert(kind == BOOK8_GUI_CODEEDIT && !strcmp(path, "A:\\DOWNLOADS\\TEST.C")); ++opens; return true; }
static struct WindowApp explorer;
const struct book8gui_app_class book8_explorer_class = {.name = "Explorer"};
struct WindowApp *book8gui_launch_class(const struct book8gui_app_class *cls)
{ assert(cls == &book8_explorer_class); return &explorer; }
bool book8_explorer_open_directory(const struct book8gui_platform *p, struct WindowApp *app, const char *path)
{ (void)p; assert(app == &explorer && !strcmp(path, "A:\\DOWNLOADS\\")); ++directories; return true; }
bool book8_share_connect(struct book8_share *s, struct book8_net *n, uint32_t ip, const char *share, uint64_t now)
{
    (void)now; assert(ip == BOOK8_NET_IP(192,168,7,1) && !strcmp(share, "public"));
    memset(s, 0, sizeof(*s)); s->tcp.net = n; s->tcp.peer = ip; s->state = SHARE_BUSY;
    strcpy(s->share, share);
    return true;
}
bool book8_share_connect_auth(struct book8_share *s, struct book8_net *n, uint32_t ip,
    const char *share, const char *username, const char *domain, const char *password, uint64_t now)
{
    assert(!strcmp(username, "tester") && !strcmp(domain, "WORKGROUP") && !strcmp(password, "synthetic-pass"));
    ++logins; book8_share_connect(s, n, ip, share, now); strcpy(s->username, username); strcpy(s->domain, domain);
    return true;
}
bool book8_share_directory(struct book8_share *s, const char *path, uint64_t now)
{ (void)now; assert(!strcmp(path, "docs") || !strcmp(path, "")); strcpy(s->path, path); s->state = SHARE_BUSY; return true; }
bool book8_share_download(struct book8_share *s, uint32_t index, uint64_t now)
{ (void)now; assert(index == 1u); s->state = SHARE_BUSY; s->downloading = true; return true; }
bool book8_share_upload(struct book8_share *s, const char *name, const void *data, uint32_t size, uint64_t now)
{ (void)now; assert(!strcmp(name, "LOCAL.C") && size == 4u && !memcmp(data, "code", 4u)); ++uploads; s->state = SHARE_BUSY; s->uploading = true; return true; }
void book8_share_close(struct book8_share *s) { s->state = SHARE_IDLE; s->tcp.net = NULL; }
void book8_share_poll(struct book8_share *s, uint64_t now)
{
    (void)now;
    if (s->state != SHARE_BUSY) { return; }
    if (bad_login) { s->state = SHARE_FAILED; s->auth_failed = true; strcpy(s->error, "Login failed. Check credentials."); }
    else if (s->uploading) { s->state = SHARE_UPLOADED; s->uploading = false; }
    else if (s->downloading) { s->state = SHARE_DOWNLOADED; s->downloaded = 4u; memcpy(s->data, "code", 4u); s->downloading = false; }
    else {
        s->state = SHARE_READY; s->authenticated = true; s->count = 2u;
        s->entries[0] = (struct book8_share_entry){.name = "docs", .directory = true};
        s->entries[1] = (struct book8_share_entry){.name = "TEST.C", .size = 4u, .created = 116444736000000000ull};
    }
}
static struct book8_net *network(void) { return online ? &net : NULL; }
static uint64_t clock_ticks(void) { return ticks++; }
static bool mkdir_local(const char *path)
{ assert(!strcmp(path, "A:\\DOWNLOADS") || !strcmp(path, "A:\\DOWNLOADS\\NEW")); if (strstr(path, "\\NEW")) { ++folders; } return true; }
static bool create_file(const char *path, const void *data, uint32_t size)
{
    ++writes; assert(size == 4u && !memcmp(data, "code", 4u));
    snprintf(destination, sizeof(destination), "%s", path); last_error = "Existing destination refused"; return !fail_write;
}
static bool list(const char *path, book8_directory_entry_fn visitor, void *ctx)
{ assert(!strcmp(path, "A:\\DOWNLOADS\\")); return visitor("docs", true, 0u, ctx) && visitor("LOCAL.C", false, 4u, ctx); }
static bool read_file(const char *path, void *data, uint32_t capacity, uint32_t *size)
{
    if (!strcmp(path, "A:\\NETSHARE.CFG")) { assert(profile_size && capacity >= profile_size); memcpy(data, profile_bytes, profile_size); *size = profile_size; return true; }
    assert(!strcmp(path, "A:\\DOWNLOADS\\LOCAL.C") && capacity >= 4u); memcpy(data, "code", 4u); *size = 4u; return true;
}
static bool write_file(const char *path, const void *data, uint32_t size)
{
    assert(!strcmp(path, "A:\\NETSHARE.CFG") && size <= sizeof(profile_bytes)); ++profile_writes;
    memcpy(profile_bytes, data, size); profile_size = size;
    for (uint32_t i = 0; i + strlen("synthetic-pass") <= size; ++i) { assert(memcmp(profile_bytes + i, "synthetic-pass", strlen("synthetic-pass"))); }
    return true;
}
static bool metadata(const char *path, struct book8_file_info *info)
{
    if (!strcmp(path, "A:\\NETSHARE.CFG") && !profile_size) { last_error = "Directory not found."; return false; }
    *info = (struct book8_file_info){.size = 4u, .allocated_bytes = 4096u, .allocation_valid = true,
        .creation_valid = true, .year = 2026u, .month = 11u, .day = 9u};
    return true;
}
static bool rename_local(const char *path, const char *name)
{ assert(!strcmp(path, "A:\\DOWNLOADS\\LOCAL.C") && !strcmp(name, "NEW.C")); ++renames; return true; }
static bool delete_local(const char *path)
{ assert(!strcmp(path, "A:\\DOWNLOADS\\LOCAL.C") || !strcmp(path, "A:\\DOWNLOADS\\docs")); ++deletes; return true; }
static const char *error(void) { return last_error; }
static void fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{ (void)color; assert(x + width <= 800u && y + height <= 600u); }
static void text(uint32_t x, uint32_t y, const char *value, uint32_t fg, uint32_t bg)
{
    (void)y; (void)fg; (void)bg; assert(x + strlen(value) * 8u <= 800u);
    assert(!strstr(value, "synthetic-pass"));
    if (strstr(value, "***")) { masked_seen = true; }
    if (!strcmp(value, "Created: 1970-01-01 00:00:00 UTC")) { created_seen = true; }
    if (!strcmp(value, "192.168.7.1")) { gateway_seen = true; }
}
static void key(const struct book8gui_platform *p, struct WindowApp *app, uint32_t k)
{ struct book8gui_event e = {.type = BOOK8GUI_EVENT_KEY, .key = k}; assert(app->handle_event(p, app, &e)); }
static void click(const struct book8gui_platform *p, struct WindowApp *app, int32_t x, int32_t y)
{
    struct book8gui_event e = {.type = BOOK8GUI_EVENT_MOUSE, .mouse_x = x, .mouse_y = y, .mouse_buttons = 1u};
    assert(app->handle_event(p, app, &e)); e.mouse_buttons = 0u; assert(app->handle_event(p, app, &e));
}
static void type(const struct book8gui_platform *p, struct WindowApp *app, const char *text)
{ while (*text) { key(p, app, (uint32_t)*text++); } }
int main(void)
{
    assert(book8_session_initialize(0x4e455453u));
    assert(book8_session_login("admin", "default"));
    online = true; net.ip = BOOK8_NET_IP(192,168,7,10); net.gateway = BOOK8_NET_IP(192,168,7,1);
    struct book8gui_platform p = {.width = 800u, .height = 600u, .network = network,
        .clock_ticks = clock_ticks, .clock_hz = 1000u, .make_directory = mkdir_local,
        .create_file = create_file, .last_error_text = error, .fill_rect = fill, .draw_text = text,
        .read_file = read_file, .write_file = write_file, .list_directory = list, .file_info = metadata,
        .rename_entry = rename_local, .delete_entry = delete_local,
        .session_uid = session_uid, .session_token = session_token};
    struct WindowApp app = {.cls = &book8_netshare_class, .bounds = {40u,40u,720u,460u},
        .handle_event = book8_netshare_class.handle_event};
    app.state = app.cls->create(&p); assert(app.state);
    assert(!app.cls->create(&p));
    app.cls->render(&p, &app); assert(gateway_seen);
    for (uint32_t i = 0u; i < 16u; ++i) { key(&p, &app, BOOK8GUI_KEY_BACKSPACE); }
    key(&p, &app, 'x'); key(&p, &app, BOOK8GUI_KEY_ENTER); assert(modal);
    app.cls->render(&p, &app); key(&p, &app, BOOK8GUI_KEY_ESCAPE); assert(!modal);
    key(&p, &app, BOOK8GUI_KEY_BACKSPACE);
    const char *ip = "192.168.7.1";
    for (uint32_t i = 0u; ip[i]; ++i) { key(&p, &app, (uint32_t)ip[i]); }
    online = false; key(&p, &app, BOOK8GUI_KEY_ENTER); assert(modal);
    key(&p, &app, BOOK8GUI_KEY_ENTER); online = true;
    key(&p, &app, BOOK8GUI_KEY_ENTER); assert(modal);
    click(&p, &app, 300, 410); app.cls->tick(&p, &app);
    app.cls->render(&p, &app); assert(!modal);
    click(&p, &app, 60, 218); app.cls->tick(&p, &app);
    click(&p, &app, 310, 218); click(&p, &app, 120, 150);
    app.cls->tick(&p, &app);
    assert(writes == 1u && !strcmp(destination, "A:\\DOWNLOADS\\TEST.C"));
    click(&p, &app, 250, 150); assert(opens == 1u);
    click(&p, &app, 360, 150); assert(directories == 1u);
    fail_write = true; click(&p, &app, 120, 150); app.cls->tick(&p, &app);
    assert(modal && writes == 2u);
    key(&p, &app, BOOK8GUI_KEY_ESCAPE);
    click(&p, &app, 660, 95); click(&p, &app, 300, 410); online = false; app.cls->tick(&p, &app);
    assert(modal); key(&p, &app, BOOK8GUI_KEY_ESCAPE);
    online = true;
    click(&p, &app, 660, 95); assert(modal);
    type(&p, &app, "tester"); key(&p, &app, BOOK8GUI_KEY_TAB); type(&p, &app, "WORKGROUP");
    key(&p, &app, BOOK8GUI_KEY_TAB); type(&p, &app, "synthetic-pass"); app.cls->render(&p, &app);
    key(&p, &app, BOOK8GUI_KEY_ENTER); app.cls->tick(&p, &app); assert(logins == 1u && !modal);
    click(&p, &app, 700, 178); assert(modal);
    click(&p, &app, 300, 410); assert(profile_writes == 1u); key(&p, &app, BOOK8GUI_KEY_ESCAPE);
    click(&p, &app, 60, 178); click(&p, &app, 310, 218);
    click(&p, &app, 300, 178); assert(modal); app.cls->render(&p, &app); key(&p, &app, BOOK8GUI_KEY_ESCAPE);
    click(&p, &app, 160, 178); app.cls->tick(&p, &app); app.cls->tick(&p, &app); assert(uploads == 1u);
    click(&p, &app, 310, 218);
    click(&p, &app, 350, 178); assert(modal);
    for (uint32_t i = 0; i < 7u; ++i) { key(&p, &app, BOOK8GUI_KEY_BACKSPACE); }
    type(&p, &app, "NEW.C"); key(&p, &app, BOOK8GUI_KEY_ENTER); assert(renames == 1u && !modal);
    click(&p, &app, 310, 218); click(&p, &app, 480, 178); assert(modal);
    key(&p, &app, BOOK8GUI_KEY_ESCAPE); assert(!deletes);
    click(&p, &app, 480, 178); key(&p, &app, BOOK8GUI_KEY_ENTER); assert(deletes == 1u);
    click(&p, &app, 600, 178); type(&p, &app, "../bad"); key(&p, &app, BOOK8GUI_KEY_ENTER); assert(modal && !folders);
    for (uint32_t i = 0; i < 6u; ++i) { key(&p, &app, BOOK8GUI_KEY_BACKSPACE); }
    type(&p, &app, "NEW"); key(&p, &app, BOOK8GUI_KEY_ENTER); assert(folders == 1u && !modal);
    key(&p, &app, BOOK8GUI_KEY_DOWN); click(&p, &app, 480, 178);
    key(&p, &app, BOOK8GUI_KEY_ENTER); assert(deletes == 2u);
    app.cls->destroy(app.state); app.state = app.cls->create(&p); assert(app.state);
    click(&p, &app, 700, 178); click(&p, &app, 200, 188); key(&p, &app, BOOK8GUI_KEY_ENTER);
    click(&p, &app, 660, 95); assert(!modal && logins == 2u); app.cls->tick(&p, &app);
    bad_login = true; click(&p, &app, 660, 95); app.cls->tick(&p, &app); assert(modal && logins == 3u);
    app.cls->render(&p, &app); bad_login = false;
    click(&p, &app, 300, 267); type(&p, &app, "synthetic-pass"); key(&p, &app, BOOK8GUI_KEY_ENTER); app.cls->tick(&p, &app);
    assert(!modal && logins == 4u);
    click(&p, &app, 310, 218); click(&p, &app, 300, 178); assert(modal);
    app.cls->render(&p, &app); assert(created_seen); key(&p, &app, BOOK8GUI_KEY_ESCAPE);
    click(&p, &app, 700, 178); click(&p, &app, 450, 410); assert(profile_writes == 2u);
    key(&p, &app, BOOK8GUI_KEY_ESCAPE); click(&p, &app, 660, 95); assert(modal);
    click(&p, &app, 300, 267); type(&p, &app, "synthetic-pass");
    click(&p, &app, 300, 300); key(&p, &app, BOOK8GUI_KEY_ENTER); app.cls->tick(&p, &app);
    assert(!modal && logins == 5u);
    click(&p, &app, 660, 95); assert(modal); masked_seen = false; app.cls->render(&p, &app);
    assert(!masked_seen && logins == 5u);
    key(&p, &app, BOOK8GUI_KEY_ESCAPE);
    app.bounds.width = 240u; app.bounds.height = 160u; app.cls->render(&p, &app);
    app.cls->destroy(app.state);
    puts("PASS: NetShare local operations, properties, uploads/downloads, password masking, password-free profiles, session cache, auth retry and lifecycle");
    return 0;
}
