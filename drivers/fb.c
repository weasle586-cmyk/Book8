#include "fb.h"

typedef uint32_t __attribute__((__may_alias__)) fb_u32;
typedef uint16_t __attribute__((__may_alias__)) fb_u16;
typedef uint64_t __attribute__((__may_alias__)) fb_u64;

static inline bool fb_usable(const struct fb *fb)
{
    return fb != NULL && fb->draw != NULL;
}

static inline uint8_t *fb_row(const struct fb *fb, uint32_t y)
{
    return fb->draw + (size_t)y * fb->pitch;
}

/* Intersects a rectangle with the screen; false when nothing is visible. */
static bool fb_clip(const struct fb *fb, int32_t *x, int32_t *y, uint32_t *w,
    uint32_t *h)
{
    int64_t x0 = *x, y0 = *y;
    int64_t x1 = x0 + (int64_t)*w, y1 = y0 + (int64_t)*h;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int64_t)fb->width) {
        x1 = fb->width;
    }
    if (y1 > (int64_t)fb->height) {
        y1 = fb->height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }
    *x = (int32_t)x0;
    *y = (int32_t)y0;
    *w = (uint32_t)(x1 - x0);
    *h = (uint32_t)(y1 - y0);
    return true;
}

static inline void fb_store(uint8_t *p, uint32_t color, uint8_t bytes)
{
    if (bytes == 4u && ((uintptr_t)p & 3u) == 0u) {
        *(fb_u32 *)p = color;
    } else if (bytes == 2u && ((uintptr_t)p & 1u) == 0u) {
        *(fb_u16 *)p = (uint16_t)color;
    } else {
        for (uint8_t i = 0u; i < bytes; ++i) {
            p[i] = (uint8_t)(color >> (8u * i));
        }
    }
}

static void fb_fill_span(uint8_t *p, uint32_t count, uint32_t color,
    uint8_t bytes)
{
    if (bytes == 4u && ((uintptr_t)p & 3u) == 0u) {
        uint64_t pair = ((uint64_t)color << 32) | color;

        if (count != 0u && ((uintptr_t)p & 4u) != 0u) {
            *(fb_u32 *)p = color;
            p += 4;
            --count;
        }
        for (; count >= 2u; count -= 2u, p += 8) {
            *(fb_u64 *)p = pair;
        }
        if (count != 0u) {
            *(fb_u32 *)p = color;
        }
        return;
    }
    for (; count != 0u; --count, p += bytes) {
        fb_store(p, color, bytes);
    }
}

/* memmove for scanline data; aligned bodies move 8 bytes at a time. */
static void fb_move(uint8_t *dst, const uint8_t *src, size_t n)
{
    bool wide = (((uintptr_t)dst | (uintptr_t)src) & 7u) == 0u;

    if (dst == src || n == 0u) {
        return;
    }
    if (dst < src) {
        if (wide) {
            for (; n >= 8u; n -= 8u, dst += 8, src += 8) {
                *(fb_u64 *)dst = *(const fb_u64 *)src;
            }
        }
        while (n--) {
            *dst++ = *src++;
        }
    } else {
        dst += n;
        src += n;
        if (wide) {
            for (; n >= 8u; n -= 8u) {
                dst -= 8;
                src -= 8;
                *(fb_u64 *)dst = *(const fb_u64 *)src;
            }
        }
        while (n--) {
            *--dst = *--src;
        }
    }
}

static void fb_seed_backbuffer(const struct fb *fb, uint8_t *memory)
{
    uint32_t logical_width = fb->width;
    uint32_t logical_height = fb->height;

    if (logical_width == fb->scanout_width
        && logical_height == fb->scanout_height
        && fb->pitch == fb->scanout_pitch) {
        fb_move(memory, fb->front,
            (size_t)fb->scanout_pitch * fb->scanout_height);
        return;
    }
    for (uint32_t y = 0u; y < logical_height; ++y) {
        uint32_t sy = (uint32_t)((uint64_t)y * fb->scanout_height
            / logical_height);
        uint8_t *dst = memory + (size_t)y * fb->pitch;
        const uint8_t *src = fb->front + (size_t)sy * fb->scanout_pitch;

        for (uint32_t x = 0u; x < logical_width; ++x) {
            uint32_t sx = (uint32_t)((uint64_t)x * fb->scanout_width
                / logical_width);
            fb_move(dst + (size_t)x * fb->bytes,
                src + (size_t)sx * fb->bytes, fb->bytes);
        }
    }
}

