#include "../drivers/hda.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MMIO_BYTES 0x4000u
#define CODEC_FG 1u
#define CODEC_DAC 2u
#define CODEC_MIXER 3u
#define CODEC_SELECTOR 6u

struct fake_hda {
    uint32_t config[64];
    uint8_t regs[MMIO_BYTES];
    struct hda_dma *dma;
    uint16_t corb_consumed;
    uint16_t rirb_wp;
    uint8_t pin_controls[128];
    uint8_t selectors[128];
    uint16_t amp_values[128][16][2];
    uint16_t stream_formats[128];
    uint8_t stream_tag;
    uint8_t stream_channel;
    uint8_t eapd[128];
    unsigned pci_writes;
    uint8_t pci_write_offsets[8];
    unsigned command_count;
    unsigned log_count;
    bool separate_dac;
};

static uint16_t read16(const uint8_t *bytes)
{
    uint16_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t read32(const uint8_t *bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write16(uint8_t *bytes, uint16_t value)
{
    memcpy(bytes, &value, sizeof(value));
}

static uint32_t fake_pci_read(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset)
{
    struct fake_hda *fake = ctx;
    assert(bus == 0u && slot == 0x14u && function == 2u);
    return fake->config[offset / 4u];
}

static void fake_pci_write16(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint16_t value)
{
    struct fake_hda *fake = ctx;
    assert(bus == 0u && slot == 0x14u && function == 2u);
    assert(fake->pci_writes < sizeof(fake->pci_write_offsets));
    fake->pci_write_offsets[fake->pci_writes++] = offset;
    uint32_t *word = &fake->config[offset / 4u];
    uint32_t shift = (offset & 2u) * 8u;
    *word = (*word & ~(0xffffu << shift)) | ((uint32_t)value << shift);
}

static volatile uint8_t *fake_map(void *ctx, uint64_t phys, uint64_t size,
    bool uncached)
{
    struct fake_hda *fake = ctx;
    assert(phys == 0xfcff4000ull);
    assert(size == MMIO_BYTES && uncached);
    return fake->regs;
}

static bool fake_dma_address(void *ctx, const void *ptr, size_t size,
    uint64_t *address)
{
    struct fake_hda *fake = ctx;
    uintptr_t base = (uintptr_t)fake->dma;
    uintptr_t current = (uintptr_t)ptr;
    if (current < base || current - base > sizeof(*fake->dma)
        || size > sizeof(*fake->dma) - (current - base)) {
        return false;
    }
    *address = 0x00100000ull + (current - base);
    return true;
}

static uint32_t widget_caps(uint8_t nid)
{
    switch (nid) {
    case CODEC_DAC:
        return (1u << 0u) | (1u << 2u) | (1u << 10u);
    case CODEC_MIXER:
        return (2u << 20u) | (1u << 1u) | (1u << 2u)
            | (1u << 8u) | (1u << 10u);
    case CODEC_SELECTOR:
        return (3u << 20u) | (1u << 1u) | (1u << 2u)
            | (1u << 8u) | (1u << 10u);
    case 7u:
        return (1u << 0u) | (1u << 2u) | (1u << 10u);
    case 8u:
        return (2u << 20u) | (1u << 1u) | (1u << 2u)
            | (1u << 8u) | (1u << 10u);
    case HDA_PIN_REAR_GREEN:
    case HDA_PIN_FRONT_HEADPHONE:
        return (4u << 20u) | (1u << 2u) | (1u << 8u)
            | (1u << 10u);
    default:
        return 0u;
    }
}

static uint8_t connection_for(const struct fake_hda *fake, uint8_t nid)
{
    switch (nid) {
    case CODEC_MIXER: return CODEC_DAC;
    case CODEC_SELECTOR:
        return fake->separate_dac ? 8u : CODEC_MIXER;
    case 8u:
        return 7u;
    case HDA_PIN_REAR_GREEN: return CODEC_MIXER;
    case HDA_PIN_FRONT_HEADPHONE: return CODEC_SELECTOR;
    default: return 0u;
    }
}

static uint32_t codec_parameter_value(const struct fake_hda *fake,
    uint8_t nid, uint8_t parameter)
{
    if (nid == 0u && parameter == 0x00u)
        return HDA_CODEC_VIA_ID;
    if (nid == 0u && parameter == 0x04u)
        return (CODEC_FG << 16u) | 1u;
    if (nid == CODEC_FG && parameter == 0x05u)
        return 1u;
    if (nid == CODEC_FG && parameter == 0x04u)
        return (2u << 16u) | 28u;
    if (parameter == 0x09u)
        return widget_caps(nid);
    if ((nid == CODEC_DAC || nid == 7u) && parameter == 0x0au)
        return (1u << 6u) | (1u << 17u);
    if ((nid == CODEC_DAC || nid == 7u) && parameter == 0x0bu)
        return 1u;
    if (parameter == 0x0eu && connection_for(fake, nid) != 0u)
        return 1u;
    if (parameter == 0x0cu
        && (nid == HDA_PIN_REAR_GREEN || nid == HDA_PIN_FRONT_HEADPHONE)) {
        return (1u << 3u) | (1u << 4u) | (1u << 16u);
    }
    if (parameter == 0x12u)
        return (20u << 8u) | 10u;
    if (parameter == 0x0du)
        return (15u << 8u) | 7u;
    return 0u;
}

static uint32_t execute_command(struct fake_hda *fake, uint32_t command)
{
    uint8_t nid = (uint8_t)(command >> 20u);
    uint16_t encoded_verb = (uint16_t)((command >> 8u) & 0x0fffu);
    uint8_t verb12 = (uint8_t)(encoded_verb >> 4u);
    uint8_t param8 = (uint8_t)command;
    ++fake->command_count;

    if ((encoded_verb & 0x0f00u) == 0x0f00u) {
        switch (encoded_verb) {
        case 0x0f00u:
            return codec_parameter_value(fake, nid, param8);
        case 0x0f02u:
            return connection_for(fake, nid);
        case 0x0f05u:
            return 0u;
        case 0x0f0cu:
            return codec_parameter_value(fake, nid, 0x0cu);
        default:
            return 0u;
        }
    }
    if (encoded_verb == 0x0200u) {
        fake->stream_formats[nid] = (uint16_t)command;
        return 0u;
    }
    if ((encoded_verb & 0x0f00u) == 0x0700u) {
        switch (encoded_verb) {
        case 0x0701u:
            fake->selectors[nid] = param8;
            break;
        case 0x0705u:
            break;
        case 0x0706u:
            fake->stream_tag = (uint8_t)(param8 >> 4u);
            fake->stream_channel = (uint8_t)(param8 & 0x0fu);
            break;
        case 0x0707u:
            fake->pin_controls[nid] = param8;
            break;
        case 0x070cu:
            fake->eapd[nid] = param8;
            break;
        default:
            break;
        }
        return 0u;
    }
    if ((command & 0x000f0000u) == 0x00030000u) {
        uint16_t payload = (uint16_t)command;
        bool output = (payload & 0x8000u) != 0u;
        bool input = (payload & 0x4000u) != 0u;
        uint8_t index = (uint8_t)((payload >> 8u) & 0x0fu);
        fake->amp_values[nid][index][output ? 0u : input ? 1u : 0u] =
            payload;
    }
    (void)verb12;
    return 0u;
}

static void fake_relax(void *ctx)
{
    struct fake_hda *fake = ctx;
    uint16_t corb_wp = (uint16_t)(read16(fake->regs + 0x48u) & 0xffu);
    while (fake->corb_consumed != corb_wp) {
        uint16_t next = (uint16_t)((fake->corb_consumed + 1u) & 0xffu);
        uint32_t command = fake->dma->corb[next];
        uint16_t response_index =
            (uint16_t)((fake->rirb_wp + 1u) & 0xffu);
        fake->dma->rirb[response_index].response =
            execute_command(fake, command);
        fake->dma->rirb[response_index].extended = 0u;
        fake->rirb_wp = response_index;
        fake->corb_consumed = next;
        write16(fake->regs + 0x4au, fake->corb_consumed);
        write16(fake->regs + 0x58u, fake->rirb_wp);
    }
}

static void fake_log(void *ctx, const char *text)
{
    struct fake_hda *fake = ctx;
    assert(strstr(text, "48 kHz stereo output active") != NULL);
    ++fake->log_count;
}

static void setup(struct fake_hda *fake, struct hda_dma *dma,
    bool separate_dac)
{
    memset(fake, 0, sizeof(*fake));
    memset(dma, 0, sizeof(*dma));
    fake->dma = dma;
    fake->separate_dac = separate_dac;
    fake->config[0] = HDA_ATI_VENDOR_ID
        | ((uint32_t)HDA_ATI_DEVICE_ID << 16u);
    fake->config[1] = 0u;
    fake->config[2] = 0x04030001u;
    fake->config[4] = 0xfcff4004u;
    fake->config[5] = 0u;
    fake->config[16] = 0x00010000u;
    fake->config[17] = 1u;
    fake->config[19] = 1u;
    write16(fake->regs, 0x4400u);
    write16(fake->regs + 0x0eu, 1u);
    fake->regs[0x4eu] = 0x40u;
    fake->regs[0x5eu] = 0x40u;
}

int main(void)
{
    struct fake_hda fake;
    struct hda_dma dma;
    struct hda_device device;
    setup(&fake, &dma, false);
    const struct hda_platform platform = {
        .ctx = &fake,
        .pci_read32 = fake_pci_read,
        .pci_write16 = fake_pci_write16,
        .map_mmio = fake_map,
        .dma_address = fake_dma_address,
        .relax = fake_relax,
        .log = fake_log
    };

    enum hda_status status =
        hda_init(&platform, &device, &dma, 0u, 0x14u, 2u);
    assert(status == HDA_OK);
    assert(device.present && device.running);
    assert(device.mmio_base == 0xfcff4000ull);
    assert(device.stream_offset == 0x100u);
    assert(device.codec_id == HDA_CODEC_VIA_ID);
    assert(fake.command_count > 40u);
    assert(fake.log_count == 1u);
    assert((read32(fake.regs + 0x08u) & 1u) != 0u);
    assert((fake.regs[0x4cu] & 0x02u) != 0u);
    assert((fake.regs[0x5cu] & 0x02u) != 0u);
    assert(read32(fake.regs + 0x40u)
        == 0x00100000u + offsetof(struct hda_dma, corb));
    assert(read32(fake.regs + 0x50u)
        == 0x00100000u + offsetof(struct hda_dma, rirb));

    assert((fake.config[1] & 0x0006u) == 0x0006u);
    assert((fake.config[16] & 0x00020000u) != 0u);
    assert((fake.config[16] & 0x00010000u) != 0u);
    assert(fake.config[17] == 1u);
    assert(fake.config[19] == 1u);
    assert(fake.pci_writes == 2u);
    assert(fake.pci_write_offsets[0] == 4u);
    assert(fake.pci_write_offsets[1] == 0x42u);

    assert(read32(fake.regs + device.stream_offset + 0x08u) == 8192u);
    assert(read16(fake.regs + device.stream_offset + 0x0cu) == 1u);
    assert(read16(fake.regs + device.stream_offset + 0x12u)
        == HDA_PCM_48K_STEREO_S16);
    assert((fake.regs[device.stream_offset] & 0x06u) == 0x02u);
    assert(read32(fake.regs + 0x20u) == 0u);
    assert(fake.stream_tag == HDA_STREAM_TAG);
    assert(fake.stream_channel == 0u);
    assert(fake.stream_formats[CODEC_DAC] == HDA_PCM_48K_STEREO_S16);
    assert(fake.pin_controls[HDA_PIN_REAR_GREEN] == 0x40u);
    assert(fake.pin_controls[HDA_PIN_FRONT_HEADPHONE] == 0xc0u);
    assert(fake.eapd[HDA_PIN_REAR_GREEN] == 0x02u);
    assert(fake.eapd[HDA_PIN_FRONT_HEADPHONE] == 0x02u);
    assert(fake.selectors[CODEC_SELECTOR] == 0u);
    assert(hda_is_available(&device));
    assert(hda_get_volume(&device) == 70u && !hda_get_mute(&device));
    assert(device.amp_control_count > 0u);
    assert(fake.amp_values[CODEC_DAC][0][0] == 0xb00eu);
    assert(fake.amp_values[CODEC_MIXER][0][1] == 0x700bu);
    assert(fake.amp_values[HDA_PIN_FRONT_HEADPHONE][0][0] == 0xb00eu);
    assert(hda_set_volume(&platform, &device, 101u) == HDA_BAD_ARGUMENT);
    assert(hda_set_volume(&platform, &device, 100u) == HDA_OK);
    assert(hda_get_volume(&device) == 100u);
    assert(fake.amp_values[CODEC_DAC][0][0] == 0xb014u);
    assert(fake.amp_values[CODEC_MIXER][0][1] == 0x700fu);
    assert(hda_set_mute(&platform, &device, true) == HDA_OK);
    assert(hda_get_mute(&device));
    assert(fake.amp_values[CODEC_DAC][0][0] == 0xb094u);
    assert(hda_set_mute(&platform, &device, false) == HDA_OK);
    assert(!hda_get_mute(&device));
    assert(fake.amp_values[CODEC_DAC][0][0] == 0xb014u);
    assert(hda_set_volume(&platform, &device, 0u) == HDA_OK);
    assert(hda_get_volume(&device) == 0u && !hda_get_mute(&device));
    assert(fake.amp_values[CODEC_DAC][0][0] == 0xb080u);
    uint32_t bdl_phys = read32(fake.regs + device.stream_offset + 0x18u);
    assert(bdl_phys >= 0x00100000u);
    assert((bdl_phys & (HDA_DMA_ALIGNMENT - 1u)) == 0u);
    size_t bdl_offset = bdl_phys - 0x00100000u;
    const struct hda_bdl_entry *bdl =
        (const struct hda_bdl_entry *)((const uint8_t *)&dma + bdl_offset);
    assert(bdl[0].length == HDA_DMA_BLOCK_SIZE && bdl[0].flags == 1u);
    assert(bdl[1].length == HDA_DMA_BLOCK_SIZE && bdl[1].flags == 1u);
    assert(bdl[0].address_high == 0u && bdl[1].address_high == 0u);
    assert((bdl[0].address_low & (HDA_DMA_ALIGNMENT - 1u)) == 0u);
    assert((bdl[1].address_low & (HDA_DMA_ALIGNMENT - 1u)) == 0u);
    for (size_t i = 0u; i < sizeof(dma.samples); ++i)
        assert(((const uint8_t *)dma.samples)[i] == 0u);

    puts("HDA host test: PCI, snoop preservation, VIA routing, DMA and stream PASS");

    setup(&fake, &dma, true);
    status = hda_init(&platform, &device, &dma, 0u, 0x14u, 2u);
    assert(status == HDA_OK);
    assert(fake.stream_formats[CODEC_DAC] == HDA_PCM_48K_STEREO_S16);
    assert(fake.stream_formats[7u] == HDA_PCM_48K_STEREO_S16);
    assert(fake.selectors[CODEC_SELECTOR] == 0u);
    puts("HDA host test: independent rear/front DAC routes PASS");
    return 0;
}
