#ifndef BOOK8_MEDIA_EXTENSION_H
#define BOOK8_MEDIA_EXTENSION_H

#include <stdbool.h>
#include <stdint.h>

#define BOOK8_MEDIA_URL_CAPACITY 256u
#define BOOK8_MEDIA_LABEL_CAPACITY 80u

enum book8_media_kind {
    BOOK8_MEDIA_OTHER,
    BOOK8_MEDIA_WAV,
    BOOK8_MEDIA_UNSUPPORTED_AUDIO,
    BOOK8_MEDIA_VIDEO
};

struct book8_media_source {
    char url[BOOK8_MEDIA_URL_CAPACITY];
    char label[BOOK8_MEDIA_LABEL_CAPACITY];
    enum book8_media_kind kind;
};

typedef bool (*book8_media_visitor)(void *context,
    const struct book8_media_source *source);

enum book8_media_kind book8_media_classify_url(const char *url);
uint32_t book8_media_scan_html(const char *html, uint32_t size,
    book8_media_visitor visitor, void *context);

/* Media Player preferences shared with the Web Browser. */
#define BOOK8_MEDIA_PREFS_PATH "A:\\MEDIA.CFG"

#define BOOK8_MEDIA_IF_STATUS 0x1u
#define BOOK8_MEDIA_IF_PLAYLIST 0x2u
#define BOOK8_MEDIA_IF_DARK_CANVAS 0x4u

#define BOOK8_MEDIA_PLUGIN_PCM_WAV 0x1u
#define BOOK8_MEDIA_PLUGIN_HTTP_WAV 0x2u
#define BOOK8_MEDIA_PLUGIN_PLAYLIST 0x4u
#define BOOK8_MEDIA_PLUGIN_VISUALIZATION 0x8u

#define BOOK8_MEDIA_WEB_AUTOPLAY 0x1u
#define BOOK8_MEDIA_WEB_MEDIA_PANEL 0x2u
#define BOOK8_MEDIA_WEB_OPEN_IN_PLAYER 0x4u

#define BOOK8_MEDIA_REPEAT_NONE 0u
#define BOOK8_MEDIA_REPEAT_ALL 1u
#define BOOK8_MEDIA_REPEAT_ONE 2u

struct book8_media_preferences {
    uint32_t interface_flags;
    uint32_t plugins;
    uint32_t web;
    uint8_t max_volume;
    uint8_t repeat;
};

struct book8gui_platform;

const struct book8_media_preferences *book8_media_prefs(void);
void book8_media_prefs_defaults(struct book8_media_preferences *prefs);
bool book8_media_prefs_equal(const struct book8_media_preferences *a,
    const struct book8_media_preferences *b);
bool book8_media_prefs_load(const struct book8gui_platform *platform);
bool book8_media_prefs_save(const struct book8gui_platform *platform,
    const struct book8_media_preferences *prefs);

#endif
