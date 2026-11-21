#include "hda.h"

#define HDA_MMIO_SIZE 0x4000u
#define HDA_RESET_POLLS 1000000u
#define HDA_RESET_DELAY_POLLS 100000u
#define HDA_RING_POLLS 1000000u
#define HDA_VERB_POLLS 1000000u
#define HDA_STREAM_POLLS 100000u

#define HDA_REG_GCAP 0x00u
#define HDA_REG_GCTL 0x08u
#define HDA_REG_STATESTS 0x0eu
#define HDA_REG_INTCTL 0x20u
#define HDA_REG_INTSTS 0x24u
#define HDA_REG_CORBLBASE 0x40u
#define HDA_REG_CORBUBASE 0x44u
#define HDA_REG_CORBWP 0x48u
#define HDA_REG_CORBRP 0x4au
#define HDA_REG_CORBCTL 0x4cu
#define HDA_REG_CORBSIZE 0x4eu
#define HDA_REG_RIRBLBASE 0x50u
#define HDA_REG_RIRBUBASE 0x54u
#define HDA_REG_RIRBWP 0x58u
#define HDA_REG_RIRBCTL 0x5cu
#define HDA_REG_RIRBSTS 0x5du
#define HDA_REG_RIRBSIZE 0x5eu

#define HDA_GCTL_CRST (1u << 0)
#define HDA_GCAP_ISS_MASK 0x0f00u
#define HDA_GCAP_OSS_MASK 0xf000u
#define HDA_CORB_RUN 0x02u
#define HDA_RIRB_DMA_ENABLE 0x02u
#define HDA_CORBRP_RESET 0x8000u
#define HDA_RIRBWP_RESET 0x8000u
#define HDA_RING_SIZE_2 0x00u
#define HDA_RING_SIZE_16 0x01u
#define HDA_RING_SIZE_256 0x02u
#define HDA_RING_CAP_2 0x10u
#define HDA_RING_CAP_16 0x20u
#define HDA_RING_CAP_256 0x40u
#define HDA_RING_CAP_MASK 0xf0u
#define HDA_RING_SIZE_MASK 0x03u

#define HDA_SD_BASE 0x80u
#define HDA_SD_STRIDE 0x20u
#define HDA_SD_CTL 0x00u
#define HDA_SD_LPIB 0x04u
#define HDA_SD_CBL 0x08u
#define HDA_SD_LVI 0x0cu
#define HDA_SD_FORMAT 0x12u
#define HDA_SD_BDLPL 0x18u
#define HDA_SD_BDLPU 0x1cu
#define HDA_SD_SRST 0x01u
#define HDA_SD_RUN 0x02u
#define HDA_SD_STATUS 0x03u
#define HDA_SD_STATUS_MASK 0x1cu

#define HDA_VERB_GET_PARAMETER 0xf00u
#define HDA_VERB_GET_CONNECTION_LIST 0xf02u
#define HDA_VERB_GET_POWER_STATE 0xf05u
#define HDA_VERB_GET_PIN_CAPS 0xf0cu
#define HDA_VERB_SET_AMP 0x300u
#define HDA_VERB_SET_STREAM_FORMAT 0x200u
#define HDA_VERB_SET_CONNECT_SEL 0x701u
#define HDA_VERB_SET_POWER_STATE 0x705u
#define HDA_VERB_SET_STREAM_CHANNEL 0x706u
#define HDA_VERB_SET_PIN_CONTROL 0x707u
#define HDA_VERB_SET_EAPD 0x70cu
#define HDA_PARAM_VENDOR_ID 0x00u
#define HDA_PARAM_NODE_COUNT 0x04u
#define HDA_PARAM_FUNCTION_TYPE 0x05u
#define HDA_PARAM_WIDGET_CAPS 0x09u
#define HDA_PARAM_PCM 0x0au
#define HDA_PARAM_STREAM_FORMATS 0x0bu
#define HDA_PARAM_PIN_CAPS 0x0cu
#define HDA_PARAM_CONNECTION_LENGTH 0x0eu
#define HDA_PARAM_OUTPUT_AMP_CAPS 0x12u
#define HDA_PARAM_INPUT_AMP_CAPS 0x0du

#define HDA_FG_AUDIO 0x01u
#define HDA_WIDGET_AUDIO_OUTPUT 0x00u
#define HDA_WIDGET_AUDIO_MIXER 0x02u
#define HDA_WIDGET_AUDIO_SELECTOR 0x03u
#define HDA_WIDGET_PIN 0x04u
#define HDA_WCAP_INPUT_AMP (1u << 1)
#define HDA_WCAP_OUTPUT_AMP (1u << 2)
#define HDA_WCAP_STEREO (1u << 0)
#define HDA_WCAP_CONNECTION_LIST (1u << 8)
#define HDA_WCAP_POWER (1u << 10)
#define HDA_WCAP_TYPE_SHIFT 20u
#define HDA_PINCAP_OUTPUT (1u << 4)
#define HDA_PINCAP_HP_DRIVE (1u << 3)
#define HDA_PINCAP_EAPD (1u << 16)
#define HDA_PIN_CONTROL_OUTPUT 0x40u
#define HDA_PIN_CONTROL_HEADPHONE 0x80u
#define HDA_EAPD_ENABLE 0x02u
#define HDA_AMP_OUTPUT 0x8000u
#define HDA_AMP_INPUT 0x4000u
#define HDA_AMP_LEFT 0x2000u
#define HDA_AMP_RIGHT 0x1000u
#define HDA_AMP_INDEX_SHIFT 8u
#define HDA_AMP_INDEX_MAX 0x0fu
#define HDA_AMP_MUTE 0x0080u
#define HDA_PCM_RATE_48K (1u << 6)
#define HDA_PCM_BITS_16 (1u << 17)
#define HDA_STREAM_FORMAT_PCM (1u << 0)
#define HDA_BDL_IOC 0x01u
#define HDA_CODEC_UNSOLICITED (1u << 4)
#define HDA_CODEC_ADDRESS_MASK 0x0fu
#define HDA_CODEC_COUNT 128u
#define HDA_ROUTE_MAX_CONNECTIONS 127u

