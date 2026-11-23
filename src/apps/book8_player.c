#include "book8_player.h"
#include "book8_ui.h"

#include <stddef.h>
#include "../../include/syscall.h"
#include "../../include/stdio.h"
#include "../../include/string.h"
#include "book8_media_extension.h"

#define PLAYER_PATH_CAPACITY 160u
#define PLAYER_MESSAGE_CAPACITY 128u
#define PLAYER_PLAYLIST_COUNT 8u
#define PLAYER_MENU_COUNT 8u
#define PLAYER_MENU_ITEM_LIMIT 14u
#define PLAYER_PREFS_SECTION_COUNT 6u
#define PLAYER_FFT_POINTS 32u
#define PLAYER_FFT_BINS (PLAYER_FFT_POINTS / 2u)
#define PLAYER_FFT_PCM_BYTES (PLAYER_FFT_POINTS * 4u)

#define PL_BACKGROUND 0x101010u
#define PL_PANEL 0x1c1c1cu
#define PL_CANVAS 0x000000u
#define PL_TEXT 0xf2f2f2u
#define PL_MUTED 0x9a9a9au
#define PL_BORDER 0x404040u
#define PL_BUTTON 0x2b2b2bu
#define PL_BUTTON_LIGHT 0x555555u
#define PL_BUTTON_SHADOW 0x111111u
#define PL_ORANGE 0xff8800u
#define PL_ORANGE_DARK 0xc05a00u
#define PL_SELECT_TEXT 0x000000u
#define PL_FIELD 0x080808u
#define PL_GREEN 0x35c659u
#define PL_RED 0xe81123u

#define CMD_SEPARATOR 0u
#define CMD_OPEN_FILE 1u
#define CMD_OPEN_NETWORK 2u
#define CMD_OPEN_FOLDER 3u
#define CMD_QUIT 4u
#define CMD_PLAY_PAUSE 5u
#define CMD_STOP 6u
#define CMD_PREV 7u
#define CMD_NEXT 8u
#define CMD_GOTO_TIME 9u
#define CMD_BOOKMARK_CREATE 10u
#define CMD_BOOKMARKS 11u
#define CMD_SPEED_SLOWER 12u
#define CMD_SPEED_NORMAL 13u
#define CMD_SPEED_FASTER 14u
#define CMD_AUDIO_STEREO 15u
#define CMD_AUDIO_MONO 16u
#define CMD_MUTE 17u
#define CMD_VISUALIZATION 18u
#define CMD_MAX_VOLUME 19u
#define CMD_VIDEO_INFO 20u
#define CMD_FULLSCREEN 21u
#define CMD_SUBTITLE_INFO 22u
#define CMD_PREFERENCES 23u
#define CMD_CODEC_INFO 24u
#define CMD_RESET_PREFS 25u
#define CMD_VIEW_PLAYLIST 26u
#define CMD_VIEW_NOW 27u
#define CMD_STATUS_BAR 28u
#define CMD_ABOUT 29u
#define CMD_REPEAT_NONE 30u
#define CMD_REPEAT_ALL 31u
#define CMD_REPEAT_ONE 32u

enum player_view {
    PLAYER_VIEW_NOW_PLAYING,
    PLAYER_VIEW_PLAYLIST,
    PLAYER_VIEW_PREFERENCES,
    PLAYER_VIEW_ABOUT
};

enum player_dialog {
    PLAYER_DIALOG_NONE,
    PLAYER_DIALOG_OPEN,
    PLAYER_DIALOG_GOTO,
    PLAYER_DIALOG_BOOKMARKS,
    PLAYER_DIALOG_CONFIRM_STREAM
};

enum player_open_tab {
    PLAYER_OPEN_FILE,
    PLAYER_OPEN_NETWORK
};

enum player_drag {
    PLAYER_DRAG_NONE,
    PLAYER_DRAG_TIMELINE,
    PLAYER_DRAG_VOLUME
};

struct player_state {
    char path[PLAYER_PATH_CAPACITY];
    char url[BOOK8_MEDIA_URL_CAPACITY];
    char goto_field[16u];
    char message[PLAYER_MESSAGE_CAPACITY];
    char playlist[PLAYER_PLAYLIST_COUNT][PLAYER_PATH_CAPACITY];
    struct book8_media_bookmark_set bookmarks;
    uint32_t playlist_count;
    uint32_t playlist_index;
    uint32_t position_ms;
    uint32_t duration_ms;
    struct book8_media_preferences prefs;
    struct book8_media_preferences prefs_saved;
    uint8_t volume;
    uint8_t previous_buttons;
    uint8_t speed_index;
    uint8_t menu;
    uint8_t prefs_section;
    bool path_focused;
    bool url_focused;
    bool loaded;
    bool playing;
    bool paused;
    bool failed;
    enum player_view view;
    enum player_dialog dialog;
    enum player_open_tab open_tab;
    enum player_drag drag;
    uint8_t repeat_counter; /* software repeat: counts down to auto-restart */
};

struct menu_entry {
    uint8_t command;
    const char *label;
    const char *shortcut;
};

static const struct menu_entry menu_items[PLAYER_MENU_COUNT][PLAYER_MENU_ITEM_LIMIT] = {
    {
        {CMD_OPEN_FILE, "Open File...", "Ctrl+O"},
        {CMD_OPEN_NETWORK, "Open Network Stream...", "Ctrl+N"},
        {CMD_OPEN_FOLDER, "Open Folder / Playlist...", "Ctrl+F"},
        {CMD_SEPARATOR, "", NULL},
        {CMD_QUIT, "Quit", "Ctrl+Q"}
    },
    {
        {CMD_PLAY_PAUSE, "Play / Pause", "Space"},
        {CMD_STOP, "Stop", "S"},
        {CMD_SEPARATOR, "", NULL},
        {CMD_PREV, "Previous", "P"},
        {CMD_NEXT, "Next", "N"},
        {CMD_GOTO_TIME, "Jump to Specific Time...", "Ctrl+T"},
        {CMD_SEPARATOR, "", NULL},
        {CMD_BOOKMARK_CREATE, "Create Bookmark", "Ctrl+B"},
        {CMD_BOOKMARKS, "Manage Bookmarks...", ""},
        {CMD_SEPARATOR, "", NULL},
        {CMD_SPEED_SLOWER, "Slower", "["},
        {CMD_SPEED_NORMAL, "Normal Speed", "\\"},
        {CMD_SPEED_FASTER, "Faster", "]"}
    },
    {
        {CMD_AUDIO_STEREO, "Audio Track: Stereo", ""},
        {CMD_AUDIO_MONO, "Audio Track: Mono", ""},
        {CMD_SEPARATOR, "", NULL},
        {CMD_MUTE, "Mute", "M"},
        {CMD_REPEAT_NONE, "Repeat: Off", ""},
        {CMD_REPEAT_ALL, "Repeat: All", ""},
        {CMD_REPEAT_ONE, "Repeat: One", ""},
        {CMD_VISUALIZATION, "Visualizations", ""},
        {CMD_MAX_VOLUME, "Maximum Volume...", ""}
    },
    {
        {CMD_VIDEO_INFO, "Video Track Information", ""},
        {CMD_FULLSCREEN, "Fullscreen", "F"}
    },
    {
        {CMD_SUBTITLE_INFO, "Add Subtitle File...", ""}
    },
    {
        {CMD_PREFERENCES, "Preferences...", "Ctrl+P"},
        {CMD_CODEC_INFO, "Codec Information", "Ctrl+J"},
        {CMD_SEPARATOR, "", NULL},
        {CMD_RESET_PREFS, "Reset Preferences", ""}
    },
    {
        {CMD_VIEW_PLAYLIST, "Playlist", "Ctrl+L"},
        {CMD_VIEW_NOW, "Now Playing", ""},
        {CMD_SEPARATOR, "", NULL},
        {CMD_STATUS_BAR, "Status Bar", ""}
    },
    {
        {CMD_ABOUT, "About BOOK8 Player", "F1"}
    }
};

static const char *const menu_labels[PLAYER_MENU_COUNT] = {
    "Media", "Playback", "Audio", "Video", "Subtitle", "Tools", "View", "Help"
};

static const uint32_t menu_widths[PLAYER_MENU_COUNT] = {
    50u, 64u, 48u, 48u, 56u, 46u, 40u, 40u
};

static const char *const prefs_sections[PLAYER_PREFS_SECTION_COUNT] = {
    "Interface", "Audio", "Input / Codecs", "Media Plugins",
    "Browser Integration", "Hotkeys"
};

static const uint32_t speeds[] = {50u, 75u, 100u, 150u, 200u};

static struct player_state player;
static uint8_t wav_buffer[2u * 1024u * 1024u];

struct player_layout {
    struct book8gui_rect client, menu, toolbar, canvas, seek, status;
    struct book8gui_rect prev, play, pause, stop, next, playlist, prefs;
    struct book8gui_rect mute, volume_track, repeat, bookmark, speed;
    uint32_t time_y;
    uint32_t controls_y;
};

static bool inside(struct book8gui_rect rect, int32_t x, int32_t y)
{
    return x >= (int32_t)rect.x && y >= (int32_t)rect.y
        && x < (int32_t)(rect.x + rect.width)
        && y < (int32_t)(rect.y + rect.height);
}

static uint32_t text_width(const char *text)
{
    return (uint32_t)strlen(text) * 8u;
}

static void draw_text_centered(const struct book8gui_platform *p,
    struct book8gui_rect rect, const char *text,
    uint32_t foreground, uint32_t background)
{
    uint32_t width = text_width(text);
    uint32_t x = rect.width > width ? rect.x + (rect.width - width) / 2u
        : rect.x;
    uint32_t y = rect.height > 12u ? rect.y + (rect.height - 12u) / 2u
        : rect.y;
    p->draw_text(x, y, text, foreground, background);
}

static void draw_text_clipped(const struct book8gui_platform *p, uint32_t x,
    uint32_t y, uint32_t width, const char *text,
    uint32_t foreground, uint32_t background)
{
    char clipped[128u];
    uint32_t maximum = width / 8u;
    uint32_t length = 0u;
    if (maximum >= sizeof(clipped)) {
        maximum = sizeof(clipped) - 1u;
    }
    while (text[length] != '\0' && length < maximum) {
        clipped[length] = text[length];
        ++length;
    }
    clipped[length] = '\0';
    p->draw_text(x, y, clipped, foreground, background);
}

static void bevel(const struct book8gui_platform *p,
    struct book8gui_rect rect, bool pressed, uint32_t background)
{
    if (rect.width == 0u || rect.height == 0u) {
        return;
    }
    uint32_t light = pressed ? PL_BUTTON_SHADOW : PL_BUTTON_LIGHT;
    uint32_t shadow = pressed ? PL_BUTTON_LIGHT : PL_BUTTON_SHADOW;
    p->fill_rect(rect.x, rect.y, rect.width, 1u, light);
    p->fill_rect(rect.x, rect.y, 1u, rect.height, light);
    p->fill_rect(rect.x, rect.y + rect.height - 1u, rect.width, 1u,
        shadow);
    p->fill_rect(rect.x + rect.width - 1u, rect.y, 1u, rect.height,
        shadow);
    (void)background;
}

