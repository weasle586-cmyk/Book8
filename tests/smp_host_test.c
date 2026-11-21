#include "../kernel/smp.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    write_u32(bytes, (uint32_t)value);
    write_u32(bytes + 4u, (uint32_t)(value >> 32u));
}

static void set_checksum(uint8_t *table, size_t length)
{
    table[9] = 0u;
    uint8_t sum = 0u;
    for (size_t i = 0u; i < length; ++i)
        sum = (uint8_t)(sum + table[i]);
    table[9] = (uint8_t)(0u - sum);
}

int main(void)
{
    uint8_t table[44u + 8u * 2u + 16u * 2u + 12u] = {0};
    memcpy(table, "APIC", 4u);
    write_u32(table + 4u, sizeof(table));
    write_u32(table + 36u, 0xfee00000u);
    size_t offset = 44u;
    table[offset] = 0u;
    table[offset + 1u] = 8u;
    table[offset + 3u] = 0u;
    write_u32(table + offset + 4u, 1u);
    offset += 8u;
    table[offset] = 0u;
    table[offset + 1u] = 8u;
    table[offset + 3u] = 1u;
    write_u32(table + offset + 4u, 0u);
    offset += 8u;
    table[offset] = 9u;
    table[offset + 1u] = 16u;
    write_u32(table + offset + 4u, 2u);
    write_u32(table + offset + 8u, 3u);
    offset += 16u;
    table[offset] = 9u;
    table[offset + 1u] = 16u;
    write_u32(table + offset + 4u, 3u);
    write_u32(table + offset + 8u, 1u);
    offset += 16u;
    table[offset] = 5u;
    table[offset + 1u] = 12u;
    write_u64(table + offset + 4u, 0xfee01000u);
    set_checksum(table, sizeof(table));

    struct book8_smp_topology topology;
    assert(book8_smp_parse_madt(table, sizeof(table), &topology));
    assert(topology.local_apic_address == 0xfee01000u);
    assert(topology.cpu_count == 3u);
    assert(topology.apic_ids[0] == 0u && topology.apic_ids[1] == 2u
        && topology.apic_ids[2] == 3u);
    assert(topology.has_x2apic_entries);
    assert(book8_smp_set_bsp(&topology, 3u));
    assert(topology.apic_ids[0] == 3u && topology.apic_ids[2] == 0u);
    assert(!book8_smp_parse_madt(table, sizeof(table) - 1u, &topology));
    table[sizeof(table) - 1u] ^= 1u;
    assert(!book8_smp_parse_madt(table, sizeof(table), &topology));
    puts("PASS: MADT processor enumeration, overrides and validation");
    return 0;
}