struct hda_connection {
    uint8_t nid;
    uint8_t index;
};

struct hda_route {
    uint8_t parent[HDA_CODEC_COUNT];
    uint8_t parent_index[HDA_CODEC_COUNT];
    uint8_t seen[HDA_CODEC_COUNT];
    uint8_t queue[HDA_CODEC_COUNT];
};

_Static_assert(sizeof(struct hda_rirb_entry) == 8u, "RIRB entry size");
_Static_assert(sizeof(struct hda_bdl_entry) == 16u, "BDL entry size");
_Static_assert(offsetof(struct hda_dma, rirb) % HDA_DMA_ALIGNMENT == 0u,
    "RIRB alignment");
_Static_assert(offsetof(struct hda_dma, bdl) % HDA_DMA_ALIGNMENT == 0u,
    "BDL alignment");
_Static_assert(offsetof(struct hda_dma, samples) % HDA_DMA_ALIGNMENT == 0u,
    "sample alignment");

static void hda_relax(const struct hda_platform *p)
{
    p->relax(p->ctx);
}

static void hda_barrier(void)
{
    __sync_synchronize();
}

static uint8_t mmio_read8(const struct hda_device *d, uint32_t offset)
{
    return d->regs[offset];
}

static uint16_t mmio_read16(const struct hda_device *d, uint32_t offset)
{
    return *(volatile uint16_t *)(d->regs + offset);
}

static uint32_t mmio_read32(const struct hda_device *d, uint32_t offset)
{
    return *(volatile uint32_t *)(d->regs + offset);
}

static void mmio_write8(struct hda_device *d, uint32_t offset, uint8_t value)
{
    d->regs[offset] = value;
}

static void mmio_write16(struct hda_device *d, uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(d->regs + offset) = value;
}

static void mmio_write32(struct hda_device *d, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(d->regs + offset) = value;
}

static void hda_log(const struct hda_platform *p, const char *text)
{
    p->log(p->ctx, text);
}

const char *hda_status_name(enum hda_status status)
{
    switch (status) {
    case HDA_OK: return "ok";
    case HDA_ABSENT: return "controller absent";
    case HDA_BAD_ARGUMENT: return "invalid argument";
    case HDA_BAD_DEVICE: return "PCI identity/class mismatch";
    case HDA_PCI_ERROR: return "PCI command or snoop setup failed";
    case HDA_BAD_BAR: return "invalid firmware BAR";
    case HDA_MAP_FAILED: return "MMIO mapping failed";
    case HDA_DMA_ERROR: return "DMA address/alignment invalid";
    case HDA_RESET_TIMEOUT: return "controller reset timeout";
    case HDA_NO_CODEC: return "codec address 0 absent";
    case HDA_RING_ERROR: return "CORB/RIRB initialization failed";
    case HDA_VERB_TIMEOUT: return "codec verb timeout";
    case HDA_CODEC_UNSUPPORTED: return "unsupported codec or PCM path";
    case HDA_CODEC_ROUTE_ERROR: return "codec output route unavailable";
    case HDA_STREAM_ERROR: return "output stream setup failed";
    }
    return "unknown";
}

static bool pci_command_enable(const struct hda_platform *p,
    struct hda_device *d)
{
    uint32_t value = p->pci_read32(p->ctx, d->bus, d->slot, d->function, 4u);
    uint16_t command;

    if (value == UINT32_MAX)
        return false;
    command = (uint16_t)value;
    if ((command & 0x0006u) != 0x0006u) {
        p->pci_write16(p->ctx, d->bus, d->slot, d->function, 4u,
            (uint16_t)(command | 0x0006u));
        value = p->pci_read32(p->ctx, d->bus, d->slot, d->function, 4u);
        if (value == UINT32_MAX || (value & 0x0006u) != 0x0006u)
            return false;
    }
    return true;
}

static bool enable_ati_snoop(const struct hda_platform *p,
    struct hda_device *d)
{
    uint32_t config = p->pci_read32(p->ctx, d->bus, d->slot, d->function, 0x40u);
    uint16_t snoop;

    if (config == UINT32_MAX)
        return false;
    snoop = (uint16_t)(config >> 16u);
    if ((snoop & 0x0002u) == 0u) {
        snoop |= 0x0002u;
        p->pci_write16(p->ctx, d->bus, d->slot, d->function, 0x42u, snoop);
        config = p->pci_read32(p->ctx, d->bus, d->slot, d->function, 0x40u);
        if (config == UINT32_MAX || (config & 0x00020000u) == 0u)
            return false;
    }
    /* PCI config 0x44 and 0x4c are deliberately left untouched. */
    return true;
}

static bool hda_dma_addr(const struct hda_platform *p, const void *ptr,
    size_t size, uint64_t *address)
{
    return p->dma_address(p->ctx, ptr, size, address)
        && *address != 0u && *address < 0x100000000ull
        && size != 0u && size <= 0x100000000ull - *address;
}

static bool hda_wait_gctl(struct hda_device *d, uint32_t mask, bool set)
{
    for (uint32_t i = 0u; i < HDA_RESET_POLLS; ++i) {
        if (((mmio_read32(d, HDA_REG_GCTL) & mask) != 0u) == set)
            return true;
    }
    return false;
}

static bool controller_reset(const struct hda_platform *p,
    struct hda_device *d)
{
    uint32_t control = mmio_read32(d, HDA_REG_GCTL);