static void draw_button(const struct book8gui_platform *p,
    struct book8gui_rect rect, const char *label, bool active)
{
    uint32_t background = active ? PL_ORANGE : PL_BUTTON;
    p->fill_rect(rect.x, rect.y, rect.width, rect.height, background);
    bevel(p, rect, active, background);
    draw_text_centered(p, rect, label,
        active ? PL_SELECT_TEXT : PL_TEXT, background);
}

static void draw_field(const struct book8gui_platform *p,
    struct book8gui_rect rect, const char *text, bool focused)
{
    p->fill_rect(rect.x, rect.y, rect.width, rect.height, PL_FIELD);
    p->fill_rect(rect.x, rect.y, rect.width, 1u,
        focused ? PL_ORANGE : PL_BORDER);
    p->fill_rect(rect.x, rect.y + rect.height - 1u, rect.width, 1u,
        focused ? PL_ORANGE : PL_BORDER);
    p->fill_rect(rect.x, rect.y, 1u, rect.height,
        focused ? PL_ORANGE : PL_BORDER);
    p->fill_rect(rect.x + rect.width - 1u, rect.y, 1u, rect.height,
        focused ? PL_ORANGE : PL_BORDER);
    uint32_t length = (uint32_t)strlen(text);
    uint32_t columns = rect.width > 8u ? (rect.width - 8u) / 8u : 0u;
    if (length > columns) {
        text += length - columns;
    }
    p->draw_text(rect.x + 4u,
        rect.height > 12u ? rect.y + (rect.height - 12u) / 2u : rect.y,
        text, PL_TEXT, PL_FIELD);
}

static void format_time(uint32_t milliseconds, char *buffer, uint32_t capacity)
{
    uint32_t seconds = milliseconds / 1000u;
    snprintf(buffer, capacity, "%u:%02u", seconds / 60u, seconds % 60u);
}

static void set_message(const char *text)
{
    snprintf(player.message, sizeof(player.message), "%s", text);
}

static struct player_layout player_layout(const struct WindowApp *app)
{
    struct player_layout l = {0};
    l.client = ui_client(app);
    struct book8gui_rect c = l.client;
    uint32_t right = c.x + c.width;

    l.menu = (struct book8gui_rect){c.x, c.y, c.width, 18u};
    l.toolbar = (struct book8gui_rect){c.x, c.y + 20u, c.width, 32u};
    l.status = (struct book8gui_rect){c.x, c.y + c.height - 14u,
        c.width, 14u};
    l.controls_y = l.status.y > 30u ? l.status.y - 30u : c.y + c.height;
    l.seek = (struct book8gui_rect){c.x + 96u, l.controls_y - 14u,
        c.width > 116u ? c.width - 106u : 0u, 8u};
    l.time_y = l.seek.y > 12u ? l.seek.y - 12u : c.y;

    uint32_t canvas_y = c.y + 54u;
    l.canvas = (struct book8gui_rect){c.x, canvas_y, c.width,
        l.time_y > canvas_y + 4u ? l.time_y - 4u - canvas_y : 0u};

    l.prev = (struct book8gui_rect){c.x + 6u, l.toolbar.y + 2u, 34u, 28u};
    l.play = (struct book8gui_rect){c.x + 44u, l.toolbar.y + 2u, 44u, 28u};
    l.pause = (struct book8gui_rect){c.x + 92u, l.toolbar.y + 2u, 44u, 28u};
    l.stop = (struct book8gui_rect){c.x + 140u, l.toolbar.y + 2u, 44u, 28u};
    l.next = (struct book8gui_rect){c.x + 188u, l.toolbar.y + 2u, 34u, 28u};
    l.playlist = (struct book8gui_rect){right - 100u, l.toolbar.y + 2u,
        44u, 28u};
    l.prefs = (struct book8gui_rect){right - 52u, l.toolbar.y + 2u,
        44u, 28u};

    l.mute = (struct book8gui_rect){c.x + 6u, l.controls_y, 24u, 24u};
    uint32_t volume_width = c.width > 320u ? 120u : c.width / 3u;
    l.volume_track = (struct book8gui_rect){c.x + 36u, l.controls_y + 8u,
        volume_width, 8u};
    uint32_t after_volume = c.x + 44u + volume_width;
    l.repeat = (struct book8gui_rect){after_volume, l.controls_y, 36u, 24u};
    l.bookmark = (struct book8gui_rect){after_volume + 42u, l.controls_y,
        50u, 24u};
    l.speed = (struct book8gui_rect){after_volume + 98u, l.controls_y,
        46u, 24u};
    return l;
}

static struct book8gui_rect menu_rect(const struct player_layout *layout,
    uint32_t index)
{
    uint32_t x = layout->menu.x + 2u;
    for (uint32_t i = 0u; i < index; ++i) {
        x += menu_widths[i] + 2u;
    }
    return (struct book8gui_rect){x, layout->menu.y + 1u,
        menu_widths[index], 16u};
}

static uint32_t menu_item_count(uint32_t menu)
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < PLAYER_MENU_ITEM_LIMIT; ++i) {
        if (menu_items[menu][i].label == NULL) {
            break;
        }
        ++count;
    }
    return count;
}

static struct book8gui_rect menu_dropdown(const struct player_layout *layout,
    uint32_t menu)
{
    struct book8gui_rect anchor = menu_rect(layout, menu);
    return (struct book8gui_rect){anchor.x, layout->menu.y + 18u, 210u,
        menu_item_count(menu) * 18u + 4u};
}

static struct book8gui_rect open_dialog_rect(const struct player_layout *layout)
{
    return (struct book8gui_rect){layout->client.x + 30u,
        layout->client.y + 52u, layout->client.width - 60u, 120u};
}

static struct book8gui_rect open_tab_rect(const struct player_layout *layout,
    uint32_t tab)
{
    struct book8gui_rect dialog = open_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + 8u + tab * 88u,
        dialog.y + 20u, tab == 0u ? 80u : 96u, 20u};
}

static struct book8gui_rect open_field_rect(const struct player_layout *layout)
{
    struct book8gui_rect dialog = open_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + 10u, dialog.y + 50u,
        dialog.width - 20u, 24u};
}

static struct book8gui_rect open_play_rect(const struct player_layout *layout)
{
    struct book8gui_rect dialog = open_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + dialog.width - 150u,
        dialog.y + 82u, 64u, 24u};
}

static struct book8gui_rect open_cancel_rect(const struct player_layout *layout)
{
    struct book8gui_rect dialog = open_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + dialog.width - 78u,
        dialog.y + 82u, 64u, 24u};
}

static struct book8gui_rect goto_dialog_rect(const struct player_layout *layout)
{
    return (struct book8gui_rect){layout->client.x + 80u,
        layout->client.y + 66u, layout->client.width - 160u, 96u};
}

static struct book8gui_rect goto_field_rect(const struct player_layout *layout)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + 12u, dialog.y + 34u,
        dialog.width - 24u, 24u};
}

static struct book8gui_rect goto_ok_rect(const struct player_layout *layout)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + dialog.width - 150u,
        dialog.y + 64u, 64u, 22u};
}

static struct book8gui_rect goto_cancel_rect(const struct player_layout *layout)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    return (struct book8gui_rect){dialog.x + dialog.width - 78u,
        dialog.y + 64u, 64u, 22u};
}

static struct book8gui_rect confirm_open_rect(const struct player_layout *layout)
{
    return goto_ok_rect(layout);
}

static struct book8gui_rect confirm_cancel_rect(
    const struct player_layout *layout)
{
    return goto_cancel_rect(layout);
}

static struct book8gui_rect prefs_panel(const struct player_layout *layout)
{
    return (struct book8gui_rect){layout->client.x + 2u,
        layout->client.y + 20u, layout->client.width - 4u,
        layout->client.height - 22u};
}

static struct book8gui_rect prefs_nav_rect(const struct player_layout *layout,
    uint32_t index)
{
    struct book8gui_rect panel = prefs_panel(layout);
    return (struct book8gui_rect){panel.x + 8u, panel.y + 28u + index * 22u,
        132u, 20u};
}

static struct book8gui_rect prefs_content_rect(const struct player_layout *layout)
{
    struct book8gui_rect panel = prefs_panel(layout);
    return (struct book8gui_rect){panel.x + 150u, panel.y + 28u,
        panel.width > 170u ? panel.width - 160u : 0u,
        panel.height > 78u ? panel.height - 78u : 0u};
}

static struct book8gui_rect prefs_row_rect(const struct player_layout *layout,
    uint32_t row)
{
    struct book8gui_rect content = prefs_content_rect(layout);
    return (struct book8gui_rect){content.x + 4u, content.y + row * 22u,
        content.width - 8u, 20u};
}

static struct book8gui_rect prefs_save_rect(const struct player_layout *layout)
{
    struct book8gui_rect panel = prefs_panel(layout);
    return (struct book8gui_rect){panel.x + panel.width - 190u,
        panel.y + panel.height - 30u, 56u, 22u};
}

static struct book8gui_rect prefs_cancel_rect(const struct player_layout *layout)
{
    struct book8gui_rect panel = prefs_panel(layout);
    return (struct book8gui_rect){panel.x + panel.width - 128u,
        panel.y + panel.height - 30u, 56u, 22u};
}

static struct book8gui_rect prefs_reset_rect(const struct player_layout *layout)
{
    struct book8gui_rect panel = prefs_panel(layout);
    return (struct book8gui_rect){panel.x + panel.width - 66u,
        panel.y + panel.height - 30u, 56u, 22u};
}

static struct book8gui_rect playlist_panel(const struct player_layout *layout)
{
    return (struct book8gui_rect){layout->client.x + 2u,
        layout->client.y + 20u, layout->client.width - 4u,
        layout->client.height - 22u};
}

static struct book8gui_rect playlist_row_rect(const struct player_layout *layout,
    uint32_t row)
{
    struct book8gui_rect panel = playlist_panel(layout);
    return (struct book8gui_rect){panel.x + 8u, panel.y + 32u + row * 18u,
        panel.width - 16u, 18u};
}

static struct book8gui_rect playlist_close_rect(const struct player_layout *layout)
{
    struct book8gui_rect panel = playlist_panel(layout);
    return (struct book8gui_rect){panel.x + panel.width - 64u,
        panel.y + 6u, 56u, 20u};
}

