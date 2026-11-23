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

/* Bookmarks live in a separate file so the frozen 28-byte MEDIA.CFG layout is
 * never expanded. The file holds a bounded set of path-keyed bookmark lists. */
#define BOOK8_MEDIA_BOOKMARK_PATH "A:\\MEDIA.BMK"
#define BOOK8_MEDIA_BOOKMARK_PATH_CAPACITY 160u
#define BOOK8_MEDIA_BOOKMARK_LABEL_CAPACITY 24u
#define BOOK8_MEDIA_BOOKMARK_MAX_PER_MEDIA 8u
#define BOOK8_MEDIA_BOOKMARK_MAX_MEDIA 8u

struct book8_media_bookmark {
    uint32_t position_ms;
    char label[BOOK8_MEDIA_BOOKMARK_LABEL_CAPACITY];
};

struct book8_media_bookmark_set {
    char path[BOOK8_MEDIA_BOOKMARK_PATH_CAPACITY];
    uint32_t count;
    struct book8_media_bookmark bookmarks[BOOK8_MEDIA_BOOKMARK_MAX_PER_MEDIA];
};

/* -------------------------------------------------------------------------
 * FROZEN media preference bit positions.
 *
 * These values are written verbatim into A:\MEDIA.CFG (see MEDIA_PREFS_BYTES
 * in book8_media_extension.c) and are shared between the Player and Browser.
 * They must never be renumbered: doing so silently reinterprets saved user
 * settings. New flags must claim a bit marked "reserved" below, and the
 * _Static_assert block in book8_media_extension.c will reject any drift.
 *
 * interface_flags:
 *   bit 0 STATUS, bit 1 PLAYLIST, bit 2 DARK_CANVAS,
 *   bit 3 reserved, bit 4 SUBTITLES, bits 5..31 reserved
 * plugins:
 *   bit 0 PCM_WAV, bit 1 HTTP_WAV, bit 2 PLAYLIST, bit 3 VISUALIZATION,
 *   bit 4 MP3, bit 5 Video, bits 6..31 reserved
 * web (separate field; see book8_browser.c for gating):
 *   bit 0 AUTOPLAY, bit 1 MEDIA_PANEL, bit 2 OPEN_IN_PLAYER,
 *   bit 3 JAVASCRIPT, bits 4..5 reserved, bit 6 CSS, bits 7..31 reserved
 * ------------------------------------------------------------------------- */
#define BOOK8_MEDIA_IF_STATUS          (1u << 0)  /* 0x01 */
#define BOOK8_MEDIA_IF_PLAYLIST        (1u << 1)  /* 0x02 */
#define BOOK8_MEDIA_IF_DARK_CANVAS     (1u << 2)  /* 0x04 */
                                              /* 0x08 reserved */
#define BOOK8_MEDIA_IF_SUBTITLES       (1u << 4)  /* 0x10 */

#define BOOK8_MEDIA_PLUGIN_PCM_WAV     (1u << 0)  /* 0x01 */
#define BOOK8_MEDIA_PLUGIN_HTTP_WAV    (1u << 1)  /* 0x02 */
#define BOOK8_MEDIA_PLUGIN_PLAYLIST    (1u << 2)  /* 0x04 */
#define BOOK8_MEDIA_PLUGIN_VISUALIZATION (1u << 3) /* 0x08 */
#define BOOK8_MEDIA_PLUGIN_MP3         (1u << 4)  /* 0x10 */
#define BOOK8_MEDIA_PLUGIN_Video       (1u << 5)  /* 0x20 */

#define BOOK8_MEDIA_WEB_AUTOPLAY       (1u << 0)  /* 0x01 */
#define BOOK8_MEDIA_WEB_MEDIA_PANEL    (1u << 1)  /* 0x02 */
#define BOOK8_MEDIA_WEB_OPEN_IN_PLAYER (1u << 2)  /* 0x04 */
#define BOOK8_MEDIA_WEB_JAVASCRIPT     (1u << 3)  /* 0x08 */
                                              /* 0x10, 0x20 reserved */
#define BOOK8_MEDIA_WEB_CSS            (1u << 6)  /* 0x40 */

/* Repeat mode occupies a byte, not a bit field; also frozen. */
#define BOOK8_MEDIA_REPEAT_NONE 0u
#define BOOK8_MEDIA_REPEAT_ALL  1u
#define BOOK8_MEDIA_REPEAT_ONE  2u

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

/* Load the saved bookmark list for path. False means no saved list exists. */
bool book8_media_bookmarks_load(const struct book8gui_platform *platform,
    const char *path, struct book8_media_bookmark_set *set);
/* Replace (or add) the bookmark list for path, keeping the file bounded. */
bool book8_media_bookmarks_save(const struct book8gui_platform *platform,
    const char *path, const struct book8_media_bookmark_set *set);

#endif
