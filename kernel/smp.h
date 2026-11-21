#ifndef BOOK8_SMP_H
#define BOOK8_SMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK8_SMP_MAX_CPUS 64u

struct book8_smp_topology {
    uint64_t local_apic_address;
    uint32_t cpu_count;
    uint32_t apic_ids[BOOK8_SMP_MAX_CPUS];
    bool has_x2apic_entries;
};

bool book8_smp_parse_madt(const void *table, size_t size,
    struct book8_smp_topology *topology);
bool book8_smp_add_cpu(struct book8_smp_topology *topology,
    uint32_t apic_id);
bool book8_smp_set_bsp(struct book8_smp_topology *topology,
    uint32_t apic_id);

#endif
