#include "kernel_internal.h"

/* Port I/O, interrupt flags, reboot and CPU idle helpers. */

uint64_t irq_save(void)
{
    uint64_t flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void irq_restore(uint64_t flags)
{
    if ((flags & 0x200u) != 0u) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

void reboot_system(void)
{
    __asm__ volatile ("cli" : : : "memory");
    bool input_buffer_clear = false;
    for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
        if ((inb(0x64u) & 0x02u) == 0u) {
            input_buffer_clear = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    if (input_buffer_clear) {
        outb(0x64u, 0xfeu);
        for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
            __asm__ volatile ("pause");
        }
    }

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) empty_idt = {0u, 0u};
    __asm__ volatile ("lidt %0; int3"
        : : "m"(empty_idt) : "memory");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
