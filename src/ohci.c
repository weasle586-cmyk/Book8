#include "ohci.h"
#include "../drivers/pcie.h"

#include <stddef.h>

#ifdef BOOK8_USB_DRIVER_MODULE
extern uint32_t b8_hal_pci_read32(uint8_t, uint8_t, uint8_t, uint8_t);
extern void b8_hal_pci_write32(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
extern void b8_hal_pci_write16(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
extern int32_t b8_hal_dma_address(const void *, size_t, uint64_t *);
#define book8_pci_read32 b8_hal_pci_read32
#define book8_pci_write32 b8_hal_pci_write32
#define book8_pci_write16 b8_hal_pci_write16
#endif

#define OHCI_MAX_CONTROLLERS 8u
#define OHCI_MAX_HID 6u
#define OHCI_MAX_PORTS 15u
#define OHCI_CONTROL_TDS 4u
#define OHCI_CONTROL_BUFFER 256u
#define OHCI_HID_BUFFER 64u
#define OHCI_CONTROL_TIMEOUT_MS 100u

/* Register indices (32-bit words from the MMIO base). */
#define HC_CONTROL 0x04u
#define HC_COMMAND_STATUS 0x08u
#define HC_INTERRUPT_STATUS 0x0cu
#define HC_INTERRUPT_DISABLE 0x14u
#define HC_HCCA 0x18u
#define HC_CONTROL_HEAD_ED 0x20u
#define HC_CONTROL_CURRENT_ED 0x24u
#define HC_BULK_HEAD_ED 0x28u
#define HC_BULK_CURRENT_ED 0x2cu
#define HC_FM_INTERVAL 0x34u
#define HC_FM_NUMBER 0x3cu
#define HC_PERIODIC_START 0x40u
#define HC_LS_THRESHOLD 0x44u
#define HC_RH_DESCRIPTOR_A 0x48u
#define HC_RH_STATUS 0x50u
#define HC_RH_PORT_STATUS 0x54u

#define CONTROL_HCFS_MASK (3u << 6u)
#define CONTROL_HCFS_OPERATIONAL (2u << 6u)
#define CONTROL_PLE (1u << 2u)
#define CONTROL_CLE (1u << 4u)
#define CONTROL_IR (1u << 8u)
#define CONTROL_CBSR_3 3u
#define CMD_HCR (1u << 0u)
#define CMD_CLF (1u << 1u)
#define CMD_OCR (1u << 3u)
#define INT_WDH (1u << 1u)

#define PORT_CCS (1u << 0u)
#define PORT_PES (1u << 1u)
#define PORT_PRS (1u << 4u)
#define PORT_PPS (1u << 8u)
#define PORT_LSDA (1u << 9u)
#define PORT_CSC (1u << 16u)
#define PORT_PESC (1u << 17u)
#define PORT_PSSC (1u << 18u)
#define PORT_OCIC (1u << 19u)
#define PORT_PRSC (1u << 20u)
#define PORT_CHANGE_MASK (PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC)

#define ED_SKIP (1u << 14u)
#define ED_LOW_SPEED (1u << 13u)
#define ED_HALTED 1u
#define TD_CC_SHIFT 28u
#define TD_CC_NOT_ACCESSED 0xfu
#define TD_DI_NONE (7u << 21u)
#define TD_ROUNDING (1u << 18u)
#define TD_DP_SETUP (0u << 19u)
#define TD_DP_OUT (1u << 19u)
#define TD_DP_IN (2u << 19u)
#define TD_TOGGLE_DATA0 (2u << 24u)
#define TD_TOGGLE_DATA1 (3u << 24u)

struct ohci_ed {
    volatile uint32_t flags;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t next;
} __attribute__((aligned(16)));

struct ohci_td {
    volatile uint32_t flags;
    volatile uint32_t buffer;
    volatile uint32_t next;
    volatile uint32_t buffer_end;
} __attribute__((aligned(16)));

struct ohci_hcca {
    volatile uint32_t interrupt_table[32];
    volatile uint16_t frame_number;
    volatile uint16_t pad;
    volatile uint32_t done_head;
    uint8_t reserved[116];
} __attribute__((aligned(256)));

_Static_assert(sizeof(struct ohci_ed) == 16u, "OHCI ED is 16 bytes");
_Static_assert(sizeof(struct ohci_td) == 16u, "OHCI TD is 16 bytes");
_Static_assert(sizeof(struct ohci_hcca) == 256u, "HCCA is 256 bytes");

struct ohci_controller {
    volatile uint32_t *regs;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t port_count;
    uint8_t next_address;
    bool started;
    uint32_t first_hid_ed; /* head of the periodic ED chain (physical) */
};

enum hid_kind { HID_MOUSE, HID_KEYBOARD };

struct ohci_hid {
    bool active;
    enum hid_kind kind;
    uint8_t controller;
    uint8_t max_packet;
    uint8_t current_td;
};

struct hid_found {
    bool found;
    uint8_t interface_number;
    uint8_t endpoint;
    uint8_t max_packet;
};

static struct ohci_controller controllers[OHCI_MAX_CONTROLLERS];
static uint32_t controller_count;
static struct ohci_hcca hcca[OHCI_MAX_CONTROLLERS];
static struct ohci_ed control_eds[OHCI_MAX_CONTROLLERS];
static struct ohci_td control_tds[OHCI_MAX_CONTROLLERS][OHCI_CONTROL_TDS];
static uint8_t control_buffer[OHCI_CONTROL_BUFFER] __attribute__((aligned(256)));
static uint8_t setup_buffer[8] __attribute__((aligned(16)));

static struct ohci_hid hids[OHCI_MAX_HID];
static uint32_t hid_count;
static struct ohci_ed hid_eds[OHCI_MAX_HID];
static struct ohci_td hid_tds[OHCI_MAX_HID][2];
static uint8_t hid_buffers[OHCI_MAX_HID][OHCI_HID_BUFFER]
    __attribute__((aligned(16)));
static struct book8_usb_diagnostics *diag;

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset)
{
    return book8_pci_read32(bus, slot, function, offset);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t function,
    uint8_t offset, uint32_t value)
{
    book8_pci_write32(bus, slot, function, offset, value);
}

static uint32_t phys(const volatile void *pointer)
{
    uint64_t physical;
    if (b8_hal_dma_address((const void *)pointer, 1u, &physical) == 0
        || physical > UINT32_MAX) {
        if (diag != NULL)
            ++diag->ohci_errors;
        return 0u;
    }
    return (uint32_t)physical;
}

/* Frame-counter based delay (1 ms per frame once the HC is operational),
 * with a spin cap so a dead controller cannot hang the boot. */
static void wait_ms(struct ohci_controller *c, uint32_t ms)
{
    uint32_t start = c->regs[HC_FM_NUMBER / 4u] & 0xffffu;

    for (uint32_t spins = 0u; spins < ms * 400000u; ++spins) {
        uint32_t now = c->regs[HC_FM_NUMBER / 4u] & 0xffffu;
        if (((now - start) & 0xffffu) >= ms) {
            return;
        }
        __asm__ volatile ("pause");
    }
}

static void stop_controller(struct ohci_controller *c)
{
    uint32_t identity, bar, command;

    if (!c->started || c->regs == NULL) {
        return;
    }
    identity = pci_read32(c->bus, c->slot, c->function, 0u);
    bar = pci_read32(c->bus, c->slot, c->function, 0x10u);
    if (identity == UINT32_MAX) {
        c->started = false;
        return;
    }
    if (bar != UINT32_MAX
        && (bar & ~0x0fu) == (uint32_t)(uintptr_t)c->regs) {
        c->regs[HC_INTERRUPT_DISABLE / 4u] = UINT32_MAX;
        c->regs[HC_CONTROL / 4u] = 0u;
    }
    command = pci_read32(c->bus, c->slot, c->function, 4u);
    if (command != UINT32_MAX) {
        book8_pci_write16(c->bus, c->slot, c->function, 4u,
            (uint16_t)(command & ~(1u << 2u)));
    }
    c->started = false;
}

void book8_ohci_reset(void)
{
    for (uint32_t i = 0u; i < controller_count; ++i) {
        stop_controller(&controllers[i]);
    }
    controller_count = 0u;
    hid_count = 0u;
    for (uint32_t i = 0u; i < OHCI_MAX_HID; ++i) {
        hids[i].active = false;
    }
}

void book8_ohci_add(uint8_t bus, uint8_t slot, uint8_t function)
{
    if (controller_count >= OHCI_MAX_CONTROLLERS) {
        return;
    }
    controllers[controller_count++] = (struct ohci_controller){
        .bus = bus, .slot = slot, .function = function
    };
}

static void td_fill(struct ohci_td *td, uint32_t flags, const void *buffer,
    uint32_t length, uint32_t next)
{
    td->flags = flags | (TD_CC_NOT_ACCESSED << TD_CC_SHIFT) | TD_DI_NONE;
    td->buffer = length != 0u ? phys(buffer) : 0u;
    td->buffer_end = length != 0u ? phys(buffer) + length - 1u : 0u;
    td->next = next;
}

static bool td_failed(const struct ohci_td *td)
{
    uint32_t cc = td->flags >> TD_CC_SHIFT;
    return cc != 0u && cc != TD_CC_NOT_ACCESSED;
}

static void acknowledge_done(struct ohci_controller *c)
{
    if ((c->regs[HC_INTERRUPT_STATUS / 4u] & INT_WDH) != 0u) {
        c->regs[HC_INTERRUPT_STATUS / 4u] = INT_WDH;
    }
}

/* One control transfer on the default/assigned pipe; returns bytes moved in
 * the data stage through *actual. */
static bool control_once(uint32_t index, bool low_speed, uint8_t address,
    uint8_t max_packet, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t wIndex, void *data, uint16_t length,
    uint16_t *actual)
{
    struct ohci_controller *c = &controllers[index];
    struct ohci_ed *ed = &control_eds[index];
    struct ohci_td *td = control_tds[index];
    bool in = (request_type & 0x80u) != 0u;
    uint32_t count = 0u;
    uint32_t data_index = 0u;
    uint32_t status_index;

    if (length > OHCI_CONTROL_BUFFER) {
        return false;
    }
    setup_buffer[0] = request_type;
    setup_buffer[1] = request;
    setup_buffer[2] = (uint8_t)value;
    setup_buffer[3] = (uint8_t)(value >> 8u);
    setup_buffer[4] = (uint8_t)wIndex;
    setup_buffer[5] = (uint8_t)(wIndex >> 8u);
    setup_buffer[6] = (uint8_t)length;
    setup_buffer[7] = (uint8_t)(length >> 8u);

    /* Quiesce the control list so the HC cannot write a stale HeadP back into
     * the ED while it is rebuilt (spec 5.2.7.1): stop the list, wait past an
     * SOF, then clear the cached current-ED pointer. */
    c->regs[HC_CONTROL / 4u] &= ~CONTROL_CLE;
    ed->flags |= ED_SKIP;
    wait_ms(c, 2u);
    acknowledge_done(c);
    c->regs[HC_CONTROL_CURRENT_ED / 4u] = 0u;
    td_fill(&td[count], TD_DP_SETUP | TD_TOGGLE_DATA0, setup_buffer, 8u,
        phys(&td[count + 1u]));
    ++count;
    if (length != 0u) {
        data_index = count;
        td_fill(&td[count], (in ? TD_DP_IN | TD_ROUNDING : TD_DP_OUT)
            | TD_TOGGLE_DATA1, control_buffer, length,
            phys(&td[count + 1u]));
        ++count;
    }
    status_index = count;
    td_fill(&td[count], ((in && length != 0u) ? TD_DP_OUT : TD_DP_IN)
        | TD_TOGGLE_DATA1, NULL, 0u, phys(&td[count + 1u]));
    ++count;
    td[count].flags = 0u;
    td[count].buffer = 0u;
    td[count].buffer_end = 0u;
    td[count].next = 0u;

    ed->flags = (uint32_t)address | ((uint32_t)max_packet << 16u)
        | (low_speed ? ED_LOW_SPEED : 0u) | ED_SKIP;
    ed->tail = phys(&td[count]);
    ed->head = phys(&td[0]);
    ed->flags &= ~ED_SKIP;
    c->regs[HC_CONTROL / 4u] |= CONTROL_CLE;
    c->regs[HC_COMMAND_STATUS / 4u] = CMD_CLF;

    /* Frame-based deadline (USB allows up to 50 ms for a status stage and
     * devices may NAK meanwhile); the spin cap only guards a stopped HC. */
    bool done = false;
    uint32_t start_frame = c->regs[HC_FM_NUMBER / 4u] & 0xffffu;
    for (uint32_t spins = 0u; spins < 200000000u; ++spins) {
        acknowledge_done(c);
        if (td_failed(&td[0]) || (length != 0u && td_failed(&td[data_index]))
            || td_failed(&td[status_index]) || (ed->head & ED_HALTED) != 0u) {
            break;
        }
        if ((td[status_index].flags >> TD_CC_SHIFT) == 0u) {
            done = true;
            break;
        }
        if (((c->regs[HC_FM_NUMBER / 4u] - start_frame) & 0xffffu)
            > OHCI_CONTROL_TIMEOUT_MS) {
            break;
        }
        __asm__ volatile ("pause");
    }
    ed->flags |= ED_SKIP;
    if (!done) {
        uint32_t cc = TD_CC_NOT_ACCESSED;
        uint32_t stage = count;
        for (uint32_t i = 0u; i < count; ++i) {
            uint32_t value = td[i].flags >> TD_CC_SHIFT;
            if (value != 0u) {
                cc = value;
                stage = i;
                break;
            }
        }
        diag->ohci_last_cc = cc;
        diag->ohci_last_stage = stage;
        return false;
    }
    if (length != 0u && in) {
        uint32_t cbp = td[data_index].buffer;
        uint16_t moved = cbp == 0u ? length
            : (uint16_t)(cbp - phys(control_buffer));
        for (uint16_t i = 0u; i < moved; ++i) {
            ((uint8_t *)data)[i] = control_buffer[i];
        }
        if (actual != NULL) {
            *actual = moved;
        }
    } else if (actual != NULL) {
        *actual = length;
    }
    return true;
}

static bool control_transfer(uint32_t index, bool low_speed, uint8_t address,
    uint8_t max_packet, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t wIndex, void *data, uint16_t length,
    uint16_t *actual)
{
    /* Real devices may NAK or stall briefly after SET_ADDRESS/CONFIGURATION.
     * SET_ADDRESS is retried by the caller, which can probe the new address. */
    uint32_t attempts = request == 5u ? 1u : 3u;

    for (uint32_t attempt = 0u; attempt < attempts; ++attempt) {
        if (control_once(index, low_speed, address, max_packet, request_type,
                request, value, wIndex, data, length, actual)) {
            return true;
        }
        if (attempt + 1u < attempts) {
            ++diag->ohci_control_retries;
            wait_ms(&controllers[index], 20u);
        }
    }
    return false;
}

static bool control_in(uint32_t index, bool low, uint8_t address,
    uint8_t mps, uint16_t value, uint16_t length, uint16_t *actual)
{
    return control_transfer(index, low, address, mps, 0x80u, 6u, value, 0u,
        control_buffer, length, actual);
}

static volatile uint32_t *port_register(struct ohci_controller *c,
    uint32_t port)
{
    return &c->regs[HC_RH_PORT_STATUS / 4u + port];
}

static bool reset_port(struct ohci_controller *c, uint32_t port, bool *low)
{
    volatile uint32_t *reg = port_register(c, port);

    *reg = PORT_PRS;
    /* The HC clears PRS and raises PRSC when the reset pulse has finished. */
    for (uint32_t i = 0u; i < 150u; ++i) {
        if ((*reg & PORT_PRS) == 0u && (*reg & PORT_PRSC) != 0u) {
            break;
        }
        wait_ms(c, 1u);
    }
    if ((*reg & PORT_PRS) != 0u || (*reg & PORT_PRSC) == 0u) {
        return false;
    }
    *reg = PORT_PRSC | PORT_CSC;
    /* TRSTRCY: 10 ms reset recovery before the first transaction. */
    wait_ms(c, 10u);
    for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
        for (uint32_t i = 0u; i < 20u; ++i) {
            if ((*reg & PORT_PES) != 0u) {
                break;
            }
            wait_ms(c, 1u);
        }
        if ((*reg & PORT_PES) != 0u) {
            break;
        }
        *reg = PORT_PES;
    }
    if ((*reg & (PORT_CCS | PORT_PES)) != (PORT_CCS | PORT_PES)) {
        return false;
    }
    wait_ms(c, 10u);
    *low = (*reg & PORT_LSDA) != 0u;
    return true;
}

