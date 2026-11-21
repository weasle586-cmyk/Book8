#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/apps/book8_browser.h"
#include "../src/apps/book8_media_extension.h"
#include "../src/apps/book8_paintbook.h"
#include "../src/apps/book8_player.h"
#include "../src/apps/book8_ui.h"
#include "../net/net.h"
#include "../net/network_settings.h"

static char last_text[256];
static char drawn_text[256][128];
static uint32_t drawn_text_count;
static char saved_path[256];
static char saved_homepage[256];
static uint32_t bitmap_draws;
static uint32_t saved_size;
static uint8_t test_wav[48];
static uint32_t media_play_calls;
static uint32_t host_volume;
static struct book8gui_media_status host_media_state;
static uint32_t media_candidate_count;
static struct book8_media_source media_candidates[8];
static struct book8_net browser_test_net;
static uint8_t browser_test_tx[BOOK8_NET_FRAME_MAX];
static size_t browser_test_tx_size;
static uint64_t host_ticks = 1u;

#define BROWSER_TEST_LOCAL BOOK8_NET_IP(192, 168, 7, 10)
#define BROWSER_TEST_DNS BOOK8_NET_IP(192, 168, 7, 53)
#define BROWSER_TEST_WEB BOOK8_NET_IP(192, 168, 7, 20)

const uint32_t book8_accent_colors[8] = {
    0x000080u, 0x008000u, 0x800000u, 0x008080u,
    0x800080u, 0x808000u, 0x000000u, 0x808080u
};

uint32_t book8_config_value(enum book8_config_key key)
{
    (void)key;
    return 0u;
}

void book8gui_invalidate(void)
{
}

void book8gui_request_close(const struct WindowApp *app)
{
    (void)app;
}

void book8gui_note_disk(const struct WindowApp *app, uint32_t read_bytes,
    uint32_t write_bytes)
{
    (void)app; (void)read_bytes; (void)write_bytes;
}

static void fill(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t color)
{
    (void)x; (void)y; (void)width; (void)height; (void)color;
}

static void draw_rgba(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const uint8_t *pixels, uint32_t stride, uint32_t scale)
{
    assert(x != 0u && y != 0u && width != 0u && height != 0u);
    assert(pixels != NULL && stride >= width * 4u && scale != 0u);
    ++bitmap_draws;
}

static void draw_text(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background)
{
    (void)x; (void)y; (void)foreground; (void)background;
    snprintf(last_text, sizeof(last_text), "%s", text);
    if (drawn_text_count < sizeof(drawn_text) / sizeof(drawn_text[0])) {
        snprintf(drawn_text[drawn_text_count], sizeof(drawn_text[0]), "%s", text);
        ++drawn_text_count;
    }
}

static uint64_t ticks(void)
{
    return host_ticks;
}

bool book8_network_settings_get(struct book8_network_status *status)
{
    if (status == NULL) {
        return false;
    }
    *status = (struct book8_network_status){
        .dns = BROWSER_TEST_DNS,
        .ready = true
    };
    return true;
}

static enum book8_net_result capture_send(void *context,
    const void *frame, size_t length)
{
    (void)context;
    assert(length <= sizeof(browser_test_tx));
    memcpy(browser_test_tx, frame, length);
    browser_test_tx_size = length;
    return BOOK8_NET_OK;
}

static enum book8_net_result empty_receive(void *context, void *frame,
    size_t capacity, size_t *length)
{
    (void)context; (void)frame; (void)capacity;
    *length = 0u;
    return BOOK8_NET_EMPTY;
}

static struct book8_net *browser_network(void)
{
    return &browser_test_net;
}

static bool media_play_wav(const uint8_t *wav, uint32_t size)
{
    ++media_play_calls;
    return wav != NULL && size == sizeof(test_wav)
        && memcmp(wav, "RIFF", 4u) == 0
        && memcmp(wav + 8u, "WAVE", 4u) == 0;
}

static bool media_state(struct book8gui_media_status *status)
{
    if (status == NULL) {
        return false;
    }
    *status = host_media_state;
    return true;
}

