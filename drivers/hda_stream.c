#include "hda_stream.h"

#include "../include/string.h"

#define HDA_SD_CTL 0x00u
#define HDA_SD_LPIB 0x04u
#define HDA_SD_STATUS 0x03u
#define HDA_SD_RUN 0x02u
#define HDA_SD_SRST 0x01u
#define HDA_SD_STATUS_MASK 0x1cu
#define HDA_STREAM_CBL (HDA_DMA_BLOCK_SIZE * HDA_DMA_BLOCK_COUNT)
#define HDA_STREAM_POLLS 100000u
#define HDA_PCM_BYTES_PER_MILLISECOND 192u

struct wav_format {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static enum hda_stream_result parse_wav(const uint8_t *wav,
    uint32_t wav_size, uint32_t *data_offset, uint32_t *data_size)
{
    struct wav_format format = {0};
    bool have_format = false, have_data = false;
    if (wav == NULL || data_offset == NULL || data_size == NULL
        || wav_size < 12u) {
        return HDA_STREAM_BAD_ARGUMENT;
    }
    if (memcmp(wav, "RIFF", 4u) != 0 || memcmp(wav + 8u, "WAVE", 4u) != 0)
        return HDA_STREAM_INVALID_WAV;
    uint32_t riff_end = read_le32(wav + 4u);
    if (riff_end > wav_size - 8u || riff_end < 4u)
        return HDA_STREAM_INVALID_WAV;
    riff_end += 8u;
    for (uint32_t offset = 12u; offset <= riff_end - 8u;) {
        uint32_t chunk_size = read_le32(wav + offset + 4u);
        uint32_t chunk_data = offset + 8u;
        if (chunk_size > riff_end - chunk_data)
            return HDA_STREAM_INVALID_WAV;
        if (memcmp(wav + offset, "fmt ", 4u) == 0) {
            if (chunk_size < 16u)
                return HDA_STREAM_INVALID_WAV;
            format.format = read_le16(wav + chunk_data);
            format.channels = read_le16(wav + chunk_data + 2u);
            format.sample_rate = read_le32(wav + chunk_data + 4u);
            format.block_align = read_le16(wav + chunk_data + 12u);
            format.bits_per_sample = read_le16(wav + chunk_data + 14u);
            have_format = true;
        } else if (memcmp(wav + offset, "data", 4u) == 0 && !have_data) {
            *data_offset = chunk_data;
            *data_size = chunk_size;
            have_data = true;
        }
        uint32_t padded_size = chunk_size + (chunk_size & 1u);
        if (padded_size > riff_end - chunk_data
            || padded_size > UINT32_MAX - chunk_data) {
            return HDA_STREAM_INVALID_WAV;
        }
        offset = chunk_data + padded_size;
    }
    if (!have_format || !have_data || *data_size == 0u
        || (*data_size & 3u) != 0u) {
        return HDA_STREAM_INVALID_WAV;
    }
    if (format.format != 1u || format.channels != 2u
        || format.sample_rate != 48000u || format.block_align != 4u
        || format.bits_per_sample != 16u) {
        return HDA_STREAM_UNSUPPORTED_FORMAT;
    }
    if (*data_size > HDA_STREAM_MAX_PCM_BYTES)
        return HDA_STREAM_TOO_LARGE;
    return HDA_STREAM_OK;
}

static uint8_t *stream_control(struct hda_device *device)
{
    return (uint8_t *)(uintptr_t)(device->regs + device->stream_offset);
}

static uint32_t stream_read32(const struct hda_device *device,
    uint32_t offset)
{
    return *(volatile uint32_t *)(device->regs + device->stream_offset + offset);
}

static enum hda_stream_result wait_control(const struct hda_platform *platform,
    struct hda_device *device, uint8_t mask, bool set)
{
    volatile uint8_t *control = stream_control(device);
    for (uint32_t i = 0u; i < HDA_STREAM_POLLS; ++i) {
        if (((*control & mask) != 0u) == set)
            return HDA_STREAM_OK;
        platform->relax(platform->ctx);
    }
    return HDA_STREAM_ENGINE_ERROR;
}

static enum hda_stream_result reset_stream(
    const struct hda_platform *platform, struct hda_device *device,
    bool start)
{
    volatile uint8_t *control = stream_control(device);
    *control &= (uint8_t)~HDA_SD_RUN;
    if (wait_control(platform, device, HDA_SD_RUN, false) != HDA_STREAM_OK)
        return HDA_STREAM_ENGINE_ERROR;
    *control = (uint8_t)((*control & (uint8_t)~HDA_SD_RUN) | HDA_SD_SRST);
    if (wait_control(platform, device, HDA_SD_SRST, true) != HDA_STREAM_OK)
        return HDA_STREAM_ENGINE_ERROR;
    *control &= (uint8_t)~HDA_SD_SRST;
    if (wait_control(platform, device, HDA_SD_SRST, false) != HDA_STREAM_OK)
        return HDA_STREAM_ENGINE_ERROR;
    device->regs[device->stream_offset + HDA_SD_STATUS] =
        HDA_SD_STATUS_MASK;
    if (start) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        *control |= HDA_SD_RUN;
        if (wait_control(platform, device, HDA_SD_RUN, true) != HDA_STREAM_OK)
            return HDA_STREAM_ENGINE_ERROR;
    }
    return HDA_STREAM_OK;
}

