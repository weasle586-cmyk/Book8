#include "book8_browser.h"
#include "book8_ui.h"

#include <stddef.h>
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
#define BROWSER_COOKIE_COUNT 4u
#define BROWSER_COOKIE_NAME_CAPACITY 40u
#define BROWSER_COOKIE_VALUE_CAPACITY 80u
#define BROWSER_IMAGE_POOL_SIZE (8u * 1024u * 1024u)
#define BROWSER_HOME_PATH "A:\\BROWSER.CFG"
#define BROWSER_DNS_PORT 53053u
#define BROWSER_DNS_QUERY_CAPACITY 512u
#define BROWSER_DNS_RETRIES 3u
#define BROWSER_DNS_TIMEOUT_MS 8000u

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
    char dns_name[BROWSER_URL_CAPACITY];
    char title[BROWSER_URL_CAPACITY + 32u];
    char status[160];
    char page[BROWSER_TEXT_CAPACITY];
    char response[BROWSER_RESPONSE_CAPACITY + 1u];
    struct browser_link links[BROWSER_LINK_COUNT];
    struct browser_cookie cookies[BROWSER_COOKIE_COUNT];
    char history[BROWSER_HISTORY_COUNT][BROWSER_URL_CAPACITY];
    uint8_t *image_pixels;
    uint32_t image_width, image_height;
    uint32_t dns_server, resolved_ip;
    uint32_t response_size, body_offset, body_size, content_size;
    uint32_t page_length, page_scroll, link_count, history_count, history_index;
    uint32_t text_scale, menu, dns_query_size, dns_retries;
    uint16_t dns_id;
    uint64_t dns_retry_at, dns_deadline;
    uint8_t dns_query[BROWSER_DNS_QUERY_CAPACITY];
    uint8_t previous_buttons;
    uint32_t cookie_replace;
    bool address_focused, content_length_known, request_sent, html_content;
    bool image_content, audio_content, video_content;
    bool chunked_content, dns_bound, dns_resolved, dns_failed;
};

struct browser_layout {
    struct book8gui_rect menu, nav[6], address, content, links, status;
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
static void close_connection(void);

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
    const char *name = browser.state == BROWSER_HOME
        ? "Start Page" : browser.address;
    snprintf(browser.title, sizeof(browser.title), "Web Browser - %s", name);
    app->title = browser.title;
}

static void set_status(const char *message)
{
    snprintf(browser.status, sizeof(browser.status), "%s", message);
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
    snprintf(browser.page, sizeof(browser.page),
        "Welcome to BOOK8 Web Browser\n\n"
        "Enter an HTTP URL or hostname, such as www.google.com.\n"
        "Text, simple HTML and standalone BMP/GIF/JPEG/PNG images are supported. "
        "Image links can be opened from Page Links.\n\n"
        "DNS and chunked HTTP responses are supported. HTTPS/TLS and JavaScript "
        "are not available.");
    browser.page_length = (uint32_t)strlen(browser.page);
    set_status("Ready.");
    set_title(app);
    book8gui_invalidate();
}

static void go_home(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.homepage[0] != '\0') {
        (void)begin_request(p, app, browser.homepage, true);
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
    char normalized[BROWSER_URL_CAPACITY];
    char path[BROWSER_URL_CAPACITY], host[BROWSER_URL_CAPACITY];
    close_connection();
    if (url == NULL || url[0] == '\0'
        || starts_with(url, "https://")
        || (starts_with(url, "http://")
            ? snprintf(normalized, sizeof(normalized), "%s", url)
            : snprintf(normalized, sizeof(normalized), "http://%s", url))
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
        return begin_dns(p, app, net, host);
    }
    return connect_request(p, app, net, ip);
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
    if (status_code < 200u || status_code >= 300u) {
        if (status_code >= 300u && status_code < 400u) {
            snprintf(browser.status, sizeof(browser.status),
                "HTTP redirect %u is unsupported; HTTPS/TLS is not available.",
                status_code);
        } else {
            snprintf(browser.status, sizeof(browser.status),
                "Server returned HTTP status %u.", status_code);
        }
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
    return true;
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
        }
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
}

static void append_link_char(char *label, uint32_t capacity, char ch)
{
    uint32_t length = (uint32_t)strlen(label);
    if (length + 1u < capacity && ch >= 32 && ch <= 126) {
        label[length] = ch;
        label[length + 1u] = '\0';
    }
}

