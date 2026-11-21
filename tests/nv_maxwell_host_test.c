#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/nv_maxwell.h"

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

/* ---- fake PCI configuration space ------------------------------------- */

static uint32_t cfg[64];
static uint32_t cfg_fn1[64];
static bool fn1_present;
static bool command_stuck;
static uint32_t bar_mask[6];
static unsigned map_calls;
static unsigned unmap_calls;
static unsigned pci_write_calls;

#define UNMAP_LOG_MAX 32u
struct unmap_record {
    volatile uint8_t *ptr;
    uint64_t size;
};
static struct unmap_record unmap_log[UNMAP_LOG_MAX];

static void reset_unmap_log(void)
{
    unmap_calls = 0u;
    memset(unmap_log, 0, sizeof(unmap_log));
}

static uint32_t t_read(void *ctx, uint8_t bus, uint8_t slot, uint8_t fn,
    uint8_t off)
{
    (void)ctx;
    if (bus == 1u && slot == 0u) {
        if (fn == 0u) {
            return cfg[off / 4u];
        }
        if (fn == 1u && fn1_present) {
            return cfg_fn1[off / 4u];
        }
    }
    return 0xffffffffu;
}

static void t_write(void *ctx, uint8_t bus, uint8_t slot, uint8_t fn,
    uint8_t off, uint32_t value)
{
    (void)ctx;
    ++pci_write_calls;
    if (bus != 1u || slot != 0u) {
        return;
    }
    uint32_t *space;
    if (fn == 0u) {
        space = cfg;
    } else if (fn == 1u && fn1_present) {
        space = cfg_fn1;
    } else {
        return;
    }
    unsigned i = off / 4u;
    if (i == 1u && command_stuck) {
        return;
    }
    if (i >= 4u && i <= 9u && value == 0xffffffffu) {
        space[i] = bar_mask[i - 4u];
    } else {
        space[i] = value;
    }
}

/* ---- fake BAR0 / VRAM / BAR1 USERD ------------------------------------ */

static uint8_t bar0_mem[0x700000];
static uint8_t fake_bar0[0x900000];
static uint8_t fake_vram[0x800000];
static uint32_t fake_userd[1024] __attribute__((aligned(4096)));
static uint32_t log_lines;

#define FAKE_USERD_VRAM 0x600000ull

#define SAVED_CHAN_CTRL     0x12345678u
#define SAVED_CHAN_INST     0x87654321u
#define SAVED_RUNLIST_BASE  0x000000abu
#define SAVED_RUNLIST_COUNT 0u
#define SAVED_SCHED_DISABLE 0x0000000fu
#define SAVED_BAR1_USERD    0x00abcdefu
#define SAVED_PFIFO_INTR    0x00000055u
#define SAVED_PMC_INTR_EN   0x000000ffu
#define SAVED_PMC_DEV       0x00000003u
#define SAVED_BAR1_BIND     0x80000123u
#define SAVED_PRAMIN        0x00000055u
#define SAVED_PMC_ENABLE    0xc0012121u

static void put32(uint8_t *base, uint32_t off, uint32_t value)
{
    memcpy(base + off, &value, sizeof(value));
}

static uint32_t reg(uint32_t off)
{
    uint32_t v;
    memcpy(&v, fake_bar0 + off, sizeof(v));
    return v;
}

static uint32_t vram32(uint64_t addr)
{
    uint32_t v;
    memcpy(&v, fake_vram + addr, sizeof(v));
    return v;
}

static uint64_t vram64(uint64_t addr)
{
    return (uint64_t)vram32(addr) | ((uint64_t)vram32(addr + 4u) << 32u);
}

static void seed_bar0(void)
{
    static const uint32_t ptop[NV_PTOP_ENTRIES] = {
        0x80000007u, /* CE engine type, more words follow */
        0xc4400032u, /* engine 1, runlist 2, more words follow */
        0x00000001u, /* data word: end of device */
    };

    memset(fake_bar0, 0, sizeof(fake_bar0));
    fake_bar0[0] = 0xa1;
    fake_bar0[1] = 0x20;
    fake_bar0[2] = 0x40;
    fake_bar0[3] = 0x12;
    put32(fake_bar0, NV_PMC_ENABLE, SAVED_PMC_ENABLE);
    put32(fake_bar0, NV_MMU_FLUSH_STATUS, 0x00ff8000u);
    put32(fake_bar0, NV_PFB_CONFIG, 0x12345678u);
    put32(fake_bar0, NV_PFB_CSTATUS, 0x80000000u);
    put32(fake_bar0, NV_PDISP_HEAD_MASK, 1u);
    put32(fake_bar0, NV_PDISP_HEAD_TIMING(0u), (525u << 16u) | 800u);

    for (uint32_t i = 0u; i < NV_PTOP_ENTRIES; ++i) {
        put32(fake_bar0, NV_PTOP_INFO(i), ptop[i]);
    }

    put32(fake_bar0, NV_PFIFO_CHAN_CTRL(NV_CH_ID), SAVED_CHAN_CTRL);
    put32(fake_bar0, NV_PFIFO_CHAN_INST(NV_CH_ID), SAVED_CHAN_INST);
    put32(fake_bar0, NV_PFIFO_RUNLIST_BASE, SAVED_RUNLIST_BASE);
    put32(fake_bar0, NV_PFIFO_RUNLIST_COUNT, SAVED_RUNLIST_COUNT);
    put32(fake_bar0, NV_PFIFO_SCHED_DISABLE, SAVED_SCHED_DISABLE);
    put32(fake_bar0, NV_PFIFO_BAR1_USERD, SAVED_BAR1_USERD);
    put32(fake_bar0, NV_PFIFO_INTR, SAVED_PFIFO_INTR);
    for (uint32_t p = 0u; p < NV_PBDMA_MAX; ++p) {
        put32(fake_bar0, NV_PBDMA_CTRL13C(p), 0x00000100u | p);
        put32(fake_bar0, NV_PBDMA_INTR(p), 0x000000a0u | p);
    }
    put32(fake_bar0, NV_PMC_INTR_EN_0, SAVED_PMC_INTR_EN);
    put32(fake_bar0, NV_PMC_DEV_ENABLE, SAVED_PMC_DEV);
    put32(fake_bar0, NV_BUS_BAR1_BIND, SAVED_BAR1_BIND);
    put32(fake_bar0, NV_PRAMIN_WINDOW, SAVED_PRAMIN);
}