static void clamp_volume(void)
{
    if (player.volume > player.prefs.max_volume) {
        player.volume = player.prefs.max_volume;
    }
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

static void apply_preferences(const struct book8gui_platform *p)
{
    player.prefs = *book8_media_prefs();
    clamp_volume();
    if (p->audio_set_volume != NULL && player.volume != 0u) {
        (void)p->audio_set_volume(player.volume);
    }
}

static bool start_wav(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    if ((player.prefs.plugins & BOOK8_MEDIA_PLUGIN_PCM_WAV) == 0u) {
        set_message("PCM WAV plugin is disabled in Preferences.");
        book8gui_invalidate();
        return false;
    }
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
    if (player.playlist_count == 0u) {
        snprintf(player.playlist[0], sizeof(player.playlist[0]), "%s",
            player.path);
        player.playlist_count = 1u;
        player.playlist_index = 0u;
    }
    if (!book8_media_bookmarks_load(p, player.path, &player.bookmarks)) {
        memset(&player.bookmarks, 0, sizeof(player.bookmarks));
        snprintf(player.bookmarks.path, sizeof(player.bookmarks.path), "%s",
            player.path);
    }
    player.failed = false;
    set_message("Playing PCM 48 kHz stereo WAV.");
    update_media_state(p);
    clamp_volume();
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

static void play_url(const struct book8gui_platform *p)
{
    (void)p;
    if ((player.prefs.plugins & BOOK8_MEDIA_PLUGIN_HTTP_WAV) == 0u) {
        set_message("HTTP WAV plugin is disabled in Preferences.");
        book8gui_invalidate();
        return;
    }
    if (player.url[0] == '\0') {
        set_message("Enter an HTTP stream URL.");
        book8gui_invalidate();
        return;
    }
    if ((player.prefs.web & BOOK8_MEDIA_WEB_AUTOPLAY) == 0u) {
        set_message("Web audio autoplay is disabled in Preferences.");
        book8gui_invalidate();
        return;
    }
    if (book8gui_queue_utility(BOOK8_GUI_BROWSER, player.url) != 0) {
        set_message("Web Browser is unavailable for this stream.");
    } else {
        set_message("Opening HTTP stream in Web Browser.");
    }
    book8gui_invalidate();
}

struct playlist_scan {
    const char *directory;
    uint32_t count;
};

static bool collect_wav_entry(const char *name, bool directory,
    uint32_t size, void *context)
{
    struct playlist_scan *scan = context;
    (void)size;
    if (directory || scan->count >= PLAYER_PLAYLIST_COUNT
        || player.playlist_count >= PLAYER_PLAYLIST_COUNT) {
        return true;
    }
    uint32_t length = (uint32_t)strlen(name);
    if (length < 4u) {
        return true;
    }
    const char *suffix = name + length - 4u;
    if (suffix[0] != '.' || (suffix[1] != 'w' && suffix[1] != 'W')
        || (suffix[2] != 'a' && suffix[2] != 'A')
        || (suffix[3] != 'v' && suffix[3] != 'V')) {
        return true;
    }
    uint32_t directory_length = (uint32_t)strlen(scan->directory);
    char separator = directory_length != 0u
        && (scan->directory[directory_length - 1u] == '\\'
            || scan->directory[directory_length - 1u] == '/') ? '\0' : '\\';
    snprintf(player.playlist[player.playlist_count],
        sizeof(player.playlist[0]), "%s%c%s", scan->directory, separator, name);
    ++player.playlist_count;
    ++scan->count;
    return true;
}

static void open_playlist_directory(const struct book8gui_platform *p)
{
    if ((player.prefs.plugins & BOOK8_MEDIA_PLUGIN_PLAYLIST) == 0u) {
        set_message("Playlist plugin is disabled in Preferences.");
        book8gui_invalidate();
        return;
    }
    if (p->list_directory == NULL || player.path[0] == '\0') {
        set_message("Directory browsing is unavailable.");
        book8gui_invalidate();
        return;
    }
    player.playlist_count = 0u;
    player.playlist_index = 0u;
    struct playlist_scan scan = {player.path, 0u};
    if (!p->list_directory(player.path, collect_wav_entry, &scan)
        || player.playlist_count == 0u) {
        set_message("No WAV files were found in that folder.");
        book8gui_invalidate();
        return;
    }
    snprintf(player.path, sizeof(player.path), "%s",
        player.playlist[0]);
    player.view = PLAYER_VIEW_PLAYLIST;
    set_message("Playlist loaded; press Play to start.");
    book8gui_invalidate();
}

static void playlist_select(const struct book8gui_platform *p,
    struct WindowApp *app, uint32_t index)
{
    if (index >= player.playlist_count) {
        return;
    }
    player.playlist_index = index;
    char selected[PLAYER_PATH_CAPACITY];
    snprintf(selected, sizeof(selected), "%s", player.playlist[index]);
    snprintf(player.path, sizeof(player.path), "%s", selected);
    (void)start_wav(p, app);
}

static void playlist_step(const struct book8gui_platform *p,
    struct WindowApp *app, int32_t direction)
{
    if (player.playlist_count == 0u) {
        set_message("The playlist is empty.");
        book8gui_invalidate();
        return;
    }
    int32_t index = (int32_t)player.playlist_index + direction;
    if (index < 0) {
        index = (int32_t)player.playlist_count - 1;
    }
    if ((uint32_t)index >= player.playlist_count) {
        index = 0;
    }
    playlist_select(p, app, (uint32_t)index);
}

static void seek_to_position(const struct book8gui_platform *p, uint32_t position)
{
    if (!player.loaded || player.duration_ms == 0u || p->media_seek == NULL) {
        return;
    }
    if (position > player.duration_ms) {
        position = player.duration_ms;
    }
    if (p->media_seek(position)) {
        player.position_ms = position;
        set_message(player.paused ? "Playback paused at selected position."
                                  : "Seeking.");
    } else {
        set_message("Could not seek in this WAV file.");
    }
    book8gui_invalidate();
}

static void seek_to_x(const struct book8gui_platform *p, uint32_t x,
    const struct book8gui_rect *track)
{
    if (track->width <= 1u) {
        return;
    }
    uint32_t offset = x <= track->x ? 0u
        : x >= track->x + track->width ? track->width - 1u : x - track->x;
    uint32_t position = (uint32_t)(((uint64_t)offset * player.duration_ms)
        / (track->width - 1u));
    seek_to_position(p, position);
}

static void seek_delta(const struct book8gui_platform *p, int32_t delta_ms)
{
    int64_t next = (int64_t)player.position_ms + (int64_t)delta_ms;
    if (next < 0) {
        next = 0;
    }
    if ((uint64_t)next > player.duration_ms) {
        next = (int64_t)player.duration_ms;
    }
    seek_to_position(p, (uint32_t)next);
}

static bool parse_goto_time(uint32_t *position)
{
    uint32_t values[3] = {0u, 0u, 0u};
    uint32_t index = 0u;
    const char *text = player.goto_field;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            if (*text != ':' || index == 2u) {
                return false;
            }
            ++index;
            ++text;
            continue;
        }
        values[index] = values[index] * 10u + (uint32_t)(*text - '0');
        if (values[index] > 999u) {
            return false;
        }
        ++text;
    }
    if (values[1] > 59u || values[2] > 59u) {
        return false;
    }
    *position = (values[0] * 60u + values[1]) * 1000u + values[2] * 1000u;
    return true;
}

static void goto_activate(const struct book8gui_platform *p)
{
    uint32_t position = 0u;
    if (!parse_goto_time(&position) || position > player.duration_ms) {
        set_message("Enter a time within the track as hh:mm:ss.");
        book8gui_invalidate();
        return;
    }
    seek_to_position(p, position);
    player.dialog = PLAYER_DIALOG_NONE;
}

static void set_volume(const struct book8gui_platform *p, uint8_t volume)
{
    if (volume > player.prefs.max_volume) {
        volume = player.prefs.max_volume;
    }
    if (volume == 0u) {
        volume = 1u;
    }
    if (p->audio_set_volume == NULL) {
        set_message("HDA volume control is unavailable.");
    } else if (p->audio_set_volume(volume)) {
        player.volume = volume;
        set_message("Volume changed.");
    } else {
        set_message("HDA volume control is unavailable.");
    }
    book8gui_invalidate();
}

static void set_volume_from_x(const struct book8gui_platform *p, uint32_t x,
    const struct book8gui_rect *track)
{
    if (track->width <= 1u) {
        return;
    }
    uint32_t offset = x <= track->x ? 0u
        : x >= track->x + track->width ? track->width - 1u : x - track->x;
    set_volume(p, (uint8_t)(((uint64_t)offset * 100u)
        / (track->width - 1u)));
}

static void toggle_mute(const struct book8gui_platform *p)
{
    uint8_t volume = player.volume;
    bool muted = false;
    if (p->audio_state == NULL || p->audio_set_mute == NULL
        || !p->audio_state(&volume, &muted)) {
        set_message("HDA mute control is unavailable.");
    } else if (p->audio_set_mute(!muted)) {
        set_message(muted ? "Audio unmuted." : "Audio muted.");
    } else {
        set_message("HDA mute control is unavailable.");
    }
    book8gui_invalidate();
}

static void create_bookmark(const struct book8gui_platform *p)
{
    if (player.path[0] == '\0' || !player.loaded) {
        set_message("Load a WAV file before creating a bookmark.");
        book8gui_invalidate();
        return;
    }
    struct book8_media_bookmark_set saved;
    if (!book8_media_bookmarks_load(p, player.path, &saved)) {
        memset(&saved, 0, sizeof(saved));
        snprintf(saved.path, sizeof(saved.path), "%s", player.path);
    }
    if (saved.count >= BOOK8_MEDIA_BOOKMARK_MAX_PER_MEDIA) {
        set_message("Bookmark list is full.");
        player.bookmarks = saved;
        book8gui_invalidate();
        return;
    }
    struct book8_media_bookmark *bookmark = &saved.bookmarks[saved.count];
    bookmark->position_ms = player.position_ms;
    char current[12u], total[12u];
    format_time(player.position_ms, current, sizeof(current));
    format_time(player.duration_ms, total, sizeof(total));
    snprintf(bookmark->label, sizeof(bookmark->label), "%.9s/%.9s", current,
        total);
    ++saved.count;
    if (!book8_media_bookmarks_save(p, player.path, &saved)) {
        set_message("Bookmark could not be saved to A:\\MEDIA.BMK.");
        book8gui_invalidate();
        return;
    }
    player.bookmarks = saved;
    set_message("Bookmark saved to A:\\MEDIA.BMK.");
    book8gui_invalidate();
}

static void commit_preferences(const struct book8gui_platform *p, bool save)
{
    if (save && !book8_media_prefs_save(p, &player.prefs)) {
        set_message("Preferences could not be saved.");
        book8gui_invalidate();
        return;
    }
    if (!save) {
        (void)book8_media_prefs_save(p, &player.prefs_saved);
    }
    apply_preferences(p);
    if (p->audio_set_volume != NULL && player.volume != 0u) {
        (void)p->audio_set_volume(player.volume);
    }
    player.view = PLAYER_VIEW_NOW_PLAYING;
    set_message(save ? "Preferences saved." : "Preferences discarded.");
    book8gui_invalidate();
}

