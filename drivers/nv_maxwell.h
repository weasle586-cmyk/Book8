#ifndef BOOK8_NV_MAXWELL_H
#define BOOK8_NV_MAXWELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NVIDIA Maxwell (GM204) discovery, BAR mapping, PFIFO channel setup and
 * copy-engine (CE) 2D acceleration.
 *
 * Acceleration design
 * -------------------
 * The NV902D 2D class lives on the graphics engine, which needs signed
 * FECS/GPCCS firmware on GM20x.  That firmware is not available, so NV902D
 * can never execute here.  The Maxwell copy engine (MAXWELL_DMA_COPY_A,
 * class 0xB0B5) is fixed function, needs no firmware, and can do everything
 * the desktop needs from the GPU:
 *   - pitch-linear 2D rectangle copies (scrolling, window moves), and
 *   - constant fills through the remap unit (REMAP_CONST_A).
 * The driver discovers the copy engine's runlist through PTOP, binds one
 * channel to it, and only reports nv_accel_ready() after an on-hardware
 * self-test has proven that fill and copy really executed and that the
 * completion semaphore arrives.  Every operation is synchronous (it returns
 * after the GPU signalled completion) and performs the CPU cache maintenance
 * required because the boot framebuffer is mapped write-back.
 *
 * Display modesetting is still not implemented. */

#define NV_VENDOR_ID 0x10deu
#define NV_DEVICE_GTX980 0x13c0u
#define NV_DEVICE_GTX970 0x13c2u
#define NV_CHIPSET_GM204 0x124u

#define NV_GM204_BAR0_SIZE 0x01000000ull
#define NV_GM204_BAR1_SIZE 0x10000000ull
#define NV_GM204_BAR0_PHYS 0xfd000000ull
#define NV_GM204_BAR1_PHYS 0xd0000000ull
#define NV_PMC_BOOT_0 0x000000u
#define NV_PMC_BOOT_0_CHIPSET_SHIFT 20u
#define NV_PMC_BOOT_0_CHIPSET_MASK 0x1ffu
#define NV_PMC_ENABLE 0x000200u
#define NV_PMC_ENABLE_PFIFO 0x00000100u
#define NV_PMC_ENABLE_PGRAPH 0x00001000u
#define NV_PMC_ENABLE_PDISP 0x00020000u
#define NV_PMC_ENABLE_PCRTC 0x00040000u
#define NV_PMC_DEV_ENABLE 0x000204u   /* PBDMA unit enable mask (gk104+) */
#define NV_PMC_INTR_0 0x000100u
#define NV_PMC_INTR_EN_0 0x000140u
#define NV_PFB_CONFIG 0x100200u
#define NV_PFB_CSTATUS 0x10020cu
#define NV_PDISP_HEAD_MASK 0x612004u
#define NV_PDISP_HEAD_TIMING(h) (0x640414u + (h) * 0x300u)
#define NV_PRAMIN_WINDOW 0x001700u
#define NV_BUS_BAR1_BIND 0x001704u
#define NV_PRAMIN 0x700000u
#define NV_PRAMIN_SIZE 0x100000u
#define NV_PFIFO_INTR 0x002100u
#define NV_PFIFO_ENGN_STATUS(e) (0x002640u + (e) * 4u)
#define NV_PFIFO_SCHED_DISABLE 0x002630u  /* bit n set = runlist n blocked */
#define NV_PBDMA_MAX 8u
#define NV_PBDMA_INTR(p) (0x040108u + (p) * 0x2000u)
#define NV_PBDMA_CTRL13C(p) (0x04013cu + (p) * 0x2000u)
#define NV_PBDMA_CTRL13C_MASK 0x10000100u
/* Host (PBDMA) methods live below 0x100 and are valid on any subchannel.
 * NOP is 0x08 (NVB06F_NOP).  Method 0x100 and up is forwarded to the engine
 * bound to the subchannel, so it must not be used as a host NOP. */
