#include "book8_image.h"

#include <stddef.h>
#include "../../include/string.h"

#define IMAGE_POOL_SIZE (8u * 1024u * 1024u)

static uint8_t image_pool[IMAGE_POOL_SIZE] __attribute__((aligned(16)));
static struct book8_image_pool default_image_pool = {
    .memory = image_pool,
    .capacity = IMAGE_POOL_SIZE
};
static struct book8_image_pool *active_image_pool = &default_image_pool;

static void *image_alloc(size_t size)
{
    if (active_image_pool == NULL
        || active_image_pool->used > active_image_pool->capacity) {
        return NULL;
    }
    if (size > SIZE_MAX - 15u) {
        return NULL;
    }
    size = (size + 15u) & ~(size_t)15u;
    if (size > active_image_pool->capacity - active_image_pool->used) {
        return NULL;
    }
    void *result = active_image_pool->memory + active_image_pool->used;
    active_image_pool->used += (uint32_t)size;
    return result;
}

static void *image_realloc(void *old, size_t old_size, size_t new_size)
{
    if (old == NULL) {
        return image_alloc(new_size);
    }
    void *result = image_alloc(new_size);
    if (result != NULL && old_size != 0u) {
        size_t copy = old_size < new_size ? old_size : new_size;
        memcpy(result, old, copy);
    }
    return result;
}

static void image_free(void *pointer)
{
    (void)pointer;
}

#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_ASSERT(condition) ((void)0)
#define STBI_MALLOC(size) image_alloc(size)
#define STBI_REALLOC(pointer, size) image_realloc(pointer, 0u, size)
#define STBI_REALLOC_SIZED(pointer, old_size, new_size) \
    image_realloc(pointer, old_size, new_size)
#define STBI_FREE(pointer) image_free(pointer)
#define STB_IMAGE_IMPLEMENTATION
#include "../../lib/stb_image.h"

bool book8_image_decode_with_pool(const uint8_t *data, uint32_t size,
    struct book8_decoded_image *image, struct book8_image_pool *pool)
{
    if (image == NULL) {
        return false;
    }
    *image = (struct book8_decoded_image){0};
    if (pool == NULL || pool->memory == NULL || pool->capacity == 0u) {
        return false;
    }
    pool->used = 0u;
    if (data == NULL || size == 0u || size > 4u * 1024u * 1024u) {
        return false;
    }
    active_image_pool = pool;
    int width = 0, height = 0, channels = 0;
    uint8_t *rgba = stbi_load_from_memory(data, (int)size, &width, &height,
        &channels, 4);
    active_image_pool = &default_image_pool;
    if (rgba == NULL || width <= 0 || height <= 0
        || (uint32_t)width > BOOK8_IMAGE_MAX_WIDTH
        || (uint32_t)height > BOOK8_IMAGE_MAX_HEIGHT
        || (uint64_t)(uint32_t)width * (uint32_t)height
            > BOOK8_IMAGE_MAX_PIXELS) {
        if (rgba != NULL) {
            image_free(rgba);
        }
        pool->used = 0u;
        return false;
    }
    *image = (struct book8_decoded_image){
        .rgba = rgba,
        .width = (uint32_t)width,
        .height = (uint32_t)height
    };
    return true;
}

bool book8_image_decode(const uint8_t *data, uint32_t size,
    struct book8_decoded_image *image)
{
    return book8_image_decode_with_pool(data, size, image,
        &default_image_pool);
}

void book8_image_pool_release(struct book8_image_pool *pool)
{
    if (pool != NULL) {
        pool->used = 0u;
    }
}

uint8_t *book8_image_new(uint32_t width, uint32_t height, uint32_t color)
{
    uint64_t bytes = (uint64_t)width * height * 4u;
    if (width == 0u || height == 0u
        || width > BOOK8_IMAGE_MAX_WIDTH || height > BOOK8_IMAGE_MAX_HEIGHT
        || bytes > IMAGE_POOL_SIZE) {
        return NULL;
    }
    active_image_pool = &default_image_pool;
    default_image_pool.used = 0u;
    uint8_t *pixels = image_alloc((size_t)bytes);
    if (pixels == NULL) {
        return NULL;
    }
    for (uint64_t i = 0u; i < (uint64_t)width * height; ++i) {
        pixels[i * 4u] = (uint8_t)(color >> 16u);
        pixels[i * 4u + 1u] = (uint8_t)(color >> 8u);
        pixels[i * 4u + 2u] = (uint8_t)color;
        pixels[i * 4u + 3u] = 0xffu;
    }
    return pixels;
}

void book8_image_release(void)
{
    active_image_pool = &default_image_pool;
    default_image_pool.used = 0u;
}
