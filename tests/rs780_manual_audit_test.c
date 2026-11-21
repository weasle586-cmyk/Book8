#include "../drivers/rs780_manual_audit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct book8_pcie_state pcie;
static struct rs780_audit audit;
static uint32_t config[64];
static unsigned accesses, stores, fault_at;
static bool sending, irq_active;
static char packet[128];

static void log_event(const char *message)
{
    assert(message != NULL);
}

static uint64_t irq_save(void)
{
    assert(!irq_active);
    irq_active = true;
    return 0x200u;
}

static void irq_restore(uint64_t flags)
{
    assert(irq_active && flags == 0x200u);
    irq_active = false;
}

static uint32_t read_safe(volatile uint32_t *address, uint32_t *value)
{
    assert(irq_active && address >= config && address < config + 64u);
    if (++accesses == fault_at)
        return 14u;
    *value = *address;
    return 0u;
}

static uint32_t write_safe(volatile uint32_t *address, uint32_t value)
{
    assert(irq_active && address == config + RS780_CFG_INDEX / 4u);
    ++stores;
    if (++accesses == fault_at)
        return 13u;
    *address = value;
    return 0u;
}

static bool emit(const char *record)
{
    assert(!irq_active && strlen(record) < 128u);
    snprintf(packet, sizeof(packet), "%s", record);
    return sending;
}

/* Stub only address resolution; the production wrapper still executes
 * the configured guarded callbacks once per INTENT ACK. */
volatile uint8_t *book8_pcie_ecam_address(struct book8_pcie_state *s,
    uint16_t segment, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset)
{
    assert(s == &pcie && segment == 0u && bus == 0u && slot == 2u && fn == 0u);
    assert(offset <= 0xfcu && (offset & 3u) == 0u);
    return s->ecam_count == 0u ? NULL : (volatile uint8_t *)config + offset;
}

static void fixture(void)
{
    memset(&audit, 0, sizeof(audit));
    memset(config, 0, sizeof(config));
    pcie = (struct book8_pcie_state){.amd_host_verified = true,
        .ecam_count = 1u, .platform = {
            .log = log_event, .read32_safe = read_safe, .write32_safe = write_safe,
            .irq_save = irq_save, .irq_restore = irq_restore}};
    config[0xe0u / 4u] = 0x52u;
    config[0xe4u / 4u] = 0x20212210u;
    config[4u / 4u] = 1u << 20u;
    config[0x34u / 4u] = 0x40u;
    config[0x40u / 4u] = 0x00035801u;
    config[0x58u / 4u] = 0x00410010u;
    config[0x64u / 4u] = 0x00300d02u;
    config[0x68u / 4u] = 0xf1010040u;
    accesses = stores = fault_at = 0u;
    sending = true;
    irq_active = false;
    assert(rs780_audit_init(&audit, &pcie, 0x123456789abcdef0ull, emit));
    assert(pcie.rs780_access_blocked && accesses == 0u);
}

static bool request(void)
{
    if (audit.operation == RS780_READ_LINKCAP)
        return rs780_audit_request_linkcap(&audit);
    if (audit.operation == RS780_READ_LINKSTATUS)
        return rs780_audit_request_linkstatus(&audit);
    return rs780_audit_request_next(&audit);
}

static void confirm(enum rs780_phase phase)
{
    assert(rs780_audit_confirm_collector(&audit, audit.session, audit.id, phase, packet));
}

static void transaction(void)
{
    unsigned before = accesses;
    assert(request());
    assert(accesses == before && audit.phase == RS780_INTENT);
    assert(!rs780_audit_ack_intent(&audit, audit.id));
    confirm(RS780_INTENT);
    assert(rs780_audit_ack_intent(&audit, audit.id));
    assert(accesses == before + 1u && audit.phase == RS780_RESULT);
    assert(!rs780_audit_ack_intent(&audit, audit.id));
    assert(!rs780_audit_ack_result(&audit, audit.id));
    confirm(RS780_RESULT);
    assert(rs780_audit_ack_result(&audit, audit.id));
    assert(accesses == before + 1u && audit.phase == RS780_IDLE);
}

static void reach(enum rs780_operation operation)
{
    while (audit.operation != operation)
        transaction();
}

static void test_success(void)
{
    fixture();
    assert(!rs780_audit_request_linkcap(&audit));
    assert(!rs780_audit_request_linkstatus(&audit));
    reach(RS780_READ_LINKCAP);
    assert(config[0xe0u / 4u] == 0x52u && stores == 2u);
    assert(!rs780_audit_request_next(&audit));
    assert(!rs780_audit_request_linkstatus(&audit));
    transaction();
    assert(audit.linkcap == 0x00300d02u && audit.offset == 0x64u);
    assert(!rs780_audit_preflight(&audit));
    assert(!rs780_audit_request_next(&audit));
    transaction();
    assert(audit.linkstatus == 0xf1010040u && audit.offset == 0x68u);
    assert(audit.complete && !pcie.rs780_access_blocked && !audit.owned);
    assert(rs780_audit_preflight(&audit) && !pcie.target_ready);
    assert(accesses == 12u && stores == 2u);
    assert(!rs780_audit_request_linkstatus(&audit));
    assert(!rs780_audit_init(&audit, &pcie, 1u, emit));
    audit.linkstatus |= 1u << 27u;
    assert(!rs780_audit_preflight(&audit));
}