bool fb_init(struct fb *fb, void *base, uint32_t width, uint32_t height,
    uint32_t pitch, const struct fb_format *format)
{
    const struct fb_format *f = format;
    uint32_t bytes;
    uint64_t row_bytes;
    uint64_t buffer_bytes;

    if (fb == NULL) {
        return false;
    }
    *fb = (struct fb){0};
    if (base == NULL || f == NULL || width == 0u || height == 0u
        || (f->bpp != 16u && f->bpp != 24u && f->bpp != 32u)) {
        return false;
    }
    bytes = f->bpp / 8u;
    row_bytes = (uint64_t)width * bytes;
    buffer_bytes = (uint64_t)pitch * height;
    if (row_bytes > pitch || buffer_bytes > SIZE_MAX
        || f->red_size > 8u || f->green_size > 8u || f->blue_size > 8u
        || f->red_pos + f->red_size > f->bpp
        || f->green_pos + f->green_size > f->bpp
        || f->blue_pos + f->blue_size > f->bpp) {
        return false;
    }
    fb->front = base;
    fb->draw = base;
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->scanout_width = width;
    fb->scanout_height = height;
    fb->scanout_pitch = pitch;
    fb->bytes = (uint8_t)bytes;
    fb->format = *f;
    return true;
}

void fb_set_backbuffer(struct fb *fb, void *memory)
{
    if (!fb_usable(fb)) {
        return;
    }
    if (memory == NULL) {
        fb->width = fb->scanout_width;
        fb->height = fb->scanout_height;
        fb->pitch = fb->scanout_pitch;
        fb->draw = fb->front;
        return;
    }
    fb_seed_backbuffer(fb, memory);
    fb->draw = memory;
}

bool fb_set_logical_mode(struct fb *fb, uint32_t width, uint32_t height,
    void *memory, size_t capacity)
{
    uint64_t pitch, size;

    if (!fb_usable(fb) || memory == NULL || width == 0u || height == 0u) {
        return false;
    }
    pitch = (uint64_t)width * fb->bytes;
    if (pitch > UINT32_MAX || pitch > (uint64_t)capacity / height) {
        return false;
    }
    size = pitch * height;
    if (size > SIZE_MAX) {
        return false;
    }
    fb->width = width;
    fb->height = height;
    fb->pitch = (uint32_t)pitch;
    fb_seed_backbuffer(fb, memory);
    fb->draw = memory;
    return true;
}

