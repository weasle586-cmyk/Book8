#include "book8_player.h"
#include "book8_ui.h"

#include "../../drivers/hda_stream.h"
#include "../../include/stdio.h"
#include "../../include/string.h"

#define PLAYER_PATH_CAPACITY 160u
#define PLAYER_FACE 0xc0c0c0u
#define PLAYER_LIGHT 0xffffffu
#define PLAYER_SHADOW 0x808080u
#define PLAYER_EDGE 0x000000u
#define PLAYER_TEXT 0x000000u
#define PLAYER_FIELD 0xffffffu
#define PLAYER_SELECT 0x000080u
#define PLAYER_SELECT_TEXT 0xffffffu

enum player_drag {
    PLAYER_DRAG_NONE,
    PLAYER_DRAG_TIMELINE,
    PLAYER_DRAG_VOLUME
};

struct player_state {
    char path[PLAYER_PATH_CAPACITY];
    char message[128];
    uint32_t position_ms;
    uint32_t duration_ms;
    uint8_t volume;
    uint8_t previous_buttons;
    bool path_focused;
    bool loaded;
    bool playing;
    bool paused;
    bool failed;
    enum player_drag drag;
};

static struct player_state player;
static uint8_t wav_buffer[HDA_STREAM_MAX_WAV_BYTES];

static bool inside(const struct book8gui_rect *rect, int32_t x, int32_t y)
{
    return x >= (int32_t)rect->x && y >= (int32_t)rect->y
        && x < (int32_t)(rect->x + rect->width)
        && y < (int32_t)(rect->y + rect->height);
}

static void bevel(const struct book8gui_platform *p,
    const struct book8gui_rect *r, bool sunken)
{
    uint32_t top = sunken ? PLAYER_SHADOW : PLAYER_LIGHT;
    uint32_t bottom = sunken ? PLAYER_LIGHT : PLAYER_SHADOW;
    p->fill_rect(r->x, r->y, r->width, 1u, top);
    p->fill_rect(r->x, r->y, 1u, r->height, top);
    p->fill_rect(r->x, r->y + r->height - 1u, r->width, 1u, bottom);
    p->fill_rect(r->x + r->width - 1u, r->y, 1u, r->height, bottom);
}

static void draw_button(const struct book8gui_platform *p,
    const struct book8gui_rect *r, const char *label, bool pressed)
{
    p->fill_rect(r->x, r->y, r->width, r->height, PLAYER_FACE);
    bevel(p, r, pressed);
    p->draw_text(r->x + 7u, r->y + 6u, label, PLAYER_TEXT, PLAYER_FACE);
}

static void draw_field(const struct book8gui_platform *p,
    const struct book8gui_rect *r, const char *text, bool focused)
{
    p->fill_rect(r->x, r->y, r->width, r->height, PLAYER_FIELD);
    bevel(p, r, true);
    uint32_t columns = r->width > 8u ? (r->width - 8u) / 8u : 0u;
    uint32_t length = (uint32_t)strlen(text);
    if (length > columns) {
        text += length - columns;
    }
    p->draw_text(r->x + 4u, r->y + 5u, text,
        focused ? PLAYER_SELECT_TEXT : PLAYER_TEXT,
        focused ? PLAYER_SELECT : PLAYER_FIELD);
}

static void update_media_state(const struct book8gui_platform *p)
{
    if (p->media_state == NULL) {
        player.loaded = player.playing = player.paused = false;
        player.failed = true;
        return;
    }
    struct book8gui_media_status state = {0};
    if (!p->media_state(&state)) {
        player.loaded = player.playing = player.paused = false;
        player.failed = true;
        return;
    }
    player.position_ms = state.position_ms;
    player.duration_ms = state.duration_ms;
    player.loaded = state.loaded;
    player.playing = state.playing;
    player.paused = state.paused;
    player.failed = state.failed;
}

static void set_message(const char *text)
{
    snprintf(player.message, sizeof(player.message), "%s", text);
}