static bool media_seek(uint32_t position_ms)
{
    host_media_state.position_ms = position_ms;
    return true;
}

static bool media_set_volume(uint8_t volume)
{
    host_volume = volume;
    return true;
}

static bool audio_state(uint8_t *volume, bool *muted)
{
    if (volume == NULL || muted == NULL) {
        return false;
    }
    *volume = (uint8_t)host_volume;
    *muted = false;
    return true;
}

static bool collect_media(void *context,
    const struct book8_media_source *source)
{
    (void)context;
    assert(source != NULL);
    if (media_candidate_count < 8u) {
        media_candidates[media_candidate_count++] = *source;
    }
    return true;
}

static void put_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void initialize_test_wav(void)
{
    memset(test_wav, 0, sizeof(test_wav));
    memcpy(test_wav, "RIFF", 4u);
    put_le32(test_wav + 4u, 40u);
    memcpy(test_wav + 8u, "WAVEfmt ", 8u);
    put_le32(test_wav + 16u, 16u);
    put_le16(test_wav + 20u, 1u);
    put_le16(test_wav + 22u, 2u);
    put_le32(test_wav + 24u, 48000u);
    put_le32(test_wav + 28u, 192000u);
    put_le16(test_wav + 32u, 4u);
    put_le16(test_wav + 34u, 16u);
    memcpy(test_wav + 36u, "data", 4u);
    put_le32(test_wav + 40u, 4u);
    test_wav[44] = 1u;
    test_wav[45] = 2u;
    test_wav[46] = 3u;
    test_wav[47] = 4u;
}

static void put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8u);
    data[1] = (uint8_t)value;
}

static void put32(uint8_t *data, uint32_t value)
{
    put16(data, (uint16_t)(value >> 16u));
    put16(data + 2u, (uint16_t)value);
}

