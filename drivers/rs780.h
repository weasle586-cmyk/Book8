#ifndef BOOK8_RS780_H
#define BOOK8_RS780_H

#include "pcie.h"

#define BOOK8_RS780_INDEX 0xe0u
#define BOOK8_RS780_DATA 0xe4u
#define BOOK8_RS780_LINK_WIDTH 0xa2u
#define BOOK8_RS780_LC_STATE0 0xa5u
#define BOOK8_RS780_STATE_MASK 0x3fu
#define BOOK8_RS780_L0 0x10u

struct book8_rs780_ops {
    uint32_t (*read)(volatile uint32_t *, uint32_t *);
    uint32_t (*write)(volatile uint32_t *, uint32_t);
};

struct book8_rs780_result {
    uint32_t value, exception, error, restore_exception, restore_error;
    uint16_t offset;
    bool valid;
    const char *step;
};

/* Low-level transport for boot prerequisites and the manual probe controller.
 * Configuration offsets at 0000:00:02.0, never CPU I/O ports or GPP core space.
 * E4 writes are deliberately unsupported. Fault guards cannot recover bus hangs. */
struct book8_rs780_result book8_rs780_read_index(struct book8_pcie_state *,
    const struct book8_rs780_ops *, uint32_t);
struct book8_rs780_result book8_rs780_read_link(struct book8_pcie_state *,
    const struct book8_rs780_ops *, bool);

#endif