    mmio_write16(d, HDA_REG_STATESTS, 0x7fffu);
    mmio_write32(d, HDA_REG_GCTL, control & ~HDA_GCTL_CRST);
    hda_barrier();
    if (!hda_wait_gctl(d, HDA_GCTL_CRST, false))
        return false;
    for (uint32_t i = 0u; i < HDA_RESET_DELAY_POLLS; ++i)
        hda_relax(p);
    mmio_write32(d, HDA_REG_GCTL,
        (mmio_read32(d, HDA_REG_GCTL) | HDA_GCTL_CRST) & ~(1u << 8u));
    hda_barrier();
    if (!hda_wait_gctl(d, HDA_GCTL_CRST, true))
        return false;

    for (uint32_t i = 0u; i < HDA_RESET_POLLS; ++i) {
        if ((mmio_read16(d, HDA_REG_STATESTS) & 1u) != 0u)
            return true;
        hda_relax(p);
    }
    return false;
}

static bool ring_choose_size(uint8_t caps, uint8_t *selection,
    uint16_t *entries)
{
    caps &= HDA_RING_CAP_MASK;
    if ((caps & HDA_RING_CAP_256) != 0u) {
        *selection = HDA_RING_SIZE_256;
        *entries = 256u;
    } else if ((caps & HDA_RING_CAP_16) != 0u) {
        *selection = HDA_RING_SIZE_16;
        *entries = 16u;
    } else if ((caps & HDA_RING_CAP_2) != 0u) {
        *selection = HDA_RING_SIZE_2;
        *entries = 2u;
    } else {
        return false;
    }
    return true;
}

static bool reset_ring_pointer(const struct hda_platform *p,
    struct hda_device *d, uint32_t offset, uint16_t reset_bit)
{
    mmio_write16(d, offset, reset_bit);
    for (uint32_t i = 0u; i < 1000u; ++i) {
        uint16_t pointer = mmio_read16(d, offset);
        if ((pointer & reset_bit) == 0u && (pointer & 0x7fffu) == 0u)
            return true;
        hda_relax(p);
    }

    mmio_write16(d, offset, 0u);
    for (uint32_t i = 0u; i < HDA_RING_POLLS; ++i) {
        if ((mmio_read16(d, offset) & (uint16_t)(reset_bit | 0x7fffu)) == 0u)
            return true;
        hda_relax(p);
    }
    return false;
}

static enum hda_status ring_setup(const struct hda_platform *p,
    struct hda_device *d, struct hda_dma *dma, uint32_t gcaps)
{
    uint8_t corb_select, rirb_select;
    uint16_t corb_entries, rirb_entries;
    uint64_t corb_phys, rirb_phys;

    if (!ring_choose_size(mmio_read8(d, HDA_REG_CORBSIZE),
            &corb_select, &corb_entries)
        || !ring_choose_size(mmio_read8(d, HDA_REG_RIRBSIZE),
            &rirb_select, &rirb_entries)
        || corb_entries < 2u || rirb_entries < 2u) {
        return HDA_RING_ERROR;
    }
    if (!hda_dma_addr(p, dma->corb, sizeof(dma->corb), &corb_phys)
        || !hda_dma_addr(p, dma->rirb, sizeof(dma->rirb), &rirb_phys)
        || (corb_phys & (HDA_DMA_ALIGNMENT - 1u)) != 0u
        || (rirb_phys & (HDA_DMA_ALIGNMENT - 1u)) != 0u
        || ((gcaps & 1u) == 0u && ((corb_phys | rirb_phys) >> 32u) != 0u)) {
        return HDA_DMA_ERROR;
    }

    mmio_write8(d, HDA_REG_CORBCTL, 0u);
    mmio_write8(d, HDA_REG_RIRBCTL, 0u);
    for (uint32_t i = 0u; i < HDA_RING_POLLS; ++i) {
        if ((mmio_read8(d, HDA_REG_CORBCTL) & HDA_CORB_RUN) == 0u
            && (mmio_read8(d, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE) == 0u)
            break;
        if (i + 1u == HDA_RING_POLLS)
            return HDA_RING_ERROR;
        hda_relax(p);
    }

    for (size_t i = 0u; i < sizeof(dma->corb) / sizeof(uint32_t); ++i)
        dma->corb[i] = 0u;
    for (size_t i = 0u; i < sizeof(dma->rirb) / sizeof(dma->rirb[0]); ++i)
        dma->rirb[i] = (struct hda_rirb_entry){0};

    mmio_write8(d, HDA_REG_CORBSIZE, corb_select);
    mmio_write8(d, HDA_REG_RIRBSIZE, rirb_select);
    if ((mmio_read8(d, HDA_REG_CORBSIZE) & HDA_RING_SIZE_MASK) != corb_select
        || (mmio_read8(d, HDA_REG_RIRBSIZE) & HDA_RING_SIZE_MASK) != rirb_select) {
        return HDA_RING_ERROR;
    }
    mmio_write32(d, HDA_REG_CORBLBASE, (uint32_t)corb_phys);
    mmio_write32(d, HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32u));
    mmio_write32(d, HDA_REG_RIRBLBASE, (uint32_t)rirb_phys);
    mmio_write32(d, HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32u));

    mmio_write16(d, HDA_REG_CORBWP, 0u);
    if (!reset_ring_pointer(p, d, HDA_REG_CORBRP, HDA_CORBRP_RESET)
        || !reset_ring_pointer(p, d, HDA_REG_RIRBWP, HDA_RIRBWP_RESET)) {
        return HDA_RING_ERROR;
    }
    mmio_write8(d, HDA_REG_RIRBSTS, 0x05u);
    mmio_write8(d, HDA_REG_RIRBCTL, HDA_RIRB_DMA_ENABLE);
    mmio_write8(d, HDA_REG_CORBCTL, HDA_CORB_RUN);
    hda_barrier();
    if ((mmio_read8(d, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE) == 0u
        || (mmio_read8(d, HDA_REG_CORBCTL) & HDA_CORB_RUN) == 0u) {
        return HDA_RING_ERROR;
    }
    d->corb_entries = corb_entries;
    d->rirb_entries = rirb_entries;
    d->corb_wp = 0u;
    d->rirb_rp = 0u;
    return HDA_OK;
}

