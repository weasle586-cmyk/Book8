#ifndef BOOK8_APPS_UI_H
#define BOOK8_APPS_UI_H

#include <stdint.h>

#include "../book8gui.h"

/*
 * Shared window chrome for system apps, matching NoteEditor: a 4 px margin
 * inside the window frame, a sunken white viewport with 3 px padding and a
 * 12 px scrollbar on its right edge. Header-only so apps need no new objects.
 */
#define UI_FACE (book8_config_value(BOOK8_CFG_DARK) ? 0x303030u : 0xc0c0c0u)
#define UI_LIGHT 0xffffffu
#define UI_SHADOW 0x808080u
#define UI_EDGE 0x000000u
#define UI_FIELD (book8_config_value(BOOK8_CFG_DARK) ? 0x202020u : 0xffffffu)
#define UI_TEXT (book8_config_value(BOOK8_CFG_DARK) ? 0xf0f0f0u : 0x000000u)
#define UI_MUTED (book8_config_value(BOOK8_CFG_DARK) ? 0xb0b0b0u : 0x404040u)
#define UI_SELECTED (book8_accent_colors[book8_config_value(BOOK8_CFG_ACCENT)])
#define UI_SELECTED_TEXT 0xffffffu
#define UI_PAD 3u
#define UI_SCROLL_W 12u
#define UI_STATUS_H 20u
#define UI_THUMB_MIN 16u

/* Area under the title bar that apps may draw in. */
static inline struct book8gui_rect ui_client(const struct WindowApp *app)
{
    return (struct book8gui_rect){
        app->bounds.x + 4u, app->bounds.y + 30u,
        app->bounds.width > 8u ? app->bounds.width - 8u : 0u,
        app->bounds.height > 34u ? app->bounds.height - 34u : 0u
    };
}

static inline void ui_sunken(const struct book8gui_platform *platform,
    const struct book8gui_rect *rect, uint32_t face)
{
    if (rect->width < 4u || rect->height < 4u) {
        return;
    }
    platform->fill_rect(rect->x, rect->y, rect->width, rect->height,
        UI_SHADOW);
    platform->fill_rect(rect->x + 1u, rect->y + 1u, rect->width - 2u,
        rect->height - 2u, face);
}

static inline bool ui_contains(const struct book8gui_rect *rect, int32_t x,
    int32_t y)
{
    return x >= (int32_t)rect->x && y >= (int32_t)rect->y
        && x < (int32_t)(rect->x + rect->width)
        && y < (int32_t)(rect->y + rect->height);
}

/* Scrollbar track for a sunken viewport frame (right edge, inside border). */
static inline struct book8gui_rect ui_scroll_track(
    const struct book8gui_rect *frame)
{
    return (struct book8gui_rect){
        frame->x + (frame->width > UI_SCROLL_W + 1u ? frame->width - UI_SCROLL_W - 1u : 0u),
        frame->y + 2u, frame->width > UI_SCROLL_W ? UI_SCROLL_W : frame->width,
        frame->height > 4u ? frame->height - 4u : 0u
    };
}

static inline uint32_t ui_thumb_height(const struct book8gui_rect *track,
    uint32_t total, uint32_t visible)
{
    if (total <= visible || total == 0u) {
        return track->height;
    }
    uint32_t thumb = (uint32_t)((uint64_t)track->height * visible / total);
    if (thumb < UI_THUMB_MIN) {
        thumb = UI_THUMB_MIN;
    }
    return thumb > track->height ? track->height : thumb;
}

static inline struct book8gui_rect ui_scroll_thumb(
    const struct book8gui_rect *track, uint32_t total, uint32_t visible,
    uint32_t first)
{
    uint32_t height = ui_thumb_height(track, total, visible);
    uint32_t span = track->height - height;
    uint32_t max_first = total > visible ? total - visible : 0u;
    uint32_t offset = max_first == 0u ? 0u : first >= max_first ? span
        : (uint32_t)((uint64_t)span * first / max_first);
    return (struct book8gui_rect){track->x, track->y + offset, track->width, height};
}

/* Preserve the point grabbed inside the thumb; track clicks center it. */
static inline int32_t ui_scrollbar_grab(const struct book8gui_rect *track,
    uint32_t total, uint32_t visible, uint32_t first, int32_t y)
{
    struct book8gui_rect thumb = ui_scroll_thumb(track, total, visible, first);
    return y >= (int32_t)thumb.y && y < (int32_t)(thumb.y + thumb.height)
        ? y - (int32_t)thumb.y : (int32_t)(thumb.height / 2u);
}

/* first is the index of the first visible row/line of total. */
static inline void ui_scrollbar(const struct book8gui_platform *platform,
    const struct book8gui_rect *track, uint32_t total, uint32_t visible,
    uint32_t first)
{
    if (track->height < 4u || track->width < 3u) {
        return;
    }
    platform->fill_rect(track->x, track->y, track->width, track->height,
        UI_FACE);
    if (total <= visible) {
        return;
    }
    struct book8gui_rect thumb = ui_scroll_thumb(track, total, visible, first);
    platform->fill_rect(thumb.x + 1u, thumb.y, thumb.width - 2u,
        thumb.height, UI_SHADOW);
}

/* Maps a pointer y inside/near the track to a first row/line index. */
static inline uint32_t ui_scrollbar_drag_position(
    const struct book8gui_rect *track, uint32_t total, uint32_t visible,
    int32_t y, int32_t grab)
{
    if (total <= visible) {
        return 0u;
    }
    uint32_t thumb = ui_thumb_height(track, total, visible);
    uint32_t span = track->height - thumb;
    if (span == 0u) { return 0u; }
    int32_t relative = y - (int32_t)track->y - grab;
    uint32_t clamped = relative < 0 ? 0u
        : ((uint32_t)relative > span ? span : (uint32_t)relative);
    return (uint32_t)(((uint64_t)(total - visible) * clamped + span / 2u) / span);
}

static inline uint32_t ui_scrollbar_position(
    const struct book8gui_rect *track, uint32_t total, uint32_t visible, int32_t y)
{
    return ui_scrollbar_drag_position(track, total, visible, y,
        (int32_t)(ui_thumb_height(track, total, visible) / 2u));
}

/* Wheel notches: positive is wheel up (toward the first row). */
static inline uint32_t ui_wheel_scroll(uint32_t first, uint32_t total,
    uint32_t visible, int32_t wheel, uint32_t lines_per_notch)
{
    uint32_t max_first = total > visible ? total - visible : 0u;
    int64_t next = (int64_t)first - (int64_t)wheel * lines_per_notch;
    if (next < 0) {
        next = 0;
    }
    return next > max_first ? max_first : (uint32_t)next;
}

static inline int32_t ui_wheel_steps(int16_t delta, int32_t *remainder)
{
    int32_t accumulated = *remainder + delta;
    int32_t steps = accumulated / BOOK8_WHEEL_DELTA;
    *remainder = accumulated % BOOK8_WHEEL_DELTA;
    return steps;
}

static inline uint32_t ui_wheel_scroll_delta(uint32_t first, uint32_t total,
    uint32_t visible, int16_t delta, uint32_t lines_per_detent, int32_t *remainder)
{
    return ui_wheel_scroll(first, total, visible,
        ui_wheel_steps(delta, remainder), lines_per_detent);
}

#endif
