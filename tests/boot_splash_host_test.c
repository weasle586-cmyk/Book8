/* Host validation for the pre-rendered flat boot splash asset. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_BYTES 16u

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > HEADER_BYTES && length <= 4 * 1024 * 1024);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (uint32_t)length;
    return data;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/book8-splash.b8s";
    uint32_t size = 0u;
    uint8_t *data = read_file(path, &size);

    assert(memcmp(data, "B8SPLASH", 8u) == 0);
    uint32_t width = (uint32_t)data[8u] | ((uint32_t)data[9u] << 8u)
        | ((uint32_t)data[10u] << 16u) | ((uint32_t)data[11u] << 24u);
    uint32_t height = (uint32_t)data[12u] | ((uint32_t)data[13u] << 8u)
        | ((uint32_t)data[14u] << 16u) | ((uint32_t)data[15u] << 24u);
    assert(width == 512u && height == 286u);
    assert(size == HEADER_BYTES + width * height * 4u);

    bool opaque = false;
    for (uint32_t i = HEADER_BYTES; i < size; i += 4u) {
        if (data[i + 3u] != 0u) {
            opaque = true;
            break;
        }
    }
    assert(opaque);

    free(data);
    printf("PASS: flat boot splash %ux%u, %u bytes, no runtime decoder\n",
        width, height, size);
    return 0;
}