static uint32_t make_verb12(uint8_t nid, uint16_t verb, uint8_t payload)
{
    return ((uint32_t)HDA_CODEC_ADDRESS << 28u)
        | ((uint32_t)nid << 20u) | ((uint32_t)verb << 8u) | payload;
}

static uint32_t make_verb4(uint8_t nid, uint8_t verb, uint16_t payload)
{
    return ((uint32_t)HDA_CODEC_ADDRESS << 28u)
        | ((uint32_t)nid << 20u) | ((uint32_t)verb << 16u) | payload;
}

static bool send_verb(const struct hda_platform *p, struct hda_device *d,
    uint32_t command, uint32_t *response)
{
    uint16_t next = (uint16_t)((d->corb_wp + 1u) % d->corb_entries);
    uint16_t old_rirb = (uint16_t)(mmio_read16(d, HDA_REG_RIRBWP) & 0xffu);
    uint16_t corb_rp = (uint16_t)(mmio_read16(d, HDA_REG_CORBRP) & 0xffu);
    uint16_t rirb_mask = (uint16_t)(d->rirb_entries - 1u);

    if (next == corb_rp)
        return false;
    d->dma->corb[next] = command;
    hda_barrier();
    mmio_write16(d, HDA_REG_CORBWP, next);
    d->corb_wp = next;

    for (uint32_t i = 0u; i < HDA_VERB_POLLS; ++i) {
        uint16_t wp = (uint16_t)(mmio_read16(d, HDA_REG_RIRBWP) & 0xffu);

        while (d->rirb_rp != wp) {
            d->rirb_rp = (uint16_t)((d->rirb_rp + 1u) & rirb_mask);
            struct hda_rirb_entry entry = d->dma->rirb[d->rirb_rp];
            if ((entry.extended & HDA_CODEC_UNSOLICITED) != 0u)
                continue;
            if ((entry.extended & HDA_CODEC_ADDRESS_MASK)
                    != HDA_CODEC_ADDRESS) {
                continue;
            }
            if (response != NULL)
                *response = entry.response;
            return true;
        }
        if ((mmio_read16(d, HDA_REG_CORBRP) & 0xffu) == next
            && wp != old_rirb) {
            continue;
        }
        hda_relax(p);
    }
    d->command_timeout = true;
    return false;
}

static bool codec_parameter(const struct hda_platform *p,
    struct hda_device *d, uint8_t nid, uint8_t parameter, uint32_t *value)
{
    return send_verb(p, d,
        make_verb12(nid, HDA_VERB_GET_PARAMETER, parameter), value);
}

static bool codec_set12(const struct hda_platform *p, struct hda_device *d,
    uint8_t nid, uint16_t verb, uint8_t payload)
{
    return send_verb(p, d, make_verb12(nid, verb, payload), NULL);
}

static bool codec_set4(const struct hda_platform *p, struct hda_device *d,
    uint8_t nid, uint8_t verb, uint16_t payload)
{
    return send_verb(p, d, make_verb4(nid, verb, payload), NULL);
}

static bool codec_power_d0(const struct hda_platform *p,
    struct hda_device *d, uint8_t nid, uint32_t widget_caps)
{
    uint32_t state;

    if ((widget_caps & HDA_WCAP_POWER) == 0u)
        return true;
    if (!codec_set12(p, d, nid, HDA_VERB_SET_POWER_STATE, 0u))
        return false;
    for (uint32_t i = 0u; i < HDA_VERB_POLLS; ++i) {
        if (!send_verb(p, d, make_verb12(nid, HDA_VERB_GET_POWER_STATE, 0u),
                &state)) {
            return false;
        }
        if ((state & 0x0fu) == 0u && ((state >> 4u) & 0x0fu) == 0u)
            return true;
        hda_relax(p);
    }
    return false;
}

static bool codec_unmute_amp(const struct hda_platform *p,
    struct hda_device *d, uint8_t nid, bool output, uint8_t index)
{
    uint32_t amp_caps;
    uint8_t amp_parameter = output
        ? HDA_PARAM_OUTPUT_AMP_CAPS : HDA_PARAM_INPUT_AMP_CAPS;

    if (index > HDA_AMP_INDEX_MAX)
        return false;
    for (uint8_t i = 0u; i < d->amp_control_count; ++i) {
        const struct hda_amp_control *control = &d->amp_controls[i];
        if (control->nid == nid && control->index == index
            && control->output == output) {
            return true;
        }
    }
    if (!codec_parameter(p, d, nid, amp_parameter, &amp_caps))
        return false;
    if (d->amp_control_count >= HDA_MAX_AMP_CONTROLS)
        return false;
    struct hda_amp_control *control =
        &d->amp_controls[d->amp_control_count++];
    *control = (struct hda_amp_control){
        .nid = nid,
        .index = index,
        .steps = (uint8_t)((amp_caps >> 8u) & 0x7fu),
        .zero_db = (uint8_t)(amp_caps & 0x7fu),
        .output = output
    };
    uint16_t payload = (uint16_t)(output ? HDA_AMP_OUTPUT : HDA_AMP_INPUT);
    payload |= HDA_AMP_LEFT | HDA_AMP_RIGHT
        | ((uint16_t)index << HDA_AMP_INDEX_SHIFT) | control->zero_db;
    return codec_set4(p, d, nid, (uint8_t)(HDA_VERB_SET_AMP >> 8u), payload);
}

