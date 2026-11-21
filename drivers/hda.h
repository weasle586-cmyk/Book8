#ifndef BOOK8_HDA_H
#define BOOK8_HDA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HDA_ATI_VENDOR_ID 0x1002u
#define HDA_ATI_DEVICE_ID 0x4383u
#define HDA_CODEC_VIA_ID 0x11060397u
#define HDA_CODEC_ADDRESS 0u
#define HDA_PIN_REAR_GREEN 0x1cu
#define HDA_PIN_FRONT_HEADPHONE 0x1du
#define HDA_STREAM_TAG 1u
#define HDA_PCM_48K_STEREO_S16 0x0011u
#define HDA_DMA_ALIGNMENT 128u
#define HDA_DMA_BLOCK_SIZE 4096u
#define HDA_DMA_BLOCK_COUNT 2u
#define HDA_MAX_AMP_CONTROLS 32u

struct hda_amp_control {
    uint8_t nid;
    uint8_t index;
    uint8_t steps;
    uint8_t zero_db;
    bool output;
};

struct hda_platform {
    void *ctx;
    uint32_t (*pci_read32)(void *, uint8_t, uint8_t, uint8_t, uint8_t);
    void (*pci_write16)(void *, uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
    volatile uint8_t *(*map_mmio)(void *, uint64_t, uint64_t, bool);
    bool (*dma_address)(void *, const void *, size_t, uint64_t *);
    void (*relax)(void *);
    void (*log)(void *, const char *);
};

struct hda_rirb_entry {
    uint32_t response;
    uint32_t extended;
};

struct hda_bdl_entry {
    uint32_t address_low;
    uint32_t address_high;
    uint32_t length;
    uint32_t flags;
};

struct hda_dma {
    uint32_t corb[256] __attribute__((aligned(HDA_DMA_ALIGNMENT)));
    struct hda_rirb_entry rirb[256]
        __attribute__((aligned(HDA_DMA_ALIGNMENT)));
    struct hda_bdl_entry bdl[HDA_DMA_BLOCK_COUNT]
        __attribute__((aligned(HDA_DMA_ALIGNMENT)));
    uint8_t samples[HDA_DMA_BLOCK_COUNT][HDA_DMA_BLOCK_SIZE]
        __attribute__((aligned(HDA_DMA_ALIGNMENT)));
};

enum hda_status {
    HDA_OK = 0,
    HDA_ABSENT,
    HDA_BAD_ARGUMENT,
    HDA_BAD_DEVICE,
    HDA_PCI_ERROR,
    HDA_BAD_BAR,
    HDA_MAP_FAILED,
    HDA_DMA_ERROR,
    HDA_RESET_TIMEOUT,
    HDA_NO_CODEC,
    HDA_RING_ERROR,
    HDA_VERB_TIMEOUT,
    HDA_CODEC_UNSUPPORTED,
    HDA_CODEC_ROUTE_ERROR,
    HDA_STREAM_ERROR
};

struct hda_device {
    uint8_t bus, slot, function;
    volatile uint8_t *regs;
    uint64_t mmio_base;
    uint32_t gcap;
    uint32_t codec_id;
    uint32_t stream_offset;
    uint16_t corb_entries, rirb_entries;
    uint16_t corb_wp, rirb_rp;
    bool present;
    bool running;
    bool command_timeout;
    bool volume_controls_valid;
    bool muted;
    uint8_t volume_percent;
    uint8_t amp_control_count;
    struct hda_amp_control amp_controls[HDA_MAX_AMP_CONTROLS];
    struct hda_dma *dma;
};

/* Probe and initialize one PCI function; BAR resources are never reassigned. */
enum hda_status hda_init(const struct hda_platform *platform,
    struct hda_device *device, struct hda_dma *dma,
    uint8_t bus, uint8_t slot, uint8_t function);
bool hda_is_available(const struct hda_device *device);
uint8_t hda_get_volume(const struct hda_device *device);
bool hda_get_mute(const struct hda_device *device);
enum hda_status hda_set_volume(const struct hda_platform *platform,
    struct hda_device *device, uint8_t percent);
enum hda_status hda_set_mute(const struct hda_platform *platform,
    struct hda_device *device, bool muted);
const char *hda_status_name(enum hda_status status);

#endif