static void setup_cfg(uint16_t device)
{
    memset(cfg, 0, sizeof(cfg));
    cfg[0] = 0x10deu | ((uint32_t)device << 16u);
    cfg[1] = 0x00100003u; /* status=0x0010xxxx, command=0x0003 */
    cfg[2] = 0x03000000u | 0xa1u;
    cfg[3] = 0u;
    cfg[4] = 0xf6000000u;
    cfg[5] = 0xe000000cu;
    cfg[6] = 0u;
    bar_mask[0] = 0xff000000u;
    bar_mask[1] = 0xf000000cu;
    bar_mask[2] = 0xffffffffu;

    memset(bar0_mem, 0, sizeof(bar0_mem));
    bar0_mem[0] = 0xa1;
    bar0_mem[1] = 0x20;
    bar0_mem[2] = 0x40;
    bar0_mem[3] = 0x12;
    put32(bar0_mem, NV_PMC_ENABLE, SAVED_PMC_ENABLE);
    put32(bar0_mem, NV_MMU_FLUSH_STATUS, 0x00ff8000u);
    put32(bar0_mem, NV_PFB_CONFIG, 0x12345678u);
    put32(bar0_mem, NV_PFB_CSTATUS, 0x80000000u);
    put32(bar0_mem, NV_PDISP_HEAD_MASK, 1u);
    put32(bar0_mem, NV_PDISP_HEAD_TIMING(0u), (525u << 16u) | 800u);

    seed_bar0();
}

static volatile uint8_t *t_map(void *ctx, uint64_t phys, uint64_t size,
    bool uncached)
{
    (void)ctx;
    (void)size;
    (void)uncached;
    ++map_calls;
    return phys == 0xf6000000ull ? bar0_mem : bar0_mem + 0x800;
}

static volatile uint8_t *c_map(void *ctx, uint64_t phys, uint64_t size,
    bool uncached)
{
    (void)ctx;
    (void)uncached;
    ++map_calls;
    if (phys == 0xf6000000ull && size == NV_GM204_BAR0_SIZE) {
        return (volatile uint8_t *)fake_bar0;
    }
    if (phys == 0xe0000000ull && size == NV_GM204_BAR1_SIZE) {
        /* BAR1 aliases VRAM in the fake, so framebuffer cache maintenance
         * touches a real, sufficiently large backing array. */
        return (volatile uint8_t *)fake_vram;
    }
    return (volatile uint8_t *)fake_userd; /* one-page USERD BAR1 window */
}

static void c_unmap(void *ctx, volatile uint8_t *ptr, uint64_t size)
{
    (void)ctx;
    if (unmap_calls < UNMAP_LOG_MAX) {
        unmap_log[unmap_calls].ptr = ptr;
        unmap_log[unmap_calls].size = size;
    }
    ++unmap_calls;
}

static uint32_t c_vrd(void *ctx, uint64_t addr)
{
    (void)ctx;
    if (addr >= FAKE_USERD_VRAM && addr < FAKE_USERD_VRAM + 0x1000u) {
        uint32_t v;
        memcpy(&v, (uint8_t *)fake_userd + (addr - FAKE_USERD_VRAM),
            sizeof(v));
        return v;
    }
    return vram32(addr);
}

static bool fake_gpu_runs;
static bool fake_clobber_bar1_spt;
static uint64_t fake_bind_base;
static uint64_t fake_scanout_vram;
static uint32_t fake_gp_get;
static uint32_t fake_seen_class;
static uint32_t fake_bad_va;
static uint32_t fake_bad_sem;
static uint32_t fake_fills;
static uint32_t fake_copies;

static void c_vwr(void *ctx, uint64_t addr, uint32_t value)
{
    (void)ctx;
    /* Fault injection: channel-SPT construction must not destroy the BAR1
     * identity PTEs that nv_build_bar1() installed first. */
    if (fake_clobber_bar1_spt && fake_bind_base != 0u
        && addr >= fake_bind_base + NV_CH_SPT
        && addr < fake_bind_base + NV_CH_SPT + NV_CH_SPT_BYTES) {
        uint64_t page = fake_scanout_vram >> 12u;
        uint64_t spt = fake_bind_base
            + (page < 0x8000u ? NV_B1_SPT0 : NV_B1_SPT1);
        put32(fake_vram, spt + (page & 0x7fffu) * 8u, 0u);
    }
    if (addr >= FAKE_USERD_VRAM && addr < FAKE_USERD_VRAM + 0x1000u) {
        memcpy((uint8_t *)fake_userd + (addr - FAKE_USERD_VRAM), &value,
            sizeof(value));
        return;
    }
    put32(fake_vram, addr, value);
}

static void c_log(void *ctx, const char *label, uint32_t value)
{
    (void)ctx;
    (void)label;
    (void)value;
    ++log_lines;
}

/* ---- fake copy engine -------------------------------------------------- */

struct fake_ce_state {
    uint32_t sem_hi;
    uint32_t sem_lo;
    uint32_t sem_payload;
    uint32_t remap_a;
    uint32_t remap_b;
    uint32_t remap_comp;
    uint64_t src_va;
    uint64_t dst_va;
    uint32_t src_pitch;
    uint32_t dst_pitch;
    uint32_t line_len;
    uint32_t lines;
    uint32_t launch;
};

static struct fake_ce_state fake_ce;

static void fake_reset(struct nv_gpu *gpu, uint64_t base)
{
    fake_gp_get = 0u;
    fake_seen_class = 0u;
    fake_bad_va = 0u;
    fake_bad_sem = 0u;
    fake_fills = 0u;
    fake_copies = 0u;
    fake_bind_base = base;
    fake_scanout_vram = gpu->scanout.vram_offset & ~0xfffull;
    fake_clobber_bar1_spt = false;
    memset(&fake_ce, 0, sizeof(fake_ce));
}

