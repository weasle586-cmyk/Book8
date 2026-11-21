#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/fb.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static const struct fb_format fmt32 = {32, 16, 8, 8, 8, 0, 8};
static const struct fb_format fmt24 = {24, 16, 8, 8, 8, 0, 8};
static const struct fb_format fmt16 = {16, 11, 5, 5, 6, 0, 5};

static uint32_t pixel(const struct fb *fb, uint32_t x, uint32_t y)
{
    const uint8_t *p = fb->front + (size_t)y * fb->scanout_pitch
        + (size_t)x * fb->bytes;
    uint32_t value = 0u;

    for (uint8_t i = 0u; i < fb->bytes; ++i) {
        value |= (uint32_t)p[i] << (8u * i);
    }
    return value;
}

static void test_init_and_pack(void)
{
    static uint8_t mem[64 * 64 * 4];
    struct fb fb;
    struct fb_format bad = fmt32;

    CHECK(fb_init(&fb, mem, 64, 64, 256, &fmt32));
    CHECK(fb_pack(&fb, 0x123456u) == 0x123456u);
    CHECK(fb_init(&fb, mem, 64, 64, 192, &fmt24));
    CHECK(fb_pack(&fb, 0xffffffu) == 0xffffffu);
    CHECK(fb_init(&fb, mem, 64, 64, 128, &fmt16));
    CHECK(fb_pack(&fb, 0xffffffu) == 0xffffu);
    CHECK(fb_pack(&fb, 0xff0000u) == 0xf800u);
    CHECK(fb_pack(&fb, 0x0000ffu) == 0x001fu);

    CHECK(!fb_init(&fb, mem, 64, 64, 100, &fmt32));   /* pitch too small */
    CHECK(!fb_init(&fb, mem, UINT32_MAX, 1, UINT32_MAX - 3u, &fmt32));
    CHECK(!fb_init(&fb, NULL, 64, 64, 256, &fmt32));
    CHECK(!fb_init(&fb, mem, 0, 64, 256, &fmt32));
    bad.bpp = 8;
    CHECK(!fb_init(&fb, mem, 64, 64, 256, &bad));
    bad = fmt32;
    bad.red_pos = 28;                                  /* overflows bpp */
    CHECK(!fb_init(&fb, mem, 64, 64, 256, &bad));
    fb_put_pixel(&fb, 1, 1, 1u);                       /* unusable: no-op */
}

static void test_primitives(const struct fb_format *format, uint32_t pitch)
{
    static uint8_t mem[40 * 176];
    struct fb fb;
    uint32_t c, bg;

    memset(mem, 0, sizeof(mem));
    CHECK(fb_init(&fb, mem, 40, 40, pitch, format));
    c = fb_pack(&fb, 0xffffffu);
    bg = fb_pack(&fb, 0x000000u);

    fb_put_pixel(&fb, 3, 4, c);
    CHECK(pixel(&fb, 3, 4) == c);
    fb_put_pixel(&fb, -1, 0, c);
    fb_put_pixel(&fb, 40, 0, c);
    fb_put_pixel(&fb, 0, 40, c);

    fb_fill_rect(&fb, -5, -5, 10, 10, c);              /* clipped corner */
    CHECK(pixel(&fb, 0, 0) == c && pixel(&fb, 4, 4) == c);
    CHECK(pixel(&fb, 5, 5) == bg && pixel(&fb, 5, 0) == bg);

    fb_fill_rect(&fb, 30, 30, 100, 100, c);            /* clipped far edge */
    CHECK(pixel(&fb, 39, 39) == c && pixel(&fb, 29, 30) == bg);

    fb_clear(&fb, bg);
    fb_hline(&fb, 5, 7, 10, c);
    CHECK(pixel(&fb, 5, 7) == c && pixel(&fb, 14, 7) == c);
    CHECK(pixel(&fb, 15, 7) == bg && pixel(&fb, 4, 7) == bg);
    fb_vline(&fb, 9, 10, 5, c);
    CHECK(pixel(&fb, 9, 10) == c && pixel(&fb, 9, 14) == c
        && pixel(&fb, 9, 15) == bg);

    fb_clear(&fb, bg);
    fb_draw_rect(&fb, 2, 2, 6, 4, c);
    CHECK(pixel(&fb, 2, 2) == c && pixel(&fb, 7, 5) == c);
    CHECK(pixel(&fb, 4, 3) == bg && pixel(&fb, 4, 2) == c);

    /* Odd x start exercises the unaligned fill path. */
    fb_clear(&fb, bg);
    fb_fill_rect(&fb, 1, 1, 7, 2, c);
    CHECK(pixel(&fb, 1, 1) == c && pixel(&fb, 7, 2) == c
        && pixel(&fb, 8, 1) == bg && pixel(&fb, 0, 1) == bg);
}