void fb_present_rect(struct fb *fb, int32_t x, int32_t y, uint32_t w,
    uint32_t h)
{
    if (!fb_usable(fb) || fb->draw == fb->front) {
        return;
    }
    if (!fb_clip(fb, &x, &y, &w, &h)) {
        return;
    }
    if (fb->width != fb->scanout_width
        || fb->height != fb->scanout_height) {
        uint32_t scan_x0 = (uint32_t)(((uint64_t)(uint32_t)x
            * fb->scanout_width + fb->width - 1u) / fb->width);
        uint32_t scan_y0 = (uint32_t)(((uint64_t)(uint32_t)y
            * fb->scanout_height + fb->height - 1u) / fb->height);
        uint32_t scan_x1 = (uint32_t)(((uint64_t)((uint32_t)x + w)
            * fb->scanout_width + fb->width - 1u) / fb->width);
        uint32_t scan_y1 = (uint32_t)(((uint64_t)((uint32_t)y + h)
            * fb->scanout_height + fb->height - 1u) / fb->height);

        if (scan_x1 > fb->scanout_width) {
            scan_x1 = fb->scanout_width;
        }
        if (scan_y1 > fb->scanout_height) {
            scan_y1 = fb->scanout_height;
        }
        for (uint32_t dy = scan_y0; dy < scan_y1; ++dy) {
            uint32_t sy = (uint32_t)((uint64_t)dy * fb->height
                / fb->scanout_height);
            uint8_t *dst = fb->front + (size_t)dy * fb->scanout_pitch;
            const uint8_t *src = fb->draw + (size_t)sy * fb->pitch;

            for (uint32_t dx = scan_x0; dx < scan_x1; ++dx) {
                uint32_t sx = (uint32_t)((uint64_t)dx * fb->width
                    / fb->scanout_width);
                fb_move(dst + (size_t)dx * fb->bytes,
                    src + (size_t)sx * fb->bytes, fb->bytes);
            }
        }
        return;
    }
    for (uint32_t row = 0u; row < h; ++row) {
        size_t source_offset = (size_t)(y + row) * fb->pitch
            + (size_t)x * fb->bytes;
        size_t target_offset = (size_t)(y + row) * fb->scanout_pitch
            + (size_t)x * fb->bytes;

        fb_move(fb->front + target_offset, fb->draw + source_offset,
            (size_t)w * fb->bytes);
    }
}

void fb_present(struct fb *fb)
{
    if (fb_usable(fb)) {
        fb_present_rect(fb, 0, 0, fb->width, fb->height);
    }
}

uint32_t fb_pack(const struct fb *fb, uint32_t rgb)
{
    const struct fb_format *f = &fb->format;

    return (((rgb >> 16) & 0xffu) >> (8u - f->red_size)) << f->red_pos
        | (((rgb >> 8) & 0xffu) >> (8u - f->green_size)) << f->green_pos
        | ((rgb & 0xffu) >> (8u - f->blue_size)) << f->blue_pos;
}

void fb_put_pixel(struct fb *fb, int32_t x, int32_t y, uint32_t color)
{
    if (!fb_usable(fb) || x < 0 || y < 0 || (uint32_t)x >= fb->width
        || (uint32_t)y >= fb->height) {
        return;
    }
    fb_store(fb_row(fb, (uint32_t)y) + (size_t)x * fb->bytes, color,
        fb->bytes);
}

void fb_hline(struct fb *fb, int32_t x, int32_t y, uint32_t length,
    uint32_t color)
{
    fb_fill_rect(fb, x, y, length, 1u, color);
}

void fb_vline(struct fb *fb, int32_t x, int32_t y, uint32_t length,
    uint32_t color)
{
    fb_fill_rect(fb, x, y, 1u, length, color);
}

void fb_fill_rect(struct fb *fb, int32_t x, int32_t y, uint32_t w, uint32_t h,
    uint32_t color)
{
    if (!fb_usable(fb) || !fb_clip(fb, &x, &y, &w, &h)) {
        return;
    }
    for (uint32_t row = 0u; row < h; ++row) {
        fb_fill_span(fb_row(fb, (uint32_t)y + row) + (size_t)x * fb->bytes,
            w, color, fb->bytes);
    }
}

void fb_draw_rect(struct fb *fb, int32_t x, int32_t y, uint32_t w, uint32_t h,
    uint32_t color)
{
    if (w == 0u || h == 0u) {
        return;
    }
    fb_hline(fb, x, y, w, color);
    fb_hline(fb, x, y + (int32_t)h - 1, w, color);
    fb_vline(fb, x, y, h, color);
    fb_vline(fb, x + (int32_t)w - 1, y, h, color);
}

void fb_clear(struct fb *fb, uint32_t color)
{
    if (fb_usable(fb)) {
        fb_fill_rect(fb, 0, 0, fb->width, fb->height, color);
    }
}

