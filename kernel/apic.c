#include "apic.h"

#include <stddef.h>

#define IA32_APIC_BASE 0x1bu
#define APIC_BASE_ENABLE (1ull << 11u)
#define APIC_BASE_X2APIC (1ull << 10u)
#define APIC_ID 0x020u
#define APIC_TPR 0x080u
#define APIC_EOI 0x0b0u
#define APIC_SVR 0x0f0u
#define APIC_ESR 0x280u
#define APIC_ICR_LOW 0x300u
#define APIC_ICR_HIGH 0x310u
#define APIC_LVT_TIMER 0x320u
#define APIC_LVT_LINT0 0x350u
#define APIC_LVT_LINT1 0x360u
#define APIC_LVT_ERROR 0x370u
#define APIC_TIMER_INITIAL 0x380u
#define APIC_TIMER_CURRENT 0x390u
#define APIC_TIMER_DIVIDE 0x3e0u
#define APIC_DELIVERY_PENDING (1u << 12u)
#define APIC_LVT_MASKED (1u << 16u)
#define APIC_TIMER_PERIODIC (1u << 17u)
#define APIC_LVT_EXTINT (7u << 8u)

static volatile uint32_t *local_apic;
static volatile uint32_t icr_lock;

extern volatile uint8_t *book8_apic_map_mmio(uint64_t physical,
    uint64_t size);

static inline uint64_t read_tsc(void)
{
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32u) | low;
}

static inline uint64_t read_msr(uint32_t index)
{
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32u) | low;
}

static inline void write_msr(uint32_t index, uint64_t value)
{
    __asm__ volatile ("wrmsr" : : "c"(index), "a"((uint32_t)value),
        "d"((uint32_t)(value >> 32u)) : "memory");
}

static inline void io_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t read_register(uint32_t offset)
{
    return local_apic[offset / sizeof(uint32_t)];
}

static inline void write_register(uint32_t offset, uint32_t value)
{
    local_apic[offset / sizeof(uint32_t)] = value;
    (void)local_apic[APIC_ID / sizeof(uint32_t)];
}

static bool wait_delivery(void)
{
    for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
        if ((read_register(APIC_ICR_LOW) & APIC_DELIVERY_PENDING) == 0u)
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void delay_us(uint64_t tsc_hz, uint32_t microseconds)
{
    uint64_t ticks = tsc_hz / 1000000u * microseconds;
    uint64_t start = read_tsc();
    while (read_tsc() - start < ticks)
        __asm__ volatile ("pause");
}

bool book8_apic_initialize(uint64_t physical_base)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx),
        "=d"(edx) : "a"(1u), "c"(0u));
    (void)eax;
    (void)ebx;
    (void)ecx;
    if ((edx & (1u << 9u)) == 0u || (edx & (1u << 5u)) == 0u
        || physical_base == 0u
        || physical_base >= 0x100000000ull
        || (physical_base & 0xfffu) != 0u)
        return false;

    uint64_t apic_base = read_msr(IA32_APIC_BASE);
    if ((apic_base & APIC_BASE_X2APIC) != 0u)
        return false;
    apic_base = (apic_base & ~0xfffff000ull)
        | (physical_base & 0xfffff000ull) | APIC_BASE_ENABLE;
    write_msr(IA32_APIC_BASE, apic_base);
    local_apic = (volatile uint32_t *)book8_apic_map_mmio(physical_base,
        4096u);
    if (local_apic == NULL)
        return false;
    return book8_apic_initialize_local();
}

bool book8_apic_initialize_local(void)
{
    if (local_apic == NULL)
        return false;
    uint64_t apic_base = read_msr(IA32_APIC_BASE);
    if ((apic_base & APIC_BASE_X2APIC) != 0u)
        return false;
    apic_base = (apic_base & ~0xfffff000ull)
        | ((uint64_t)(uintptr_t)local_apic & 0xfffff000ull)
        | APIC_BASE_ENABLE;
    write_msr(IA32_APIC_BASE, apic_base);
    write_register(APIC_TPR, 0u);
    write_register(APIC_LVT_TIMER, APIC_LVT_MASKED | 0x40u);
    write_register(APIC_LVT_LINT0, APIC_LVT_MASKED);
    write_register(APIC_LVT_LINT1, APIC_LVT_MASKED);
    write_register(APIC_LVT_ERROR, APIC_LVT_MASKED | 0xfeu);
    write_register(APIC_SVR, 0x100u | 0xffu);
    write_register(APIC_ESR, 0u);
    write_register(APIC_ESR, 0u);
    return true;
}

void book8_apic_enable_legacy_pic(void)
{
    if (local_apic == NULL)
        return;
    write_register(APIC_LVT_LINT0, APIC_LVT_EXTINT);
    io_out8(0x22u, 0x70u);
    io_out8(0x23u, 0x01u);
}

uint32_t book8_apic_current_id(void)
{
    return local_apic == NULL ? UINT32_MAX
        : read_register(APIC_ID) >> 24u;
}

void book8_apic_eoi(void)
{
    if (local_apic != NULL)
        write_register(APIC_EOI, 0u);
}

bool book8_apic_send_ipi(uint32_t apic_id, uint32_t command)
{
    if (local_apic == NULL || apic_id > 255u)
        return false;
    while (__atomic_exchange_n(&icr_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    bool ok = wait_delivery();
    if (ok) {
        write_register(APIC_ICR_HIGH, apic_id << 24u);
        write_register(APIC_ICR_LOW, command);
        ok = wait_delivery();
    }
    __atomic_store_n(&icr_lock, 0u, __ATOMIC_RELEASE);
    return ok;
}

bool book8_apic_start_cpu(uint32_t apic_id, uint8_t startup_vector,
    uint64_t tsc_hz)
{
    if (tsc_hz < 1000000u || startup_vector == 0u)
        return false;
    if (!book8_apic_send_ipi(apic_id, 0x0000c500u))
        return false;
    delay_us(tsc_hz, 10000u);
    if (!book8_apic_send_ipi(apic_id, 0x00008500u))
        return false;
    delay_us(tsc_hz, 200u);
    if (!book8_apic_send_ipi(apic_id, 0x00000600u | startup_vector))
        return false;
    delay_us(tsc_hz, 200u);
    return book8_apic_send_ipi(apic_id, 0x00000600u | startup_vector);
}

uint32_t book8_apic_calibrate_timer(uint64_t tsc_hz)
{
    if (local_apic == NULL || tsc_hz < 1000000u)
        return 0u;
    write_register(APIC_TIMER_DIVIDE, 0x3u);
    write_register(APIC_LVT_TIMER, APIC_LVT_MASKED | 0x40u);
    write_register(APIC_TIMER_INITIAL, UINT32_MAX);
    uint64_t start = read_tsc();
    uint64_t interval = tsc_hz / 100u;
    if (interval == 0u)
        return 0u;
    while (read_tsc() - start < interval)
        __asm__ volatile ("pause");
    uint32_t elapsed = UINT32_MAX - read_register(APIC_TIMER_CURRENT);
    write_register(APIC_TIMER_INITIAL, 0u);
    return elapsed == 0u ? 0u : elapsed;
}

void book8_apic_timer_start(uint32_t initial_count, uint8_t vector)
{
    if (local_apic == NULL || initial_count == 0u)
        return;
    write_register(APIC_TIMER_DIVIDE, 0x3u);
    write_register(APIC_LVT_TIMER, APIC_TIMER_PERIODIC | vector);
    write_register(APIC_TIMER_INITIAL, initial_count);
}
