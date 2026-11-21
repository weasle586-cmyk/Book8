#include "rs780.h"

static bool available(struct book8_pcie_state *s, const struct book8_rs780_ops *ops)
{
    return s->amd_host_verified && !s->rs780_access_blocked
        && ops->read != NULL && s->platform.irq_save != NULL
        && s->platform.irq_restore != NULL;
}

static bool read_config(struct book8_pcie_state *s,
    const struct book8_rs780_ops *ops, uint16_t offset,
    struct book8_rs780_result *result, uint32_t *value)
{
    volatile uint32_t *address = (volatile uint32_t *)book8_pcie_ecam_address(
        s, 0u, 0u, 2u, 0u, offset);
    result->offset = offset;
    if (address == NULL) {
        result->error = 19u;
        return false;
    }
    result->exception = ops->read(address, value);
    if (result->exception != 0u || *value == UINT32_MAX) {
        result->error = result->exception != 0u ? 5u : 19u;
        return false;
    }
    return true;
}

struct book8_rs780_result book8_rs780_read_index(struct book8_pcie_state *s,
    const struct book8_rs780_ops *ops, uint32_t index)
{
    struct book8_rs780_result r = {.step = "gate"};
    if (index != BOOK8_RS780_LINK_WIDTH && (index < 0xa5u || index > 0xafu)) {
        r.error = 1u;
        s->platform.log("RS780 ERROR indirect index outside read-only allowlist");
        return r;
    }
    if (!available(s, ops) || ops->write == NULL) {
        r.error = 19u;
        s->platform.log("RS780 ERROR verified host or guarded/serialized access unavailable");
        return r;
    }
    volatile uint32_t *selector = (volatile uint32_t *)book8_pcie_ecam_address(
        s, 0u, 0u, 2u, 0u, BOOK8_RS780_INDEX);
    volatile uint32_t *data = (volatile uint32_t *)book8_pcie_ecam_address(
        s, 0u, 0u, 2u, 0u, BOOK8_RS780_DATA);
    if (selector == NULL || data == NULL) {
        r.error = 19u;
        s->platform.log("RS780 ERROR validated root-port ECAM unavailable");
        return r;
    }
    uint64_t flags = s->platform.irq_save();
    uint32_t saved = UINT32_MAX, selected = UINT32_MAX;
    r.step = "save";
    r.offset = BOOK8_RS780_INDEX;
    r.exception = ops->read(selector, &saved);
    if (r.exception != 0u || saved == UINT32_MAX) {
        r.error = r.exception != 0u ? 5u : 19u;
        goto done;
    }
    r.step = "select";
    r.exception = ops->write(selector, index);
    if (r.exception == 0u) {
        r.step = "select-check";
        r.exception = ops->read(selector, &selected);
        if (r.exception == 0u && selected == index) {
            r.step = "data";
            r.offset = BOOK8_RS780_DATA;
            r.exception = ops->read(data, &r.value);
            r.valid = r.exception == 0u && r.value != UINT32_MAX;
            if (!r.valid)
                r.error = r.exception != 0u ? 5u : 19u;
        } else {
            r.error = 5u;
        }
    } else {
        r.error = 5u;
    }
    /* Restore even after a selector fault; echoing E4 data could clear W1C bits. */
    r.restore_exception = ops->write(selector, saved);
    if (r.restore_exception == 0u) {
        uint32_t restored = UINT32_MAX;
        r.restore_exception = ops->read(selector, &restored);
        if (r.restore_exception != 0u || restored != saved)
            r.restore_error = 5u;
    } else {
        r.restore_error = 5u;
    }
    if (r.restore_error != 0u) {
        s->rs780_access_blocked = true;
        s->amd_host_verified = s->target_link_verified = false;
    }
done:
    s->platform.irq_restore(flags);
    if (r.error != 0u || r.restore_error != 0u)
        s->platform.log("RS780 ERROR guarded indirect access or selector restoration failed");
    return r;
}

struct book8_rs780_result book8_rs780_read_link(struct book8_pcie_state *s,
    const struct book8_rs780_ops *ops, bool status)
{
    struct book8_rs780_result r = {.step = "gate"};
    if (!available(s, ops)) {
        r.error = 19u;
        s->platform.log("RS780 ERROR verified host or guarded/serialized access unavailable");
        return r;
    }
    uint64_t flags = s->platform.irq_save();
    uint32_t word = 0u;
    r.step = "cap-status";
    if (!read_config(s, ops, 4u, &r, &word))
        goto done;
    if ((word & (1u << 20u)) == 0u) {
        r.error = 19u;
        goto done;
    }
    r.step = "cap-pointer";
    if (!read_config(s, ops, 0x34u, &r, &word))
        goto done;
    uint32_t offset = word & 0xffu;
    uint64_t visited = 0u;
    while (offset != 0u) {
        r.step = "cap-chain";
        if (offset < 0x40u || offset > 0xfcu || (offset & 3u) != 0u
            || (visited & (1ull << (offset >> 2u))) != 0u) {
            r.error = 5u;
            goto done;
        }
        visited |= 1ull << (offset >> 2u);
        if (!read_config(s, ops, (uint16_t)offset, &r, &word))
            goto done;
        if ((word & 0xffu) == 0x10u) {
            uint32_t capability = word >> 16u;
            /* PCIe capability v1/v2, device/port type 4 (Root Port). */
            if ((capability & 0xfu) < 1u || (capability & 0xfu) > 2u
                || ((capability >> 4u) & 0xfu) != 4u || offset > 0xecu) {
                r.error = 5u;
                goto done;
            }
            r.step = status ? "link-status" : "link-cap";
            if (read_config(s, ops, (uint16_t)(offset + (status ? 0x10u : 0x0cu)),
                &r, &r.value))
                r.valid = true;
            goto done;
        }
        offset = (word >> 8u) & 0xffu;
    }
    r.error = 19u;
done:
    s->platform.irq_restore(flags);
    if (r.error != 0u)
        s->platform.log("RS780 ERROR PCIe link capability absent, malformed or faulted");
    return r;
}
