#include "rs780_manual_audit.h"

int snprintf(char *, size_t, const char *, ...);
int strcmp(const char *, const char *);

static bool reject(struct rs780_audit *a, const char *message)
{
    if (a != NULL && a->pcie != NULL && a->pcie->platform.log != NULL)
        a->pcie->platform.log(message);
    return false;
}

static bool publish(struct rs780_audit *a)
{
    a->confirmed = false;
    a->submitted = a->emit(a->record);
    return a->submitted || reject(a, "RSAUDIT ERROR telemetry failed; ACK blocked");
}

static void release(struct rs780_audit *a)
{
    uint64_t flags = a->pcie->platform.irq_save();
    a->pcie->rs780_access_blocked = false;
    a->owned = false;
    a->pcie->platform.irq_restore(flags);
}

bool rs780_audit_init(struct rs780_audit *a, struct book8_pcie_state *pcie,
                     uint64_t session, bool (*emit)(const char *))
{
    if (a == NULL || pcie == NULL || pcie->platform.log == NULL)
        return false;
    if (a->initialized || session == 0u || emit == NULL) {
        pcie->platform.log("RSAUDIT ERROR reused context or invalid initialization");
        return false;
    }
    if (pcie->platform.irq_save == NULL || pcie->platform.irq_restore == NULL
        || pcie->platform.read32_safe == NULL || pcie->platform.write32_safe == NULL) {
        pcie->platform.log("RSAUDIT ERROR guarded transport unavailable");
        return false;
    }
    uint64_t flags = pcie->platform.irq_save();
    bool allowed = pcie->amd_host_verified && !pcie->rs780_access_blocked;
    if (allowed)
        pcie->rs780_access_blocked = true;
    pcie->platform.irq_restore(flags);
    if (!allowed) {
        pcie->platform.log("RSAUDIT ERROR host unverified or indexed transport locked");
        return false;
    }
    *a = (struct rs780_audit){.pcie = pcie, .emit = emit, .session = session,
        .phase = RS780_IDLE, .operation = RS780_SAVE_SELECTOR,
        .initialized = true, .owned = true};
    pcie->platform.log("RSAUDIT initialized; no hardware access; GPU MMIO unchanged");
    return true;
}

static bool prepare(struct rs780_audit *a, enum rs780_operation requested)
{
    if (a == NULL || !a->initialized || !a->owned)
        return reject(a, "RSAUDIT ERROR ownership unavailable");
    if (a->phase != RS780_IDLE || a->operation != requested
        || requested == RS780_DONE || (a->failed && !a->recovering))
        return reject(a, "RSAUDIT ERROR pending, failed or out-of-order request");
    if (a->id == UINT32_MAX)
        return reject(a, "RSAUDIT ERROR IDs exhausted");
    a->offset = RS780_CFG_INDEX;
    a->write = false;
    a->write_value = 0u;
    switch (requested) {
    case RS780_SELECT_LTSSM:
        a->write = true;
        a->write_value = RS780_IND_LTSSM;
        break;
    case RS780_RESTORE_SELECTOR:
        if (!a->saved_valid)
            return reject(a, "RSAUDIT ERROR original selector unavailable");
        a->write = true;
        a->write_value = a->saved_selector;
        break;
    case RS780_READ_LTSSM:
        a->offset = RS780_CFG_DATA;
        break;
    case RS780_READ_CAP_STATUS:
        a->offset = 0x04u;
        break;
    case RS780_READ_CAP_POINTER:
        a->offset = 0x34u;
        break;
    case RS780_READ_CAP_HEADER:
        a->offset = a->capability_base;
        break;
    case RS780_READ_LINKCAP:
        a->offset = a->capability_base + RS780_PCIE_LINKCAP_REL;
        break;
    case RS780_READ_LINKSTATUS:
        a->offset = a->capability_base + RS780_PCIE_LINKSTATUS_REL;
        break;
    case RS780_SAVE_SELECTOR:
    case RS780_CHECK_SELECTOR:
    case RS780_CHECK_RESTORE:
        break;
    default:
        return reject(a, "RSAUDIT ERROR invalid operation");
    }
    ++a->id;
    a->phase = RS780_INTENT;
    a->valid = false;
    a->data = a->exception = a->error = 0u;
    a->submitted = a->confirmed = false;
    int length = snprintf(a->record, sizeof(a->record),
        "RSAUDIT s=%016llx id=%u INTENT bdf=00:02.0 off=%03x op=%u wr=%u value=%08x",
        (unsigned long long)a->session, a->id, a->offset,
        (unsigned)a->operation, a->write ? 1u : 0u, a->write_value);
    if (length < 0 || (size_t)length >= sizeof(a->record)) {
        a->record[0] = '\0';
        a->failed = true;
        return reject(a, "RSAUDIT ERROR INTENT encoding failed");
    }
    a->pcie->platform.log(a->record);
    return publish(a);
}