static uint8_t *fake_va_ptr(struct nv_gpu *gpu, uint64_t va, uint64_t bytes)
{
    uint64_t off;

    if (va >= NV_CH_VA_SCRATCH && va < NV_CH_VA_FB) {
        off = va - NV_CH_VA_SCRATCH;
        if (off > NV_CH_SCRATCH_BYTES
            || bytes > NV_CH_SCRATCH_BYTES - off) {
            ++fake_bad_va;
            return NULL;
        }
        return fake_vram + gpu->vram_base + NV_CH_SCRATCH + off;
    }
    if (va >= NV_CH_VA_FB) {
        uint64_t fb_base = gpu->scanout.vram_offset & ~0xfffull;
        uint64_t fb_size = (uint64_t)gpu->scanout.pitch * gpu->scanout.height;
        off = va - NV_CH_VA_FB;
        if (off > fb_size || bytes > fb_size - off) {
            ++fake_bad_va;
            return NULL;
        }
        return fake_vram + fb_base + off;
    }
    ++fake_bad_va;
    return NULL;
}

static void fake_launch(struct nv_gpu *gpu)
{
    bool fill = (fake_ce.launch & NVB0B5_LAUNCH_REMAP) != 0u;
    uint32_t row_bytes = fill ? fake_ce.line_len * 4u : fake_ce.line_len;
    uint64_t span;
    uint8_t *dst;
    uint8_t *src = NULL;

    if (fake_ce.lines == 0u || row_bytes == 0u) {
        return;
    }
    span = (uint64_t)(fake_ce.lines - 1u) * fake_ce.dst_pitch + row_bytes;
    dst = fake_va_ptr(gpu, fake_ce.dst_va, span);
    if (!fill) {
        uint64_t src_span = (uint64_t)(fake_ce.lines - 1u)
            * fake_ce.src_pitch + row_bytes;
        src = fake_va_ptr(gpu, fake_ce.src_va, src_span);
    }
    if (dst == NULL || (!fill && src == NULL)) {
        return;
    }
    for (uint32_t r = 0u; r < fake_ce.lines; ++r) {
        uint8_t *out = dst + (uint64_t)r * fake_ce.dst_pitch;
        if (fill) {
            for (uint32_t i = 0u; i < fake_ce.line_len; ++i) {
                memcpy(out + (uint64_t)i * 4u, &fake_ce.remap_a, 4u);
            }
        } else {
            memmove(out, src + (uint64_t)r * fake_ce.src_pitch, row_bytes);
        }
    }
    if (fill) {
        ++fake_fills;
    } else {
        ++fake_copies;
    }

    if ((fake_ce.launch & NVB0B5_LAUNCH_SEM_ONE_WORD) != 0u) {
        uint64_t sem = ((uint64_t)(fake_ce.sem_hi & 0xffu) << 32u)
            | fake_ce.sem_lo;
        if (gpu->fence != NULL && sem == gpu->fence_gpu_addr) {
            gpu->fence[0] = fake_ce.sem_payload;
        } else {
            ++fake_bad_sem;
        }
    }
}

static void fake_apply(struct nv_gpu *gpu, uint32_t subchannel,
    uint32_t address, uint32_t value)
{
    if (address == NV_HOST_SET_OBJECT) {
        fake_seen_class = value;
        return;
    }
    if (address == NV_HOST_NOP) {
        return;
    }
    if (subchannel != NV_SUBCHANNEL_CE) {
        return;
    }
    switch (address) {
    case NVB0B5_SET_SEMAPHORE_A:
        fake_ce.sem_hi = value;
        break;
    case NVB0B5_SET_SEMAPHORE_B:
        fake_ce.sem_lo = value;
        break;
    case NVB0B5_SET_SEMAPHORE_PAYLOAD:
        fake_ce.sem_payload = value;
        break;
    case NVB0B5_SET_REMAP_CONST_A:
        fake_ce.remap_a = value;
        break;
    case NVB0B5_SET_REMAP_CONST_B:
        fake_ce.remap_b = value;
        break;
    case NVB0B5_SET_REMAP_COMPONENTS:
        fake_ce.remap_comp = value;
        break;
    case NVB0B5_OFFSET_IN_UPPER:
        fake_ce.src_va = (fake_ce.src_va & 0xffffffffu)
            | ((uint64_t)(value & 0xffu) << 32u);
        break;
    case NVB0B5_OFFSET_IN_LOWER:
        fake_ce.src_va = (fake_ce.src_va & ~0xffffffffull) | value;
        break;
    case NVB0B5_OFFSET_OUT_UPPER:
        fake_ce.dst_va = (fake_ce.dst_va & 0xffffffffu)
            | ((uint64_t)(value & 0xffu) << 32u);
        break;
    case NVB0B5_OFFSET_OUT_LOWER:
        fake_ce.dst_va = (fake_ce.dst_va & ~0xffffffffull) | value;
        break;
    case NVB0B5_PITCH_IN:
        fake_ce.src_pitch = value;
        break;
    case NVB0B5_PITCH_OUT:
        fake_ce.dst_pitch = value;
        break;
    case NVB0B5_LINE_LENGTH_IN:
        fake_ce.line_len = value;
        break;
    case NVB0B5_LINE_COUNT:
        fake_ce.lines = value;
        break;
    case NVB0B5_LAUNCH_DMA:
        fake_ce.launch = value;
        fake_launch(gpu);
        break;
    default:
        break;
    }
}

static void fake_execute(struct nv_gpu *gpu, const uint32_t *words,
    uint32_t count)
{
    uint32_t i = 0u;

    while (i < count) {
        uint32_t header = words[i++];
        uint32_t op = header >> 29u;
        uint32_t data_count = (header >> 16u) & 0x1fffu;
        uint32_t subchannel = (header >> 13u) & 7u;
        uint32_t method = (header & 0xfffu) << 2u;

        if (op != NV_PKT_INC && op != NV_PKT_NONINC
            && op != NV_PKT_INC_ONCE) {
            i += data_count;
            continue;
        }
        if (data_count > count - i) {
            ++fake_bad_va;
            return;
        }
        for (uint32_t k = 0u; k < data_count; ++k) {
            uint32_t address = method
                + (op == NV_PKT_INC ? k * 4u : 0u);
            fake_apply(gpu, subchannel, address, words[i + k]);
        }
        i += data_count;
    }
}

