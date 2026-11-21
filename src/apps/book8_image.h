#ifndef BOOK8_IMAGE_H
#define BOOK8_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#define BOOK8_IMAGE_MAX_WIDTH 1024u
#define BOOK8_IMAGE_MAX_HEIGHT 768u
#define BOOK8_IMAGE_MAX_PIXELS (BOOK8_IMAGE_MAX_WIDTH * BOOK8_IMAGE_MAX_HEIGHT)

struct book8_decoded_image {
    uint8_t *rgba;
    uint32_t width, height;
};

struct book8_image_pool {
    uint8_t *memory;
    uint32_t capacity;
    uint32_t used;
};

bool book8_image_decode(const uint8_t *data, uint32_t size,
    struct book8_decoded_image *image);
bool book8_image_decode_with_pool(const uint8_t *data, uint32_t size,
    struct book8_decoded_image *image, struct book8_image_pool *pool);
void book8_image_pool_release(struct book8_image_pool *pool);
uint8_t *book8_image_new(uint32_t width, uint32_t height, uint32_t color);
void book8_image_release(void);

#endif