static void execute_command(const struct book8gui_platform *p,
    struct WindowApp *app, uint8_t command)
{
    switch (command) {
    case CMD_OPEN_FILE:
        player.dialog = PLAYER_DIALOG_OPEN;
        player.open_tab = PLAYER_OPEN_FILE;
        player.path_focused = true;
        player.url_focused = false;
        break;
    case CMD_OPEN_NETWORK:
        player.dialog = PLAYER_DIALOG_OPEN;
        player.open_tab = PLAYER_OPEN_NETWORK;
        player.path_focused = false;
        player.url_focused = true;
        break;
    case CMD_OPEN_FOLDER:
        open_playlist_directory(p);
        break;
    case CMD_QUIT:
        book8gui_request_close(app);
        break;
    case CMD_PLAY_PAUSE:
        if (player.playing && !player.paused) {
            pause_playback(p);
        } else {
            play_or_resume(p, app);
        }
        break;
    case CMD_STOP:
        stop_playback(p);
        break;
    case CMD_PREV:
        playlist_step(p, app, -1);
        break;
    case CMD_NEXT:
        playlist_step(p, app, 1);
        break;
    case CMD_GOTO_TIME:
        player.goto_field[0] = '\0';
        player.dialog = PLAYER_DIALOG_GOTO;
        break;
    case CMD_BOOKMARK_CREATE:
        create_bookmark(p);
        break;
    case CMD_BOOKMARKS:
        player.dialog = PLAYER_DIALOG_BOOKMARKS;
        break;
    case CMD_SPEED_SLOWER:
        if (player.speed_index != 0u) {
            --player.speed_index;
        }
        set_message("Playback speed marker adjusted; HDA plays at source rate.");
        break;
    case CMD_SPEED_NORMAL:
        player.speed_index = 2u;
        set_message("Playback speed marker reset to 100%.");
        break;
    case CMD_SPEED_FASTER:
        if (player.speed_index + 1u < sizeof(speeds) / sizeof(speeds[0])) {
            ++player.speed_index;
        }
        set_message("Playback speed marker adjusted; HDA plays at source rate.");
        break;
    case CMD_AUDIO_STEREO:
        set_message("Stereo output selected.");
        break;
    case CMD_AUDIO_MONO:
        set_message("Mono mixing is unavailable; stereo output retained.");
        break;
    case CMD_MUTE:
        toggle_mute(p);
        break;
    case CMD_REPEAT_NONE:
        player.prefs.repeat = BOOK8_MEDIA_REPEAT_NONE;
        player.repeat_counter = 0u;
        (void)book8_media_prefs_save(p, &player.prefs);
        set_message("Repeat: off.");
        break;
    case CMD_REPEAT_ALL:
        player.prefs.repeat = BOOK8_MEDIA_REPEAT_ALL;
        player.repeat_counter = 0u;
        (void)book8_media_prefs_save(p, &player.prefs);
        set_message("Repeat: all tracks.");
        break;
    case CMD_REPEAT_ONE:
        player.prefs.repeat = BOOK8_MEDIA_REPEAT_ONE;
        player.repeat_counter = 0u;
        (void)book8_media_prefs_save(p, &player.prefs);
        set_message("Repeat: one track.");
        break;
    case CMD_VISUALIZATION:
        player.prefs.plugins ^= BOOK8_MEDIA_PLUGIN_VISUALIZATION;
        (void)book8_media_prefs_save(p, &player.prefs);
        set_message((player.prefs.plugins & BOOK8_MEDIA_PLUGIN_VISUALIZATION) != 0u
            ? "Audio visualization enabled." : "Audio visualization disabled.");
        break;
    case CMD_MAX_VOLUME:
        player.prefs_section = 1u;
        player.prefs_saved = *book8_media_prefs();
        player.prefs = player.prefs_saved;
        player.view = PLAYER_VIEW_PREFERENCES;
        break;
    case CMD_VIDEO_INFO:
        set_message("Video decoding is unavailable in BOOK8 Player.");
        break;
    case CMD_FULLSCREEN:
        set_message("Fullscreen video requires a video decoder.");
        break;
    case CMD_SUBTITLE_INFO:
        set_message("Subtitle tracks are unavailable in BOOK8 Player.");
        break;
    case CMD_PREFERENCES:
        player.prefs_saved = *book8_media_prefs();
        player.prefs = player.prefs_saved;
        player.prefs_section = 0u;
        player.view = PLAYER_VIEW_PREFERENCES;
        player.dialog = PLAYER_DIALOG_NONE;
        break;
    case CMD_CODEC_INFO:
        set_message("Codec: PCM 48 kHz stereo 16-bit WAV (built-in plugin).");
        break;
    case CMD_RESET_PREFS:
        book8_media_prefs_defaults(&player.prefs);
        (void)book8_media_prefs_save(p, &player.prefs);
        apply_preferences(p);
        set_message("Preferences reset to defaults.");
        break;
    case CMD_VIEW_PLAYLIST:
        player.view = PLAYER_VIEW_PLAYLIST;
        break;
    case CMD_VIEW_NOW:
        player.view = PLAYER_VIEW_NOW_PLAYING;
        break;
    case CMD_STATUS_BAR:
        player.prefs.interface_flags ^= BOOK8_MEDIA_IF_STATUS;
        (void)book8_media_prefs_save(p, &player.prefs);
        apply_preferences(p);
        break;
    case CMD_ABOUT:
        player.view = PLAYER_VIEW_ABOUT;
        break;
    default:
        break;
    }
    book8gui_invalidate();
}

static void draw_cone(const struct book8gui_platform *p,
    const struct book8gui_rect *canvas)
{
    uint32_t center_x = canvas->x + canvas->width / 2u;
    uint32_t center_y = canvas->y + (canvas->height > 68u
        ? (canvas->height - 68u) / 2u : 0u);
    for (uint32_t row = 0u; row < 10u; ++row) {
        uint32_t width = 4u + row * 4u;
        uint32_t y = center_y + row * 4u;
        if (y >= canvas->y + canvas->height) {
            break;
        }
        p->fill_rect(center_x - width / 2u, y, width, 4u,
            row % 2u == 0u ? PL_ORANGE : PL_ORANGE_DARK);
    }
    p->fill_rect(center_x - 30u, center_y + 42u, 60u, 5u, PL_ORANGE_DARK);
    p->fill_rect(center_x - 22u, center_y + 47u, 44u, 3u, PL_BORDER);
}

/* Q15 twiddles for a 32-point radix-2 FFT: round(32767 * cos/sin(2*pi*k/32)).
 * All arithmetic below is integer; no libm, double or SIMD is required. */
static const int16_t player_fft_cos[PLAYER_FFT_BINS] = {
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
    0, -6393, -12539, -18204, -23170, -27245, -30273, -32137
};
static const int16_t player_fft_sin[PLAYER_FFT_BINS] = {
    0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393
};

static int32_t player_abs32(int32_t value)
{
    return value < 0 ? -value : value;
}

static int16_t player_read_pcm_sample(const uint8_t *bytes)
{
    uint16_t value = (uint16_t)bytes[0u] | (uint16_t)((uint16_t)bytes[1u] << 8u);
    return (int16_t)value;
}

static void player_pcm_fft(const uint8_t *pcm, uint32_t size,
    uint64_t position_bytes, uint32_t magnitudes[PLAYER_FFT_BINS])
{
    int32_t real[PLAYER_FFT_POINTS] = {0};
    int32_t imaginary[PLAYER_FFT_POINTS] = {0};
    uint32_t start = 0u;

    if (pcm != NULL && size >= PLAYER_FFT_PCM_BYTES) {
        uint64_t requested = position_bytes & ~3ull;
        if (requested > size) {
            requested = size;
        }
        start = (uint32_t)requested;
        if (start > size - PLAYER_FFT_PCM_BYTES) {
            start = size - PLAYER_FFT_PCM_BYTES;
        }
        start &= ~3u;
    }
    /* DIT butterflies require bit-reversed input and yield natural-order bins. */
    for (uint32_t i = 0u; i < PLAYER_FFT_POINTS; ++i) {
        uint32_t offset = start + i * 4u;
        if (pcm != NULL && offset + 4u <= size) {
            int32_t left = player_read_pcm_sample(pcm + offset);
            int32_t right = player_read_pcm_sample(pcm + offset + 2u);
            uint32_t reversed = ((i & 1u) << 4u) | ((i & 2u) << 2u)
                | (i & 4u) | ((i & 8u) >> 2u) | ((i & 16u) >> 4u);
            real[reversed] = (left + right) / 2;
        }
    }

    for (uint32_t length = 2u; length <= PLAYER_FFT_POINTS; length <<= 1u) {
        uint32_t half = length >> 1u;
        uint32_t step = PLAYER_FFT_POINTS / length;
        for (uint32_t base = 0u; base < PLAYER_FFT_POINTS; base += length) {
            for (uint32_t j = 0u; j < half; ++j) {
                uint32_t twiddle = (j * step) & (PLAYER_FFT_POINTS - 1u);
                int64_t wr = player_fft_cos[twiddle];
                int64_t wi = player_fft_sin[twiddle];
                uint32_t even = base + j;
                uint32_t odd = even + half;
                int32_t tr = (int32_t)((wr * real[odd]
                    - wi * imaginary[odd]) >> 15u);
                int32_t ti = (int32_t)((wi * real[odd]
                    + wr * imaginary[odd]) >> 15u);
                int32_t er = real[even], ei = imaginary[even];
                real[even] = er + tr;
                imaginary[even] = ei + ti;
                real[odd] = er - tr;
                imaginary[odd] = ei - ti;
            }
        }
    }

    for (uint32_t i = 0u; i < PLAYER_FFT_BINS; ++i) {
        int32_t re = player_abs32(real[i]);
        int32_t im = player_abs32(imaginary[i]);
        magnitudes[i] = (uint32_t)(re + im / 2);
    }
}

static void draw_visualization(const struct book8gui_platform *p,
    const struct book8gui_rect *canvas)
{
    if ((player.prefs.plugins & BOOK8_MEDIA_PLUGIN_VISUALIZATION) == 0u
        || !player.playing || player.paused || canvas->height < 24u) {
        return;
    }
    const uint8_t *pcm = NULL;
    uint32_t pcm_size = 0u;
    uint64_t position_bytes = 0u;
    uint32_t magnitudes[PLAYER_FFT_BINS] = {0};
    uint32_t maximum = 0u;
    if (p->media_pcm_window != NULL
        && p->media_pcm_window(&pcm, &pcm_size, &position_bytes)) {
        player_pcm_fft(pcm, pcm_size, position_bytes, magnitudes);
        for (uint32_t i = 0u; i < PLAYER_FFT_BINS; ++i) {
            if (magnitudes[i] > maximum) {
                maximum = magnitudes[i];
            }
        }
    }
    uint32_t columns = canvas->width / 12u;
    if (columns > 32u) {
        columns = 32u;
    }
    uint32_t maximum_height = canvas->height / 2u;
    uint32_t base_y = canvas->y + canvas->height - 4u;
    for (uint32_t i = 0u; i < columns; ++i) {
        uint32_t height = 3u;
        if (maximum != 0u && maximum_height > 3u) {
            uint64_t scaled = (uint64_t)magnitudes[i % PLAYER_FFT_BINS]
                * (maximum_height - 3u);
            height = 3u + (uint32_t)(scaled / maximum);
        }
        if (height > maximum_height) {
            height = maximum_height;
        }
        p->fill_rect(canvas->x + 6u + i * 12u, base_y - height, 8u, height,
            PL_GREEN);
    }
}