static bool block_tag(const char *tag, uint32_t length)
{
    return (length >= 1u && (tag[0] == 'p' || tag[0] == 'P'
        || tag[0] == 'h' || tag[0] == 'H' || tag[0] == 'd'
        || tag[0] == 'D' || tag[0] == 'l' || tag[0] == 'L'))
        || starts_with(tag, "br") || starts_with(tag, "/br");
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

static void add_link(const char *url, const char *label,
    enum book8_media_kind media_kind)
{
    if (url[0] == '\0') {
        return;
    }
    if (media_kind == BOOK8_MEDIA_OTHER) {
        media_kind = book8_media_classify_url(url);
    }
    for (uint32_t i = 0u; i < browser.link_count; ++i) {
        if (strcmp(browser.links[i].url, url) == 0) {
            return;
        }
    }
    if (browser.link_count == BROWSER_LINK_COUNT) { return; }
    struct browser_link *link = &browser.links[browser.link_count];
    snprintf(link->url, sizeof(link->url), "%s", url);
    const char *display = label[0] != '\0' ? label : url;
    if (media_kind == BOOK8_MEDIA_WAV && !starts_with(display, "[Audio")) {
        snprintf(link->label, sizeof(link->label), "[Audio WAV] %.64s", display);
    } else if (media_kind == BOOK8_MEDIA_VIDEO
        && !starts_with(display, "[Video")) {
        snprintf(link->label, sizeof(link->label),
            "[Video unsupported] %.56s", display);
    } else if (media_kind == BOOK8_MEDIA_UNSUPPORTED_AUDIO
        && !starts_with(display, "[Audio")) {
        snprintf(link->label, sizeof(link->label),
            "[Audio unsupported] %.58s", display);
    } else {
        snprintf(link->label, sizeof(link->label), "%s", display);
    }
    link->media_kind = media_kind;
    ++browser.link_count;
}

static bool add_media_source(void *context,
    const struct book8_media_source *source)
{
    (void)context;
    add_link(source->url, source->label, source->kind);
    return true;
}

static void html_to_text(const char *body, uint32_t size)
{
    browser.page[0] = '\0';
    browser.page_length = 0u;
    browser.link_count = 0u;
    bool in_anchor = false, skip_script = false;
    char anchor_url[BROWSER_LINK_CAPACITY] = {0};
    char anchor_label[80] = {0};
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
            if (starts_with(tag, "script") || starts_with(tag, "style")) {
                skip_script = true;
            } else if (starts_with(tag, "/script") || starts_with(tag, "/style")) {
                skip_script = false;
            }
            if (!skip_script && length > 1u && starts_with(tag, "a ")) {
                const char *href = NULL;
                for (uint32_t j = 2u; j + 5u < length; ++j) {
                    if (starts_with(tag + j, "href")) {
                        uint32_t k = j + 4u;
                        while (k < length && (tag[k] == ' ' || tag[k] == '=')) {
                            ++k;
                        }
                        char quote = tag[k] == '"' || tag[k] == '\''
                            ? tag[k++] : '\0';
                        uint32_t begin = k;
                        while (k < length && (quote != '\0'
                            ? tag[k] != quote : tag[k] != ' ' && tag[k] != '/')) {
                            ++k;
                        }
                        if (k > begin) {
                            uint32_t copy = k - begin;
                            if (copy >= sizeof(anchor_url)) {
                                copy = sizeof(anchor_url) - 1u;
                            }
                            memcpy(anchor_url, tag + begin, copy);
                            anchor_url[copy] = '\0';
                            href = anchor_url;
                        }
                        break;
                    }
                }
                if (href != NULL) {
                    in_anchor = true;
                    anchor_label[0] = '\0';
                }
            } else if (!skip_script && starts_with(tag, "/a")) {
                if (in_anchor) {
                    add_link(anchor_url, anchor_label, BOOK8_MEDIA_OTHER);
                }
                in_anchor = false;
            } else if (!skip_script && starts_with(tag, "img ")) {
                char image_url[BROWSER_LINK_CAPACITY];
                char image_alt[80];
                if (tag_attribute(tag, length, "src", image_url,
                        sizeof(image_url))) {
                    if (!tag_attribute(tag, length, "alt", image_alt,
                            sizeof(image_alt))) {
                        snprintf(image_alt, sizeof(image_alt), "Image");
                    }
                    char label[80];
                    snprintf(label, sizeof(label), "[Image] %.68s", image_alt);
                    add_link(image_url, label, BOOK8_MEDIA_OTHER);
                    append_page_char('\n');
                    append_page_char('[');
                    for (uint32_t j = 0u; label[j] != '\0'; ++j) {
                        append_page_char(label[j]);
                    }
                    append_page_char(']');
                    append_page_char('\n');
                }
            }
            if (!skip_script && block_tag(tag, length)) {
                append_page_char('\n');
            }
            i = end + 1u;
            continue;
        }
        if (!skip_script) {
            char ch = body[i];
            if (ch == '&') {
                if (i + 5u <= size && memcmp(body + i, "&amp;", 5u) == 0) {
                    ch = '&'; i += 4u;
                } else if (i + 4u <= size && memcmp(body + i, "&lt;", 4u) == 0) {
                    ch = '<'; i += 3u;
                } else if (i + 4u <= size && memcmp(body + i, "&gt;", 4u) == 0) {
                    ch = '>'; i += 3u;
                } else if (i + 6u <= size && memcmp(body + i, "&quot;", 6u) == 0) {
                    ch = '"'; i += 5u;
                }
            }
            append_page_char(ch);
            if (in_anchor) {
                append_link_char(anchor_label, sizeof(anchor_label), ch);
            }
        }
        ++i;
    }
    while (browser.page_length != 0u
        && browser.page[browser.page_length - 1u] == '\n') {
        browser.page[--browser.page_length] = '\0';
    }
    (void)book8_media_scan_html(body, size, add_media_source, NULL);
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
        if (p->media_play_wav != NULL
            && p->media_play_wav((const uint8_t *)body, size)) {
            snprintf(browser.page, sizeof(browser.page),
                "WAV audio is playing through the HDA output.\n"
                "Open BOOK8 Player to control playback.");
            browser.page_length = (uint32_t)strlen(browser.page);
            browser.state = BROWSER_READY;
            set_status("Playing WAV audio.");
            set_title(app);
            book8gui_invalidate();
            return;
        }
        snprintf(browser.page, sizeof(browser.page),
            "Audio playback supports PCM 48 kHz stereo 16-bit WAV only.");
        browser.page_length = (uint32_t)strlen(browser.page);
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
            "User-Agent: BOOK8 Browser/1.0\r\nAccept-Encoding: identity\r\n"
            "Accept: text/html,text/plain,image/*\r\n%s%s\r\n",
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
    char resolved[BROWSER_URL_CAPACITY];
    if (starts_with(href, "http://") || starts_with(href, "https://")) {
        snprintf(resolved, sizeof(resolved), "%s", href);
    } else if (href[0] == '/') {
        uint32_t host_end = 7u;
        while (browser.address[host_end] != '\0'
            && browser.address[host_end] != '/') {
            ++host_end;
        }
        snprintf(resolved, sizeof(resolved), "%.*s%s",
            (int)host_end, browser.address, href);
    } else {
        uint32_t base_end = (uint32_t)strlen(browser.address);
        while (base_end > 7u && browser.address[base_end - 1u] != '/') {
            --base_end;
        }
        snprintf(resolved, sizeof(resolved), "%.*s%s",
            (int)base_end, browser.address, href);
    }
    (void)begin_request(p, app, resolved, true);
}

