#include "kernel_internal.h"

/* Serial and framebuffer console, text output and drawing primitives. */

void control_log_char(char c)
{
    if (c == '\r') { return; }
    if (c == '\n') {
        control_log_line = (control_log_line + 1u) % 8u;
        control_log_column = 0u;
        control_log[control_log_line][0] = '\0';
    } else if (control_log_column + 1u < sizeof(control_log[0])) {
        control_log[control_log_line][control_log_column++] = c;
        control_log[control_log_line][control_log_column] = '\0';
    }
}

void zero_memory(void *memory, size_t size)
{
    uint8_t *bytes = memory;
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0u;
    }
}

void copy_string(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0u) {
        return;
    }
    size_t i = 0u;
    while (i + 1u < capacity && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

bool ascii_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        char a = *left++;
        char b = *right++;
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (a != b) {
            return false;
        }
    }
    return *left == *right;
}

char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text;
}

void serial_init(void)
{
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x80u);
    outb(COM1, 0x03u);
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x03u);
    outb(COM1 + 2u, 0xc7u);
    outb(COM1 + 4u, 0x0bu);
    serial_available = inb(COM1 + 5u) != 0xffu;
}

bool serial_ready(void)
{
    return serial_available && (inb(COM1 + 5u) & 1u) != 0u;
}

void serial_putc(char value)
{
    book8_boot_log_char(value);
    if (!serial_available) {
        return;
    }
    for (uint32_t attempt = 0u; attempt < SERIAL_TX_WAIT_LIMIT; ++attempt) {
        if ((inb(COM1 + 5u) & 0x20u) != 0u) {
            outb(COM1, (uint8_t)value);
            return;
        }
        __asm__ volatile ("pause");
    }
}

void serial_write(const char *text)
{
    while (*text != '\0') {
        serial_putc(*text++);
    }
}

void book8_usb_debug_serial_putc(char value)
{
    serial_putc(value);
}

uint32_t pack_rgb(uint32_t color)
{
    return fb_pack(&fbdev, color);
}

long book8_framebuffer_info(struct book8_fb_info *info)
{
    if (screen.pixels == NULL) {
        return -BOOK8_ENODEV;
    }
    info->width = screen.width;
    info->height = screen.height;
    info->pitch = screen.pitch_bytes;
    info->bits_per_pixel = screen.bytes_per_pixel * 8u;
    info->red_position = screen.red_position;
    info->red_mask_size = screen.red_mask_size;
    info->green_position = screen.green_position;
    info->green_mask_size = screen.green_mask_size;
    info->blue_position = screen.blue_position;
    info->blue_mask_size = screen.blue_mask_size;
    info->reserved = 0u;
    return 0;
}

long book8_framebuffer_draw_pixel(uint64_t x, uint64_t y, uint32_t rgb)
{
    if (screen.pixels == NULL) {
        return -BOOK8_ENODEV;
    }
    if (x >= screen.width || y >= screen.height) {
        return -BOOK8_EINVAL;
    }
    fb_put_pixel(&fbdev, (int32_t)x, (int32_t)y, pack_rgb(rgb));
    return 0;
}

void draw_char_colored(char value, uint32_t x, uint32_t y,
    uint32_t foreground, uint32_t background)
{
    fb_draw_char(&fbdev, (int32_t)x, (int32_t)y, value,
        pack_rgb(foreground), pack_rgb(background));
}

void draw_char(char value, uint32_t x, uint32_t y)
{
    draw_char_colored(value, x, y, 0xffffffu, 0u);
}

void gui_fill_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t color)
{
    if (screen.pixels == NULL || x >= screen.width || y >= screen.height) {
        return;
    }
    if (fbdev.draw == fbdev.front && nv_graphics != NULL
        && nv_graphics->abi_version == 1u && nv_graphics->fill_rect != NULL
        && nv_graphics->fill_rect(&nv_gpu, x, y, width, height,
            pack_rgb(color)) == 0) {
            return;
    }
    fb_fill_rect(&fbdev, (int32_t)x, (int32_t)y, width, height,
        pack_rgb(color));
}