static void find_hid(const uint8_t *cfg, uint32_t length,
    struct hid_found *mouse, struct hid_found *keyboard)
{
    struct hid_found *current = NULL;

    for (uint32_t offset = 0u; offset + 2u <= length;) {
        uint8_t size = cfg[offset];
        uint8_t type = cfg[offset + 1u];

        if (size < 2u || size > length - offset) {
            return;
        }
        if (type == 4u && size >= 9u) {
            current = NULL;
            if (cfg[offset + 5u] == 3u && cfg[offset + 6u] == 1u
                && cfg[offset + 3u] == 0u) {
                if (cfg[offset + 7u] == 2u && !mouse->found) {
                    current = mouse;
                } else if (cfg[offset + 7u] == 1u && !keyboard->found) {
                    current = keyboard;
                }
                if (current != NULL) {
                    current->interface_number = cfg[offset + 2u];
                }
            }
        } else if (type == 5u && size >= 7u && current != NULL
            && !current->found && (cfg[offset + 2u] & 0x80u) != 0u
            && (cfg[offset + 3u] & 3u) == 3u) {
            uint16_t packet = (uint16_t)(cfg[offset + 4u]
                | ((uint16_t)cfg[offset + 5u] << 8u)) & 0x7ffu;
            if (packet >= 3u && packet <= OHCI_HID_BUFFER) {
                current->endpoint = cfg[offset + 2u] & 0x0fu;
                current->max_packet = (uint8_t)packet;
                current->found = true;
            }
        }
        offset += size;
    }
}

