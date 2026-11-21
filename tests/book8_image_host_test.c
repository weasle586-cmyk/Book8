#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/apps/book8_image.h"

static void put_u16(uint8_t *data, uint32_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
}

static void put_u32(uint8_t *data, uint32_t offset, uint32_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
    data[offset + 2u] = (uint8_t)(value >> 16u);
    data[offset + 3u] = (uint8_t)(value >> 24u);
}

static uint32_t make_bmp(uint8_t *data, uint32_t width, uint32_t height)
{
    uint32_t row = (width * 3u + 3u) & ~3u;
    uint32_t size = 54u + row * height;
    memset(data, 0, size);
    data[0] = 'B';
    data[1] = 'M';
    put_u32(data, 2u, size);
    put_u32(data, 10u, 54u);
    put_u32(data, 14u, 40u);
    put_u32(data, 18u, width);
    put_u32(data, 22u, height);
    put_u16(data, 26u, 1u);
    put_u16(data, 28u, 24u);
    put_u32(data, 34u, row * height);
    if (width == 1u && height == 1u) {
        data[54] = 0x33u;
        data[55] = 0x22u;
        data[56] = 0x11u;
    }
    return size;
}

int main(void)
{
    uint8_t bmp[54u + 1028u * 3u];
    static uint8_t isolated_pool_memory[4096];
    struct book8_image_pool isolated_pool = {
        .memory = isolated_pool_memory,
        .capacity = sizeof(isolated_pool_memory)
    };
    struct book8_decoded_image image;
    uint32_t size = make_bmp(bmp, 1u, 1u);
    assert(book8_image_decode(bmp, size, &image));
    assert(image.width == 1u && image.height == 1u);
    assert(image.rgba[0] == 0x11u && image.rgba[1] == 0x22u
        && image.rgba[2] == 0x33u && image.rgba[3] == 0xffu);
    uint8_t *default_pixels = image.rgba;
    assert(book8_image_decode_with_pool(bmp, size, &image, &isolated_pool));
    assert(image.rgba != default_pixels);
    assert(default_pixels[0] == 0x11u && default_pixels[1] == 0x22u
        && default_pixels[2] == 0x33u && default_pixels[3] == 0xffu);
    book8_image_pool_release(&isolated_pool);
    book8_image_release();

    size = make_bmp(bmp, BOOK8_IMAGE_MAX_WIDTH + 1u, 1u);
    assert(!book8_image_decode(bmp, size, &image));
    assert(image.rgba == NULL);
    assert(!book8_image_decode(bmp, 10u, &image));
    assert(!book8_image_decode(NULL, size, &image));
    assert(!book8_image_decode(bmp, 4u * 1024u * 1024u + 1u, &image));
    assert(!book8_image_decode(bmp, size, NULL));
    return 0;
}
