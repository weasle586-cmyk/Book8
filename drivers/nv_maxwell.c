#include "nv_maxwell.h"

#define PCI_COMMAND 0x04u
#define PCI_BAR(i) ((uint8_t)(0x10u + (i) * 4u))
#define FOUR_GIB 0x100000000ull

static bool nv_device_supported(uint16_t id)
{
    return id == NV_DEVICE_GTX980 || id == NV_DEVICE_GTX970;
}

/* GM204 BAR apertures are fixed-size. Read the programmed base only; probing
 * BAR size by writing all-ones can disturb firmware-owned scanout. */
static bool nv_read_bar(const struct nv_platform *p, const struct nv_gpu *g,
    uint32_t index, uint64_t *phys, uint64_t *size, bool *is64)
{
    uint8_t reg = PCI_BAR(index);
    uint32_t orig = p->pci_read32(p->ctx, g->bus, g->slot, g->function, reg);
    if (orig == UINT32_MAX || (orig & 1u) != 0u) {
        return false;
    }
    bool wide = ((orig >> 1u) & 3u) == 2u;
    uint32_t orig_hi = 0u;
    if (wide) {
        if (index >= 5u) {
            return false;
        }
        orig_hi = p->pci_read32(p->ctx, g->bus, g->slot, g->function,
            (uint8_t)(reg + 4u));
        if (orig_hi == UINT32_MAX) {
            return false;
        }
    }
    *phys = ((uint64_t)orig_hi << 32u) | (orig & ~0xfu);
    *size = index == 0u ? NV_GM204_BAR0_SIZE : NV_GM204_BAR1_SIZE;
    *is64 = wide;
    return *phys != 0u;
}

bool nv_probe_at(const struct nv_platform *p, struct nv_gpu *gpu,
    uint8_t bus, uint8_t slot, uint8_t function)
{
    *gpu = (struct nv_gpu){0};
    gpu->plat = *p;
    uint32_t id = p->pci_read32(p->ctx, bus, slot, function, 0u);
    uint32_t header = p->pci_read32(p->ctx, bus, slot, function, 0x0cu);
    uint32_t cls = p->pci_read32(p->ctx, bus, slot, function, 8u);
    if ((uint16_t)id != NV_VENDOR_ID
        || !nv_device_supported((uint16_t)(id >> 16u))
        || ((header >> 16u) & 0x7fu) != 0u
        || (cls >> 24u) != 0x03u)
        return false;
    gpu->device_id = (uint16_t)(id >> 16u);
    gpu->bus = bus;
    gpu->slot = slot;
    gpu->function = function;
    bool wide0 = false, wide1 = false;
    if (!nv_read_bar(p, gpu, 0u, &gpu->bar0_phys, &gpu->bar0_size, &wide0)
        || wide0)
        return false;
    if (!nv_read_bar(p, gpu, 1u, &gpu->bar1_phys, &gpu->bar1_size, &wide1)) {
        gpu->bar1_phys = 0u;
        gpu->bar1_size = 0u;
    }
    gpu->present = true;
    return true;
}