static void render_now_playing(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    const struct book8gui_rect *canvas = &layout->canvas;
    p->fill_rect(canvas->x, canvas->y, canvas->width, canvas->height,
        PL_CANVAS);
    p->fill_rect(canvas->x, canvas->y, canvas->width, 1u, PL_BORDER);
    p->fill_rect(canvas->x, canvas->y + canvas->height - 1u, canvas->width,
        1u, PL_BORDER);
    draw_cone(p, canvas);

    if (player.loaded) {
        draw_text_clipped(p, canvas->x + 10u, canvas->y + 8u,
            canvas->width - 20u, player.path, PL_TEXT, PL_CANVAS);
        char status[48u];
        format_time(player.position_ms, status, sizeof(status));
        strncat(status, " / ", sizeof(status) - strlen(status) - 1u);
        char total[16u];
        format_time(player.duration_ms, total, sizeof(total));
        strncat(status, total, sizeof(status) - strlen(status) - 1u);
        snprintf(total, sizeof(total), "  %u%%", speeds[player.speed_index]);
        strncat(status, total, sizeof(status) - strlen(status) - 1u);
        p->draw_text(canvas->x + 10u, canvas->y + 22u, status,
            PL_MUTED, PL_CANVAS);
    } else {
        p->draw_text(canvas->x + 10u, canvas->y + 8u,
            "BOOK8 Player", PL_TEXT, PL_CANVAS);
        p->draw_text(canvas->x + 10u, canvas->y + 22u,
            "Media  |  Playback  |  Web", PL_MUTED, PL_CANVAS);
    }
    if (player.failed) {
        p->draw_text(canvas->x + 10u, canvas->y + canvas->height - 20u,
            "Playback error", PL_RED, PL_CANVAS);
    }
    draw_visualization(p, canvas);

    p->fill_rect(layout->toolbar.x, layout->toolbar.y,
        layout->toolbar.width, layout->toolbar.height, PL_PANEL);
    draw_button(p, layout->prev, "|<", false);
    draw_button(p, layout->play, ">", player.playing && !player.paused);
    draw_button(p, layout->pause, "||", player.paused);
    draw_button(p, layout->stop, "[]", false);
    draw_button(p, layout->next, ">|", false);
    draw_button(p, layout->playlist, "PL",
        player.view == PLAYER_VIEW_PLAYLIST);
    draw_button(p, layout->prefs, "Prefs", false);

    char elapsed[16u], total[16u], time_text[40u];
    format_time(player.position_ms, elapsed, sizeof(elapsed));
    format_time(player.duration_ms, total, sizeof(total));
    snprintf(time_text, sizeof(time_text), "%s / %s", elapsed, total);
    p->draw_text(layout->client.x + 6u, layout->time_y, time_text,
        PL_TEXT, PL_BACKGROUND);

    const struct book8gui_rect *seek = &layout->seek;
    p->fill_rect(seek->x, seek->y, seek->width, seek->height, 0x000000u);
    p->fill_rect(seek->x, seek->y, seek->width, 1u, PL_BORDER);
    p->fill_rect(seek->x, seek->y + seek->height - 1u, seek->width, 1u,
        PL_BORDER);
    if (player.duration_ms != 0u && seek->width > 2u) {
        uint32_t progress = (uint32_t)(((uint64_t)(seek->width - 2u)
            * player.position_ms) / player.duration_ms);
        if (progress > seek->width - 2u) {
            progress = seek->width - 2u;
        }
        p->fill_rect(seek->x + 1u, seek->y + 1u, progress,
            seek->height - 2u, PL_ORANGE);
    }

    draw_button(p, layout->mute, "M", false);
    p->fill_rect(layout->volume_track.x, layout->volume_track.y,
        layout->volume_track.width, layout->volume_track.height, 0x000000u);
    p->fill_rect(layout->volume_track.x, layout->volume_track.y,
        layout->volume_track.width, 1u, PL_BORDER);
    if (layout->volume_track.width > 2u) {
        uint32_t fill = (uint32_t)(((uint64_t)(layout->volume_track.width - 2u)
            * player.volume) / 100u);
        p->fill_rect(layout->volume_track.x + 1u,
            layout->volume_track.y + 1u, fill,
            layout->volume_track.height - 2u, PL_GREEN);
    }
    char volume_text[16u];
    snprintf(volume_text, sizeof(volume_text), "%u%%", player.volume);
    draw_text_centered(p, layout->volume_track, volume_text, PL_TEXT,
        0x000000u);
    const char *repeat_text = player.prefs.repeat == BOOK8_MEDIA_REPEAT_ONE
        ? "R1" : player.prefs.repeat == BOOK8_MEDIA_REPEAT_ALL ? "RA" : "R";
    draw_button(p, layout->repeat, repeat_text,
        player.prefs.repeat != BOOK8_MEDIA_REPEAT_NONE);
    draw_button(p, layout->bookmark, "Book", false);
    draw_button(p, layout->speed, "Speed", false);
}

static void render_menu(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    p->fill_rect(layout->menu.x, layout->menu.y, layout->menu.width,
        layout->menu.height, PL_PANEL);
    for (uint32_t i = 0u; i < PLAYER_MENU_COUNT; ++i) {
        struct book8gui_rect rect = menu_rect(layout, i);
        bool active = player.menu != 0xffu && player.menu == i;
        if (active) {
            p->fill_rect(rect.x, rect.y, rect.width, rect.height, PL_ORANGE);
        }
        p->draw_text(rect.x + 4u, rect.y + 2u, menu_labels[i],
            active ? PL_SELECT_TEXT : PL_TEXT,
            active ? PL_ORANGE : PL_PANEL);
    }
    if (player.menu == 0xffu) {
        return;
    }
    struct book8gui_rect dropdown = menu_dropdown(layout, player.menu);
    p->fill_rect(dropdown.x, dropdown.y, dropdown.width, dropdown.height,
        PL_PANEL);
    p->fill_rect(dropdown.x, dropdown.y, dropdown.width, 1u, PL_ORANGE);
    p->fill_rect(dropdown.x, dropdown.y + dropdown.height - 1u,
        dropdown.width, 1u, PL_BORDER);
    uint32_t y = dropdown.y + 3u;
    for (uint32_t i = 0u; i < PLAYER_MENU_ITEM_LIMIT; ++i) {
        const struct menu_entry *item = &menu_items[player.menu][i];
        if (item->label == NULL) {
            break;
        }
        if (item->command == CMD_SEPARATOR) {
            p->fill_rect(dropdown.x + 6u, y + 8u, dropdown.width - 12u, 1u,
                PL_BORDER);
            y += 18u;
            continue;
        }
        p->draw_text(dropdown.x + 8u, y + 2u, item->label, PL_TEXT, PL_PANEL);
        if (item->shortcut != NULL && item->shortcut[0] != '\0') {
            uint32_t width = text_width(item->shortcut);
            p->draw_text(dropdown.x + dropdown.width - width - 8u, y + 2u,
                item->shortcut, PL_MUTED, PL_PANEL);
        }
        y += 18u;
    }
}

static void draw_toggle_row(const struct book8gui_platform *p,
    struct book8gui_rect rect, const char *label, bool enabled)
{
    p->fill_rect(rect.x, rect.y, rect.width, rect.height, PL_BUTTON);
    p->fill_rect(rect.x + 4u, rect.y + 4u, 12u, 12u, PL_FIELD);
    if (enabled) {
        p->fill_rect(rect.x + 6u, rect.y + 6u, 8u, 8u, PL_ORANGE);
    }
    draw_text_clipped(p, rect.x + 24u, rect.y + 4u,
        rect.width > 28u ? rect.width - 28u : 0u, label,
        enabled ? PL_TEXT : PL_MUTED, PL_BUTTON);
}

static void draw_readonly_row(const struct book8gui_platform *p,
    struct book8gui_rect rect, const char *label, const char *value)
{
    p->fill_rect(rect.x, rect.y, rect.width, rect.height, PL_PANEL);
    draw_text_clipped(p, rect.x + 4u, rect.y + 4u,
        rect.width > 4u ? rect.width - 4u : 0u, label, PL_MUTED, PL_PANEL);
    if (value != NULL) {
        uint32_t width = text_width(value);
        draw_text_clipped(p, rect.x + rect.width - width - 4u,
            rect.y + 4u, width, value, PL_TEXT, PL_PANEL);
    }
}

static void render_preferences(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect panel = prefs_panel(layout);
    p->fill_rect(panel.x, panel.y, panel.width, panel.height, PL_PANEL);
    p->fill_rect(panel.x, panel.y, panel.width, 1u, PL_ORANGE);
    p->draw_text(panel.x + 8u, panel.y + 7u, "Tools / Preferences",
        PL_TEXT, PL_PANEL);

    for (uint32_t i = 0u; i < PLAYER_PREFS_SECTION_COUNT; ++i) {
        struct book8gui_rect nav = prefs_nav_rect(layout, i);
        bool active = i == player.prefs_section;
        p->fill_rect(nav.x, nav.y, nav.width, nav.height,
            active ? PL_ORANGE : PL_BUTTON);
        p->draw_text(nav.x + 4u, nav.y + 4u, prefs_sections[i],
            active ? PL_SELECT_TEXT : PL_TEXT,
            active ? PL_ORANGE : PL_BUTTON);
    }

    char value[32u];
    switch (player.prefs_section) {
    case 0u:
        draw_toggle_row(p, prefs_row_rect(layout, 0u), "Show status bar",
            (player.prefs.interface_flags & BOOK8_MEDIA_IF_STATUS) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 1u),
            "Show playlist on startup",
            (player.prefs.interface_flags & BOOK8_MEDIA_IF_PLAYLIST) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 2u), "Dark canvas",
            (player.prefs.interface_flags & BOOK8_MEDIA_IF_DARK_CANVAS) != 0u);
        draw_readonly_row(p, prefs_row_rect(layout, 3u), "Interface",
            "Qt-style desktop");
        break;
    case 1u: {
        struct book8gui_rect row = prefs_row_rect(layout, 0u);
        p->fill_rect(row.x, row.y, row.width, row.height, PL_BUTTON);
        draw_text_clipped(p, row.x + 4u, row.y + 4u, 150u,
            "Maximum volume", PL_TEXT, PL_BUTTON);
        struct book8gui_rect track = {row.x + row.width - 160u, row.y + 6u,
            120u, 8u};
        p->fill_rect(track.x, track.y, track.width, track.height, 0x000000u);
        uint32_t fill = (uint32_t)(((uint64_t)(track.width - 2u)
            * player.prefs.max_volume) / 100u);
        p->fill_rect(track.x + 1u, track.y + 1u, fill, track.height - 2u,
            PL_GREEN);
        snprintf(value, sizeof(value), "%u%%", player.prefs.max_volume);
        p->draw_text(track.x + track.width + 6u, row.y + 4u, value,
            PL_TEXT, PL_BUTTON);
        draw_readonly_row(p, prefs_row_rect(layout, 1u), "Audio output",
            "HDA stereo");
        draw_readonly_row(p, prefs_row_rect(layout, 2u), "Decoded format",
            "48 kHz S16");
        break;
    }
    case 2u:
        draw_toggle_row(p, prefs_row_rect(layout, 0u), "PCM WAV decoder",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_PCM_WAV) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 1u), "HTTP WAV stream",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_HTTP_WAV) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 2u), "Playlist browser",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_PLAYLIST) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 3u), "Audio visualization",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_VISUALIZATION) != 0u);
        draw_readonly_row(p, prefs_row_rect(layout, 4u), "MP3 / video codecs",
            "unavailable");
        break;
    case 3u:
        draw_toggle_row(p, prefs_row_rect(layout, 0u),
            "PCM WAV playback plugin",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_PCM_WAV) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 1u),
            "HTTP stream plugin",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_HTTP_WAV) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 2u),
            "Playlist plugin",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_PLAYLIST) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 3u),
            "Visualization plugin",
            (player.prefs.plugins & BOOK8_MEDIA_PLUGIN_VISUALIZATION) != 0u);
        draw_readonly_row(p, prefs_row_rect(layout, 4u), "Plugin cache",
            "verified");
        break;
    case 4u:
        draw_toggle_row(p, prefs_row_rect(layout, 0u),
            "Auto-play HTTP WAV responses",
            (player.prefs.web & BOOK8_MEDIA_WEB_AUTOPLAY) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 1u),
            "Show Page Links media sources",
            (player.prefs.web & BOOK8_MEDIA_WEB_MEDIA_PANEL) != 0u);
        draw_toggle_row(p, prefs_row_rect(layout, 2u),
            "Open WAV links in Player",
            (player.prefs.web & BOOK8_MEDIA_WEB_OPEN_IN_PLAYER) != 0u);
        draw_readonly_row(p, prefs_row_rect(layout, 3u), "Web transport",
            "HTTP only");
        break;
    default:
        draw_readonly_row(p, prefs_row_rect(layout, 0u), "Ctrl+O",
            "Open file");
        draw_readonly_row(p, prefs_row_rect(layout, 1u), "Ctrl+N",
            "Network stream");
        draw_readonly_row(p, prefs_row_rect(layout, 2u), "Ctrl+L",
            "Playlist");
        draw_readonly_row(p, prefs_row_rect(layout, 3u), "Space",
            "Play / Pause");
        draw_readonly_row(p, prefs_row_rect(layout, 4u), "Ctrl+T",
            "Jump to time");
        break;
    }

    draw_button(p, prefs_save_rect(layout), "Save", false);
    draw_button(p, prefs_cancel_rect(layout), "Cancel", false);
    draw_button(p, prefs_reset_rect(layout), "Reset", false);
}