static void fake_pump(struct nv_gpu *gpu)
{
    uint32_t put;

    if (!fake_gpu_runs || gpu->gpfifo == NULL || gpu->userd == NULL
        || gpu->pushbuf == NULL) {
        return;
    }
    put = gpu->userd[NV_USERD_GP_PUT / 4u]
        & (gpu->gpfifo_entries - 1u);
    while (fake_gp_get != put) {
        uint64_t entry = gpu->gpfifo[fake_gp_get];
        uint32_t words = (uint32_t)((entry >> 42u) & 0x1fffffu);
        uint64_t va = (entry & 0xffffffffu)
            | (((entry >> 32u) & 0xffu) << 32u);
        if (va >= gpu->pushbuf_gpu_addr) {
            uint64_t offset = (va - gpu->pushbuf_gpu_addr) / 4u;
            if (offset + words <= gpu->pushbuf_words) {
                fake_execute(gpu, gpu->pushbuf + offset, words);
            } else {
                ++fake_bad_va;
            }
        } else {
            ++fake_bad_va;
        }
        fake_gp_get = (fake_gp_get + 1u) & (gpu->gpfifo_entries - 1u);
        gpu->userd[NV_USERD_GP_GET / 4u] = fake_gp_get;
    }
}

static void c_relax(void *ctx)
{
    fake_pump((struct nv_gpu *)ctx);
}

/* ---- assertions over driver state ------------------------------------- */

static void verify_restored_state(void)
{
    CHECK(reg(NV_PFIFO_CHAN_CTRL(NV_CH_ID)) == SAVED_CHAN_CTRL);
    CHECK(reg(NV_PFIFO_CHAN_INST(NV_CH_ID)) == SAVED_CHAN_INST);
    CHECK(reg(NV_PFIFO_RUNLIST_BASE) == SAVED_RUNLIST_BASE);
    CHECK(reg(NV_PFIFO_RUNLIST_COUNT) == SAVED_RUNLIST_COUNT);
    CHECK(reg(NV_PFIFO_SCHED_DISABLE) == SAVED_SCHED_DISABLE);
    CHECK(reg(NV_PFIFO_BAR1_USERD) == SAVED_BAR1_USERD);
    CHECK(reg(NV_PFIFO_INTR) == SAVED_PFIFO_INTR);
    for (uint32_t p = 0u; p < NV_PBDMA_MAX; ++p) {
        CHECK(reg(NV_PBDMA_CTRL13C(p)) == (0x00000100u | p));
        CHECK(reg(NV_PBDMA_INTR(p)) == (0x000000a0u | p));
    }
    CHECK(reg(NV_PMC_INTR_EN_0) == SAVED_PMC_INTR_EN);
    CHECK(reg(NV_PMC_DEV_ENABLE) == SAVED_PMC_DEV);
    CHECK(reg(NV_BUS_BAR1_BIND) == SAVED_BAR1_BIND);
    CHECK(reg(NV_PRAMIN_WINDOW) == SAVED_PRAMIN);
}

/* All operations are synchronous, so the last submitted GPFIFO entry identifies
 * the pushbuffer segment emitted by the operation under test. */
static uint32_t last_submitted_start(const struct nv_gpu *gpu)
{
    uint32_t last = (gpu->gp_put + gpu->gpfifo_entries - 1u)
        & (gpu->gpfifo_entries - 1u);
    return gpu->seg_offset[last];
}

static void verify_layout(const struct nv_gpu *gpu, uint64_t base)
{
    uint64_t spt0 = base + NV_B1_SPT0;
    uint64_t spt1 = base + NV_B1_SPT1;
    uint64_t cspt = base + NV_CH_SPT;
    uint32_t pb_pages = (gpu->pushbuf_words * 4u + 0xfffu) >> 12u;
    uint32_t gp_pages = (gpu->gpfifo_entries * 8u + 0xfffu) >> 12u;
    uint32_t fence_pte = (NV_CH_VA_PUSHBUF >> 12u) + pb_pages + gp_pages;
    uint64_t fence_phys = (uint64_t)(uintptr_t)gpu->fence;

    CHECK(NV_B1_SPT0 + NV_B1_SPT_BYTES == NV_B1_SPT1);
    CHECK(NV_B1_SPT1 + NV_B1_SPT_BYTES == NV_CH_SPT);
    CHECK(NV_CH_SPT + NV_CH_SPT_BYTES <= NV_CH_USERD);
    CHECK(NV_CH_SCRATCH + NV_CH_SCRATCH_BYTES <= NV_CH_VRAM_SPAN);

    /* Both BAR1 SPTs are fully identity-mapped; the old overlapping layout
     * lost these entries when the channel SPT was zeroed. */
    CHECK(vram64(spt0) == 1u);
    CHECK(vram64(spt0 + 5u * 8u) == ((0x5000u >> 8u) | 1u));
    CHECK(vram64(spt0 + 32767u * 8u) == ((0x7fff000u >> 8u) | 1u));
    CHECK(vram64(spt1) == ((0x8000000ull >> 8u) | 1u));
    CHECK(vram64(spt1 + 8u)
        == (((0x8000000ull + 0x1000u) >> 8u) | 1u));
    CHECK(vram64(spt1 + 32767u * 8u) == ((0xffff000ull >> 8u) | 1u));

    /* The channel SPT starts zeroed and holds only this channel's mappings. */
    CHECK(vram64(cspt + 16u * 8u)
        == (((uint64_t)(uintptr_t)gpu->pushbuf >> 8u) | 1u
            | (2ull << 33u) | (1ull << 32u)));
    CHECK(vram64(cspt + (uint64_t)fence_pte * 8u)
        == ((fence_phys >> 8u) | 1u | (2ull << 33u) | (1ull << 32u)));
    CHECK(vram64(cspt + ((NV_CH_VA_SCRATCH >> 12u) * 8u))
        == (((base + NV_CH_SCRATCH) >> 8u) | 1u));
    CHECK(vram64(cspt + ((NV_CH_VA_FB >> 12u) * 8u))
        == (((gpu->scanout.vram_offset & ~0xfffull) >> 8u) | 1u));
}