bool nv_probe(const struct nv_platform *p, struct nv_gpu *gpu)
{
    *gpu = (struct nv_gpu){0};
    gpu->plat = *p;
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint32_t count = 1u;
            for (uint32_t fn = 0u; fn < count; ++fn) {
                uint32_t id = p->pci_read32(p->ctx, (uint8_t)bus,
                    (uint8_t)slot, (uint8_t)fn, 0u);
                uint16_t vendor = (uint16_t)id;
                if (vendor == 0xffffu || vendor == 0u) {
                    continue;
                }
                uint32_t header = p->pci_read32(p->ctx, (uint8_t)bus,
                    (uint8_t)slot, (uint8_t)fn, 0x0cu);
                if (fn == 0u && ((header >> 16u) & 0x80u) != 0u) {
                    count = 8u;
                }
                if (vendor != NV_VENDOR_ID
                    || !nv_device_supported((uint16_t)(id >> 16u))
                    || ((header >> 16u) & 0x7fu) != 0u) {
                    continue;
                }
                uint32_t cls = p->pci_read32(p->ctx, (uint8_t)bus,
                    (uint8_t)slot, (uint8_t)fn, 8u);
                if ((cls >> 24u) != 0x03u) {
                    continue;
                }
                /* A failed detailed probe is not fatal on a multifunction
                 * card; another function may still be the graphics engine. */
                if (nv_probe_at(p, gpu, (uint8_t)bus, (uint8_t)slot,
                        (uint8_t)fn)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static void nv_unmap_bars(const struct nv_platform *p, struct nv_gpu *gpu)
{
    if (gpu->bar1 != NULL) {
        if (p != NULL && p->unmap_mmio != NULL) {
            p->unmap_mmio(p->ctx, gpu->bar1, gpu->bar1_size);
        }
        gpu->bar1 = NULL;
    }
    if (gpu->bar0 != NULL) {
        if (p != NULL && p->unmap_mmio != NULL) {
            p->unmap_mmio(p->ctx, gpu->bar0, gpu->bar0_size);
        }
        gpu->bar0 = NULL;
    }
}

bool nv_map_bars(const struct nv_platform *p, struct nv_gpu *gpu)
{
    gpu->mapped = false;
    nv_unmap_bars(p, gpu);
    if (!gpu->present || gpu->bar0_size < 4u || gpu->bar0_phys >= FOUR_GIB
        || gpu->bar0_size > FOUR_GIB - gpu->bar0_phys) {
        return false;
    }
    uint32_t cmd = p->pci_read32(p->ctx, gpu->bus, gpu->slot, gpu->function,
        PCI_COMMAND);
    if (cmd == UINT32_MAX) {
        if (p->log != NULL)
            p->log(p->ctx, "PCI command unavailable; MMIO blocked", cmd);
        return false;
    }
    /* PCI_COMMAND is a 16-bit Command register followed by a 16-bit Status
     * register.  The platform API only writes 32 bits, so preserve the upper
     * half instead of zeroing firmware-owned Status bits. */
    p->pci_write32(p->ctx, gpu->bus, gpu->slot, gpu->function, PCI_COMMAND,
        (cmd & 0xffff0000u) | ((cmd | 0x6u) & 0xffffu));
    uint32_t readback = p->pci_read32(p->ctx, gpu->bus, gpu->slot,
        gpu->function, PCI_COMMAND);
    if (readback == UINT32_MAX || (readback & 6u) != 6u) {
        if (p->log != NULL)
            p->log(p->ctx, "PCI MSE/BME verification failed; MMIO blocked", readback);
        return false;
    }

    gpu->bar0 = p->map_mmio(p->ctx, gpu->bar0_phys, gpu->bar0_size, true);
    if (gpu->bar0 == NULL) {
        return false;
    }
    if (gpu->bar1_size != 0u && gpu->bar1_phys < FOUR_GIB
        && gpu->bar1_size <= FOUR_GIB - gpu->bar1_phys) {
        gpu->bar1 = p->map_mmio(p->ctx, gpu->bar1_phys, gpu->bar1_size, false);
    }
    gpu->boot0 = nv_read32(gpu, NV_PMC_BOOT_0);
    gpu->chipset = (gpu->boot0 >> NV_PMC_BOOT_0_CHIPSET_SHIFT)
        & NV_PMC_BOOT_0_CHIPSET_MASK;
    if (gpu->boot0 == 0xffffffffu || gpu->chipset != NV_CHIPSET_GM204) {
        nv_unmap_bars(p, gpu);
        gpu->mapped = false;
        return false;
    }
    gpu->mapped = true;
    nv_refresh_telemetry(gpu);
    nv_discover_engines(gpu);
    return true;
}

static uint32_t nv_rd32(const struct nv_gpu *gpu, uint32_t offset)
{
    if (gpu->bar0 == NULL || gpu->bar0_size < sizeof(uint32_t)
        || (offset & 3u) != 0u
        || offset >= NV_GM204_BAR0_SIZE
        || offset > gpu->bar0_size - sizeof(uint32_t)) {
        return 0xffffffffu;
    }
    return *(volatile const uint32_t *)(gpu->bar0 + offset);
}

static void nv_wr32(const struct nv_gpu *gpu, uint32_t offset, uint32_t value)
{
    if (gpu->bar0 != NULL && gpu->bar0_size >= sizeof(uint32_t)
        && (offset & 3u) == 0u
        && offset < NV_GM204_BAR0_SIZE
        && offset <= gpu->bar0_size - sizeof(uint32_t)) {
        *(volatile uint32_t *)(gpu->bar0 + offset) = value;
    }
}

uint32_t nv_read32(const struct nv_gpu *gpu, uint32_t offset)
{
    return nv_rd32(gpu, offset);
}

void nv_write32(const struct nv_gpu *gpu, uint32_t offset, uint32_t value)
{
    nv_wr32(gpu, offset, value);
}

void nv_refresh_telemetry(struct nv_gpu *gpu)
{
    if (gpu == NULL || !gpu->mapped) {
        return;
    }

    gpu->boot0 = nv_rd32(gpu, NV_PMC_BOOT_0);
    gpu->chipset = (gpu->boot0 >> NV_PMC_BOOT_0_CHIPSET_SHIFT)
        & NV_PMC_BOOT_0_CHIPSET_MASK;
    gpu->chip_rev = (uint8_t)((gpu->boot0 >> 8u) & 0x0fu);
    gpu->foundry_id = (uint8_t)gpu->boot0;
    gpu->pmc_enable = nv_rd32(gpu, NV_PMC_ENABLE);
    gpu->pmc_intr = nv_rd32(gpu, NV_PMC_INTR_0);
    gpu->pmc_intr_en_0 = nv_rd32(gpu, NV_PMC_INTR_EN_0);
    gpu->pfb_config = nv_rd32(gpu, NV_PFB_CONFIG);
    gpu->pfb_cstatus = nv_rd32(gpu, NV_PFB_CSTATUS);
    gpu->detected_vram_mb = gpu->pfb_cstatus == UINT32_MAX ? 0u
        : (gpu->pfb_cstatus & 0xff000000u) >> 20u;
    uint32_t head_mask = nv_rd32(gpu, NV_PDISP_HEAD_MASK);
    gpu->pdisp_head_mask = head_mask == UINT32_MAX ? 0u
        : head_mask & 0x0fu;
    gpu->pdisp_active_head = UINT32_MAX;
    gpu->pdisp_htotal = 0u;
    gpu->pdisp_vtotal = 0u;
    for (uint32_t head = 0u; head < 4u; ++head) {
        if ((gpu->pdisp_head_mask & (1u << head)) == 0u) {
            continue;
        }
        uint32_t timing = nv_rd32(gpu, NV_PDISP_HEAD_TIMING(head));
        if (timing == UINT32_MAX) {
            continue;
        }
        uint32_t htotal = timing & 0xffffu;
        uint32_t vtotal = timing >> 16u;
        if (htotal != 0u && vtotal != 0u) {
            gpu->pdisp_active_head = head;
            gpu->pdisp_htotal = htotal;
            gpu->pdisp_vtotal = vtotal;
            break;
        }
    }
}

static void nv_diag(const struct nv_gpu *gpu, const char *label,
    uint32_t value)
{
    if (gpu->plat.log != NULL) {
        gpu->plat.log(gpu->plat.ctx, label, value);
    }
}

/* ---- Engine topology (PTOP) ------------------------------------------- */

/* The device-info table lists, per device, a DATA word, an ENUM word with the
 * engine and runlist ids and an ENGINE_TYPE word; bit 31 chains words that
 * belong to the same device.  Layout follows nouveau gk104_top_oneinit. */
void nv_discover_engines(struct nv_gpu *gpu)
{
    if (gpu == NULL || !gpu->mapped) {
        return;
    }
    gpu->ptop_valid = false;
    gpu->gr_found = false;
    gpu->ce_found = false;
    gpu->ce_count = 0u;
    gpu->ptop_devices = 0u;
    gpu->gr_engine = gpu->gr_runlist = NV_NO_ID;
    gpu->ce_engine = gpu->ce_runlist = NV_NO_ID;

    uint8_t ce_eng[4], ce_run[4];
    uint32_t engine = NV_NO_ID, runlist = NV_NO_ID, type = UINT32_MAX;
    bool open = false;
    for (uint32_t i = 0u; i < NV_PTOP_ENTRIES; ++i) {
        uint32_t d = nv_rd32(gpu, NV_PTOP_INFO(i));
        if (d == UINT32_MAX) {
            return;     /* register space not responding */
        }
        switch (d & 3u) {
        case 0u:
            continue;
        case 1u:
            open = true;
            break;
        case 2u:
            open = true;
            if ((d & 0x20u) != 0u)
                engine = (d & 0x3c000000u) >> 26u;
            if ((d & 0x10u) != 0u)
                runlist = (d & 0x01e00000u) >> 21u;
            break;
        default:
            open = true;
            type = (d & 0x7ffffffcu) >> 2u;
            break;
        }
        if ((d & 0x80000000u) != 0u || !open) {
            continue;   /* more words for this device */
        }
        ++gpu->ptop_devices;
        nv_diag(gpu, "PTOP device (type<<16|engine<<8|runlist)",
            (type << 16u) | ((engine & 0xffu) << 8u) | (runlist & 0xffu));
        if (type == NV_PTOP_TYPE_GR && engine != NV_NO_ID
            && runlist != NV_NO_ID && !gpu->gr_found) {
            gpu->gr_found = true;
            gpu->gr_engine = (uint8_t)engine;
            gpu->gr_runlist = (uint8_t)runlist;
        } else if ((type == NV_PTOP_TYPE_CE || type == NV_PTOP_TYPE_CE_PASCAL)
            && engine != NV_NO_ID && runlist != NV_NO_ID
            && gpu->ce_count < 4u) {
            ce_eng[gpu->ce_count] = (uint8_t)engine;
            ce_run[gpu->ce_count] = (uint8_t)runlist;
            ++gpu->ce_count;
        }
        engine = runlist = NV_NO_ID;
        type = UINT32_MAX;
        open = false;
    }
    gpu->ptop_valid = gpu->ptop_devices != 0u;

    /* Prefer a copy engine that does not share the graphics runlist: the
     * graphics engine has no context here and must not gate our channel. */
    for (uint32_t i = 0u; i < gpu->ce_count; ++i) {
        if (!gpu->ce_found
            || (gpu->gr_found && gpu->ce_runlist == gpu->gr_runlist
                && ce_run[i] != gpu->gr_runlist)) {
            gpu->ce_found = true;
            gpu->ce_engine = ce_eng[i];
            gpu->ce_runlist = ce_run[i];
        }
    }
}

/* ---- Push buffer ring ------------------------------------------------ */

bool nv_pushbuf_init(struct nv_gpu *gpu, uint32_t *pushbuf, uint32_t words,
    uint64_t *gpfifo, uint32_t *seg_offset, uint32_t entries,
    volatile uint32_t *userd)
{
    if (pushbuf == NULL || gpfifo == NULL || seg_offset == NULL
        || userd == NULL || words < 16u || entries < 2u
        || (entries & (entries - 1u)) != 0u) {
        return false;
    }
    gpu->fence = NULL;
    gpu->fence_gpu_addr = 0u;
    if (words >= 2048u && (((uintptr_t)pushbuf) & 0xfffu) == 0u) {
        /* Reserve the final 4 KiB page as the completion fence. */
        words &= ~1023u;
        words -= 1024u;
        gpu->fence = (volatile uint32_t *)(pushbuf + words);
        gpu->fence[0] = 0u;
    }
    gpu->pushbuf = pushbuf;
    gpu->pushbuf_words = words;
    gpu->gpfifo = gpfifo;
    gpu->seg_offset = seg_offset;
    gpu->gpfifo_entries = entries;
    gpu->userd = userd;
    gpu->put = 0u;
    gpu->seg_start = 0u;
    gpu->gp_put = 0u;
    gpu->gp_get_shadow = 0u;
    gpu->fence_seq = 0u;
    gpu->pushbuf_gpu_addr = (uint64_t)(uintptr_t)pushbuf;
    userd[NV_USERD_GP_GET / 4u] = 0u;
    userd[NV_USERD_GP_PUT / 4u] = 0u;
    return true;
}

static uint32_t nv_gp_get(const struct nv_gpu *gpu)
{
    return gpu->userd[NV_USERD_GP_GET / 4u] & (gpu->gpfifo_entries - 1u);
}

static uint32_t nv_inflight(const struct nv_gpu *gpu)
{
    return (gpu->gp_put - nv_gp_get(gpu)) & (gpu->gpfifo_entries - 1u);
}

bool nv_pb_submit(struct nv_gpu *gpu)
{
    if (gpu->put == gpu->seg_start) {
        return true;
    }
    if (nv_inflight(gpu) >= gpu->gpfifo_entries - 1u) {
        ++gpu->pb_overflows;
        return false;
    }
    uint32_t words = gpu->put - gpu->seg_start;
    gpu->gpfifo[gpu->gp_put] = nv_gpfifo_entry(
        gpu->pushbuf_gpu_addr + (uint64_t)gpu->seg_start * 4u, words);
    gpu->seg_offset[gpu->gp_put] = gpu->seg_start;
    __sync_synchronize();
    gpu->gp_put = (gpu->gp_put + 1u) & (gpu->gpfifo_entries - 1u);
    gpu->seg_start = gpu->put;
    if (gpu->channel_bound) {
        gpu->userd[NV_USERD_GP_PUT / 4u] = gpu->gp_put;
    }
    return true;
}

static bool nv_pb_reserve(struct nv_gpu *gpu, uint32_t words)
{
    if (words > gpu->pushbuf_words - 1u) {
        return false;
    }
    uint32_t inflight = nv_inflight(gpu);
    if (inflight == 0u && gpu->put == gpu->seg_start) {
        gpu->put = 0u;
        gpu->seg_start = 0u;
    }
    uint32_t tail = inflight != 0u ? gpu->seg_offset[nv_gp_get(gpu)]
                                   : gpu->put;
    if (gpu->put >= tail) {
        if (gpu->pushbuf_words - gpu->put >= words) {
            return true;
        }
        if (tail <= words) {
            ++gpu->pb_overflows;
            return false;
        }
        if (!nv_pb_submit(gpu)) {
            return false;
        }
        gpu->put = 0u;
        gpu->seg_start = 0u;
        return true;
    }
    if (tail - gpu->put - 1u >= words) {
        return true;
    }
    ++gpu->pb_overflows;
    return false;
}

bool nv_pb_method(struct nv_gpu *gpu, uint32_t subchannel, uint32_t method,
    const uint32_t *data, uint32_t count)
{
    if (gpu->pushbuf == NULL || count == 0u || count > 0x1fffu
        || !nv_pb_reserve(gpu, count + 1u)) {
        return false;
    }
    gpu->pushbuf[gpu->put++] =
        nv_pkt_header(NV_PKT_INC, subchannel, method, count);
    for (uint32_t i = 0u; i < count; ++i) {
        gpu->pushbuf[gpu->put++] = data[i];
    }
    return true;
}

static void nv_relax(const struct nv_gpu *gpu)
{
    if (gpu->plat.relax != NULL) {
        gpu->plat.relax(gpu->plat.ctx);
    }
    __asm__ volatile ("" ::: "memory");
}

bool nv_pb_idle(struct nv_gpu *gpu)
{
    if (!nv_pb_submit(gpu)) {
        return false;
    }
    for (uint32_t i = 0u; i < 1000000u; ++i) {
        if (nv_inflight(gpu) == 0u) {
            return true;
        }
        nv_relax(gpu);
    }
    return false;
}

/* ---- VRAM access through the PRAMIN window -------------------------- */

static void nv_vram_wr32(struct nv_gpu *gpu, uint64_t addr, uint32_t value)
{
    if (gpu->plat.vram_write32 != NULL) {
        gpu->plat.vram_write32(gpu->plat.ctx, addr, value);
        return;
    }
    uint32_t window = (uint32_t)(addr >> 16u);
    if (gpu->pramin_window != window + 1u) {
        nv_write32(gpu, NV_PRAMIN_WINDOW, window);
        (void)nv_read32(gpu, NV_PRAMIN_WINDOW);
        gpu->pramin_window = window + 1u;
    }
    nv_write32(gpu, NV_PRAMIN + (uint32_t)(addr & 0xffffu), value);
}

static uint32_t nv_vram_rd32(struct nv_gpu *gpu, uint64_t addr)
{
    if (gpu->plat.vram_read32 != NULL) {
        return gpu->plat.vram_read32(gpu->plat.ctx, addr);
    }
    uint32_t window = (uint32_t)(addr >> 16u);
    if (gpu->pramin_window != window + 1u) {
        nv_write32(gpu, NV_PRAMIN_WINDOW, window);
        (void)nv_read32(gpu, NV_PRAMIN_WINDOW);
        gpu->pramin_window = window + 1u;
    }
    return nv_read32(gpu, NV_PRAMIN + (uint32_t)(addr & 0xffffu));
}

static void nv_vram_wr64(struct nv_gpu *gpu, uint64_t addr, uint64_t value)
{
    nv_vram_wr32(gpu, addr, (uint32_t)value);
    nv_vram_wr32(gpu, addr + 4u, (uint32_t)(value >> 32u));
}

static void nv_vram_zero(struct nv_gpu *gpu, uint64_t addr, uint32_t bytes)
{
    for (uint32_t i = 0u; i < bytes; i += 4u) {
        nv_vram_wr32(gpu, addr + i, 0u);
    }
}

/* ---- GPU virtual memory (gf100 MMU, 4 KiB small pages, VRAM tables) -- */

#define NV_PTE_VALID 1ull
#define NV_PTE_VOL (1ull << 32u)
#define NV_APER_VRAM 0ull
#define NV_APER_HOST 2ull

/* gf100_vmm_valid: bit0 valid, bit32 volatile, bits 34:33 aperture. */
static uint64_t nv_pte(uint64_t addr, uint64_t aper, bool vol)
{
    return (addr >> 8u) | NV_PTE_VALID | (aper << 33u)
        | (vol ? NV_PTE_VOL : 0ull);
}

/* gf100_vmm_pgd_pde: the small-page table pointer lives in the high word. */
static uint64_t nv_pde_small(uint64_t spt_vram)
{
    return (1ull << 32u) | (spt_vram << 24u);
}

/* Instance block words 0x200..0x20f: page directory pointer and VA limit. */
static void nv_inst_join(struct nv_gpu *gpu, uint64_t inst, uint64_t pd,
    uint64_t limit)
{
    nv_vram_wr64(gpu, inst + 0x200u, pd | 0u);
    nv_vram_wr64(gpu, inst + 0x208u, limit - 1u);
}

static bool nv_mmu_flush(struct nv_gpu *gpu, uint64_t pd_vram)
{
    uint32_t i;
    for (i = 0u; i < 100000u; ++i) {
        if ((nv_read32(gpu, NV_MMU_FLUSH_STATUS) & 0x00ff0000u) != 0u) {
            break;
        }
        nv_relax(gpu);
    }
    if (i == 100000u) {
        return false;
    }
    nv_write32(gpu, NV_MMU_INVALIDATE_PDB, (uint32_t)((pd_vram >> 12u) << 4u));
    nv_write32(gpu, NV_MMU_INVALIDATE, 0x80000001u);
    for (i = 0u; i < 100000u; ++i) {
        if ((nv_read32(gpu, NV_MMU_FLUSH_STATUS) & 0x00008000u) != 0u) {
            return true;
        }
        nv_relax(gpu);
    }
    return false;
}

static uint64_t nv_bar1_map_size(const struct nv_gpu *gpu)
{
    return gpu->bar1_size < NV_BAR1_MAX_MAP ? gpu->bar1_size
                                            : NV_BAR1_MAX_MAP;
}

static uint32_t nv_pbdma_count(const struct nv_gpu *gpu)
{
    uint32_t mask = nv_rd32(gpu, NV_PMC_DEV_ENABLE);
    uint32_t n = 0u;
    if (mask == UINT32_MAX) {
        return 0u;
    }
    for (; mask != 0u && n < NV_PBDMA_MAX; mask &= mask - 1u) {
        ++n;
    }
    return n;
}

/* Register snapshot for physical-hardware debugging.  PBDMA/engine status
 * reads are best-effort (read-only, layout taken from nouveau gk104). */
static void nv_diag_state(struct nv_gpu *gpu, const char *why)
{
    uint32_t get = gpu->userd != NULL
        ? gpu->userd[NV_USERD_GP_GET / 4u] & (gpu->gpfifo_entries - 1u) : 0u;
    nv_diag(gpu, why, 0u);
    nv_diag(gpu, "PMC_ENABLE", nv_read32(gpu, NV_PMC_ENABLE));
    nv_diag(gpu, "PMC_DEV_ENABLE", nv_read32(gpu, NV_PMC_DEV_ENABLE));
    nv_diag(gpu, "BAR1_BIND", nv_read32(gpu, NV_BUS_BAR1_BIND));
    nv_diag(gpu, "CHAN_INST", nv_read32(gpu, NV_PFIFO_CHAN_INST(NV_CH_ID)));
    nv_diag(gpu, "CHAN_CTRL", nv_read32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID)));
    nv_diag(gpu, "RUNL_PENDING",
        nv_read32(gpu, NV_PFIFO_RUNL_PENDING(gpu->chan_runl & 15u)));
    nv_diag(gpu, "SCHED_DISABLE", nv_read32(gpu, NV_PFIFO_SCHED_DISABLE));
    nv_diag(gpu, "PFIFO_INTR", nv_read32(gpu, NV_PFIFO_INTR));
    uint32_t n = nv_pbdma_count(gpu);
    for (uint32_t p = 0u; p < n && p < 4u; ++p) {
        nv_diag(gpu, "PBDMA_INTR", nv_read32(gpu, NV_PBDMA_INTR(p)));
    }
    nv_diag(gpu, "ENGN0_STATUS", nv_read32(gpu, NV_PFIFO_ENGN_STATUS(0u)));
    nv_diag(gpu, "GP_GET", get);
    nv_diag(gpu, "GP_PUT", gpu->gp_put);
    nv_diag(gpu, "GP_DELTA", (gpu->gp_put - get) & (gpu->gpfifo_entries - 1u));
    nv_diag(gpu, "FENCE", gpu->fence != NULL ? gpu->fence[0] : 0u);
    nv_diag(gpu, "FENCE_SEQ", gpu->fence_seq);
}

/* The firmware's BAR1 mapping is replaced by an identity map of VRAM.  The
 * display engine scans out by VRAM address (not through BAR1), so the picture
 * cannot blank; what must hold is that CPU framebuffer writes through BAR1
 * still land on the same VRAM pages, so verify every scan-out PTE. */
static bool nv_verify_bar1_scanout(struct nv_gpu *gpu, uint64_t map)
{
    if (!gpu->scanout.valid) {
        return false;
    }
    uint64_t first = gpu->scanout.vram_offset & ~0xfffull;
    uint64_t end = gpu->scanout.vram_offset
        + (uint64_t)gpu->scanout.pitch * gpu->scanout.height;
    if (end > map) {
        return false;
    }
    uint64_t base = gpu->vram_base;
    uint64_t spt[2] = { base + NV_B1_SPT0, base + NV_B1_SPT1 };
    for (uint64_t off = first; off < end; off += 0x1000u) {
        uint64_t page = off >> 12u;
        uint64_t want = nv_pte(off, NV_APER_VRAM, false);
        if (nv_vram_rd32(gpu, spt[page >> 15u] + (page & 0x7fffu) * 8u)
                != (uint32_t)want
            || nv_vram_rd32(gpu, spt[page >> 15u] + (page & 0x7fffu) * 8u
                + 4u) != (uint32_t)(want >> 32u)) {
            return false;
        }
    }
    return true;
}

static bool nv_build_bar1(struct nv_gpu *gpu)
{
    uint64_t base = gpu->vram_base;
    uint64_t map = nv_bar1_map_size(gpu);
    uint64_t inst = base + NV_B1_INST;
    uint64_t pd = base + NV_B1_PD;
    uint64_t spt[2] = { base + NV_B1_SPT0, base + NV_B1_SPT1 };

    nv_vram_zero(gpu, inst, 0x1000u);
    nv_vram_zero(gpu, pd, 0x1000u);
    for (uint32_t pde = 0u; pde < 2u; ++pde) {
        nv_vram_zero(gpu, spt[pde], NV_B1_SPT_BYTES);
    }
    nv_inst_join(gpu, inst, pd, map);
    for (uint32_t pde = 0u; pde < 2u; ++pde) {
        if ((uint64_t)pde << 27u >= map) {
            break;
        }
        nv_vram_wr64(gpu, pd + pde * 8u, nv_pde_small(spt[pde]));
    }
    /* Identity map BAR1 window offset -> VRAM offset so the existing
     * scan-out surface keeps its address. */
    for (uint64_t off = 0u; off < map; off += 0x1000u) {
        uint64_t page = off >> 12u;
        nv_vram_wr64(gpu, spt[page >> 15u] + (page & 0x7fffu) * 8u,
            nv_pte(off, NV_APER_VRAM, false));
    }
    if (!nv_verify_bar1_scanout(gpu, map)) {
        return false;
    }
    gpu->saved_bar1_bind = nv_read32(gpu, NV_BUS_BAR1_BIND);
    gpu->bar1_replaced = true;
    nv_diag(gpu, "BAR1_BIND(old)", gpu->saved_bar1_bind);
    nv_write32(gpu, NV_BUS_BAR1_BIND, 0x80000000u | (uint32_t)(inst >> 12u));
    return nv_mmu_flush(gpu, pd);
}

static void nv_map_pages(struct nv_gpu *gpu, uint64_t spt, uint32_t first_pte,
    uint64_t phys, uint32_t pages, uint64_t aper, bool vol)
{
    for (uint32_t i = 0u; i < pages; ++i) {
        nv_vram_wr64(gpu, spt + (uint64_t)(first_pte + i) * 8u,
            nv_pte(phys + (uint64_t)i * 0x1000u, aper, vol));
    }
}

/* Channel address space (one 128 MiB PDE, 4 KiB pages):
 *   0x010000  pushbuffer, GPFIFO ring, fence page    (host memory)
 *   0x200000  CE self-test scratch surface           (VRAM)
 *   0x400000  scan-out surface window                (VRAM)             */
static bool nv_build_channel_vm(struct nv_gpu *gpu, uint64_t *gpfifo_va)
{
    uint64_t base = gpu->vram_base;
    uint64_t pd = base + NV_CH_PD;
    uint64_t spt = base + NV_CH_SPT;
    uint32_t pb_pages = (gpu->pushbuf_words * 4u + 0xfffu) >> 12u;
    uint32_t gp_pages = (gpu->gpfifo_entries * 8u + 0xfffu) >> 12u;
    uint64_t pb_phys = (uint64_t)(uintptr_t)gpu->pushbuf;
    uint64_t gp_phys = (uint64_t)(uintptr_t)gpu->gpfifo;
    uint64_t fence_phys = (uint64_t)(uintptr_t)gpu->fence;
    const struct nv_scanout *s = &gpu->scanout;

    if (gpu->fence == NULL
        || ((pb_phys | gp_phys | fence_phys) & 0xfffu) != 0u) {
        return false;
    }
    uint32_t pte_pb = NV_CH_VA_PUSHBUF >> 12u;
    uint32_t pte_gp = pte_pb + pb_pages;
    uint32_t pte_fence = pte_gp + gp_pages;
    if (pte_fence + 1u > (NV_CH_VA_SCRATCH >> 12u)) {
        return false;
    }
    uint64_t fb_first = s->vram_offset >> 12u;
    uint64_t fb_end = (s->vram_offset + (uint64_t)s->pitch * s->height
        + 0xfffu) >> 12u;
    uint64_t fb_pages = fb_end - fb_first;
    if (fb_pages == 0u || fb_pages > (NV_CH_FB_MAX >> 12u)
        || (NV_CH_VA_FB >> 12u) + fb_pages
            > NV_CH_SPT_BYTES / 8u) {
        return false;
    }

    nv_vram_zero(gpu, pd, 0x1000u);
    nv_vram_zero(gpu, spt, NV_CH_SPT_BYTES);
    nv_vram_wr64(gpu, pd, nv_pde_small(spt));

    nv_map_pages(gpu, spt, pte_pb, pb_phys, pb_pages, NV_APER_HOST, true);
    *gpfifo_va = (uint64_t)pte_gp << 12u;
    nv_map_pages(gpu, spt, pte_gp, gp_phys, gp_pages, NV_APER_HOST, true);
    gpu->fence_gpu_addr = (uint64_t)pte_fence << 12u;
    nv_map_pages(gpu, spt, pte_fence, fence_phys, 1u, NV_APER_HOST, true);
    nv_map_pages(gpu, spt, NV_CH_VA_SCRATCH >> 12u, base + NV_CH_SCRATCH,
        NV_CH_SCRATCH_BYTES >> 12u, NV_APER_VRAM, false);
    nv_map_pages(gpu, spt, NV_CH_VA_FB >> 12u, fb_first << 12u,
        (uint32_t)fb_pages, NV_APER_VRAM, false);

    gpu->pushbuf_gpu_addr = NV_CH_VA_PUSHBUF;
    gpu->fb_gpu_addr = NV_CH_VA_FB + (s->vram_offset & 0xfffu);
    nv_inst_join(gpu, base + NV_CH_INST, pd, NV_CH_VA_LIMIT);
    return nv_mmu_flush(gpu, pd);
}

static void nv_write_ramfc(struct nv_gpu *gpu, uint64_t inst, uint64_t userd,
    uint64_t gpfifo_va)
{
    uint32_t limit2 = 0u;
    while ((1u << (limit2 + 1u)) <= gpu->gpfifo_entries) {
        ++limit2;
    }
    struct nv_ramfc f = {
        .userd_lo = (uint32_t)userd,
        .userd_hi = (uint32_t)(userd >> 32u),
        .signature = 0x0000faceu,
        .gpfifo_lo = (uint32_t)gpfifo_va,
        .gpfifo_hi_limit = (uint32_t)(gpfifo_va >> 32u) | (limit2 << 16u),
        .chid = NV_CH_ID,
    };
    nv_vram_zero(gpu, inst, 0x200u);
    nv_vram_wr32(gpu, inst + 0x08u, f.userd_lo);
    nv_vram_wr32(gpu, inst + 0x0cu, f.userd_hi);
    nv_vram_wr32(gpu, inst + 0x10u, f.signature);
    nv_vram_wr32(gpu, inst + 0x30u, 0xfffff902u);
    nv_vram_wr32(gpu, inst + 0x48u, f.gpfifo_lo);
    nv_vram_wr32(gpu, inst + 0x4cu, f.gpfifo_hi_limit);
    nv_vram_wr32(gpu, inst + 0x84u, 0x20400000u);
    nv_vram_wr32(gpu, inst + 0x94u, 0x30000fffu);
    nv_vram_wr32(gpu, inst + 0x9cu, 0x00000100u);
    nv_vram_wr32(gpu, inst + 0xacu, 0x0000001fu);
    nv_vram_wr32(gpu, inst + 0xb8u, 0xf8000000u);
    nv_vram_wr32(gpu, inst + 0xe4u, 0u);
    nv_vram_wr32(gpu, inst + 0xe8u, f.chid);
    nv_vram_wr32(gpu, inst + 0xf8u, 0x10003080u);
    nv_vram_wr32(gpu, inst + 0xfcu, 0x10000010u);
}

static bool nv_verify_ramfc(struct nv_gpu *gpu, uint64_t inst, uint64_t userd,
    uint64_t gpfifo_va, uint64_t pd)
{
    uint32_t limit2 = 0u;
    while ((1u << (limit2 + 1u)) <= gpu->gpfifo_entries) {
        ++limit2;
    }
    return nv_vram_rd32(gpu, inst + 0x08u) == (uint32_t)userd
        && nv_vram_rd32(gpu, inst + 0x0cu) == (uint32_t)(userd >> 32u)
        && nv_vram_rd32(gpu, inst + 0x10u) == 0x0000faceu
        && nv_vram_rd32(gpu, inst + 0x48u) == (uint32_t)gpfifo_va
        && nv_vram_rd32(gpu, inst + 0x4cu)
            == ((uint32_t)(gpfifo_va >> 32u) | (limit2 << 16u))
        && nv_vram_rd32(gpu, inst + 0x94u) == 0x30000fffu
        && nv_vram_rd32(gpu, inst + 0xe4u) == 0u /* non-privileged */
        && nv_vram_rd32(gpu, inst + 0xe8u) == NV_CH_ID
        && nv_vram_rd32(gpu, inst + 0x200u) == (uint32_t)pd
        && nv_vram_rd32(gpu, inst + 0x204u) == (uint32_t)(pd >> 32u)
        && nv_vram_rd32(gpu, inst + 0x208u)
            == (uint32_t)(NV_CH_VA_LIMIT - 1u)
        && nv_vram_rd32(gpu, inst + 0x20cu) == 0u;
}

static bool nv_wait_runlist_id(struct nv_gpu *gpu, uint8_t runlist)
{
    uint32_t reg = NV_PFIFO_RUNL_PENDING(runlist & 15u);
    for (uint32_t i = 0u; i < 1000000u; ++i) {
        if ((nv_read32(gpu, reg) & NV_PFIFO_RUNL_PENDING_BIT) == 0u) {
            return true;
        }
        nv_relax(gpu);
    }
    return false;
}

static bool nv_wait_runlist(struct nv_gpu *gpu)
{
    return nv_wait_runlist_id(gpu, gpu->chan_runl);
}

static bool nv_runlist_valid(uint32_t base, uint32_t count)
{
    return base != UINT32_MAX && count != UINT32_MAX;
}

static void nv_clear_bind_state(struct nv_gpu *gpu)
{
    gpu->pfifo_enabled_by_us = false;
    gpu->bar1_replaced = false;
    gpu->pmc_204_changed = false;
    gpu->runlist_committed = false;
    gpu->pfifo_state_saved = false;
    gpu->userd_mapped_by_us = false;
}

/* Capture every PFIFO/PBDMA register nv_channel_bind is about to change,
 * then mask PMC interrupts for the duration of the channel's ownership. */
static void nv_save_pfifo_state(struct nv_gpu *gpu)
{
    gpu->saved_pfifo_chan_ctrl =
        nv_read32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID));
    gpu->saved_pfifo_chan_inst =
        nv_read32(gpu, NV_PFIFO_CHAN_INST(NV_CH_ID));
    gpu->saved_pfifo_runlist_base =
        nv_read32(gpu, NV_PFIFO_RUNLIST_BASE);
    gpu->saved_pfifo_runlist_count =
        nv_read32(gpu, NV_PFIFO_RUNLIST_COUNT);
    gpu->saved_pfifo_sched_disable =
        nv_read32(gpu, NV_PFIFO_SCHED_DISABLE);
    gpu->saved_pfifo_bar1_userd =
        nv_read32(gpu, NV_PFIFO_BAR1_USERD);
    gpu->saved_pfifo_intr =
        nv_read32(gpu, NV_PFIFO_INTR);
    for (uint32_t p = 0u; p < NV_PBDMA_MAX; ++p) {
        gpu->saved_pbdma_ctrl13c[p] =
            nv_read32(gpu, NV_PBDMA_CTRL13C(p));
        gpu->saved_pbdma_intr[p] =
            nv_read32(gpu, NV_PBDMA_INTR(p));
    }
    gpu->saved_pmc_intr_en_0 =
        nv_read32(gpu, NV_PMC_INTR_EN_0);
    gpu->pfifo_state_saved = true;

    nv_diag(gpu, "PMC_INTR_EN_0(saved)", gpu->saved_pmc_intr_en_0);
    nv_write32(gpu, NV_PMC_INTR_EN_0, 0u);
}

