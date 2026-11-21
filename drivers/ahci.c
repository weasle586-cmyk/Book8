#include "ahci.h"

/* Bare-metal AHCI bring-up: HBA reset, AE, port discovery, command list / FIS
 * / command table setup. No interrupts or command issue yet. */

#define AHCI_POOL_PORTS 16u
#define AHCI_RESET_SPINS 50000000u  /* spec allows 1 s for HR to clear */
#define AHCI_STOP_SPINS  20000000u  /* spec allows 500 ms for CR/FR */
#define AHCI_LINK_SPINS  2000000u
#define AHCI_READY_SPINS 20000000u
#define AHCI_CMD_SPINS   4000000u

/* Identity-mapped kernel image (below 4 GiB), 1 KiB aligned per port. */
static struct ahci_port_mem ahci_pool[AHCI_POOL_PORTS];

static inline void ahci_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline void ahci_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static bool ahci_wait_clear(volatile uint32_t *reg, uint32_t mask,
    uint32_t spins)
{
    while (spins--) {
        if ((*reg & mask) == 0u) {
            return true;
        }
        ahci_relax();
    }
    return (*reg & mask) == 0u;
}

static void ahci_zero(void *ptr, size_t size)
{
    volatile uint8_t *bytes = ptr;

    while (size--) {
        *bytes++ = 0u;
    }
}

enum ahci_status ahci_port_stop(volatile struct ahci_port_regs *port)
{
    port->cmd &= ~AHCI_PCMD_ST;
    if (!ahci_wait_clear(&port->cmd, AHCI_PCMD_CR, AHCI_STOP_SPINS)) {
        return AHCI_ERR_STOP;
    }
    port->cmd &= ~AHCI_PCMD_FRE;
    if (!ahci_wait_clear(&port->cmd, AHCI_PCMD_FR, AHCI_STOP_SPINS)) {
        return AHCI_ERR_STOP;
    }
    return AHCI_OK;
}

void ahci_port_start(volatile struct ahci_port_regs *port)
{
    (void)ahci_wait_clear(&port->cmd, AHCI_PCMD_CR, AHCI_STOP_SPINS);
    port->cmd |= AHCI_PCMD_FRE;
    port->cmd |= AHCI_PCMD_ST;
}

enum ahci_status ahci_port_setup(struct ahci_port *port)
{
    struct ahci_port_mem *mem = port->mem;
    volatile struct ahci_port_regs *regs = port->regs;
    uint64_t cl = (uint64_t)(uintptr_t)mem->cl;
    uint64_t fis = (uint64_t)(uintptr_t)&mem->rfis;
    uint32_t slot;

    if ((cl | fis | (uint64_t)(uintptr_t)&mem->ct[AHCI_CMD_SLOTS - 1u]) >> 32
        && !port->s64a) {
        return AHCI_ERR_ADDRESS;
    }

    ahci_zero(mem, sizeof(*mem));
    for (slot = 0; slot < AHCI_CMD_SLOTS; slot++) {
        uint64_t ct = (uint64_t)(uintptr_t)&mem->ct[slot];

        mem->cl[slot].ctba = (uint32_t)ct;
        mem->cl[slot].ctbau = (uint32_t)(ct >> 32);
        mem->cl[slot].prdtl = 0u;
    }

    regs->clb = (uint32_t)cl;
    regs->clbu = (uint32_t)(cl >> 32);
    regs->fb = (uint32_t)fis;
    regs->fbu = (uint32_t)(fis >> 32);

    regs->serr = 0xffffffffu;   /* RWC: clear stale errors */
    regs->is = 0xffffffffu;
    regs->ie = 0u;              /* polled operation for now */
    ahci_barrier();
    return AHCI_OK;
}

bool ahci_port_probe(struct ahci_port *port)
{
    volatile struct ahci_port_regs *regs = port->regs;
    uint32_t spins;

    port->present = false;
    for (spins = AHCI_LINK_SPINS; spins; spins--) {
        if (AHCI_SSTS_DET(regs->ssts) == AHCI_DET_PRESENT) {
            break;
        }
        ahci_relax();
    }
    if (AHCI_SSTS_DET(regs->ssts) != AHCI_DET_PRESENT
        || AHCI_SSTS_IPM(regs->ssts) != AHCI_IPM_ACTIVE) {
        return false;
    }

    ahci_port_start(regs);
    if (!ahci_wait_clear(&regs->tfd, AHCI_TFD_BSY | AHCI_TFD_DRQ,
            AHCI_READY_SPINS)) {
        return false;
    }
    port->signature = regs->sig;
    port->present = true;
    return true;
}