static bool start_wav(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    if (player.path[0] == '\0') {
        set_message("Enter a WAV file path.");
        book8gui_invalidate();
        return false;
    }
    if (p->read_file == NULL || p->media_play_wav == NULL) {
        set_message("File loading or audio playback is unavailable.");
        book8gui_invalidate();
        return false;
    }
    uint32_t size = 0u;
    if (!p->read_file(player.path, wav_buffer, sizeof(wav_buffer), &size)) {
        const char *error = p->last_error_text != NULL
            ? p->last_error_text() : NULL;
        set_message(error != NULL && error[0] != '\0'
            ? error : "Could not read the selected WAV file.");
        book8gui_invalidate();
        return false;
    }
    book8gui_note_disk(app, size, 0u);
    if (size == 0u || size > sizeof(wav_buffer)) {
        set_message("WAV file is empty or exceeds the 2 MiB limit.");
        book8gui_invalidate();
        return false;
    }
    if (!p->media_play_wav(wav_buffer, size)) {
        set_message("Unsupported WAV or HDA output is unavailable.");
        book8gui_invalidate();
        return false;
    }
    set_message("Playing 48 kHz stereo PCM WAV.");
    player.failed = false;
    update_media_state(p);
    book8gui_invalidate();
    return true;
}

static void play_or_resume(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    update_media_state(p);
    if (player.loaded && player.paused && p->media_pause != NULL) {
        if (p->media_pause(false)) {
            set_message("Playback resumed.");
        } else {
            set_message("Could not resume playback.");
        }
        book8gui_invalidate();
        return;
    }
    (void)start_wav(p, app);
}

static void pause_playback(const struct book8gui_platform *p)
{
    update_media_state(p);
    if (!player.loaded || p->media_pause == NULL) {
        set_message("Load a WAV file before pausing.");
    } else if (p->media_pause(!player.paused)) {
        set_message(player.paused ? "Playback resumed." : "Playback paused.");
    } else {
        set_message("Could not change playback state.");
    }
    update_media_state(p);
    book8gui_invalidate();
}

static void stop_playback(const struct book8gui_platform *p)
{
    if (p->media_stop != NULL) {
        p->media_stop();
    }
    set_message("Playback stopped.");
    update_media_state(p);
    book8gui_invalidate();
}

static void seek_to(const struct book8gui_platform *p, uint32_t x,
    const struct book8gui_rect *track)
{
    if (!player.loaded || player.duration_ms == 0u || p->media_seek == NULL
        || track->width <= 1u) {
        return;
    }
    uint32_t offset = x <= track->x ? 0u
        : x >= track->x + track->width ? track->width - 1u : x - track->x;
    uint32_t position = (uint32_t)(((uint64_t)offset * player.duration_ms)
        / (track->width - 1u));
    if (p->media_seek(position)) {
        player.position_ms = position;
        set_message(player.paused ? "Playback paused at selected position."
                                  : "Seeking.");
    } else {
        set_message("Could not seek in this WAV file.");
    }
    book8gui_invalidate();
}

