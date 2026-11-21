#ifndef BOOK8_APIC_H
#define BOOK8_APIC_H

#include <stdbool.h>
#include <stdint.h>

bool book8_apic_initialize(uint64_t physical_base);
bool book8_apic_initialize_local(void);
void book8_apic_enable_legacy_pic(void);
uint32_t book8_apic_current_id(void);
void book8_apic_eoi(void);
bool book8_apic_send_ipi(uint32_t apic_id, uint32_t command);
bool book8_apic_start_cpu(uint32_t apic_id, uint8_t startup_vector,
    uint64_t tsc_hz);
uint32_t book8_apic_calibrate_timer(uint64_t tsc_hz);
void book8_apic_timer_start(uint32_t initial_count, uint8_t vector);

#endif
