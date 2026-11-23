#include "kernel_internal.h"

/* Multiboot2 command-line parsing and boot splash playback. */

bool cmdline_has_option(const char *cmdline, uint32_t size,
    const char *option)
{
    uint32_t start = 0u;
    while (start < size && cmdline[start] != '\0') {
        while (start < size && cmdline[start] == ' ') {
            ++start;
        }
        uint32_t end = start;
        while (end < size && cmdline[end] != '\0' && cmdline[end] != ' ') {
            ++end;
        }
        uint32_t i = 0u;
        while (start + i < end && option[i] != '\0'
            && cmdline[start + i] == option[i]) {
            ++i;
        }
        if (option[i] == '\0' && start + i == end) {
            return true;
        }
        start = end;
    }
    return false;
}

uint32_t book8_splash_cycles_option(const char *cmdline, uint32_t size)
{
    static const char prefix[] = "book8.splash=";
    uint32_t start = 0u;
    while (start < size && cmdline[start] != '\0') {
        while (start < size && cmdline[start] == ' ') {
            ++start;
        }
        uint32_t end = start;
        while (end < size && cmdline[end] != '\0' && cmdline[end] != ' ') {
            ++end;
        }
        uint32_t matched = 0u;
        while (matched + 1u < sizeof(prefix) && start + matched < end
            && cmdline[start + matched] == prefix[matched]) {
            ++matched;
        }
        if (matched + 1u == sizeof(prefix) && start + matched < end) {
            uint32_t value = 0u;
            for (uint32_t i = start + matched; i < end; ++i) {
                if (cmdline[i] < '0' || cmdline[i] > '9') {
                    return 1u;
                }
                value = value * 10u + (uint32_t)(cmdline[i] - '0');
                if (value > 16u) {
                    value = 16u;
                }
            }
            return value;
        }
        start = end;
    }
    return 1u;
}

void boot_splash_delay_ms(uint32_t milliseconds)
{
    uint64_t start = read_tsc();
    uint64_t ticks = tsc_hz / 1000u * milliseconds;
    while (read_tsc() - start < ticks) {
        __asm__ volatile ("pause");
    }
}

void play_boot_splash(void)
{
    if (book8_splash_cycles == 0u) {
        serial_write("BOOK8-SPLASH: disabled by command line\r\n");
        return;
    }
    if (splash_image == NULL || splash_image_size < BOOK8_SPLASH_HEADER_BYTES) {
        serial_write("BOOK8-SPLASH: no usable module\r\n");
        return;
    }
    if (screen.pixels == NULL || tsc_hz == 0u) {
        serial_write("BOOK8-SPLASH: framebuffer or clock unavailable\r\n");
        return;
    }

    static const uint8_t magic[8u] = {
        'B', '8', 'S', 'P', 'L', 'A', 'S', 'H'
    };
    const uint8_t *data = splash_image;
    if (memcmp(data, magic, sizeof(magic)) != 0u) {
        serial_write("BOOK8-SPLASH: invalid image module\r\n");
        return;
    }
    uint32_t width = (uint32_t)data[8u]
        | ((uint32_t)data[9u] << 8u)
        | ((uint32_t)data[10u] << 16u)
        | ((uint32_t)data[11u] << 24u);
    uint32_t height = (uint32_t)data[12u]
        | ((uint32_t)data[13u] << 8u)
        | ((uint32_t)data[14u] << 16u)
        | ((uint32_t)data[15u] << 24u);
    uint64_t required = BOOK8_SPLASH_HEADER_BYTES
        + (uint64_t)width * (uint64_t)height * 4u;
    if (width == 0u || height == 0u
        || width > BOOK8_SPLASH_MAX_WIDTH
        || height > BOOK8_SPLASH_MAX_HEIGHT
        || required != splash_image_size) {
        serial_write("BOOK8-SPLASH: invalid image module\r\n");
        return;
    }
    const uint8_t *pixels = data + BOOK8_SPLASH_HEADER_BYTES;
    if (!boot_hash_verify_memory("SPLASH", data, splash_image_size)) {
        serial_write("BOOK8-HASH: splash verification failed\r\n");
        return;
    }

    serial_write("BOOK8-SPLASH: playing\r\n");
    for (uint32_t cycle = 0u; cycle < book8_splash_cycles; ++cycle) {
        gui_fill_rect(0u, 0u, screen.width, screen.height, 0u);
        uint32_t x = screen.width > width ? (screen.width - width) / 2u : 0u;
        uint32_t y = screen.height > height ? (screen.height - height) / 2u : 0u;
        gui_draw_rgba(x, y, width, height, pixels, width * 4u, 1u);
        gui_present();
        boot_splash_delay_ms(BOOK8_SPLASH_HOLD_MS);
    }
    serial_write("BOOK8-SPLASH: complete\r\n");
}