static void test_blit_copy_scroll(void)
{
    static uint8_t mem[32 * 32 * 4];
    static uint32_t src[4 * 3];
    struct fb fb;
    struct fb_image image;
    uint32_t red, blue;

    CHECK(fb_init(&fb, mem, 32, 32, 128, &fmt32));
    red = fb_pack(&fb, 0xff0000u);
    blue = fb_pack(&fb, 0x0000ffu);

    for (int i = 0; i < 12; ++i) {
        src[i] = red;
    }
    image = (struct fb_image){src, 4, 3, 16};
    fb_blit(&fb, 10, 10, &image);
    CHECK(pixel(&fb, 10, 10) == red && pixel(&fb, 13, 12) == red);
    CHECK(pixel(&fb, 14, 10) == 0u && pixel(&fb, 10, 13) == 0u);
    fb_blit(&fb, -2, -1, &image);                      /* clipped */
    CHECK(pixel(&fb, 0, 0) == red && pixel(&fb, 1, 1) == red);
    CHECK(pixel(&fb, 2, 0) == 0u && pixel(&fb, 0, 2) == 0u);

    memset(mem, 0, sizeof(mem));
    for (int i = 0; i < 12; ++i) {
        src[i] = i == 5 ? 0x0000ffu : 0xff0000u;
    }
    fb_blit_rgb(&fb, 4, 4, src, 4, 3, 16, true, 0x0000ffu);
    CHECK(pixel(&fb, 4, 4) == red && pixel(&fb, 5, 5) == 0u);
    fb_blit_rgb(&fb, 4, 8, src, 4, 3, 16, false, 0);
    CHECK(pixel(&fb, 5, 9) == blue);

    /* Overlapping copy in both directions. */
    memset(mem, 0, sizeof(mem));
    fb_fill_rect(&fb, 0, 0, 8, 8, red);
    fb_copy_rect(&fb, 4, 4, 0, 0, 8, 8);               /* down-right */
    CHECK(pixel(&fb, 11, 11) == red && pixel(&fb, 4, 4) == red);
    CHECK(pixel(&fb, 3, 3) == red && pixel(&fb, 12, 12) == 0u);
    memset(mem, 0, sizeof(mem));
    fb_fill_rect(&fb, 4, 4, 8, 8, blue);
    fb_copy_rect(&fb, 0, 0, 4, 4, 8, 8);               /* up-left */
    CHECK(pixel(&fb, 0, 0) == blue && pixel(&fb, 7, 7) == blue);
    CHECK(pixel(&fb, 8, 8) == blue && pixel(&fb, 12, 12) == 0u);

    memset(mem, 0, sizeof(mem));
    fb_fill_rect(&fb, 0, 16, 32, 4, red);
    fb_scroll_up(&fb, 16, blue);
    CHECK(pixel(&fb, 5, 0) == red && pixel(&fb, 5, 3) == red);
    CHECK(pixel(&fb, 5, 4) == 0u && pixel(&fb, 5, 16) == blue
        && pixel(&fb, 31, 31) == blue);
    fb_scroll_up(&fb, 1000, red);
    CHECK(pixel(&fb, 0, 0) == red && pixel(&fb, 31, 31) == red);
}

static void test_backbuffer(void)
{
    static uint8_t front[16 * 16 * 4], back[16 * 16 * 4];
    struct fb fb;
    uint32_t c;

    CHECK(fb_init(&fb, front, 16, 16, 64, &fmt32));
    c = fb_pack(&fb, 0x00ff00u);
    fb_put_pixel(&fb, 1, 1, c);
    fb_set_backbuffer(&fb, back);
    CHECK(((uint32_t *)back)[17] == c);                /* seeded from front */
    fb_fill_rect(&fb, 8, 8, 4, 4, c);
    CHECK(pixel(&fb, 8, 8) == 0u);                     /* not yet visible */
    fb_present_rect(&fb, 0, 0, 8, 8);
    CHECK(pixel(&fb, 8, 8) == 0u);
    fb_present(&fb);
    CHECK(pixel(&fb, 8, 8) == c && pixel(&fb, 11, 11) == c);
    fb_set_backbuffer(&fb, NULL);
    CHECK(fb.draw == fb.front);
}

static void test_padded_pitch_present(void)
{
    static uint8_t front[2 * 12], back[2 * 12];
    struct fb fb;
    uint32_t green;

    memset(front, 0, sizeof(front));
    memset(back, 0, sizeof(back));
    CHECK(fb_init(&fb, front, 2, 2, 12, &fmt32));
    fb_set_backbuffer(&fb, back);
    green = fb_pack(&fb, 0x00ff00u);
    fb_put_pixel(&fb, 1, 1, green);
    fb_present_rect(&fb, 1, 1, 1, 1);
    CHECK(pixel(&fb, 1, 1) == green);
    CHECK(pixel(&fb, 0, 1) == 0u && pixel(&fb, 1, 0) == 0u);
    CHECK(front[8] == 0u && front[9] == 0u
        && front[10] == 0u && front[11] == 0u);
}

