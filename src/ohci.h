#ifndef BOOK8_OHCI_H
#define BOOK8_OHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "usb.h"

/* OHCI (USB 1.1) host controller driver for boot-protocol HID mice and
 * keyboards. Polled, no interrupts. It owns the full/low-speed root ports that
 * EHCI hands to its companion controllers (AMD SB7xx, many Intel/NVIDIA
 * chipsets) and the standalone OHCI functions. */

void book8_ohci_reset(void);
void book8_ohci_add(uint8_t bus, uint8_t slot, uint8_t function);
/* Takes over every added controller, enumerates root ports and counts results
 * in the shared diagnostics. Call after the EHCI handoff/port routing. */
void book8_ohci_start(struct book8_usb_diagnostics *diagnostics);
bool book8_ohci_mouse_active(void);
bool book8_ohci_keyboard_active(void);
uint32_t book8_ohci_active_devices(void);
bool book8_ohci_poll_mouse(struct book8_usb_mouse_report *report);
bool book8_ohci_poll_keyboard(struct book8_usb_keyboard_report *report);

#endif