/* Restores PFIFO/PBDMA registers in reverse order.  PMC interrupt enable is
 * restored later, after PFIFO itself has been disabled again. */
static void nv_restore_pfifo_state(struct nv_gpu *gpu)
{
    uint64_t base = gpu->vram_base;

    nv_write32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID),
        nv_read32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID)) | 0x800u);
    nv_write32(gpu, NV_PFIFO_CHAN_INST(NV_CH_ID), 0u);

    if (gpu->runlist_committed) {
        nv_write32(gpu, NV_PFIFO_RUNLIST_BASE,
            (uint32_t)((base + NV_CH_RUNLIST) >> 12u));
        nv_write32(gpu, NV_PFIFO_RUNLIST_COUNT,
            (gpu->chan_runl & 15u) << 20u);
        if (!nv_wait_runlist(gpu)) {
            nv_diag(gpu, "teardown: runlist still pending", 0u);
        }
        gpu->runlist_committed = false;
    }

    nv_write32(gpu, NV_PFIFO_RUNLIST_BASE, gpu->saved_pfifo_runlist_base);
    nv_write32(gpu, NV_PFIFO_RUNLIST_COUNT, gpu->saved_pfifo_runlist_count);
    if (nv_runlist_valid(gpu->saved_pfifo_runlist_base,
            gpu->saved_pfifo_runlist_count)
        && !nv_wait_runlist_id(gpu,
            (uint8_t)(gpu->saved_pfifo_runlist_count >> 20u))) {
        nv_diag(gpu, "teardown: saved runlist still pending",
            gpu->saved_pfifo_runlist_count >> 20u);
    }

    nv_write32(gpu, NV_PFIFO_CHAN_INST(NV_CH_ID),
        gpu->saved_pfifo_chan_inst);
    nv_write32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID),
        gpu->saved_pfifo_chan_ctrl);
    nv_write32(gpu, NV_PFIFO_BAR1_USERD, gpu->saved_pfifo_bar1_userd);
    nv_write32(gpu, NV_PFIFO_SCHED_DISABLE, gpu->saved_pfifo_sched_disable);
    nv_write32(gpu, NV_PFIFO_INTR, gpu->saved_pfifo_intr);
    for (uint32_t i = 0u; i < NV_PBDMA_MAX; ++i) {
        uint32_t p = NV_PBDMA_MAX - 1u - i;
        nv_write32(gpu, NV_PBDMA_INTR(p), gpu->saved_pbdma_intr[p]);
        nv_write32(gpu, NV_PBDMA_CTRL13C(p), gpu->saved_pbdma_ctrl13c[p]);
    }
}