static void arm_hid_td(uint32_t slot, uint32_t td_index, uint32_t next)
{
    struct ohci_td *td = &hid_tds[slot][td_index];

    td_fill(td, TD_DP_IN | TD_ROUNDING, hid_buffers[slot],
        hids[slot].max_packet, next);
}

static bool add_hid(uint32_t controller, bool low, uint8_t address,
    uint8_t endpoint, uint8_t max_packet, enum hid_kind kind)
{
    struct ohci_controller *c = &controllers[controller];
    uint32_t slot = hid_count;
    struct ohci_ed *ed;

    if (slot >= OHCI_MAX_HID) {
        return false;
    }
    hids[slot] = (struct ohci_hid){
        .kind = kind, .controller = (uint8_t)controller,
        .max_packet = max_packet, .current_td = 0u
    };
    ed = &hid_eds[slot];
    arm_hid_td(slot, 0u, phys(&hid_tds[slot][1]));
    hid_tds[slot][1].flags = 0u;
    hid_tds[slot][1].buffer = 0u;
    hid_tds[slot][1].buffer_end = 0u;
    hid_tds[slot][1].next = 0u;
    ed->flags = (uint32_t)address | ((uint32_t)endpoint << 7u)
        | ((uint32_t)max_packet << 16u) | (low ? ED_LOW_SPEED : 0u);
    ed->tail = phys(&hid_tds[slot][1]);
    ed->head = phys(&hid_tds[slot][0]);
    ed->next = c->first_hid_ed;
    c->first_hid_ed = phys(ed);
    for (uint32_t i = 0u; i < 32u; ++i) {
        hcca[controller].interrupt_table[i] = c->first_hid_ed;
    }
    hids[slot].active = true;
    ++hid_count;
    return true;
}

