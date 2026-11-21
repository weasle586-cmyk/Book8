#ifndef RS780_MANUAL_AUDIT_H
#define RS780_MANUAL_AUDIT_H

#include "pcie.h"

/* PCI configuration offsets at 0000:00:02.0, not CPU I/O ports. */
#define RS780_CFG_INDEX           0xE0u
#define RS780_CFG_DATA            0xE4u
#define RS780_IND_LINK_WIDTH      0xA2u
#define RS780_IND_LTSSM           0xA5u
#define RS780_LTSSM_MASK          0x3Fu
#define RS780_LTSSM_L0            0x10u
#define RS780_PCIE_LINKCAP_REL    0x0Cu
#define RS780_PCIE_LINKSTATUS_REL 0x10u
#define RS780_SPEED_MASK          0xFu
#define RS780_LANES_MASK          0x3Fu
#define RS780_LANES_SHIFT         4u
#define RS780_STATUS_SHIFT        16u
#define RS780_DLL_REPORTING       (1u << 20u)
#define RS780_LINK_TRAINING       (1u << 11u)
#define RS780_DLL_ACTIVE          (1u << 13u)

#define RS780_OBSERVED_CAP_BASE   0x058u
#define RS780_OBSERVED_LINKCAP    0x064u
#define RS780_OBSERVED_LINKSTATUS 0x068u

enum rs780_phase { RS780_IDLE, RS780_INTENT, RS780_RESULT };

enum rs780_operation {
    RS780_SAVE_SELECTOR, RS780_SELECT_LTSSM, RS780_CHECK_SELECTOR,
    RS780_READ_LTSSM, RS780_RESTORE_SELECTOR, RS780_CHECK_RESTORE,
    RS780_READ_CAP_STATUS, RS780_READ_CAP_POINTER, RS780_READ_CAP_HEADER,
    RS780_READ_LINKCAP, RS780_READ_LINKSTATUS, RS780_DONE
};

/* Zero-initialize once; serialize calls on the BSP. Every indexed accessor
 * must honor rs780_access_blocked. This does not exclude SMM/raw PCI access.
 * emit returns actual UDP submission success, not collector receipt. */
struct rs780_audit {
    struct book8_pcie_state *pcie;
    bool (*emit)(const char *record);
    uint64_t session;
    uint64_t visited;
    uint32_t id;
    uint32_t saved_selector;
    uint32_t ltssm;
    uint32_t linkcap;
    uint32_t linkstatus;
    uint32_t data;
    uint32_t write_value;
    uint32_t exception;
    uint32_t error;
    uint16_t capability_base;
    uint16_t offset;
    enum rs780_phase phase;
    enum rs780_operation operation;
    bool initialized;
    bool owned;
    bool write;
    bool valid;
    bool submitted;
    bool confirmed;
    bool saved_valid;
    bool dirty;
    bool failed;
    bool recovering;
    bool complete;
    char record[128];
};

bool rs780_audit_init(struct rs780_audit *, struct book8_pcie_state *,
                     uint64_t session, bool (*emit)(const char *));
bool rs780_audit_request_next(struct rs780_audit *);
bool rs780_audit_request_linkcap(struct rs780_audit *);
bool rs780_audit_request_linkstatus(struct rs780_audit *);
bool rs780_audit_retry_record(struct rs780_audit *);

/* Operator attestation of the current-session collector record; never call
 * automatically from emit/RX. Validate target source before invoking. */
bool rs780_audit_confirm_collector(struct rs780_audit *, uint64_t session,
                                  uint32_t id, enum rs780_phase,
                                  const char *collector_record);
bool rs780_audit_ack_intent(struct rs780_audit *, uint32_t id);
bool rs780_audit_ack_result(struct rs780_audit *, uint32_t id);
bool rs780_audit_abort(struct rs780_audit *);
/* Finish only the six selector/LTSSM steps; full link preflight stays incomplete. */
bool rs780_audit_finish(struct rs780_audit *);
bool rs780_audit_preflight(struct rs780_audit *);

#endif