#define NV_HOST_SET_OBJECT 0x0000u
#define NV_HOST_NOP 0x0008u
#define NV_PBDMA_NOP NV_HOST_NOP
#define NV_PFIFO_BAR1_USERD 0x002254u
#define NV_PFIFO_RUNLIST_BASE 0x002270u
#define NV_PFIFO_RUNLIST_COUNT 0x002274u
#define NV_PFIFO_RUNL_PENDING(r) (0x002284u + (r) * 8u)
#define NV_PFIFO_RUNL_PENDING_BIT 0x00100000u
#define NV_PFIFO_CHAN_INST(c) (0x800000u + (c) * 8u)
#define NV_PFIFO_CHAN_CTRL(c) (0x800004u + (c) * 8u)
#define NV_PFIFO_CHAN_START 0x00000400u
#define NV_MMU_FLUSH_STATUS 0x100c80u
#define NV_MMU_INVALIDATE_PDB 0x100cb8u
#define NV_MMU_INVALIDATE 0x100cbcu

/* PTOP device-info table (gk104+): 64 words, entries chain while bit 31 is
 * set.  Word types (bits 1:0): 1 DATA, 2 ENUM (engine / runlist ids),
 * 3 ENGINE_TYPE. */
#define NV_PTOP_INFO(i) (0x022700u + (i) * 4u)
#define NV_PTOP_ENTRIES 64u
#define NV_PTOP_TYPE_GR 0u
#define NV_PTOP_TYPE_CE 1u
#define NV_PTOP_TYPE_CE_PASCAL 0x13u
#define NV_NO_ID 0xffu

/* Command packet header: sec_op[31:29] count/data[28:16] subc[15:13]
 * method[11:0] (method address >> 2). */
#define NV_PKT_INC 1u
#define NV_PKT_NONINC 3u
#define NV_PKT_IMM 4u
#define NV_PKT_INC_ONCE 5u

#define NV_USERD_GP_GET 0x88u
#define NV_USERD_GP_PUT 0x8cu

/* MAXWELL_DMA_COPY_A methods, from NVIDIA open-gpu-doc clb0b5.h. */
#define NVB0B5_CLASS 0xb0b5u
#define NVB0B5_SET_SEMAPHORE_A 0x0240u        /* [7:0] address upper */
#define NVB0B5_SET_SEMAPHORE_B 0x0244u        /* address lower */
#define NVB0B5_SET_SEMAPHORE_PAYLOAD 0x0248u
#define NVB0B5_SET_SRC_PHYS_MODE 0x0260u
#define NVB0B5_SET_DST_PHYS_MODE 0x0264u
#define NVB0B5_LAUNCH_DMA 0x0300u
#define NVB0B5_OFFSET_IN_UPPER 0x0400u        /* 8 consecutive registers: */
#define NVB0B5_OFFSET_IN_LOWER 0x0404u        /*  in hi/lo, out hi/lo,    */
#define NVB0B5_OFFSET_OUT_UPPER 0x0408u       /*  pitch in/out,           */
#define NVB0B5_OFFSET_OUT_LOWER 0x040cu       /*  line length, line count */
#define NVB0B5_PITCH_IN 0x0410u
#define NVB0B5_PITCH_OUT 0x0414u
#define NVB0B5_LINE_LENGTH_IN 0x0418u         /* bytes; elements if REMAP */
#define NVB0B5_LINE_COUNT 0x041cu
#define NVB0B5_SET_REMAP_CONST_A 0x0700u      /* 3 consecutive: A, B, COMP */
#define NVB0B5_SET_REMAP_CONST_B 0x0704u
#define NVB0B5_SET_REMAP_COMPONENTS 0x0708u