static void enumerate_port(uint32_t index, uint32_t port)
{
    struct ohci_controller *c = &controllers[index];
    bool low = false;
    uint16_t actual = 0u;
    uint8_t address;
    uint8_t mps;
    uint16_t vendor_id, product_id;
    uint16_t total;
    struct hid_found mouse = {0};
    struct hid_found keyboard = {0};

    ++diag->connected_ports;
    if (!reset_port(c, port, &low)) {
        ++diag->ohci_reset_failures;
        return;
    }
    ++diag->reset_ports;
    if (!control_in(index, low, 0u, 8u, 0x0100u, 8u, &actual) || actual < 8u) {
        ++diag->device_descriptor_failures;
        return;
    }
    mps = control_buffer[7];
    if (mps != 8u && mps != 16u && mps != 32u && mps != 64u) {
        ++diag->device_descriptor_failures;
        return;
    }
    if (control_buffer[4] == 9u) {
        if (control_in(index, low, 0u, mps, 0x0100u, 18u, &actual)
            && actual >= 18u) {
            vendor_id = (uint16_t)(control_buffer[8]
                | ((uint16_t)control_buffer[9] << 8u));
            product_id = (uint16_t)(control_buffer[10]
                | ((uint16_t)control_buffer[11] << 8u));
            (void)book8_usb_bind_interface(vendor_id, product_id, 9u,
                control_buffer[5], control_buffer[6], 0u, 0xffu);
        }
        ++diag->ohci_hub_devices;
        return;
    }
    address = c->next_address++;
    bool addressed = false;
    for (uint32_t attempt = 0u; attempt < 3u && !addressed; ++attempt) {
        if (control_transfer(index, low, 0u, mps, 0x00u, 5u, address, 0u,
                NULL, 0u, NULL)) {
            addressed = true;
        } else {
            /* The device may have switched address even though the status
             * stage was missed; probe the new address before retrying. */
            wait_ms(c, 10u);
            if (control_in(index, low, address, mps, 0x0100u, 8u, &actual)
                && actual >= 8u) {
                addressed = true;
            } else {
                ++diag->ohci_control_retries;
            }
        }
    }
    if (!addressed) {
        ++diag->ohci_control_failures;
        return;
    }
    ++diag->addressed_devices;
    wait_ms(c, 10u);
    if (!control_in(index, low, address, mps, 0x0100u, 18u, &actual)
        || actual < 18u) {
        ++diag->device_descriptor_failures;
        return;
    }
    vendor_id = (uint16_t)(control_buffer[8]
        | ((uint16_t)control_buffer[9] << 8u));
    product_id = (uint16_t)(control_buffer[10]
        | ((uint16_t)control_buffer[11] << 8u));
    if (control_buffer[4] != 0u) {
        (void)book8_usb_bind_interface(vendor_id, product_id,
            control_buffer[4], control_buffer[5], control_buffer[6],
            address, 0xffu);
    }
    if (!control_in(index, low, address, mps, 0x0200u, 9u, &actual)
        || actual < 9u || control_buffer[1] != 2u) {
        ++diag->configuration_descriptor_failures;
        return;
    }
    total = (uint16_t)(control_buffer[2] | ((uint16_t)control_buffer[3] << 8u));
    if (total < 9u) {
        ++diag->configuration_descriptor_failures;
        return;
    }
    if (total > OHCI_CONTROL_BUFFER) {
        total = OHCI_CONTROL_BUFFER;
    }
    uint8_t configuration = control_buffer[5];
    if (!control_in(index, low, address, mps, 0x0200u, total, &actual)
        || actual < total) {
        ++diag->configuration_descriptor_failures;
        return;
    }
    book8_usb_bind_configuration_interfaces(vendor_id, product_id,
        control_buffer, actual, address);
    find_hid(control_buffer, total, &mouse, &keyboard);
    if (!mouse.found && !keyboard.found) {
        ++diag->non_boot_hid_devices;
        return;
    }
    ++diag->boot_hid_interfaces;
    if (!control_transfer(index, low, address, mps, 0x00u, 9u, configuration,
            0u, NULL, 0u, NULL)) {
        ++diag->hid_control_failures;
        return;
    }
    wait_ms(c, 10u);
    struct hid_found *found[2] = {&mouse, &keyboard};
    for (uint32_t i = 0u; i < 2u; ++i) {
        if (!found[i]->found) {
            continue;
        }
        if (book8_usb_bind_interface(vendor_id, product_id, 3u, 1u,
                i == 0u ? 2u : 1u, address,
                found[i]->interface_number) == NULL) {
            continue;
        }
        /* Boot protocol (0) and infinite idle; stalls are tolerated. */
        (void)control_transfer(index, low, address, mps, 0x21u, 0x0bu, 0u,
            found[i]->interface_number, NULL, 0u, NULL);
        (void)control_transfer(index, low, address, mps, 0x21u, 0x0au, 0u,
            found[i]->interface_number, NULL, 0u, NULL);
        if (add_hid(index, low, address, found[i]->endpoint,
                found[i]->max_packet, i == 0u ? HID_MOUSE : HID_KEYBOARD)) {
            ++diag->ohci_hid_devices;
        }
    }
}