void fb_blit(struct fb *fb, int32_t x, int32_t y, const struct fb_image *src)
{
    int32_t cx = x, cy = y;
    uint32_t w, h;

    if (!fb_usable(fb) || src == NULL || src->pixels == NULL) {
        return;
    }
    w = src->width;
    h = src->height;
    if (!fb_clip(fb, &cx, &cy, &w, &h)) {
        return;
    }
    for (uint32_t row = 0u; row < h; ++row) {
        const uint8_t *from = (const uint8_t *)src->pixels
            + (size_t)(cy - y + row) * src->pitch
            + (size_t)(cx - x) * fb->bytes;

        fb_move(fb_row(fb, (uint32_t)cy + row) + (size_t)cx * fb->bytes,
            from, (size_t)w * fb->bytes);
    }
}

void fb_blit_rgb(struct fb *fb, int32_t x, int32_t y, const uint32_t *src,
    uint32_t w, uint32_t h, uint32_t pitch, bool use_key, uint32_t key)
{
    int32_t cx = x, cy = y;
    uint32_t cw = w, ch = h;

    if (!fb_usable(fb) || src == NULL
        || !fb_clip(fb, &cx, &cy, &cw, &ch)) {
        return;
    }
    key &= 0xffffffu;
    for (uint32_t row = 0u; row < ch; ++row) {
        const uint32_t *from = (const uint32_t *)((const uint8_t *)src
            + (size_t)(cy - y + row) * pitch) + (cx - x);
        uint8_t *to = fb_row(fb, (uint32_t)cy + row)
            + (size_t)cx * fb->bytes;

        for (uint32_t col = 0u; col < cw; ++col, to += fb->bytes) {
            uint32_t rgb = from[col] & 0xffffffu;

            if (!use_key || rgb != key) {
                fb_store(to, fb_pack(fb, rgb), fb->bytes);
            }
        }
    }
}

void fb_copy_rect(struct fb *fb, int32_t dx, int32_t dy, int32_t sx,
    int32_t sy, uint32_t w, uint32_t h)
{
    int64_t width = w, height = h;

    if (!fb_usable(fb)) {
        return;
    }
    if (sx < 0) { dx -= sx; width += sx; sx = 0; }
    if (sy < 0) { dy -= sy; height += sy; sy = 0; }
    if (dx < 0) { sx -= dx; width += dx; dx = 0; }
    if (dy < 0) { sy -= dy; height += dy; dy = 0; }
    if (width > (int64_t)fb->width - sx) { width = (int64_t)fb->width - sx; }
    if (width > (int64_t)fb->width - dx) { width = (int64_t)fb->width - dx; }
    if (height > (int64_t)fb->height - sy) { height = (int64_t)fb->height - sy; }
    if (height > (int64_t)fb->height - dy) { height = (int64_t)fb->height - dy; }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int64_t i = 0; i < height; ++i) {
        int64_t row = dy > sy ? height - 1 - i : i;

        fb_move(fb_row(fb, (uint32_t)(dy + row)) + (size_t)dx * fb->bytes,
            fb_row(fb, (uint32_t)(sy + row)) + (size_t)sx * fb->bytes,
            (size_t)width * fb->bytes);
    }
}

void fb_scroll_up(struct fb *fb, uint32_t lines, uint32_t color)
{
    if (!fb_usable(fb) || lines == 0u) {
        return;
    }
    if (lines >= fb->height) {
        fb_clear(fb, color);
        return;
    }
    fb_move(fb->draw, fb->draw + (size_t)lines * fb->pitch,
        (size_t)(fb->height - lines) * fb->pitch);
    fb_fill_rect(fb, 0, (int32_t)(fb->height - lines), fb->width, lines,
        color);
}

static const uint8_t *fb_glyph(char c)
{
    uint8_t code = (uint8_t)c;

    if (code < FB_FONT_FIRST || code >= FB_FONT_FIRST + FB_FONT_GLYPHS) {
        code = '?';
    }
    return fb_font8x16[code - FB_FONT_FIRST];
}