static void set_volume_from_x(const struct book8gui_platform *p, uint32_t x,
    const struct book8gui_rect *track)
{
    if (track->width <= 1u || p->audio_set_volume == NULL) {
        return;
    }
    uint32_t offset = x <= track->x ? 0u
        : x >= track->x + track->width ? track->width - 1u : x - track->x;
    uint8_t volume = (uint8_t)(((uint64_t)offset * 100u)
        / (track->width - 1u));
    if (p->audio_set_volume(volume)) {
        player.volume = volume;
        set_message("Volume changed.");
    } else {
        set_message("HDA volume control is unavailable.");
    }
    book8gui_invalidate();
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct book8gui_rect c = ui_client(app);
    if (c.width < 220u || c.height < 170u) {
        return;
    }
    p->fill_rect(c.x, c.y, c.width, c.height, PLAYER_FACE);

    uint32_t right = c.x + c.width;
    struct book8gui_rect path = {c.x + 10u, c.y + 10u,
        c.width > 116u ? c.width - 92u : 0u, 24u};
    struct book8gui_rect open = {right - 74u, c.y + 10u, 64u, 24u};
    p->draw_text(c.x + 10u, c.y + 42u, "File:", PLAYER_TEXT, PLAYER_FACE);
    draw_field(p, &path, player.path, player.path_focused);
    draw_button(p, &open, "Load", false);

    struct book8gui_rect play = {c.x + 18u, c.y + 58u, 62u, 26u};
    struct book8gui_rect pause = {c.x + 88u, c.y + 58u, 62u, 26u};
    struct book8gui_rect stop = {c.x + 158u, c.y + 58u, 62u, 26u};
    draw_button(p, &play, "Play", false);
    draw_button(p, &pause, "Pause", false);
    draw_button(p, &stop, "Stop", false);

    struct book8gui_rect timeline = {c.x + 18u, c.y + 112u,
        c.width - 36u, 12u};
    char time_text[48];
    snprintf(time_text, sizeof(time_text), "%u:%02u / %u:%02u",
        player.position_ms / 60000u, (player.position_ms / 1000u) % 60u,
        player.duration_ms / 60000u, (player.duration_ms / 1000u) % 60u);
    p->draw_text(c.x + 18u, c.y + 92u, time_text, PLAYER_TEXT, PLAYER_FACE);
    p->fill_rect(timeline.x, timeline.y, timeline.width, timeline.height,
        PLAYER_FIELD);
    bevel(p, &timeline, true);
    if (player.duration_ms != 0u) {
        uint32_t progress = (uint32_t)(((uint64_t)(timeline.width - 2u)
            * player.position_ms) / player.duration_ms);
        p->fill_rect(timeline.x + 1u, timeline.y + 1u, progress,
            timeline.height - 2u, PLAYER_SELECT);
    }

    uint8_t current_volume = player.volume;
    bool muted = false;
    bool audio_available = p->audio_state != NULL
        && p->audio_state(&current_volume, &muted);
    (void)muted;
    if (audio_available) {
        player.volume = current_volume;
    }
    struct book8gui_rect volume = {c.x + 82u, c.y + 154u,
        c.width - 112u, 12u};
    char volume_text[16];
    snprintf(volume_text, sizeof(volume_text), "Volume: %u%%", player.volume);
    p->draw_text(c.x + 10u, c.y + 153u, "Volume:", PLAYER_TEXT, PLAYER_FACE);
    p->fill_rect(volume.x, volume.y, volume.width, volume.height, PLAYER_FIELD);
    bevel(p, &volume, true);
    if (volume.width > 2u) {
        uint32_t fill = (uint32_t)(((uint64_t)(volume.width - 2u)
            * player.volume) / 100u);
        p->fill_rect(volume.x + 1u, volume.y + 1u, fill,
            volume.height - 2u, PLAYER_SELECT);
    }
    p->draw_text(right - 58u, c.y + 153u, volume_text,
        PLAYER_TEXT, PLAYER_FACE);

    struct book8gui_rect status = {c.x + 10u, c.y + c.height - 27u,
        c.width - 20u, 20u};
    p->draw_text(status.x, status.y + 5u, player.message,
        player.failed ? 0x800000u : PLAYER_TEXT, PLAYER_FACE);
}

static void tick(const struct book8gui_platform *p, struct WindowApp *app)
{
    (void)app;
    update_media_state(p);
}

static bool handle_event(const struct book8gui_platform *p,
    struct WindowApp *app, const struct book8gui_event *event)
{
    struct book8gui_rect c = ui_client(app);
    struct book8gui_rect path = {c.x + 10u, c.y + 10u,
        c.width > 116u ? c.width - 92u : 0u, 24u};
    struct book8gui_rect open = {c.x + c.width - 74u, c.y + 10u, 64u, 24u};
    struct book8gui_rect timeline = {c.x + 18u, c.y + 112u,
        c.width - 36u, 12u};
    struct book8gui_rect volume = {c.x + 82u, c.y + 154u,
        c.width - 112u, 12u};

