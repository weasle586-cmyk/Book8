#include "hwdiag.h"
#include "../include/syscall.h"
#include <stddef.h>

static const struct nv_gpu *device;

void hwdiag_init(const struct nv_gpu *gpu)
{
    device = gpu;
}

long hwdiag_info(struct book8_hwdiag_info *info)
{
    *info = (struct book8_hwdiag_info){0};
    if (device == NULL || !device->present)
        return 0;
    info->flags = BOOK8_HWDIAG_PRESENT
        | (device->mapped ? BOOK8_HWDIAG_MAPPED : 0u);
    info->bar0_phys = device->bar0_phys;
    info->bar0_size = device->bar0_size;
    info->bar1_phys = device->bar1_phys;
    info->bar1_size = device->bar1_size;
    info->pci_id = ((uint32_t)device->device_id << 16u) | NV_VENDOR_ID;
    info->bdf = ((uint32_t)device->bus << 8u)
        | ((uint32_t)device->slot << 3u) | device->function;
    info->boot0 = device->boot0;
    return 0;
}

static uint32_t config_read(uint8_t offset)
{
    return device->plat.pci_read32(device->plat.ctx, device->bus,
        device->slot, device->function, offset);
}

/* Read-only walk of the conventional PCI capability chain. Returns the
 * dword-aligned config offset of the requested capability, or 0 if absent or
 * the chain is malformed. */
static uint8_t pci_capability(uint8_t cap_id)
{
    uint32_t status = config_read(4u);
    if (status == UINT32_MAX || (status & (1u << 20u)) == 0u) {
        return 0u;
    }
    if (((config_read(0x0cu) >> 16u) & 0x7fu) != 0u) {
        return 0u;
    }
    uint8_t offset = (uint8_t)(config_read(0x34u) & 0xffu);
    uint64_t visited = 0u;
    while (offset != 0u) {
        if (offset < 0x40u || offset > 0xf8u || (offset & 3u) != 0u) {
            return 0u;
        }
        uint64_t bit = 1ull << (offset >> 2u);
        if ((visited & bit) != 0u) {
            return 0u;
        }
        visited |= bit;
        uint32_t cap = config_read(offset);
        if ((cap & 0xffu) == cap_id) {
            return offset;
        }
        offset = (uint8_t)((cap >> 8u) & 0xffu);
    }
    return 0u;
}

static long register_accessible(void);

static bool snapshot_register(struct book8_nvdiag_snapshot *snapshot,
    uint32_t offset, uint32_t flag, uint32_t *destination)
{
    if (device->bar0_size < sizeof(uint32_t)
        || offset > device->bar0_size - sizeof(uint32_t)) {
        return false;
    }
    uint32_t value = *(volatile const uint32_t *)(device->bar0 + offset);
    if (value == UINT32_MAX) {
        return false;
    }
    *destination = value;
    snapshot->register_flags |= flag;
    return true;
}