static void fb_glyph_draw(struct fb *fb, int32_t x, int32_t y, char c,
    uint32_t fg, uint32_t bg, bool opaque)
{
    const uint8_t *glyph = fb_glyph(c);
    int32_t cx = x, cy = y;
    uint32_t w = FB_FONT_WIDTH, h = FB_FONT_HEIGHT;

    if (!fb_usable(fb) || !fb_clip(fb, &cx, &cy, &w, &h)) {
        return;
    }
    for (uint32_t row = 0u; row < h; ++row) {
        uint8_t bits = glyph[(uint32_t)(cy - y) + row];
        uint8_t *p = fb_row(fb, (uint32_t)cy + row) + (size_t)cx * fb->bytes;

        for (uint32_t col = 0u; col < w; ++col, p += fb->bytes) {
            if ((bits >> (7u - ((uint32_t)(cx - x) + col))) & 1u) {
                fb_store(p, fg, fb->bytes);
            } else if (opaque) {
                fb_store(p, bg, fb->bytes);
            }
        }
    }
}

void fb_draw_char(struct fb *fb, int32_t x, int32_t y, char c, uint32_t fg,
    uint32_t bg)
{
    fb_glyph_draw(fb, x, y, c, fg, bg, true);
}

void fb_draw_char_fg(struct fb *fb, int32_t x, int32_t y, char c, uint32_t fg)
{
    fb_glyph_draw(fb, x, y, c, fg, 0u, false);
}

void fb_draw_string(struct fb *fb, int32_t x, int32_t y, const char *text,
    uint32_t fg, uint32_t bg)
{
    int32_t pen = x;

    for (; text != NULL && *text != '\0'; ++text) {
        if (*text == '\n') {
            y += (int32_t)FB_FONT_HEIGHT;
            pen = x;
        } else if (*text == '\r') {
            pen = x;
        } else {
            fb_draw_char(fb, pen, y, *text, fg, bg);
            pen += (int32_t)FB_FONT_WIDTH;
        }
    }
}

void fb_console_init(struct fb_console *con, struct fb *fb, uint32_t fg,
    uint32_t bg)
{
    con->fb = fb;
    con->columns = fb_usable(fb) ? fb->width / FB_FONT_WIDTH : 0u;
    con->rows = fb_usable(fb) ? fb->height / FB_FONT_HEIGHT : 0u;
    con->col = 0u;
    con->row = 0u;
    con->fg = fg;
    con->bg = bg;
}

void fb_console_clear(struct fb_console *con)
{
    fb_clear(con->fb, con->bg);
    con->col = 0u;
    con->row = 0u;
}

static void fb_console_newline(struct fb_console *con)
{
    con->col = 0u;
    if (++con->row >= con->rows) {
        fb_scroll_up(con->fb, FB_FONT_HEIGHT, con->bg);
        con->row = con->rows - 1u;
    }
}

void fb_console_putc(struct fb_console *con, char c)
{
    if (con->columns == 0u || con->rows == 0u) {
        return;
    }
    switch (c) {
    case '\n':
        fb_console_newline(con);
        return;
    case '\r':
        con->col = 0u;
        return;
    case '\b':
        if (con->col != 0u) {
            --con->col;
            fb_draw_char(con->fb, (int32_t)(con->col * FB_FONT_WIDTH),
                (int32_t)(con->row * FB_FONT_HEIGHT), ' ', con->fg, con->bg);
        }
        return;
    case '\t':
        con->col = (con->col + 8u) & ~7u;
        if (con->col >= con->columns) {
            fb_console_newline(con);
        }
        return;
    default:
        break;
    }
    fb_draw_char(con->fb, (int32_t)(con->col * FB_FONT_WIDTH),
        (int32_t)(con->row * FB_FONT_HEIGHT), c, con->fg, con->bg);
    if (++con->col >= con->columns) {
        fb_console_newline(con);
    }
}

void fb_console_write(struct fb_console *con, const char *text)
{
    for (; text != NULL && *text != '\0'; ++text) {
        fb_console_putc(con, *text);
    }
}
