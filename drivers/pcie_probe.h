#ifndef BOOK8_PCIE_PROBE_H
#define BOOK8_PCIE_PROBE_H

#include "pcie.h"
#include "rs780.h"

enum book8_probe_phase { BOOK8_PROBE_IDLE, BOOK8_PROBE_INTENT, BOOK8_PROBE_RESULT };
enum book8_probe_target {
    BOOK8_PROBE_INDEX, BOOK8_PROBE_LINK_CAP, BOOK8_PROBE_LINK_STATUS
};

struct book8_probe_ops {
    bool (*emit)(const char *);
    uint32_t (*read)(volatile uint32_t *, uint32_t *);
    uint32_t (*write)(volatile uint32_t *, uint32_t);
};

struct book8_pcie_probe {
    struct book8_pcie_state *pcie;
    struct book8_probe_ops ops;
    enum book8_probe_phase phase;
    enum book8_probe_target target;
    uint32_t id, index, mask, value, exception, error, restore_exception, restore_error;
    uint16_t offset;
    uint32_t link_cap, link_status;
    bool ltssm_acknowledged, link_cap_acknowledged, link_status_acknowledged;
    bool sent, value_valid, poisoned;
    const char *step;
    char record[128];
};

void book8_probe_init(struct book8_pcie_probe *, struct book8_pcie_state *,
    const struct book8_probe_ops *);
bool book8_probe_begin(struct book8_pcie_probe *, uint32_t, uint32_t);
bool book8_probe_begin_link(struct book8_pcie_probe *, bool);
bool book8_probe_ack(struct book8_pcie_probe *, uint32_t, enum book8_probe_phase);
bool book8_probe_retry(struct book8_pcie_probe *);
bool book8_probe_link_verified(const struct book8_pcie_probe *);

/* Return 0, #GP (13), or #PF (14); read output is unchanged on a fault.
 * These wrappers do not recover bus hangs or machine checks. */
uint32_t book8_read32_safe(volatile uint32_t *, uint32_t *);
uint32_t book8_write32_safe(volatile uint32_t *, uint32_t);

#endif