/* Undoes everything nv_channel_bind touched, in reverse order, and is safe to
 * call at any point of a partial bind (and twice).  All channel memory is
 * static kernel BSS or VRAM at vram_base, so there is nothing to free. */
static void nv_channel_teardown(struct nv_gpu *gpu)
{
    if (gpu->bar0 != NULL && gpu->bind_started) {
        uint64_t base = gpu->vram_base;

        if (gpu->pfifo_state_saved) {
            nv_restore_pfifo_state(gpu);
        }
        if (gpu->bar1_replaced) {
            nv_write32(gpu, NV_BUS_BAR1_BIND, gpu->saved_bar1_bind);
            (void)nv_read32(gpu, NV_BUS_BAR1_BIND);
            gpu->bar1_replaced = false;
        }
        if (gpu->pmc_204_changed) {
            nv_write32(gpu, NV_PMC_DEV_ENABLE, gpu->saved_pmc_204);
            gpu->pmc_204_changed = false;
        }
        if (gpu->pfifo_enabled_by_us) {
            nv_write32(gpu, NV_PMC_ENABLE,
                nv_read32(gpu, NV_PMC_ENABLE) & ~NV_PMC_ENABLE_PFIFO);
            gpu->pfifo_enabled_by_us = false;
        }
        if (gpu->pfifo_state_saved) {
            nv_write32(gpu, NV_PMC_INTR_EN_0, gpu->saved_pmc_intr_en_0);
            gpu->pmc_intr_en_0 = gpu->saved_pmc_intr_en_0;
            gpu->pfifo_state_saved = false;
        }
        static const struct { uint32_t off, bytes; } blocks[] = {
            { NV_CH_INST, 0x1000u }, { NV_CH_RUNLIST, 0x1000u },
            { NV_CH_PD, 0x1000u },
            { NV_B1_INST, 0x1000u }, { NV_B1_PD, 0x1000u },
            { NV_B1_SPT0, NV_B1_SPT_BYTES },
            { NV_B1_SPT1, NV_B1_SPT_BYTES },
            { NV_CH_SPT, NV_CH_SPT_BYTES },
            { NV_CH_USERD, 0x1000u }, { NV_CH_SCRATCH, NV_CH_SCRATCH_BYTES },
        };
        for (uint32_t i = 0u; i < sizeof(blocks) / sizeof(blocks[0]); ++i) {
            nv_vram_zero(gpu, base + blocks[i].off, blocks[i].bytes);
        }
        nv_write32(gpu, NV_PRAMIN_WINDOW, gpu->saved_pramin_window);
        gpu->pramin_window = 0u;
    }
    if (gpu->userd_mapped_by_us && gpu->userd != NULL) {
        if (gpu->plat.unmap_mmio != NULL) {
            gpu->plat.unmap_mmio(gpu->plat.ctx,
                (volatile uint8_t *)gpu->userd, 0x1000u);
        }
        gpu->userd_mapped_by_us = false;
    }
    if (gpu->saved_userd != NULL) {
        gpu->userd = gpu->saved_userd;
        gpu->saved_userd = NULL;
    }
    gpu->put = 0u;
    gpu->seg_start = 0u;
    gpu->gp_put = 0u;
    gpu->bind_started = false;
    gpu->channel_bound = false;
    gpu->ring_acked = false;
    gpu->ce_verified = false;
    if (gpu->pushbuf != NULL) {
        gpu->pushbuf_gpu_addr = (uint64_t)(uintptr_t)gpu->pushbuf;
    }
}

