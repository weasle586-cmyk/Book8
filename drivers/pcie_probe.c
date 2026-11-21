#include "pcie_probe.h"

int snprintf(char *, size_t, const char *, ...);

static bool reject(struct book8_pcie_probe *p, const char *message)
{
    p->pcie->platform.log(message);
    return false;
}

void book8_probe_init(struct book8_pcie_probe *p, struct book8_pcie_state *s,
    const struct book8_probe_ops *ops)
{
    *p = (struct book8_pcie_probe){.pcie = s, .ops = *ops};
}

bool book8_probe_retry(struct book8_pcie_probe *p)
{
    if (p->phase == BOOK8_PROBE_IDLE)
        return reject(p, "PROBE1 ERROR no pending record");
    p->sent = p->ops.emit != NULL && p->ops.emit(p->record);
    if (!p->sent)
        return reject(p, "PROBE1 ERROR UDP submission failed; RETRY record only; ACK blocked");
    return true;
}

static bool begin(struct book8_pcie_probe *p, enum book8_probe_target target,
    uint32_t index, uint32_t mask)
{
    if (p->phase != BOOK8_PROBE_IDLE)
        return reject(p, "PROBE1 ERROR previous probe awaits ACK; no access");
    if (p->poisoned || p->pcie->rs780_access_blocked)
        return reject(p, "PROBE1 ERROR selector restoration failed; further probes blocked until reboot");
    if (target == BOOK8_PROBE_INDEX && index != BOOK8_RS780_LINK_WIDTH
        && (index < 0xa5u || index > 0xafu))
        return reject(p, "PROBE1 ERROR only RS780 indices A2/A5..AF permitted; no access");
    if (p->id == UINT32_MAX)
        return reject(p, "PROBE1 ERROR probe ID exhausted; reboot required");
    ++p->id;
    p->index = index;
    p->target = target;
    p->mask = mask;
    p->offset = 0u;
    p->value = p->exception = p->error = p->restore_exception = p->restore_error = 0u;
    p->value_valid = p->sent = false;
    p->phase = BOOK8_PROBE_INTENT;
    if (target == BOOK8_PROBE_INDEX) {
        if (index == BOOK8_RS780_LC_STATE0)
            p->ltssm_acknowledged = false;
        snprintf(p->record, sizeof(p->record),
            "PROBE1 id=%u INTENT 0000:00:02.0 E0/E4 idx=%02x width=32 mask=%08x data-write=BLOCKED",
            p->id, index, mask);
    } else {
        if (target == BOOK8_PROBE_LINK_CAP)
            p->link_cap_acknowledged = false;
        else
            p->link_status_acknowledged = false;
        snprintf(p->record, sizeof(p->record),
            "PROBE1 id=%u INTENT 0000:00:02.0 target=%s PCIe-cap-chain width=32 read-only",
            p->id, target == BOOK8_PROBE_LINK_CAP ? "LINKCAP" : "LINKSTATUS");
    }
    p->pcie->platform.log(p->record);
    (void)book8_probe_retry(p);
    return true;
}

bool book8_probe_begin(struct book8_pcie_probe *p, uint32_t index, uint32_t mask)
{
    return begin(p, BOOK8_PROBE_INDEX, index, mask);
}

bool book8_probe_begin_link(struct book8_pcie_probe *p, bool status)
{
    return begin(p, status ? BOOK8_PROBE_LINK_STATUS : BOOK8_PROBE_LINK_CAP, 0u, 0u);
}

static void access_one(struct book8_pcie_probe *p)
{
    struct book8_pcie_state *s = p->pcie;
    p->step = "gate";
    if (p->mask != 0u) {
        p->error = 1u;
        return;
    }
    const struct book8_rs780_ops ops = {.read = p->ops.read, .write = p->ops.write};
    struct book8_rs780_result result = p->target == BOOK8_PROBE_INDEX
        ? book8_rs780_read_index(s, &ops, p->index)
        : book8_rs780_read_link(s, &ops, p->target == BOOK8_PROBE_LINK_STATUS);
    p->value = result.value;
    p->value_valid = result.valid;
    p->offset = result.offset;
    p->step = result.step;
    p->exception = result.exception;
    p->error = result.error;
    p->restore_exception = result.restore_exception;
    p->restore_error = result.restore_error;
    p->poisoned = s->rs780_access_blocked;
}

bool book8_probe_link_verified(const struct book8_pcie_probe *p)
{
    if (p->phase != BOOK8_PROBE_IDLE || p->poisoned || p->pcie->rs780_access_blocked
        || !p->pcie->amd_host_verified || !p->ltssm_acknowledged
        || !p->link_cap_acknowledged || !p->link_status_acknowledged)
        return false;
    uint32_t status = p->link_status >> 16u;
    uint32_t speed = status & 0xfu, width = (status >> 4u) & 0x3fu;
    return speed != 0u && width != 0u && speed <= (p->link_cap & 0xfu)
        && width <= ((p->link_cap >> 4u) & 0x3fu) && (status & (1u << 11u)) == 0u
        && ((p->link_cap & (1u << 20u)) == 0u || (status & (1u << 13u)) != 0u);
}

bool book8_probe_ack(struct book8_pcie_probe *p, uint32_t id,
    enum book8_probe_phase phase)
{
    if (p->phase == BOOK8_PROBE_IDLE || id != p->id || phase != p->phase || !p->sent)
        return reject(p, "PROBE1 ERROR stale/wrong-phase ACK or record not submitted; no access");
    if (phase == BOOK8_PROBE_RESULT) {
        bool ok = p->value_valid && p->exception == 0u && p->error == 0u
            && p->restore_exception == 0u && p->restore_error == 0u;
        if (p->target == BOOK8_PROBE_INDEX && p->index == BOOK8_RS780_LC_STATE0)
            p->ltssm_acknowledged = ok
                && (p->value & BOOK8_RS780_STATE_MASK) == BOOK8_RS780_L0;
        else if (p->target == BOOK8_PROBE_LINK_CAP) {
            p->link_cap_acknowledged = ok;
            p->link_cap = p->value;
        } else if (p->target == BOOK8_PROBE_LINK_STATUS) {
            p->link_status_acknowledged = ok;
            p->link_status = p->value;
        }
        p->phase = BOOK8_PROBE_IDLE;
        return true;
    }
    /* Change phase before access so retransmissions can never repeat it. */
    p->phase = BOOK8_PROBE_RESULT;
    p->sent = false;
    access_one(p);
    if (p->target == BOOK8_PROBE_INDEX) {
        snprintf(p->record, sizeof(p->record),
            "PROBE1 id=%u RESULT idx=%02x step=%s valid=%u data=%08x ex=%u errno=%u restore-ex=%u restore-errno=%u",
            p->id, p->index, p->step, p->value_valid ? 1u : 0u, p->value,
            p->exception, p->error, p->restore_exception, p->restore_error);
    } else {
        snprintf(p->record, sizeof(p->record),
            "PROBE1 id=%u RESULT target=%s off=%03x valid=%u data=%08x ex=%u errno=%u",
            p->id, p->target == BOOK8_PROBE_LINK_CAP ? "LINKCAP" : "LINKSTATUS",
            p->offset, p->value_valid ? 1u : 0u, p->value, p->exception, p->error);
    }
    p->pcie->platform.log(p->record);
    (void)book8_probe_retry(p);
    return true;
}
