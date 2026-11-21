#include "amd_host_bridge.h"
#include "rs780.h"

int snprintf(char *, size_t, const char *, ...);

#define RS780_HOST_ID 0x96001022u
#define RS780_GFX_PORT_ID 0x96031022u

/* RS780 PCIEIND_P: AMD 780G BIOS Developer's Guide, section 2.2.
 * LC_STATE0/L0 encoding is corroborated by coreboot 4.11 rs780/cmn.c.
 * Selecting a register is the only vendor write; never write the data port. */
static uint32_t root_read(struct book8_pcie_state *s, uint16_t reg)
{
    return book8_pcie_read32(s, 0u, 0u, 2u, 0u, reg);
}

bool book8_amd_host_bridge_probe(struct book8_pcie_state *s)
{
    s->amd_host_verified = false;
    if (s->rs780_access_blocked) {
        s->platform.log("PCIE1 HOST BLOCKED selector restoration failure; reboot required");
        return false;
    }
    bool mapped = false;
    for (size_t i = 0u; i < s->ecam_count; ++i)
        if (s->ecam[i].segment == 0u && s->ecam[i].first == 0u
            && s->ecam[i].mapped != NULL)
            mapped = true;
    if (!mapped) {
        s->platform.log("PCIE1 HOST BLOCKED requires validated MCFG ECAM for bus 0");
        return false;
    }
    if (book8_pcie_read32(s, 0u, 0u, 0u, 0u, 0u) != RS780_HOST_ID
        || root_read(s, 0u) != RS780_GFX_PORT_ID
        || (root_read(s, 8u) >> 16u) != 0x0604u
        || ((root_read(s, 0x0cu) >> 16u) & 0x7fu) != 1u
        || (root_read(s, 0x18u) & 0xffffffu) != 0x010100u) {
        s->platform.log("PCIE1 HOST BLOCKED RS780 host/root identity or bus routing mismatch");
        return false;
    }
    s->amd_host_verified = true;
    s->platform.log("PCIE1 HOST RS780 00:00.0 / 00:02.0 verified via ECAM");
    return true;
}

static bool read_ltssm(struct book8_pcie_state *s, uint32_t *value)
{
    const struct book8_rs780_ops ops = {
        .read = s->platform.read32_safe, .write = s->platform.write32_safe
    };
    struct book8_rs780_result result = book8_rs780_read_index(
        s, &ops, BOOK8_RS780_LC_STATE0);
    if (!result.valid || result.error != 0u || result.restore_error != 0u)
        return false;
    *value = result.value;
    return true;
}

bool book8_amd_root_link_ready(struct book8_pcie_state *s)
{
    s->target_link_verified = false;
    if (!book8_amd_host_bridge_probe(s))
        return false;
    for (uint32_t poll = 0u; poll < 50u; ++poll) {
        uint32_t state = UINT32_MAX;
        if (!read_ltssm(s, &state)) {
            s->platform.log("PCIE1 LTSSM ERROR index/data read or restore failed; command writes blocked");
            return false;
        }
        char text[128];
        snprintf(text, sizeof(text), "PCIE1 LTSSM 0000:00:02.0 A5=%08x state=%02x poll=%u",
            state, state & BOOK8_RS780_STATE_MASK, poll);
        s->platform.log(text);
        if ((state & BOOK8_RS780_STATE_MASK) == BOOK8_RS780_L0) {
            s->target_link_verified = true;
            s->platform.log("PCIE1 LTSSM L0 verified; link prerequisite satisfied; lane power unmeasured");
            return true;
        }
        if (s->platform.delay_ms == NULL || !s->platform.delay_ms(10u)) {
            s->platform.log("PCIE1 LTSSM ERROR polling delay unavailable; command writes blocked");
            return false;
        }
    }
    s->platform.log("PCIE1 LTSSM TIMEOUT no L0 in 50 polls; command writes blocked");
    return false;
}