void nv_channel_unbind(struct nv_gpu *gpu)
{
    nv_channel_teardown(gpu);
}

static bool nv_bind_fail(struct nv_gpu *gpu, const char *why)
{
    gpu->bind_fail = why;
    nv_diag_state(gpu, why);
    nv_channel_teardown(gpu);
    return false;
}

/* gk104_fifo_init equivalents that matter for a polled, single-channel use:
 * make sure every PBDMA unit is enabled, clear stale PBDMA interrupt state
 * and unblock the runlist scheduler.  nv_save_pfifo_state() has already saved
 * this state and masked PMC interrupts; teardown restores both. */
static void nv_pfifo_prepare(struct nv_gpu *gpu)
{
    uint32_t dev = nv_read32(gpu, NV_PMC_DEV_ENABLE);
    nv_diag(gpu, "PMC_DEV_ENABLE(before)", dev);
    if (dev != UINT32_MAX) {
        gpu->saved_pmc_204 = dev;
        nv_write32(gpu, NV_PMC_DEV_ENABLE, 0xffffffffu);
        gpu->pmc_204_changed = true;
        nv_diag(gpu, "PMC_DEV_ENABLE(after)",
            nv_read32(gpu, NV_PMC_DEV_ENABLE));
    }
    uint32_t n = nv_pbdma_count(gpu);
    for (uint32_t p = 0u; p < n; ++p) {
        nv_write32(gpu, NV_PBDMA_CTRL13C(p),
            nv_read32(gpu, NV_PBDMA_CTRL13C(p)) & ~NV_PBDMA_CTRL13C_MASK);
        nv_write32(gpu, NV_PBDMA_INTR(p), 0xffffffffu);
    }
    nv_write32(gpu, NV_PFIFO_INTR, 0xffffffffu);
    nv_write32(gpu, NV_PFIFO_SCHED_DISABLE,
        nv_read32(gpu, NV_PFIFO_SCHED_DISABLE) & ~(1u << (gpu->chan_runl & 15u)));
}

/* ---- CPU cache maintenance ------------------------------------------- *
 * The scan-out surface is mapped write-back.  The GPU writes VRAM behind the
 * CPU's back, so dirty lines must be written back before the GPU reads or
 * overwrites a region (otherwise they can later evict over the GPU's result)
 * and stale lines must be dropped afterwards.  CLFLUSH does both. */