#define NVB0B5_LAUNCH_NON_PIPELINED 0x00000002u
#define NVB0B5_LAUNCH_FLUSH 0x00000004u
#define NVB0B5_LAUNCH_SEM_ONE_WORD 0x00000008u
#define NVB0B5_LAUNCH_SRC_PITCH 0x00000080u
#define NVB0B5_LAUNCH_DST_PITCH 0x00000100u
#define NVB0B5_LAUNCH_MULTI_LINE 0x00000200u
#define NVB0B5_LAUNCH_REMAP 0x00000400u
/* DST_X = CONST_A (4), component size four bytes (3 << 16), one source and
 * one destination component: each element is one constant 32-bit pixel. */
#define NVB0B5_REMAP_FILL32 0x00030004u

/* NV902D (Fermi/Maxwell 2D).  Kept for reference only: the graphics engine
 * needs signed FECS/GPCCS firmware on GM20x, so the driver does not emit
 * these methods. */
#define NV902D_CLASS 0x902du
#define NV902D_SET_OBJECT 0x0000u
#define NV902D_SET_DST_FORMAT 0x0200u
#define NV902D_SET_DST_MEMORY_LAYOUT 0x0204u
#define NV902D_SET_DST_PITCH 0x0214u
#define NV902D_SET_DST_WIDTH 0x0218u
#define NV902D_SET_DST_HEIGHT 0x021cu
#define NV902D_SET_DST_OFFSET_UPPER 0x0220u
#define NV902D_SET_DST_OFFSET_LOWER 0x0224u
#define NV902D_SET_SRC_FORMAT 0x0230u
#define NV902D_SET_SRC_MEMORY_LAYOUT 0x0234u
#define NV902D_SET_SRC_PITCH 0x0244u
#define NV902D_SET_SRC_WIDTH 0x0248u
#define NV902D_SET_SRC_HEIGHT 0x024cu
#define NV902D_SET_SRC_OFFSET_UPPER 0x0250u
#define NV902D_SET_SRC_OFFSET_LOWER 0x0254u
#define NV902D_SET_OPERATION 0x02acu
#define NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT 0x0584u
#define NV902D_SET_RENDER_SOLID_PRIM_COLOR 0x0588u
#define NV902D_RENDER_SOLID_PRIM_MODE 0x0580u
#define NV902D_RENDER_SOLID_PRIM_POINT_X(j) (0x0600u + (j) * 8u)
#define NV902D_RENDER_SOLID_PRIM_POINT_Y(j) (0x0604u + (j) * 8u)
#define NV902D_FORMAT_X8R8G8B8 0xe6u
#define NV902D_LAYOUT_PITCH 1u
#define NV902D_OPERATION_SRCCOPY 3u
#define NV902D_PRIM_MODE_RECTS 4u
#define NV_SUBCHANNEL_2D 3u
#define NV_SUBCHANNEL_CE 4u

struct nv_platform {
    void *ctx;
    uint32_t (*pci_read32)(void *ctx, uint8_t bus, uint8_t slot,
        uint8_t function, uint8_t offset);
    void (*pci_write32)(void *ctx, uint8_t bus, uint8_t slot,
        uint8_t function, uint8_t offset, uint32_t value);
    /* Returns a kernel mapping of [phys, phys + size), or NULL.  Register
     * windows are mapped uncached; the VRAM aperture keeps the existing
     * mapping so the boot framebuffer's CPU drawing speed is unchanged. */
    volatile uint8_t *(*map_mmio)(void *ctx, uint64_t phys, uint64_t size,
        bool uncached);
    /* Optional: release a mapping returned by map_mmio.  The kernel restores
     * the original 2 MiB page-table cache bits for uncached mappings. */
    void (*unmap_mmio)(void *ctx, volatile uint8_t *ptr, uint64_t size);
    /* Optional: VRAM access.  NULL selects the BAR0 PRAMIN window. */
    uint32_t (*vram_read32)(void *ctx, uint64_t vram_addr);
    void (*vram_write32)(void *ctx, uint64_t vram_addr, uint32_t value);
    /* Optional: called inside every hardware poll loop (pause / short delay). */
    void (*relax)(void *ctx);
    /* Optional: diagnostic sink, one "label = value" line per call. */
    void (*log)(void *ctx, const char *label, uint32_t value);
    /* Optional: write back and invalidate the CPU cache lines covering
     * [addr, addr + len).  NULL selects CLFLUSH on x86 (64-byte lines). */
    void (*cache_flush)(void *ctx, const void *addr, size_t len);
};