static enum hda_status apply_amp_state(const struct hda_platform *p,
    struct hda_device *d, uint8_t percent, bool muted)
{
    for (uint8_t i = 0u; i < d->amp_control_count; ++i) {
        const struct hda_amp_control *control = &d->amp_controls[i];
        uint8_t gain = (uint8_t)(((uint16_t)control->steps * percent + 50u)
            / 100u);
        uint16_t payload = (uint16_t)(control->output
            ? HDA_AMP_OUTPUT : HDA_AMP_INPUT);
        payload |= HDA_AMP_LEFT | HDA_AMP_RIGHT
            | ((uint16_t)control->index << HDA_AMP_INDEX_SHIFT) | gain;
        if (muted || percent == 0u)
            payload |= HDA_AMP_MUTE;
        if (!codec_set4(p, d, control->nid,
                (uint8_t)(HDA_VERB_SET_AMP >> 8u), payload)) {
            d->volume_controls_valid = false;
            return d->command_timeout ? HDA_VERB_TIMEOUT : HDA_RING_ERROR;
        }
    }
    return HDA_OK;
}

static bool get_connections(const struct hda_platform *p,
    struct hda_device *d, uint8_t nid, struct hda_connection *connections,
    uint8_t *connection_count)
{
    uint32_t parameter, packed;
    uint8_t encoded_count, previous = 0u, expanded = 0u;
    bool long_form;

    *connection_count = 0u;
    if (!codec_parameter(p, d, nid, HDA_PARAM_CONNECTION_LENGTH, &parameter))
        return false;
    encoded_count = (uint8_t)(parameter & 0x7fu);
    long_form = (parameter & 0x80u) != 0u;
    if (encoded_count > HDA_ROUTE_MAX_CONNECTIONS)
        return false;
    for (uint8_t start = 0u; start < encoded_count;) {
        uint8_t per_response = long_form ? 2u : 4u;
        uint8_t command_start = start;

        if (!send_verb(p, d, make_verb12(nid,
                HDA_VERB_GET_CONNECTION_LIST, command_start), &packed)) {
            return false;
        }
        for (uint8_t slot = 0u; slot < per_response && start < encoded_count;
             ++slot, ++start) {
            uint16_t raw = long_form
                ? (uint16_t)(packed >> (slot * 16u))
                : (uint8_t)(packed >> (slot * 8u));
            bool range = long_form ? (raw & 0x8000u) != 0u
                : (raw & 0x80u) != 0u;
            uint8_t target = (uint8_t)(raw & 0x7fu);

            if (range) {
                if (expanded == 0u || target <= previous)
                    return false;
                for (uint16_t node = (uint16_t)previous + 1u;
                     node <= target; ++node) {
                    if (expanded >= HDA_ROUTE_MAX_CONNECTIONS)
                        return false;
                    connections[expanded] = (struct hda_connection){
                        .nid = (uint8_t)node, .index = expanded};
                    ++expanded;
                }
            } else {
                if (expanded >= HDA_ROUTE_MAX_CONNECTIONS)
                    return false;
                connections[expanded] = (struct hda_connection){
                    .nid = target, .index = expanded};
                ++expanded;
            }
            previous = target;
        }
    }
    *connection_count = expanded;
    return true;
}

static bool find_audio_function_group(const struct hda_platform *p,
    struct hda_device *d, uint8_t *fg_nid, uint8_t *fg_start,
    uint8_t *fg_count)
{
    uint32_t nodes;
    uint8_t start, count;

    if (!codec_parameter(p, d, 0u, HDA_PARAM_NODE_COUNT, &nodes))
        return false;
    start = (uint8_t)((nodes >> 16u) & 0xffu);
    count = (uint8_t)(nodes & 0xffu);
    if (count == 0u || start >= HDA_CODEC_COUNT
        || (uint16_t)start + count > HDA_CODEC_COUNT) {
        return false;
    }
    for (uint16_t node = start; node < (uint16_t)start + count; ++node) {
        uint32_t type, child_nodes;
        if (!codec_parameter(p, d, (uint8_t)node,
                HDA_PARAM_FUNCTION_TYPE, &type)) {
            return false;
        }
        if ((type & 0xffu) != HDA_FG_AUDIO)
            continue;
        if (!codec_parameter(p, d, (uint8_t)node,
                HDA_PARAM_NODE_COUNT, &child_nodes)) {
            return false;
        }
        uint8_t child_start = (uint8_t)((child_nodes >> 16u) & 0xffu);
        uint8_t child_count = (uint8_t)(child_nodes & 0xffu);
        if (child_count == 0u || child_start >= HDA_CODEC_COUNT
            || (uint16_t)child_start + child_count > HDA_CODEC_COUNT) {
            return false;
        }
        *fg_nid = (uint8_t)node;
        *fg_start = child_start;
        *fg_count = child_count;
        return true;
    }
    return false;
}

static bool route_to_dac(const struct hda_platform *p, struct hda_device *d,
    uint8_t pin_nid, uint8_t first_nid, uint8_t node_count,
    uint8_t *dac_nid, struct hda_route *route)
{
    uint16_t tail = 0u, head = 0u;

    *route = (struct hda_route){0};
    if (pin_nid < first_nid || pin_nid >= (uint16_t)first_nid + node_count)
        return false;
    route->queue[tail++] = pin_nid;
    route->seen[pin_nid] = 1u;
    while (head < tail) {
        uint8_t current = route->queue[head++];
        uint32_t widget;
        if (!codec_parameter(p, d, current, HDA_PARAM_WIDGET_CAPS, &widget))
            return false;
        if (((widget >> HDA_WCAP_TYPE_SHIFT) & 0x0fu)
            == HDA_WIDGET_AUDIO_OUTPUT) {
            *dac_nid = current;
            return true;
        }
        if ((widget & HDA_WCAP_CONNECTION_LIST) == 0u)
            continue;
        struct hda_connection connections[HDA_ROUTE_MAX_CONNECTIONS];
        uint8_t count;
        if (!get_connections(p, d, current, connections, &count))
            return false;
        for (uint8_t i = 0u; i < count; ++i) {
            uint8_t source = connections[i].nid;
            if (source < first_nid
                || source >= (uint16_t)first_nid + node_count
                || route->seen[source] != 0u) {
                continue;
            }
            route->seen[source] = 1u;
            route->parent[source] = current;
            route->parent_index[source] = connections[i].index;
            route->queue[tail++] = source;
        }
    }
    return false;
}

