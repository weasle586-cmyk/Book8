#include "kernel_internal.h"

/* Hardware diagnostics and CPU timing helpers. */

bool read_active_tss(uint16_t *selector, uint64_t *rsp0)
{
    struct gdtr {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;
    uint16_t tr;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    __asm__ volatile ("str %0" : "=r"(tr));
    *selector = tr;
    if ((tr & 4u) != 0u || (tr & 0xfff8u) == 0u) {
        return false;
    }
    uint64_t descriptor_offset = tr & 0xfff8u;
    if (descriptor_offset + 15u > gdtr.limit
        || gdtr.base > 0x100000000ull - 16u
        || descriptor_offset > 0x100000000ull - gdtr.base - 16u) {
        return false;
    }
    const volatile uint64_t *descriptor =
        (const volatile uint64_t *)(uintptr_t)(gdtr.base + descriptor_offset);
    uint64_t low = descriptor[0], high = descriptor[1];
    uint8_t type = (uint8_t)((low >> 40u) & 0x0fu);
    uint32_t limit = (uint32_t)(low & 0xffffu)
        | (uint32_t)((low >> 32u) & 0x000f0000u);
    uint64_t base = ((low >> 16u) & 0xffffffu)
        | (((low >> 56u) & 0xffu) << 24u)
        | ((high & 0xffffffffu) << 32u);
    if ((type != 9u && type != 11u) || limit < 11u
        || base >= 0x100000000ull - 12u) {
        return false;
    }
    *rsp0 = *(const volatile uint64_t *)(uintptr_t)(base + 4u);
    return true;
}

void hwdiag_command(void)
{
    struct gdtr {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;
    uint64_t cr0, cr3, cr4, rsp0 = 0u;
    uint16_t tr = 0u;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    bool tss_valid = read_active_tss(&tr, &rsp0);
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    console_write("HWDIAG GDT: base=0x");
    print_hex64(gdtr.base);
    console_write(" limit=0x");
    print_hex32(gdtr.limit);
    console_write(" TSS TR=0x");
    print_hex32(tr);
    console_write(tss_valid ? " active RSP0=0x" : " unavailable RSP0=unavailable");
    if (tss_valid) {
        print_hex64(rsp0);
        console_write(rsp0 != 0u && rsp0 < 0x100000000ull
                && (rsp0 & 0x0fu) == 0u ? " valid\n" : " invalid\n");
    } else {
        console_putc('\n');
    }
    console_write("Paging: CR0=0x");
    print_hex64(cr0);
    console_write(" CR3=0x");
    print_hex64(cr3);
    console_write(" PML4=0x");
    print_hex64(cr3 & 0x000ffffffffff000ull);
    console_write(" CR4=0x");
    print_hex64(cr4);
    console_write((cr0 & (1ull << 31u)) != 0u ? " PG=on" : " PG=off");
    console_write((cr4 & (1ull << 5u)) != 0u ? " PAE=on\n" : " PAE=off\n");
    console_write("PnP: PCI devices=");
    print_unsigned(pcie.node_count);
    console_write(" registered=");
    print_unsigned(pcie.registered_driver_count);
    console_write(" bound=");
    print_unsigned(pcie.driver_count);
    console_write("; USB registered=");
    print_unsigned(book8_usb_registered_driver_count());
    console_write(" bound=");
    print_unsigned(book8_usb_bound_device_count());
    console_putc('\n');
    console_write("PnP totals: registered drivers=");
    print_unsigned(pcie.registered_driver_count
        + book8_usb_registered_driver_count());
    console_write(" bound devices=");
    print_unsigned((uint32_t)pcie.driver_count
        + book8_usb_bound_device_count());
    console_putc('\n');
    console_write("Registered bus_driver entries: ");
    print_unsigned(sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    console_putc('\n');
    for (size_t i = 0u; i < sizeof(bus_drivers) / sizeof(bus_drivers[0]); ++i) {
        const struct book8_bus_driver *driver = &bus_drivers[i];
        console_write("  ");
        console_write(driver->name);
        console_write(" matches=");
        print_unsigned(driver->match_count);
        if (driver->bar0_window_size != 0u) {
            console_write(" BAR0-window=0x");
            print_hex64(driver->bar0_window_size);
        }
        console_putc('\n');
    }
    print_input_status();
}

inline uint64_t read_tsc(void)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32u) | low;
}

void cpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
    uint32_t *d)
{
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0u));
}

void tsc_calibrate(void)
{
    uint64_t pit_start = pit_clock_ticks();
    uint64_t tsc_start = read_tsc();
    uint64_t elapsed = 0u;
    for (uint32_t spin = 0u; elapsed < PIT_HZ / 20u && spin < 20000000u;
        ++spin) {
        elapsed = pit_clock_ticks() - pit_start;
    }
    if (elapsed != 0u) {
        tsc_hz = (read_tsc() - tsc_start) * PIT_HZ / elapsed;
    }
}

void msr_guard_test(void)
{
    interrupt_table_initialize();
    uint64_t before = 0u, after = 0u, invalid = 0x123456789abcdef0ull;
    uint64_t flags = irq_save();
    irq_restore(flags);
    bool first = book8_read_msr_safe(0xc0000080u, &before);
    /* Reserved x2APIC register zero faults even when arbitrary unknown MSRs
     * are emulated as zero by QEMU TCG. This is a read-only fault probe. */
    bool rejected = !book8_read_msr_safe(0x800u, &invalid);
    bool second = book8_read_msr_safe(0xc0000080u, &after);
    uint32_t memory = 0x12345678u, output = 0xabcdef01u;
    bool memory_ok = book8_read32_safe(&memory, &output) == 0u && output == memory
        && book8_write32_safe(&memory, 0x87654321u) == 0u && memory == 0x87654321u;
    /* Noncanonical addresses #GP; an absent identity-map address #PF.
     * No physical device is accessed by either fault injection. */
    volatile uint32_t *gp_address = (volatile uint32_t *)(uintptr_t)0x0100000000000000ull;
    volatile uint32_t *pf_address = (volatile uint32_t *)(uintptr_t)0x100000000ull;
    output = 0xabcdef01u;
    bool memory_faults = book8_read32_safe(gp_address, &output) == 13u
        && book8_write32_safe(gp_address, 0u) == 13u
        && book8_read32_safe(pf_address, &output) == 14u
        && book8_write32_safe(pf_address, 0u) == 14u
        && output == 0xabcdef01u;
    uint64_t final_flags = irq_save();
    irq_restore(final_flags);
    console_write(first && second && before == after && rejected && memory_ok && memory_faults
        && invalid == 0x123456789abcdef0ull && (flags & 0x200u) == (final_flags & 0x200u)
        ? "MSRGUARDTEST: PASS valid EFER read, #GP recovery, output/IF preserved\n"
        : "MSRGUARDTEST: FAIL safe MSR recovery or state preservation\n");
    console_write(memory_ok && memory_faults
        ? "MEMGUARDTEST: PASS RAM read/write, #GP/#PF read/write recovery, failed output preserved\n"
        : "MEMGUARDTEST: FAIL guarded memory accesses\n");
}