bool rs780_audit_request_next(struct rs780_audit *a)
{
    if (a == NULL)
        return false;
    if (a->operation == RS780_READ_LINKCAP || a->operation == RS780_READ_LINKSTATUS)
        return reject(a, "RSAUDIT ERROR use isolated LINKCAP/LINKSTATUS request");
    return prepare(a, a->operation);
}

bool rs780_audit_request_linkcap(struct rs780_audit *a)
{
    return prepare(a, RS780_READ_LINKCAP);
}

bool rs780_audit_request_linkstatus(struct rs780_audit *a)
{
    return prepare(a, RS780_READ_LINKSTATUS);
}

bool rs780_audit_retry_record(struct rs780_audit *a)
{
    if (a == NULL || !a->initialized || a->phase == RS780_IDLE || a->record[0] == '\0')
        return reject(a, "RSAUDIT ERROR no valid pending record");
    return publish(a);
}

bool rs780_audit_confirm_collector(struct rs780_audit *a, uint64_t session,
                                  uint32_t id, enum rs780_phase phase,
                                  const char *collector_record)
{
    if (a == NULL || !a->initialized || !a->submitted || session != a->session
        || id != a->id || phase == RS780_IDLE || phase != a->phase
        || collector_record == NULL || strcmp(collector_record, a->record) != 0)
        return reject(a, "RSAUDIT ERROR collector session/ID/phase/record mismatch");
    a->confirmed = true;
    return true;
}

/* Exactly one guarded transaction; only E0 selector writes are permitted. */
static void guarded_access(struct rs780_audit *a)
{
    if (!a->pcie->rs780_access_blocked
        || (!a->recovering && !a->pcie->amd_host_verified)) {
        a->error = 19u;
        return;
    }
    if (a->write && (a->offset != RS780_CFG_INDEX
        || (a->operation != RS780_SELECT_LTSSM && a->operation != RS780_RESTORE_SELECTOR))) {
        a->error = 1u;
        return;
    }
    if (a->pcie->platform.read32_safe == NULL || a->pcie->platform.write32_safe == NULL) {
        a->error = 19u;
        return;
    }
    volatile uint32_t *address = (volatile uint32_t *)book8_pcie_ecam_address(
        a->pcie, 0u, 0u, 2u, 0u, a->offset);
    if (address == NULL) {
        a->error = 19u;
        return;
    }
    uint64_t flags = a->pcie->platform.irq_save();
    if (a->operation == RS780_SELECT_LTSSM)
        a->dirty = true;
    a->exception = a->write
        ? a->pcie->platform.write32_safe(address, a->write_value)
        : a->pcie->platform.read32_safe(address, &a->data);
    a->pcie->platform.irq_restore(flags);
    if (a->exception != 0u)
        a->error = 5u;
    else if (!a->write && a->data == UINT32_MAX)
        a->error = 19u;
    else
        a->valid = true;
}