static bool take_over(struct ohci_controller *c)
{
    volatile uint32_t *r = c->regs;
    uint32_t fit;
    uint32_t fi;

    if ((r[HC_CONTROL / 4u] & CONTROL_IR) != 0u) {
        r[HC_COMMAND_STATUS / 4u] = CMD_OCR;
        for (uint32_t i = 0u; i < 2000000u
            && (r[HC_CONTROL / 4u] & CONTROL_IR) != 0u; ++i) {
            __asm__ volatile ("pause");
        }
        if ((r[HC_CONTROL / 4u] & CONTROL_IR) != 0u) {
            ++diag->ohci_ownership_failures;
        }
    }
    r[HC_INTERRUPT_DISABLE / 4u] = 0xc000007fu;
    fit = (r[HC_FM_INTERVAL / 4u] & 0x80000000u) ^ 0x80000000u;
    r[HC_COMMAND_STATUS / 4u] = CMD_HCR;
    for (uint32_t i = 0u; i < 1000000u
        && (r[HC_COMMAND_STATUS / 4u] & CMD_HCR) != 0u; ++i) {
        __asm__ volatile ("pause");
    }
    if ((r[HC_COMMAND_STATUS / 4u] & CMD_HCR) != 0u) {
        return false;
    }
    /* Reset leaves the HC in USBSUSPEND; it must be operational within 2 ms. */
    fi = 0x2edfu;
    c->first_hid_ed = 0u;
    r[HC_INTERRUPT_DISABLE / 4u] = 0xc000007fu;
    r[HC_HCCA / 4u] = phys(&hcca[c - controllers]);
    r[HC_CONTROL_HEAD_ED / 4u] = phys(&control_eds[c - controllers]);
    r[HC_CONTROL_CURRENT_ED / 4u] = 0u;
    r[HC_BULK_HEAD_ED / 4u] = 0u;
    r[HC_BULK_CURRENT_ED / 4u] = 0u;
    r[HC_FM_INTERVAL / 4u] = fi | ((((fi - 210u) * 6u) / 7u) << 16u) | fit;
    r[HC_PERIODIC_START / 4u] = (fi * 9u) / 10u;
    r[HC_LS_THRESHOLD / 4u] = 0x628u;
    r[HC_CONTROL / 4u] = CONTROL_CBSR_3 | CONTROL_PLE | CONTROL_CLE
        | CONTROL_HCFS_OPERATIONAL;
    return (r[HC_CONTROL / 4u] & CONTROL_HCFS_MASK) == CONTROL_HCFS_OPERATIONAL;
}