    if (event->type == BOOK8GUI_EVENT_KEY && player.path_focused) {
        uint32_t key = event->key & BOOK8GUI_KEY_MASK;
        if ((event->key & BOOK8GUI_KEY_CTRL) != 0u
            && (key == 'a' || key == 'A')) {
            player.path[0] = '\0';
        } else if (key == BOOK8GUI_KEY_ENTER) {
            (void)start_wav(p, app);
        } else if (key == BOOK8GUI_KEY_ESCAPE) {
            player.path_focused = false;
        } else if (key == BOOK8GUI_KEY_BACKSPACE) {
            uint32_t length = (uint32_t)strlen(player.path);
            if (length != 0u) {
                player.path[length - 1u] = '\0';
            }
        } else if (key >= 32u && key <= 126u
            && strlen(player.path) + 1u < sizeof(player.path)) {
            uint32_t length = (uint32_t)strlen(player.path);
            player.path[length] = (char)key;
            player.path[length + 1u] = '\0';
        } else {
            return false;
        }
        book8gui_invalidate();
        return true;
    }
    if (event->type != BOOK8GUI_EVENT_MOUSE) {
        return false;
    }
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool pressed = down && (player.previous_buttons & 1u) == 0u;
    if (player.drag != PLAYER_DRAG_NONE) {
        if (down) {
            if (player.drag == PLAYER_DRAG_TIMELINE) {
                seek_to(p, (uint32_t)(event->mouse_x < 0 ? 0 : event->mouse_x),
                    &timeline);
            } else {
                set_volume_from_x(p,
                    (uint32_t)(event->mouse_x < 0 ? 0 : event->mouse_x),
                    &volume);
            }
        } else {
            player.drag = PLAYER_DRAG_NONE;
        }
        player.previous_buttons = event->mouse_buttons;
        return true;
    }
    if (pressed) {
        if (inside(&open, event->mouse_x, event->mouse_y)) {
            (void)start_wav(p, app);
        } else if (inside(&path, event->mouse_x, event->mouse_y)) {
            player.path_focused = true;
            book8gui_invalidate();
        } else {
            struct book8gui_rect play = {c.x + 18u, c.y + 58u, 62u, 26u};
            struct book8gui_rect pause = {c.x + 88u, c.y + 58u, 62u, 26u};
            struct book8gui_rect stop = {c.x + 158u, c.y + 58u, 62u, 26u};
            if (inside(&play, event->mouse_x, event->mouse_y)) {
                play_or_resume(p, app);
            } else if (inside(&pause, event->mouse_x, event->mouse_y)) {
                pause_playback(p);
            } else if (inside(&stop, event->mouse_x, event->mouse_y)) {
                stop_playback(p);
            } else if (inside(&timeline, event->mouse_x, event->mouse_y)) {
                player.drag = PLAYER_DRAG_TIMELINE;
                seek_to(p, (uint32_t)event->mouse_x, &timeline);
            } else if (inside(&volume, event->mouse_x, event->mouse_y)) {
                player.drag = PLAYER_DRAG_VOLUME;
                set_volume_from_x(p, (uint32_t)event->mouse_x, &volume);
            } else {
                player.path_focused = false;
            }
        }
    }
    player.previous_buttons = event->mouse_buttons;
    return true;
}

static void *create(const struct book8gui_platform *p)
{
    player = (struct player_state){0};
    set_message("Open a 48 kHz stereo PCM WAV file to begin.");
    if (p->audio_state != NULL) {
        bool muted = false;
        (void)p->audio_state(&player.volume, &muted);
    }
    update_media_state(p);
    return &player;
}

static void destroy(void *state)
{
    (void)state;
    if (player.loaded && player.playing) {
        player.loaded = false;
    }
}

const struct book8gui_app_class book8_player_class = {
    .name = "BOOK8 Player",
    .default_width = 500u,
    .default_height = 260u,
    .single_instance = true,
    .wants_all_keys = true,
    .state_size = sizeof(struct player_state),
    .create = create,
    .destroy = destroy,
    .render = render,
    .handle_event = handle_event,
    .tick = tick
};
