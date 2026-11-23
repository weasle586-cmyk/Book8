#ifndef BOOK8_HDA_MODULE_H
#define BOOK8_HDA_MODULE_H

#include "hda_stream.h"
#include "../include/b8_driver.h"

#define B8_HDA_MODULE_API_VERSION 1u
#define B8_HDA_IOCTL_GET_API 1u
#define B8_HDA_IOCTL_GET_PCM_WINDOW 2u

/* GUI visualization hook data. This is an ioctl payload, not an extension of
 * the versioned b8_hda_module_api struct above. */
struct b8_hda_pcm_window {
    const uint8_t *pcm;
    uint32_t size;
    uint64_t position_bytes;
};

struct b8_hda_module_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void (*get_device)(struct hda_device *device);
    bool (*is_available)(void);
    uint8_t (*get_volume)(void);
    bool (*get_mute)(void);
    int32_t (*set_volume)(uint8_t percent);
    int32_t (*set_mute)(bool muted);
    int32_t (*play_wav)(const uint8_t *wav, uint32_t size);
    int32_t (*pause)(bool paused);
    void (*stop)(void);
    int32_t (*seek)(uint32_t position_ms);
    void (*get_stream_info)(struct hda_stream_info *info);
    int32_t (*service)(void);
    const char *(*status_name)(int32_t status);
    const char *(*stream_result_name)(int32_t result);
};

#endif
