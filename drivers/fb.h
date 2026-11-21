#ifndef BOOK8_FB_H
#define BOOK8_FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Linear framebuffer driver: 16/24/32 bpp, clipped primitives, optional
 * system-memory back buffer, 8x16 bitmap font and a text console. Colors
 * passed to the API are 0xRRGGBB; fb_pack() converts to the native pixel
 * format. No dependencies beyond <stdint.h>. */

#define FB_FONT_WIDTH   8u
#define FB_FONT_HEIGHT  16u
#define FB_FONT_FIRST   0x20u
#define FB_FONT_GLYPHS  95u     /* printable ASCII 0x20..0x7e */

extern const uint8_t fb_font8x16[FB_FONT_GLYPHS][FB_FONT_HEIGHT];

struct fb_format {
    uint8_t bpp;                /* 16, 24 or 32 */
    uint8_t red_pos, red_size;
    uint8_t green_pos, green_size;
    uint8_t blue_pos, blue_size;
};

struct fb {
    uint8_t *front;             /* mapped scanout memory */
    uint8_t *draw;              /* where primitives write (front or back) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per scanline */
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t scanout_pitch;
    uint8_t bytes;              /* bytes per pixel */
    struct fb_format format;
};

struct fb_rect {
    int32_t x, y;
    uint32_t w, h;
};

/* Pixels already in the target's native format (see fb_pack). */
struct fb_image {
    const void *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per source row */
};

/* Validates geometry and format; returns false (leaving fb unusable) on
 * bad input. `base` is the identity-mapped framebuffer address. */
bool fb_init(struct fb *fb, void *base, uint32_t width, uint32_t height,
    uint32_t pitch, const struct fb_format *format);

/* Redirects drawing to `memory` (>= pitch * height bytes) and copies the
 * current front contents into it; fb_present() publishes it. Passing NULL
 * returns to native scanout dimensions and direct drawing. */
void fb_set_backbuffer(struct fb *fb, void *memory);
/* Sets a software/logical canvas scaled to the firmware-owned scanout.
 * Returns false without changing fb when the requested surface does not fit. */
bool fb_set_logical_mode(struct fb *fb, uint32_t width, uint32_t height,
    void *memory, size_t capacity);
void fb_present(struct fb *fb);
void fb_present_rect(struct fb *fb, int32_t x, int32_t y, uint32_t w,
    uint32_t h);

uint32_t fb_pack(const struct fb *fb, uint32_t rgb);

/* Drawing primitives. `color` is native (pre-packed with fb_pack). All
 * coordinates are clipped to the screen. */
void fb_put_pixel(struct fb *fb, int32_t x, int32_t y, uint32_t color);
void fb_hline(struct fb *fb, int32_t x, int32_t y, uint32_t length,
    uint32_t color);
void fb_vline(struct fb *fb, int32_t x, int32_t y, uint32_t length,
    uint32_t color);
void fb_fill_rect(struct fb *fb, int32_t x, int32_t y, uint32_t w, uint32_t h,
    uint32_t color);
void fb_draw_rect(struct fb *fb, int32_t x, int32_t y, uint32_t w, uint32_t h,
    uint32_t color);
void fb_clear(struct fb *fb, uint32_t color);

/* Copies native-format pixels; per-row copy, clipped. */
void fb_blit(struct fb *fb, int32_t x, int32_t y, const struct fb_image *src);
/* Converts 32-bit 0x00RRGGBB source pixels (pitch in bytes). Pixels equal to
 * `key` are skipped when `use_key` is set. */
void fb_blit_rgb(struct fb *fb, int32_t x, int32_t y, const uint32_t *src,
    uint32_t w, uint32_t h, uint32_t pitch, bool use_key, uint32_t key);
/* Overlap-safe copy inside the framebuffer (scrolling, window moves). */
void fb_copy_rect(struct fb *fb, int32_t dx, int32_t dy, int32_t sx,
    int32_t sy, uint32_t w, uint32_t h);
/* Scrolls the whole screen up by `lines` pixel rows, filling with color. */
void fb_scroll_up(struct fb *fb, uint32_t lines, uint32_t color);

/* Text: fb_draw_char paints foreground and background; the _fg variant
 * leaves background pixels untouched. Unprintable codes draw as '?'. */
void fb_draw_char(struct fb *fb, int32_t x, int32_t y, char c, uint32_t fg,
    uint32_t bg);
void fb_draw_char_fg(struct fb *fb, int32_t x, int32_t y, char c, uint32_t fg);
void fb_draw_string(struct fb *fb, int32_t x, int32_t y, const char *text,
    uint32_t fg, uint32_t bg);

/* Cell-based console for early logging and the shell prompt. */
struct fb_console {
    struct fb *fb;
    uint32_t columns, rows;
    uint32_t col, row;
    uint32_t fg, bg;            /* native colors */
};

void fb_console_init(struct fb_console *con, struct fb *fb, uint32_t fg,
    uint32_t bg);
void fb_console_clear(struct fb_console *con);
/* Handles \n, \r, \b, \t; wraps and scrolls at the bottom. */
void fb_console_putc(struct fb_console *con, char c);
void fb_console_write(struct fb_console *con, const char *text);

#endif
