#ifndef BOOK8_HWDIAG_H
#define BOOK8_HWDIAG_H

#include <stdint.h>

#define BOOK8_HWDIAG_INFO 0u
#define BOOK8_HWDIAG_READ32 1u
#define BOOK8_HWDIAG_REPORT 2u
#define BOOK8_HWDIAG_PRESENT 1u
#define BOOK8_HWDIAG_MAPPED 2u

#define BOOK8_NVDIAG_PRESENT 0x0001u
#define BOOK8_NVDIAG_MAPPED 0x0002u
#define BOOK8_NVDIAG_PCI_CONFIG 0x0004u
#define BOOK8_NVDIAG_MMIO_READABLE 0x0008u
#define BOOK8_NVDIAG_CHANNEL_BOUND 0x0010u
#define BOOK8_NVDIAG_RING_ACKED 0x0020u
#define BOOK8_NVDIAG_RUNLIST_COMMITTED 0x0040u
#define BOOK8_NVDIAG_SCANOUT_VALID 0x0080u
#define BOOK8_NVDIAG_CE_FOUND 0x0100u
#define BOOK8_NVDIAG_CE_VERIFIED 0x0200u
#define BOOK8_NVDIAG_ACCEL_ACTIVE 0x0400u
#define BOOK8_NVDIAG_ACCEL_FAILED 0x0800u

#define BOOK8_NVDIAG_REG_PMC_ENABLE 0x0001u
#define BOOK8_NVDIAG_REG_BAR1_BIND 0x0002u
#define BOOK8_NVDIAG_REG_CHAN_INST 0x0004u
#define BOOK8_NVDIAG_REG_CHAN_CTRL 0x0008u
#define BOOK8_NVDIAG_REG_RUNLIST_PENDING 0x0010u
#define BOOK8_NVDIAG_REG_PFIFO_INTR 0x0020u
#define BOOK8_NVDIAG_REG_PBDMA_INTR 0x0040u
#define BOOK8_NVDIAG_REG_ENGINE_STATUS 0x0080u

struct book8_hwdiag_info {
    uint64_t bar0_phys, bar0_size;
    uint64_t bar1_phys, bar1_size;
    uint32_t flags;
    uint32_t pci_id;
    uint32_t bdf;
    uint32_t boot0;
};

struct book8_nvdiag_snapshot {
    uint64_t bar0_phys, bar0_size;
    uint64_t bar1_phys, bar1_size;
    uint64_t vram_base;
    uint64_t scanout_vram_offset;
    uint32_t flags;
    uint32_t register_flags;
    uint32_t pci_id;
    uint32_t bdf;
    uint32_t pci_command;
    uint32_t pci_status;
    uint32_t class_revision;
    uint32_t subsystem_id;
    uint32_t boot0;
    uint32_t chipset;
    uint32_t pmc_enable;
    uint32_t bar1_bind;
    uint32_t channel_inst;
    uint32_t channel_ctrl;
    uint32_t runlist_pending;
    uint32_t pfifo_intr;
    uint32_t pbdma_intr;
    uint32_t engine_status;
    uint32_t gp_get;
    uint32_t gp_put;
    uint32_t pushbuf_put;
    uint32_t pushbuf_words;
    uint32_t pushbuf_used;
    uint32_t gpfifo_entries;
    uint32_t pushbuf_overflows;
    uint32_t accel_min_pixels;
    uint8_t ce_engine;
    uint8_t ce_runlist;
    uint8_t ce_count;
    uint8_t ptop_devices;
    uint64_t accel_fills;
    uint64_t accel_copies;
    uint64_t accel_small_skips;
    uint64_t accel_unsupported;
    uint64_t accel_fence_timeouts;
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t scanout_pitch;
};

struct book8_hwdiag_read32 {
    uint32_t offset;
    uint32_t value;
};

/* Userspace observation, not an authenticated kernel measurement. */
struct book8_hwdiag_report {
    uint32_t pmc_enable;
    uint32_t read_errno;
};

#endif
