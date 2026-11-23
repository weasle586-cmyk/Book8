#include "kernel_internal.h"

/* ACPI/SMBIOS discovery and firmware shutdown. */

void acpi_log_hex64(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int32_t shift = 60; shift >= 0; shift -= 4) {
        console_putc(digits[(value >> (uint32_t)shift) & 0x0fu]);
    }
}

uint32_t acpi_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

uint64_t acpi_read_u64(const uint8_t *bytes)
{
    return (uint64_t)acpi_read_u32(bytes)
        | ((uint64_t)acpi_read_u32(bytes + 4u) << 32u);
}

bool acpi_bytes_equal(const uint8_t *bytes, const char *text,
    size_t length)
{
    for (size_t i = 0u; i < length; ++i) {
        if (bytes[i] != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

bool acpi_checksum_valid(const uint8_t *bytes, uint32_t length)
{
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < length; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0u;
}

const uint8_t *acpi_table(uint64_t address, const char *signature)
{
    if (address == 0u || address > 0x100000000ull - 36u) {
        return NULL;
    }
    const uint8_t *table = (const uint8_t *)(uintptr_t)address;
    uint32_t length = acpi_read_u32(table + 4u);
    if (length < 36u || length > 1024u * 1024u
        || length > 0x100000000ull - address
        || !acpi_bytes_equal(table, signature, 4u)
        || !acpi_checksum_valid(table, length)) {
        return NULL;
    }
    return table;
}

bool acpi_aml_integer(const uint8_t *aml, uint32_t end,
    uint32_t *offset, uint64_t *value)
{
    if (*offset >= end) {
        return false;
    }
    uint8_t opcode = aml[(*offset)++];
    if (opcode == 0x00u) {
        *value = 0u;
        return true;
    }
    if (opcode == 0x01u) {
        *value = 1u;
        return true;
    }
    if (opcode == 0xffu) {
        *value = UINT64_MAX;
        return true;
    }
    uint32_t size;
    if (opcode == 0x0au) {
        size = 1u;
    } else if (opcode == 0x0bu) {
        size = 2u;
    } else if (opcode == 0x0cu) {
        size = 4u;
    } else if (opcode == 0x0eu) {
        size = 8u;
    } else {
        return false;
    }
    if (size > end - *offset) {
        return false;
    }
    *value = 0u;
    for (uint32_t i = 0u; i < size; ++i) {
        *value |= (uint64_t)aml[(*offset)++] << (i * 8u);
    }
    return true;
}

bool acpi_aml_package_length(const uint8_t *aml, uint32_t end,
    uint32_t *offset, uint32_t *length)
{
    if (*offset >= end) {
        return false;
    }
    uint8_t lead = aml[(*offset)++];
    uint32_t follow_bytes = lead >> 6u;
    if (follow_bytes > 3u || follow_bytes > end - *offset) {
        return false;
    }
    if (follow_bytes == 0u) {
        *length = lead & 0x3fu;
        return true;
    }
    *length = lead & 0x0fu;
    for (uint32_t i = 0u; i < follow_bytes; ++i) {
        *length |= (uint32_t)aml[(*offset)++] << (4u + i * 8u);
    }
    return true;
}

bool acpi_find_s5_types(const uint8_t *dsdt, uint32_t length,
    uint8_t *sleep_type)
{
    for (uint32_t i = 37u; i + 5u < length; ++i) {
        if (!acpi_bytes_equal(dsdt + i, "_S5_", 4u)
            || dsdt[i + 4u] != 0x12u) {
            continue;
        }
        uint32_t name_start = i;
        while (name_start > 37u
            && (dsdt[name_start - 1u] == 0x5eu
                || dsdt[name_start - 1u] == 0x5cu)) {
            --name_start;
        }
        if (name_start == 0u || dsdt[name_start - 1u] != 0x08u) {
            continue;
        }
        uint32_t offset = i + 5u;
        uint32_t package_length;
        if (!acpi_aml_package_length(dsdt, length, &offset,
            &package_length)) {
            continue;
        }
        uint32_t package_length_bytes = offset - (i + 5u);
        if (package_length < package_length_bytes) {
            continue;
        }
        uint32_t package_end = i + 5u + package_length;
        if (package_end > length || offset >= package_end) {
            continue;
        }
        uint8_t element_count = dsdt[offset++];
        uint64_t type_a;
        uint64_t type_b;
        if (element_count < 2u
            || !acpi_aml_integer(dsdt, package_end, &offset, &type_a)
            || !acpi_aml_integer(dsdt, package_end, &offset, &type_b)
            || type_a > 7u || type_b > 7u) {
            continue;
        }
        *sleep_type = (uint8_t)type_a;
        return true;
    }
    return false;
}

bool acpi_fadt_shutdown(const uint8_t *fadt)
{
    uint32_t fadt_length = acpi_read_u32(fadt + 4u);
    if (fadt_length < 116u) {
        return false;
    }

    uint64_t dsdt_address = acpi_read_u32(fadt + 40u);
    uint64_t control_address = acpi_read_u32(fadt + 64u);
    uint8_t control_length = fadt[89u];
    uint8_t control_address_space = 1u;
    uint64_t second_control_address = acpi_read_u32(fadt + 68u);
    uint8_t second_control_address_space = 1u;
    uint32_t smi_command = acpi_read_u32(fadt + 48u);
    uint8_t enable_command = fadt[52u];
    if (fadt_length >= 184u) {
        const uint8_t *gas = fadt + 172u;
        uint64_t extended_address = acpi_read_u64(gas + 4u);
        if ((gas[0] == 0u || gas[0] == 1u) && gas[1] >= 16u
            && gas[2] == 0u && extended_address != 0u) {
            control_address = extended_address;
            control_address_space = gas[0];
            control_length = (uint8_t)((gas[1] + 7u) / 8u);
        }
        gas = fadt + 184u;
        if (fadt_length >= 196u) {
            extended_address = acpi_read_u64(gas + 4u);
            if ((gas[0] == 0u || gas[0] == 1u) && gas[1] >= 16u
                && gas[2] == 0u && extended_address != 0u) {
                second_control_address = extended_address;
                second_control_address_space = gas[0];
            }
        }
    }
    if (control_address == 0u || control_length < 2u
        || (control_address_space == 1u
            && control_address > UINT16_MAX - 1u)
        || (control_address_space == 0u
            && control_address > UINT32_MAX - 1u)) {
        return false;
    }

    if (fadt_length >= 148u) {
        uint64_t extended_dsdt = acpi_read_u64(fadt + 140u);
        if (extended_dsdt != 0u) {
            dsdt_address = extended_dsdt;
        }
    }
    const uint8_t *dsdt = acpi_table(dsdt_address, "DSDT");
    uint8_t sleep_type;
    if (dsdt == NULL
        || !acpi_find_s5_types(dsdt, acpi_read_u32(dsdt + 4u),
            &sleep_type)) {
        return false;
    }
    acpi_pm1a_control_address = control_address;
    acpi_pm1a_address_space = control_address_space;
    if (second_control_address != 0u
        && (second_control_address_space == 0u
            || second_control_address <= UINT16_MAX - 1u)
        && (second_control_address_space != 0u
            || second_control_address <= UINT32_MAX - 1u)) {
        acpi_pm1b_control_address = second_control_address;
        acpi_pm1b_address_space = second_control_address_space;
    }
    acpi_s5_sleep_type = sleep_type;
    acpi_s5_available = true;
    if (smi_command != 0u && smi_command <= UINT16_MAX
        && enable_command != 0u) {
        acpi_smi_command_port = (uint16_t)smi_command;
        acpi_enable_command = enable_command;
        acpi_enable_available = true;
    }

    console_write("ACPI: FADT rev 0x");
    acpi_log_hex64(fadt[8u]);
    console_write(" at 0x");
    acpi_log_hex64((uint64_t)(uintptr_t)fadt);
    console_write("; DSDT 0x");
    acpi_log_hex64(dsdt_address);
    console_write("; PM1a ");
    console_write(control_address_space == 1u ? "I/O " : "MMIO ");
    console_write("0x");
    acpi_log_hex64(control_address);
    console_write("; S5 type ");
    print_unsigned(sleep_type);
    console_write(".\n");
    return true;
}

bool acpi_root_table(uint64_t address, bool xsdt)
{
    const uint8_t *root = acpi_table(address, xsdt ? "XSDT" : "RSDT");
    if (root == NULL) {
        return false;
    }
    acpi_root_seen = true;
    uint32_t length = acpi_read_u32(root + 4u);
    uint32_t entry_size = xsdt ? 8u : 4u;
    if ((length - 36u) % entry_size != 0u) {
        return false;
    }
    for (uint32_t offset = 36u; offset < length; offset += entry_size) {
        uint64_t table_address = xsdt
            ? acpi_read_u64(root + offset)
            : acpi_read_u32(root + offset);
        const uint8_t *madt = acpi_table(table_address, "APIC");
        if (madt != NULL && smp_topology.cpu_count == 0u) {
            if (book8_smp_parse_madt(madt, acpi_read_u32(madt + 4u),
                    &smp_topology)) {
                char message[96];
                snprintf(message, sizeof(message),
                    "BOOK8-SMP: MADT LAPIC=0x%08x enabled CPUs=%u\r\n",
                    (uint32_t)smp_topology.local_apic_address,
                    smp_topology.cpu_count);
                serial_write(message);
            } else {
                serial_write("BOOK8-SMP: invalid or unsupported MADT; BSP-only mode\r\n");
            }
        }
        const uint8_t *mcfg = acpi_table(table_address, "MCFG");
        if (mcfg != NULL && pcie.ecam_count == 0u) {
            (void)book8_pcie_add_mcfg(&pcie, mcfg, acpi_read_u32(mcfg + 4u));
        }
        const uint8_t *fadt = acpi_table(table_address, "FACP");
        if (fadt != NULL && !acpi_s5_available) {
            (void)acpi_fadt_shutdown(fadt);
        }
        /* DMAR (Intel VT-d) and IVRS (AMD-Vi) are the firmware's IOMMU
         * descriptions. Their presence is the only IOMMU fact BOOK8 can
         * assert without enabling address translation. */
        if (acpi_table(table_address, "DMAR") != NULL
            || acpi_table(table_address, "IVRS") != NULL) {
            acpi_iommu_present = true;
        }
    }
    return true;
}

void acpi_initialize_from_rsdp(const uint8_t *rsdp,
    uint32_t available)
{
    if (available < 20u || !acpi_bytes_equal(rsdp, "RSD PTR ", 8u)
        || !acpi_checksum_valid(rsdp, 20u)) {
        return;
    }
    uint8_t revision = rsdp[15u];
    if (revision >= 2u && available >= 36u) {
        uint32_t length = acpi_read_u32(rsdp + 20u);
        if (length < 36u || length > available
            || !acpi_checksum_valid(rsdp, length)) {
            return;
        }
        uint64_t xsdt_address = acpi_read_u64(rsdp + 24u);
        console_write("ACPI: RSDP rev ");
        print_unsigned(revision);
        console_write(" XSDT 0x");
        acpi_log_hex64(xsdt_address);
        console_write(" RSDT 0x");
        acpi_log_hex64(acpi_read_u32(rsdp + 16u));
        console_write(".\n");
        if (xsdt_address != 0u && acpi_root_table(xsdt_address, true)) {
            return;
        }
    } else {
        console_write("ACPI: RSDP rev ");
        print_unsigned(revision);
        console_write(" RSDT 0x");
        acpi_log_hex64(acpi_read_u32(rsdp + 16u));
        console_write(".\n");
    }
    (void)acpi_root_table(acpi_read_u32(rsdp + 16u), false);
}

void acpi_discover(const uint8_t *info, uint32_t total_size)
{
    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(info + offset);
        if (tag->size < sizeof(*tag) || tag->size > total_size - offset) {
            return;
        }
        if ((tag->type == MULTIBOOT_TAG_ACPI_OLD
                || tag->type == MULTIBOOT_TAG_ACPI_NEW)
            && tag->size > sizeof(*tag)) {
            acpi_initialize_from_rsdp((const uint8_t *)tag + sizeof(*tag),
                tag->size - sizeof(*tag));
            if (acpi_s5_available) {
                return;
            }
        }
        if (tag->type == MULTIBOOT_TAG_END) {
            return;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            return;
        }
        offset += next;
    }
}

void acpi_scan_rsdp_range(uint32_t start, uint32_t length)
{
    uint32_t end = start + length;
    for (uint32_t address = (start + 15u) & ~15u;
        address <= end - 20u; address += 16u) {
        const uint8_t *rsdp = (const uint8_t *)(uintptr_t)address;
        if (acpi_bytes_equal(rsdp, "RSD PTR ", 8u)) {
            uint32_t available = end - address;
            if (available > 4096u) {
                available = 4096u;
            }
            acpi_initialize_from_rsdp(rsdp, available);
            if (acpi_s5_available) {
                return;
            }
        }
    }
}

void acpi_discover_firmware(void)
{
    uintptr_t ebda_pointer_address = 0x40eu;
    uint16_t ebda_segment;
    __asm__ volatile ("movw (%1), %0"
        : "=r"(ebda_segment) : "r"(ebda_pointer_address) : "memory");
    uint32_t ebda = (uint32_t)ebda_segment << 4u;
    if (ebda >= 0x80000u && ebda < 0xa0000u) {
        acpi_scan_rsdp_range(ebda, 1024u);
    }
    if (!acpi_s5_available) {
        acpi_scan_rsdp_range(0xe0000u, 0x20000u);
    }
}

void smbios_copy_string(char *destination, uint32_t capacity,
    const uint8_t *strings, const uint8_t *end, uint8_t index)
{
    destination[0] = '\0';
    if (index == 0u || strings == NULL || strings >= end) {
        return;
    }
    const uint8_t *cursor = strings;
    for (uint8_t current = 1u; current < index; ++current) {
        while (cursor < end && *cursor != 0u) {
            ++cursor;
        }
        if (cursor >= end) {
            return;
        }
        ++cursor;
    }
    if (cursor >= end) {
        return;
    }
    uint32_t n = 0u;
    while (cursor < end && *cursor != 0u && n + 1u < capacity) {
        destination[n++] = (char)*cursor++;
    }
    destination[n] = '\0';
}

uint32_t smbios_structure_end(const uint8_t *strings,
    const uint8_t *end)
{
    const uint8_t *cursor = strings;
    while (cursor + 1u < end) {
        if (cursor[0] == 0u && cursor[1] == 0u) {
            return (uint32_t)(cursor + 2u - strings);
        }
        ++cursor;
    }
    return 0u;
}

uint32_t smbios_u16(const uint8_t *bytes)
{
    return (uint32_t)bytes[0u] | ((uint32_t)bytes[1u] << 8u);
}

uint32_t smbios_u32(const uint8_t *bytes)
{
    return smbios_u16(bytes) | (smbios_u16(bytes + 2u) << 16u);
}

void smbios_append_slot(const char *slot, const char *capacity)
{
    uint32_t used = (uint32_t)strlen(platform_inventory.memory_layout);
    if (used + 1u >= sizeof(platform_inventory.memory_layout)) {
        return;
    }
    int written = snprintf(platform_inventory.memory_layout + used,
        sizeof(platform_inventory.memory_layout) - used,
        "%s%s=%s", used != 0u ? ", " : "", slot, capacity);
    if (written < 0 || (uint32_t)written
        >= sizeof(platform_inventory.memory_layout) - used) {
        platform_inventory.memory_layout[
            sizeof(platform_inventory.memory_layout) - 1u] = '\0';
    }
    platform_inventory.flags |= BOOK8_PLATFORM_DIMMS_VALID;
}

void smbios_parse_table(void)
{
    uint32_t offset = 0u;
    for (uint32_t index = 0u; index < 512u
        && offset + 4u <= smbios_table_size; ++index) {
        const uint8_t *header = smbios_table + offset;
        uint8_t length = header[1u];
        if (length < 4u || offset + length > smbios_table_size) {
            break;
        }
        const uint8_t *strings = header + length;
        const uint8_t *table_end = smbios_table + smbios_table_size;
        uint32_t span = smbios_structure_end(strings, table_end);
        if (span == 0u) {
            break;
        }

        switch (header[0u]) {
        case 0u: /* BIOS information */
            if (length <= 8u) {
                break;
            }
            smbios_copy_string(platform_inventory.bios_vendor,
                sizeof(platform_inventory.bios_vendor), strings, table_end,
                header[4u]);
            smbios_copy_string(platform_inventory.bios_version,
                sizeof(platform_inventory.bios_version), strings, table_end,
                header[5u]);
            smbios_copy_string(platform_inventory.bios_date,
                sizeof(platform_inventory.bios_date), strings, table_end,
                header[8u]);
            platform_inventory.flags |= BOOK8_PLATFORM_SMBIOS_VALID;
            break;
        case 1u: /* System information */
            if (length <= 5u) {
                break;
            }
            if (platform_inventory.board_vendor[0u] == '\0') {
                smbios_copy_string(platform_inventory.board_vendor,
                    sizeof(platform_inventory.board_vendor), strings,
                    table_end, header[4u]);
            }
            if (platform_inventory.board_product[0u] == '\0') {
                smbios_copy_string(platform_inventory.board_product,
                    sizeof(platform_inventory.board_product), strings,
                    table_end, header[5u]);
            }
            break;
        case 2u: /* Baseboard (module) information */
            if (length <= 5u) {
                break;
            }
            smbios_copy_string(platform_inventory.board_vendor,
                sizeof(platform_inventory.board_vendor), strings, table_end,
                header[4u]);
            smbios_copy_string(platform_inventory.board_product,
                sizeof(platform_inventory.board_product), strings, table_end,
                header[5u]);
            break;
        case 4u: /* Processor information */
            if (length <= 0x10u) {
                break;
            }
            if (platform_inventory.cpu_model[0u] == '\0') {
                smbios_copy_string(platform_inventory.cpu_model,
                    sizeof(platform_inventory.cpu_model), strings, table_end,
                    header[0x10u]);
                platform_inventory.cpu_family = header[6u];
            }
            break;
        case 17u: /* Memory device */
            if (length >= 0x18u) {
                uint16_t size = (uint16_t)smbios_u16(header + 0x0cu);
                char slot[24] = "DIMM";
                char capacity[24] = "unknown";
                if (platform_inventory.dimm_slots < 0xffffu) {
                    ++platform_inventory.dimm_slots;
                }
                smbios_copy_string(slot, sizeof(slot), strings, table_end,
                    header[0x10u]);
                if (size == 0u) {
                    snprintf(capacity, sizeof(capacity), "empty");
                } else if (size != 0xffffu) {
                    uint32_t mb = size & 0x7fffu;
                    if ((size & 0x8000u) != 0u) {
                        mb *= 64u;
                    }
                    platform_inventory.dimm_installed_mb += mb;
                    if (platform_inventory.dimm_count < 0xffffu) {
                        ++platform_inventory.dimm_count;
                    }
                    snprintf(capacity, sizeof(capacity), "%u MB", (unsigned)mb);
                }
                smbios_append_slot(slot, capacity);
            }
            break;
        default:
            break;
        }
        offset += length + span;
    }
}

void smbios_table_init(const uint8_t *entry, uint32_t available)
{
    if (entry == NULL || available < 0x18u) {
        return;
    }
    if (acpi_bytes_equal(entry, "_SM3_", 5u)) {
        uint32_t entry_length = entry[6u];
        if (entry_length < 0x18u || entry_length > available
            || !acpi_checksum_valid(entry, entry_length)) {
            return;
        }
        uint32_t size = smbios_u32(entry + 0x0cu);
        uint64_t address = 0u;
        for (uint32_t i = 0u; i < 8u; ++i) {
            address |= (uint64_t)entry[0x10u + i] << (8u * i);
        }
        if (size >= 4u && size <= 0x10000000u && address != 0u
            && address <= 0xffffffffu) {
            smbios_table = (const uint8_t *)(uintptr_t)address;
            smbios_table_size = size;
            smbios_parse_table();
        }
        return;
    }
    if (available < 0x1fu || !acpi_bytes_equal(entry, "_SM_", 4u)
        || !acpi_bytes_equal(entry + 0x0fu, "_DMI_", 5u)) {
        return;
    }
    uint32_t entry_length = entry[5u] != 0u ? entry[5u] : 0x1fu;
    if (entry_length < 0x1fu) {
        entry_length = 0x1fu;
    }
    if (entry_length > available || !acpi_checksum_valid(entry, entry_length)
        || !acpi_checksum_valid(entry + 0x0fu, 0x10u)) {
        return;
    }
    uint32_t size = smbios_u16(entry + 0x15u);
    uint32_t address = smbios_u32(entry + 0x17u);
    if (size >= 4u && size <= 0x10000000u && address != 0u) {
        smbios_table = (const uint8_t *)(uintptr_t)address;
        smbios_table_size = size;
        smbios_parse_table();
    }
}

void smbios_discover(const uint8_t *info, uint32_t total_size)
{
    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(info + offset);
        if (tag->type == 13u && tag->size > sizeof(*tag)) { /* SMBIOS tag */
            smbios_table_init((const uint8_t *)tag + sizeof(*tag),
                tag->size - (uint32_t)sizeof(*tag));
        }
        if (tag->type == 10u || tag->type == 11u
            || tag->type == 16u || tag->type == 17u) {
            boot_handoff_uefi = true; /* EFI system table or image handle */
        }
        if (tag->type == MULTIBOOT_TAG_END || tag->size < sizeof(*tag)
            || tag->size > total_size - offset) {
            break;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            break;
        }
        offset += next;
    }
    if (smbios_table != NULL) {
        return;
    }
    for (uint32_t address = 0xf0000u; address <= 0xffff0u; address += 16u) {
        const uint8_t *entry = (const uint8_t *)(uintptr_t)address;
        if (acpi_bytes_equal(entry, "_SM_", 4u)
            || acpi_bytes_equal(entry, "_SM3_", 5u)) {
            smbios_table_init(entry, 0x20u);
            if (smbios_table != NULL) {
                return;
            }
        }
    }
}

void acpi_shutdown(void)
{
    if (!acpi_s5_available || acpi_pm1a_control_address == 0u) {
        console_write("ACPI S5 shutdown is unavailable; halting CPU.\n");
    } else {
        uint16_t control = acpi_pm1a_address_space == 1u
            ? inw((uint16_t)acpi_pm1a_control_address)
            : *(volatile uint16_t *)(uintptr_t)acpi_pm1a_control_address;
        if ((control & 1u) == 0u && acpi_enable_available) {
            outb(acpi_smi_command_port, acpi_enable_command);
            for (uint32_t attempt = 0u; attempt < 10000000u; ++attempt) {
                control = acpi_pm1a_address_space == 1u
                    ? inw((uint16_t)acpi_pm1a_control_address)
                    : *(volatile uint16_t *)(uintptr_t)
                        acpi_pm1a_control_address;
                if ((control & 1u) != 0u) {
                    break;
                }
                __asm__ volatile ("pause");
            }
        }
        if ((control & 1u) == 0u && acpi_enable_available) {
            console_write("ACPI: SCI enable timed out; attempting S5.\n");
        }
        control = (uint16_t)((control & ~((7u << 10u) | (1u << 13u)))
            | ((uint16_t)acpi_s5_sleep_type << 10u) | (1u << 13u));
        if (acpi_pm1a_address_space == 1u) {
            outw((uint16_t)acpi_pm1a_control_address, control);
        } else {
            *(volatile uint16_t *)(uintptr_t)acpi_pm1a_control_address =
                control;
        }
        if (acpi_pm1b_control_address != 0u) {
            uint16_t control_b = acpi_pm1b_address_space == 1u
                ? inw((uint16_t)acpi_pm1b_control_address)
                : *(volatile uint16_t *)(uintptr_t)
                    acpi_pm1b_control_address;
            control_b = (uint16_t)((control_b & ~((7u << 10u) | (1u << 13u)))
                | ((uint16_t)acpi_s5_sleep_type << 10u) | (1u << 13u));
            if (acpi_pm1b_address_space == 1u) {
                outw((uint16_t)acpi_pm1b_control_address, control_b);
            } else {
                *(volatile uint16_t *)(uintptr_t)acpi_pm1b_control_address =
                    control_b;
            }
        }
    }
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
