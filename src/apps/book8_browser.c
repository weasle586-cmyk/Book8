#include "book8_browser.h"
#include "book8_ui.h"

#include <stddef.h>
#include "../../include/syscall.h"
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "book8_image.h"
#include "book8_media_extension.h"
#include "../../net/net.h"
#include "../../net/network_settings.h"
#include "../../net/tcp.h"

#define BROWSER_URL_CAPACITY 256u
#define BROWSER_RESPONSE_CAPACITY (512u * 1024u)
#define BROWSER_TEXT_CAPACITY 8192u
#define BROWSER_LINK_COUNT 24u
#define BROWSER_LINK_CAPACITY 256u
#define BROWSER_HISTORY_COUNT 8u
#define BROWSER_TEXT_RUN_COUNT 64u
#define BROWSER_TITLE_CAPACITY 160u
#define BROWSER_REDIRECT_LIMIT 4u
#define BROWSER_REFRESH_DELAY_MS 1200u
#define BROWSER_COOKIE_COUNT 4u
#define BROWSER_COOKIE_NAME_CAPACITY 40u
#define BROWSER_COOKIE_VALUE_CAPACITY 80u
#define BROWSER_IMAGE_POOL_SIZE (8u * 1024u * 1024u)
#define BROWSER_HOME_PATH "A:\\BROWSER.CFG"
#define BROWSER_DNS_PORT 53053u
#define BROWSER_DNS_QUERY_CAPACITY 512u
#define BROWSER_DNS_RETRIES 3u
#define BROWSER_DNS_TIMEOUT_MS 8000u
#define BROWSER_HOVER_NONE 0xffu

enum browser_state {
    BROWSER_HOME,
    BROWSER_RESOLVING,
    BROWSER_CONNECTING,
    BROWSER_REQUESTING,
    BROWSER_LOADING,
    BROWSER_READY,
    BROWSER_FAILED
};

struct browser_link {
    char label[80];
    char url[BROWSER_LINK_CAPACITY];
    enum book8_media_kind media_kind;
};

/* One rendered hyperlink span, addressed by logical page line and column. */
struct browser_text_run {
    uint16_t line;
    uint16_t start;
    uint16_t end;
    uint8_t link;
    bool active;
};

enum browser_icon {
    BROWSER_ICON_BACK, BROWSER_ICON_FORWARD, BROWSER_ICON_STOP,
    BROWSER_ICON_REFRESH, BROWSER_ICON_HOME, BROWSER_ICON_GO
};

struct browser_cookie {
    char host[BROWSER_URL_CAPACITY];
    char name[BROWSER_COOKIE_NAME_CAPACITY];
    char value[BROWSER_COOKIE_VALUE_CAPACITY];
    bool used;
};

struct browser_app {
    struct book8_tcp tcp;
    struct book8_net *dns_net;
    enum browser_state state;
    char address[BROWSER_URL_CAPACITY];
    char homepage[BROWSER_URL_CAPACITY];
    char base_url[BROWSER_URL_CAPACITY];
    char dns_name[BROWSER_URL_CAPACITY];
    char title[BROWSER_URL_CAPACITY + 32u];
    char page_title[BROWSER_TITLE_CAPACITY];
    char status[160];
    char redirect_target[BROWSER_URL_CAPACITY];
    char refresh_target[BROWSER_URL_CAPACITY];
    char page[BROWSER_TEXT_CAPACITY];
    char response[BROWSER_RESPONSE_CAPACITY + 1u];
    struct browser_link links[BROWSER_LINK_COUNT];
    struct browser_text_run runs[BROWSER_TEXT_RUN_COUNT];
    struct browser_cookie cookies[BROWSER_COOKIE_COUNT];
    char history[BROWSER_HISTORY_COUNT][BROWSER_URL_CAPACITY];
    uint8_t *image_pixels;
    uint32_t image_width, image_height;
    uint32_t dns_server, resolved_ip;
    uint32_t response_size, body_offset, body_size, content_size;
    uint32_t page_length, page_scroll, link_count, history_count, history_index;
    uint32_t page_line, page_column, page_lines, page_rows_total;
    uint32_t run_count, redirect_count, refresh_delay_ms;
    uint32_t text_scale, menu, dns_query_size, dns_retries;
    uint16_t dns_id;
    uint64_t dns_retry_at, dns_deadline, refresh_at;
    uint8_t dns_query[BROWSER_DNS_QUERY_CAPACITY];
    uint8_t previous_buttons, hover_nav, pressed_nav;
    int32_t hover_link;
    uint32_t cookie_replace;
    bool address_focused, content_length_known, request_sent, html_content;
    bool image_content, audio_content, video_content;
    bool chunked_content, dns_bound, dns_resolved, dns_failed;
};

struct browser_layout {
    struct book8gui_rect menu, toolbar, nav[6], address, address_field, go;
    struct book8gui_rect content, links, status;
    uint32_t page_rows, columns;
};

static struct browser_app browser;
static uint8_t browser_image_memory[BROWSER_IMAGE_POOL_SIZE]
    __attribute__((aligned(16)));
static struct book8_image_pool browser_image_pool = {
    .memory = browser_image_memory,
    .capacity = sizeof(browser_image_memory)
};
static const char *const menus[] = {
    "File", "Edit", "View", "Favorites", "Help", "Settings"
};

static bool begin_request(const struct book8gui_platform *p,
    struct WindowApp *app, const char *url, bool add_history);
static bool navigate(const struct book8gui_platform *p,
    struct WindowApp *app, const char *url, bool add_history);
static void close_connection(void);
static void response_failed(struct WindowApp *app, const char *message);
static void parse_meta_refresh(const char *tag, uint32_t length);
static void count_page_lines(void);

static char lower_ascii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch + ('a' - 'A')) : ch;
}

static bool starts_with(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text == '\0' || lower_ascii(*text++) != lower_ascii(*prefix++)) {
            return false;
        }
    }
    return true;
}

static uint64_t milliseconds(const struct book8gui_platform *p)
{
    return p->clock_ticks != NULL && p->clock_hz != 0u
        ? p->clock_ticks() * 1000u / p->clock_hz : 0u;
}

static void set_title(struct WindowApp *app)
{
    const char *name = browser.state == BROWSER_HOME ? "Start Page"
        : browser.page_title[0] != '\0' ? browser.page_title : browser.address;
    snprintf(browser.title, sizeof(browser.title), "Internet Explorer - %s", name);
    app->title = browser.title;
}

static void set_status(const char *message)
{
    snprintf(browser.status, sizeof(browser.status), "%s", message);
}

static void reset_page_state(void)
{
    browser.base_url[0] = '\0';
    browser.page_title[0] = '\0';
    browser.redirect_target[0] = '\0';
    browser.refresh_target[0] = '\0';
    browser.refresh_at = 0u;
    browser.refresh_delay_ms = BROWSER_REFRESH_DELAY_MS;
    browser.run_count = 0u;
    browser.page_line = 0u;
    browser.page_column = 0u;
    browser.page_lines = 0u;
    browser.page_rows_total = 0u;
}

static void reset_navigation_state(void)
{
    reset_page_state();
    browser.redirect_count = 0u;
}

static void show_start_page(struct WindowApp *app)
{
    close_connection();
    browser.state = BROWSER_HOME;
    browser.address[0] = '\0';
    browser.page_scroll = 0u;
    browser.link_count = 0u;
    browser.page[0] = '\0';
    browser.page_length = 0u;
    browser.image_pixels = NULL;
    browser.image_width = browser.image_height = 0u;
    book8_image_pool_release(&browser_image_pool);
    reset_navigation_state();
    snprintf(browser.page, sizeof(browser.page),
        "BOOK8 Internet Explorer\n"
        "=======================\n\n"
        "Type an HTTP address above and press Enter, or click Go.\n\n"
        "Modern HTML documents are rendered as readable text with clickable "
        "links, semantic layout, character entities, page titles, relative "
        "URLs, HTTP redirects and meta-refresh navigation.\n\n"
        "Standalone BMP/GIF/JPEG/PNG images and HTTP WAV audio are also "
        "supported. HTTPS/TLS, JavaScript, CSS layout and video codecs are "
        "not available in this build.");
    browser.page_length = (uint32_t)strlen(browser.page);
    browser.hover_link = -1;
    count_page_lines();
    set_status("Ready.");
    set_title(app);
    book8gui_invalidate();
}

static void go_home(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.homepage[0] != '\0') {
        (void)navigate(p, app, browser.homepage, true);
    } else {
        show_start_page(app);
    }
}

static void close_connection(void)
{
    if (browser.dns_bound && browser.dns_net != NULL) {
        (void)book8_udp_unbind(browser.dns_net, BROWSER_DNS_PORT);
    }
    browser.dns_net = NULL;
    browser.dns_bound = false;
    browser.dns_resolved = false;
    browser.dns_failed = false;
    if (browser.tcp.net != NULL) {
        book8_tcp_close(&browser.tcp);
    }
    browser.request_sent = false;
}

static bool valid_hostname(const char *host)
{
    uint32_t label_length = 0u;
    uint32_t total_length = 0u;
    char previous = '\0';
    for (const char *p = host; *p != '\0'; ++p) {
        char ch = *p;
        if (ch == '.') {
            if (label_length == 0u || previous == '-') {
                return false;
            }
            label_length = 0u;
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '-')
                || (label_length == 0u && ch == '-')
                || ++label_length > 63u) {
                return false;
            }
        }
        previous = ch;
        if (++total_length > 253u) {
            return false;
        }
    }
    return label_length != 0u && previous != '-';
}

/* Trim leading/trailing HTTP whitespace and reject active-content schemes. */
static bool clean_url(const char *text, char *out, uint32_t capacity)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    if (starts_with(text, "javascript:") || starts_with(text, "data:")
        || starts_with(text, "mailto:") || starts_with(text, "about:")) {
        return false;
    }
    size_t length = strlen(text);
    while (length != 0u
        && (text[length - 1u] == ' ' || text[length - 1u] == '\t'
            || text[length - 1u] == '\r' || text[length - 1u] == '\n')) {
        --length;
    }
    if (length == 0u || length + 1u > capacity) {
        return false;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return true;
}

static bool normalize_http_url(char *url, uint32_t capacity)
{
    if (!starts_with(url, "http://")) {
        return false;
    }
    char *path = url + 7u;
    while (*path != '\0' && *path != '/') {
        ++path;
    }
    if (*path == '\0') {
        if (strlen(url) + 2u > capacity) {
            return false;
        }
        strcat(url, "/");
        return true;
    }
    char *tail = path;
    while (*tail != '\0' && *tail != '?' && *tail != '#') {
        ++tail;
    }
    char *segments[32];
    uint32_t lengths[32];
    uint32_t count = 0u;
    char *at = path;
    while (at < tail) {
        if (*at == '/') {
            ++at;
            continue;
        }
        char *begin = at;
        while (at < tail && *at != '/') {
            ++at;
        }
        uint32_t length = (uint32_t)(at - begin);
        if (length == 1u && begin[0] == '.') {
            continue;
        }
        if (length == 2u && begin[0] == '.' && begin[1] == '.') {
            if (count != 0u) {
                --count;
            }
            continue;
        }
        if (count == 32u || length + 2u > capacity) {
            return false;
        }
        segments[count] = begin;
        lengths[count++] = length;
    }
    uint32_t prefix = (uint32_t)(path - url);
    char result[BROWSER_URL_CAPACITY];
    memcpy(result, url, prefix);
    uint32_t used = prefix;
    if (count == 0u && (*tail == '\0' || *tail == '?' || *tail == '#')) {
        result[used++] = '/';
    }
    for (uint32_t i = 0u; i < count; ++i) {
        result[used++] = '/';
        memcpy(result + used, segments[i], lengths[i]);
        used += lengths[i];
    }
    for (char *p = tail; *p != '\0'; ++p) {
        result[used++] = *p;
    }
    if (used + 1u > capacity || used >= sizeof(result)) {
        return false;
    }
    result[used] = '\0';
    snprintf(url, capacity, "%s", result);
    return true;
}

static bool resolve_url(const char *base, const char *href,
    char *out, uint32_t capacity)
{
    char clean[BROWSER_URL_CAPACITY];
    if (!clean_url(href, clean, sizeof(clean))) {
        return false;
    }
    if (starts_with(clean, "http://") || starts_with(clean, "https://")) {
        snprintf(out, capacity, "%s", clean);
        return strlen(clean) + 1u <= capacity;
    }
    if (clean[0] == '/' && clean[1] == '/') {
        if (snprintf(out, capacity, "http:%s", clean)
            >= (int)capacity) {
            return false;
        }
        return normalize_http_url(out, capacity);
    }
    if (!starts_with(base, "http://")) {
        return false;
    }
    const char *authority = base + 7u;
    const char *path = authority;
    while (*path != '\0' && *path != '/') {
        ++path;
    }
    uint32_t authority_length = (uint32_t)(path - authority);
    if (clean[0] == '/') {
        if (snprintf(out, capacity, "http://%.*s%s", (int)authority_length,
                authority, clean) >= (int)capacity) {
            return false;
        }
        return normalize_http_url(out, capacity);
    }
    const char *path_end = path;
    while (*path_end != '\0' && *path_end != '?' && *path_end != '#') {
        ++path_end;
    }
    if (clean[0] == '?' || clean[0] == '#') {
        uint32_t base_length = (uint32_t)(path_end - base);
        if (base_length + strlen(clean) + 1u > capacity) {
            return false;
        }
        memcpy(out, base, base_length);
        snprintf(out + base_length, capacity - base_length, "%s", clean);
        return true;
    }
    const char *directory_end = path_end;
    while (directory_end > path && directory_end[-1] != '/') {
        --directory_end;
    }
    uint32_t directory_length = (uint32_t)(directory_end - base);
    if (directory_length + strlen(clean) + 1u > capacity) {
        return false;
    }
    memcpy(out, base, directory_length);
    snprintf(out + directory_length, capacity - directory_length, "%s", clean);
    return normalize_http_url(out, capacity);
}

