// ... existing includes ...

#define NV_PMC_ENABLE          0x00000200
#define NV_PMU_FALCON_BASE     0x0010A000
#define FALCON_OS              0x00000080
#define PMU_SCRATCH_LOCK       0x000005C0 // Relative to PMU Base

// ... existing code ...

void gm204_pmu_unlock(uint8_t* bar0, void* hs_blob, uint32_t blob_size) {
    uint32_t val = mmio_read32(bar0 + NV_PMC_ENABLE);

    // Toggle PMU reset (Bit 13)
    mmio_write32(bar0 + NV_PMC_ENABLE, val & ~(1 << 13));
    (void)mmio_read32(bar0 + NV_PMC_ENABLE); // Bus Flush
    mmio_write32(bar0 + NV_PMC_ENABLE, val | (1 << 13));
    (void)mmio_read32(bar0 + NV_PMC_ENABLE); // Bus Flush

    // Load HS Blob to IMEM Tag 0
    falcon_load_imem(bar0, NV_PMU_FALCON_BASE, hs_blob, blob_size, 0);

    // Memory and Bus Barriers
    __builtin_ia32_sfence();
    (void)mmio_read32(bar0 + NV_PMU_FALCON_BASE + 0x180); // Read IMEMC to flush
    asm volatile("" ::: "memory");

    // Execute from Boot Vector 0
    mmio_write32(bar0 + NV_PMU_FALCON_BASE + 0x104, 0); // FALCON_BOOTVEC
    mmio_write32(bar0 + NV_PMU_FALCON_BASE + 0x100, 2); // FALCON_CPUCTL (Start)
}

int nv_gm204_init(gpu_device_t* dev) {
    // ... setup BARs and load firmware blobs ...

    gm204_pmu_unlock(dev->bar0, dev->hs_pmu_blob, dev->hs_pmu_size);

    // Status Polling Loop
    uint32_t status = 0;
    uint64_t timeout = 1000000; // Adjust based on kernel clock
    
    while (timeout--) {
        status = mmio_read32(dev->bar0 + NV_PMU_FALCON_BASE + FALCON_OS);
        uint32_t lock_check = mmio_read32(dev->bar0 + NV_PMU_FALCON_BASE + PMU_SCRATCH_LOCK);

        // If the lock 0xC0012121 is cleared and OS status is 0 (Success)
        if (status == 0 && lock_check != 0xC0012121) {
            kprintf("GM204: Falcon unlocked and operational.\n");
            return 0; 
        }
        cpu_relax();
    }

    kprintf("GM204: Timeout waiting for Falcon unlock. Status: 0x%x\n", status);
    return -1;
}

// ... rest of code ...