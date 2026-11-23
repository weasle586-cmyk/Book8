#include "kernel_internal.h"

/* AP bootstrap, interrupt table and SMP coordination. */

__attribute__((noreturn)) void smp_ap_relaunch_halt(void)
{
    uintptr_t offset = (uintptr_t)book8_ap_relaunch_halt
        - (uintptr_t)book8_ap_trampoline_start;
    void (*halt)(void) = (void (*)(void))(uintptr_t)(SMP_TRAMPOLINE_PHYS + offset);
    halt();
    __builtin_unreachable();
}

inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void idt_set_gate(uint8_t vector, void (*handler)(void))
{
    uintptr_t address = (uintptr_t)handler;
    struct idt_entry *entry = &interrupt_table[vector];

    entry->offset_low = (uint16_t)address;
    entry->selector = 0x08u;
    entry->ist = 0u;
    entry->attributes = 0x8eu;
    entry->offset_middle = (uint16_t)(address >> 16u);
    entry->offset_high = (uint32_t)(address >> 32u);
    entry->reserved = 0u;
}

void interrupt_table_load(void)
{
    struct idt_pointer pointer = {
        .limit = (uint16_t)(sizeof(interrupt_table) - 1u),
        .address = (uintptr_t)interrupt_table,
    };
    __asm__ volatile ("lidt %0" : : "m"(pointer) : "memory");
}

void interrupt_table_initialize(void)
{
    if (interrupt_table_ready) {
        interrupt_table_load();
        return;
    }
    for (uint32_t vector = 0u; vector < 256u; ++vector) {
        idt_set_gate((uint8_t)vector, book8_unhandled_interrupt);
    }
    idt_set_gate(0x20u, book8_timer_irq_entry);
    idt_set_gate(0x2cu, book8_ps2_mouse_irq_entry);
    idt_set_gate(SMP_APIC_TIMER_VECTOR, book8_apic_timer_entry);
    idt_set_gate(SMP_SCHEDULE_IPI_VECTOR, book8_sched_ipi_entry);
    idt_set_gate(13u, book8_gp_entry);
    idt_set_gate(14u, book8_pf_entry);

    outb(0x20u, 0x11u);
    outb(0x80u, 0u);
    outb(0xa0u, 0x11u);
    outb(0x80u, 0u);
    outb(0x21u, 0x20u);
    outb(0xa1u, 0x28u);
    outb(0x21u, 0x04u);
    outb(0xa1u, 0x02u);
    outb(0x21u, 0x01u);
    outb(0xa1u, 0x01u);
    outb(0x21u, 0xfeu);
    outb(0xa1u, 0xffu);
    interrupt_table_ready = true;
    interrupt_table_load();
}

struct timer_frame *book8_timer_tick(struct timer_frame *frame)
{
    book8_apic_eoi();
    ++timer_irq_count;
    if (book8_smp_current_cpu_index() == 0u && exec_user_scheduler_active())
        return exec_timer_tick(frame);
    return sched_timer_tick(frame);
}

uint32_t book8_smp_current_cpu_index(void)
{
    uint32_t apic_id = book8_apic_current_id();
    if (apic_id == UINT32_MAX)
        return 0u;
    for (uint32_t i = 0u; i < smp_topology.cpu_count; ++i) {
        if (smp_topology.apic_ids[i] == apic_id)
            return i;
    }
    return 0u;
}

void book8_sched_request_reschedule(uint32_t cpu_id)
{
    uint32_t current_cpu = book8_smp_current_cpu_index();
    if (cpu_id == current_cpu) {
        __asm__ volatile ("int $0x41" : : : "memory");
    } else if (local_apic_ready && cpu_id < smp_topology.cpu_count) {
        (void)book8_apic_send_ipi(smp_topology.apic_ids[cpu_id],
            SMP_SCHEDULE_IPI_VECTOR);
    }
}

struct timer_frame *book8_apic_timer_tick(struct timer_frame *frame)
{
    book8_apic_eoi();
    uint32_t cpu = book8_smp_current_cpu_index();
    if (cpu != 0u && __atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_or_fetch(&smp_relaunch_halted_mask, 1ull << cpu, __ATOMIC_RELEASE);
        smp_ap_relaunch_halt();
    }
    if (cpu == 0u && exec_user_scheduler_active())
        return exec_timer_tick(frame);
    return sched_timer_tick_cpu(frame, cpu, false, false);
}

struct timer_frame *book8_sched_ipi_tick(struct timer_frame *frame)
{
    book8_apic_eoi();
    uint32_t cpu = book8_smp_current_cpu_index();
    if (cpu != 0u && __atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_or_fetch(&smp_relaunch_halted_mask, 1ull << cpu, __ATOMIC_RELEASE);
        smp_ap_relaunch_halt();
    }
    if (cpu == 0u && exec_user_scheduler_active())
        return exec_timer_tick(frame);
    return sched_timer_tick_cpu(frame, cpu, false, true);
}

