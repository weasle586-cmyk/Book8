#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../drivers/hda_stream.h"

#define TEST_STREAM_OFFSET 0x80u

static uint32_t registers[128];
static struct hda_dma dma;
static struct hda_stream stream;
static struct hda_device device;
static uint8_t wav[44u + 12288u];

static void relax(void *context)
{
    (void)context;
}

static void put_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void set_lpib(uint32_t value)
{
    *(uint32_t *)(void *)((uint8_t *)registers + TEST_STREAM_OFFSET + 4u) = value;
}

static void make_wav(uint32_t pcm_size, uint32_t sample_rate)
{
    memset(wav, 0, sizeof(wav));
    memcpy(wav, "RIFF", 4u);
    put_le32(wav + 4u, 36u + pcm_size);
    memcpy(wav + 8u, "WAVEfmt ", 8u);
    put_le32(wav + 16u, 16u);
    put_le16(wav + 20u, 1u);
    put_le16(wav + 22u, 2u);
    put_le32(wav + 24u, sample_rate);
    put_le32(wav + 28u, sample_rate * 4u);
    put_le16(wav + 32u, 4u);
    put_le16(wav + 34u, 16u);
    memcpy(wav + 36u, "data", 4u);
    put_le32(wav + 40u, pcm_size);
    for (uint32_t i = 0u; i < pcm_size; ++i) {
        wav[44u + i] = (uint8_t)(i * 37u + 11u);
    }
}

int main(void)
{
    const struct hda_platform platform = {.relax = relax};
    device = (struct hda_device){
        .regs = (volatile uint8_t *)registers,
        .stream_offset = TEST_STREAM_OFFSET,
        .running = true,
        .volume_controls_valid = true,
        .amp_control_count = 1u,
        .dma = &dma
    };

    make_wav(12288u, 48000u);
    assert(hda_stream_play_wav(&platform, &device, &stream,
        wav, sizeof(wav)) == HDA_STREAM_OK);
    assert((registers[TEST_STREAM_OFFSET / 4u] & 2u) != 0u);
    assert(memcmp(dma.samples[0], wav + 44u, HDA_DMA_BLOCK_SIZE) == 0);
    assert(memcmp(dma.samples[1], wav + 44u + HDA_DMA_BLOCK_SIZE,
        HDA_DMA_BLOCK_SIZE) == 0);

    assert(hda_stream_pause(&platform, &device, &stream, true) == HDA_STREAM_OK);
    set_lpib(512u);
    assert(hda_stream_service(&platform, &device, &stream) == HDA_STREAM_OK);
    assert(stream.played_bytes == 0u);
    assert(hda_stream_pause(&platform, &device, &stream, false) == HDA_STREAM_OK);

    set_lpib(512u + HDA_DMA_BLOCK_SIZE);
    assert(hda_stream_service(&platform, &device, &stream) == HDA_STREAM_OK);
    assert(memcmp(dma.samples[0], wav + 44u + 2u * HDA_DMA_BLOCK_SIZE,
        HDA_DMA_BLOCK_SIZE) == 0);
    assert(hda_stream_seek(&platform, &device, &stream, 25u) == HDA_STREAM_OK);
    assert(stream.active && !stream.paused && stream.played_bytes == 4800u);
    assert(memcmp(dma.samples[0], wav + 44u + 4800u,
        HDA_DMA_BLOCK_SIZE) == 0);

    assert(hda_stream_seek(&platform, &device, &stream, 64u) == HDA_STREAM_OK);
    assert(!stream.active && stream.loaded && !stream.paused);
    assert((*(volatile uint8_t *)(device.regs + TEST_STREAM_OFFSET) & 2u) == 0u);
    struct hda_stream_info info;
    hda_stream_get_info(&stream, &info);
    assert(info.loaded && !info.playing && info.position_ms == 64u
        && info.duration_ms == 64u);

    make_wav(4u, 44100u);
    assert(hda_stream_play_wav(&platform, &device, &stream,
        wav, 48u) == HDA_STREAM_UNSUPPORTED_FORMAT);
    make_wav(4u, 48000u);
    put_le32(wav + 4u, 0xffffffffu);
    assert(hda_stream_play_wav(&platform, &device, &stream,
        wav, 48u) == HDA_STREAM_INVALID_WAV);
    assert(hda_stream_play_wav(&platform, &device, &stream,
        wav, HDA_STREAM_MAX_WAV_BYTES + 1u) == HDA_STREAM_TOO_LARGE);

    device.running = false;
    assert(hda_stream_seek(&platform, &device, &stream, 1u)
        == HDA_STREAM_UNAVAILABLE);
    hda_stream_stop(&device, &stream);
    assert(!stream.loaded && !stream.active);
    puts("PASS: HDA WAV stream host test");
    return 0;
}