bool rs780_audit_ack_intent(struct rs780_audit *a, uint32_t id)
{
    if (a == NULL || !a->owned || a->phase != RS780_INTENT || id != a->id
        || !a->submitted || !a->confirmed || (a->failed && !a->recovering))
        return reject(a, "RSAUDIT ERROR INTENT ACK refused");
    a->phase = RS780_RESULT;
    a->submitted = a->confirmed = false;
    a->data = a->exception = a->error = 0u;
    a->valid = false;
    guarded_access(a);
    if (!a->valid)
        a->failed = true;
    int length = snprintf(a->record, sizeof(a->record),
        "RSAUDIT s=%016llx id=%u RESULT off=%03x op=%u valid=%u data=%08x ex=%u errno=%u",
        (unsigned long long)a->session, a->id, a->offset,
        (unsigned)a->operation, a->valid ? 1u : 0u,
        a->data, a->exception, a->error);
    if (length < 0 || (size_t)length >= sizeof(a->record)) {
        a->record[0] = '\0';
        a->failed = true;
        return reject(a, "RSAUDIT ERROR RESULT encoding failed; hardware not retried");
    }
    a->pcie->platform.log(a->record);
    return publish(a);
}

static bool accept_pointer(struct rs780_audit *a, uint32_t offset)
{
    if (offset < 0x40u || offset > 0xFCu || (offset & 3u) != 0u)
        return false;
    uint64_t bit = 1ull << (offset >> 2u);
    if ((a->visited & bit) != 0u)
        return false;
    a->visited |= bit;
    a->capability_base = (uint16_t)offset;
    return true;
}

bool rs780_audit_ack_result(struct rs780_audit *a, uint32_t id)
{
    if (a == NULL || !a->owned || a->phase != RS780_RESULT || id != a->id
        || !a->submitted || !a->confirmed)
        return reject(a, "RSAUDIT ERROR RESULT ACK refused");
    if (!a->valid || a->exception != 0u || a->error != 0u)
        return reject(a, "RSAUDIT ERROR failed RESULT; explicit abort required");
    bool ok = true, another_header = false;
    switch (a->operation) {
    case RS780_SAVE_SELECTOR:
        a->saved_selector = a->data;
        a->saved_valid = true;
        break;
    case RS780_CHECK_SELECTOR:
        ok = a->data == RS780_IND_LTSSM;
        break;
    case RS780_READ_LTSSM:
        ok = (a->data & RS780_LTSSM_MASK) == RS780_LTSSM_L0;
        if (ok)
            a->ltssm = a->data;
        break;
    case RS780_CHECK_RESTORE:
        ok = a->data == a->saved_selector;
        if (ok)
            a->dirty = false;
        break;
    case RS780_READ_CAP_STATUS:
        ok = (a->data & (1u << 20u)) != 0u;
        break;
    case RS780_READ_CAP_POINTER:
        ok = accept_pointer(a, a->data & 0xFFu);
        break;
    case RS780_READ_CAP_HEADER:
        if ((a->data & 0xFFu) == 0x10u) {
            uint32_t cap = a->data >> 16u;
            uint32_t version = cap & 0xFu;
            ok = (version == 1u || version == 2u) && ((cap >> 4u) & 0xFu) == 4u
                && a->capability_base <= 0xECu;
        } else {
            ok = accept_pointer(a, (a->data >> 8u) & 0xFFu);
            another_header = ok;
        }
        break;
    case RS780_READ_LINKCAP:
        a->linkcap = a->data;
        break;
    case RS780_READ_LINKSTATUS:
        a->linkstatus = a->data;
        break;
    case RS780_SELECT_LTSSM:
    case RS780_RESTORE_SELECTOR:
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) {
        a->failed = true;
        return reject(a, "RSAUDIT ERROR L0/selector/capability check failed; abort required");
    }
    a->phase = RS780_IDLE;
    a->confirmed = false;
    if (a->recovering && a->operation == RS780_CHECK_RESTORE) {
        a->operation = RS780_DONE;
        release(a);
        return true;
    }
    if (!another_header)
        a->operation = (enum rs780_operation)(a->operation + 1);
    if (a->operation == RS780_DONE) {
        a->complete = true;
        release(a);
    }
    return true;
}