/* Channel memory map.  PFIFO on Kepler/Maxwell needs the instance block, page
 * tables, runlist and USERD in VRAM; all offsets are from vram_base, which
 * must be 2 MiB aligned and lie in unused VRAM (past the scan-out surface). */
#define NV_CH_INST 0x000000u
#define NV_CH_RUNLIST 0x001000u
#define NV_CH_PD 0x002000u
#define NV_B1_INST 0x004000u
#define NV_B1_PD 0x005000u
/* BAR1 identity maps use two 128 MiB PDEs, so each BAR1 SPT needs 32768
 * PTEs (0x40000 bytes).  The channel SPT must not overlap either table. */
#define NV_B1_SPT0 0x040000u
#define NV_B1_SPT1 0x080000u
#define NV_B1_SPT_BYTES 0x040000u
#define NV_CH_SPT 0x0c0000u          /* channel page table, full 128 MiB PDE */
#define NV_CH_SPT_BYTES 0x040000u
#define NV_CH_USERD 0x200000u
#define NV_CH_SCRATCH 0x300000u      /* CE self-test surface */
#define NV_CH_SCRATCH_BYTES 0x010000u
#define NV_CH_VRAM_SPAN 0x400000u
/* Channel GPU virtual address map (all inside the first 128 MiB PDE). */
#define NV_CH_VA_PUSHBUF 0x010000u   /* pushbuffer, GPFIFO, fence page */
#define NV_CH_VA_SCRATCH 0x200000u
#define NV_CH_VA_FB 0x400000u        /* scan-out window, up to NV_CH_FB_MAX */
#define NV_CH_FB_MAX 0x02000000u
#define NV_CH_VA_LIMIT (1ull << 27)
#define NV_CH_ID 0u
#define NV_BAR1_MAX_MAP 0x10000000ull

/* Rectangles smaller than this (in pixels) are left to the CPU: a
 * synchronous GPU round trip plus cache maintenance costs more than drawing
 * them.  A zero nv_gpu.min_pixels selects this default; set 1 to force GPU. */
#define NV_ACCEL_DEFAULT_MIN_PIXELS 4096u
#define NV_ACCEL_MAX_BANDS 64u
#define NV_CACHE_LINE 64u

/* RAMFC image at the start of the channel instance block (gk104 layout). */
struct nv_ramfc {
    uint32_t userd_lo, userd_hi;          /* 0x08, 0x0c */
    uint32_t signature;                   /* 0x10 = 0xface */
    uint32_t gpfifo_lo, gpfifo_hi_limit;  /* 0x48, 0x4c */
    uint32_t chid;                        /* 0xe8 */
};

struct nv_scanout {
    bool valid;
    uint64_t physical_base;
    uint64_t vram_offset;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

struct nv_gpu {
    bool present;
    bool mapped;
    uint16_t device_id;
    uint8_t bus, slot, function;
    uint64_t bar0_phys, bar0_size;
    uint64_t bar1_phys, bar1_size;
    volatile uint8_t *bar0;
    volatile uint8_t *bar1;
    uint32_t boot0;
    uint32_t chipset;
    uint8_t chip_rev;
    uint8_t foundry_id;
    uint32_t pmc_enable;
    uint32_t pmc_intr;
    uint32_t pmc_intr_en_0;
    uint32_t pfb_config;
    uint32_t pfb_cstatus;
    uint32_t detected_vram_mb;
    uint32_t pdisp_head_mask;
    uint32_t pdisp_active_head;
    uint32_t pdisp_htotal;
    uint32_t pdisp_vtotal;