static void nv_cpu_flush(const struct nv_gpu *gpu, const volatile void *p,
    size_t len)
{
    if (len == 0u) {
        return;
    }
    if (gpu->plat.cache_flush != NULL) {
        gpu->plat.cache_flush(gpu->plat.ctx, (const void *)p, len);
        return;
    }
#if defined(__x86_64__) || defined(__i386__)
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(NV_CACHE_LINE - 1u);
    uintptr_t e = (uintptr_t)p + len;
    __asm__ volatile ("mfence" ::: "memory");
    for (; a < e; a += NV_CACHE_LINE) {
        __asm__ volatile ("clflush (%0)" :: "r"(a) : "memory");
    }
    __asm__ volatile ("mfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static void nv_flush_fb_rect(const struct nv_gpu *gpu, uint32_t x, uint32_t y,
    uint32_t w, uint32_t h)
{
    const struct nv_scanout *s = &gpu->scanout;
    const volatile uint8_t *fb = gpu->bar1 + s->vram_offset;
    if (w * 4u == s->pitch) {
        nv_cpu_flush(gpu, fb + (uint64_t)y * s->pitch, (size_t)s->pitch * h);
        return;
    }
    for (uint32_t r = 0u; r < h; ++r) {
        nv_cpu_flush(gpu, fb + (uint64_t)(y + r) * s->pitch + (uint64_t)x * 4u,
            (size_t)w * 4u);
    }
}

/* ---- Copy-engine command stream -------------------------------------- */

static bool nv_ce_fence_wait(struct nv_gpu *gpu, uint32_t seq)
{
    for (uint32_t i = 0u; i < 2000000u; ++i) {
        if (gpu->fence[0] == seq) {
            return true;
        }
        nv_relax(gpu);
    }
    return false;
}

bool nv_accel_sync(struct nv_gpu *gpu)
{
    if (gpu == NULL || gpu->fence == NULL || !gpu->channel_bound) {
        return false;
    }
    if (!nv_pb_submit(gpu)) {
        return false;
    }
    return gpu->fence_seq == 0u || nv_ce_fence_wait(gpu, gpu->fence_seq);
}

/* Emits one copy-engine operation.  remap != 0 fills with const_a, else a
 * pitch-linear copy of line_bytes x lines.  With fence the engine releases
 * the completion semaphore (after flushing) when the transfer is done. */
static bool nv_ce_emit(struct nv_gpu *gpu, bool fill, uint64_t src_va,
    uint32_t src_pitch, uint64_t dst_va, uint32_t dst_pitch,
    uint32_t line_len, uint32_t lines, uint32_t const_a, bool fence)
{
    uint32_t launch = NVB0B5_LAUNCH_NON_PIPELINED | NVB0B5_LAUNCH_FLUSH
        | NVB0B5_LAUNCH_SRC_PITCH | NVB0B5_LAUNCH_DST_PITCH
        | NVB0B5_LAUNCH_MULTI_LINE;
    if (fill) {
        launch |= NVB0B5_LAUNCH_REMAP;
    }
    if (fence) {
        const uint32_t sem[3] = {
            (uint32_t)((gpu->fence_gpu_addr >> 32u) & 0xffu),
            (uint32_t)gpu->fence_gpu_addr,
            gpu->fence_seq + 1u,
        };
        if (!nv_pb_method(gpu, NV_SUBCHANNEL_CE, NVB0B5_SET_SEMAPHORE_A,
                sem, 3u)) {
            return false;
        }
        launch |= NVB0B5_LAUNCH_SEM_ONE_WORD;
    }
    if (fill) {
        const uint32_t remap[3] = { const_a, 0u, NVB0B5_REMAP_FILL32 };
        if (!nv_pb_method(gpu, NV_SUBCHANNEL_CE, NVB0B5_SET_REMAP_CONST_A,
                remap, 3u)) {
            return false;
        }
    }
    const uint32_t regs[8] = {
        (uint32_t)((src_va >> 32u) & 0xffu), (uint32_t)src_va,
        (uint32_t)((dst_va >> 32u) & 0xffu), (uint32_t)dst_va,
        src_pitch, dst_pitch, line_len, lines,
    };
    if (!nv_pb_method(gpu, NV_SUBCHANNEL_CE, NVB0B5_OFFSET_IN_UPPER, regs, 8u)
        || !nv_pb_method(gpu, NV_SUBCHANNEL_CE, NVB0B5_LAUNCH_DMA, &launch,
            1u)) {
        return false;
    }
    if (fence) {
        ++gpu->fence_seq;
    }
    return true;
}

/* Worst case for one emitted operation: semaphore (4) + remap (4) + offsets
 * (9) + launch (2) words; reserving up front keeps an operation from being
 * left half written in the pushbuffer. */
#define NV_CE_OP_WORDS 24u

static bool nv_ce_begin(struct nv_gpu *gpu, uint32_t ops)
{
    return nv_pb_reserve(gpu, NV_CE_OP_WORDS * ops);
}

static bool nv_ce_finish(struct nv_gpu *gpu)
{
    uint32_t seq = gpu->fence_seq;
    if (!nv_pb_submit(gpu)) {
        return false;
    }
    if (nv_ce_fence_wait(gpu, seq)) {
        return true;
    }
    ++gpu->stat_fence_timeouts;
    gpu->accel_failed = true;
    nv_diag_state(gpu, "CE fence timeout; GPU acceleration disabled");
    return false;
}

static bool nv_ce_fill_va(struct nv_gpu *gpu, uint64_t surface_va,
    uint32_t pitch, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    uint32_t color)
{
    uint64_t dst = surface_va + (uint64_t)y * pitch + (uint64_t)x * 4u;
    return nv_ce_begin(gpu, 1u)
        && nv_ce_emit(gpu, true, dst, pitch, dst, pitch, w, h, color, true)
        && nv_ce_finish(gpu);
}

/* Same-surface rectangle copy with memmove semantics.  The engine walks
 * lines in order, so rectangles that overlap in both axes are split into
 * bands no taller than the vertical shift (each band's source and
 * destination are then disjoint) and issued in the safe direction.  Overlap
 * on the very same rows cannot be split and is left to the CPU. */
static bool nv_ce_copy_va(struct nv_gpu *gpu, uint64_t surface_va,
    uint32_t pitch, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
    uint32_t w, uint32_t h)
{
    if (sx == dx && sy == dy) {
        return true;
    }
    bool y_overlap = sy < dy + h && dy < sy + h;
    bool x_overlap = sx < dx + w && dx < sx + w;
    bool down = dy > sy;
    uint32_t band = h, bands = 1u;
    if (y_overlap && x_overlap) {
        if (sy == dy) {
            ++gpu->stat_unsupported;
            return false;
        }
        band = down ? dy - sy : sy - dy;
        bands = (h + band - 1u) / band;
        if (bands > NV_ACCEL_MAX_BANDS) {
            ++gpu->stat_unsupported;
            return false;
        }
    }
    if (!nv_ce_begin(gpu, bands)) {
        return false;
    }
    for (uint32_t b = 0u; b < bands; ++b) {
        uint32_t idx = down ? bands - 1u - b : b;
        uint32_t r0 = idx * band;
        uint32_t rows = h - r0 < band ? h - r0 : band;
        uint64_t src = surface_va + (uint64_t)(sy + r0) * pitch
            + (uint64_t)sx * 4u;
        uint64_t dst = surface_va + (uint64_t)(dy + r0) * pitch
            + (uint64_t)dx * 4u;
        if (!nv_ce_emit(gpu, false, src, pitch, dst, pitch, w * 4u, rows, 0u,
                b + 1u == bands)) {
            return false;
        }
    }
    return nv_ce_finish(gpu);
}

/* ---- On-hardware self-test -------------------------------------------- *
 * Runs on a private VRAM scratch surface (never the scan-out) and compares
 * the engine's output with a CPU reference model, including the banded
 * overlapping copy path.  Any mismatch keeps acceleration disabled. */
#define NV_ST_PITCH 256u
#define NV_ST_PX 64u
#define NV_ST_ROWS 16u
static uint32_t nv_st_ref[NV_ST_PX * NV_ST_ROWS];

static void nv_st_ref_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    uint32_t c)
{
    for (uint32_t r = 0u; r < h; ++r)
        for (uint32_t i = 0u; i < w; ++i)
            nv_st_ref[(y + r) * NV_ST_PX + x + i] = c;
}

static void nv_st_ref_copy(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
    uint32_t w, uint32_t h)
{
    uint32_t tmp[NV_ST_PX * 8u];
    for (uint32_t r = 0u; r < h; ++r)
        for (uint32_t i = 0u; i < w; ++i)
            tmp[r * w + i] = nv_st_ref[(sy + r) * NV_ST_PX + sx + i];
    for (uint32_t r = 0u; r < h; ++r)
        for (uint32_t i = 0u; i < w; ++i)
            nv_st_ref[(dy + r) * NV_ST_PX + dx + i] = tmp[r * w + i];
}

static bool nv_st_verify(struct nv_gpu *gpu, const char *step)
{
    uint64_t base = gpu->vram_base + NV_CH_SCRATCH;
    for (uint32_t y = 0u; y < NV_ST_ROWS; ++y) {
        for (uint32_t x = 0u; x < NV_ST_PX; ++x) {
            uint32_t got = nv_vram_rd32(gpu,
                base + (uint64_t)y * NV_ST_PITCH + x * 4u);
            if (got != nv_st_ref[y * NV_ST_PX + x]) {
                nv_diag(gpu, step, (y << 16u) | x);
                nv_diag(gpu, "  expected", nv_st_ref[y * NV_ST_PX + x]);
                nv_diag(gpu, "  got", got);
                return false;
            }
        }
    }
    return true;
}

static bool nv_ce_selftest(struct nv_gpu *gpu)
{
    const uint32_t bg = 0xa5a5a5a5u, c1 = 0x00c0ffeeu, c2 = 0x00123456u;
    uint64_t base = gpu->vram_base + NV_CH_SCRATCH;
    for (uint32_t y = 0u; y < NV_ST_ROWS; ++y) {
        for (uint32_t x = 0u; x < NV_ST_PX; ++x) {
            nv_st_ref[y * NV_ST_PX + x] = bg;
            nv_vram_wr32(gpu, base + (uint64_t)y * NV_ST_PITCH + x * 4u, bg);
        }
    }
    (void)nv_vram_rd32(gpu, base);       /* order the posted writes */
    const uint64_t va = NV_CH_VA_SCRATCH;

    if (!nv_ce_fill_va(gpu, va, NV_ST_PITCH, 8u, 2u, 16u, 2u, c1)
        || !nv_ce_fill_va(gpu, va, NV_ST_PITCH, 8u, 4u, 16u, 2u, c2)) {
        nv_diag(gpu, "self-test: fill did not complete", 0u);
        return false;
    }
    nv_st_ref_fill(8u, 2u, 16u, 2u, c1);
    nv_st_ref_fill(8u, 4u, 16u, 2u, c2);
    if (!nv_st_verify(gpu, "self-test fill mismatch (y<<16|x)")) {
        return false;
    }
    /* Disjoint copy. */
    if (!nv_ce_copy_va(gpu, va, NV_ST_PITCH, 8u, 2u, 32u, 8u, 16u, 4u)) {
        nv_diag(gpu, "self-test: copy did not complete", 0u);
        return false;
    }
    nv_st_ref_copy(8u, 2u, 32u, 8u, 16u, 4u);
    if (!nv_st_verify(gpu, "self-test copy mismatch (y<<16|x)")) {
        return false;
    }
    /* Overlapping downward copy: exercises the banded memmove path. */
    if (!nv_ce_copy_va(gpu, va, NV_ST_PITCH, 8u, 2u, 8u, 3u, 16u, 4u)) {
        nv_diag(gpu, "self-test: overlap copy did not complete", 0u);
        return false;
    }
    nv_st_ref_copy(8u, 2u, 8u, 3u, 16u, 4u);
    if (!nv_st_verify(gpu, "self-test overlap mismatch (y<<16|x)")) {
        return false;
    }
    /* Overlapping upward copy. */
    if (!nv_ce_copy_va(gpu, va, NV_ST_PITCH, 8u, 3u, 8u, 2u, 16u, 4u)) {
        nv_diag(gpu, "self-test: upward copy did not complete", 0u);
        return false;
    }
    nv_st_ref_copy(8u, 3u, 8u, 2u, 16u, 4u);
    return nv_st_verify(gpu, "self-test upward mismatch (y<<16|x)");
}

/* ---- Channel bring-up ------------------------------------------------- */