void gui_draw_rgba(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const uint8_t *pixels, uint32_t stride, uint32_t scale)
{
    if (screen.pixels == NULL || pixels == NULL || scale == 0u
        || width > 0xffffffffu / 4u
        || stride < width * 4u) {
        return;
    }
    for (uint32_t sy = 0u; sy < height; ++sy) {
        uint64_t dy = (uint64_t)y + (uint64_t)sy * scale;
        if (dy >= screen.height) {
            break;
        }
        const uint8_t *source = pixels + (uint64_t)sy * stride;
        for (uint32_t sx = 0u; sx < width; ++sx) {
            uint64_t dx = (uint64_t)x + (uint64_t)sx * scale;
            if (dx >= screen.width) {
                break;
            }
            const uint8_t *pixel = source + sx * 4u;
            uint32_t alpha = pixel[3];
            uint32_t red = (pixel[0] * alpha + 255u * (255u - alpha)) / 255u;
            uint32_t green = (pixel[1] * alpha + 255u * (255u - alpha)) / 255u;
            uint32_t blue = (pixel[2] * alpha + 255u * (255u - alpha)) / 255u;
            uint32_t packed = pack_rgb((red << 16u) | (green << 8u) | blue);
            for (uint32_t yy = 0u; yy < scale && dy + yy < screen.height; ++yy) {
                for (uint32_t xx = 0u; xx < scale && dx + xx < screen.width; ++xx) {
                    uint8_t *target = fbdev.draw
                        + (dy + yy) * fbdev.pitch
                        + (dx + xx) * fbdev.bytes;
                    for (uint32_t byte = 0u; byte < screen.bytes_per_pixel; ++byte) {
                        target[byte] = (uint8_t)(packed >> (byte * 8u));
                    }
                }
            }
        }
    }
}

void gui_draw_text(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background)
{
    while (*text != '\0' && x < screen.width) {
        if (*text == '\n') {
            y += CELL_HEIGHT;
            x = 0u;
        } else if (*text == '\r') {
            x = 0u;
        } else {
            draw_char_colored(*text, x, y, foreground, background);
            x += CELL_WIDTH;
        }
        ++text;
    }
}

void gui_draw_text_scaled(uint32_t x, uint32_t y, const char *text,
    uint32_t foreground, uint32_t background, uint32_t scale, bool bold)
{
    if (scale < 1u) {
        scale = 1u;
    } else if (scale > 4u) {
        scale = 4u;
    }
    uint32_t packed = pack_rgb(foreground);
    for (; *text != '\0' && x < screen.width; ++text, x += 8u * scale) {
        uint32_t code = (uint8_t)*text;
        if (code < FB_FONT_FIRST || code >= FB_FONT_FIRST + FB_FONT_GLYPHS) {
            code = '?';
        }
        const uint8_t *glyph = fb_font8x16[code - FB_FONT_FIRST];
        gui_fill_rect(x, y, 8u * scale, 16u * scale, background);
        for (uint32_t row = 0u; row < 16u; ++row) {
            uint32_t bits = glyph[row];
            if (bold) {
                bits |= bits >> 1u;
            }
            uint32_t col = 0u;
            while (col < 8u) {
                if (((bits >> (7u - col)) & 1u) == 0u) {
                    ++col;
                    continue;
                }
                uint32_t run = 1u;
                while (col + run < 8u
                    && ((bits >> (7u - (col + run))) & 1u) != 0u) {
                    ++run;
                }
                fb_fill_rect(&fbdev, (int32_t)(x + col * scale),
                    (int32_t)(y + row * scale), run * scale, scale, packed);
                col += run;
            }
        }
    }
}

bool gui_read_file(const char *path, void *destination,
    uint32_t capacity, uint32_t *size)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_read_file(path, destination, capacity, size);
}

bool gui_write_file(const char *path, const void *data, uint32_t size)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_write_file(path, data, size);
}

void scroll_screen(void)
{
    if (screen.pixels != NULL) {
        uint32_t rows = screen.height / CELL_HEIGHT;
        if (rows > 1u) {
            fb_scroll_up(&fbdev, CELL_HEIGHT, 0u);
            cursor_y = rows - 1u;
        }
    } else if (use_vga) {
        for (uint32_t i = 0; i < (VGA_ROWS - 1u) * VGA_COLUMNS; ++i) {
            VGA_MEMORY[i] = VGA_MEMORY[i + VGA_COLUMNS];
        }
        for (uint32_t i = (VGA_ROWS - 1u) * VGA_COLUMNS; i < VGA_ROWS * VGA_COLUMNS; ++i) {
            VGA_MEMORY[i] = 0x0720u;
        }
        cursor_y = VGA_ROWS - 1u;
    }
}