static void start_controller(uint32_t index)
{
    struct ohci_controller *c = &controllers[index];
    uint32_t bar = pci_read32(c->bus, c->slot, c->function, 0x10u);
    uint32_t command;
    uint32_t descriptor_a;

    if ((bar & 1u) != 0u || (bar & ~0xfu) == 0u || (bar & 6u) == 4u) {
        ++diag->pci_enable_failures;
        return;
    }
    command = pci_read32(c->bus, c->slot, c->function, 0x04u) & 0xffffu;
    pci_write32(c->bus, c->slot, c->function, 0x04u, command | 6u);
    if ((pci_read32(c->bus, c->slot, c->function, 0x04u) & 6u) != 6u) {
        ++diag->pci_enable_failures;
        return;
    }
    c->regs = (volatile uint32_t *)(uintptr_t)(bar & ~0xfu);
    if ((c->regs[0] & 0xffu) == 0xffu) {
        c->regs = NULL;
        ++diag->pci_enable_failures;
        return;
    }
    for (uint32_t i = 0u; i < 32u; ++i) {
        hcca[index].interrupt_table[i] = 0u;
    }
    hcca[index].done_head = 0u;
    control_eds[index].flags = ED_SKIP;
    control_eds[index].tail = 0u;
    control_eds[index].head = 0u;
    control_eds[index].next = 0u;
    c->next_address = 1u;
    if (!take_over(c)) {
        ++diag->ohci_reset_failures;
        return;
    }
    c->started = true;
    ++diag->ohci_started;

    descriptor_a = c->regs[HC_RH_DESCRIPTOR_A / 4u];
    c->port_count = (uint8_t)(descriptor_a & 0xffu);
    if (c->port_count > OHCI_MAX_PORTS) {
        c->port_count = OHCI_MAX_PORTS;
    }
    diag->ohci_ports += c->port_count;
    c->regs[HC_RH_STATUS / 4u] = 1u << 16u;
    for (uint32_t port = 0u; port < c->port_count; ++port) {
        if ((*port_register(c, port) & PORT_PPS) == 0u) {
            *port_register(c, port) = PORT_PPS;
        }
    }
    /* Power-on-to-power-good, then the USB 100 ms attach debounce. */
    wait_ms(c, ((descriptor_a >> 24u) & 0xffu) * 2u + 100u);
    for (uint32_t port = 0u; port < c->port_count; ++port) {
        volatile uint32_t *reg = port_register(c, port);

        *reg = PORT_CHANGE_MASK;
        if ((*reg & PORT_CCS) != 0u) {
            ++diag->ohci_connected_ports;
            enumerate_port(index, port);
        }
    }
}