static void render_playlist(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect panel = playlist_panel(layout);
    p->fill_rect(panel.x, panel.y, panel.width, panel.height, PL_PANEL);
    p->fill_rect(panel.x, panel.y, panel.width, 1u, PL_ORANGE);
    p->draw_text(panel.x + 8u, panel.y + 7u, "Playlist", PL_TEXT, PL_PANEL);
    draw_button(p, playlist_close_rect(layout), "Close", false);

    if (player.playlist_count == 0u) {
        p->draw_text(panel.x + 10u, panel.y + 36u,
            "Use Media > Open Folder / Playlist to add WAV files.",
            PL_MUTED, PL_PANEL);
        return;
    }
    uint32_t visible = (panel.height - 44u) / 18u;
    for (uint32_t i = 0u; i < player.playlist_count && i < visible; ++i) {
        struct book8gui_rect row = playlist_row_rect(layout, i);
        bool active = i == player.playlist_index;
        p->fill_rect(row.x, row.y, row.width, row.height,
            active ? PL_ORANGE : PL_BUTTON);
        char index[12u];
        snprintf(index, sizeof(index), "%u.", i + 1u);
        p->draw_text(row.x + 4u, row.y + 3u, index,
            active ? PL_SELECT_TEXT : PL_MUTED,
            active ? PL_ORANGE : PL_BUTTON);
        draw_text_clipped(p, row.x + 30u, row.y + 3u, row.width - 34u,
            player.playlist[i], active ? PL_SELECT_TEXT : PL_TEXT,
            active ? PL_ORANGE : PL_BUTTON);
    }
}

static void render_open_dialog(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect dialog = open_dialog_rect(layout);
    p->fill_rect(dialog.x, dialog.y, dialog.width, dialog.height, PL_PANEL);
    p->fill_rect(dialog.x, dialog.y, dialog.width, 1u, PL_ORANGE);
    p->fill_rect(dialog.x, dialog.y + dialog.height - 1u, dialog.width, 1u,
        PL_BORDER);
    p->draw_text(dialog.x + 10u, dialog.y + 7u, "Open Media",
        PL_TEXT, PL_PANEL);

    bool file_tab = player.open_tab == PLAYER_OPEN_FILE;
    struct book8gui_rect file = open_tab_rect(layout, 0u);
    struct book8gui_rect network = open_tab_rect(layout, 1u);
    p->fill_rect(file.x, file.y, file.width, file.height,
        file_tab ? PL_ORANGE : PL_BUTTON);
    p->fill_rect(network.x, network.y, network.width, network.height,
        file_tab ? PL_BUTTON : PL_ORANGE);
    p->draw_text(file.x + 8u, file.y + 4u, "File",
        file_tab ? PL_SELECT_TEXT : PL_TEXT,
        file_tab ? PL_ORANGE : PL_BUTTON);
    p->draw_text(network.x + 8u, network.y + 4u, "Network Stream",
        file_tab ? PL_TEXT : PL_SELECT_TEXT,
        file_tab ? PL_BUTTON : PL_ORANGE);

    draw_field(p, open_field_rect(layout),
        file_tab ? player.path : player.url,
        file_tab ? player.path_focused : player.url_focused);
    p->draw_text(dialog.x + 10u, dialog.y + 82u + 6u,
        file_tab ? "Local PCM WAV file" : "HTTP WAV URL / web page",
        PL_MUTED, PL_PANEL);
    draw_button(p, open_play_rect(layout), "Play", false);
    draw_button(p, open_cancel_rect(layout), "Cancel", false);
}

static void render_goto_dialog(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    p->fill_rect(dialog.x, dialog.y, dialog.width, dialog.height, PL_PANEL);
    p->fill_rect(dialog.x, dialog.y, dialog.width, 1u, PL_ORANGE);
    p->draw_text(dialog.x + 12u, dialog.y + 10u, "Go to Time",
        PL_TEXT, PL_PANEL);
    draw_field(p, goto_field_rect(layout), player.goto_field, true);
    p->draw_text(dialog.x + 12u, dialog.y + 66u, "hh:mm:ss",
        PL_MUTED, PL_PANEL);
    draw_button(p, goto_ok_rect(layout), "Go", false);
    draw_button(p, goto_cancel_rect(layout), "Cancel", false);
}

static void render_confirm_stream_dialog(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    p->fill_rect(dialog.x, dialog.y, dialog.width, dialog.height, PL_PANEL);
    p->fill_rect(dialog.x, dialog.y, dialog.width, 1u, PL_ORANGE);
    p->fill_rect(dialog.x, dialog.y + dialog.height - 1u, dialog.width, 1u,
        PL_BORDER);
    p->draw_text(dialog.x + 12u, dialog.y + 8u, "Open Network Stream",
        PL_TEXT, PL_PANEL);
    p->draw_text(dialog.x + 12u, dialog.y + 28u,
        "This opens the URL in Web Browser.", PL_MUTED, PL_PANEL);
    draw_text_clipped(p, dialog.x + 12u, dialog.y + 46u,
        dialog.width > 24u ? dialog.width - 24u : 0u, player.url,
        PL_TEXT, PL_PANEL);
    draw_button(p, confirm_open_rect(layout), "Open", false);
    draw_button(p, confirm_cancel_rect(layout), "Cancel", false);
}

static void render_bookmarks_dialog(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    p->fill_rect(dialog.x, dialog.y, dialog.width, dialog.height, PL_PANEL);
    p->fill_rect(dialog.x, dialog.y, dialog.width, 1u, PL_ORANGE);
    p->draw_text(dialog.x + 12u, dialog.y + 7u, "Custom Bookmarks",
        PL_TEXT, PL_PANEL);
    if (player.bookmarks.count == 0u) {
        p->draw_text(dialog.x + 12u, dialog.y + 30u,
            "No bookmarks saved for this file.", PL_MUTED, PL_PANEL);
    }
    for (uint32_t i = 0u; i < player.bookmarks.count && i < 2u; ++i) {
        struct book8gui_rect row = {dialog.x + 12u, dialog.y + 26u + i * 18u,
            dialog.width - 24u, 18u};
        p->fill_rect(row.x, row.y, row.width, row.height, PL_BUTTON);
        p->draw_text(row.x + 4u, row.y + 3u,
            player.bookmarks.bookmarks[i].label, PL_TEXT, PL_BUTTON);
    }
    draw_button(p, goto_cancel_rect(layout), "Close", false);
}

static void render_about(const struct book8gui_platform *p,
    const struct player_layout *layout)
{
    struct book8gui_rect panel = playlist_panel(layout);
    p->fill_rect(panel.x, panel.y, panel.width, panel.height, PL_PANEL);
    p->fill_rect(panel.x, panel.y, panel.width, 1u, PL_ORANGE);
    p->draw_text(panel.x + 16u, panel.y + 16u,
        "BOOK8 Player - VLC-style media controls", PL_TEXT, PL_PANEL);
    p->draw_text(panel.x + 16u, panel.y + 36u,
        "Local and HTTP PCM WAV playback through HDA.", PL_MUTED, PL_PANEL);
    p->draw_text(panel.x + 16u, panel.y + 52u,
        "Media plugins and web playback are configured in Preferences.",
        PL_MUTED, PL_PANEL);
    draw_button(p, playlist_close_rect(layout), "Close", false);
}

static void render(const struct book8gui_platform *p, struct WindowApp *app)
{
    struct player_layout layout = player_layout(app);
    if (layout.client.width < 340u || layout.client.height < 190u
        || layout.canvas.height < 20u) {
        return;
    }
    p->fill_rect(layout.client.x, layout.client.y, layout.client.width,
        layout.client.height, PL_BACKGROUND);
    render_menu(p, &layout);
    switch (player.view) {
    case PLAYER_VIEW_PLAYLIST:
        render_now_playing(p, &layout);
        render_playlist(p, &layout);
        break;
    case PLAYER_VIEW_PREFERENCES:
        render_now_playing(p, &layout);
        render_preferences(p, &layout);
        break;
    case PLAYER_VIEW_ABOUT:
        render_now_playing(p, &layout);
        render_about(p, &layout);
        break;
    default:
        render_now_playing(p, &layout);
        break;
    }
    if (player.dialog == PLAYER_DIALOG_OPEN) {
        render_open_dialog(p, &layout);
    } else if (player.dialog == PLAYER_DIALOG_GOTO) {
        render_goto_dialog(p, &layout);
    } else if (player.dialog == PLAYER_DIALOG_BOOKMARKS) {
        render_bookmarks_dialog(p, &layout);
    } else if (player.dialog == PLAYER_DIALOG_CONFIRM_STREAM) {
        render_confirm_stream_dialog(p, &layout);
    }
    if ((player.prefs.interface_flags & BOOK8_MEDIA_IF_STATUS) != 0u
        && player.dialog == PLAYER_DIALOG_NONE
        && player.view == PLAYER_VIEW_NOW_PLAYING) {
        p->fill_rect(layout.status.x, layout.status.y, layout.status.width,
            layout.status.height, PL_PANEL);
        draw_text_clipped(p, layout.status.x + 4u, layout.status.y + 2u,
            layout.status.width - 8u, player.message, PL_TEXT, PL_PANEL);
    }
}