bool rs780_audit_finish(struct rs780_audit *a)
{
    if (a == NULL || !a->initialized || !a->owned || a->failed || a->recovering
        || a->dirty || !a->saved_valid || a->phase != RS780_IDLE || a->id != 6u
        || a->operation != RS780_READ_CAP_STATUS || !a->valid
        || a->exception != 0u || a->error != 0u || a->data != a->saved_selector
        || (a->ltssm & RS780_LTSSM_MASK) != RS780_LTSSM_L0
        || !a->pcie->amd_host_verified || !a->pcie->rs780_access_blocked)
        return reject(a, "RSAUDIT ERROR finish requires all six successful manual steps");
    a->operation = RS780_DONE;
    a->complete = false;
    release(a);
    a->pcie->platform.log("RSAUDIT six-step sampled L0 audit finished; full preflight incomplete; GPU MMIO unchanged");
    return true;
}

bool rs780_audit_abort(struct rs780_audit *a)
{
    if (a == NULL || !a->initialized || !a->owned)
        return reject(a, "RSAUDIT ERROR no owned audit to abort");
    if (a->phase == RS780_RESULT && (!a->submitted || !a->confirmed))
        return reject(a, "RSAUDIT ERROR deliver and confirm RESULT before abort");
    a->failed = true;
    a->complete = false;
    if (a->recovering && a->phase == RS780_RESULT
        && (a->operation == RS780_RESTORE_SELECTOR || a->operation == RS780_CHECK_RESTORE)) {
        a->pcie->amd_host_verified = a->pcie->target_link_verified = false;
        return reject(a, "RSAUDIT ERROR recovery failed; lock retained; reboot required");
    }
    a->phase = RS780_IDLE;
    a->submitted = a->confirmed = false;
    if (!a->dirty) {
        a->operation = RS780_DONE;
        release(a);
        return true;
    }
    if (!a->saved_valid)
        return reject(a, "RSAUDIT ERROR unknown selector; lock retained");
    a->recovering = true;
    a->operation = RS780_RESTORE_SELECTOR;
    a->pcie->platform.log("RSAUDIT aborted; manual restore and readback handshakes required");
    return true;
}

bool rs780_audit_preflight(struct rs780_audit *a)
{
    if (a == NULL || !a->initialized || !a->complete || a->failed || a->recovering
        || a->dirty || a->owned || a->phase != RS780_IDLE
        || !a->pcie->amd_host_verified || a->pcie->rs780_access_blocked)
        return reject(a, "RSAUDIT ERROR preflight incomplete or interlocked");
    uint32_t status = a->linkstatus >> RS780_STATUS_SHIFT;
    uint32_t speed = status & RS780_SPEED_MASK;
    uint32_t width = (status >> RS780_LANES_SHIFT) & RS780_LANES_MASK;
    bool ok = (a->ltssm & RS780_LTSSM_MASK) == RS780_LTSSM_L0
        && speed != 0u && width != 0u && speed <= (a->linkcap & RS780_SPEED_MASK)
        && width <= ((a->linkcap >> RS780_LANES_SHIFT) & RS780_LANES_MASK)
        && (status & RS780_LINK_TRAINING) == 0u
        && ((a->linkcap & RS780_DLL_REPORTING) == 0u || (status & RS780_DLL_ACTIVE) != 0u);
    a->pcie->platform.log(ok ? "RSAUDIT sampled preflight PASS; GPU MMIO unchanged"
        : "RSAUDIT sampled preflight FAIL; no GPU operation permitted");
    return ok;
}