static bool enable_route(const struct hda_platform *p,
    struct hda_device *d, uint8_t pin_nid, uint8_t dac_nid,
    const struct hda_route *route)
{
    uint8_t current = dac_nid;
    uint32_t widget, pin_caps;

    if (!codec_parameter(p, d, dac_nid, HDA_PARAM_WIDGET_CAPS, &widget)
        || !codec_power_d0(p, d, dac_nid, widget)
        || ((widget & HDA_WCAP_OUTPUT_AMP) != 0u
            && !codec_unmute_amp(p, d, dac_nid, true, 0u))) {
        return false;
    }
    while (current != pin_nid) {
        uint8_t downstream = route->parent[current];
        uint8_t index = route->parent_index[current];
        if (!codec_parameter(p, d, downstream, HDA_PARAM_WIDGET_CAPS, &widget)
            || !codec_power_d0(p, d, downstream, widget)) {
            return false;
        }
        if (((widget >> HDA_WCAP_TYPE_SHIFT) & 0x0fu)
            == HDA_WIDGET_AUDIO_SELECTOR
            && !codec_set12(p, d, downstream, HDA_VERB_SET_CONNECT_SEL,
                index)) {
            return false;
        }
        if ((widget & HDA_WCAP_INPUT_AMP) != 0u
            && !codec_unmute_amp(p, d, downstream, false, index)) {
            return false;
        }
        if ((widget & HDA_WCAP_OUTPUT_AMP) != 0u
            && !codec_unmute_amp(p, d, downstream, true, 0u)) {
            return false;
        }
        current = downstream;
    }
    if (!codec_parameter(p, d, pin_nid, HDA_PARAM_WIDGET_CAPS, &widget)
        || !codec_power_d0(p, d, pin_nid, widget)
        || !codec_parameter(p, d, pin_nid, HDA_PARAM_PIN_CAPS, &pin_caps)
        || (pin_caps & HDA_PINCAP_OUTPUT) == 0u) {
        return false;
    }
    if ((widget & HDA_WCAP_OUTPUT_AMP) != 0u
        && !codec_unmute_amp(p, d, pin_nid, true, 0u)) {
        return false;
    }
    if (!codec_set12(p, d, pin_nid, HDA_VERB_SET_PIN_CONTROL,
            (uint8_t)(HDA_PIN_CONTROL_OUTPUT
                | (pin_nid == HDA_PIN_FRONT_HEADPHONE
                    && (pin_caps & HDA_PINCAP_HP_DRIVE) != 0u
                    ? HDA_PIN_CONTROL_HEADPHONE : 0u)))) {
        return false;
    }
    if ((pin_caps & HDA_PINCAP_EAPD) != 0u
        && !codec_set12(p, d, pin_nid, HDA_VERB_SET_EAPD,
            HDA_EAPD_ENABLE)) {
        return false;
    }
    return true;
}

static bool codec_configure(const struct hda_platform *p,
    struct hda_device *d, uint8_t dac_nids[2], uint8_t *dac_count)
{
    uint8_t fg_nid, first_nid, node_count;
    uint32_t pcm_caps, stream_caps;
    struct hda_route rear_route, front_route;
    uint8_t rear_dac, front_dac;

    if (!codec_parameter(p, d, HDA_CODEC_ADDRESS, HDA_PARAM_VENDOR_ID,
            &d->codec_id)) {
        return false;
    }
    if ((d->codec_id >> 16u) != (HDA_CODEC_VIA_ID >> 16u))
        return false;
    if (!find_audio_function_group(p, d, &fg_nid, &first_nid, &node_count)) {
        return false;
    }
    if (!codec_power_d0(p, d, fg_nid, (1u << 10u))) {
        return false;
    }

    if (!route_to_dac(p, d, HDA_PIN_REAR_GREEN, first_nid, node_count,
            &rear_dac, &rear_route)
        || !route_to_dac(p, d, HDA_PIN_FRONT_HEADPHONE, first_nid, node_count,
            &front_dac, &front_route)) {
        return false;
    }
    for (uint8_t i = 0u; i < 2u; ++i) {
        uint8_t nid = i == 0u ? rear_dac : front_dac;
        if (i == 1u && front_dac == rear_dac)
            continue;
        uint32_t dac_caps;
        if (!codec_parameter(p, d, nid, HDA_PARAM_WIDGET_CAPS, &dac_caps)
            || (dac_caps & HDA_WCAP_STEREO) == 0u
            || !codec_parameter(p, d, nid, HDA_PARAM_PCM, &pcm_caps)
            || !codec_parameter(p, d, nid, HDA_PARAM_STREAM_FORMATS,
                &stream_caps)
            || (pcm_caps & (HDA_PCM_RATE_48K | HDA_PCM_BITS_16))
                != (HDA_PCM_RATE_48K | HDA_PCM_BITS_16)
            || (stream_caps & HDA_STREAM_FORMAT_PCM) == 0u) {
            return false;
        }
    }

    if (!enable_route(p, d, HDA_PIN_REAR_GREEN, rear_dac, &rear_route)
        || !enable_route(p, d, HDA_PIN_FRONT_HEADPHONE, front_dac,
            &front_route)) {
        return false;
    }
    dac_nids[0] = rear_dac;
    *dac_count = 1u;
    if (front_dac != rear_dac)
        dac_nids[(*dac_count)++] = front_dac;
    return true;
}