long hwdiag_snapshot(struct book8_nvdiag_snapshot *snapshot)
{
    *snapshot = (struct book8_nvdiag_snapshot){0};
    if (device == NULL || !device->present) {
        return 0;
    }
    snapshot->flags = BOOK8_NVDIAG_PRESENT
        | (device->mapped ? BOOK8_NVDIAG_MAPPED : 0u)
        | (device->channel_bound ? BOOK8_NVDIAG_CHANNEL_BOUND : 0u)
        | (device->ring_acked ? BOOK8_NVDIAG_RING_ACKED : 0u)
        | (device->runlist_committed
            ? BOOK8_NVDIAG_RUNLIST_COMMITTED : 0u)
        | (device->scanout.valid ? BOOK8_NVDIAG_SCANOUT_VALID : 0u)
        | (device->ce_found ? BOOK8_NVDIAG_CE_FOUND : 0u)
        | (device->ce_verified ? BOOK8_NVDIAG_CE_VERIFIED : 0u)
        | (nv_accel_active(device) ? BOOK8_NVDIAG_ACCEL_ACTIVE : 0u)
        | (device->accel_failed ? BOOK8_NVDIAG_ACCEL_FAILED : 0u);
    snapshot->bar0_phys = device->bar0_phys;
    snapshot->bar0_size = device->bar0_size;
    snapshot->bar1_phys = device->bar1_phys;
    snapshot->bar1_size = device->bar1_size;
    snapshot->vram_base = device->vram_base;
    snapshot->scanout_vram_offset = device->scanout.vram_offset;
    snapshot->boot0 = device->boot0;
    snapshot->chipset = device->chipset;
    snapshot->scanout_width = device->scanout.width;
    snapshot->scanout_height = device->scanout.height;
    snapshot->scanout_pitch = device->scanout.pitch;
    snapshot->chip_revision = device->chip_rev;
    snapshot->foundry_id = device->foundry_id;
    snapshot->detected_vram_mb = device->detected_vram_mb;
    snapshot->pfb_config = device->pfb_config;
    snapshot->pdisp_head_mask = device->pdisp_head_mask;
    snapshot->pdisp_active_head = device->pdisp_active_head;
    snapshot->pdisp_htotal = device->pdisp_htotal;
    snapshot->pdisp_vtotal = device->pdisp_vtotal;
    snapshot->pushbuf_put = device->put;
    snapshot->pushbuf_words = device->pushbuf_words;
    snapshot->pushbuf_used = device->put >= device->seg_start
        ? device->put - device->seg_start : 0u;
    snapshot->gpfifo_entries = device->gpfifo_entries;
    snapshot->pushbuf_overflows = device->pb_overflows;
    snapshot->accel_min_pixels = device->min_pixels != 0u
        ? device->min_pixels : NV_ACCEL_DEFAULT_MIN_PIXELS;
    snapshot->ce_engine = device->ce_engine;
    snapshot->ce_runlist = device->ce_runlist;
    snapshot->ce_count = device->ce_count;
    snapshot->ptop_devices = device->ptop_devices;
    snapshot->accel_fills = device->stat_gpu_fills;
    snapshot->accel_copies = device->stat_gpu_copies;
    snapshot->accel_small_skips = device->stat_small_skips;
    snapshot->accel_unsupported = device->stat_unsupported;
    snapshot->accel_fence_timeouts = device->stat_fence_timeouts;
    snapshot->gp_put = device->gp_put;
    if (device->userd != NULL && device->gpfifo_entries != 0u) {
        snapshot->gp_get = device->userd[NV_USERD_GP_GET / 4u]
            & (device->gpfifo_entries - 1u);
    }

    if (device->plat.pci_read32 == NULL) {
        return 0;
    }
    uint32_t pci_id = config_read(0u);
    if (pci_id == UINT32_MAX
        || pci_id != (((uint32_t)device->device_id << 16u) | NV_VENDOR_ID)) {
        return 0;
    }
    snapshot->flags |= BOOK8_NVDIAG_PCI_CONFIG;
    snapshot->pci_id = pci_id;
    snapshot->bdf = ((uint32_t)device->bus << 8u)
        | ((uint32_t)device->slot << 3u) | device->function;
    snapshot->rom_bar = config_read(0x30u);
    uint8_t pcie = pci_capability(0x10u);
    if (pcie != 0u) {
        snapshot->flags |= BOOK8_NVDIAG_PCI_EXPRESS;
        snapshot->pcie_link_cap = config_read((uint8_t)(pcie + 0x0cu));
        uint32_t link = config_read((uint8_t)(pcie + 0x10u));
        snapshot->pcie_link_status = (link >> 16u) & 0xffffu;
    }
    uint32_t command_status = config_read(4u);
    if (command_status != UINT32_MAX) {
        snapshot->pci_command = command_status & 0xffffu;
        snapshot->pci_status = command_status >> 16u;
    }
    snapshot->class_revision = config_read(8u);
    snapshot->subsystem_id = config_read(0x2cu);
    if (snapshot->class_revision == UINT32_MAX) {
        snapshot->class_revision = 0u;
    }
    if (snapshot->subsystem_id == UINT32_MAX) {
        snapshot->subsystem_id = 0u;
    }

    if (!device->mapped || device->bar0 == NULL
        || register_accessible() != 0) {
        return 0;
    }
    snapshot->flags |= BOOK8_NVDIAG_MMIO_READABLE;
    (void)snapshot_register(snapshot, NV_PMC_ENABLE,
        BOOK8_NVDIAG_REG_PMC_ENABLE, &snapshot->pmc_enable);
    (void)snapshot_register(snapshot, NV_BUS_BAR1_BIND,
        BOOK8_NVDIAG_REG_BAR1_BIND, &snapshot->bar1_bind);
    (void)snapshot_register(snapshot, NV_PFIFO_CHAN_INST(NV_CH_ID),
        BOOK8_NVDIAG_REG_CHAN_INST, &snapshot->channel_inst);
    (void)snapshot_register(snapshot, NV_PFIFO_CHAN_CTRL(NV_CH_ID),
        BOOK8_NVDIAG_REG_CHAN_CTRL, &snapshot->channel_ctrl);
    (void)snapshot_register(snapshot,
        NV_PFIFO_RUNL_PENDING(device->chan_runl & 15u),
        BOOK8_NVDIAG_REG_RUNLIST_PENDING, &snapshot->runlist_pending);
    (void)snapshot_register(snapshot, NV_PFIFO_INTR,
        BOOK8_NVDIAG_REG_PFIFO_INTR, &snapshot->pfifo_intr);
    (void)snapshot_register(snapshot, NV_PBDMA_INTR(0u),
        BOOK8_NVDIAG_REG_PBDMA_INTR, &snapshot->pbdma_intr);
    (void)snapshot_register(snapshot, NV_PFIFO_ENGN_STATUS(0u),
        BOOK8_NVDIAG_REG_ENGINE_STATUS, &snapshot->engine_status);
    return 0;
}