static void fill_block(struct hda_device *device, struct hda_stream *stream,
    uint32_t block)
{
    uint32_t remaining = stream->pcm_size - stream->next_pcm_offset;
    uint32_t copy = remaining < HDA_DMA_BLOCK_SIZE
        ? remaining : HDA_DMA_BLOCK_SIZE;
    uint8_t *destination = device->dma->samples[block];
    if (copy != 0u) {
        memcpy(destination, stream->pcm + stream->next_pcm_offset, copy);
        stream->next_pcm_offset += copy;
    }
    if (copy < HDA_DMA_BLOCK_SIZE) {
        memset(destination + copy, 0, HDA_DMA_BLOCK_SIZE - copy);
    }
}

static enum hda_stream_result start_at(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, uint32_t position)
{
    enum hda_stream_result result = reset_stream(platform, device, false);
    if (result != HDA_STREAM_OK) {
        stream->failed = true;
        return result;
    }
    if (position >= stream->pcm_size) {
        memset(device->dma->samples, 0, sizeof(device->dma->samples));
        stream->played_bytes = stream->pcm_size;
        stream->next_pcm_offset = stream->pcm_size;
        stream->active = false;
        stream->paused = false;
        stream->loaded = true;
        return HDA_STREAM_OK;
    }
    stream->next_pcm_offset = position;
    stream->played_bytes = position;
    stream->total_position = 0u;
    fill_block(device, stream, 0u);
    fill_block(device, stream, 1u);
    stream->last_lpib = stream_read32(device, HDA_SD_LPIB);
    if (stream->last_lpib >= HDA_STREAM_CBL) {
        stream->failed = true;
        return HDA_STREAM_ENGINE_ERROR;
    }
    result = reset_stream(platform, device, true);
    if (result != HDA_STREAM_OK) {
        stream->failed = true;
        return result;
    }
    stream->last_lpib = stream_read32(device, HDA_SD_LPIB);
    stream->active = true;
    stream->paused = false;
    stream->loaded = true;
    return HDA_STREAM_OK;
}

enum hda_stream_result hda_stream_play_wav(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, const uint8_t *wav, uint32_t wav_size)
{
    uint32_t data_offset = 0u, data_size = 0u;
    if (platform == NULL || platform->relax == NULL || device == NULL
        || device->regs == NULL || device->dma == NULL || stream == NULL) {
        return HDA_STREAM_BAD_ARGUMENT;
    }
    if (!hda_is_available(device))
        return HDA_STREAM_UNAVAILABLE;
    if (wav_size > HDA_STREAM_MAX_WAV_BYTES)
        return HDA_STREAM_TOO_LARGE;
    enum hda_stream_result result =
        parse_wav(wav, wav_size, &data_offset, &data_size);
    if (result != HDA_STREAM_OK)
        return result;
    memcpy(stream->pcm, wav + data_offset, data_size);
    stream->pcm_size = data_size;
    stream->failed = false;
    stream->loaded = true;
    result = start_at(platform, device, stream, 0u);
    if (result != HDA_STREAM_OK) {
        stream->active = false;
        return result;
    }
    return HDA_STREAM_OK;
}

enum hda_stream_result hda_stream_service(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream)
{
    if (platform == NULL || platform->relax == NULL || device == NULL
        || device->regs == NULL || device->dma == NULL || stream == NULL) {
        return HDA_STREAM_BAD_ARGUMENT;
    }
    if (!stream->active || stream->paused)
        return HDA_STREAM_OK;
    uint32_t lpib = stream_read32(device, HDA_SD_LPIB);
    if (lpib >= HDA_STREAM_CBL) {
        stream->failed = true;
        stream->active = false;
        (void)reset_stream(platform, device, false);
        return HDA_STREAM_ENGINE_ERROR;
    }
    uint32_t delta = lpib >= stream->last_lpib
        ? lpib - stream->last_lpib
        : HDA_STREAM_CBL - stream->last_lpib + lpib;
    uint32_t old_position = stream->total_position;
    stream->total_position += delta;
    stream->last_lpib = lpib;
    uint32_t old_block = old_position / HDA_DMA_BLOCK_SIZE;
    uint32_t new_block = stream->total_position / HDA_DMA_BLOCK_SIZE;
    if (new_block - old_block > 1u) {
        stream->failed = true;
        stream->active = false;
        (void)reset_stream(platform, device, false);
        return HDA_STREAM_UNDERRUN;
    }
    if (new_block != old_block) {
        fill_block(device, stream, old_block & 1u);
    }
    uint32_t remaining = stream->pcm_size - stream->played_bytes;
    stream->played_bytes += delta < remaining ? delta : remaining;
    if (stream->played_bytes >= stream->pcm_size) {
        (void)reset_stream(platform, device, false);
        stream->active = false;
        stream->paused = false;
    }
    return HDA_STREAM_OK;
}