bool ahci_port_issue(struct ahci_port *port, uint8_t command, uint64_t lba,
    uint16_t count, void *data, uint32_t bytes, bool write)
{
    volatile struct ahci_port_regs *regs = port->regs;
    struct ahci_port_mem *mem = port->mem;
    struct ahci_cmd_table *table = &mem->ct[0];
    struct ahci_fis_h2d *fis = (struct ahci_fis_h2d *)table->cfis;
    uint64_t address = (uint64_t)(uintptr_t)data;
    uint32_t spins;

    if (bytes > (4u << 20) || (bytes && data == NULL)
        || (bytes && (address >> 32) && !port->s64a)) {
        return false;
    }
    if (!(regs->cmd & AHCI_PCMD_ST)) {
        if (ahci_port_stop(regs) != AHCI_OK
            || ahci_port_setup(port) != AHCI_OK) {
            return false;
        }
        ahci_port_start(regs);
    }
    if ((regs->ci | regs->sact) & 1u) {
        return false;
    }

    ahci_zero(table, sizeof(*table));
    mem->cl[0].flags = (uint16_t)(AHCI_CMD_FIS_DWORDS
        | (write ? AHCI_CMD_FLAG_WRITE : 0u));
    mem->cl[0].prdtl = bytes ? 1u : 0u;
    mem->cl[0].prdbc = 0u;
    if (bytes) {
        table->prdt[0].dba = (uint32_t)address;
        table->prdt[0].dbau = (uint32_t)(address >> 32);
        table->prdt[0].dbc = bytes - 1u;
    }
    fis->type = AHCI_FIS_TYPE_H2D;
    fis->flags = 1u << 7;
    fis->command = command;
    fis->device = 1u << 6;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->count_low = (uint8_t)count;
    fis->count_high = (uint8_t)(count >> 8);

    regs->is = 0xffffffffu;
    regs->serr = 0xffffffffu;
    ahci_barrier();
    regs->ci = 1u;
    for (spins = AHCI_CMD_SPINS; spins; spins--) {
        if (!(regs->ci & 1u)) {
            return !(regs->is & AHCI_PIS_TFES)
                && !(regs->tfd & AHCI_TFD_ERR);
        }
        ahci_relax();
    }
    return false;
}

/* Claims the HBA from firmware when BOHC is implemented (CAP2.BOH). */
static void ahci_bios_handoff(volatile struct ahci_hba_regs *regs)
{
    uint32_t spins;

    if (!(regs->cap2 & AHCI_CAP2_BOH) || !(regs->bohc & AHCI_BOHC_BOS)) {
        return;
    }
    regs->bohc |= AHCI_BOHC_OOS;
    for (spins = AHCI_STOP_SPINS; spins; spins--) {
        if (!(regs->bohc & (AHCI_BOHC_BOS | AHCI_BOHC_BB))) {
            return;
        }
        ahci_relax();
    }
}

enum ahci_status ahci_init(struct ahci_hba *hba, void *abar)
{
    volatile struct ahci_hba_regs *regs = abar;
    uint32_t i;
    uint32_t pool_used = 0u;

    if (hba == NULL || abar == NULL) {
        return AHCI_ERR_ARG;
    }
    ahci_zero(hba, sizeof(*hba));
    hba->regs = regs;

    /* Enable AHCI first so HR/registers behave per spec, then reset. */
    regs->ghc |= AHCI_GHC_AE;
    ahci_bios_handoff(regs);
    regs->ghc |= AHCI_GHC_HR;
    if (!ahci_wait_clear(&regs->ghc, AHCI_GHC_HR, AHCI_RESET_SPINS)) {
        return AHCI_ERR_RESET;
    }

    /* HR resets GHC.AE and every port register: re-enable AHCI mode. */
    regs->ghc |= AHCI_GHC_AE;
    regs->ghc &= ~AHCI_GHC_IE;
    regs->is = 0xffffffffu;

    hba->cap = regs->cap;
    hba->version = regs->vs;
    hba->ports_implemented = regs->pi;
    hba->slots = (uint8_t)(AHCI_CAP_NCS(hba->cap) + 1u);
    if (hba->ports_implemented == 0u) {
        return AHCI_ERR_NO_PORTS;
    }

    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        struct ahci_port *port;
        volatile struct ahci_port_regs *pregs;
        enum ahci_status status;

        if (!(hba->ports_implemented & (1u << i))) {
            continue;
        }
        if (pool_used == AHCI_POOL_PORTS) {
            break;
        }

        pregs = &regs->ports[i];
        status = ahci_port_stop(pregs);
        if (status != AHCI_OK) {
            continue;   /* wedged port: skip, keep the rest usable */
        }

        port = &hba->ports[hba->port_count];
        port->regs = pregs;
        port->mem = &ahci_pool[pool_used];
        port->index = (uint8_t)i;
        port->s64a = (hba->cap & AHCI_CAP_S64A) != 0u;
        if (ahci_port_setup(port) != AHCI_OK) {
            continue;
        }
        pool_used++;
        hba->port_count++;
        port->present = false;
        port->signature = 0u;
    }

    return hba->port_count ? AHCI_OK : AHCI_ERR_STOP;
}