bool nv_channel_bind(struct nv_gpu *gpu, uint64_t vram_base)
{
    uint64_t userd_vram = vram_base + NV_CH_USERD;
    uint64_t gpfifo_va = 0u;

    if (!gpu->present || !gpu->mapped || gpu->chipset != NV_CHIPSET_GM204
        || gpu->bar1_size == 0u || gpu->bar1 == NULL || gpu->pushbuf == NULL
        || gpu->fence == NULL
        || !gpu->scanout.valid || gpu->bind_started
        || (vram_base & 0x1fffffu) != 0u
        || vram_base + NV_CH_VRAM_SPAN > gpu->bar1_size
        || vram_base + NV_CH_VRAM_SPAN > NV_BAR1_MAX_MAP
        || vram_base < gpu->scanout.vram_offset
                + (uint64_t)gpu->scanout.pitch * gpu->scanout.height) {
        gpu->bind_fail = "placement rejected";
        return false;
    }
    if (!gpu->ptop_valid) {
        nv_discover_engines(gpu);
    }
    if (!gpu->ce_found) {
        gpu->bind_fail = "no copy engine found in PTOP";
        nv_diag(gpu, "no copy engine found in PTOP", gpu->ptop_devices);
        return false;
    }
    nv_diag(gpu, "CE engine", gpu->ce_engine);
    nv_diag(gpu, "CE runlist", gpu->ce_runlist);
    nv_clear_bind_state(gpu);
    gpu->bind_fail = NULL;
    gpu->accel_failed = false;
    gpu->ce_verified = false;
    gpu->chan_runl = gpu->ce_runlist;
    gpu->vram_base = vram_base;
    gpu->bind_started = true;
    gpu->saved_userd = gpu->userd;
    gpu->saved_pramin_window = nv_read32(gpu, NV_PRAMIN_WINDOW);
    gpu->pramin_window = 0u;
    gpu->channel_bound = false;
    gpu->ring_acked = false;
    gpu->fence_seq = 0u;
    gpu->fence[0] = 0u;

    uint32_t pmc = nv_read32(gpu, NV_PMC_ENABLE);
    nv_diag(gpu, "PMC_ENABLE(before)", pmc);
    if ((pmc & NV_PMC_ENABLE_PFIFO) == 0u) {
        nv_write32(gpu, NV_PMC_ENABLE, pmc | NV_PMC_ENABLE_PFIFO);
        gpu->pfifo_enabled_by_us = true;
        pmc = nv_read32(gpu, NV_PMC_ENABLE);
        nv_diag(gpu, "PMC_ENABLE(after)", pmc);
        if ((pmc & NV_PMC_ENABLE_PFIFO) == 0u) {
            return nv_bind_fail(gpu, "PFIFO enable did not stick");
        }
    }
    /* Capture all PFIFO/PBDMA state before pfifo_prepare changes it and keep
     * PMC interrupts masked until teardown restores the original mask. */
    nv_save_pfifo_state(gpu);
    nv_pfifo_prepare(gpu);

    if (!nv_build_bar1(gpu)) {
        return nv_bind_fail(gpu, "BAR1 VM build/verify/flush");
    }
    if (!nv_build_channel_vm(gpu, &gpfifo_va)) {
        return nv_bind_fail(gpu, "channel VM build/flush");
    }
    /* Building the channel VM zeroes NV_CH_SPT.  Re-check the BAR1 identity
     * PTEs now that all VRAM tables are final. */
    if (!nv_verify_bar1_scanout(gpu, nv_bar1_map_size(gpu))) {
        return nv_bind_fail(gpu,
            "BAR1 scan-out PTEs lost after channel VM build");
    }
    nv_write_ramfc(gpu, vram_base + NV_CH_INST, userd_vram, gpfifo_va);
    if (!nv_verify_ramfc(gpu, vram_base + NV_CH_INST, userd_vram, gpfifo_va,
            vram_base + NV_CH_PD)) {
        return nv_bind_fail(gpu, "RAMFC readback mismatch");
    }

    /* USERD is reached through BAR1 (identity mapped above); only the one
     * page holding it is remapped uncached so framebuffer pages keep their
     * cacheable mapping. */
    nv_vram_zero(gpu, userd_vram, 0x1000u);
    volatile uint32_t *userd = (volatile uint32_t *)gpu->plat.map_mmio(
        gpu->plat.ctx, gpu->bar1_phys + userd_vram, 0x1000u, true);
    if (userd == NULL) {
        return nv_bind_fail(gpu, "USERD BAR1 map");
    }
    gpu->userd = userd;
    gpu->userd_mapped_by_us = true;
    /* A CPU write through BAR1 must land on the VRAM page PRAMIN sees. */
    userd[0x100u / 4u] = 0x4e564231u;
    if (userd[0x100u / 4u] != 0x4e564231u
        || nv_vram_rd32(gpu, userd_vram + 0x100u) != 0x4e564231u) {
        return nv_bind_fail(gpu, "BAR1 identity probe");
    }
    userd[0x100u / 4u] = 0u;
    gpu->gp_put = 0u;
    gpu->put = 0u;
    gpu->seg_start = 0u;
    nv_write32(gpu, NV_PFIFO_BAR1_USERD,
        0x10000000u | (uint32_t)(userd_vram >> 12u));

    /* Runlist entry (gk104): { chid, 0 }, committed from VRAM (target 0). */
    uint64_t runlist = vram_base + NV_CH_RUNLIST;
    nv_vram_wr32(gpu, runlist, NV_CH_ID);
    nv_vram_wr32(gpu, runlist + 4u, 0u);
    if (nv_vram_rd32(gpu, runlist) != NV_CH_ID
        || nv_vram_rd32(gpu, runlist + 4u) != 0u) {
        return nv_bind_fail(gpu, "runlist entry readback");
    }

    uint32_t ctrl = nv_read32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID));
    nv_write32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID),
        (ctrl & ~0x000f0000u) | ((gpu->chan_runl & 15u) << 16u));
    uint32_t inst_reg = 0x80000000u
        | (uint32_t)((vram_base + NV_CH_INST) >> 12u);
    nv_write32(gpu, NV_PFIFO_CHAN_INST(NV_CH_ID), inst_reg);
    if (nv_read32(gpu, NV_PFIFO_CHAN_INST(NV_CH_ID)) != inst_reg) {
        return nv_bind_fail(gpu, "channel instance readback");
    }
    if (!nv_wait_runlist(gpu)) {
        return nv_bind_fail(gpu, "runlist busy before commit");
    }
    nv_write32(gpu, NV_PFIFO_RUNLIST_BASE, (uint32_t)(runlist >> 12u));
    nv_write32(gpu, NV_PFIFO_RUNLIST_COUNT,
        ((gpu->chan_runl & 15u) << 20u) | 1u);
    gpu->runlist_committed = true;
    if (!nv_wait_runlist(gpu)) {
        return nv_bind_fail(gpu, "runlist commit timeout");
    }
    nv_write32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID),
        nv_read32(gpu, NV_PFIFO_CHAN_CTRL(NV_CH_ID)) | NV_PFIFO_CHAN_START);
    nv_diag_state(gpu, "channel started");

    /* Prove the GPU fetches the ring: queue a host NOP (method 0x08, valid
     * on any subchannel) and require GP_GET to catch up with GP_PUT. */
    static const uint32_t nop_data = 0u;
    gpu->channel_bound = true;
    if (!nv_pb_method(gpu, 0u, NV_HOST_NOP, &nop_data, 1u)
        || !nv_pb_idle(gpu)) {
        return nv_bind_fail(gpu, "GPFIFO ring not acknowledged");
    }
    nv_diag_state(gpu, "ring acknowledged");
    gpu->ring_acked = true;

    /* Bind the copy-engine class to its subchannel, then prove it executes. */
    static const uint32_t ce_class = NVB0B5_CLASS;
    if (!nv_pb_method(gpu, NV_SUBCHANNEL_CE, NV_HOST_SET_OBJECT, &ce_class,
            1u) || !nv_pb_submit(gpu)) {
        return nv_bind_fail(gpu, "copy engine SET_OBJECT");
    }
    if (!nv_ce_selftest(gpu)) {
        return nv_bind_fail(gpu, "copy engine self-test failed");
    }
    gpu->ce_verified = true;
    nv_diag(gpu, "copy engine self-test passed", gpu->fence_seq);
    return true;
}

bool nv_adopt_scanout(struct nv_gpu *gpu, uint64_t fb_phys, uint32_t width,
    uint32_t height, uint32_t pitch, uint32_t bits_per_pixel)
{
    gpu->scanout.valid = false;
    if (!gpu->present || gpu->bar1_size == 0u || bits_per_pixel != 32u
        || fb_phys < gpu->bar1_phys
        || pitch < width * 4u || (pitch & 3u) != 0u
        || fb_phys + (uint64_t)pitch * height
            > gpu->bar1_phys + gpu->bar1_size) {
        return false;
    }
    gpu->scanout = (struct nv_scanout){
        .valid = true,
        .physical_base = fb_phys,
        .vram_offset = fb_phys - gpu->bar1_phys,
        .width = width,
        .height = height,
        .pitch = pitch,
    };
    return true;
}

bool nv_accel_ready(const struct nv_gpu *gpu)
{
    return nv_accel_active(gpu);
}

const char *nv_accel_state(const struct nv_gpu *gpu)
{
    if (!gpu->present)
        return "no GPU";
    if (!gpu->mapped)
        return "registers not mapped";
    if (gpu->accel_failed)
        return "disabled after GPU fence timeout";
    if (nv_accel_ready(gpu))
        return "copy-engine acceleration active";
    if (gpu->bind_fail != NULL)
        return gpu->bind_fail;
    if (!gpu->channel_bound)
        return "channel not bound (CPU drawing)";
    return "copy engine not verified";
}

static bool nv_clip(const struct nv_gpu *gpu, uint32_t *x, uint32_t *y,
    uint32_t *w, uint32_t *h)
{
    if (*x >= gpu->scanout.width || *y >= gpu->scanout.height || *w == 0u
        || *h == 0u) {
        return false;
    }
    if (*w > gpu->scanout.width - *x) {
        *w = gpu->scanout.width - *x;
    }
    if (*h > gpu->scanout.height - *y) {
        *h = gpu->scanout.height - *y;
    }
    return true;
}

static bool nv_too_small(struct nv_gpu *gpu, uint32_t w, uint32_t h)
{
    uint32_t min = gpu->min_pixels != 0u ? gpu->min_pixels
                                         : NV_ACCEL_DEFAULT_MIN_PIXELS;
    if ((uint64_t)w * h < min) {
        ++gpu->stat_small_skips;
        return true;
    }
    return false;
}