static void verify_fill_stream(const struct nv_gpu *gpu, uint32_t start,
    uint32_t seq, uint32_t color, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h)
{
    const uint32_t *pb = gpu->pushbuf;
    uint64_t dst = gpu->fb_gpu_addr + (uint64_t)y * gpu->scanout.pitch
        + (uint64_t)x * 4u;
    uint32_t launch;

    CHECK(gpu->put - start == 19u);
    CHECK(pb[start] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_SET_SEMAPHORE_A, 3u));
    CHECK(pb[start + 1u] == (uint32_t)((gpu->fence_gpu_addr >> 32u) & 0xffu));
    CHECK(pb[start + 2u] == (uint32_t)gpu->fence_gpu_addr);
    CHECK(pb[start + 3u] == seq + 1u);
    CHECK(pb[start + 4u] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_SET_REMAP_CONST_A, 3u));
    CHECK(pb[start + 5u] == color);
    CHECK(pb[start + 6u] == 0u);
    CHECK(pb[start + 7u] == NVB0B5_REMAP_FILL32);
    CHECK(pb[start + 8u] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_OFFSET_IN_UPPER, 8u));
    CHECK(pb[start + 9u] == (uint32_t)((dst >> 32u) & 0xffu));
    CHECK(pb[start + 10u] == (uint32_t)dst);
    CHECK(pb[start + 13u] == gpu->scanout.pitch);
    CHECK(pb[start + 14u] == gpu->scanout.pitch);
    CHECK(pb[start + 15u] == w);
    CHECK(pb[start + 16u] == h);
    CHECK(pb[start + 17u] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_LAUNCH_DMA, 1u));
    launch = pb[start + 18u];
    CHECK((launch & (NVB0B5_LAUNCH_REMAP | NVB0B5_LAUNCH_SEM_ONE_WORD
        | NVB0B5_LAUNCH_SRC_PITCH | NVB0B5_LAUNCH_DST_PITCH
        | NVB0B5_LAUNCH_MULTI_LINE | NVB0B5_LAUNCH_FLUSH
        | NVB0B5_LAUNCH_NON_PIPELINED))
        == (NVB0B5_LAUNCH_REMAP | NVB0B5_LAUNCH_SEM_ONE_WORD
            | NVB0B5_LAUNCH_SRC_PITCH | NVB0B5_LAUNCH_DST_PITCH
            | NVB0B5_LAUNCH_MULTI_LINE | NVB0B5_LAUNCH_FLUSH
            | NVB0B5_LAUNCH_NON_PIPELINED));
}

static void verify_copy_stream(const struct nv_gpu *gpu, uint32_t start,
    uint32_t seq, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
    uint32_t w, uint32_t h)
{
    const uint32_t *pb = gpu->pushbuf;
    uint64_t src = gpu->fb_gpu_addr + (uint64_t)sy * gpu->scanout.pitch
        + (uint64_t)sx * 4u;
    uint64_t dst = gpu->fb_gpu_addr + (uint64_t)dy * gpu->scanout.pitch
        + (uint64_t)dx * 4u;
    uint32_t launch;

    CHECK(gpu->put - start == 15u);
    CHECK(pb[start] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_SET_SEMAPHORE_A, 3u));
    CHECK(pb[start + 3u] == seq + 1u);
    CHECK(pb[start + 4u] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_OFFSET_IN_UPPER, 8u));
    CHECK(pb[start + 5u] == (uint32_t)((src >> 32u) & 0xffu));
    CHECK(pb[start + 6u] == (uint32_t)src);
    CHECK(pb[start + 7u] == (uint32_t)((dst >> 32u) & 0xffu));
    CHECK(pb[start + 8u] == (uint32_t)dst);
    CHECK(pb[start + 9u] == gpu->scanout.pitch);
    CHECK(pb[start + 10u] == gpu->scanout.pitch);
    CHECK(pb[start + 11u] == w * 4u);
    CHECK(pb[start + 12u] == h);
    CHECK(pb[start + 13u] == nv_pkt_header(NV_PKT_INC, NV_SUBCHANNEL_CE,
        NVB0B5_LAUNCH_DMA, 1u));
    launch = pb[start + 14u];
    CHECK((launch & NVB0B5_LAUNCH_REMAP) == 0u);
    CHECK((launch & (NVB0B5_LAUNCH_SEM_ONE_WORD | NVB0B5_LAUNCH_FLUSH
        | NVB0B5_LAUNCH_NON_PIPELINED | NVB0B5_LAUNCH_MULTI_LINE
        | NVB0B5_LAUNCH_SRC_PITCH | NVB0B5_LAUNCH_DST_PITCH))
        == (NVB0B5_LAUNCH_SEM_ONE_WORD | NVB0B5_LAUNCH_FLUSH
            | NVB0B5_LAUNCH_NON_PIPELINED | NVB0B5_LAUNCH_MULTI_LINE
            | NVB0B5_LAUNCH_SRC_PITCH | NVB0B5_LAUNCH_DST_PITCH));
}

/* ---- BAR0 / BAR1 / USERD release tests -------------------------------- */