void smp_ap_main(void *argument)
{
    struct smp_boot_state *state = argument;
    if (state == NULL || !book8_apic_initialize_local()
        || book8_apic_current_id() != state->apic_id) {
        if (state != NULL)
            __atomic_store_n(&state->status, 2u, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    /* Each AP owns its privilege stack and TSS; interrupts from any CPL need
     * a valid RSP0 on the CPU that took them. */
    syscall_cpu_init(state->cpu_index);
    interrupt_table_load();
    uint32_t timer_count = book8_apic_calibrate_timer(tsc_hz);
    if (timer_count == 0u || !sched_cpu_online(state->cpu_index)) {
        __atomic_store_n(&state->status, 2u, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    book8_apic_timer_start(timer_count, SMP_APIC_TIMER_VECTOR);
    __atomic_store_n(&state->status, 1u, __ATOMIC_RELEASE);
    for (;;) {
        if (__atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE) != 0u) {
            __atomic_or_fetch(&smp_relaunch_halted_mask,
                1ull << state->cpu_index, __ATOMIC_RELEASE);
            smp_ap_relaunch_halt();
        }
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}

bool smp_prepare_trampoline(void)
{
    uintptr_t source = (uintptr_t)book8_ap_trampoline_start;
    uintptr_t end = (uintptr_t)book8_ap_trampoline_end;
    uintptr_t data_offset = (uintptr_t)book8_ap_trampoline_data - source;
    if (!smp_trampoline_page_usable || end <= source
        || end - source > 4096u || data_offset + 4u * sizeof(uint64_t) > 4096u)
        return false;
    volatile uint8_t *destination =
        (volatile uint8_t *)(uintptr_t)SMP_TRAMPOLINE_PHYS;
    const uint8_t *bytes = (const uint8_t *)source;
    for (uintptr_t i = 0u; i < end - source; ++i)
        destination[i] = bytes[i];
    return true;
}

bool smp_start_application_processors(void)
{
    if (smp_topology.cpu_count <= 1u)
        return true;
    if (!smp_prepare_trampoline())
        return false;

    uint64_t data_offset = (uintptr_t)book8_ap_trampoline_data
        - (uintptr_t)book8_ap_trampoline_start;
    volatile uint64_t *startup_data = (volatile uint64_t *)(uintptr_t)
        (SMP_TRAMPOLINE_PHYS + data_offset);
    startup_data[0] = (uint64_t)(uintptr_t)boot_pml4;
    startup_data[3] = (uint64_t)(uintptr_t)smp_ap_main;

    for (uint32_t cpu = 1u; cpu < smp_topology.cpu_count; ++cpu) {
        struct smp_boot_state *state = &smp_boot_states[cpu];
        state->cpu_index = cpu;
        state->apic_id = smp_topology.apic_ids[cpu];
        __atomic_store_n(&state->status, 0u, __ATOMIC_RELAXED);
        uintptr_t stack_top = (uintptr_t)&smp_ap_stacks[cpu][SMP_AP_STACK_SIZE];
        startup_data[1] = (uint64_t)(stack_top - sizeof(uint64_t));
        startup_data[2] = (uint64_t)(uintptr_t)state;
        if (!book8_apic_start_cpu(state->apic_id,
                (uint8_t)(SMP_TRAMPOLINE_PHYS >> 12u), tsc_hz)) {
            __atomic_store_n(&state->status, 2u, __ATOMIC_RELEASE);
            continue;
        }
        uint64_t start = read_tsc();
        uint64_t timeout = tsc_hz / 2u;
        while (__atomic_load_n(&state->status, __ATOMIC_ACQUIRE) == 0u
            && read_tsc() - start < timeout)
            __asm__ volatile ("pause");
        if (__atomic_load_n(&state->status, __ATOMIC_ACQUIRE) != 1u)
            continue;
        char message[80];
        snprintf(message, sizeof(message),
            "BOOK8-SMP: AP online id=%u logical=%u\r\n",
            state->apic_id, cpu);
        serial_write(message);
    }
    return true;
}

void smp_runtime_initialize(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpu_id(1u, &eax, &ebx, &ecx, &edx);
    uint32_t bsp_apic_id = ebx >> 24u;
    if (smp_topology.cpu_count == 0u
        || !book8_smp_set_bsp(&smp_topology, bsp_apic_id)
        || !book8_apic_initialize(smp_topology.local_apic_address)) {
        local_apic_ready = false;
        sched_init(1u);
        serial_write("BOOK8-SMP: APIC/MADT unavailable; BSP-only scheduling\r\n");
        return;
    }
    local_apic_ready = true;
    book8_apic_enable_legacy_pic();
    uint32_t supported = 1u;
    for (uint32_t i = 1u; i < smp_topology.cpu_count; ++i) {
        if (smp_topology.apic_ids[i] <= 255u)
            smp_topology.apic_ids[supported++] = smp_topology.apic_ids[i];
    }
    smp_topology.cpu_count = supported;
    if (supported > 1u && !smp_trampoline_page_usable) {
        smp_topology.cpu_count = 1u;
        sched_init(1u);
        serial_write("BOOK8-SMP: no usable low trampoline page; BSP-only scheduling\r\n");
        return;
    }
    sched_init(smp_topology.cpu_count);
    if (!smp_start_application_processors()) {
        sched_init(1u);
        smp_topology.cpu_count = 1u;
        serial_write("BOOK8-SMP: AP trampoline unavailable; BSP-only scheduling\r\n");
        return;
    }
    char message[80];
    snprintf(message, sizeof(message),
        "BOOK8-SMP: online CPUs=%u detected=%u\r\n",
        sched_online_cpu_count(), smp_topology.cpu_count);
    serial_write(message);
}

void scheduler_test(bool user_mode)
{
    if (gui_active) {
        console_write("SCHEDTEST: unavailable while the desktop is active\n");
        return;
    }
    uint64_t flags = irq_save();
    interrupt_table_initialize();
    uint8_t master_mask = inb(0x21u);
    uint8_t slave_mask = inb(0xa1u);
    outb(0x21u, 0xfeu);
    outb(0xa1u, 0xffu);
    pit_clock_start();
    if (user_mode && tsc_hz == 0u) {
        (void)pit_clock_ticks();
        tsc_calibrate();
        if (tsc_hz == 0u) {
            outb(0x21u, master_mask);
            outb(0xa1u, slave_mask);
            irq_restore(flags);
            console_write("SCHEDUSER: clock calibration failed\n");
            return;
        }
    }
    if (!exec_scheduler_test_start()) {
        outb(0x21u, master_mask);
        outb(0xa1u, slave_mask);
        irq_restore(flags);
        console_write("SCHEDTEST: unavailable while a task is active\n");
        return;
    }
    long status = 0;
    if (user_mode)
        status = syscall_scheduler_smoke(exec_scheduler_test_current(), tsc_hz);
    else {
        while (exec_scheduler_test_ticks() < 12u)
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }
    bool user_switched = !user_mode || exec_scheduler_test_user_switches() >= 2u;
    bool passed = exec_scheduler_test_finish() && user_switched;
    outb(0x21u, master_mask);
    outb(0xa1u, slave_mask);
    irq_restore(flags);
    if (user_mode)
        console_write(passed && status == 0 ? "SCHEDUSER: PASS\n" : "SCHEDUSER: FAIL\n");
    else
        console_write(passed ? "SCHEDTEST: PASS\n" : "SCHEDTEST: FAIL\n");
}

void smp_test_worker(void *argument)
{
    uint32_t expected_cpu = (uint32_t)(uintptr_t)argument;
    uint32_t actual_cpu = book8_smp_current_cpu_index();
    if (actual_cpu != expected_cpu)
        __atomic_add_fetch(&smp_test_errors, 1u, __ATOMIC_RELAXED);
    for (uint32_t i = 0u; i < 50000u; ++i)
        __atomic_add_fetch(&smp_test_cpu_runs[expected_cpu], 1u,
            __ATOMIC_RELAXED);
    __atomic_fetch_or(&smp_test_completed, 1ull << expected_cpu,
        __ATOMIC_RELEASE);
}

void smp_scheduler_test(void)
{
    if (gui_active || sched_online_cpu_count() == 0u) {
        console_write("BOOK8-SMP-TEST: unavailable\r\n");
        return;
    }
    __atomic_store_n(&smp_test_completed, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&smp_test_errors, 0u, __ATOMIC_RELAXED);
    uint64_t expected = 0u;
    for (uint32_t cpu = 0u; cpu < sched_cpu_capacity(); ++cpu) {
        __atomic_store_n(&smp_test_cpu_runs[cpu], 0u, __ATOMIC_RELAXED);
        uint32_t tid = sched_spawn_on("smp-check", smp_test_worker,
            (void *)(uintptr_t)cpu, 1ull << cpu);
        if (tid != 0u)
            expected |= 1ull << cpu;
    }
    uint64_t start = read_tsc();
    uint64_t timeout = tsc_hz == 0u ? UINT64_MAX : tsc_hz * 5u;
    while ((__atomic_load_n(&smp_test_completed, __ATOMIC_ACQUIRE)
            & expected) != expected
        && read_tsc() - start < timeout) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
    uint32_t completed = 0u;
    for (uint32_t cpu = 0u; cpu < sched_cpu_capacity(); ++cpu)
        completed += (expected & (1ull << cpu)) != 0u;
    bool passed = expected != 0u
        && (__atomic_load_n(&smp_test_completed, __ATOMIC_ACQUIRE) & expected)
            == expected
        && __atomic_load_n(&smp_test_errors, __ATOMIC_RELAXED) == 0u;
    for (uint32_t cpu = 0u; passed && cpu < sched_cpu_capacity(); ++cpu) {
        if ((expected & (1ull << cpu)) != 0u
            && __atomic_load_n(&smp_test_cpu_runs[cpu], __ATOMIC_RELAXED)
                != 50000u)
            passed = false;
    }
    char message[96];
    snprintf(message, sizeof(message),
        "BOOK8-SMP-TEST: %s cores=%u online=%u\r\n",
        passed ? "PASS" : "FAIL", completed, sched_online_cpu_count());
    serial_write(message);
}

void gui_present(void)
{
    fb_present(&fbdev);
}