static void tick(const struct book8gui_platform *p, struct WindowApp *app)
{
    (void)app;
    update_media_state(p);
    if (player.loaded && player.playing && !player.paused
        && player.duration_ms != 0u
        && player.position_ms >= player.duration_ms
        && player.prefs.repeat != BOOK8_MEDIA_REPEAT_NONE) {
        if (player.prefs.repeat == BOOK8_MEDIA_REPEAT_ONE
            || player.playlist_count <= 1u) {
            seek_to_position(p, 0u);
        } else {
            playlist_step(p, app, 1);
        }
    }
}

static bool edit_field(char *field, uint32_t capacity, uint32_t key)
{
    uint32_t character = key & BOOK8GUI_KEY_MASK;
    if ((key & BOOK8GUI_KEY_CTRL) != 0u
        && (character == 'a' || character == 'A')) {
        field[0] = '\0';
        return true;
    }
    if (character == BOOK8GUI_KEY_BACKSPACE) {
        uint32_t length = (uint32_t)strlen(field);
        if (length != 0u) {
            field[length - 1u] = '\0';
        }
        return true;
    }
    if (character >= 32u && character <= 126u
        && strlen(field) + 1u < capacity) {
        uint32_t length = (uint32_t)strlen(field);
        field[length] = (char)character;
        field[length + 1u] = '\0';
        return true;
    }
    return false;
}

static char *focused_text(uint32_t *capacity)
{
    if (player.dialog == PLAYER_DIALOG_GOTO) {
        *capacity = sizeof(player.goto_field);
        return player.goto_field;
    }
    if (player.dialog == PLAYER_DIALOG_OPEN) {
        if (player.open_tab == PLAYER_OPEN_NETWORK) {
            *capacity = sizeof(player.url);
            return player.url;
        }
        *capacity = sizeof(player.path);
        return player.path;
    }
    return NULL;
}

static void accept_network_stream(const struct book8gui_platform *p)
{
    player.dialog = PLAYER_DIALOG_NONE;
    player.url_focused = false;
    play_url(p);
}

static void open_dialog_activate(const struct book8gui_platform *p,
    struct WindowApp *app)
{
    if (player.open_tab == PLAYER_OPEN_FILE) {
        player.dialog = PLAYER_DIALOG_NONE;
        player.path_focused = false;
        (void)start_wav(p, app);
    } else if (player.url[0] == '\0') {
        set_message("Enter an HTTP stream URL.");
        book8gui_invalidate();
    } else {
        player.dialog = PLAYER_DIALOG_CONFIRM_STREAM;
        player.path_focused = false;
        player.url_focused = false;
        set_message("Confirm opening the stream in Web Browser.");
        book8gui_invalidate();
    }
}

static bool handle_key(const struct book8gui_platform *p,
    struct WindowApp *app, const struct book8gui_event *event)
{
    uint32_t key = event->key & BOOK8GUI_KEY_MASK;
    bool control = (event->key & BOOK8GUI_KEY_CTRL) != 0u;
    uint32_t capacity = 0u;
    char *field = focused_text(&capacity);

    if (field != NULL) {
        if (key == BOOK8GUI_KEY_ENTER) {
            if (player.dialog == PLAYER_DIALOG_GOTO) {
                goto_activate(p);
            } else {
                open_dialog_activate(p, app);
            }
            book8gui_invalidate();
            return true;
        }
        if (key == BOOK8GUI_KEY_ESCAPE) {
            player.dialog = PLAYER_DIALOG_NONE;
            player.path_focused = false;
            player.url_focused = false;
            book8gui_invalidate();
            return true;
        }
        if (edit_field(field, capacity, event->key)) {
            book8gui_invalidate();
            return true;
        }
        return false;
    }

    if (player.dialog == PLAYER_DIALOG_CONFIRM_STREAM) {
        if (key == BOOK8GUI_KEY_ENTER || key == 'y' || key == 'Y') {
            accept_network_stream(p);
        } else if (key == BOOK8GUI_KEY_ESCAPE || key == 'n'
            || key == 'N') {
            player.dialog = PLAYER_DIALOG_NONE;
            set_message("Network stream cancelled.");
            book8gui_invalidate();
        }
        return true;
    }
    if (player.dialog == PLAYER_DIALOG_BOOKMARKS
        || player.dialog == PLAYER_DIALOG_GOTO) {
        if (key == BOOK8GUI_KEY_ESCAPE) {
            player.dialog = PLAYER_DIALOG_NONE;
            book8gui_invalidate();
            return true;
        }
        return false;
    }
    if (player.view == PLAYER_VIEW_PREFERENCES
        || player.view == PLAYER_VIEW_ABOUT) {
        if (key == BOOK8GUI_KEY_ESCAPE) {
            player.view = PLAYER_VIEW_NOW_PLAYING;
            book8gui_invalidate();
            return true;
        }
        return false;
    }

    if (control) {
        switch (key) {
        case 'o': case 'O':
            execute_command(p, app, CMD_OPEN_FILE);
            return true;
        case 'n': case 'N':
            execute_command(p, app, CMD_OPEN_NETWORK);
            return true;
        case 'f': case 'F':
            execute_command(p, app, CMD_OPEN_FOLDER);
            return true;
        case 'p': case 'P':
            execute_command(p, app, CMD_PREFERENCES);
            return true;
        case 'l': case 'L':
            execute_command(p, app, player.view == PLAYER_VIEW_PLAYLIST
                ? CMD_VIEW_NOW : CMD_VIEW_PLAYLIST);
            return true;
        case 't': case 'T':
            execute_command(p, app, CMD_GOTO_TIME);
            return true;
        case 'b': case 'B':
            execute_command(p, app, CMD_BOOKMARK_CREATE);
            return true;
        case 'j': case 'J':
            execute_command(p, app, CMD_CODEC_INFO);
            return true;
        case 'q': case 'Q':
            execute_command(p, app, CMD_QUIT);
            return true;
        default:
            return false;
        }
    }

    switch (key) {
    case ' ':
        execute_command(p, app, CMD_PLAY_PAUSE);
        return true;
    case 's': case 'S':
        execute_command(p, app, CMD_STOP);
        return true;
    case 'p': case 'P':
        execute_command(p, app, CMD_PREV);
        return true;
    case 'n': case 'N':
        execute_command(p, app, CMD_NEXT);
        return true;
    case 'm': case 'M':
        execute_command(p, app, CMD_MUTE);
        return true;
    case 'f': case 'F':
        execute_command(p, app, CMD_FULLSCREEN);
        return true;
    case '[':
        execute_command(p, app, CMD_SPEED_SLOWER);
        return true;
    case ']':
        execute_command(p, app, CMD_SPEED_FASTER);
        return true;
    case BOOK8GUI_KEY_LEFT:
        seek_delta(p, -5000);
        return true;
    case BOOK8GUI_KEY_RIGHT:
        seek_delta(p, 5000);
        return true;
    case BOOK8GUI_KEY_ESCAPE:
        player.menu = 0xffu;
        player.path_focused = false;
        player.url_focused = false;
        book8gui_invalidate();
        return true;
    default:
        return false;
    }
}

static bool handle_menu_click(const struct book8gui_platform *p,
    struct WindowApp *app, const struct player_layout *layout,
    const struct book8gui_event *event)
{
    if (player.menu == 0xffu) {
        for (uint32_t i = 0u; i < PLAYER_MENU_COUNT; ++i) {
            struct book8gui_rect rect = menu_rect(layout, i);
            if (inside(rect, event->mouse_x, event->mouse_y)) {
                player.menu = (uint8_t)i;
                book8gui_invalidate();
                return true;
            }
        }
        return false;
    }

    struct book8gui_rect dropdown = menu_dropdown(layout, player.menu);
    if (inside(dropdown, event->mouse_x, event->mouse_y)) {
        uint32_t row = (uint32_t)(event->mouse_y - dropdown.y - 3u) / 18u;
        if (row < PLAYER_MENU_ITEM_LIMIT) {
            const struct menu_entry *item = &menu_items[player.menu][row];
            if (item->label != NULL && item->command != CMD_SEPARATOR) {
                player.menu = 0xffu;
                execute_command(p, app, item->command);
                return true;
            }
        }
    }
    bool clicked_menu_bar = false;
    for (uint32_t i = 0u; i < PLAYER_MENU_COUNT; ++i) {
        struct book8gui_rect rect = menu_rect(layout, i);
        if (inside(rect, event->mouse_x, event->mouse_y)) {
            clicked_menu_bar = true;
            break;
        }
    }
    player.menu = clicked_menu_bar ? player.menu : 0xffu;
    book8gui_invalidate();
    return true;
}

static bool handle_open_dialog_click(const struct book8gui_platform *p,
    struct WindowApp *app, const struct player_layout *layout,
    const struct book8gui_event *event)
{
    struct book8gui_rect file = open_tab_rect(layout, 0u);
    struct book8gui_rect network = open_tab_rect(layout, 1u);
    struct book8gui_rect field = open_field_rect(layout);
    struct book8gui_rect play = open_play_rect(layout);
    struct book8gui_rect cancel = open_cancel_rect(layout);
    if (inside(file, event->mouse_x, event->mouse_y)) {
        player.open_tab = PLAYER_OPEN_FILE;
        player.path_focused = true;
        player.url_focused = false;
    } else if (inside(network, event->mouse_x, event->mouse_y)) {
        player.open_tab = PLAYER_OPEN_NETWORK;
        player.path_focused = false;
        player.url_focused = true;
    } else if (inside(field, event->mouse_x, event->mouse_y)) {
        player.path_focused = player.open_tab == PLAYER_OPEN_FILE;
        player.url_focused = player.open_tab == PLAYER_OPEN_NETWORK;
    } else if (inside(play, event->mouse_x, event->mouse_y)) {
        open_dialog_activate(p, app);
    } else if (inside(cancel, event->mouse_x, event->mouse_y)) {
        player.dialog = PLAYER_DIALOG_NONE;
        player.path_focused = false;
        player.url_focused = false;
    }
    book8gui_invalidate();
    return true;
}

static bool handle_goto_click(const struct book8gui_platform *p,
    const struct player_layout *layout, const struct book8gui_event *event)
{
    struct book8gui_rect field = goto_field_rect(layout);
    struct book8gui_rect ok = goto_ok_rect(layout);
    struct book8gui_rect cancel = goto_cancel_rect(layout);
    if (inside(ok, event->mouse_x, event->mouse_y)) {
        goto_activate(p);
    } else if (inside(cancel, event->mouse_x, event->mouse_y)
        || inside(field, event->mouse_x, event->mouse_y)) {
        player.dialog = PLAYER_DIALOG_NONE;
    }
    book8gui_invalidate();
    return true;
}

