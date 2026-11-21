#ifndef BOOK8_DRIVER_LOADER_H
#define BOOK8_DRIVER_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"

#define B8_DRIVER_MAX_PROVIDERS 32u

struct b8_loaded_driver {
    const b8_driver_t *descriptor;
    void *image;
    uint32_t image_pages;
    bool initialized;
};

int32_t b8_driver_load(const char *path, const uint8_t expected_digest[32],
    const b8_sym_export_t *providers, uint32_t provider_count,
    struct b8_loaded_driver *loaded);
void b8_driver_unload(struct b8_loaded_driver *loaded);
int32_t b8_driver_probe(struct b8_loaded_driver *loaded,
    const struct b8_driver_device *device, void *state);
void b8_driver_remove(struct b8_loaded_driver *loaded,
    const struct b8_driver_device *device, void *state);

#endif