enum hda_stream_result hda_stream_pause(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, bool paused)
{
    if (platform == NULL || platform->relax == NULL || device == NULL
        || device->regs == NULL || device->dma == NULL || stream == NULL) {
        return HDA_STREAM_BAD_ARGUMENT;
    }
    if (!stream->active)
        return HDA_STREAM_UNAVAILABLE;
    if (stream->paused == paused)
        return HDA_STREAM_OK;
    volatile uint8_t *control = stream_control(device);
    if (paused) {
        *control &= (uint8_t)~HDA_SD_RUN;
        if (wait_control(platform, device, HDA_SD_RUN, false)
            != HDA_STREAM_OK) {
            stream->failed = true;
            return HDA_STREAM_ENGINE_ERROR;
        }
    } else {
        *control |= HDA_SD_RUN;
        if (wait_control(platform, device, HDA_SD_RUN, true)
            != HDA_STREAM_OK) {
            stream->failed = true;
            return HDA_STREAM_ENGINE_ERROR;
        }
        stream->last_lpib = stream_read32(device, HDA_SD_LPIB);
    }
    stream->paused = paused;
    return HDA_STREAM_OK;
}

enum hda_stream_result hda_stream_seek(
    const struct hda_platform *platform, struct hda_device *device,
    struct hda_stream *stream, uint32_t position_ms)
{
    if (platform == NULL || platform->relax == NULL || device == NULL
        || device->regs == NULL || device->dma == NULL || stream == NULL) {
        return HDA_STREAM_BAD_ARGUMENT;
    }
    if (!stream->loaded) { return HDA_STREAM_BAD_ARGUMENT; }
    if (!hda_is_available(device)) { return HDA_STREAM_UNAVAILABLE; }
    uint64_t requested = (uint64_t)position_ms
        * HDA_PCM_BYTES_PER_MILLISECOND;
    uint32_t position = requested >= stream->pcm_size
        ? stream->pcm_size : (uint32_t)requested;
    position &= ~3u;
    stream->failed = false;
    return start_at(platform, device, stream, position);
}

void hda_stream_stop(struct hda_device *device, struct hda_stream *stream)
{
    if (device != NULL && device->regs != NULL) {
        volatile uint8_t *control = stream_control(device);
        *control &= (uint8_t)~HDA_SD_RUN;
    }
    if (stream != NULL) {
        stream->active = false;
        stream->paused = false;
        stream->loaded = false;
        stream->failed = false;
        stream->pcm_size = 0u;
        stream->next_pcm_offset = 0u;
        stream->played_bytes = 0u;
        stream->total_position = 0u;
        stream->last_lpib = 0u;
    }
}

void hda_stream_get_info(const struct hda_stream *stream,
    struct hda_stream_info *info)
{
    if (stream == NULL || info == NULL)
        return;
    info->position_ms = stream->played_bytes / HDA_PCM_BYTES_PER_MILLISECOND;
    info->duration_ms = stream->pcm_size
        / HDA_PCM_BYTES_PER_MILLISECOND;
    info->loaded = stream->loaded;
    info->playing = stream->active && !stream->paused;
    info->paused = stream->paused;
    info->failed = stream->failed;
}

const char *hda_stream_result_name(enum hda_stream_result result)
{
    switch (result) {
    case HDA_STREAM_OK: return "ok";
    case HDA_STREAM_BAD_ARGUMENT: return "invalid argument";
    case HDA_STREAM_UNAVAILABLE: return "audio stream unavailable";
    case HDA_STREAM_INVALID_WAV: return "invalid PCM WAV file";
    case HDA_STREAM_UNSUPPORTED_FORMAT: return "unsupported WAV format";
    case HDA_STREAM_TOO_LARGE: return "WAV exceeds the 2 MiB limit";
    case HDA_STREAM_ENGINE_ERROR: return "HDA stream engine error";
    case HDA_STREAM_UNDERRUN: return "HDA stream underrun";
    }
    return "unknown stream error";
}