void book8_ohci_start(struct book8_usb_diagnostics *diagnostics)
{
    diag = diagnostics;
    for (uint32_t i = 0u; i < controller_count; ++i) {
        start_controller(i);
    }
    diag->active_hid_devices += book8_ohci_active_devices();
}

uint32_t book8_ohci_active_devices(void)
{
    uint32_t count = 0u;

    for (uint32_t i = 0u; i < hid_count; ++i) {
        count += hids[i].active ? 1u : 0u;
    }
    return count;
}

bool book8_ohci_mouse_active(void)
{
    for (uint32_t i = 0u; i < hid_count; ++i) {
        if (hids[i].active && hids[i].kind == HID_MOUSE) {
            return true;
        }
    }
    return false;
}

bool book8_ohci_keyboard_active(void)
{
    for (uint32_t i = 0u; i < hid_count; ++i) {
        if (hids[i].active && hids[i].kind == HID_KEYBOARD) {
            return true;
        }
    }
    return false;
}

/* Returns true when a completed report of `kind` was copied to `report`, and
 * re-arms the endpoint either way. */
static bool poll_hid(uint32_t slot, uint8_t *report, uint32_t *length)
{
    struct ohci_hid *hid = &hids[slot];
    struct ohci_ed *ed = &hid_eds[slot];
    struct ohci_td *current = &hid_tds[slot][hid->current_td];
    struct ohci_td *other = &hid_tds[slot][hid->current_td ^ 1u];
    uint32_t cc = current->flags >> TD_CC_SHIFT;
    bool have = false;

    acknowledge_done(&controllers[hid->controller]);
    if (cc == TD_CC_NOT_ACCESSED && (ed->head & ED_HALTED) == 0u) {
        return false;
    }
    if (cc == 0u) {
        uint32_t cbp = current->buffer;
        uint32_t got = cbp == 0u ? hid->max_packet
            : cbp - phys(hid_buffers[slot]);

        for (uint32_t i = 0u; i < got && i < OHCI_HID_BUFFER; ++i) {
            report[i] = hid_buffers[slot][i];
        }
        *length = got;
        have = true;
        ++diag->ohci_reports;
        arm_hid_td(slot, hid->current_td ^ 1u, phys(current));
        ed->tail = phys(current);
        hid->current_td ^= 1u;
    } else {
        /* Error or halted endpoint: rebuild the pair and clear the halt. */
        ++diag->ohci_errors;
        arm_hid_td(slot, hid->current_td, phys(other));
        other->flags = 0u;
        other->buffer = 0u;
        other->buffer_end = 0u;
        other->next = 0u;
        ed->tail = phys(other);
        ed->head = phys(current);
    }
    return have;
}

