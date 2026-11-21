#ifndef BOOK8_HDA_STREAM_H
#define BOOK8_HDA_STREAM_H

#include "hda.h"

#define HDA_STREAM_MAX_PCM_BYTES (2u * 1024u * 1024u)
#define HDA_STREAM_MAX_WAV_BYTES (2u * 1024u * 1024u)

enum hda_stream_result {
    HDA_STREAM_OK = 0,
    HDA_STREAM_BAD_ARGUMENT,
    HDA_STREAM_UNAVAILABLE,
    HDA_STREAM_INVALID_WAV,
    HDA_STREAM_UNSUPPORTED_FORMAT,
    HDA_STREAM_TOO_LARGE,
    HDA_STREAM_ENGINE_ERROR,
    HDA_STREAM_UNDERRUN
};

struct hda_stream {
    uint8_t pcm[HDA_STREAM_MAX_PCM_BYTES];
    uint32_t pcm_size;
    uint32_t next_pcm_offset;
    uint32_t played_bytes;
    uint32_t total_position;
    uint32_t last_lpib;
    bool loaded;
    bool active;
    bool paused;
    bool failed;
};

struct hda_stream_info {
    uint32_t position_ms;
    uint32_t duration_ms;
    bool loaded;
    bool playing;
    bool paused;
    bool failed;
};

enum hda_stream_result hda_stream_play_wav(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, const uint8_t *wav, uint32_t wav_size);
enum hda_stream_result hda_stream_service(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream);
enum hda_stream_result hda_stream_pause(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, bool paused);
enum hda_stream_result hda_stream_seek(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, uint32_t position_ms);
void hda_stream_stop(struct hda_device *device, struct hda_stream *stream);
void hda_stream_get_info(const struct hda_stream *stream,
    struct hda_stream_info *info);
const char *hda_stream_result_name(enum hda_stream_result result);

#endif