static bool parse_url(const char *url, uint32_t *ip, uint16_t *port,
    char *path, uint32_t path_capacity, char *host, uint32_t host_capacity)
{
    if (!starts_with(url, "http://")) {
        return false;
    }
    const char *authority = url + 7u;
    uint32_t authority_length = 0u;
    while (authority[authority_length] != '\0'
        && authority[authority_length] != '/'
        && authority[authority_length] != '?'
        && authority[authority_length] != '#') {
        if ((unsigned char)authority[authority_length] <= 32u
            || authority[authority_length] == '@') {
            return false;
        }
        ++authority_length;
    }
    if (authority_length == 0u || authority_length >= host_capacity) {
        return false;
    }
    char host_part[BROWSER_URL_CAPACITY];
    if (authority_length >= sizeof(host_part)) {
        return false;
    }
    memcpy(host_part, authority, authority_length);
    host_part[authority_length] = '\0';
    char *port_text = NULL;
    for (uint32_t i = 0u; i < authority_length; ++i) {
        if (host_part[i] == ':') {
            if (port_text != NULL) {
                return false;
            }
            host_part[i] = '\0';
            port_text = host_part + i + 1u;
        }
    }
    uint32_t parsed_port = 80u;
    if (port_text != NULL) {
        if (*port_text == '\0') {
            return false;
        }
        parsed_port = 0u;
        for (const char *p = port_text; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            parsed_port = parsed_port * 10u + (uint32_t)(*p - '0');
            if (parsed_port > 65535u) {
                return false;
            }
        }
        if (parsed_port == 0u) {
            return false;
        }
    }
    if (!book8_net_parse_ip(host_part, ip)) {
        if (!valid_hostname(host_part)) {
            return false;
        }
        *ip = 0u;
        for (uint32_t i = 0u; host_part[i] != '\0'; ++i) {
            host_part[i] = lower_ascii(host_part[i]);
        }
    }
    snprintf(host, host_capacity, "%s", host_part);
    if (port_text != NULL) {
        uint32_t n = (uint32_t)strlen(host);
        if (n + 1u >= host_capacity) {
            return false;
        }
        host[n++] = ':';
        for (const char *p = port_text; *p != '\0' && n + 1u < host_capacity; ++p) {
            host[n++] = *p;
        }
        host[n] = '\0';
    }
    const char *path_start = authority + authority_length;
    if (*path_start == '\0' || *path_start == '#') {
        snprintf(path, path_capacity, "/");
    } else if (*path_start == '?') {
        if (snprintf(path, path_capacity, "/%s", path_start) >= (int)path_capacity) {
            return false;
        }
    } else {
        if (snprintf(path, path_capacity, "%s", path_start) >= (int)path_capacity) {
            return false;
        }
    }
    for (uint32_t i = 0u; path[i] != '\0'; ++i) {
        if ((unsigned char)path[i] <= 32u || path[i] == '#') {
            return false;
        }
    }
    *port = (uint16_t)parsed_port;
    return true;
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u)
        | ((uint32_t)data[2] << 8u) | data[3];
}

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8u);
    data[1] = (uint8_t)value;
}

static bool build_dns_query(const char *host)
{
    uint32_t host_length = 0u;
    while (host[host_length] != '\0' && host[host_length] != ':') {
        ++host_length;
    }
    if (host_length == 0u || host_length >= sizeof(browser.dns_name)) {
        return false;
    }
    memcpy(browser.dns_name, host, host_length);
    browser.dns_name[host_length] = '\0';
    uint8_t *query = browser.dns_query;
    memset(query, 0, 12u);
    write_be16(query, browser.dns_id);
    write_be16(query + 2u, 0x0100u);
    write_be16(query + 4u, 1u);
    uint32_t offset = 12u;
    uint32_t label = 0u;
    for (uint32_t i = 0u; i <= host_length; ++i) {
        if (browser.dns_name[i] == '.' || browser.dns_name[i] == '\0') {
            uint32_t length = i - label;
            if (length == 0u || length > 63u
                || offset + length + 6u > sizeof(browser.dns_query)) {
                return false;
            }
            query[offset++] = (uint8_t)length;
            memcpy(query + offset, browser.dns_name + label, length);
            offset += length;
            label = i + 1u;
        }
    }
    query[offset++] = 0u;
    write_be16(query + offset, 1u);
    write_be16(query + offset + 2u, 1u);
    browser.dns_query_size = offset + 4u;
    return true;
}

static bool dns_question(const uint8_t *data, size_t size,
    uint32_t *answer_offset)
{
    uint32_t at = 12u;
    uint32_t expected = 0u;
    while (at < size) {
        uint32_t length = data[at++];
        if (length == 0u) {
            break;
        }
        if ((length & 0xc0u) != 0u || length > size - at
            || (expected != 0u && browser.dns_name[expected] != '.')) {
            return false;
        }
        if (expected != 0u) {
            ++expected;
        }
        for (uint32_t i = 0u; i < length; ++i) {
            if (browser.dns_name[expected] == '\0'
                || lower_ascii((char)data[at + i])
                    != browser.dns_name[expected]) {
                return false;
            }
            ++expected;
        }
        at += length;
    }
    if (expected != strlen(browser.dns_name) || at + 4u > size
        || read_be16(data + at) != 1u || read_be16(data + at + 2u) != 1u) {
        return false;
    }
    *answer_offset = at + 4u;
    return true;
}

static bool dns_skip_name(const uint8_t *data, size_t size,
    uint32_t *offset)
{
    uint32_t at = *offset;
    uint32_t consumed = 0u;
    uint32_t hops = 0u;
    bool jumped = false;
    for (;;) {
        if (at >= size || ++hops > 32u) {
            return false;
        }
        uint8_t length = data[at++];
        if ((length & 0xc0u) == 0xc0u) {
            if (at >= size) {
                return false;
            }
            uint32_t target = ((uint32_t)(length & 0x3fu) << 8u) | data[at++];
            if (target >= size) {
                return false;
            }
            if (!jumped) {
                consumed = at;
                jumped = true;
            }
            at = target;
        } else if (length == 0u) {
            *offset = jumped ? consumed : at;
            return true;
        } else {
            if ((length & 0xc0u) != 0u || length > size - at) {
                return false;
            }
            at += length;
        }
    }
}

static void dns_receive(void *context, uint32_t source, uint16_t source_port,
    const uint8_t *data, size_t length)
{
    struct browser_app *state = context;
    if (state != &browser || source != browser.dns_server
        || source_port != 53u || length < 12u
        || read_be16(data) != browser.dns_id) {
        return;
    }
    uint16_t flags = read_be16(data + 2u);
    if ((flags & 0x8000u) == 0u || (flags & 0x7800u) != 0u
        || (flags & 0x0200u) != 0u) {
        return;
    }
    if ((flags & 0x000fu) != 0u) {
        browser.dns_failed = true;
        return;
    }
    uint32_t at;
    if (read_be16(data + 4u) != 1u
        || !dns_question(data, length, &at)) {
        return;
    }
    uint16_t answers = read_be16(data + 6u);
    for (uint32_t i = 0u; i < answers; ++i) {
        if (!dns_skip_name(data, length, &at) || at + 10u > length) {
            return;
        }
        uint16_t type = read_be16(data + at);
        uint16_t record_class = read_be16(data + at + 2u);
        uint16_t record_length = read_be16(data + at + 8u);
        at += 10u;
        if (record_length > length - at) {
            return;
        }
        if (type == 1u && record_class == 1u && record_length == 4u) {
            uint32_t ip = read_be32(data + at);
            uint32_t first = ip >> 24u;
            if (first != 0u && first != 127u && first < 224u
                && ip != UINT32_MAX) {
                browser.resolved_ip = ip;
                browser.dns_resolved = true;
                return;
            }
        }
        at += record_length;
    }
    browser.dns_failed = true;
}

static enum book8_net_result send_dns_query(uint64_t now)
{
    enum book8_net_result result = book8_udp_send(browser.dns_net,
        BROWSER_DNS_PORT, browser.dns_server, 53u, browser.dns_query,
        browser.dns_query_size);
    if (result == BOOK8_NET_OK) {
        ++browser.dns_retries;
        browser.dns_retry_at = now + 1000u;
    } else if (result == BOOK8_NET_ARP_PENDING) {
        browser.dns_retry_at = now + 200u;
    }
    return result;
}