static void test_logical_mode_scaling(void)
{
    static uint8_t front[2 * 12], back[4 * 4 * 4];
    struct fb fb;
    uint32_t red, green, blue, white;

    memset(front, 0, sizeof(front));
    memset(back, 0, sizeof(back));
    CHECK(fb_init(&fb, front, 2u, 2u, 12u, &fmt32));
    red = fb_pack(&fb, 0xff0000u);
    green = fb_pack(&fb, 0x00ff00u);
    blue = fb_pack(&fb, 0x0000ffu);
    white = fb_pack(&fb, 0xffffffu);
    fb_put_pixel(&fb, 0, 0, red);
    fb_put_pixel(&fb, 1, 0, green);
    fb_put_pixel(&fb, 0, 1, blue);
    fb_put_pixel(&fb, 1, 1, white);

    CHECK(fb_set_logical_mode(&fb, 4u, 4u, back, sizeof(back)));
    CHECK(fb.width == 4u && fb.height == 4u && fb.pitch == 16u);
    CHECK(fb.scanout_width == 2u && fb.scanout_height == 2u
        && fb.scanout_pitch == 12u);
    CHECK(((uint32_t *)fb.draw)[0] == red);
    CHECK(((uint32_t *)fb.draw)[2] == green);
    CHECK(((uint32_t *)fb.draw)[8] == blue);
    CHECK(((uint32_t *)fb.draw)[10] == white);
    fb_fill_rect(&fb, 0, 0, 4u, 4u, red);
    fb_put_pixel(&fb, 2, 2, blue);
    fb_present_rect(&fb, 2, 2, 1u, 1u);
    CHECK(pixel(&fb, 0u, 0u) == red && pixel(&fb, 1u, 1u) == blue
        && pixel(&fb, 0u, 1u) == blue && pixel(&fb, 1u, 0u) == green);
    CHECK(front[8] == 0u && front[9] == 0u
        && front[10] == 0u && front[11] == 0u);

    CHECK(!fb_set_logical_mode(&fb, 100u, 100u, back, sizeof(back)));
    CHECK(fb.width == 4u && fb.height == 4u && fb.pitch == 16u);
    fb_set_backbuffer(&fb, NULL);
    CHECK(fb.width == 2u && fb.height == 2u && fb.pitch == 12u);
    CHECK(fb.draw == fb.front);
}

static void test_text(void)
{
    static uint8_t mem[80 * 48 * 4];
    struct fb fb;
    struct fb_console con;
    uint32_t fg, bg, lit = 0u;

    CHECK(fb_init(&fb, mem, 80, 48, 320, &fmt32));
    fg = fb_pack(&fb, 0xffffffu);
    bg = fb_pack(&fb, 0x000040u);

    fb_clear(&fb, bg);
    fb_draw_char(&fb, 0, 0, 'A', fg, bg);
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            lit += pixel(&fb, x, y) == fg;
            CHECK(pixel(&fb, x, y) == fg || pixel(&fb, x, y) == bg);
        }
    }
    CHECK(lit > 10 && lit < 100);

    fb_clear(&fb, bg);
    fb_draw_char_fg(&fb, 0, 0, 'A', fg);               /* transparent bg */
    CHECK(pixel(&fb, 0, 0) == bg);
    fb_draw_char(&fb, 76, 40, 'W', fg, bg);            /* clipped glyph */
    fb_draw_char(&fb, -4, -8, 'W', fg, bg);
    fb_draw_char(&fb, 0, 16, '\x01', fg, bg);          /* falls back to '?' */
    {
        uint32_t q = 0u;

        for (uint32_t x = 0; x < 8; ++x) {
            for (uint32_t y = 16; y < 32; ++y) {
                q += pixel(&fb, x, y) == fg;
            }
        }
        CHECK(q > 0u);
    }

    fb_console_init(&con, &fb, fg, bg);
    CHECK(con.columns == 10u && con.rows == 3u);
    fb_console_clear(&con);
    fb_console_write(&con, "A:\\>");
    CHECK(con.col == 4u && con.row == 0u);
    fb_console_putc(&con, '\b');
    CHECK(con.col == 3u);
    fb_console_putc(&con, '\t');
    CHECK(con.col == 8u);
    fb_console_write(&con, "abc");                     /* wraps at col 10 */
    CHECK(con.row == 1u && con.col == 1u);
    fb_console_write(&con, "\n\n");                    /* scrolls */
    CHECK(con.row == 2u && con.col == 0u);
    fb_console_putc(&con, '\r');
    CHECK(con.col == 0u);
}

int main(void)
{
    test_init_and_pack();
    test_primitives(&fmt32, 160);
    test_primitives(&fmt32, 168);                       /* padded pitch */
    test_primitives(&fmt24, 120);
    test_primitives(&fmt24, 123);                       /* unaligned rows */
    test_primitives(&fmt16, 80);
    test_blit_copy_scroll();
    test_backbuffer();
    test_padded_pitch_present();
    test_logical_mode_scaling();
    test_text();
    if (failures != 0) {
        printf("fb host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("fb host test: PASS\n");
    return 0;
}