static void unmap_tests(void)
{
    struct nv_platform platform = {
        .pci_read32 = t_read,
        .pci_write32 = t_write,
        .map_mmio = c_map,
        .unmap_mmio = c_unmap,
    };
    struct nv_gpu gpu;
    uint32_t base;

    reset_unmap_log();

    setup_cfg(0x13c0u);
    CHECK(nv_probe(&platform, &gpu));
    CHECK(nv_map_bars(&platform, &gpu));
    CHECK(gpu.bar0 == (volatile uint8_t *)fake_bar0);
    CHECK(gpu.bar1 == (volatile uint8_t *)fake_vram);

    /* Re-mapping releases the old BAR1 then BAR0 before mapping again. */
    base = unmap_calls;
    CHECK(nv_map_bars(&platform, &gpu));
    CHECK(unmap_calls == base + 2u);
    CHECK(unmap_log[base].ptr == (volatile uint8_t *)fake_vram);
    CHECK(unmap_log[base].size == NV_GM204_BAR1_SIZE);
    CHECK(unmap_log[base + 1u].ptr == (volatile uint8_t *)fake_bar0);
    CHECK(unmap_log[base + 1u].size == NV_GM204_BAR0_SIZE);
    CHECK(gpu.bar0 == (volatile uint8_t *)fake_bar0);
    CHECK(gpu.bar1 == (volatile uint8_t *)fake_vram);

    /* A rejected chipset releases both fresh mappings before returning. */
    fake_bar0[3] = 0x11u;
    base = unmap_calls;
    CHECK(!nv_map_bars(&platform, &gpu));
    CHECK(!gpu.mapped && gpu.bar0 == NULL && gpu.bar1 == NULL);
    CHECK(unmap_calls == base + 4u);
    CHECK(unmap_log[base + 2u].ptr == (volatile uint8_t *)fake_vram);
    CHECK(unmap_log[base + 2u].size == NV_GM204_BAR1_SIZE);
    CHECK(unmap_log[base + 3u].ptr == (volatile uint8_t *)fake_bar0);
    CHECK(unmap_log[base + 3u].size == NV_GM204_BAR0_SIZE);
    fake_bar0[3] = 0x12u;
}

/* ---- channel / copy-engine tests -------------------------------------- */