bool book8_ohci_poll_mouse(struct book8_usb_mouse_report *report)
{
    uint8_t data[OHCI_HID_BUFFER];
    uint32_t length = 0u;

    for (uint32_t i = 0u; report != NULL && i < hid_count; ++i) {
        if (!hids[i].active || hids[i].kind != HID_MOUSE
            || !poll_hid(i, data, &length) || length < 3u) {
            continue;
        }
        report->buttons = data[0] & 0x07u;
        report->x = (int8_t)data[1];
        report->y = (int8_t)data[2];
        report->wheel_delta = length >= 4u ? book8_wheel_detents((int8_t)data[3]) : 0;
        report->absolute = false;
        report->logical_min_x = report->logical_min_y = -127;
        report->logical_max_x = report->logical_max_y = 127;
        return true;
    }
    return false;
}

bool book8_ohci_poll_keyboard(struct book8_usb_keyboard_report *report)
{
    uint8_t data[OHCI_HID_BUFFER];
    uint32_t length = 0u;

    for (uint32_t i = 0u; report != NULL && i < hid_count; ++i) {
        if (!hids[i].active || hids[i].kind != HID_KEYBOARD
            || !poll_hid(i, data, &length) || length < 8u) {
            continue;
        }
        report->modifiers = data[0];
        for (uint32_t key = 0u; key < 6u; ++key) {
            report->keys[key] = data[2u + key];
        }
        return true;
    }
    return false;
}