static long register_accessible(void)
{
    uint32_t id = config_read(0u);
    if (id != (((uint32_t)device->device_id << 16u) | NV_VENDOR_ID))
        return -BOOK8_ENODEV;
    uint32_t command_status = config_read(4u);
    if ((command_status & 2u) == 0u)
        return -BOOK8_EAGAIN;
    if ((command_status & (1u << 20u)) == 0u)
        return 0;
    /* Conventional PCI capabilities only; malformed chains fail closed. */
    uint32_t header = (config_read(0x0cu) >> 16u) & 0x7fu;
    if (header != 0u)
        return -BOOK8_EIO;
    uint32_t offset = config_read(0x34u) & 0xffu;
    if (offset == 0u)
        return -BOOK8_EIO;
    uint64_t visited = 0u;
    while (offset != 0u) {
        if (offset < 0x40u || offset > 0xf8u || (offset & 3u) != 0u)
            return -BOOK8_EIO;
        uint64_t bit = 1ull << (offset >> 2u);
        if ((visited & bit) != 0u)
            return -BOOK8_EIO;
        visited |= bit;
        uint32_t cap = config_read((uint8_t)offset);
        if ((cap & 0xffu) == 1u) {
            uint32_t pmcsr = config_read((uint8_t)(offset + 4u));
            if (pmcsr == UINT32_MAX)
                return -BOOK8_ENODEV;
            return (pmcsr & 3u) == 0u ? 0 : -BOOK8_EAGAIN;
        }
        offset = (cap >> 8u) & 0xffu;
    }
    return 0;
}

long hwdiag_read32(struct book8_hwdiag_read32 *request)
{
    uint32_t offset = request->offset;
    if (offset != NV_PMC_BOOT_0 && offset != NV_PMC_ENABLE)
        return -BOOK8_EPERM;
    if (device == NULL || !device->present || !device->mapped
        || device->bar0 == NULL || device->plat.pci_read32 == NULL)
        return -BOOK8_ENODEV;
    if (((uintptr_t)device->bar0 & 3u) != 0u
        || device->bar0_size < 4u || offset > device->bar0_size - 4u)
        return -BOOK8_ENODEV;
    long result = register_accessible();
    if (result != 0)
        return result;
    uint32_t value = *(volatile const uint32_t *)(device->bar0 + offset);
    if (value == UINT32_MAX)
        return -BOOK8_EIO;
    request->value = value;
    return 0;
}

long hwdiag_report(const struct book8_hwdiag_report *report)
{
    (void)report;
    return -BOOK8_ENOSYS;
}