static void channel_tests(void)
{
    static uint32_t pb[2048] __attribute__((aligned(4096)));
    static uint64_t gp[512] __attribute__((aligned(4096)));
    static uint32_t seg[512];
    static uint32_t host_userd[64];
    struct nv_gpu gpu;
    struct nv_platform platform = {
        .ctx = &gpu,
        .pci_read32 = t_read,
        .pci_write32 = t_write,
        .map_mmio = c_map,
        .unmap_mmio = c_unmap,
        .vram_read32 = c_vrd,
        .vram_write32 = c_vwr,
        .relax = c_relax,
        .log = c_log,
    };
    const uint64_t base = 0x400000ull;
    const uint32_t pitch = 2560u;
    uint32_t seq;
    uint32_t mark;
    uint32_t fills;
    uint32_t copies;
    uint32_t unmaps;
    uint32_t color;

    reset_unmap_log();

    /* Rejected placements never touch hardware state. */
    setup_cfg(0x13c0u);
    CHECK(nv_probe(&platform, &gpu) && nv_map_bars(&platform, &gpu));
    CHECK(nv_pushbuf_init(&gpu, pb, 2048u, gp, seg, 512u, host_userd));
    CHECK(gpu.fence != NULL && gpu.pushbuf_words == 1024u);
    CHECK(nv_adopt_scanout(&gpu, 0xe0100000ull, 640u, 480u, pitch, 32u));
    fake_reset(&gpu, base);
    fake_gpu_runs = false;
    CHECK(!nv_channel_bind(&gpu, base + 0x1000u));
    CHECK(!nv_channel_bind(&gpu, 0x200000ull));
    CHECK(!nv_channel_bind(&gpu, 0x10000000ull));
    CHECK(!gpu.channel_bound && gpu.bind_fail != NULL);
    CHECK(reg(NV_PFIFO_RUNLIST_COUNT) == SAVED_RUNLIST_COUNT);

    /* GPU never consumes the ring: bind must fail and restore all state. */
    setup_cfg(0x13c0u);
    CHECK(nv_probe(&platform, &gpu) && nv_map_bars(&platform, &gpu));
    CHECK(nv_pushbuf_init(&gpu, pb, 2048u, gp, seg, 512u, host_userd));
    CHECK(nv_adopt_scanout(&gpu, 0xe0100000ull, 640u, 480u, pitch, 32u));
    memset(fake_vram, 0, sizeof(fake_vram));
    fake_reset(&gpu, base);
    fake_gpu_runs = false;
    log_lines = 0u;
    CHECK(!nv_channel_bind(&gpu, base));
    CHECK(!gpu.channel_bound && !gpu.ring_acked && !gpu.ce_verified);
    CHECK(gpu.bind_fail != NULL && log_lines > 10u);
    CHECK(gpu.userd == (volatile uint32_t *)host_userd && gpu.gp_put == 0u
        && gpu.put == 0u);
    CHECK(!gpu.bind_started && !gpu.bar1_replaced);
    CHECK(!gpu.pfifo_state_saved);
    verify_restored_state();

    /* Successful bind: CE class, self-test, BAR1/channel layout. */
    setup_cfg(0x13c0u);
    CHECK(nv_probe(&platform, &gpu) && nv_map_bars(&platform, &gpu));
    CHECK(nv_pushbuf_init(&gpu, pb, 2048u, gp, seg, 512u, host_userd));
    CHECK(nv_adopt_scanout(&gpu, 0xe0100000ull, 640u, 480u, pitch, 32u));
    memset(fake_vram, 0, sizeof(fake_vram));
    memcpy(fake_userd, host_userd, sizeof(host_userd));
    /* Beyond the first page the self-test never touches; teardown must
     * clear the whole allocated scratch surface. */
    put32(fake_vram, base + NV_CH_SCRATCH + 0x1000u, 0x5eed5eedu);
    fake_reset(&gpu, base);
    fake_gpu_runs = true;
    unmaps = unmap_calls;
    CHECK(nv_channel_bind(&gpu, base));
    CHECK(gpu.bind_fail == NULL && gpu.channel_bound && gpu.ring_acked);
    CHECK(gpu.ce_verified && nv_accel_ready(&gpu));
    /* NOP probe, CE SET_OBJECT, 2 fills, 1 copy, 4-band down, 4-band up. */
    CHECK(gpu.userd == (volatile uint32_t *)fake_userd && gpu.gp_put == 7u);
    CHECK(unmap_calls == unmaps);
    CHECK(fake_seen_class == NVB0B5_CLASS);
    CHECK(fake_bad_va == 0u && fake_bad_sem == 0u);
    CHECK(fake_fills >= 2u && fake_copies >= 3u);
    CHECK(gpu.stat_fence_timeouts == 0u && !gpu.accel_failed);
    verify_layout(&gpu, base);
    CHECK(reg(NV_PFIFO_CHAN_INST(NV_CH_ID)) != SAVED_CHAN_INST);
    CHECK(reg(NV_PFIFO_RUNLIST_BASE) != SAVED_RUNLIST_BASE);
    CHECK(reg(NV_PFIFO_INTR) == UINT32_MAX);
    CHECK(reg(NV_PMC_DEV_ENABLE) == UINT32_MAX);
    CHECK(reg(NV_PMC_INTR_EN_0) == 0u);

    /* Small rectangles stay on the CPU unless the threshold is lowered. */
    CHECK(!nv_2d_fill_rect(&gpu, 0u, 0u, 8u, 8u, 0xffffffu));
    CHECK(gpu.stat_small_skips == 1u);
    CHECK(!nv_2d_fill_rect(&gpu, 640u, 0u, 5u, 5u, 0u));
    CHECK(!nv_2d_fill_rect(&gpu, 0u, 0u, 0u, 5u, 0u));

    /* CE fill stream and its result in VRAM. */
    seq = gpu.fence_seq;
    fills = fake_fills;
    color = 0x00c0ffeeu;
    CHECK(nv_2d_fill_rect(&gpu, 100u, 100u, 100u, 100u, color));
    CHECK(fake_fills == fills + 1u);
    CHECK(gpu.fence_seq == seq + 1u);
    CHECK(vram32(gpu.scanout.vram_offset + 100u * pitch + 100u * 4u)
        == color);
    CHECK(vram32(gpu.scanout.vram_offset + 199u * pitch + 199u * 4u)
        == color);
    mark = last_submitted_start(&gpu);
    verify_fill_stream(&gpu, mark, seq, color, 100u, 100u, 100u, 100u);

    /* CE copy stream and its result (same-surface, disjoint). */
    copies = fake_copies;
    color = 0x11223344u;
    CHECK(nv_2d_fill_rect(&gpu, 200u, 100u, 64u, 64u, color));
    CHECK(vram32(gpu.scanout.vram_offset + 100u * pitch + 200u * 4u)
        == color);
    seq = gpu.fence_seq;
    CHECK(nv_2d_copy_rect(&gpu, 200u, 100u, 300u, 200u, 64u, 64u));
    CHECK(fake_copies == copies + 1u);
    CHECK(vram32(gpu.scanout.vram_offset + 200u * pitch + 300u * 4u)
        == color);
    mark = last_submitted_start(&gpu);
    verify_copy_stream(&gpu, mark, seq, 200u, 100u, 300u, 200u, 64u, 64u);
    CHECK(gpu.stat_gpu_fills >= 2u && gpu.stat_gpu_copies == 1u);
    CHECK(nv_accel_sync(&gpu));
    CHECK(nv_pb_idle(&gpu));

    /* Teardown restores firmware state, unmaps USERD and clears all scratch. */
    unmaps = unmap_calls;
    nv_channel_unbind(&gpu);
    CHECK(!nv_accel_ready(&gpu)
        && reg(NV_PFIFO_CHAN_INST(NV_CH_ID)) == SAVED_CHAN_INST);
    CHECK(unmap_calls == unmaps + 1u);
    CHECK(unmap_log[unmaps].ptr == (volatile uint8_t *)fake_userd);
    CHECK(unmap_log[unmaps].size == 0x1000u);
    CHECK(!gpu.userd_mapped_by_us);
    verify_restored_state();
    CHECK(vram32(base + NV_CH_SCRATCH + 0x1000u) == 0u);

    /* Regression: clobbering a BAR1 identity PTE while the channel SPT is
     * built must fail the bind after the re-verify step. */
    setup_cfg(0x13c0u);
    CHECK(nv_probe(&platform, &gpu) && nv_map_bars(&platform, &gpu));
    CHECK(nv_pushbuf_init(&gpu, pb, 2048u, gp, seg, 512u, host_userd));
    CHECK(nv_adopt_scanout(&gpu, 0xe0100000ull, 640u, 480u, pitch, 32u));
    fake_reset(&gpu, base);
    fake_gpu_runs = true;
    fake_clobber_bar1_spt = true;
    CHECK(!nv_channel_bind(&gpu, base));
    CHECK(gpu.bind_fail != NULL);
    CHECK(strcmp(gpu.bind_fail,
        "BAR1 scan-out PTEs lost after channel VM build") == 0);
    CHECK(!gpu.channel_bound && !gpu.pfifo_state_saved);
    verify_restored_state();
}

/* ---- probe / mapping tests -------------------------------------------- */

