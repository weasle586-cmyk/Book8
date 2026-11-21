#ifndef BOOK8_PCIE_H
#define BOOK8_PCIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus.h"

#define BOOK8_PCIE_TARGET_ID 0x13c210deu
#define BOOK8_PCIE_MAX_ECAM 16u
#define BOOK8_PCIE_MAX_NODES 512u
#define BOOK8_PCIE_BAR_COUNT 6u
#define BOOK8_PCIE_ANY_ID 0xffffu

struct book8_pcie_state;
struct book8_pcie_node;
struct book8_pcie_driver;

enum book8_pcie_cache_policy {
    BOOK8_PCIE_CACHE_UNKNOWN,
    BOOK8_PCIE_CACHE_UC,
    BOOK8_PCIE_CACHE_WC,
    BOOK8_PCIE_CACHE_WB
};

struct book8_pcie_match {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t class_code;
    uint32_t class_mask;
};

struct book8_pcie_driver {
    const char *name;
    const struct book8_pcie_match *matches;
    size_t match_count;
    /* Driver-declared BAR0 window; zero means no automatic mapping. BAR
     * aperture sizing is intentionally not done by destructive config writes. */
    uint32_t bar0_window_size;
    bool (*probe)(struct book8_pcie_state *, struct book8_pcie_node *,
        volatile uint8_t *bar0);
};

struct book8_pcie_bar {
    uint64_t base;
    uint64_t size;
    uint64_t mapped_size;
    bool valid;
    bool io;
    bool is_64;
    bool prefetchable;
    bool mapped;
    enum book8_pcie_cache_policy cache_policy;
};

/* Return zero or the recovered CPU exception vector. These callbacks do not
 * guarantee PCIe transaction completion or recover a stalled bus. */
typedef uint32_t (*book8_pcie_guarded_read32)(volatile uint32_t *, uint32_t *);
typedef uint32_t (*book8_pcie_guarded_write32)(volatile uint32_t *, uint32_t);

struct book8_pcie_platform {
    uint32_t (*legacy_read)(uint8_t, uint8_t, uint8_t, uint8_t);
    void (*legacy_write)(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
    void (*legacy_write16)(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
    /* Maps the physical range with UC semantics for ECAM and device BARs. */
    volatile uint8_t *(*map)(uint64_t, uint64_t);
    void (*log)(const char *);
    bool (*delay_ms)(uint32_t);
    book8_pcie_guarded_read32 read32_safe;
    book8_pcie_guarded_write32 write32_safe;
    uint64_t (*irq_save)(void);
    void (*irq_restore)(uint64_t);
};

struct book8_pcie_ecam {
    uint64_t base;
    uint16_t segment;
    uint8_t first, last;
    volatile uint8_t *mapped;
};

struct book8_pcie_node {
    uint32_t id, class_revision;
    uint16_t segment;
    uint8_t bus, slot, function, header;
    uint8_t primary, secondary, subordinate;
    uint8_t irq_line, irq_pin, bar_count;
    struct book8_pcie_bar bars[BOOK8_PCIE_BAR_COUNT];
    struct book8_bus_device bus_device;
    const struct book8_pcie_driver *driver;
};

struct book8_pcie_state {
    struct book8_pcie_platform platform;
    struct book8_pcie_ecam ecam[BOOK8_PCIE_MAX_ECAM];
    struct book8_pcie_node nodes[BOOK8_PCIE_MAX_NODES];
    size_t ecam_count, node_count, driver_count, registered_driver_count;
    bool topology_valid, target_present, target_ready;
    const char *target_status;
    bool amd_host_verified, target_link_verified;
    /* Shared BSP indexed-access ownership/restoration interlock. */
    bool rs780_access_blocked;
};

void book8_pcie_init(struct book8_pcie_state *,
    const struct book8_pcie_platform *);
bool book8_pcie_add_mcfg(struct book8_pcie_state *, const uint8_t *, size_t);
volatile uint8_t *book8_pcie_ecam_address(struct book8_pcie_state *, uint16_t,
    uint8_t, uint8_t, uint8_t, uint16_t);
uint32_t book8_pcie_read32(struct book8_pcie_state *, uint16_t,
    uint8_t, uint8_t, uint8_t, uint16_t);
void book8_pcie_write32(struct book8_pcie_state *, uint16_t,
    uint8_t, uint8_t, uint8_t, uint16_t, uint32_t);
void book8_pcie_write16(struct book8_pcie_state *, uint16_t,
    uint8_t, uint8_t, uint8_t, uint16_t, uint16_t);
void book8_pcie_enumerate(struct book8_pcie_state *);
size_t book8_pcie_bind_drivers(struct book8_pcie_state *,
    const struct book8_pcie_driver *, size_t);
size_t book8_pcie_bind_bus_drivers(struct book8_pcie_state *,
    const struct book8_bus_driver *, size_t);
size_t book8_pcie_rescan(struct book8_pcie_state *,
    const struct book8_bus_driver *, size_t);
bool book8_pcie_registry_sync(const struct book8_pcie_state *);
/* Maps a driver-declared BAR window through the platform's UC mapping hook. */
volatile uint8_t *book8_pcie_map_bar(struct book8_pcie_state *,
    const struct book8_pcie_node *, uint8_t, uint64_t);
bool book8_pcie_prepare_target(struct book8_pcie_state *);
bool book8_pcie_enable_target_command(struct book8_pcie_state *);

/* Segment-zero adapters shared by the kernel's legacy driver interfaces. */
uint32_t book8_pci_read32(uint8_t, uint8_t, uint8_t, uint8_t);
void book8_pci_write32(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
void book8_pci_write16(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);

#endif