static enum hda_status setup_output_stream(const struct hda_platform *p,
    struct hda_device *d, struct hda_dma *dma, const uint8_t *dac_nids,
    uint8_t dac_count, uint32_t gcaps)
{
    uint64_t bdl_phys, sample_phys[HDA_DMA_BLOCK_COUNT];
    uint8_t input_streams = (uint8_t)((gcaps & HDA_GCAP_ISS_MASK) >> 8u);
    uint8_t output_streams = (uint8_t)((gcaps & HDA_GCAP_OSS_MASK) >> 12u);
    uint32_t stream_offset, ctl;

    if (output_streams == 0u)
        return HDA_STREAM_ERROR;
    if (!hda_dma_addr(p, dma->bdl, sizeof(dma->bdl), &bdl_phys)
        || (bdl_phys & (HDA_DMA_ALIGNMENT - 1u)) != 0u
        || ((gcaps & 1u) == 0u && (bdl_phys >> 32u) != 0u)) {
        return HDA_DMA_ERROR;
    }
    for (uint32_t i = 0u; i < HDA_DMA_BLOCK_COUNT; ++i) {
        if (!hda_dma_addr(p, dma->samples[i], HDA_DMA_BLOCK_SIZE,
                &sample_phys[i])
            || (sample_phys[i] & (HDA_DMA_ALIGNMENT - 1u)) != 0u
            || ((gcaps & 1u) == 0u && (sample_phys[i] >> 32u) != 0u)) {
            return HDA_DMA_ERROR;
        }
        for (uint32_t j = 0u; j < HDA_DMA_BLOCK_SIZE; ++j)
            dma->samples[i][j] = 0u;
        dma->bdl[i] = (struct hda_bdl_entry){
            .address_low = (uint32_t)sample_phys[i],
            .address_high = (uint32_t)(sample_phys[i] >> 32u),
            .length = HDA_DMA_BLOCK_SIZE,
            .flags = HDA_BDL_IOC
        };
    }
    stream_offset = HDA_SD_BASE + (uint32_t)input_streams * HDA_SD_STRIDE;
    if (stream_offset + HDA_SD_STRIDE > HDA_MMIO_SIZE)
        return HDA_STREAM_ERROR;
    d->stream_offset = stream_offset;

    ctl = mmio_read32(d, stream_offset + HDA_SD_CTL);
    mmio_write8(d, stream_offset + HDA_SD_CTL,
        (uint8_t)(ctl & ~HDA_SD_RUN));
    if ((mmio_read8(d, stream_offset + HDA_SD_CTL) & HDA_SD_RUN) != 0u)
        return HDA_STREAM_ERROR;
    mmio_write8(d, stream_offset + HDA_SD_CTL,
        (uint8_t)(mmio_read8(d, stream_offset + HDA_SD_CTL) | HDA_SD_SRST));
    for (uint32_t i = 0u; i < HDA_STREAM_POLLS; ++i) {
        if ((mmio_read8(d, stream_offset + HDA_SD_CTL) & HDA_SD_SRST) != 0u)
            break;
        if (i + 1u == HDA_STREAM_POLLS)
            return HDA_STREAM_ERROR;
        hda_relax(p);
    }
    mmio_write8(d, stream_offset + HDA_SD_CTL,
        (uint8_t)(mmio_read8(d, stream_offset + HDA_SD_CTL) & ~HDA_SD_SRST));
    for (uint32_t i = 0u; i < HDA_STREAM_POLLS; ++i) {
        if ((mmio_read8(d, stream_offset + HDA_SD_CTL) & HDA_SD_SRST) == 0u)
            break;
        if (i + 1u == HDA_STREAM_POLLS)
            return HDA_STREAM_ERROR;
        hda_relax(p);
    }

    mmio_write8(d, stream_offset + HDA_SD_STATUS, HDA_SD_STATUS_MASK);
    mmio_write32(d, stream_offset + HDA_SD_CTL,
        (uint32_t)HDA_STREAM_TAG << 20u);
    mmio_write32(d, stream_offset + HDA_SD_CBL,
        HDA_DMA_BLOCK_SIZE * HDA_DMA_BLOCK_COUNT);
    mmio_write16(d, stream_offset + HDA_SD_LVI,
        (uint16_t)(HDA_DMA_BLOCK_COUNT - 1u));
    mmio_write16(d, stream_offset + HDA_SD_FORMAT,
        HDA_PCM_48K_STEREO_S16);
    mmio_write32(d, stream_offset + HDA_SD_BDLPL, (uint32_t)bdl_phys);
    mmio_write32(d, stream_offset + HDA_SD_BDLPU,
        (uint32_t)(bdl_phys >> 32u));
    hda_barrier();

    for (uint8_t i = 0u; i < dac_count; ++i) {
        if (!codec_set4(p, d, dac_nids[i],
                (uint8_t)(HDA_VERB_SET_STREAM_FORMAT >> 8u),
                HDA_PCM_48K_STEREO_S16)
            || !codec_set12(p, d, dac_nids[i],
                HDA_VERB_SET_STREAM_CHANNEL,
                (uint8_t)(HDA_STREAM_TAG << 4u))) {
            return HDA_STREAM_ERROR;
        }
    }
    ctl = mmio_read32(d, stream_offset + HDA_SD_CTL);
    mmio_write32(d, stream_offset + HDA_SD_CTL, ctl & ~0x1fu);
    mmio_write8(d, stream_offset + HDA_SD_CTL,
        (uint8_t)(mmio_read8(d, stream_offset + HDA_SD_CTL) | HDA_SD_RUN));
    hda_barrier();
    if ((mmio_read8(d, stream_offset + HDA_SD_CTL) & HDA_SD_RUN) == 0u)
        return HDA_STREAM_ERROR;
    d->running = true;
    return HDA_OK;
}