static void probe_tests(void)
{
    struct nv_platform platform = {
        .pci_read32 = t_read,
        .pci_write32 = t_write,
        .map_mmio = t_map,
        .unmap_mmio = c_unmap,
    };
    struct nv_gpu gpu;
    static uint32_t pb[64] __attribute__((aligned(4096)));
    static uint64_t gp[4];
    static uint32_t seg[4];
    static uint32_t userd[64] __attribute__((aligned(4096)));
    uint32_t dummy_fence = 0u;

    CHECK(nv_pkt_header(NV_PKT_INC, 3u, 0x200u, 1u) == 0x20016080u);
    CHECK(nv_pkt_header(NV_PKT_NONINC, 0u, 0x40u, 5u) == 0x60050010u);
    CHECK(nv_pkt_header(NV_PKT_INC_ONCE, 7u, 0xffcu, 2u) == 0xa002e3ffu);
    CHECK(nv_pkt_imm(1u, 0x100u, 0x1234u) == 0x92342040u);
    CHECK(nv_gpfifo_entry(0x123456789cull, 7u)
        == (((uint64_t)((0x12u) | (7u << 10u))) << 32u | 0x3456789cu));

    setup_cfg(0x13c2u);
    pci_write_calls = 0u;
    CHECK(nv_probe(&platform, &gpu));
    CHECK(pci_write_calls == 0u);
    CHECK(gpu.device_id == 0x13c2u && gpu.bus == 1u);
    CHECK(gpu.bar0_phys == 0xf6000000ull && gpu.bar0_size == 0x1000000ull);
    CHECK(gpu.bar1_phys == 0xe0000000ull && gpu.bar1_size == 0x10000000ull);
    CHECK(cfg[4] == 0xf6000000u && cfg[5] == 0xe000000cu && cfg[6] == 0u);
    CHECK((cfg[1] & 0xffffu) == 3u);
    CHECK(!nv_probe_at(&platform, &gpu, 0u, 0u, 0u));
    CHECK(nv_probe_at(&platform, &gpu, 1u, 0u, 0u));

    /* Non-endpoint PCI headers are never treated as a graphics device. */
    setup_cfg(0x13c2u);
    cfg[3] = 0x00010000u; /* header type 1: PCI-to-PCI bridge */
    CHECK(!nv_probe(&platform, &gpu));

    /* A failed function-0 candidate must not hide a valid multifunction
     * function 1. */
    setup_cfg(0x13c2u);
    cfg[3] = 0x00800000u;
    cfg[4] = 0u;
    memcpy(cfg_fn1, cfg, sizeof(cfg_fn1));
    cfg_fn1[3] = 0u;
    cfg_fn1[4] = 0xf6000000u;
    cfg_fn1[5] = 0xe000000cu;
    cfg_fn1[6] = 0u;
    fn1_present = true;
    CHECK(nv_probe(&platform, &gpu));
    CHECK(gpu.function == 1u && gpu.bar0_phys == 0xf6000000ull);
    fn1_present = false;

    setup_cfg(0x13c1u);
    CHECK(!nv_probe(&platform, &gpu));
    setup_cfg(0x13c0u);
    CHECK(nv_probe(&platform, &gpu) && gpu.device_id == 0x13c0u);

    /* Command writes must preserve the PCI Status half of the dword. */
    unsigned maps_before = map_calls;
    command_stuck = true;
    CHECK(!nv_map_bars(&platform, &gpu));
    CHECK(map_calls == maps_before && !gpu.mapped && gpu.bar0 == NULL);
    command_stuck = false;
    uint32_t saved_command = cfg[1];
    cfg[1] = UINT32_MAX;
    CHECK(!nv_map_bars(&platform, &gpu));
    CHECK(map_calls == maps_before && !gpu.mapped);
    cfg[1] = saved_command;
    CHECK(nv_map_bars(&platform, &gpu));
    CHECK(map_calls == maps_before + 2u && gpu.chipset == NV_CHIPSET_GM204);
    CHECK((cfg[1] & 6u) == 6u);
    CHECK(cfg[1] == (saved_command | 0x6u));

    CHECK(nv_read32(&gpu, NV_PMC_BOOT_0) == 0x124020a1u);
    CHECK(nv_read32(&gpu, NV_PMC_ENABLE) == SAVED_PMC_ENABLE);
    CHECK(nv_read32(&gpu, UINT32_MAX) == 0xffffffffu);
    CHECK(nv_read32(&gpu, 0x2000000u) == 0xffffffffu);
    CHECK(nv_read32(&gpu, NV_GM204_BAR0_SIZE) == 0xffffffffu);
    CHECK(nv_read32(&gpu, 3u) == 0xffffffffu);
    CHECK(gpu.pfb_config == 0x12345678u);
    CHECK(gpu.pfb_cstatus == 0x80000000u && gpu.detected_vram_mb == 2048u);
    CHECK(gpu.pdisp_active_head == 0u && gpu.pdisp_htotal == 800u
        && gpu.pdisp_vtotal == 525u);
    nv_write32(&gpu, 0x10u, 0xdeadbeefu);
    CHECK(*(volatile uint32_t *)(bar0_mem + 0x10u) == 0xdeadbeefu);

    bar0_mem[3] = 0x11;
    CHECK(!nv_map_bars(&platform, &gpu));
    CHECK(!gpu.mapped && gpu.bar0 == NULL && gpu.bar1 == NULL);
    bar0_mem[3] = 0x12;
    CHECK(nv_map_bars(&platform, &gpu));

    CHECK(!nv_pushbuf_init(&gpu, pb, 64u, gp, seg, 3u, userd));
    CHECK(nv_pushbuf_init(&gpu, pb, 64u, gp, seg, 4u, userd));
    CHECK(gpu.fence == NULL); /* no completion page without 2048 words */
    CHECK(!nv_accel_ready(&gpu));
    CHECK(!nv_2d_fill_rect(&gpu, 0u, 0u, 10u, 10u, 0xffffffu));

    CHECK(!nv_adopt_scanout(&gpu, 0xd0000000ull, 640u, 480u, 2560u, 32u));
    CHECK(!nv_adopt_scanout(&gpu, 0xe0000000ull, 640u, 480u, 1280u, 16u));
    CHECK(nv_adopt_scanout(&gpu, 0xe0100000ull, 640u, 480u, 2560u, 32u));
    CHECK(gpu.scanout.vram_offset == 0x100000u);

    gpu.channel_bound = true;
    gpu.ring_acked = true;
    CHECK(!nv_accel_ready(&gpu)); /* CE not verified and no fence page */
    gpu.ce_verified = true;
    gpu.fence = &dummy_fence;
    CHECK(nv_accel_ready(&gpu));
    gpu.ring_acked = false;
    CHECK(!nv_accel_ready(&gpu));
    gpu.ring_acked = true;
    gpu.ce_verified = false;
    CHECK(!nv_accel_ready(&gpu));
    gpu.fence = NULL;
    gpu.channel_bound = false;
}

int main(void)
{
    probe_tests();
    unmap_tests();
    channel_tests();

    puts("PASS: nv_maxwell host test");
    return 0;
}