static void go_back(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.history_count > 1u && browser.history_index > 0u) {
        --browser.history_index;
        (void)begin_request(p, app, browser.history[browser.history_index], false);
    }
}

static void go_forward(const struct book8gui_platform *p, struct WindowApp *app)
{
    if (browser.history_index + 1u < browser.history_count) {
        ++browser.history_index;
        (void)begin_request(p, app, browser.history[browser.history_index], false);
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
    uint32_t x = c.x;
    const uint32_t nav_width[] = {56u, 64u, 56u, 70u, 60u, 52u};
    for (uint32_t i = 0u; i < 6u; ++i) {
        v->nav[i] = (struct book8gui_rect){x, c.y + 22u, nav_width[i], 24u};
        x += nav_width[i] + 3u;
    }
    v->address = (struct book8gui_rect){c.x, c.y + 50u, c.width, 24u};
    v->status = (struct book8gui_rect){c.x, c.y + c.height - 20u,
        c.width, 20u};
    uint32_t page_width = c.width * 3u / 4u;
    v->content = (struct book8gui_rect){c.x, c.y + 80u,
        page_width > 8u ? page_width - 4u : page_width,
        c.height > 104u ? c.height - 104u : 0u};
    v->links = (struct book8gui_rect){c.x + page_width, c.y + 80u,
        c.width - page_width, v->content.height};
    v->columns = v->content.width > 12u ? (v->content.width - 12u) / 8u : 1u;
    v->page_rows = v->content.height > 8u ? (v->content.height - 8u) / 18u : 0u;
}

static void draw_button(const struct book8gui_platform *p,
    const struct book8gui_rect *r, const char *label, bool active)
{
    ui_sunken(p, r, UI_FACE);
    p->draw_text(r->x + 4u, r->y + 4u, label,
        active ? UI_SELECTED_TEXT : UI_TEXT,
        active ? UI_SELECTED : UI_FACE);
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
    uint32_t line = 0u, start = 0u, end = 0u;
    uint32_t size = (uint32_t)strlen(browser.page);
    while (start < size && line < browser.page_scroll + v->page_rows) {
        end = start;
        while (end < size && browser.page[end] != '\n'
            && end - start < v->columns) {
            ++end;
        }
        if (line >= browser.page_scroll) {
            char row[128];
            uint32_t length = end - start;
            if (length >= sizeof(row)) {
                length = sizeof(row) - 1u;
            }
            memcpy(row, browser.page + start, length);
            row[length] = '\0';
            if (p->draw_text_scaled != NULL && browser.text_scale > 1u) {
                p->draw_text_scaled(v->content.x + 5u,
                    v->content.y + 4u + (line - browser.page_scroll) * 18u,
                    row, UI_TEXT, UI_FIELD, browser.text_scale, false);
            } else {
                p->draw_text(v->content.x + 5u,
                    v->content.y + 4u + (line - browser.page_scroll) * 18u,
                    row, UI_TEXT, UI_FIELD);
            }
        }
        start = end + (end < size && browser.page[end] == '\n' ? 1u : 0u);
        ++line;
    }
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
        draw_button(p, &r, menus[i], browser.menu == i + 1u);
    }
    static const char *const labels[] = {"Back", "Forward", "Stop",
        "Refresh", "Home", "Go"};
    for (uint32_t i = 0u; i < 6u; ++i) {
        bool active = i == 2u
            ? browser.state == BROWSER_RESOLVING
                || browser.state == BROWSER_CONNECTING
                || browser.state == BROWSER_LOADING
            : i == 0u ? browser.history_index > 0u
            : i == 1u ? browser.history_index + 1u < browser.history_count
            : true;
        draw_button(p, &v.nav[i], labels[i], active);
    }
    p->draw_text(v.address.x, v.address.y + 4u, "Address:", UI_TEXT, UI_FACE);
    struct book8gui_rect address_field = {
        v.address.x + 64u, v.address.y, v.address.width - 64u, v.address.height
    };
    ui_sunken(p, &address_field, UI_FIELD);
    uint32_t address_len = (uint32_t)strlen(browser.address);
    uint32_t columns = (address_field.width - 8u) / 8u;
    uint32_t first = address_len > columns ? address_len - columns : 0u;
    p->draw_text(address_field.x + 4u, address_field.y + 4u,
        browser.address + first, UI_TEXT, UI_FIELD);
    if (browser.address_focused && columns != 0u) {
        uint32_t caret = address_len - first;
        p->fill_rect(address_field.x + 4u + caret * 8u,
            address_field.y + 4u, 2u, 16u, UI_TEXT);
    }
    render_page(p, &v);
    ui_sunken(p, &v.links, UI_FACE);
    p->draw_text(v.links.x + 4u, v.links.y + 4u, "Page Links", UI_TEXT, UI_FACE);
    uint32_t rows = v.links.height > 28u ? (v.links.height - 28u) / 22u : 0u;
    for (uint32_t i = 0u; i < browser.link_count && i < rows; ++i) {
        char label[80];
        uint32_t max_chars = v.links.width > 12u
            ? (v.links.width - 12u) / 8u : 0u;
        snprintf(label, sizeof(label), "%.*s",
            (int)(max_chars < sizeof(label) - 1u ? max_chars : sizeof(label) - 1u),
            browser.links[i].label);
        p->draw_text(v.links.x + 5u, v.links.y + 24u + i * 22u,
            label, UI_SELECTED, UI_FACE);
    }
    ui_sunken(p, &v.status, UI_FACE);
    p->draw_text(v.status.x + 4u, v.status.y + 2u,
        browser.status, UI_TEXT, UI_FACE);
    if (browser.menu != 0u) {
        struct book8gui_rect popup = {
            v.menu.x + (browser.menu - 1u) * 64u, v.menu.y + 20u,
            browser.menu == 6u ? 190u : 150u,
            browser.menu == 6u ? 54u : 70u
        };
        ui_sunken(p, &popup, UI_FACE);
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
                (void)begin_request(p, app, browser.address, false);
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
                (void)begin_request(p, app, browser.address, true);
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
            browser.page_scroll += 3u;
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
    bool pressed = down && (browser.previous_buttons & 1u) == 0u;
    browser.previous_buttons = event->mouse_buttons;
    struct browser_layout v;
    layout(app, &v);
    if (event->wheel_delta != 0) {
        if (event->wheel_delta > 0) {
            browser.page_scroll = browser.page_scroll > 3u
                ? browser.page_scroll - 3u : 0u;
        } else {
            browser.page_scroll += 3u;
        }
        book8gui_invalidate();
        return true;
    }
    if (!pressed) {
        return false;
    }
    if (event->mouse_y >= (int32_t)v.menu.y
        && event->mouse_y < (int32_t)(v.menu.y + v.menu.height)) {
        uint32_t menu = ((uint32_t)event->mouse_x - v.menu.x) / 64u + 1u;
        if (menu <= 6u) {
            browser.menu = browser.menu == menu ? 0u : menu;
            book8gui_invalidate();
            return true;
        }
    }
    if (browser.menu != 0u) {
        struct book8gui_rect popup = {
            v.menu.x + (browser.menu - 1u) * 64u, v.menu.y + 20u,
            browser.menu == 6u ? 190u : 150u,
            browser.menu == 6u ? 54u : 70u
        };
        if (event->mouse_x >= (int32_t)popup.x
            && event->mouse_x < (int32_t)(popup.x + popup.width)
            && event->mouse_y >= (int32_t)popup.y
            && event->mouse_y < (int32_t)(popup.y + popup.height)) {
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
    struct book8gui_rect address_field = {
        v.address.x + 64u, v.address.y, v.address.width - 64u, v.address.height
    };
    if (event->mouse_x >= (int32_t)address_field.x
        && event->mouse_x < (int32_t)(address_field.x + address_field.width)
        && event->mouse_y >= (int32_t)address_field.y
        && event->mouse_y < (int32_t)(address_field.y + address_field.height)) {
        browser.address_focused = true;
        book8gui_invalidate();
        return true;
    }
    for (uint32_t i = 0u; i < 6u; ++i) {
        struct book8gui_rect *r = &v.nav[i];
        if (event->mouse_x < (int32_t)r->x
            || event->mouse_x >= (int32_t)(r->x + r->width)
            || event->mouse_y < (int32_t)r->y
            || event->mouse_y >= (int32_t)(r->y + r->height)) {
            continue;
        }
        if (i == 0u) { go_back(p, app); }
        else if (i == 1u) { go_forward(p, app); }
        else if (i == 2u) { stop_loading(); }
        else if (i == 3u && browser.address[0] != '\0') {
            (void)begin_request(p, app, browser.address, false);
        }         else if (i == 4u) { go_home(p, app); }
        else if (i == 5u) {
            browser.address_focused = true;
            if (browser.address[0] != '\0') {
                (void)begin_request(p, app, browser.address, true);
            }
        }
        book8gui_invalidate();
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
    if (event->mouse_x >= (int32_t)v.address.x
        && event->mouse_x < (int32_t)(v.address.x + v.address.width)
        && event->mouse_y >= (int32_t)v.address.y
        && event->mouse_y < (int32_t)(v.address.y + v.address.height)) {
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
        "Welcome to BOOK8 Web Browser\n\n"
        "Enter an HTTP URL or hostname, such as www.google.com.\n"
        "Text, simple HTML and standalone BMP/GIF/JPEG/PNG images are supported. "
        "Image links can be opened from Page Links.\n\n"
        "DNS and chunked HTTP responses are supported. HTTPS/TLS and JavaScript "
        "are not available.");
    browser.page_length = (uint32_t)strlen(browser.page);
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
    return begin_request(p, app, path, true);
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