enum hda_status hda_init(const struct hda_platform *p,
    struct hda_device *d, struct hda_dma *dma,
    uint8_t bus, uint8_t slot, uint8_t function)
{
    uint32_t identity, class_revision, bar_low, bar_high = 0u, gcaps;
    uint64_t base;
    uint8_t dac_nids[2], dac_count;

    if (p == NULL || d == NULL || dma == NULL
        || p->pci_read32 == NULL || p->pci_write16 == NULL
        || p->map_mmio == NULL || p->dma_address == NULL
        || p->relax == NULL || p->log == NULL || slot >= 32u
        || function >= 8u) {
        return HDA_BAD_ARGUMENT;
    }
    *d = (struct hda_device){.bus = bus, .slot = slot, .function = function,
        .volume_controls_valid = true, .volume_percent = 70u, .dma = dma};
    identity = p->pci_read32(p->ctx, bus, slot, function, 0u);
    if ((uint16_t)identity == 0xffffu || (uint16_t)identity == 0u)
        return HDA_ABSENT;
    class_revision = p->pci_read32(p->ctx, bus, slot, function, 8u);
    if ((uint16_t)identity != HDA_ATI_VENDOR_ID
        || (uint16_t)(identity >> 16u) != HDA_ATI_DEVICE_ID
        || (class_revision >> 16u) != 0x0403u) {
        return HDA_BAD_DEVICE;
    }
    d->present = true;
    if (!pci_command_enable(p, d) || !enable_ati_snoop(p, d))
        return HDA_PCI_ERROR;

    bar_low = p->pci_read32(p->ctx, bus, slot, function, 0x10u);
    if (bar_low == UINT32_MAX || (bar_low & 1u) != 0u)
        return HDA_BAD_BAR;
    uint32_t bar_type = (bar_low >> 1u) & 3u;
    if (bar_type == 2u) {
        bar_high = p->pci_read32(p->ctx, bus, slot, function, 0x14u);
        if (bar_high == UINT32_MAX)
            return HDA_BAD_BAR;
    } else if (bar_type != 0u) {
        return HDA_BAD_BAR;
    }
    base = ((uint64_t)bar_high << 32u) | (bar_low & ~0x0fu);
    if (base == 0u || base >= 0x100000000ull
        || HDA_MMIO_SIZE > 0x100000000ull - base) {
        return HDA_BAD_BAR;
    }
    d->mmio_base = base;
    d->regs = p->map_mmio(p->ctx, base, HDA_MMIO_SIZE, true);
    if (d->regs == NULL)
        return HDA_MAP_FAILED;

    mmio_write32(d, HDA_REG_INTCTL, 0u);
    mmio_write32(d, HDA_REG_INTSTS, 0xffffffffu);
    if (!controller_reset(p, d))
        return HDA_RESET_TIMEOUT;
    gcaps = mmio_read16(d, HDA_REG_GCAP);
    d->gcap = gcaps;
    if ((gcaps & HDA_GCAP_OSS_MASK) == 0u)
        return HDA_STREAM_ERROR;
    if ((mmio_read16(d, HDA_REG_STATESTS) & 1u) == 0u)
        return HDA_NO_CODEC;

    enum hda_status status = ring_setup(p, d, dma, gcaps);
    if (status != HDA_OK)
        return status;
    if (!codec_configure(p, d, dac_nids, &dac_count)) {
        if (d->command_timeout)
            return HDA_VERB_TIMEOUT;
        if (d->codec_id != 0u
            && (d->codec_id >> 16u) != (HDA_CODEC_VIA_ID >> 16u)) {
            return HDA_CODEC_UNSUPPORTED;
        }
        return HDA_CODEC_ROUTE_ERROR;
    }
    status = apply_amp_state(p, d, d->volume_percent, d->muted);
    if (status != HDA_OK)
        return status;
    status = setup_output_stream(p, d, dma, dac_nids, dac_count, gcaps);
    if (status != HDA_OK) {
        if (d->command_timeout)
            return HDA_VERB_TIMEOUT;
        return status;
    }
    hda_log(p, "HDA: ATI SB7xx controller, VIA codec, 48 kHz stereo output active");
    return HDA_OK;
}

bool hda_is_available(const struct hda_device *d)
{
    return d != NULL && d->running && d->volume_controls_valid
        && d->amp_control_count != 0u;
}

uint8_t hda_get_volume(const struct hda_device *d)
{
    return d != NULL ? d->volume_percent : 0u;
}

bool hda_get_mute(const struct hda_device *d)
{
    return d != NULL && d->muted;
}

static bool runtime_args_valid(const struct hda_platform *p,
    const struct hda_device *d)
{
    return p != NULL && p->relax != NULL && d != NULL && d->regs != NULL
        && d->dma != NULL && hda_is_available(d);
}

enum hda_status hda_set_volume(const struct hda_platform *p,
    struct hda_device *d, uint8_t percent)
{
    if (percent > 100u)
        return HDA_BAD_ARGUMENT;
    if (!runtime_args_valid(p, d))
        return HDA_CODEC_ROUTE_ERROR;
    enum hda_status status = apply_amp_state(p, d, percent, d->muted);
    if (status == HDA_OK)
        d->volume_percent = percent;
    return status;
}

enum hda_status hda_set_mute(const struct hda_platform *p,
    struct hda_device *d, bool muted)
{
    if (!runtime_args_valid(p, d))
        return HDA_CODEC_ROUTE_ERROR;
    enum hda_status status = apply_amp_state(p, d, d->volume_percent, muted);
    if (status == HDA_OK)
        d->muted = muted;
    return status;
}