static void test_six_step_finish(void)
{
    fixture();
    assert(!rs780_audit_finish(&audit));
    transaction();
    transaction();
    transaction();
    transaction();
    assert(audit.id == 4u && audit.ltssm == 0x20212210u && audit.dirty);
    assert(!rs780_audit_finish(&audit) && pcie.rs780_access_blocked);
    transaction();
    assert(audit.id == 5u && !rs780_audit_finish(&audit));
    assert(request());
    confirm(RS780_INTENT);
    assert(rs780_audit_ack_intent(&audit, audit.id));
    assert(!rs780_audit_finish(&audit));
    confirm(RS780_RESULT);
    assert(rs780_audit_ack_result(&audit, audit.id));
    assert(rs780_audit_finish(&audit));
    assert(accesses == 6u && stores == 2u && config[0xe0u / 4u] == 0x52u);
    assert(!audit.failed && !audit.complete && !audit.owned && !pcie.rs780_access_blocked);
    assert(!rs780_audit_preflight(&audit) && !pcie.target_ready);
    assert(!rs780_audit_finish(&audit) && !request());
}

static void test_delivery(void)
{
    fixture();
    sending = false;
    assert(!request() && accesses == 0u);
    assert(!rs780_audit_ack_intent(&audit, audit.id));
    sending = true;
    assert(rs780_audit_retry_record(&audit));
    assert(!rs780_audit_confirm_collector(&audit, audit.session + 1u,
        audit.id, RS780_INTENT, packet));
    assert(!rs780_audit_confirm_collector(&audit, audit.session,
        audit.id + 1u, RS780_INTENT, packet));
    assert(!rs780_audit_confirm_collector(&audit, audit.session,
        audit.id, RS780_RESULT, packet));
    assert(!rs780_audit_confirm_collector(&audit, audit.session,
        audit.id, RS780_INTENT, "wrong"));
    confirm(RS780_INTENT);
    sending = false;
    assert(!rs780_audit_ack_intent(&audit, audit.id) && accesses == 1u);
    assert(!rs780_audit_ack_intent(&audit, audit.id));
    assert(!rs780_audit_abort(&audit));
    assert(!rs780_audit_retry_record(&audit) && accesses == 1u);
    sending = true;
    assert(rs780_audit_retry_record(&audit));
    confirm(RS780_RESULT);
    assert(rs780_audit_ack_result(&audit, audit.id));
    assert(accesses == 1u);
}

static void test_faults(void)
{
    for (unsigned at = 1u; at <= 12u; ++at) {
        fixture();
        fault_at = at;
        while (accesses < at - 1u)
            transaction();
        assert(request());
        confirm(RS780_INTENT);
        assert(rs780_audit_ack_intent(&audit, audit.id));
        assert(accesses == at && audit.failed && audit.error == 5u && !audit.valid);
        assert(audit.exception == (audit.write ? 13u : 14u) && !irq_active);
        confirm(RS780_RESULT);
        assert(!rs780_audit_ack_result(&audit, audit.id));
        assert(rs780_audit_abort(&audit));
        if (audit.dirty) {
            assert(audit.recovering && pcie.rs780_access_blocked);
            fault_at = 0u;
            transaction();
            transaction();
        }
        assert(!audit.owned && !audit.complete && !pcie.rs780_access_blocked);
        assert(config[0xe0u / 4u] == 0x52u);
        assert(!rs780_audit_preflight(&audit));
    }
}

static void test_semantics(void)
{
    fixture();
    config[0xe4u / 4u] = 0x6u;
    reach(RS780_READ_LTSSM);
    assert(request());
    confirm(RS780_INTENT);
    assert(rs780_audit_ack_intent(&audit, audit.id));
    confirm(RS780_RESULT);
    char result[128];
    snprintf(result, sizeof(result), "%s", audit.record);
    assert(!rs780_audit_ack_result(&audit, audit.id) && audit.failed);
    assert(strcmp(result, audit.record) == 0);
    assert(rs780_audit_abort(&audit));
    transaction();
    transaction();
    assert(!audit.complete && !rs780_audit_preflight(&audit));

    for (unsigned bad = 0u; bad < 5u; ++bad) {
        fixture();
        if (bad == 0u) config[0x34u / 4u] = 0x41u;
        if (bad == 1u) config[0x40u / 4u] = 0x00034001u;
        if (bad == 2u) config[0x58u / 4u] = 0x00400010u;
        if (bad == 3u) config[0x58u / 4u] = 0x00010010u;
        if (bad == 4u) {
            config[0x34u / 4u] = 0xf0u;
            config[0xf0u / 4u] = 0x00410010u;
        }
        while (!audit.failed) {
            assert(request());
            confirm(RS780_INTENT);
            assert(rs780_audit_ack_intent(&audit, audit.id));
            confirm(RS780_RESULT);
            bool ok = rs780_audit_ack_result(&audit, audit.id);
            assert(ok || audit.failed);
        }
        assert(accesses < 13u && stores == 2u);
        assert(rs780_audit_abort(&audit) && !audit.owned);
    }
    fixture();
    reach(RS780_SELECT_LTSSM);
    assert(request());
    confirm(RS780_INTENT);
    fault_at = accesses + 1u;
    assert(rs780_audit_ack_intent(&audit, audit.id));
    confirm(RS780_RESULT);
    assert(rs780_audit_abort(&audit));
    assert(request());
    confirm(RS780_INTENT);
    fault_at = accesses + 1u;
    assert(rs780_audit_ack_intent(&audit, audit.id));
    confirm(RS780_RESULT);
    assert(!rs780_audit_abort(&audit));
    assert(pcie.rs780_access_blocked && !pcie.amd_host_verified && !pcie.target_link_verified);
}

int main(void)
{
    test_success();
    test_six_step_finish();
    test_delivery();
    test_faults();
    test_semantics();
    puts("PASS: per-transaction RS780 manual ACKs, discovery, isolated link queries, delivery/fault recovery and preflight");
    puts("PASS: six-step finish requires acknowledged restore/readback and does not authorize full preflight");
    return 0;
}