    uint32_t *pushbuf;
    uint32_t pushbuf_words;
    uint32_t put;
    uint32_t seg_start;
    uint64_t *gpfifo;
    uint32_t *seg_offset;
    uint32_t gpfifo_entries;
    uint32_t gp_put;
    uint32_t gp_get_shadow;
    volatile uint32_t *userd;
    uint64_t pushbuf_gpu_addr;

    struct nv_platform plat;
    uint64_t vram_base;
    uint32_t pramin_window;
    bool channel_bound;
    bool ring_acked;        /* PFIFO advanced GP_GET after the NOP packet */
    bool runlist_committed;

    /* Bring-up bookkeeping so a failed bind can restore prior hardware state. */
    bool bind_started;
    bool pfifo_enabled_by_us;
    bool bar1_replaced;
    uint32_t saved_bar1_bind;
    uint32_t saved_pramin_window;
    volatile uint32_t *saved_userd;
    const char *bind_fail;  /* stage that failed, NULL when bound */
    struct nv_scanout scanout;
    uint32_t pb_overflows;

    /* ---- copy-engine acceleration (appended; earlier layout unchanged) -- */
    volatile uint32_t *fence;   /* last page of the pushbuffer allocation */
    uint64_t fence_gpu_addr;
    uint32_t fence_seq;
    uint64_t fb_gpu_addr;       /* channel VA of scanout byte 0 */
    uint32_t chan_runl;         /* runlist the channel was bound to */
    bool ptop_valid;
    bool gr_found;
    bool ce_found;
    uint8_t gr_engine, gr_runlist;
    uint8_t ce_engine, ce_runlist;
    uint8_t ce_count;
    uint8_t ptop_devices;
    bool ce_verified;           /* self-test proved fill + copy + fence */
    bool accel_failed;          /* latched after a fence timeout */
    bool pmc_204_changed;
    uint32_t saved_pmc_204;
    uint32_t min_pixels;
    uint64_t stat_gpu_fills;
    uint64_t stat_gpu_copies;
    uint64_t stat_small_skips;
    uint64_t stat_unsupported;
    uint64_t stat_fence_timeouts;