static uint16_t checksum(const uint8_t *data, size_t length, uint32_t sum)
{
    while (length >= 2u) {
        sum += ((uint32_t)data[0] << 8u) | data[1];
        data += 2u;
        length -= 2u;
    }
    if (length != 0u) {
        sum += (uint32_t)data[0] << 8u;
    }
    while (sum >> 16u) {
        sum = (sum & 65535u) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(uint32_t source, uint32_t destination,
    const uint8_t *data, size_t length)
{
    uint32_t sum = (source >> 16u) + (source & 65535u)
        + (destination >> 16u) + (destination & 65535u)
        + 6u + (uint32_t)length;
    return checksum(data, length, sum);
}

static void inject_udp(uint32_t source, uint32_t destination,
    uint16_t source_port, uint16_t destination_port,
    const uint8_t *payload, size_t payload_size)
{
    uint8_t frame[BOOK8_NET_FRAME_MAX] = {0};
    assert(payload_size + 42u <= sizeof(frame));
    memcpy(frame, browser_test_net.mac, 6u);
    memcpy(frame + 6u, browser_test_net.arp[0].mac, 6u);
    put16(frame + 12u, 0x0800u);
    uint8_t *ip = frame + 14u;
    ip[0] = 0x45u;
    put16(ip + 2u, (uint16_t)(28u + payload_size));
    ip[8] = 64u;
    ip[9] = 17u;
    put32(ip + 12u, source);
    put32(ip + 16u, destination);
    put16(ip + 10u, book8_net_checksum(ip, 20u));
    uint8_t *udp = ip + 20u;
    put16(udp, source_port);
    put16(udp + 2u, destination_port);
    put16(udp + 4u, (uint16_t)(8u + payload_size));
    memcpy(udp + 8u, payload, payload_size);
    assert(book8_net_input(&browser_test_net, frame,
        payload_size + 42u) == BOOK8_NET_OK);
}

static void inject_tcp(uint32_t source, uint16_t source_port,
    uint16_t destination_port, uint32_t sequence, uint32_t acknowledgment,
    uint8_t flags, const uint8_t *payload, size_t payload_size)
{
    uint8_t frame[BOOK8_NET_FRAME_MAX] = {0};
    assert(payload_size + 54u <= sizeof(frame));
    memcpy(frame, browser_test_net.mac, 6u);
    memcpy(frame + 6u, browser_test_net.arp[1].mac, 6u);
    put16(frame + 12u, 0x0800u);
    uint8_t *ip = frame + 14u;
    ip[0] = 0x45u;
    put16(ip + 2u, (uint16_t)(40u + payload_size));
    ip[8] = 64u;
    ip[9] = 6u;
    put32(ip + 12u, source);
    put32(ip + 16u, BROWSER_TEST_LOCAL);
    put16(ip + 10u, book8_net_checksum(ip, 20u));
    uint8_t *tcp = ip + 20u;
    put16(tcp, source_port);
    put16(tcp + 2u, destination_port);
    put32(tcp + 4u, sequence);
    put32(tcp + 8u, acknowledgment);
    tcp[12] = 0x50u;
    tcp[13] = flags;
    put16(tcp + 14u, 32768u);
    if (payload_size != 0u) {
        memcpy(tcp + 20u, payload, payload_size);
    }
    put16(tcp + 16u, tcp_checksum(source, BROWSER_TEST_LOCAL,
        tcp, 20u + payload_size));
    assert(book8_net_input(&browser_test_net, frame,
        54u + payload_size) == BOOK8_NET_OK);
}

static void setup_browser_network(void)
{
    static const uint8_t local_mac[6] = {2u, 0u, 0u, 0u, 0u, 1u};
    static const uint8_t dns_mac[6] = {2u, 0u, 0u, 0u, 0u, 53u};
    static const uint8_t web_mac[6] = {2u, 0u, 0u, 0u, 0u, 20u};
    const struct book8_net_transport transport = {
        .send = capture_send,
        .receive = empty_receive
    };
    assert(book8_net_init(&browser_test_net, &transport, local_mac)
        == BOOK8_NET_OK);
    assert(book8_net_configure(&browser_test_net, BROWSER_TEST_LOCAL,
        0xffffff00u, BOOK8_NET_IP(192, 168, 7, 1)) == BOOK8_NET_OK);
    browser_test_net.arp[0].ip = BROWSER_TEST_DNS;
    memcpy(browser_test_net.arp[0].mac, dns_mac, sizeof(dns_mac));
    browser_test_net.arp[1].ip = BROWSER_TEST_WEB;
    memcpy(browser_test_net.arp[1].mac, web_mac, sizeof(web_mac));
}

static void http_get(struct WindowApp *app,
    const struct book8gui_platform *platform, const char *url,
    const char *response)
{
    host_ticks = 1u;
    assert(book8_browser_class.open_path(platform, app, url));
    book8_browser_class.tick(platform, app);
    assert(browser_test_tx_size > 54u && browser_test_tx[23] == 6u
        && browser_test_tx[47] == 2u);
    const uint8_t *syn = browser_test_tx + 34u;
    uint32_t client_sequence = ((uint32_t)syn[4] << 24u)
        | ((uint32_t)syn[5] << 16u) | ((uint32_t)syn[6] << 8u) | syn[7];
    uint16_t client_port = (uint16_t)(((uint16_t)syn[0] << 8u) | syn[1]);
    inject_tcp(BROWSER_TEST_WEB, 80u, client_port, 1000u,
        client_sequence + 1u, 0x12u, NULL, 0u);
    book8_browser_class.tick(platform, app);
    book8_browser_class.tick(platform, app);
    uint32_t request_size = (uint32_t)(browser_test_tx_size - 54u);
    uint32_t request_ack = client_sequence + 1u + request_size;
    size_t response_size = strlen(response);
    inject_tcp(BROWSER_TEST_WEB, 80u, client_port, 1001u, request_ack,
        0x18u, (const uint8_t *)response, response_size);
    book8_browser_class.tick(platform, app);
}

static void expect_http_request(struct WindowApp *app,
    const struct book8gui_platform *platform, const char *path)
{
    book8_browser_class.tick(platform, app);
    assert(browser_test_tx_size > 54u && browser_test_tx[23] == 6u
        && browser_test_tx[47] == 2u);
    uint8_t *syn = browser_test_tx + 34u;
    uint32_t sequence = ((uint32_t)syn[4] << 24u)
        | ((uint32_t)syn[5] << 16u) | ((uint32_t)syn[6] << 8u) | syn[7];
    uint16_t client_port = (uint16_t)(((uint16_t)syn[0] << 8u) | syn[1]);
    inject_tcp(BROWSER_TEST_WEB, 80u, client_port, 2000u,
        sequence + 1u, 0x12u, NULL, 0u);
    book8_browser_class.tick(platform, app);
    book8_browser_class.tick(platform, app);
    char expected[256];
    int length = snprintf(expected, sizeof(expected),
        "GET %s HTTP/1.1\r\n", path);
    assert(length > 0 && (size_t)length < sizeof(expected));
    assert(browser_test_tx_size > 54u
        && memcmp(browser_test_tx + 54u, expected, (size_t)length) == 0);
}

static bool write_file(const char *path, const void *data, uint32_t size)
{
    const uint8_t *bytes = data;
    snprintf(saved_path, sizeof(saved_path), "%s", path);
    saved_size = size;
    if (strcmp(path, "A:\\BROWSER.CFG") == 0) {
        assert(size != 0u && size < sizeof(saved_homepage));
        memcpy(saved_homepage, data, size);
        saved_homepage[size] = '\0';
        return true;
    }
    assert(size > 54u && bytes[0] == 'B' && bytes[1] == 'M');
    uint32_t row = (640u * 3u + 3u) & ~3u;
    uint32_t pixel = 54u + (480u - 1u - 4u) * row + 4u * 3u;
    assert(bytes[pixel] == 0u && bytes[pixel + 1u] == 0u
        && bytes[pixel + 2u] == 0u);
    return true;
}

static bool read_file(const char *path, void *destination,
    uint32_t capacity, uint32_t *size)
{
    if (strcmp(path, "A:\\TONE.WAV") == 0) {
        if (capacity < sizeof(test_wav)) {
            return false;
        }
        memcpy(destination, test_wav, sizeof(test_wav));
        *size = sizeof(test_wav);
        return true;
    }
    if (strcmp(path, "A:\\BROWSER.CFG") != 0
        || saved_homepage[0] == '\0'
        || strlen(saved_homepage) >= capacity) {
        return false;
    }
    *size = (uint32_t)strlen(saved_homepage);
    memcpy(destination, saved_homepage, *size);
    return true;
}

int main(void)
{
    initialize_test_wav();
    static const char media_html[] =
        "<audio src='tone.wav'></audio>"
        "<audio><source src=\"backup.wav\" type=\"audio/wav\"></audio>"
        "<video src='clip.mp4'></video>"
        "<script>var x='<audio src=\"fake.wav\">';</script>";
    assert(book8_media_scan_html(media_html, sizeof(media_html) - 1u,
        collect_media, NULL) == 3u);
    assert(media_candidate_count == 3u);
    assert(media_candidates[0].kind == BOOK8_MEDIA_WAV
        && strcmp(media_candidates[0].url, "tone.wav") == 0);
    assert(media_candidates[1].kind == BOOK8_MEDIA_WAV
        && strcmp(media_candidates[1].url, "backup.wav") == 0);
    assert(media_candidates[2].kind == BOOK8_MEDIA_VIDEO
        && strcmp(media_candidates[2].url, "clip.mp4") == 0);
    assert(book8_media_classify_url("track.WAV?download=1")
        == BOOK8_MEDIA_WAV);

    struct book8gui_platform platform = {
        .width = 1024u,
        .height = 768u,
        .fill_rect = fill,
        .draw_rgba = draw_rgba,
        .draw_text = draw_text,
        .write_file = write_file,
        .read_file = read_file,
        .media_play_wav = media_play_wav,
        .media_state = media_state,
        .media_seek = media_seek,
        .audio_state = audio_state,
        .audio_set_volume = media_set_volume,
        .clock_ticks = ticks,
        .clock_hz = 1000u
    };
    struct WindowApp app = {
        .title = book8_paintbook_class.name,
        .bounds = {20u, 20u, 760u, 520u}
    };
    app.state = book8_paintbook_class.create(&platform);
    assert(app.state != NULL);
    book8_paintbook_class.render(&platform, &app);
    assert(bitmap_draws == 1u);
    assert(strncmp(app.title, "PaintBook - Untitled", 20u) == 0);
    struct book8gui_event event = {
        .type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = 30,
        .mouse_y = 132,
        .mouse_buttons = 1u
    };
    assert(book8_paintbook_class.handle_event(&platform, &app, &event));
    event.mouse_x = 35;
    event.mouse_y = 137;
    assert(book8_paintbook_class.handle_event(&platform, &app, &event));
    event.mouse_buttons = 0u;
    assert(book8_paintbook_class.handle_event(&platform, &app, &event));
    event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_KEY,
        .key = BOOK8GUI_KEY_CTRL | 's'
    };
    assert(book8_paintbook_class.handle_event(&platform, &app, &event));
    assert(strcmp(saved_path, "A:\\PAINTBOOK.BMP") == 0
        && saved_size == 54u + 640u * 3u * 480u);
    book8_paintbook_class.destroy(app.state);

    app = (struct WindowApp){
        .title = book8_player_class.name,
        .bounds = {20u, 20u, 500u, 260u}
    };
    host_volume = 40u;
    host_media_state = (struct book8gui_media_status){0};
    app.state = book8_player_class.create(&platform);
    assert(app.state != NULL);
    struct book8gui_event player_event = {
        .type = BOOK8GUI_EVENT_MOUSE, .mouse_x = 100, .mouse_y = 70,
        .mouse_buttons = 1u
    };
    assert(book8_player_class.handle_event(&platform, &app, &player_event));
    player_event.mouse_buttons = 0u;
    (void)book8_player_class.handle_event(&platform, &app, &player_event);
    static const char player_path[] = "A:\\TONE.WAV";
    for (uint32_t i = 0u; player_path[i] != '\0'; ++i) {
        player_event = (struct book8gui_event){
            .type = BOOK8GUI_EVENT_KEY, .key = (uint8_t)player_path[i]
        };
        assert(book8_player_class.handle_event(&platform, &app, &player_event));
    }
    player_event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_MOUSE, .mouse_x = 450, .mouse_y = 70,
        .mouse_buttons = 1u
    };
    assert(book8_player_class.handle_event(&platform, &app, &player_event));
    assert(media_play_calls == 1u);
    player_event.mouse_buttons = 0u;
    (void)book8_player_class.handle_event(&platform, &app, &player_event);
    host_media_state = (struct book8gui_media_status){
        .loaded = true, .playing = true, .position_ms = 250u,
        .duration_ms = 1000u
    };
    book8_player_class.tick(&platform, &app);
    player_event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_MOUSE, .mouse_x = 485, .mouse_y = 204,
        .mouse_buttons = 1u
    };
    assert(book8_player_class.handle_event(&platform, &app, &player_event));
    assert(host_volume == 100u);
    player_event.mouse_buttons = 0u;
    (void)book8_player_class.handle_event(&platform, &app, &player_event);
    player_event.mouse_x = 420;
    player_event.mouse_y = 162;
    player_event.mouse_buttons = 1u;
    assert(book8_player_class.handle_event(&platform, &app, &player_event));
    assert(host_media_state.position_ms > 800u
        && host_media_state.position_ms < 850u);
    book8_player_class.render(&platform, &app);
    book8_player_class.destroy(app.state);

    app = (struct WindowApp){
        .title = book8_browser_class.name,
        .bounds = {20u, 20u, 820u, 560u}
    };
    app.state = book8_browser_class.create(&platform);
    assert(app.state != NULL);
    book8_browser_class.render(&platform, &app);
    assert(strcmp(last_text, "Ready.") == 0);

    event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_KEY,
        .key = BOOK8GUI_KEY_CTRL | 'l'
    };
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    const char *unsupported = "https://example.com";
    for (uint32_t i = 0u; unsupported[i] != '\0'; ++i) {
        event.key = (uint8_t)unsupported[i];
        assert(book8_browser_class.handle_event(&platform, &app, &event));
    }
    event.key = BOOK8GUI_KEY_ENTER;
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    book8_browser_class.render(&platform, &app);
    assert(strcmp(last_text,
        "Use an HTTP URL or hostname; HTTPS is not supported.") == 0);
    assert(!book8_browser_class.open_path(&platform, &app,
        "http://192.168.1.20/"));

    event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = 350,
        .mouse_y = 55,
        .mouse_buttons = 1u
    };
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    event.mouse_buttons = 0u;
    (void)book8_browser_class.handle_event(&platform, &app, &event);
    event.mouse_y = 78;
    event.mouse_buttons = 1u;
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    book8_browser_class.render(&platform, &app);
    assert(strcmp(last_text, "Homepage saved.") == 0);
    assert(strcmp(saved_path, "A:\\BROWSER.CFG") == 0
        && strcmp(saved_homepage, "http://192.168.1.20/") == 0);

    event.mouse_y = 55;
    event.mouse_buttons = 0u;
    (void)book8_browser_class.handle_event(&platform, &app, &event);
    event.mouse_buttons = 1u;
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    event.mouse_y = 55;
    event.mouse_buttons = 0u;
    (void)book8_browser_class.handle_event(&platform, &app, &event);
    event.mouse_y = 104;
    event.mouse_buttons = 1u;
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    book8_browser_class.render(&platform, &app);
    assert(strcmp(last_text, "Cookies cleared.") == 0);
    book8_browser_class.destroy(app.state);

    app = (struct WindowApp){
        .title = book8_browser_class.name,
        .bounds = {20u, 20u, 820u, 560u}
    };
    app.state = book8_browser_class.create(&platform);
    assert(app.state != NULL);
    event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = 290,
        .mouse_y = 80,
        .mouse_buttons = 1u
    };
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    book8_browser_class.render(&platform, &app);
    assert(strcmp(last_text,
        "Network is offline or IPv4 is not configured.") == 0);
    book8_browser_class.destroy(app.state);

    setup_browser_network();
    platform.network = browser_network;
    app = (struct WindowApp){
        .title = book8_browser_class.name,
        .bounds = {20u, 20u, 820u, 560u}
    };
    app.state = book8_browser_class.create(&platform);
    assert(app.state != NULL);
    assert(book8_browser_class.open_path(&platform, &app, "www.google.com"));
    assert(browser_test_tx_size > 42u
        && browser_test_tx[23] == 17u);
    const uint8_t *query = browser_test_tx + 42u;
    size_t query_size = browser_test_tx_size - 42u;
    static const uint8_t expected_qname[] = {
        3u, 'w', 'w', 'w', 6u, 'g', 'o', 'o', 'g', 'l', 'e',
        3u, 'c', 'o', 'm'
    };
    assert(query_size > 12u
        && memcmp(query + 12u, expected_qname, sizeof(expected_qname)) == 0);
    uint8_t answer[BOOK8_NET_UDP_MAX] = {0};
    memcpy(answer, query, query_size);
    answer[2] = 0x81u;
    answer[3] = 0x80u;
    put16(answer + 6u, 1u);
    answer[query_size] = 0xc0u;
    answer[query_size + 1u] = 0x0cu;
    put16(answer + query_size + 2u, 1u);
    put16(answer + query_size + 4u, 1u);
    put32(answer + query_size + 6u, 60u);
    put16(answer + query_size + 10u, 4u);
    put32(answer + query_size + 12u, BROWSER_TEST_WEB);
    inject_udp(BROWSER_TEST_DNS, BROWSER_TEST_LOCAL, 53u, 53053u,
        answer, query_size + 16u);
    book8_browser_class.tick(&platform, &app);
    book8_browser_class.tick(&platform, &app);
    assert(browser_test_tx_size > 54u
        && browser_test_tx[23] == 6u
        && browser_test_tx[47] == 2u);
    uint8_t *syn = browser_test_tx + 34u;
    uint32_t client_sequence = ((uint32_t)syn[4] << 24u)
        | ((uint32_t)syn[5] << 16u) | ((uint32_t)syn[6] << 8u) | syn[7];
    uint16_t client_port = (uint16_t)(((uint16_t)syn[0] << 8u) | syn[1]);
    inject_tcp(BROWSER_TEST_WEB, 80u, client_port, 1000u,
        client_sequence + 1u, 0x12u, NULL, 0u);
    book8_browser_class.tick(&platform, &app);
    book8_browser_class.tick(&platform, &app);
    static const char expected_request[] =
        "GET / HTTP/1.1\r\nHost: www.google.com\r\n";
    assert(browser_test_tx_size > 54u
        && memcmp(browser_test_tx + 54u, expected_request,
            sizeof(expected_request) - 1u) == 0);
    uint32_t request_size = (uint32_t)(browser_test_tx_size - 54u);
    uint32_t request_ack = client_sequence + 1u + request_size;
    const char body[] = "<html><head><title>BOOK8 &amp; News</title>"
        "<base href='http://192.168.7.20/docs/'></head>"
        "<body><h1>Google text works</h1>"
        "<p>Tom &amp; Jerry &#8212; &#x2026; &copy;</p>"
        "<ul><li><a href='page2.html?a=1&amp;b=2'>Next page</a></li></ul>"
        "<audio src='tone.wav'></audio><video src='clip.mp4'></video>"
        "</body></html>";
    char response[512];
    int response_size = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Type: text/html\r\n\r\n%zX\r\n%s\r\n0\r\n\r\n",
        strlen(body), body);
    assert(response_size > 0 && (size_t)response_size < sizeof(response));
    inject_tcp(BROWSER_TEST_WEB, 80u, client_port, 1001u, request_ack,
        0x18u, (const uint8_t *)response, (size_t)response_size);
    book8_browser_class.tick(&platform, &app);
    inject_tcp(BROWSER_TEST_WEB, 80u, client_port,
        1001u + (uint32_t)response_size, request_ack,
        0x11u, NULL, 0u);
    book8_browser_class.tick(&platform, &app);
    browser_test_net.tcp_context = NULL;
    browser_test_net.tcp_input = NULL;
    drawn_text_count = 0u;
    book8_browser_class.render(&platform, &app);
    bool displayed_google_text = false;
    bool displayed_audio_link = false, displayed_video_link = false;
    for (uint32_t i = 0u; i < drawn_text_count; ++i) {
        if (strstr(drawn_text[i], "h1: Google text works") != NULL) {
            displayed_google_text = true;
        }
        if (strstr(drawn_text[i], "Audio WAV") != NULL) {
            displayed_audio_link = true;
        }
        if (strstr(drawn_text[i], "Video unsupported") != NULL) {
            displayed_video_link = true;
        }
    }
    assert(displayed_google_text && displayed_audio_link
        && displayed_video_link);
    assert(strcmp(app.title, "Internet Explorer - BOOK8 & News") == 0);
    bool displayed_entities = false, displayed_anchor_label = false;
    for (uint32_t i = 0u; i < drawn_text_count; ++i) {
        if (strstr(drawn_text[i], "Tom & Jerry - ... (c)") != NULL) {
            displayed_entities = true;
        }
        if (strstr(drawn_text[i], "Next page") != NULL) {
            displayed_anchor_label = true;
        }
    }
    assert(displayed_entities && displayed_anchor_label);

    event = (struct book8gui_event){
        .type = BOOK8GUI_EVENT_MOUSE,
        .mouse_x = 45,
        .mouse_y = 188,
        .mouse_buttons = 1u
    };
    assert(book8_browser_class.handle_event(&platform, &app, &event));
    expect_http_request(&app, &platform,
        "/docs/page2.html?a=1&b=2");

    assert(book8_browser_class.open_path(&platform, &app,
        "http://192.168.7.20/tone.wav"));
    book8_browser_class.tick(&platform, &app);
    assert(browser_test_tx_size > 54u && browser_test_tx[23] == 6u
        && browser_test_tx[47] == 2u);
    uint8_t *media_syn = browser_test_tx + 34u;
    uint32_t media_client_sequence = ((uint32_t)media_syn[4] << 24u)
        | ((uint32_t)media_syn[5] << 16u)
        | ((uint32_t)media_syn[6] << 8u) | media_syn[7];
    uint16_t media_client_port =
        (uint16_t)(((uint16_t)media_syn[0] << 8u) | media_syn[1]);
    inject_tcp(BROWSER_TEST_WEB, 80u, media_client_port, 3000u,
        media_client_sequence + 1u, 0x12u, NULL, 0u);
    book8_browser_class.tick(&platform, &app);
    book8_browser_class.tick(&platform, &app);
    assert(browser_test_tx_size > 54u
        && memcmp(browser_test_tx + 54u, "GET /tone.wav HTTP/1.1\r\n",
            sizeof("GET /tone.wav HTTP/1.1\r\n") - 1u) == 0);
    uint32_t media_request_size = (uint32_t)(browser_test_tx_size - 54u);
    uint32_t media_request_ack =
        media_client_sequence + 1u + media_request_size;
    char media_response[256];
    int media_header_size = snprintf(media_response, sizeof(media_response),
        "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n"
        "Content-Type: audio/wav\r\n\r\n", (unsigned)sizeof(test_wav));
    assert(media_header_size > 0
        && (size_t)media_header_size + sizeof(test_wav)
            <= sizeof(media_response));
    memcpy(media_response + media_header_size, test_wav, sizeof(test_wav));
    inject_tcp(BROWSER_TEST_WEB, 80u, media_client_port, 3001u,
        media_request_ack, 0x18u, (const uint8_t *)media_response,
        (size_t)media_header_size + sizeof(test_wav));
    book8_browser_class.tick(&platform, &app);
    assert(media_play_calls == 2u);
    book8_browser_class.render(&platform, &app);
    assert(strcmp(last_text, "Playing WAV audio.") == 0);
    book8_browser_class.destroy(app.state);

    app = (struct WindowApp){
        .title = book8_browser_class.name,
        .bounds = {20u, 20u, 820u, 560u}
    };
    app.state = book8_browser_class.create(&platform);
    assert(app.state != NULL);
    http_get(&app, &platform, "http://192.168.7.20/start.html",
        "HTTP/1.1 302 Found\r\nLocation: /final.html\r\n"
        "Content-Length: 0\r\n\r\n");
    expect_http_request(&app, &platform, "/final.html");
    book8_browser_class.destroy(app.state);

    app = (struct WindowApp){
        .title = book8_browser_class.name,
        .bounds = {20u, 20u, 820u, 560u}
    };
    app.state = book8_browser_class.create(&platform);
    assert(app.state != NULL);
    static const char refresh_body[] =
        "<html><head><meta http-equiv='refresh' "
        "content='0; url=/refreshed.html'></head>"
        "<body>Refresh test</body></html>";
    char refresh_response[512];
    int refresh_length = snprintf(refresh_response, sizeof(refresh_response),
        "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n"
        "Content-Type: text/html\r\n\r\n%s",
        (unsigned)(sizeof(refresh_body) - 1u), refresh_body);
    assert(refresh_length > 0
        && (size_t)refresh_length < sizeof(refresh_response));
    http_get(&app, &platform, "http://192.168.7.20/refresh.html",
        refresh_response);
    host_ticks = 5000u;
    book8_browser_class.tick(&platform, &app);
    expect_http_request(&app, &platform, "/refreshed.html");
    book8_browser_class.destroy(app.state);
    return 0;
}