void console_clear(void)
{
    if (gui_active) {
        book8gui_console_clear();
        return;
    }
    if (screen.pixels != NULL) {
        fb_clear(&fbdev, 0u);
    } else if (use_vga) {
        for (uint32_t i = 0; i < VGA_COLUMNS * VGA_ROWS; ++i) {
            VGA_MEMORY[i] = 0x0720u;
        }
    }
    cursor_x = 0u;
    cursor_y = 0u;
    vga_cursor = 0u;
    if (!gui_active) {
        fb_present(&fbdev);
    }
}

void console_present(void)
{
    if (!gui_active) {
        fb_present(&fbdev);
    }
}

void console_putc(char value)
{
    if (startup_quiet && !gui_active) {
        if (value == '\n') {
            book8_boot_log_char('\r');
        }
        book8_boot_log_char(value);
        return;
    }

    if (value == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    } else {
        serial_putc(value);
    }

    if (gui_active) {
        book8gui_console_char(value);
        return;
    }

    if (value == '\n') {
        cursor_x = 0u;
        ++cursor_y;
    } else if (value == '\r') {
        cursor_x = 0u;
    } else if (value == '\b') {
        if (cursor_x > 0u) {
            --cursor_x;
            if (screen.pixels != NULL) {
                draw_char(' ', cursor_x * CELL_WIDTH, cursor_y * CELL_HEIGHT);
            } else if (use_vga) {
                vga_cursor = (uint16_t)(cursor_y * VGA_COLUMNS + cursor_x);
                VGA_MEMORY[vga_cursor] = 0x0720u;
            }
        }
    } else {
        if (screen.pixels != NULL) {
            draw_char(value, cursor_x * CELL_WIDTH, cursor_y * CELL_HEIGHT);
        } else if (use_vga) {
            vga_cursor = (uint16_t)(cursor_y * VGA_COLUMNS + cursor_x);
            VGA_MEMORY[vga_cursor] = (uint16_t)(0x0700u | (uint8_t)value);
        }
        ++cursor_x;
    }

    uint32_t max_columns = screen.pixels != NULL
        ? screen.width / CELL_WIDTH : (use_vga ? VGA_COLUMNS : 80u);
    uint32_t max_rows = screen.pixels != NULL
        ? screen.height / CELL_HEIGHT : (use_vga ? VGA_ROWS : 25u);
    if (max_columns == 0u || max_rows == 0u) {
        return;
    }
    if (cursor_x >= max_columns) {
        cursor_x = 0u;
        ++cursor_y;
    }
    if (cursor_y >= max_rows) {
        scroll_screen();
    }
    if (value == '\n' && !console_write_active) {
        console_present();
    }
}

void console_write(const char *text)
{
    bool has_newline = false;
    console_write_active = true;
    while (*text != '\0') {
        if (*text == '\n') {
            has_newline = true;
        }
        control_log_char(*text);
        console_putc(*text++);
    }
    console_write_active = false;
    if (has_newline) {
        console_present();
    }
}

void print_unsigned(uint64_t value)
{
    char digits[21];
    size_t count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count > 0u) {
        console_putc(digits[--count]);
    }
}

void print_signed(int32_t value)
{
    if (value < 0) {
        console_putc('-');
        print_unsigned((uint32_t)(-(value + 1)) + 1u);
    } else {
        print_unsigned((uint32_t)value);
    }
}

void print_hex8(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    console_putc(digits[value >> 4u]);
    console_putc(digits[value & 0x0fu]);
}

void print_hex32(uint32_t value)
{
    print_hex8((uint8_t)(value >> 24u));
    print_hex8((uint8_t)(value >> 16u));
    print_hex8((uint8_t)(value >> 8u));
    print_hex8((uint8_t)value);
}

void print_hex64(uint64_t value)
{
    print_hex32((uint32_t)(value >> 32u));
    print_hex32((uint32_t)value);
}

long book8_console_write(const char *buf, unsigned long len)
{
    unsigned long i;

    for (i = 0; i < len; ++i) {
        console_putc(buf[i]); /* console_putc already mirrors to serial */
    }
    return (long)len;
}