static bool begin_dns(const struct book8gui_platform *p,
    struct WindowApp *app, struct book8_net *net, const char *host)
{
    struct book8_network_status status;
    if (!book8_network_settings_get(&status) || !status.ready
        || status.dns == 0u) {
        set_status("DNS is unavailable; configure a DNS server in network settings.");
        browser.state = BROWSER_FAILED;
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    ++browser.dns_id;
    if (browser.dns_id == 0u) {
        ++browser.dns_id;
    }
    if (!build_dns_query(host)) {
        set_status("The hostname is too long for a DNS query.");
        browser.state = BROWSER_FAILED;
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    browser.dns_net = net;
    browser.dns_server = status.dns;
    browser.dns_retries = 0u;
    browser.dns_resolved = false;
    browser.dns_failed = false;
    enum book8_net_result result = book8_udp_bind(net, BROWSER_DNS_PORT,
        dns_receive, &browser);
    if (result != BOOK8_NET_OK) {
        browser.dns_net = NULL;
        browser.state = BROWSER_FAILED;
        set_status(book8_net_result_string(result));
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    browser.dns_bound = true;
    uint64_t now = milliseconds(p);
    browser.dns_deadline = now + BROWSER_DNS_TIMEOUT_MS;
    result = send_dns_query(now);
    if (result != BOOK8_NET_OK && result != BOOK8_NET_ARP_PENDING) {
        close_connection();
        browser.state = BROWSER_FAILED;
        set_status(book8_net_result_string(result));
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    browser.state = BROWSER_RESOLVING;
    set_status("Resolving hostname...");
    set_title(app);
    book8gui_invalidate();
    return true;
}

static bool connect_request(const struct book8gui_platform *p,
    struct WindowApp *app, struct book8_net *net, uint32_t ip)
{
    uint32_t parsed_ip;
    uint16_t port;
    char path[BROWSER_URL_CAPACITY], host[BROWSER_URL_CAPACITY];
    if (!parse_url(browser.address, &parsed_ip, &port, path, sizeof(path),
            host, sizeof(host))) {
        browser.state = BROWSER_FAILED;
        set_status("The address is not a supported HTTP URL.");
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    (void)parsed_ip;
    enum book8_net_result result = book8_tcp_connect(&browser.tcp, net, ip,
        port, milliseconds(p));
    if (result != BOOK8_NET_OK) {
        browser.state = BROWSER_FAILED;
        set_status(book8_net_result_string(result));
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    browser.state = BROWSER_CONNECTING;
    set_status("Connecting...");
    set_title(app);
    book8gui_invalidate();
    return true;
}

static bool begin_request(const struct book8gui_platform *p,
    struct WindowApp *app, const char *url, bool add_history)
{
    uint32_t ip;
    uint16_t port;
    char requested[BROWSER_URL_CAPACITY];
    char normalized[BROWSER_URL_CAPACITY];
    char path[BROWSER_URL_CAPACITY], host[BROWSER_URL_CAPACITY];
    close_connection();
    if (url == NULL || !clean_url(url, requested, sizeof(requested))
        || starts_with(requested, "https://")
        || (starts_with(requested, "http://")
            ? snprintf(normalized, sizeof(normalized), "%s", requested)
            : snprintf(normalized, sizeof(normalized), "http://%s", requested))
            >= (int)sizeof(normalized)
        || !parse_url(normalized, &ip, &port, path, sizeof(path),
            host, sizeof(host))) {
        set_status("Use an HTTP URL or hostname; HTTPS is not supported.");
        browser.state = BROWSER_FAILED;
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    (void)port;
    snprintf(browser.address, sizeof(browser.address), "%s", normalized);
    struct book8_net *net = p->network != NULL ? p->network() : NULL;
    if (net == NULL || net->ip == 0u) {
        set_status("Network is offline or IPv4 is not configured.");
        browser.state = BROWSER_FAILED;
        set_title(app);
        book8gui_invalidate();
        return false;
    }
    if (add_history) {
        if (browser.history_index + 1u < browser.history_count) {
            browser.history_count = browser.history_index + 1u;
        }
        if (browser.history_count == BROWSER_HISTORY_COUNT) {
            for (uint32_t i = 1u; i < browser.history_count; ++i) {
                memcpy(browser.history[i - 1u], browser.history[i],
                    sizeof(browser.history[i]));
            }
            --browser.history_count;
        }
        snprintf(browser.history[browser.history_count],
            sizeof(browser.history[0]), "%s", normalized);
        browser.history_index = browser.history_count++;
    }
    reset_page_state();
    snprintf(browser.base_url, sizeof(browser.base_url), "%s", normalized);
    browser.response_size = browser.body_offset = browser.body_size = 0u;
    browser.content_size = 0u;
    browser.content_length_known = false;
    browser.html_content = false;
    browser.image_content = false;
    browser.audio_content = false;
    browser.video_content = false;
    browser.chunked_content = false;
    browser.image_pixels = NULL;
    browser.image_width = browser.image_height = 0u;
    book8_image_pool_release(&browser_image_pool);
    browser.request_sent = false;
    browser.link_count = 0u;
    browser.page_scroll = 0u;
    browser.response[0] = '\0';
    browser.page[0] = '\0';
    browser.page_length = 0u;
    if (ip == 0u) {
#if BOOK8_NET_DNS_CACHE
        uint32_t cached_ip;
        if (book8_dns_cache_lookup(host, milliseconds(p), &cached_ip)) {
            return connect_request(p, app, net, cached_ip);
        }
#endif
        return begin_dns(p, app, net, host);
    }
    return connect_request(p, app, net, ip);
}

static bool navigate(const struct book8gui_platform *p,
    struct WindowApp *app, const char *url, bool add_history)
{
    reset_navigation_state();
    return begin_request(p, app, url, add_history);
}

static void save_homepage(const struct book8gui_platform *p)
{
    uint32_t ip;
    uint16_t port;
    char path[BROWSER_URL_CAPACITY], host[BROWSER_URL_CAPACITY];
    if (browser.address[0] == '\0'
        || !parse_url(browser.address, &ip, &port, path, sizeof(path),
            host, sizeof(host))) {
        set_status("Set a supported HTTP page as the current address first.");
        return;
    }
    uint32_t size = (uint32_t)strlen(browser.address);
    if (p->write_file == NULL
        || !p->write_file(BROWSER_HOME_PATH, browser.address, size)) {
        set_status("Could not save the browser homepage.");
        return;
    }
    snprintf(browser.homepage, sizeof(browser.homepage), "%s", browser.address);
    set_status("Homepage saved.");
}

static int32_t find_sequence(const char *data, uint32_t size,
    const char *needle, uint32_t needle_size)
{
    if (needle_size > size) {
        return -1;
    }
    for (uint32_t i = 0u; i <= size - needle_size; ++i) {
        if (memcmp(data + i, needle, needle_size) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_ascii(const char *data, uint32_t size,
    const char *needle)
{
    uint32_t length = (uint32_t)strlen(needle);
    if (length > size) {
        return -1;
    }
    for (uint32_t i = 0u; i <= size - length; ++i) {
        uint32_t j = 0u;
        while (j < length
            && lower_ascii(data[i + j]) == lower_ascii(needle[j])) {
            ++j;
        }
        if (j == length) {
            return (int32_t)i;
        }
    }
    return -1;
}

static bool cookie_character(char ch, bool name)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')) {
        return true;
    }
    if (name) {
        return ch == '!' || ch == '#' || ch == '$' || ch == '%'
            || ch == '&' || ch == '\'' || ch == '*' || ch == '+'
            || ch == '-' || ch == '.' || ch == '^' || ch == '_'
            || ch == '`' || ch == '|' || ch == '~';
    }
    return ch >= 33 && ch <= 126 && ch != ';' && ch != '"' && ch != '\\';
}

static bool current_cookie_host(char *host, uint32_t capacity)
{
    uint32_t ip;
    uint16_t port;
    char path[BROWSER_URL_CAPACITY];
    return parse_url(browser.address, &ip, &port, path, sizeof(path),
        host, capacity);
}

static void store_cookie(const char *header, uint32_t length)
{
    char host[BROWSER_URL_CAPACITY];
    if (!current_cookie_host(host, sizeof(host))) {
        return;
    }
    uint32_t start = 11u;
    while (start < length && (header[start] == ' ' || header[start] == '\t')) {
        ++start;
    }
    uint32_t equals = start;
    while (equals < length && header[equals] != '=' && header[equals] != ';') {
        ++equals;
    }
    if (equals == start || equals == length || header[equals] != '=') {
        return;
    }
    uint32_t value_start = equals + 1u;
    uint32_t value_end = value_start;
    while (value_end < length && header[value_end] != ';') {
        ++value_end;
    }
    while (value_end > value_start
        && (header[value_end - 1u] == ' ' || header[value_end - 1u] == '\t')) {
        --value_end;
    }
    uint32_t name_length = equals - start;
    uint32_t value_length = value_end - value_start;
    if (name_length >= BROWSER_COOKIE_NAME_CAPACITY
        || value_length >= BROWSER_COOKIE_VALUE_CAPACITY) {
        return;
    }
    for (uint32_t i = start; i < equals; ++i) {
        if (!cookie_character(header[i], true)) {
            return;
        }
    }
    for (uint32_t i = value_start; i < value_end; ++i) {
        if (!cookie_character(header[i], false)) {
            return;
        }
    }
    uint32_t slot = BROWSER_COOKIE_COUNT;
    for (uint32_t i = 0u; i < BROWSER_COOKIE_COUNT; ++i) {
        if (browser.cookies[i].used
            && strcmp(browser.cookies[i].host, host) == 0
            && strlen(browser.cookies[i].name) == name_length
            && memcmp(browser.cookies[i].name, header + start, name_length) == 0) {
            slot = i;
            break;
        }
        if (!browser.cookies[i].used && slot == BROWSER_COOKIE_COUNT) {
            slot = i;
        }
    }
    if (slot == BROWSER_COOKIE_COUNT) {
        slot = browser.cookie_replace++ % BROWSER_COOKIE_COUNT;
    }
    if (value_length == 0u) {
        browser.cookies[slot] = (struct browser_cookie){0};
        return;
    }
    struct browser_cookie *cookie = &browser.cookies[slot];
    snprintf(cookie->host, sizeof(cookie->host), "%s", host);
    memcpy(cookie->name, header + start, name_length);
    cookie->name[name_length] = '\0';
    memcpy(cookie->value, header + value_start, value_length);
    cookie->value[value_length] = '\0';
    cookie->used = true;
}

static bool build_cookie_header(const char *host, char *out,
    uint32_t capacity)
{
    uint32_t used = 0u;
    out[0] = '\0';
    for (uint32_t i = 0u; i < BROWSER_COOKIE_COUNT; ++i) {
        const struct browser_cookie *cookie = &browser.cookies[i];
        if (!cookie->used || strcmp(cookie->host, host) != 0) {
            continue;
        }
        int written = snprintf(out + used, capacity - used, "%s%s=%s",
            used == 0u ? "" : "; ", cookie->name, cookie->value);
        if (written < 0 || (uint32_t)written >= capacity - used) {
            out[0] = '\0';
            return false;
        }
        used += (uint32_t)written;
    }
    return true;
}

static bool header_value(const char *header, uint32_t length,
    uint32_t name_length, char *out, uint32_t capacity)
{
    uint32_t begin = name_length;
    while (begin < length
        && (header[begin] == ' ' || header[begin] == '\t')) {
        ++begin;
    }
    uint32_t end = length;
    while (end > begin
        && (header[end - 1u] == ' ' || header[end - 1u] == '\t'
            || header[end - 1u] == '\r')) {
        --end;
    }
    if (end == begin || end - begin + 1u > capacity) {
        return false;
    }
    memcpy(out, header + begin, end - begin);
    out[end - begin] = '\0';
    return true;
}

static bool parse_headers(void)
{
    enum book8_media_kind url_kind =
        book8_media_classify_url(browser.address);
    int32_t end = find_sequence(browser.response, browser.response_size,
        "\r\n\r\n", 4u);
    if (end < 0) {
        return false;
    }
    browser.body_offset = (uint32_t)end + 4u;
    browser.content_length_known = false;
    browser.content_size = 0u;
    browser.html_content = false;
    browser.image_content = false;
    browser.audio_content = false;
    browser.video_content = false;
    browser.chunked_content = false;
    uint32_t line = 0u;
    int32_t line_end = find_sequence(browser.response, (uint32_t)end,
        "\r\n", 2u);
    if (line_end < 0 || !starts_with(browser.response, "HTTP/")) {
        set_status("Invalid HTTP response.");
        return false;
    }
    const char *status = browser.response + 9u;
    if (browser.response_size < 12u || status[0] < '1' || status[0] > '5'
        || status[1] < '0' || status[1] > '9'
        || status[2] < '0' || status[2] > '9') {
        set_status("Invalid HTTP status line.");
        return false;
    }
    uint32_t status_code = (uint32_t)(status[0] - '0') * 100u
        + (uint32_t)(status[1] - '0') * 10u + (uint32_t)(status[2] - '0');
    browser.redirect_target[0] = '\0';
    if (status_code < 200u || status_code >= 400u) {
        snprintf(browser.status, sizeof(browser.status),
            "Server returned HTTP status %u.", status_code);
        return false;
    }
    line = (uint32_t)line_end + 2u;
    while (line < (uint32_t)end) {
        line_end = find_sequence(browser.response + line,
            (uint32_t)end - line, "\r\n", 2u);
        if (line_end < 0) {
            break;
        }
        uint32_t length = (uint32_t)line_end;
        const char *header = browser.response + line;
        if (length >= 15u && starts_with(header, "Content-Length:")) {
            uint32_t value = 0u;
            for (uint32_t i = 15u; i < length; ++i) {
                if (header[i] == ' ' || header[i] == '\t') {
                    continue;
                }
                if (header[i] < '0' || header[i] > '9') {
                    set_status("Invalid Content-Length header.");
                    return false;
                }
                uint32_t digit = (uint32_t)(header[i] - '0');
                if (value > (BROWSER_RESPONSE_CAPACITY - digit) / 10u) {
                    set_status("Response exceeds the 512 KiB size limit.");
                    return false;
                }
                value = value * 10u + digit;
            }
            browser.content_size = value;
            browser.content_length_known = true;
        } else if (length >= 18u && starts_with(header, "Transfer-Encoding:")) {
            if (find_ascii(header, length, "chunked") < 0) {
                set_status("Unsupported HTTP transfer encoding.");
                return false;
            }
            browser.chunked_content = true;
        } else if (length >= 13u && starts_with(header, "Content-Type:")) {
            uint32_t value = 13u;
            while (value < length
                && (header[value] == ' ' || header[value] == '\t')) {
                ++value;
            }
            if (starts_with(header + value, "text/html")
                || starts_with(header + value, "application/xhtml+xml")) {
                browser.html_content = true;
            } else if (starts_with(header + value, "image/")) {
                browser.image_content = true;
            } else if (starts_with(header + value, "audio/")) {
                browser.audio_content = true;
            } else if (starts_with(header + value, "video/")) {
                browser.video_content = true;
            } else if (!starts_with(header + value, "text/plain")
                && url_kind != BOOK8_MEDIA_WAV
                && url_kind != BOOK8_MEDIA_UNSUPPORTED_AUDIO
                && url_kind != BOOK8_MEDIA_VIDEO) {
                set_status("Only text, HTML and raster image content is supported.");
                return false;
            }
        } else if (length >= 10u && starts_with(header, "Location:")) {
            (void)header_value(header, length, 9u, browser.redirect_target,
                sizeof(browser.redirect_target));
        } else if (length >= 8u && starts_with(header, "Refresh:")) {
            char directive[128];
            if (header_value(header, length, 7u, directive,
                    sizeof(directive))) {
                char meta[192];
                snprintf(meta, sizeof(meta),
                    "<meta http-equiv=refresh content=\"%s\">", directive);
                parse_meta_refresh(meta, (uint32_t)strlen(meta));
            }
        } else if (length >= 11u && starts_with(header, "Set-Cookie:")) {
            store_cookie(header, length);
        }
        line += length + 2u;
    }
    if (url_kind == BOOK8_MEDIA_WAV
        || url_kind == BOOK8_MEDIA_UNSUPPORTED_AUDIO) {
        browser.audio_content = true;
    } else if (url_kind == BOOK8_MEDIA_VIDEO) {
        browser.video_content = true;
    }
    if (browser.chunked_content) {
        browser.content_length_known = false;
    }
    if (status_code >= 300u && status_code < 400u
        && browser.redirect_target[0] == '\0'
        && browser.refresh_target[0] == '\0') {
        snprintf(browser.status, sizeof(browser.status),
            "HTTP redirect %u did not include a Location.", status_code);
        return false;
    }
    return true;
}

static void follow_redirect(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    char target[BROWSER_URL_CAPACITY];
    snprintf(target, sizeof(target), "%s", browser.redirect_target);
    if (target[0] == '\0') {
        response_failed(app, "Redirect response did not include a Location.");
        return;
    }
    if (browser.redirect_count >= BROWSER_REDIRECT_LIMIT) {
        snprintf(browser.status, sizeof(browser.status),
            "Stopped after %u redirects.", BROWSER_REDIRECT_LIMIT);
        response_failed(app, browser.status);
        return;
    }
    char resolved[BROWSER_URL_CAPACITY];
    if (!resolve_url(browser.address, target, resolved, sizeof(resolved))) {
        response_failed(app, "The redirect target is not a supported HTTP URL.");
        return;
    }
    if (starts_with(resolved, "https://")) {
        response_failed(app, "HTTPS/TLS is not supported by this browser.");
        return;
    }
    ++browser.redirect_count;
    (void)begin_request(p, app, resolved, false);
}

static bool decode_chunked_body(uint32_t *decoded_size)
{
    uint32_t end = browser.response_size;
    uint32_t read = browser.body_offset;
    uint32_t write = browser.body_offset;
    for (;;) {
        int32_t line_end = find_sequence(browser.response + read, end - read,
            "\r\n", 2u);
        if (line_end < 0) {
            return false;
        }
        uint32_t line_length = (uint32_t)line_end;
        uint32_t digits = 0u;
        uint32_t chunk_size = 0u;
        while (digits < line_length && browser.response[read + digits] != ';') {
            char ch = browser.response[read + digits];
            uint32_t value;
            if (ch >= '0' && ch <= '9') {
                value = (uint32_t)(ch - '0');
            } else if (lower_ascii(ch) >= 'a' && lower_ascii(ch) <= 'f') {
                value = (uint32_t)(lower_ascii(ch) - 'a') + 10u;
            } else {
                return false;
            }
            if (chunk_size > (BROWSER_RESPONSE_CAPACITY - value) / 16u) {
                return false;
            }
            chunk_size = chunk_size * 16u + value;
            ++digits;
        }
        if (digits == 0u) {
            return false;
        }
        read += line_length + 2u;
        if (chunk_size == 0u) {
            for (;;) {
                line_end = find_sequence(browser.response + read,
                    end - read, "\r\n", 2u);
                if (line_end < 0) {
                    return false;
                }
                line_length = (uint32_t)line_end;
                read += line_length + 2u;
                if (line_length == 0u) {
                    if (read != end) {
                        return false;
                    }
                    *decoded_size = write - browser.body_offset;
                    browser.response[write] = '\0';
                    browser.response_size = write;
                    return true;
                }
            }
        }
        if (chunk_size > end - read || end - read - chunk_size < 2u
            || browser.response[read + chunk_size] != '\r'
            || browser.response[read + chunk_size + 1u] != '\n'
            || chunk_size > BROWSER_RESPONSE_CAPACITY - write) {
            return false;
        }
        memmove(browser.response + write, browser.response + read, chunk_size);
        write += chunk_size;
        read += chunk_size + 2u;
    }
}

static void append_page_char(char ch)
{
    uint32_t length = browser.page_length;
    if (length + 1u >= sizeof(browser.page)) {
        return;
    }
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        if (length == 0u || browser.page[length - 1u] != '\n') {
            browser.page[length++] = '\n';
            browser.page[length] = '\0';
            browser.page_length = length;
            ++browser.page_line;
        }
        browser.page_column = 0u;
        return;
    }
    if (ch == ' ' || ch == '\t') {
        if (length == 0u || browser.page[length - 1u] == '\n'
            || browser.page[length - 1u] == ' ') {
            return;
        }
        ch = ' ';
    }
    if ((unsigned char)ch < 32u || (unsigned char)ch > 126u) {
        ch = '?';
    }
    browser.page[length] = ch;
    browser.page[length + 1u] = '\0';
    browser.page_length = length + 1u;
    ++browser.page_column;
}

static void append_link_char(char *label, uint32_t capacity, char ch)
{
    uint32_t length = (uint32_t)strlen(label);
    if (length + 1u < capacity && ch >= 32 && ch <= 126) {
        label[length] = ch;
        label[length + 1u] = '\0';
    }
}

static bool tag_named(const char *tag, uint32_t length, const char *name)
{
    if (length != 0u && tag[0] == '/') {
        ++tag;
        --length;
    }
    uint32_t name_length = (uint32_t)strlen(name);
    if (length < name_length) {
        return false;
    }
    for (uint32_t i = 0u; i < name_length; ++i) {
        if (lower_ascii(tag[i]) != lower_ascii(name[i])) {
            return false;
        }
    }
    if (length == name_length) {
        return true;
    }
    char next = tag[name_length];
    return next == ' ' || next == '\t' || next == '\r' || next == '\n'
        || next == '/';
}

static bool tag_is_closing(const char *tag, uint32_t length)
{
    return length != 0u && tag[0] == '/';
}

static bool open_tag(const char *tag, uint32_t length, const char *name)
{
    return !tag_is_closing(tag, length) && tag_named(tag, length, name);
}

static bool close_tag(const char *tag, uint32_t length, const char *name)
{
    return tag_is_closing(tag, length) && tag_named(tag, length, name);
}

static bool block_tag(const char *tag, uint32_t length)
{
    static const char *const names[] = {
        "address", "article", "aside", "blockquote", "br", "caption",
        "dd", "details", "dialog", "div", "dl", "dt", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
        "h4", "h5", "h6", "header", "hgroup", "hr", "li", "main",
        "nav", "ol", "p", "pre", "section", "summary", "table", "td",
        "th", "tr", "ul"
    };
    for (uint32_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (tag_named(tag, length, names[i])) {
            return true;
        }
    }
    return false;
}

static bool tag_attribute(const char *tag, uint32_t length,
    const char *name, char *value, uint32_t capacity)
{
    uint32_t name_length = (uint32_t)strlen(name);
    for (uint32_t i = 0u; i + name_length < length; ++i) {
        if (i != 0u && tag[i - 1u] != ' ' && tag[i - 1u] != '\t'
            && tag[i - 1u] != '\n') {
            continue;
        }
        uint32_t j = 0u;
        while (j < name_length
            && lower_ascii(tag[i + j]) == lower_ascii(name[j])) {
            ++j;
        }
        if (j != name_length) {
            continue;
        }
        uint32_t at = i + name_length;
        while (at < length && (tag[at] == ' ' || tag[at] == '\t')) {
            ++at;
        }
        if (at == length || tag[at++] != '=') {
            continue;
        }
        while (at < length && (tag[at] == ' ' || tag[at] == '\t')) {
            ++at;
        }
        char quote = tag[at] == '"' || tag[at] == '\'' ? tag[at++] : '\0';
        uint32_t begin = at;
        while (at < length && (quote != '\0'
            ? tag[at] != quote : tag[at] != ' ' && tag[at] != '\t')) {
            ++at;
        }
        uint32_t copy = at - begin;
        if (copy == 0u || copy >= capacity) {
            return false;
        }
        memcpy(value, tag + begin, copy);
        value[copy] = '\0';
        return true;
    }
    return false;
}

static void set_link_label(uint32_t index, const char *label)
{
    if (index >= browser.link_count) {
        return;
    }
    struct browser_link *link = &browser.links[index];
    const char *display = label[0] != '\0' ? label : link->url;
    if (link->media_kind == BOOK8_MEDIA_WAV
        && !starts_with(display, "[Audio")) {
        snprintf(link->label, sizeof(link->label), "[Audio WAV] %.64s", display);
    } else if (link->media_kind == BOOK8_MEDIA_VIDEO
        && !starts_with(display, "[Video")) {
        snprintf(link->label, sizeof(link->label),
            "[Video unsupported] %.56s", display);
    } else if (link->media_kind == BOOK8_MEDIA_UNSUPPORTED_AUDIO
        && !starts_with(display, "[Audio")) {
        snprintf(link->label, sizeof(link->label),
            "[Audio unsupported] %.58s", display);
    } else {
        snprintf(link->label, sizeof(link->label), "%s", display);
    }
}

static int32_t add_link(const char *url, const char *label,
    enum book8_media_kind media_kind)
{
    if (url[0] == '\0') {
        return -1;
    }
    if (media_kind == BOOK8_MEDIA_OTHER) {
        media_kind = book8_media_classify_url(url);
    }
    for (uint32_t i = 0u; i < browser.link_count; ++i) {
        if (strcmp(browser.links[i].url, url) == 0) {
            if (label[0] != '\0') {
                set_link_label(i, label);
            }
            return (int32_t)i;
        }
    }
    if (browser.link_count == BROWSER_LINK_COUNT) {
        return -1;
    }
    uint32_t index = browser.link_count++;
    struct browser_link *link = &browser.links[index];
    snprintf(link->url, sizeof(link->url), "%s", url);
    link->media_kind = media_kind;
    set_link_label(index, label);
    return (int32_t)index;
}

static bool add_media_source(void *context,
    const struct book8_media_source *source)
{
    (void)context;
    add_link(source->url, source->label, source->kind);
    return true;
}

static bool entity_name(const char *text, uint32_t length, const char *name)
{
    uint32_t name_length = (uint32_t)strlen(name);
    if (length != name_length) {
        return false;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        if (lower_ascii(text[i]) != lower_ascii(name[i])) {
            return false;
        }
    }
    return true;
}

static uint32_t decode_entity(const char *body, uint32_t size,
    uint32_t *index, char out[4])
{
    uint32_t at = *index + 1u;
    if (at >= size) {
        out[0] = '&';
        return 1u;
    }
    uint32_t semicolon = at;
    uint32_t limit = at + 12u < size ? at + 12u : size;
    while (semicolon < limit && body[semicolon] != ';') {
        ++semicolon;
    }
    if (semicolon == limit) {
        out[0] = '&';
        return 1u;
    }
    uint32_t length = semicolon - at;
    const char *name = body + at;
    *index = semicolon;
    if (length != 0u && body[at] == '#') {
        uint32_t value = 0u;
        uint32_t digit_at = at + 1u;
        unsigned base = 10u;
        if (digit_at < semicolon && (body[digit_at] == 'x'
                || body[digit_at] == 'X')) {
            base = 16u;
            ++digit_at;
        }
        bool valid = digit_at < semicolon;
        for (; digit_at < semicolon; ++digit_at) {
            char ch = body[digit_at];
            uint32_t digit;
            if (ch >= '0' && ch <= '9') {
                digit = (uint32_t)(ch - '0');
            } else if (base == 16u && lower_ascii(ch) >= 'a'
                && lower_ascii(ch) <= 'f') {
                digit = (uint32_t)(lower_ascii(ch) - 'a') + 10u;
            } else {
                valid = false;
                break;
            }
            if (digit >= base || value > (0x10ffffu - digit) / base) {
                valid = false;
                break;
            }
            value = value * base + digit;
        }
        if (!valid) {
            out[0] = '?';
            *index = semicolon;
            return 1u;
        }
        if (value == 160u || value == 8201u || value == 8202u) {
            out[0] = ' ';
            return 1u;
        }
        if (value == 8211u || value == 8212u || value == 8722u) {
            out[0] = '-';
            return 1u;
        }
        if (value == 8216u || value == 8217u || value == 8218u) {
            out[0] = '\'';
            return 1u;
        }
        if (value == 8220u || value == 8221u || value == 8222u) {
            out[0] = '"';
            return 1u;
        }
        if (value == 8230u) {
            memcpy(out, "...", 3u);
            *index = semicolon;
            return 3u;
        }
        if (value == 8482u) {
            memcpy(out, "(tm)", 4u);
            *index = semicolon;
            return 4u;
        }
        if (value < 128u) {
            out[0] = (char)value;
            return 1u;
        }
        if (value >= 0xc0u && value <= 0xd6u) {
            out[0] = (char)('A' + (value - 0xc0u));
        } else if (value >= 0xd8u && value <= 0xdeu) {
            out[0] = (char)('A' + (value - 0xd7u));
        } else if (value >= 0xe0u && value <= 0xf6u) {
            out[0] = (char)('a' + (value - 0xe0u));
        } else if (value >= 0xf8u && value <= 0xfeu) {
            out[0] = (char)('a' + (value - 0xf7u));
        } else {
            out[0] = '?';
        }
        *index = semicolon;
        return 1u;
    }
    if (entity_name(name, length, "amp")) { out[0] = '&'; }
    else if (entity_name(name, length, "lt")) { out[0] = '<'; }
    else if (entity_name(name, length, "gt")) { out[0] = '>'; }
    else if (entity_name(name, length, "quot")) { out[0] = '"'; }
    else if (entity_name(name, length, "apos")) { out[0] = '\''; }
    else if (entity_name(name, length, "nbsp")) { out[0] = ' '; }
    else if (entity_name(name, length, "hellip")) {
        memcpy(out, "...", 3u);
        *index = semicolon;
        return 3u;
    } else if (entity_name(name, length, "copy")) {
        memcpy(out, "(c)", 3u);
        *index = semicolon;
        return 3u;
    } else if (entity_name(name, length, "reg")) {
        memcpy(out, "(r)", 3u);
        *index = semicolon;
        return 3u;
    } else if (entity_name(name, length, "trade")) {
        memcpy(out, "(tm)", 4u);
        *index = semicolon;
        return 4u;
    } else if (entity_name(name, length, "mdash")
        || entity_name(name, length, "ndash")) { out[0] = '-'; }
    else if (entity_name(name, length, "laquo")) { out[0] = '<'; }
    else if (entity_name(name, length, "raquo")) { out[0] = '>'; }
    else if (entity_name(name, length, "lsquo")
        || entity_name(name, length, "rsquo")) { out[0] = '\''; }
    else if (entity_name(name, length, "ldquo")
        || entity_name(name, length, "rdquo")) { out[0] = '"'; }
    else { out[0] = '?'; }
    *index = semicolon;
    return 1u;
}

static void decode_entities(const char *text, uint32_t length,
    char *out, uint32_t capacity)
{
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < length && used + 1u < capacity;) {
        if (text[i] == '&') {
            char decoded[4];
            uint32_t before = i;
            uint32_t count = decode_entity(text, length, &i, decoded);
            if (i == before) {
                i = before + 1u;
                count = 1u;
            } else {
                ++i;
            }
            for (uint32_t j = 0u; j < count && used + 1u < capacity; ++j) {
                out[used++] = decoded[j];
            }
        } else {
            char ch = text[i++];
            out[used++] = ch == '\r' ? ' ' : ch;
        }
    }
    out[used] = '\0';
}

static void attribute_text(const char *tag, uint32_t length,
    const char *name, char *out, uint32_t capacity)
{
    char raw[BROWSER_LINK_CAPACITY];
    out[0] = '\0';
    if (tag_attribute(tag, length, name, raw, sizeof(raw))) {
        decode_entities(raw, (uint32_t)strlen(raw), out, capacity);
    }
}

static void set_base_url(const char *href)
{
    char resolved[BROWSER_URL_CAPACITY];
    const char *base = browser.base_url[0] != '\0' ? browser.base_url
        : browser.address;
    if (resolve_url(base, href, resolved, sizeof(resolved))) {
        snprintf(browser.base_url, sizeof(browser.base_url), "%s", resolved);
    }
}

static void append_title_char(char ch)
{
    uint32_t length = (uint32_t)strlen(browser.page_title);
    if (length + 1u >= sizeof(browser.page_title)) {
        return;
    }
    if (ch == '\r') {
        return;
    }
    if (ch == '\n' || ch == '\t') {
        ch = ' ';
    }
    if (length != 0u && browser.page_title[length - 1u] == ' ' && ch == ' ') {
        return;
    }
    if ((unsigned char)ch < 32u || (unsigned char)ch > 126u) {
        ch = '?';
    }
    browser.page_title[length] = ch;
    browser.page_title[length + 1u] = '\0';
}

static void finish_link_run(struct browser_text_run *run)
{
    if (!run->active || run->end <= run->start
        || browser.run_count == BROWSER_TEXT_RUN_COUNT) {
        return;
    }
    browser.runs[browser.run_count++] = (struct browser_text_run){
        .line = run->line, .start = run->start, .end = run->end,
        .link = run->link
    };
}

static void append_linked_char(char ch, int32_t link,
    struct browser_text_run *run)
{
    uint16_t line = (uint16_t)browser.page_line;
    uint16_t column = (uint16_t)browser.page_column;
    append_page_char(ch);
    if (link < 0) {
        return;
    }
    if (line != browser.page_line) {
        finish_link_run(run);
        run->active = false;
        run->link = (uint8_t)link;
        if (ch == '\n') {
            return;
        }
    }
    if (column == browser.page_column) {
        return;
    }
    if (!run->active) {
        run->active = true;
        run->line = line;
        run->start = column;
        run->link = (uint8_t)link;
    }
    run->end = (uint16_t)browser.page_column;
}

static void append_text(const char *text, uint32_t length, int32_t link,
    struct browser_text_run *run)
{
    for (uint32_t i = 0u; i < length;) {
        if (text[i] == '&') {
            char decoded[4];
            uint32_t before = i;
            uint32_t count = decode_entity(text, length, &i, decoded);
            if (i == before) {
                i = before + 1u;
                count = 1u;
                decoded[0] = '&';
            } else {
                ++i;
            }
            for (uint32_t j = 0u; j < count; ++j) {
                append_linked_char(decoded[j], link, run);
            }
        } else {
            append_linked_char(text[i++], link, run);
        }
    }
}

static void parse_meta_refresh(const char *tag, uint32_t length)
{
    char directive[24], content[BROWSER_URL_CAPACITY];
    if (!tag_attribute(tag, length, "http-equiv", directive, sizeof(directive))
        || !entity_name(directive, (uint32_t)strlen(directive), "refresh")
        || !tag_attribute(tag, length, "content", content, sizeof(content))) {
        return;
    }
    uint32_t content_length = (uint32_t)strlen(content);
    uint32_t at = 0u;
    uint32_t seconds = 0u;
    bool have_delay = false;
    while (at < content_length && content[at] >= '0' && content[at] <= '9') {
        have_delay = true;
        seconds = seconds * 10u + (uint32_t)(content[at] - '0');
        if (seconds > 60u) {
            seconds = 60u;
        }
        ++at;
    }
    while (at < content_length && (content[at] == ' ' || content[at] == '\t'
        || content[at] == ';' || content[at] == ',')) {
        ++at;
    }
    if (at + 3u < content_length && lower_ascii(content[at]) == 'u'
        && lower_ascii(content[at + 1u]) == 'r'
        && lower_ascii(content[at + 2u]) == 'l') {
        at += 3u;
        while (at < content_length && (content[at] == ' '
            || content[at] == '\t' || content[at] == '=')) {
            ++at;
        }
    }
    if (at < content_length && (content[at] == '"' || content[at] == '\'')) {
        ++at;
    }
    char target[BROWSER_URL_CAPACITY];
    uint32_t length_out = 0u;
    while (at < content_length && content[at] != '"' && content[at] != '\''
        && length_out + 1u < sizeof(target)) {
        target[length_out++] = content[at++];
    }
    target[length_out] = '\0';
    char resolved[BROWSER_URL_CAPACITY];
    if (!resolve_url(browser.base_url[0] != '\0' ? browser.base_url
            : browser.address, target, resolved, sizeof(resolved))) {
        return;
    }
    snprintf(browser.refresh_target, sizeof(browser.refresh_target), "%s",
        resolved);
    browser.refresh_delay_ms = have_delay ? seconds * 1000u
        : BROWSER_REFRESH_DELAY_MS;
    if (browser.refresh_delay_ms == 0u) {
        browser.refresh_delay_ms = BROWSER_REFRESH_DELAY_MS;
    }
}

static int32_t linked_url(const char *href)
{
    char resolved[BROWSER_URL_CAPACITY];
    const char *base = browser.base_url[0] != '\0' ? browser.base_url
        : browser.address;
    if (!resolve_url(base, href, resolved, sizeof(resolved))
        || starts_with(resolved, "https://")) {
        return -1;
    }
    return add_link(resolved, "", BOOK8_MEDIA_OTHER);
}

static void html_to_text(const char *body, uint32_t size)
{
    browser.page[0] = '\0';
    browser.page_length = 0u;
    browser.link_count = 0u;
    browser.run_count = 0u;
    browser.page_line = 0u;
    browser.page_column = 0u;
    bool in_head = false, in_title = false, skip_script = false;
    bool in_anchor = false;
    int32_t anchor_link = -1;
    char anchor_label[80] = {0};
    struct browser_text_run run = {0};
    for (uint32_t i = 0u; i < size;) {
        if (body[i] == '<') {
            uint32_t end = i + 1u;
            while (end < size && body[end] != '>') {
                ++end;
            }
            if (end == size) {
                break;
            }
            const char *tag = body + i + 1u;
            uint32_t length = end - i - 1u;
            if (open_tag(tag, length, "script")
                || open_tag(tag, length, "style")) {
                skip_script = true;
            } else if (close_tag(tag, length, "script")
                || close_tag(tag, length, "style")) {
                skip_script = false;
            }
            if (open_tag(tag, length, "head")) {
                in_head = true;
            } else if (close_tag(tag, length, "head")) {
                in_head = false;
            } else if (open_tag(tag, length, "title")) {
                in_title = true;
            } else if (close_tag(tag, length, "title")) {
                in_title = false;
            } else if (!skip_script && open_tag(tag, length, "meta")) {
                parse_meta_refresh(tag, length);
            } else if (!skip_script && open_tag(tag, length, "base")) {
                char href[BROWSER_LINK_CAPACITY];
                attribute_text(tag, length, "href", href, sizeof(href));
                if (href[0] != '\0') {
                    set_base_url(href);
                }
            } else if (!skip_script && !in_head) {
                if (open_tag(tag, length, "a")) {
                    finish_link_run(&run);
                    run.active = false;
                    char href[BROWSER_LINK_CAPACITY];
                    attribute_text(tag, length, "href", href, sizeof(href));
                    anchor_link = href[0] != '\0' ? linked_url(href) : -1;
                    in_anchor = true;
                    anchor_label[0] = '\0';
                } else if (close_tag(tag, length, "a")) {
                    if (anchor_link >= 0) {
                        set_link_label((uint32_t)anchor_link, anchor_label);
                    }
                    finish_link_run(&run);
                    run.active = false;
                    in_anchor = false;
                    anchor_link = -1;
                } else if (open_tag(tag, length, "img")) {
                    char src[BROWSER_LINK_CAPACITY], alt[80];
                    attribute_text(tag, length, "src", src, sizeof(src));
                    attribute_text(tag, length, "alt", alt, sizeof(alt));
                    if (alt[0] == '\0') {
                        snprintf(alt, sizeof(alt), "Image");
                    }
                    if (src[0] != '\0') {
                        int32_t image = linked_url(src);
                        char label[96];
                        snprintf(label, sizeof(label), "[Image: %.70s]", alt);
                        append_page_char('\n');
                        append_text(label, (uint32_t)strlen(label), image, &run);
                        append_page_char('\n');
                    }
                } else if (open_tag(tag, length, "li")) {
                    finish_link_run(&run);
                    run.active = false;
                    append_page_char('\n');
                    append_text("- ", 2u, -1, &run);
                } else if (open_tag(tag, length, "td")
                    || open_tag(tag, length, "th")) {
                    append_text(" | ", 3u, -1, &run);
                } else if (open_tag(tag, length, "hr")) {
                    finish_link_run(&run);
                    run.active = false;
                    append_page_char('\n');
                    append_text("--------------------", 20u, -1, &run);
                    append_page_char('\n');
                } else if (open_tag(tag, length, "h1")
                    || open_tag(tag, length, "h2")
                    || open_tag(tag, length, "h3")
                    || open_tag(tag, length, "h4")
                    || open_tag(tag, length, "h5")
                    || open_tag(tag, length, "h6")) {
                    finish_link_run(&run);
                    run.active = false;
                    append_page_char('\n');
                    char heading[8];
                    snprintf(heading, sizeof(heading), "%c%c: ",
                        lower_ascii(tag[0]), lower_ascii(tag[1]));
                    append_text(heading, (uint32_t)strlen(heading), -1, &run);
                } else if (block_tag(tag, length)) {
                    finish_link_run(&run);
                    run.active = false;
                    append_page_char('\n');
                }
            }
            i = end + 1u;
            continue;
        }
        if (skip_script) {
            ++i;
            continue;
        }
        uint32_t at = i;
        if (body[i] == '&') {
            char decoded[4];
            uint32_t count = decode_entity(body, size, &at, decoded);
            if (at == i) {
                count = 1u;
                decoded[0] = '&';
                at = i + 1u;
            } else {
                ++at;
            }
            if (in_title) {
                for (uint32_t j = 0u; j < count; ++j) {
                    append_title_char(decoded[j]);
                }
            } else if (!in_head) {
                for (uint32_t j = 0u; j < count; ++j) {
                    append_linked_char(decoded[j], in_anchor ? anchor_link : -1,
                        &run);
                    append_link_char(anchor_label, sizeof(anchor_label),
                        decoded[j]);
                }
            }
            i = at;
            continue;
        }
        if (in_title) {
            append_title_char(body[i]);
        } else if (!in_head) {
            append_linked_char(body[i], in_anchor ? anchor_link : -1, &run);
            append_link_char(anchor_label, sizeof(anchor_label), body[i]);
        }
        ++i;
    }
    finish_link_run(&run);
    while (browser.page_length != 0u
        && browser.page[browser.page_length - 1u] == '\n') {
        browser.page[--browser.page_length] = '\0';
    }
    browser.page_lines = browser.page[0] == '\0' ? 0u : 1u;
    for (uint32_t i = 0u; i < browser.page_length; ++i) {
        if (browser.page[i] == '\n') {
            ++browser.page_lines;
        }
    }
    browser.page_rows_total = browser.page_lines;
    if ((book8_media_prefs()->web & BOOK8_MEDIA_WEB_MEDIA_PANEL) != 0u) {
        (void)book8_media_scan_html(body, size, add_media_source, NULL);
    }
}

static void count_page_lines(void)
{
    browser.page_lines = browser.page[0] == '\0' ? 0u : 1u;
    for (uint32_t i = 0u; i < browser.page_length; ++i) {
        if (browser.page[i] == '\n') {
            ++browser.page_lines;
        }
    }
    browser.page_rows_total = browser.page_lines;
}

static void display_body(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    uint32_t size = browser.body_size;
    const char *body = browser.response + browser.body_offset;
    if (browser.video_content) {
        snprintf(browser.page, sizeof(browser.page),
            "Video playback is unsupported. BOOK8 does not include a video decoder.");
        browser.page_length = (uint32_t)strlen(browser.page);
    } else if (browser.audio_content) {
        const struct book8_media_preferences *prefs = book8_media_prefs();
        bool plugin_enabled =
            (prefs->plugins & BOOK8_MEDIA_PLUGIN_HTTP_WAV) != 0u;
        bool autoplay_enabled =
            (prefs->web & BOOK8_MEDIA_WEB_AUTOPLAY) != 0u;
        bool played = plugin_enabled && autoplay_enabled
            && p->media_play_wav != NULL
            && p->media_play_wav((const uint8_t *)body, size);
        if (played) {
            snprintf(browser.page, sizeof(browser.page),
                "WAV audio is playing through the HDA output.\n"
                "Open BOOK8 Player to control playback.");
            set_status("Playing WAV audio.");
            browser.state = BROWSER_READY;
        } else if (!plugin_enabled) {
            snprintf(browser.page, sizeof(browser.page),
                "HTTP WAV playback is disabled.\n"
                "Enable the HTTP stream plugin in BOOK8 Player Preferences.");
            set_status("Web audio plugin disabled.");
        } else if (!autoplay_enabled) {
            snprintf(browser.page, sizeof(browser.page),
                "Web audio autoplay is disabled.\n"
                "Enable it in BOOK8 Player Preferences to play HTTP WAV files.");
            set_status("Web audio autoplay disabled.");
        } else {
            snprintf(browser.page, sizeof(browser.page),
                "Audio playback supports PCM 48 kHz stereo 16-bit WAV only.");
            set_status("Web audio format is unsupported.");
        }
        browser.page_length = (uint32_t)strlen(browser.page);
        set_title(app);
        book8gui_invalidate();
        return;
    } else if (browser.image_content) {
        struct book8_decoded_image image;
        if (!book8_image_decode_with_pool((const uint8_t *)body, size,
                &image, &browser_image_pool)) {
            browser.state = BROWSER_FAILED;
            set_status("Image decode failed or the image exceeds 1024x768.");
            set_title(app);
            book8gui_invalidate();
            return;
        }
        browser.image_pixels = image.rgba;
        browser.image_width = image.width;
        browser.image_height = image.height;
        snprintf(browser.page, sizeof(browser.page), "Image: %ux%u",
            image.width, image.height);
        browser.page_length = (uint32_t)strlen(browser.page);
    } else if (browser.html_content || (size >= 5u
        && (memcmp(body, "<html", 5u) == 0
            || memcmp(body, "<HTML", 5u) == 0
            || memcmp(body, "<!DOC", 5u) == 0))) {
        html_to_text(body, size);
    } else {
        browser.page[0] = '\0';
        browser.page_length = 0u;
        for (uint32_t i = 0u; i < size; ++i) {
            append_page_char(body[i]);
        }
    }
    if (!browser.image_content && browser.page[0] == '\0') {
        snprintf(browser.page, sizeof(browser.page), "(The page contains no displayable text.)");
        browser.page_length = (uint32_t)strlen(browser.page);
    }
    count_page_lines();
    if (browser.refresh_target[0] != '\0') {
        browser.refresh_at = milliseconds(p) + browser.refresh_delay_ms;
    }
    browser.state = BROWSER_READY;
    if (browser.video_content) {
        set_status("Video playback is unsupported.");
    } else if (browser.audio_content) {
        set_status("Unsupported audio format or HDA output unavailable.");
    } else if (browser.image_content) {
        set_status("Image loaded.");
    } else {
        set_status("Done.");
    }
    set_title(app);
    book8gui_invalidate();
}

static void response_failed(struct WindowApp *app, const char *message)
{
    close_connection();
    browser.state = BROWSER_FAILED;
    set_status(message);
    set_title(app);
    book8gui_invalidate();
}

static void finish_response(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    if (!parse_headers()) {
        char message[sizeof(browser.status)];
        snprintf(message, sizeof(message), "%s", browser.status);
        response_failed(app, message);
        return;
    }
    if (browser.redirect_target[0] != '\0') {
        follow_redirect(p, app);
        return;
    }
    if (browser.chunked_content) {
        if (!decode_chunked_body(&browser.body_size)) {
            response_failed(app, "Invalid or incomplete chunked HTTP response.");
            return;
        }
    } else if (!browser.content_length_known) {
        browser.body_size = browser.response_size - browser.body_offset;
    } else {
        if (browser.response_size < browser.body_offset + browser.content_size) {
            response_failed(app, "Server closed before the page was complete.");
            return;
        }
        browser.body_size = browser.content_size;
    }
    display_body(p, app);
    close_connection();
}

static void tick_dns(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    struct book8_net *net = p->network != NULL ? p->network() : NULL;
    if (net == NULL || net != browser.dns_net || net->ip == 0u) {
        response_failed(app, "Network went offline during DNS lookup.");
        return;
    }
    if (browser.dns_failed) {
        response_failed(app, "DNS lookup failed for this hostname.");
        return;
    }
    if (browser.dns_resolved) {
        uint32_t ip = browser.resolved_ip;
        (void)book8_udp_unbind(browser.dns_net, BROWSER_DNS_PORT);
        browser.dns_net = NULL;
        browser.dns_bound = false;
        browser.dns_resolved = false;
#if BOOK8_NET_DNS_CACHE
        book8_dns_cache_insert(browser.dns_name, ip, milliseconds(p));
#endif
        (void)connect_request(p, app, net, ip);
        return;
    }
    uint64_t now = milliseconds(p);
    if (now >= browser.dns_deadline
        || (browser.dns_retries >= BROWSER_DNS_RETRIES
            && now >= browser.dns_retry_at)) {
        response_failed(app, "DNS lookup timed out.");
        return;
    }
    if (now >= browser.dns_retry_at) {
        enum book8_net_result result = send_dns_query(now);
        if (result != BOOK8_NET_OK && result != BOOK8_NET_ARP_PENDING) {
            response_failed(app, book8_net_result_string(result));
        }
    }
}

static void tick(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.state == BROWSER_READY && browser.refresh_target[0] != '\0'
        && milliseconds(p) >= browser.refresh_at) {
        char target[BROWSER_URL_CAPACITY];
        snprintf(target, sizeof(target), "%s", browser.refresh_target);
        (void)navigate(p, app, target, false);
        return;
    }
    if (browser.state == BROWSER_RESOLVING) {
        tick_dns(p, app);
        return;
    }
    if (browser.state != BROWSER_CONNECTING
        && browser.state != BROWSER_REQUESTING
        && browser.state != BROWSER_LOADING) {
        return;
    }
    struct book8_net *net = p->network != NULL ? p->network() : NULL;
    if (net == NULL || net != browser.tcp.net) {
        response_failed(app, "Network went offline during the request.");
        return;
    }
    book8_tcp_poll(&browser.tcp, milliseconds(p));
    if (browser.tcp.state == TCP_CONNECTED && !browser.request_sent) {
        uint32_t ip;
        uint16_t port;
        char path[BROWSER_URL_CAPACITY], host[BROWSER_URL_CAPACITY];
        char request[BOOK8_TCP_MSS];
        char cookie_header[512];
        if (!parse_url(browser.address, &ip, &port, path, sizeof(path),
                host, sizeof(host))) {
            response_failed(app, "The address is not a supported HTTP URL.");
            return;
        }
        if (!build_cookie_header(host, cookie_header, sizeof(cookie_header))) {
            response_failed(app, "Stored cookies exceed the request header limit.");
            return;
        }
        int length = snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "User-Agent: Mozilla/5.0 (compatible; BOOK8 Internet Explorer)\r\n"
            "Accept-Encoding: identity\r\n"
            "Accept: text/html,application/xhtml+xml,text/plain,image/*,*/*;q=0.1\r\n%s%s\r\n",
            path, host, cookie_header[0] != '\0' ? "Cookie: " : "",
            cookie_header[0] != '\0' ? cookie_header : "");
        if (length < 0 || (uint32_t)length >= sizeof(request)
            || book8_tcp_send(&browser.tcp, request, (uint32_t)length,
                milliseconds(p)) != BOOK8_NET_OK) {
            response_failed(app, "Could not send the HTTP request.");
            return;
        }
        browser.request_sent = true;
        browser.state = BROWSER_LOADING;
        set_status("Waiting for the server...");
    }
    if (browser.tcp.rx_length != 0u) {
        uint32_t available = browser.tcp.rx_length;
        uint32_t remaining = BROWSER_RESPONSE_CAPACITY
            - browser.response_size;
        uint32_t copy = available < remaining ? available : remaining;
        if (copy == 0u) {
            response_failed(app, "Response exceeds the 512 KiB size limit.");
            return;
        }
        memcpy(browser.response + browser.response_size, browser.tcp.rx, copy);
        browser.response_size += copy;
        browser.response[browser.response_size] = '\0';
        book8_tcp_consume(&browser.tcp, copy);
        int32_t header_end = find_sequence(browser.response, browser.response_size,
            "\r\n\r\n", 4u);
        if (header_end >= 0 && browser.body_offset == 0u) {
            if (!parse_headers()) {
                char message[sizeof(browser.status)];
                snprintf(message, sizeof(message), "%s", browser.status);
                response_failed(app, message);
                return;
            }
            if (browser.redirect_target[0] != '\0') {
                follow_redirect(p, app);
                return;
            }
        }
        if (header_end >= 0 && browser.content_length_known
            && browser.response_size >= browser.body_offset + browser.content_size) {
            finish_response(p, app);
            return;
        }
    }
    if (browser.tcp.state == TCP_FAILED) {
        if (browser.tcp.error == BOOK8_NET_REMOTE_ERROR
            && find_sequence(browser.response, browser.response_size,
                "\r\n\r\n", 4u) >= 0) {
            finish_response(p, app);
        } else {
            response_failed(app, book8_net_result_string(browser.tcp.error));
        }
    }
}

static void follow_link(uint32_t index, const struct book8gui_platform *p,
    struct WindowApp *app)
{
    if (index >= browser.link_count) {
        return;
    }
    if (browser.links[index].media_kind == BOOK8_MEDIA_VIDEO) {
        browser.state = BROWSER_READY;
        set_status("Video playback is unsupported.");
        book8gui_invalidate();
        return;
    }
    if (browser.links[index].media_kind == BOOK8_MEDIA_UNSUPPORTED_AUDIO) {
        browser.state = BROWSER_READY;
        set_status("Only PCM 48 kHz stereo WAV audio is supported.");
        book8gui_invalidate();
        return;
    }
    const char *href = browser.links[index].url;
    char target[BROWSER_URL_CAPACITY];
    const char *base = browser.base_url[0] != '\0' ? browser.base_url
        : browser.address;
    if (!resolve_url(base, href, target, sizeof(target))
        || starts_with(target, "https://")) {
        set_status("That link uses an unsupported address or HTTPS/TLS.");
        book8gui_invalidate();
        return;
    }
    /* JavaScript/CSS gating: stub flags present; no engine implemented yet. */
    if (browser.links[index].media_kind == BOOK8_MEDIA_WAV
        && (book8_media_prefs()->web & BOOK8_MEDIA_WEB_OPEN_IN_PLAYER) != 0u) {
        /* TODO: add confirmation dialog before queuing to Player */
        if (book8gui_queue_utility(BOOK8_GUI_PLAYER, target) == 0) {
            set_status("Opened WAV link in BOOK8 Player.");
        } else {
            set_status("BOOK8 Player is unavailable for this link.");
        }
        book8gui_invalidate();
        return;
    }
    if ((book8_media_prefs()->web & BOOK8_MEDIA_WEB_JAVASCRIPT) == 0u) {
        set_status("JavaScript is disabled; link navigated without script execution.");
    }
    if ((book8_media_prefs()->web & BOOK8_MEDIA_WEB_CSS) == 0u) {
        set_status("CSS layout is disabled; link rendered with basic block model.");
    }
    (void)navigate(p, app, target, true);
}

static void go_back(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.history_count > 1u && browser.history_index > 0u) {
        --browser.history_index;
        (void)navigate(p, app, browser.history[browser.history_index], false);
    }
}

static void go_forward(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.history_index + 1u < browser.history_count) {
        ++browser.history_index;
        (void)navigate(p, app, browser.history[browser.history_index], false);
    }
}

static void stop_loading(void)
{
    close_connection();
    browser.state = BROWSER_READY;
    set_status("Navigation stopped.");
    book8gui_invalidate();
}

static void layout(const struct WindowApp *app, struct browser_layout *v)
{
    struct book8gui_rect c = ui_client(app);
    v->menu = (struct book8gui_rect){c.x, c.y, c.width, 20u};
    v->toolbar = (struct book8gui_rect){c.x, c.y + 20u, c.width, 30u};
    uint32_t x = c.x;
    const uint32_t nav_width[] = {56u, 64u, 56u, 70u, 60u, 52u};
    for (uint32_t i = 0u; i < 6u; ++i) {
        v->nav[i] = (struct book8gui_rect){x, c.y + 22u, nav_width[i], 26u};
        x += nav_width[i] + 3u;
    }
    v->address = (struct book8gui_rect){c.x, c.y + 50u, c.width, 24u};
    v->address_field = (struct book8gui_rect){c.x + 64u, v->address.y,
        c.width > 126u ? c.width - 126u : 0u, v->address.height};
    v->go = (struct book8gui_rect){v->address_field.x + v->address_field.width
        + 4u, v->address.y, 54u, v->address.height};
    v->status = (struct book8gui_rect){c.x, c.y + c.height - 20u,
        c.width, 20u};
    uint32_t page_width = c.width * 3u / 4u;
    v->content = (struct book8gui_rect){c.x, c.y + 80u,
        page_width > 8u ? page_width - 4u : page_width,
        c.height > 104u ? c.height - 104u : 0u};
    v->links = (struct book8gui_rect){c.x + page_width, c.y + 80u,
        c.width - page_width, v->content.height};
    uint32_t text_width = v->content.width > UI_SCROLL_W + 8u
        ? v->content.width - UI_SCROLL_W - 8u : 0u;
    v->columns = text_width > 8u ? text_width / 8u : 1u;
    if (v->columns > 96u) {
        v->columns = 96u;
    }
    v->page_rows = v->content.height > 8u ? (v->content.height - 8u) / 18u : 0u;
}

static void draw_raised(const struct book8gui_platform *p,
    const struct book8gui_rect *r, bool pressed, bool hover, bool enabled)
{
    if (r->width < 4u || r->height < 4u) {
        return;
    }
    uint32_t face = enabled ? (hover && !pressed ? 0xd4d4d4u : UI_FACE)
        : UI_FACE;
    p->fill_rect(r->x, r->y, r->width, r->height, UI_SHADOW);
    p->fill_rect(r->x + 1u, r->y + 1u, r->width - 2u, r->height - 2u, face);
    p->fill_rect(r->x, r->y, r->width, 1u, pressed ? UI_SHADOW : UI_LIGHT);
    p->fill_rect(r->x, r->y, 1u, r->height, pressed ? UI_SHADOW : UI_LIGHT);
    p->fill_rect(r->x + 1u, r->y + r->height - 2u, r->width - 1u, 1u,
        pressed ? UI_LIGHT : UI_EDGE);
    p->fill_rect(r->x + r->width - 2u, r->y + 1u, 1u, r->height - 1u,
        pressed ? UI_LIGHT : UI_EDGE);
}

static void draw_icon(const struct book8gui_platform *p,
    enum browser_icon icon, uint32_t x, uint32_t y, bool enabled)
{
    uint32_t color = enabled ? 0x000080u : UI_MUTED;
    uint32_t stop = enabled ? 0xc00000u : UI_MUTED;
    uint32_t go = enabled ? 0x008000u : UI_MUTED;
    if (icon == BROWSER_ICON_BACK || icon == BROWSER_ICON_FORWARD
        || icon == BROWSER_ICON_GO) {
        uint32_t accent = icon == BROWSER_ICON_GO ? go : color;
        bool right = icon != BROWSER_ICON_BACK;
        p->fill_rect(x + (right ? 1u : 4u), y + 5u, 8u, 2u, accent);
        for (uint32_t row = 0u; row < 5u; ++row) {
            uint32_t inset = row < 2u ? row : 4u - row;
            uint32_t column = right ? x + 8u - inset : x + 1u + inset;
            p->fill_rect(column, y + 3u + row, 2u, 1u, accent);
        }
        return;
    }
    if (icon == BROWSER_ICON_STOP) {
        p->fill_rect(x + 2u, y + 2u, 8u, 8u, stop);
        p->fill_rect(x + 3u, y + 3u, 6u, 2u, 0xff6666u);
        return;
    }
    if (icon == BROWSER_ICON_HOME) {
        for (uint32_t row = 0u; row < 5u; ++row) {
            p->fill_rect(x + 1u + row, y + 1u + row,
                row >= 4u ? 6u : 10u - row * 2u, 1u, color);
        }
        p->fill_rect(x + 3u, y + 6u, 6u, 6u, color);
        p->fill_rect(x + 5u, y + 8u, 2u, 4u, UI_FACE);
        return;
    }
    p->fill_rect(x + 2u, y + 1u, 8u, 2u, color);
    p->fill_rect(x + 1u, y + 3u, 2u, 6u, color);
    p->fill_rect(x + 9u, y + 3u, 2u, 5u, color);
    p->fill_rect(x + 2u, y + 9u, 8u, 2u, color);
    p->fill_rect(x + 9u, y + 1u, 2u, 3u, 0x0000c0u);
}

static void draw_button(const struct book8gui_platform *p,
    const struct book8gui_rect *r, enum browser_icon icon, const char *label,
    bool enabled, bool hover, bool pressed)
{
    draw_raised(p, r, pressed, hover, enabled);
    uint32_t offset = pressed ? 1u : 0u;
    draw_icon(p, icon, r->x + 3u + offset, r->y + 6u + offset, enabled);
    p->draw_text(r->x + 16u + offset, r->y + 5u + offset, label,
        enabled ? UI_TEXT : UI_MUTED, UI_FACE);
}

static void fit_browser_image(uint32_t max_width, uint32_t max_height)
{
    uint32_t width = browser.image_width;
    uint32_t height = browser.image_height;
    if (browser.image_pixels == NULL || width == 0u || height == 0u
        || max_width == 0u || max_height == 0u) {
        return;
    }
    uint32_t fitted_width = width;
    uint32_t fitted_height = height;
    if (fitted_width > max_width) {
        fitted_height = (uint32_t)((uint64_t)fitted_height * max_width
            / fitted_width);
        fitted_width = max_width;
    }
    if (fitted_height > max_height) {
        fitted_width = (uint32_t)((uint64_t)fitted_width * max_height
            / fitted_height);
        fitted_height = max_height;
    }
    if (fitted_width == 0u) {
        fitted_width = 1u;
    }
    if (fitted_height == 0u) {
        fitted_height = 1u;
    }
    if (fitted_width != width || fitted_height != height) {
        for (uint32_t y = 0u; y < fitted_height; ++y) {
            uint32_t source_y = (uint32_t)((uint64_t)y * height
                / fitted_height);
            for (uint32_t x = 0u; x < fitted_width; ++x) {
                uint32_t source_x = (uint32_t)((uint64_t)x * width
                    / fitted_width);
                uint32_t source = (source_y * width + source_x) * 4u;
                uint32_t destination = (y * fitted_width + x) * 4u;
                uint8_t pixel[4];
                memcpy(pixel, browser.image_pixels + source, sizeof(pixel));
                memcpy(browser.image_pixels + destination, pixel, sizeof(pixel));
            }
        }
        browser.image_width = fitted_width;
        browser.image_height = fitted_height;
    }
}

static uint32_t page_line_start(uint32_t line)
{
    uint32_t start = 0u;
    for (uint32_t i = 0u; i < browser.page_length && start < line; ++i) {
        if (browser.page[i] == '\n') {
            ++start;
            if (start == line) {
                return i + 1u;
            }
        }
    }
    return browser.page[0] == '\0' ? 0u : start;
}

static uint32_t page_line_length(uint32_t line)
{
    uint32_t start = page_line_start(line);
    uint32_t end = start;
    while (end < browser.page_length && browser.page[end] != '\n') {
        ++end;
    }
    return end - start;
}

static uint32_t page_line_rows(uint32_t line, uint32_t columns)
{
    uint32_t length = page_line_length(line);
    if (columns == 0u) {
        return 1u;
    }
    return length == 0u ? 1u : (length + columns - 1u) / columns;
}

static int32_t link_at(uint32_t line, uint32_t column)
{
    for (uint32_t i = 0u; i < browser.run_count; ++i) {
        const struct browser_text_run *run = &browser.runs[i];
        if (run->line == line && column >= run->start && column < run->end) {
            return (int32_t)run->link;
        }
    }
    return -1;
}

static uint32_t max_page_scroll(const struct browser_layout *v)
{
    return browser.page_lines > v->page_rows
        ? browser.page_lines - v->page_rows : 0u;
}

static int32_t content_link_at(const struct browser_layout *v, int32_t x,
    int32_t y)
{
    if (browser.image_pixels != NULL || !ui_contains(&v->content, x, y)) {
        return -1;
    }
    struct book8gui_rect track = ui_scroll_track(&v->content);
    if (ui_contains(&track, x, y)) {
        return -1;
    }
    uint32_t row = y > (int32_t)(v->content.y + 4u)
        ? ((uint32_t)y - v->content.y - 4u) / 18u : 0u;
    uint32_t target = browser.page_scroll + row;
    if (target >= browser.page_lines) {
        return -1;
    }
    uint32_t line = 0u;
    uint32_t consumed = 0u;
    while (line < target && line < browser.page_lines) {
        consumed += page_line_rows(line, v->columns);
        ++line;
    }
    uint32_t rows = page_line_rows(target, v->columns);
    if (consumed + rows <= row) {
        return -1;
    }
    uint32_t row_in_line = row - consumed;
    uint32_t column_in_row = x > (int32_t)(v->content.x + 5u)
        ? ((uint32_t)x - v->content.x - 5u) / 8u : 0u;
    if (column_in_row >= v->columns) {
        return -1;
    }
    return link_at(target, row_in_line * v->columns + column_in_row);
}

static void draw_page_segment(const struct book8gui_platform *p,
    uint32_t x, uint32_t y, const char *text, uint32_t length, int32_t link)
{
    char segment[128];
    if (length == 0u || length >= sizeof(segment)) {
        return;
    }
    memcpy(segment, text, length);
    segment[length] = '\0';
    uint32_t foreground = UI_TEXT;
    if (link >= 0) {
        foreground = link == browser.hover_link ? 0xff0000u
            : book8_config_value(BOOK8_CFG_DARK) ? 0x6699ffu : 0x0000ffu;
    }
    if (p->draw_text_scaled != NULL && browser.text_scale > 1u) {
        p->draw_text_scaled(x, y, segment, foreground, UI_FIELD,
            browser.text_scale, false);
    } else {
        p->draw_text(x, y, segment, foreground, UI_FIELD);
    }
}

static void render_page(const struct book8gui_platform *p,
    const struct browser_layout *v)
{
    ui_sunken(p, &v->content, UI_FIELD);
    if (browser.image_pixels != NULL) {
        uint32_t max_width = v->content.width > 8u
            ? v->content.width - 8u : 0u;
        uint32_t max_height = v->content.height > 8u
            ? v->content.height - 8u : 0u;
        fit_browser_image(max_width, max_height);
        if (p->draw_rgba != NULL && browser.image_width != 0u
            && browser.image_height != 0u) {
            p->draw_rgba(v->content.x + 4u, v->content.y + 4u,
                browser.image_width, browser.image_height,
                browser.image_pixels, browser.image_width * 4u, 1u);
        } else {
            p->draw_text(v->content.x + 5u, v->content.y + 4u,
                "Image rendering unavailable.", UI_TEXT, UI_FIELD);
        }
        return;
    }
    uint32_t maximum = max_page_scroll(v);
    if (browser.page_scroll > maximum) {
        browser.page_scroll = maximum;
    }
    uint32_t visual_row = 0u;
    for (uint32_t line = browser.page_scroll;
        line < browser.page_lines && visual_row < v->page_rows; ++line) {
        uint32_t start = page_line_start(line);
        uint32_t length = page_line_length(line);
        uint32_t rows = page_line_rows(line, v->columns);
        for (uint32_t part = 0u; part < rows && visual_row < v->page_rows;
            ++part) {
            uint32_t chunk_start = start + part * v->columns;
            uint32_t chunk_length = part * v->columns < length
                ? length - part * v->columns : 0u;
            if (chunk_length > v->columns) {
                chunk_length = v->columns;
            }
            if (chunk_length > 0u) {
                uint32_t y = v->content.y + 4u + visual_row * 18u;
                uint32_t segment_start = 0u;
                int32_t segment_link = link_at(line, part * v->columns);
                for (uint32_t column = 1u; column <= chunk_length; ++column) {
                    int32_t next = column < chunk_length
                        ? link_at(line, part * v->columns + column) : -2;
                    if (next != segment_link) {
                        draw_page_segment(p, v->content.x + 5u
                            + segment_start * 8u, y,
                            browser.page + chunk_start + segment_start,
                            column - segment_start, segment_link);
                        segment_start = column;
                        segment_link = next;
                    }
                }
            }
            ++visual_row;
        }
    }
    struct book8gui_rect track = ui_scroll_track(&v->content);
    ui_scrollbar(p, &track, browser.page_lines, v->page_rows,
        browser.page_scroll);
}

static bool pressed_nav_matches(uint32_t index)
{
    return browser.pressed_nav == 8u + index;
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.state == BROWSER_HOME && app->title == book8_browser_class.name) {
        set_title(app);
    }
    struct browser_layout v;
    layout(app, &v);
    for (uint32_t i = 0u; i < 6u; ++i) {
        struct book8gui_rect r = {v.menu.x + i * 64u, v.menu.y, 64u, 20u};
        bool hover = browser.hover_nav == 8u + i;
        draw_raised(p, &r, browser.menu == i + 1u || (hover && pressed_nav_matches(i)),
            hover, true);
        p->draw_text(r.x + 3u, r.y + 2u, menus[i], UI_TEXT, UI_FACE);
    }
    p->fill_rect(v.toolbar.x, v.toolbar.y + v.toolbar.height - 2u,
        v.toolbar.width, 1u, UI_SHADOW);
    static const char *const labels[] = {"Back", "Forward", "Stop",
        "Refresh", "Home", "Go"};
    static const enum browser_icon icons[] = {
        BROWSER_ICON_BACK, BROWSER_ICON_FORWARD, BROWSER_ICON_STOP,
        BROWSER_ICON_REFRESH, BROWSER_ICON_HOME, BROWSER_ICON_GO
    };
    bool loading = browser.state == BROWSER_RESOLVING
        || browser.state == BROWSER_CONNECTING
        || browser.state == BROWSER_LOADING;
    bool enabled[] = {
        browser.history_index > 0u,
        browser.history_index + 1u < browser.history_count,
        loading,
        true,
        true,
        browser.address[0] != '\0'
    };
    for (uint32_t i = 0u; i < 6u; ++i) {
        bool hover = browser.hover_nav == i && enabled[i];
        bool pressed = browser.pressed_nav == i + 1u && hover;
        draw_button(p, &v.nav[i], icons[i], labels[i], enabled[i], hover,
            pressed);
        if (i == 1u || i == 3u || i == 5u) {
            p->fill_rect(v.nav[i].x + v.nav[i].width + 1u, v.toolbar.y + 3u,
                1u, v.toolbar.height - 8u, UI_SHADOW);
        }
    }
    p->draw_text(v.address.x + 2u, v.address.y + 4u, "Address:", UI_TEXT,
        UI_FACE);
    draw_raised(p, &v.address_field, false, false, true);
    uint32_t address_len = (uint32_t)strlen(browser.address);
    uint32_t columns = v.address_field.width > 8u
        ? (v.address_field.width - 8u) / 8u : 0u;
    uint32_t first = address_len > columns ? address_len - columns : 0u;
    if (address_len != 0u) {
        p->draw_text(v.address_field.x + 4u, v.address_field.y + 4u,
            browser.address + first, UI_TEXT, UI_FIELD);
    } else {
        p->draw_text(v.address_field.x + 4u, v.address_field.y + 4u,
            "Type an HTTP address or hostname", UI_MUTED, UI_FIELD);
    }
    if (browser.address_focused && columns != 0u) {
        uint32_t caret = address_len - first;
        p->fill_rect(v.address_field.x + 4u + caret * 8u,
            v.address_field.y + 4u, 2u, 16u, UI_TEXT);
    }
    bool go_hover = browser.hover_nav == 7u && enabled[5];
    draw_button(p, &v.go, BROWSER_ICON_GO, "Go", enabled[5], go_hover,
        browser.pressed_nav == 8u && go_hover);
    render_page(p, &v);
    draw_raised(p, &v.links, false, false, true);
    p->draw_text(v.links.x + 4u, v.links.y + 4u, "Page Links", UI_TEXT,
        UI_FACE);
    uint32_t rows = v.links.height > 28u ? (v.links.height - 28u) / 22u : 0u;
    uint32_t max_chars = v.links.width > 12u
        ? (v.links.width - 12u) / 8u : 0u;
    for (uint32_t i = 0u; i < browser.link_count && i < rows; ++i) {
        char label[80];
        snprintf(label, sizeof(label), "%.*s",
            (int)(max_chars < sizeof(label) - 1u ? max_chars : sizeof(label) - 1u),
            browser.links[i].label);
        p->draw_text(v.links.x + 5u, v.links.y + 24u + i * 22u,
            label, UI_SELECTED, UI_FACE);
    }
    struct book8gui_rect popup;
    if (browser.menu != 0u) {
        popup = (struct book8gui_rect){
            v.menu.x + (browser.menu - 1u) * 64u, v.menu.y + 20u,
            browser.menu == 6u ? 190u : 150u,
            browser.menu == 6u ? 54u : 70u
        };
        draw_raised(p, &popup, false, false, true);
        const char *item = browser.menu == 1u ? "Open Location..."
            : browser.menu == 2u ? "Select Address"
            : browser.menu == 3u ? "Text Size"
            : browser.menu == 4u ? "Go to Home Page"
            : "About Web Browser";
        if (browser.menu == 6u) {
            p->draw_text(popup.x + 8u, popup.y + 4u,
                "Set Current as Home", UI_TEXT, UI_FACE);
            p->draw_text(popup.x + 8u, popup.y + 28u,
                "Clear Cookies", UI_TEXT, UI_FACE);
        } else {
            p->draw_text(popup.x + 8u, popup.y + 8u, item, UI_TEXT, UI_FACE);
        }
    }
    ui_sunken(p, &v.status, UI_FACE);
    p->draw_text(v.status.x + 4u, v.status.y + 2u,
        browser.status, UI_TEXT, UI_FACE);
}

static bool handle_event(const struct book8gui_platform *p,
    struct WindowApp *app, const struct book8gui_event *event)
{
    if (event->type == BOOK8GUI_EVENT_KEY) {
        uint32_t key = event->key & BOOK8GUI_KEY_MASK;
        bool ctrl = (event->key & BOOK8GUI_KEY_CTRL) != 0u;
        if (ctrl && (key == 'l' || key == 'o')) {
            browser.address_focused = true;
            browser.address[0] = '\0';
            browser.menu = 0u;
            book8gui_invalidate();
            return true;
        }
        if (ctrl && key == 'r') {
            if (browser.address[0] != '\0') {
                (void)navigate(p, app, browser.address, false);
            }
            return true;
        }
        if (ctrl && key == BOOK8GUI_KEY_LEFT) {
            go_back(p, app);
            return true;
        }
        if (ctrl && key == BOOK8GUI_KEY_RIGHT) {
            go_forward(p, app);
            return true;
        }
        if (browser.address_focused) {
            uint32_t length = (uint32_t)strlen(browser.address);
            if (key == BOOK8GUI_KEY_ESCAPE) {
                browser.address_focused = false;
            } else if (key == BOOK8GUI_KEY_ENTER) {
                browser.address_focused = false;
                (void)navigate(p, app, browser.address, true);
            } else if (key == BOOK8GUI_KEY_BACKSPACE && length != 0u) {
                browser.address[length - 1u] = '\0';
            } else if (key >= 32u && key < 127u
                && length + 1u < sizeof(browser.address)) {
                browser.address[length] = (char)key;
                browser.address[length + 1u] = '\0';
            }
            book8gui_invalidate();
            return true;
        }
        if (key == BOOK8GUI_KEY_ESCAPE) {
            browser.menu = 0u;
            book8gui_invalidate();
            return true;
        }
        if (key == BOOK8GUI_KEY_PAGE_DOWN || key == BOOK8GUI_KEY_DOWN) {
            struct browser_layout key_layout;
            layout(app, &key_layout);
            uint32_t maximum = max_page_scroll(&key_layout);
            browser.page_scroll += 3u;
            if (browser.page_scroll > maximum) {
                browser.page_scroll = maximum;
            }
            book8gui_invalidate();
            return true;
        }
        if ((key == BOOK8GUI_KEY_PAGE_UP || key == BOOK8GUI_KEY_UP)
            && browser.page_scroll >= 3u) {
            browser.page_scroll -= 3u;
            book8gui_invalidate();
            return true;
        }
        return false;
    }
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool old_down = (browser.previous_buttons & 1u) != 0u;
    bool pressed = down && !old_down;
    browser.previous_buttons = event->mouse_buttons;
    struct browser_layout v;
    layout(app, &v);
    uint8_t old_hover = browser.hover_nav;
    int32_t old_link = browser.hover_link;
    browser.hover_nav = BROWSER_HOVER_NONE;
    browser.hover_link = -1;
    if (ui_contains(&v.menu, event->mouse_x, event->mouse_y)) {
        uint32_t menu = ((uint32_t)event->mouse_x - v.menu.x) / 64u;
        if (menu < 6u) {
            browser.hover_nav = (uint8_t)(8u + menu);
        }
    } else {
        for (uint32_t i = 0u; i < 6u; ++i) {
            if (ui_contains(&v.nav[i], event->mouse_x, event->mouse_y)) {
                browser.hover_nav = (uint8_t)i;
                break;
            }
        }
        if (browser.hover_nav == BROWSER_HOVER_NONE
            && ui_contains(&v.go, event->mouse_x, event->mouse_y)) {
            browser.hover_nav = 7u;
        }
        if (browser.hover_nav == BROWSER_HOVER_NONE) {
            browser.hover_link = content_link_at(&v, event->mouse_x,
                event->mouse_y);
        }
    }
    browser.pressed_nav = down ? browser.hover_nav : BROWSER_HOVER_NONE;
    if (event->wheel_delta != 0) {
        uint32_t maximum = max_page_scroll(&v);
        if (event->wheel_delta > 0) {
            browser.page_scroll = browser.page_scroll > 3u
                ? browser.page_scroll - 3u : 0u;
        } else {
            browser.page_scroll += 3u;
            if (browser.page_scroll > maximum) {
                browser.page_scroll = maximum;
            }
        }
        book8gui_invalidate();
        return true;
    }
    if (old_hover != browser.hover_nav || old_link != browser.hover_link
        || down != old_down) {
        book8gui_invalidate();
    }
    if (!pressed) {
        return false;
    }
    if (browser.hover_nav >= 8u && browser.hover_nav < 14u) {
        browser.menu = browser.menu == browser.hover_nav - 7u
            ? 0u : browser.hover_nav - 7u;
        book8gui_invalidate();
        return true;
    }
    if (browser.menu != 0u) {
        struct book8gui_rect popup = {
            v.menu.x + (browser.menu - 1u) * 64u, v.menu.y + 20u,
            browser.menu == 6u ? 190u : 150u,
            browser.menu == 6u ? 54u : 70u
        };
        if (ui_contains(&popup, event->mouse_x, event->mouse_y)) {
            if (browser.menu == 1u || browser.menu == 2u) {
                browser.address_focused = true;
            } else if (browser.menu == 3u) {
                browser.text_scale = browser.text_scale == 1u ? 2u : 1u;
            } else if (browser.menu == 4u) {
                go_home(p, app);
            } else if (browser.menu == 6u) {
                if (event->mouse_y < (int32_t)(popup.y + 27u)) {
                    save_homepage(p);
                } else {
                    memset(browser.cookies, 0, sizeof(browser.cookies));
                    browser.cookie_replace = 0u;
                    set_status("Cookies cleared.");
                }
            } else if (browser.menu == 5u) {
                set_status("Plain HTTP only; HTTPS/TLS and JavaScript are unavailable.");
            }
        }
        browser.menu = 0u;
        book8gui_invalidate();
        return true;
    }
    if (ui_contains(&v.address_field, event->mouse_x, event->mouse_y)) {
        browser.address_focused = true;
        book8gui_invalidate();
        return true;
    }
    if (ui_contains(&v.go, event->mouse_x, event->mouse_y)) {
        browser.address_focused = true;
        if (browser.address[0] != '\0') {
            (void)navigate(p, app, browser.address, true);
        }
        book8gui_invalidate();
        return true;
    }
    if (browser.hover_nav < 6u) {
        uint32_t i = browser.hover_nav;
        bool loading = browser.state == BROWSER_RESOLVING
            || browser.state == BROWSER_CONNECTING
            || browser.state == BROWSER_LOADING;
        if (i == 0u && browser.history_index > 0u) {
            go_back(p, app);
        } else if (i == 1u
            && browser.history_index + 1u < browser.history_count) {
            go_forward(p, app);
        } else if (i == 2u && loading) {
            stop_loading();
        } else if (i == 3u && browser.address[0] != '\0') {
            (void)navigate(p, app, browser.address, false);
        } else if (i == 4u) {
            go_home(p, app);
        } else if (i == 5u) {
            browser.address_focused = true;
            if (browser.address[0] != '\0') {
                (void)navigate(p, app, browser.address, true);
            }
        }
        book8gui_invalidate();
        return true;
    }
    if (browser.hover_link >= 0) {
        follow_link((uint32_t)browser.hover_link, p, app);
        return true;
    }
    uint32_t link_rows = v.links.height > 28u ? (v.links.height - 28u) / 22u : 0u;
    if (event->mouse_x >= (int32_t)v.links.x
        && event->mouse_x < (int32_t)(v.links.x + v.links.width)
        && event->mouse_y >= (int32_t)(v.links.y + 24u)) {
        uint32_t row = ((uint32_t)event->mouse_y - v.links.y - 24u) / 22u;
        if (row < browser.link_count && row < link_rows) {
            follow_link(row, p, app);
            return true;
        }
    }
    if (ui_contains(&v.address, event->mouse_x, event->mouse_y)) {
        browser.address_focused = true;
        book8gui_invalidate();
        return true;
    }
    return false;
}

static void *create(const struct book8gui_platform *p)
{
    close_connection();
    book8_image_pool_release(&browser_image_pool);
    browser = (struct browser_app){.text_scale = 1u};
    (void)book8_media_prefs_load(p);
    browser.hover_nav = BROWSER_HOVER_NONE;
    browser.pressed_nav = BROWSER_HOVER_NONE;
    browser.hover_link = -1;
    if (p->read_file != NULL) {
        char saved_home[BROWSER_URL_CAPACITY];
        uint32_t saved_size = 0u;
        if (p->read_file(BROWSER_HOME_PATH, saved_home,
                sizeof(saved_home) - 1u, &saved_size)
            && saved_size != 0u && saved_size < sizeof(saved_home)) {
            saved_home[saved_size] = '\0';
            uint32_t ip;
            uint16_t port;
            char path[BROWSER_URL_CAPACITY], host[BROWSER_URL_CAPACITY];
            if (parse_url(saved_home, &ip, &port, path, sizeof(path),
                    host, sizeof(host))) {
                snprintf(browser.homepage, sizeof(browser.homepage), "%s",
                    saved_home);
            }
        }
    }
    snprintf(browser.page, sizeof(browser.page),
        "BOOK8 Internet Explorer\n"
        "=======================\n\n"
        "Type an HTTP address above and press Enter, or click Go.\n\n"
        "Modern HTML documents are rendered as readable text with clickable "
        "links, semantic layout, character entities, page titles, relative "
        "URLs, HTTP redirects and meta-refresh navigation.\n\n"
        "Standalone BMP/GIF/JPEG/PNG images and HTTP WAV audio are also "
        "supported. HTTPS/TLS, JavaScript, CSS layout and video codecs are "
        "not available in this build.");
    browser.page_length = (uint32_t)strlen(browser.page);
    count_page_lines();
    set_status("Ready.");
    return &browser;
}

static void destroy(void *state)
{
    (void)state;
    close_connection();
    book8_image_pool_release(&browser_image_pool);
    browser = (struct browser_app){0};
}

static bool open_path(const struct book8gui_platform *p,
    struct WindowApp *app, const char *path)
{
    return navigate(p, app, path, true);
}

const struct book8gui_app_class book8_browser_class = {
    .name = "Web Browser",
    .default_width = 820u,
    .default_height = 560u,
    .single_instance = true,
    .wants_all_keys = true,
    .state_size = sizeof(struct browser_app),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event,
    .open_path = open_path,
    .tick = tick
};