    /* Complete PFIFO/PBDMA state captured before channel bring-up and written
     * back in reverse order by nv_channel_teardown().  Interrupts are masked
     * at PMC while the channel owns PFIFO, then the original enable mask is
     * restored last. */
    bool pfifo_state_saved;
    bool userd_mapped_by_us;
    uint32_t saved_pfifo_chan_ctrl;
    uint32_t saved_pfifo_chan_inst;
    uint32_t saved_pfifo_runlist_base;
    uint32_t saved_pfifo_runlist_count;
    uint32_t saved_pfifo_sched_disable;
    uint32_t saved_pfifo_bar1_userd;
    uint32_t saved_pfifo_intr;
    uint32_t saved_pbdma_ctrl13c[NV_PBDMA_MAX];
    uint32_t saved_pbdma_intr[NV_PBDMA_MAX];
    uint32_t saved_pmc_intr_en_0;
};

static inline uint32_t nv_pkt_header(uint32_t sec_op, uint32_t subchannel,
    uint32_t method, uint32_t count)
{
    return (sec_op << 29u) | ((count & 0x1fffu) << 16u)
        | ((subchannel & 7u) << 13u) | ((method >> 2u) & 0xfffu);
}

static inline uint32_t nv_pkt_imm(uint32_t subchannel, uint32_t method,
    uint32_t data)
{
    return nv_pkt_header(NV_PKT_IMM, subchannel, method, data & 0x1fffu);
}

/* GPFIFO entry: low = address[31:2], high = address[39:32] | length << 10. */
static inline uint64_t nv_gpfifo_entry(uint64_t gpu_addr, uint32_t words)
{
    uint32_t lo = (uint32_t)gpu_addr & ~3u;
    uint32_t hi = (uint32_t)((gpu_addr >> 32u) & 0xffu)
        | ((words & 0x1fffffu) << 10u);
    return ((uint64_t)hi << 32u) | lo;
}

/* Header-only so kernel diagnostics can evaluate ready state without linking
 * the runtime driver module.  Keep in sync with nv_accel_ready(). */
static inline bool nv_accel_active(const struct nv_gpu *gpu)
{
    return gpu->present && gpu->mapped && gpu->chipset == NV_CHIPSET_GM204
        && gpu->channel_bound && gpu->ring_acked && gpu->ce_verified
        && !gpu->accel_failed && gpu->scanout.valid && gpu->pushbuf != NULL
        && gpu->bar1 != NULL && gpu->fence != NULL;
}

bool nv_probe(const struct nv_platform *platform, struct nv_gpu *gpu);
bool nv_probe_at(const struct nv_platform *platform, struct nv_gpu *gpu,
    uint8_t bus, uint8_t slot, uint8_t function);
bool nv_map_bars(const struct nv_platform *platform, struct nv_gpu *gpu);
uint32_t nv_read32(const struct nv_gpu *gpu, uint32_t offset);
void nv_write32(const struct nv_gpu *gpu, uint32_t offset, uint32_t value);
void nv_refresh_telemetry(struct nv_gpu *gpu);

/* Reads the PTOP device table and records the graphics and copy engines and
 * their runlists.  Read-only; called by nv_map_bars. */
void nv_discover_engines(struct nv_gpu *gpu);

/* pushbuf must be 4 KiB aligned.  When words >= 2048 its last 4 KiB page is
 * reserved as the GPU-visible completion fence, so the usable pushbuffer is
 * (words & ~1023) - 1024 words.  The channel cannot bind without a fence. */
bool nv_pushbuf_init(struct nv_gpu *gpu, uint32_t *pushbuf, uint32_t words,
    uint64_t *gpfifo, uint32_t *seg_offset, uint32_t entries,
    volatile uint32_t *userd);
bool nv_pb_method(struct nv_gpu *gpu, uint32_t subchannel, uint32_t method,
    const uint32_t *data, uint32_t count);
bool nv_pb_submit(struct nv_gpu *gpu);
bool nv_pb_idle(struct nv_gpu *gpu);

/* Builds the BAR1 and channel address spaces, instance block, runlist and
 * USERD in VRAM at vram_base, starts a channel on the copy engine's runlist,
 * proves the GPU consumes the GPFIFO ring and finally runs a copy-engine
 * fill/copy/fence self-test.  Returns true only when all of that passed. */
bool nv_channel_bind(struct nv_gpu *gpu, uint64_t vram_base);
void nv_channel_unbind(struct nv_gpu *gpu);

bool nv_adopt_scanout(struct nv_gpu *gpu, uint64_t fb_phys, uint32_t width,
    uint32_t height, uint32_t pitch, uint32_t bits_per_pixel);

/* True only after bind + ring ack + CE self-test, and until a fence times
 * out.  Safe to poll from the drawing path. */
bool nv_accel_ready(const struct nv_gpu *gpu);
const char *nv_accel_state(const struct nv_gpu *gpu);
/* Waits for the most recently submitted operation.  All operations below are
 * already synchronous, so this only matters to callers that submit manually. */
bool nv_accel_sync(struct nv_gpu *gpu);

/* Both return true only when the GPU performed the operation.  false means
 * "nothing was drawn, use the CPU" (small rectangle, overlap that cannot be
 * split, accel unavailable or a hardware failure) and is never an error the
 * caller must report. */
bool nv_2d_fill_rect(struct nv_gpu *gpu, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t xrgb);
bool nv_2d_copy_rect(struct nv_gpu *gpu, uint32_t src_x, uint32_t src_y,
    uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);

#endif