bool nv_2d_fill_rect(struct nv_gpu *gpu, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t xrgb)
{
    if (!nv_accel_ready(gpu) || !nv_clip(gpu, &x, &y, &width, &height)
        || nv_too_small(gpu, width, height)) {
        return false;
    }
    nv_flush_fb_rect(gpu, x, y, width, height);
    bool ok = nv_ce_fill_va(gpu, gpu->fb_gpu_addr, gpu->scanout.pitch, x, y,
        width, height, xrgb);
    nv_flush_fb_rect(gpu, x, y, width, height);
    if (ok) {
        ++gpu->stat_gpu_fills;
    }
    return ok;
}

bool nv_2d_copy_rect(struct nv_gpu *gpu, uint32_t src_x, uint32_t src_y,
    uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height)
{
    if (!nv_accel_ready(gpu) || !nv_clip(gpu, &dst_x, &dst_y, &width, &height)
        || src_x >= gpu->scanout.width || src_y >= gpu->scanout.height) {
        return false;
    }
    if (width > gpu->scanout.width - src_x) {
        width = gpu->scanout.width - src_x;
    }
    if (height > gpu->scanout.height - src_y) {
        height = gpu->scanout.height - src_y;
    }
    if (nv_too_small(gpu, width, height)) {
        return false;
    }
    nv_flush_fb_rect(gpu, src_x, src_y, width, height);
    nv_flush_fb_rect(gpu, dst_x, dst_y, width, height);
    bool ok = nv_ce_copy_va(gpu, gpu->fb_gpu_addr, gpu->scanout.pitch, src_x,
        src_y, dst_x, dst_y, width, height);
    nv_flush_fb_rect(gpu, dst_x, dst_y, width, height);
    if (ok) {
        ++gpu->stat_gpu_copies;
    }
    return ok;
}

#ifdef BOOK8_NV_DRIVER_MODULE
#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"
#include "../src/syscall.h"

extern uint32_t b8_hal_pci_read32(uint8_t bus, uint8_t device,
    uint8_t function, uint8_t offset);
extern void b8_hal_pci_write32(uint8_t bus, uint8_t device,
    uint8_t function, uint8_t offset, uint32_t value);
extern volatile uint8_t *b8_hal_map_mmio(uint64_t physical, uint64_t size,
    uint32_t uncached);
extern void b8_hal_unmap_mmio(const volatile void *pointer, uint64_t size);
extern void b8_hal_relax(void);
extern void b8_hal_log(const char *text);
extern int32_t b8_hal_framebuffer_info(struct b8_framebuffer_info *info);

struct nv_driver_import_table {
    b8_sym_table_header_t header;
    b8_sym_import_t imports[7];
};

__attribute__((used, section(B8_SHLIB_IMPORT_SECTION)))
static const struct nv_driver_import_table nv_driver_imports = {
    .header = {
        .magic = {'B', '8', 'S', '1'},
        .abi_version = B8_SHLIB_ABI_VERSION,
        .count = 7u,
    },
    .imports = {
        {"b8_hal_pci_read32", 1u, 0u},
        {"b8_hal_pci_write32", 1u, 0u},
        {"b8_hal_map_mmio", 1u, 0u},
        {"b8_hal_unmap_mmio", 1u, 0u},
        {"b8_hal_relax", 1u, 0u},
        {"b8_hal_log", 1u, 0u},
        {"b8_hal_framebuffer_info", 1u, 0u},
    },
};

/* 16384 words: the last 1024 become the GPU-visible fence page (see
 * nv_pushbuf_init), leaving 15360 words of pushbuffer. */
#define NV_MODULE_PUSHBUF_WORDS 16384u
#define NV_MODULE_GPFIFO_ENTRIES 512u
static uint32_t nv_module_pushbuf[NV_MODULE_PUSHBUF_WORDS]
    __attribute__((aligned(4096)));
static uint64_t nv_module_gpfifo[NV_MODULE_GPFIFO_ENTRIES]
    __attribute__((aligned(4096)));
static uint32_t nv_module_seg_offset[NV_MODULE_GPFIFO_ENTRIES];
static uint32_t nv_module_userd[1024] __attribute__((aligned(4096)));
static struct nv_platform nv_module_platform;

static uint32_t nv_module_pci_read32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset)
{
    (void)ctx;
    return b8_hal_pci_read32(bus, slot, function, offset);
}

static void nv_module_pci_write32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint32_t value)
{
    (void)ctx;
    b8_hal_pci_write32(bus, slot, function, offset, value);
}

static volatile uint8_t *nv_module_map_mmio(void *ctx, uint64_t physical,
    uint64_t size, bool uncached)
{
    (void)ctx;
    return b8_hal_map_mmio(physical, size, uncached ? 1u : 0u);
}

static void nv_module_unmap_mmio(void *ctx, volatile uint8_t *ptr,
    uint64_t size)
{
    (void)ctx;
    if (ptr != NULL) {
        b8_hal_unmap_mmio(ptr, size);
    }
}

static void nv_module_relax(void *ctx)
{
    (void)ctx;
    b8_hal_relax();
}

/* The HAL logs plain text, so format "label = 0x%08x" here. */
static void nv_module_log(void *ctx, const char *label, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    char line[112];
    uint32_t n = 0u;
    (void)ctx;
    for (const char *p = "NV: "; *p != '\0'; ++p)
        line[n++] = *p;
    for (; *label != '\0' && n < 80u; ++label)
        line[n++] = *label;
    line[n++] = ' ';
    line[n++] = '=';
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        line[n++] = hex[(value >> (uint32_t)shift) & 0xfu];
    line[n] = '\0';
    b8_hal_log(line);
}

static int32_t nv_module_init(void)
{
    nv_module_platform = (struct nv_platform){
        .pci_read32 = nv_module_pci_read32,
        .pci_write32 = nv_module_pci_write32,
        .map_mmio = nv_module_map_mmio,
        .unmap_mmio = nv_module_unmap_mmio,
        .relax = nv_module_relax,
        .log = nv_module_log,
    };
    b8_hal_log("NVDRV: trusted NVIDIA Maxwell driver initialized");
    return 0;
}

static int32_t nv_module_probe(const struct b8_driver_device *device,
    void *state)
{
    struct nv_gpu *gpu = state;
    if (device == NULL || gpu == NULL || device->vendor_id != NV_VENDOR_ID
        || !nv_device_supported(device->device_id)
        || (device->class_code >> 16u) != 0x03u)
        return -BOOK8_ENODEV;
    if (!nv_probe_at(&nv_module_platform, gpu, device->bus, device->device,
            device->function))
        return -BOOK8_ENODEV;
    if ((device->bar0_base != 0u && gpu->bar0_phys != device->bar0_base)
        || (device->bar1_base != 0u && gpu->bar1_phys != device->bar1_base)) {
        *gpu = (struct nv_gpu){0};
        return -BOOK8_EINVAL;
    }
    if (!nv_map_bars(&nv_module_platform, gpu)
        || !nv_pushbuf_init(gpu, nv_module_pushbuf, NV_MODULE_PUSHBUF_WORDS,
            nv_module_gpfifo, nv_module_seg_offset,
            NV_MODULE_GPFIFO_ENTRIES, nv_module_userd)) {
        nv_unmap_bars(&nv_module_platform, gpu);
        *gpu = (struct nv_gpu){0};
        return -BOOK8_ENODEV;
    }
    struct b8_framebuffer_info framebuffer = {0};
    if (b8_hal_framebuffer_info(&framebuffer) == 0
        && framebuffer.red_position == 16u
        && framebuffer.green_position == 8u
        && framebuffer.blue_position == 0u) {
        (void)nv_adopt_scanout(gpu, framebuffer.physical_base,
            framebuffer.width, framebuffer.height, framebuffer.pitch,
            framebuffer.bits_per_pixel);
    }
#if BOOK8_NV_BIND_CHANNEL
    if (gpu->scanout.valid) {
        uint64_t fb_end = gpu->scanout.vram_offset
            + (uint64_t)gpu->scanout.pitch * gpu->scanout.height;
        uint64_t vram_base = ((fb_end + 0x1fffffu) & ~0x1fffffull)
            + 0x200000u;
        (void)nv_channel_bind(gpu, vram_base);
    }
#endif
    b8_hal_log("NVDRV: supported GPU probed by trusted module");
    b8_hal_log(nv_accel_state(gpu));
    return 0;
}

static void nv_module_remove(const struct b8_driver_device *device,
    void *state)
{
    struct nv_gpu *gpu = state;
    if (device != NULL && gpu != NULL && device->bus == gpu->bus
        && device->device == gpu->slot
        && device->function == gpu->function) {
        nv_channel_unbind(gpu);
        nv_unmap_bars(&nv_module_platform, gpu);
        *gpu = (struct nv_gpu){0};
    }
}

static void nv_module_refresh(void *state)
{
    nv_refresh_telemetry(state);
}

/* Both hooks return 0 only when the GPU performed the operation; a nonzero
 * result tells the kernel to draw with the CPU instead. */
static int32_t nv_module_fill_rect(void *state, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t xrgb)
{
    return nv_2d_fill_rect(state, x, y, width, height, xrgb)
        ? 0 : -BOOK8_ENODEV;
}

static int32_t nv_module_copy_rect(void *state, uint32_t src_x,
    uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width,
    uint32_t height)
{
    return nv_2d_copy_rect(state, src_x, src_y, dst_x, dst_y, width, height)
        ? 0 : -BOOK8_ENODEV;
}

static const struct b8_graphics_ops nv_module_graphics = {
    .abi_version = 1u,
    .fill_rect = nv_module_fill_rect,
    .copy_rect = nv_module_copy_rect,
};

static const struct b8_graphics_ops *nv_module_get_graphics(void)
{
    return &nv_module_graphics;
}

static const b8_driver_t nv_module_descriptor = {
    .abi_version = B8_DRIVER_ABI_VERSION,
    .struct_size = sizeof(b8_driver_t),
    .version_major = 1u,
    .version_minor = 1u,
    .version_patch = 0u,
    .vendor_id = NV_VENDOR_ID,
    .product_id = 0u,
    .pci_class = 0x03u,
    .init = nv_module_init,
    .probe = nv_module_probe,
    .remove = nv_module_remove,
    .refresh = nv_module_refresh,
    .graphics = nv_module_get_graphics,
};

__attribute__((visibility("default")))
const b8_driver_t *b8_driver_entry(void)
{
    return &nv_module_descriptor;
}
#endif
