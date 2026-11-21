#include "smp.h"

#include <string.h>

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t read_u64(const uint8_t *bytes)
{
    return (uint64_t)read_u32(bytes) | ((uint64_t)read_u32(bytes + 4u) << 32u);
}

static bool checksum_valid(const uint8_t *bytes, size_t length)
{
    uint8_t sum = 0u;
    for (size_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0u;
}

bool book8_smp_add_cpu(struct book8_smp_topology *topology,
    uint32_t apic_id)
{
    if (topology == NULL)
        return false;
    for (uint32_t i = 0u; i < topology->cpu_count; ++i) {
        if (topology->apic_ids[i] == apic_id)
            return true;
    }
    if (topology->cpu_count >= BOOK8_SMP_MAX_CPUS)
        return false;
    topology->apic_ids[topology->cpu_count++] = apic_id;
    return true;
}

bool book8_smp_set_bsp(struct book8_smp_topology *topology,
    uint32_t apic_id)
{
    if (!book8_smp_add_cpu(topology, apic_id))
        return false;
    for (uint32_t i = 0u; i < topology->cpu_count; ++i) {
        if (topology->apic_ids[i] == apic_id) {
            uint32_t first = topology->apic_ids[0];
            topology->apic_ids[0] = apic_id;
            topology->apic_ids[i] = first;
            return true;
        }
    }
    return false;
}

bool book8_smp_parse_madt(const void *table, size_t size,
    struct book8_smp_topology *topology)
{
    if (table == NULL || topology == NULL || size < 44u)
        return false;
    const uint8_t *bytes = table;
    uint32_t length = read_u32(bytes + 4u);
    if (memcmp(bytes, "APIC", 4u) != 0 || length < 44u
        || length > size || !checksum_valid(bytes, length))
        return false;

    struct book8_smp_topology parsed = {
        .local_apic_address = read_u32(bytes + 36u),
    };
    if (parsed.local_apic_address == 0u)
        return false;

    for (size_t offset = 44u; offset < length;) {
        if (length - offset < 2u)
            return false;
        uint8_t type = bytes[offset];
        uint8_t entry_length = bytes[offset + 1u];
        if (entry_length < 2u || entry_length > length - offset)
            return false;

        const uint8_t *entry = bytes + offset;
        uint32_t apic_id = 0u;
        uint32_t flags = 0u;
        bool processor_entry = false;
        if (type == 0u) {
            if (entry_length < 8u)
                return false;
            apic_id = entry[3u];
            flags = read_u32(entry + 4u);
            processor_entry = true;
        } else if (type == 9u) {
            if (entry_length < 16u)
                return false;
            apic_id = read_u32(entry + 4u);
            flags = read_u32(entry + 8u);
            processor_entry = true;
            parsed.has_x2apic_entries = true;
        } else if (type == 5u) {
            if (entry_length < 12u)
                return false;
            parsed.local_apic_address = read_u64(entry + 4u);
        }
        if (processor_entry && (flags & 3u) != 0u
            && !book8_smp_add_cpu(&parsed, apic_id))
            return false;
        offset += entry_length;
    }
    if (parsed.local_apic_address == 0u
        || parsed.local_apic_address >= 0x100000000ull
        || (parsed.local_apic_address & 0xfffu) != 0u
        || parsed.cpu_count == 0u)
        return false;
    *topology = parsed;
    return true;
}