static bool handle_confirm_stream_click(const struct book8gui_platform *p,
    const struct player_layout *layout, const struct book8gui_event *event)
{
    if (inside(confirm_open_rect(layout), event->mouse_x, event->mouse_y)) {
        accept_network_stream(p);
    } else if (inside(confirm_cancel_rect(layout), event->mouse_x,
        event->mouse_y)) {
        player.dialog = PLAYER_DIALOG_NONE;
        set_message("Network stream cancelled.");
        book8gui_invalidate();
    }
    return true;
}

static bool handle_bookmark_click(const struct book8gui_platform *p,
    const struct player_layout *layout, const struct book8gui_event *event)
{
    struct book8gui_rect dialog = goto_dialog_rect(layout);
    if (inside(goto_cancel_rect(layout), event->mouse_x, event->mouse_y)) {
        player.dialog = PLAYER_DIALOG_NONE;
        book8gui_invalidate();
        return true;
    }
    for (uint32_t i = 0u; i < player.bookmarks.count && i < 2u; ++i) {
        struct book8gui_rect row = {dialog.x + 12u, dialog.y + 26u + i * 18u,
            dialog.width - 24u, 18u};
        if (inside(row, event->mouse_x, event->mouse_y)) {
            seek_to_position(p, player.bookmarks.bookmarks[i].position_ms);
            player.dialog = PLAYER_DIALOG_NONE;
            book8gui_invalidate();
            return true;
        }
    }
    return true;
}

static bool handle_preferences_click(const struct book8gui_platform *p,
    const struct player_layout *layout, const struct book8gui_event *event)
{
    for (uint32_t i = 0u; i < PLAYER_PREFS_SECTION_COUNT; ++i) {
        struct book8gui_rect nav = prefs_nav_rect(layout, i);
        if (inside(nav, event->mouse_x, event->mouse_y)) {
            player.prefs_section = (uint8_t)i;
            book8gui_invalidate();
            return true;
        }
    }
    if (inside(prefs_save_rect(layout), event->mouse_x, event->mouse_y)) {
        commit_preferences(p, true);
        return true;
    }
    if (inside(prefs_cancel_rect(layout), event->mouse_x, event->mouse_y)) {
        commit_preferences(p, false);
        return true;
    }
    if (inside(prefs_reset_rect(layout), event->mouse_x, event->mouse_y)) {
        book8_media_prefs_defaults(&player.prefs);
        set_message("Defaults restored; press Save to keep them.");
        book8gui_invalidate();
        return true;
    }

    uint32_t maximum_rows = player.prefs_section == 5u ? 5u : 5u;
    for (uint32_t row = 0u; row < maximum_rows; ++row) {
        struct book8gui_rect rect = prefs_row_rect(layout, row);
        if (!inside(rect, event->mouse_x, event->mouse_y)) {
            continue;
        }
        switch (player.prefs_section) {
        case 0u:
            if (row == 0u) {
                player.prefs.interface_flags ^= BOOK8_MEDIA_IF_STATUS;
            } else if (row == 1u) {
                player.prefs.interface_flags ^= BOOK8_MEDIA_IF_PLAYLIST;
            } else if (row == 2u) {
                player.prefs.interface_flags ^= BOOK8_MEDIA_IF_DARK_CANVAS;
            }
            break;
        case 1u: {
            struct book8gui_rect content = prefs_content_rect(layout);
            struct book8gui_rect track = {rect.x + rect.width - 156u,
                rect.y + 6u, 120u, 8u};
            uint32_t offset = event->mouse_x < (int32_t)track.x ? 0u
                : event->mouse_x > (int32_t)(track.x + track.width)
                    ? track.width
                    : (uint32_t)(event->mouse_x - (int32_t)track.x);
            player.prefs.max_volume = (uint8_t)(((uint64_t)offset * 100u)
                / track.width);
            if (player.prefs.max_volume == 0u) {
                player.prefs.max_volume = 1u;
            }
            (void)content;
            break;
        }
        case 2u:
            if (row == 0u) {
                player.prefs.plugins ^= BOOK8_MEDIA_PLUGIN_PCM_WAV;
            } else if (row == 1u) {
                player.prefs.plugins ^= BOOK8_MEDIA_PLUGIN_HTTP_WAV;
            } else if (row == 2u) {
                player.prefs.plugins ^= BOOK8_MEDIA_PLUGIN_PLAYLIST;
            } else if (row == 3u) {
                player.prefs.plugins ^= BOOK8_MEDIA_PLUGIN_VISUALIZATION;
            }
            break;
        case 3u:
            if (row < 4u) {
                player.prefs.plugins ^= 1u << row;
            }
            break;
        case 4u:
            if (row == 0u) {
                player.prefs.web ^= BOOK8_MEDIA_WEB_AUTOPLAY;
            } else if (row == 1u) {
                player.prefs.web ^= BOOK8_MEDIA_WEB_MEDIA_PANEL;
            } else if (row == 2u) {
                player.prefs.web ^= BOOK8_MEDIA_WEB_OPEN_IN_PLAYER;
            }
            break;
        default:
            break;
        }
        book8gui_invalidate();
        return true;
    }
    return true;
}

static bool handle_event(const struct book8gui_platform *p,
    struct WindowApp *app, const struct book8gui_event *event)
{
    struct player_layout layout = player_layout(app);
    bool down = (event->mouse_buttons & 1u) != 0u;
    bool pressed = down && (player.previous_buttons & 1u) == 0u;

    if (event->type == BOOK8GUI_EVENT_KEY) {
        return handle_key(p, app, event);
    }
    if (event->type != BOOK8GUI_EVENT_MOUSE) {
        return false;
    }

    if (player.drag != PLAYER_DRAG_NONE) {
        if (down) {
            uint32_t x = event->mouse_x < 0 ? 0u : (uint32_t)event->mouse_x;
            if (player.drag == PLAYER_DRAG_TIMELINE) {
                seek_to_x(p, x, &layout.seek);
            } else {
                set_volume_from_x(p, x, &layout.volume_track);
            }
        } else {
            player.drag = PLAYER_DRAG_NONE;
        }
        player.previous_buttons = event->mouse_buttons;
        return true;
    }

    if (pressed) {
        if (handle_menu_click(p, app, &layout, event)) {
            player.previous_buttons = event->mouse_buttons;
            return true;
        }
        if (player.dialog == PLAYER_DIALOG_OPEN) {
            handle_open_dialog_click(p, app, &layout, event);
        } else if (player.dialog == PLAYER_DIALOG_GOTO) {
            handle_goto_click(p, &layout, event);
        } else if (player.dialog == PLAYER_DIALOG_BOOKMARKS) {
            handle_bookmark_click(p, &layout, event);
        } else if (player.dialog == PLAYER_DIALOG_CONFIRM_STREAM) {
            handle_confirm_stream_click(p, &layout, event);
        } else if (player.view == PLAYER_VIEW_PREFERENCES) {
            handle_preferences_click(p, &layout, event);
        } else if (player.view == PLAYER_VIEW_ABOUT) {
            if (inside(playlist_close_rect(&layout), event->mouse_x,
                event->mouse_y)) {
                player.view = PLAYER_VIEW_NOW_PLAYING;
            }
        } else if (player.view == PLAYER_VIEW_PLAYLIST) {
            if (inside(playlist_close_rect(&layout), event->mouse_x,
                event->mouse_y)) {
                player.view = PLAYER_VIEW_NOW_PLAYING;
            } else {
                uint32_t visible = (playlist_panel(&layout).height - 44u) / 18u;
                for (uint32_t i = 0u; i < player.playlist_count
                    && i < visible; ++i) {
                    struct book8gui_rect row = playlist_row_rect(&layout, i);
                    if (inside(row, event->mouse_x, event->mouse_y)) {
                        playlist_select(p, app, i);
                        break;
                    }
                }
            }
        } else if (inside(layout.prev, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_PREV);
        } else if (inside(layout.play, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_PLAY_PAUSE);
        } else if (inside(layout.pause, event->mouse_x, event->mouse_y)) {
            pause_playback(p);
        } else if (inside(layout.stop, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_STOP);
        } else if (inside(layout.next, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_NEXT);
        } else if (inside(layout.playlist, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_VIEW_PLAYLIST);
        } else if (inside(layout.prefs, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_PREFERENCES);
        } else if (inside(layout.mute, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_MUTE);
        } else if (inside(layout.repeat, event->mouse_x, event->mouse_y)) {
            ++player.prefs.repeat;
            if (player.prefs.repeat > BOOK8_MEDIA_REPEAT_ONE) {
                player.prefs.repeat = BOOK8_MEDIA_REPEAT_NONE;
            }
            (void)book8_media_prefs_save(p, &player.prefs);
        } else if (inside(layout.bookmark, event->mouse_x, event->mouse_y)) {
            execute_command(p, app, CMD_BOOKMARK_CREATE);
        } else if (inside(layout.speed, event->mouse_x, event->mouse_y)) {
            ++player.speed_index;
            if (player.speed_index >= sizeof(speeds) / sizeof(speeds[0])) {
                player.speed_index = 0u;
            }
            set_message("Speed marker changed; HDA plays at source rate.");
        } else if (inside(layout.volume_track, event->mouse_x,
            event->mouse_y)) {
            player.drag = PLAYER_DRAG_VOLUME;
            set_volume_from_x(p, (uint32_t)event->mouse_x,
                &layout.volume_track);
        } else if (inside(layout.seek, event->mouse_x, event->mouse_y)) {
            player.drag = PLAYER_DRAG_TIMELINE;
            seek_to_x(p, (uint32_t)event->mouse_x, &layout.seek);
        } else {
            player.path_focused = false;
            player.url_focused = false;
        }
        book8gui_invalidate();
    }
    player.previous_buttons = event->mouse_buttons;
    return true;
}

static void *create(const struct book8gui_platform *p)
{
    player = (struct player_state){0};
    player.speed_index = 2u;
    player.menu = 0xffu;
    (void)book8_media_prefs_load(p);
    apply_preferences(p);
    if ((player.prefs.interface_flags & BOOK8_MEDIA_IF_PLAYLIST) != 0u) {
        player.view = PLAYER_VIEW_PLAYLIST;
    }
    set_message("Open a PCM WAV file or HTTP stream to begin.");
    if (p->audio_state != NULL) {
        bool muted = false;
        (void)p->audio_state(&player.volume, &muted);
        clamp_volume();
    }
    update_media_state(p);
    return &player;
}

static void destroy(void *state)
{
    (void)state;
    player.loaded = false;
}

static bool open_path(const struct book8gui_platform *p,
    struct WindowApp *app, const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (strncmp(path, "http://", 7u) == 0
        || strncmp(path, "HTTP://", 7u) == 0) {
        snprintf(player.url, sizeof(player.url), "%s", path);
        player.dialog = PLAYER_DIALOG_NONE;
        play_url(p);
        return true;
    }
    if (book8_media_classify_url(path) == BOOK8_MEDIA_WAV) {
        snprintf(player.path, sizeof(player.path), "%s", path);
        player.dialog = PLAYER_DIALOG_NONE;
        return start_wav(p, app);
    }
    set_message("BOOK8 Player accepts WAV files and HTTP stream URLs.");
    book8gui_invalidate();
    return false;
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
    .open_path = open_path,
    .tick = tick
};
