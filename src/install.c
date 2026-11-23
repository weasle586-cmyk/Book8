#include "kernel_internal.h"

/* Interactive installer and FAT32 target preparation. */

bool confirm_text(const char *prompt)
{
    char answer[8];
    uint32_t length = 0u;
    console_write(prompt);
    for (;;) {
        char value = read_char();
        if (value == '\r' || value == '\n') {
            console_putc('\n');
            answer[length] = '\0';
            return ascii_equal(answer, "YES");
        }
        if ((value == '\b' || value == 0x7f) && length > 0u) {
            --length;
            console_putc('\b');
        } else if (value >= 0x20 && value <= 0x7e
            && length + 1u < sizeof(answer)) {
            answer[length++] = value;
            console_putc(value);
        }
    }
}

uint32_t read_install_line(char *buffer, uint32_t capacity)
{
    uint32_t length = 0u;
    for (;;) {
        char value = read_char();
        if (value == '\r' || value == '\n') {
            console_putc('\n');
            buffer[length] = '\0';
            return length;
        }
        if ((value == '\b' || value == 0x7f) && length > 0u) {
            --length;
            console_putc('\b');
        } else if (value >= 0x20 && value <= 0x7e && length + 1u < capacity) {
            buffer[length++] = value;
            console_putc(value);
        }
    }
}

bool confirm_yes_no(const char *prompt)
{
    char answer[8];
    for (;;) {
        console_write(prompt);
        uint32_t length = read_install_line(answer, sizeof(answer));
        if (length == 1u && (answer[0] == 'Y' || answer[0] == 'y')) {
            return true;
        }
        if (length == 1u && (answer[0] == 'N' || answer[0] == 'n')) {
            return false;
        }
        console_write("Please answer Y or N.\n");
    }
}

void print_install_disk(const struct disk_device *disk)
{
    console_write(disk->name);
    console_write("  ");
    console_write(disk->model);
    console_write("  ");
    print_unsigned(disk->sectors);
    console_write(" sectors (");
    print_unsigned(disk->sectors / 2048u);
    console_write(" MiB)");
}

bool select_install_disk(uint32_t *disk_index)
{
    uint32_t menu[MAX_DISKS];
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < disk_count && count < MAX_DISKS; ++i) {
        if (disks[i].sectors != 0u) {
            menu[count++] = i;
        }
    }
    if (count == 0u) {
        console_write("No disks were detected. No disk was modified.\n");
        return false;
    }
    console_write("Available disks:\n");
    for (uint32_t n = 0u; n < count; ++n) {
        console_write("  ");
        print_unsigned(n + 1u);
        console_write(") ");
        print_install_disk(&disks[menu[n]]);
        console_putc('\n');
    }
    for (;;) {
        char answer[8];
        uint32_t choice = 0u;
        console_write("Select the target disk number (1-");
        print_unsigned(count);
        console_write(", or Q to cancel): ");
        uint32_t length = read_install_line(answer, sizeof(answer));
        if (length == 1u && (answer[0] == 'Q' || answer[0] == 'q')) {
            return false;
        }
        if (parse_disk_number(answer, &choice) && choice >= 1u
            && choice <= count) {
            *disk_index = menu[choice - 1u];
            return true;
        }
        console_write("Invalid selection.\n");
    }
}

bool confirm_install_target(uint32_t disk_index)
{
    const struct disk_device *disk = &disks[disk_index];
    console_write("Selected disk: ");
    print_install_disk(disk);
    console_putc('\n');
    if (!confirm_yes_no("Is this the correct disk? (Y/N): ")) {
        return false;
    }
    console_write("WARNING: all existing partitions on ");
    console_write(disk->name);
    console_write(" will be deleted and all data on them will be lost.\n");
    if (!confirm_yes_no("Delete the existing partitions on this disk? (Y/N): ")) {
        return false;
    }
    console_write("BOOK8 will create a FAT32 root partition and a fixed 4 GiB raw swap partition across this disk.\n");
    return true;
}

static void set_all_install_apps(bool excluded)
{
    for (uint32_t i = 0u; i < BOOK8_MANIFEST_APP_COUNT; ++i)
        book8_install_app_excluded[i] = excluded ? 1u : 0u;
}

bool select_install_apps(void)
{
    char answer[16];
    uint32_t length;

    set_all_install_apps(false);
    console_write("Packaged applications:\n");
    for (uint32_t i = 0u; i < BOOK8_MANIFEST_APP_COUNT; ++i) {
        console_write("  ");
        print_unsigned(i + 1u);
        console_write(") ");
        console_write(book8_manifest_apps[i].name);
        console_write(" [");
        console_write(book8_manifest_apps[i].app_class);
        console_write("]\n");
    }
    for (;;) {
        console_write("Install applications ([A]ll default, [N]one, [S]elect, Q to cancel): ");
        length = read_install_line(answer, sizeof(answer));
        if (length == 0u || (length == 1u && (answer[0] == 'A' || answer[0] == 'a'))) {
            set_all_install_apps(false);
            return true;
        }
        if (length == 1u && (answer[0] == 'N' || answer[0] == 'n')) {
            set_all_install_apps(true);
            return true;
        }
        if (length == 1u && (answer[0] == 'S' || answer[0] == 's'))
            break;
        if (length == 1u && (answer[0] == 'Q' || answer[0] == 'q')) {
            set_all_install_apps(false);
            return false;
        }
        console_write("Enter A, N, S or Q.\n");
    }

    for (uint32_t i = 0u; i < BOOK8_MANIFEST_APP_COUNT; ++i) {
        for (;;) {
            console_write("Install ");
            print_unsigned(i + 1u);
            console_write(") ");
            console_write(book8_manifest_apps[i].name);
            console_write("? ([Y]es default, [N]o, Q to cancel): ");
            length = read_install_line(answer, sizeof(answer));
            if (length == 0u || (length == 1u
                && (answer[0] == 'Y' || answer[0] == 'y'))) {
                book8_install_app_excluded[i] = 0u;
                break;
            }
            if (length == 1u && (answer[0] == 'N' || answer[0] == 'n')) {
                book8_install_app_excluded[i] = 1u;
                break;
            }
            if (length == 1u && (answer[0] == 'Q' || answer[0] == 'q')) {
                set_all_install_apps(false);
                return false;
            }
            console_write("Enter Y, N or Q.\n");
        }
    }
    return true;
}

bool parse_disk_number(const char *text, uint32_t *disk_index)
{
    if (text == NULL || *text < '0' || *text > '9') {
        return false;
    }
    uint32_t value = 0u;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    if (*text != '\0') {
        return false;
    }
    *disk_index = value;
    return true;
}

uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

void install_report_progress(const char *operation, uint64_t completed,
    uint64_t total)
{
    console_write("[INSTALL] ");
    console_write(operation);
    console_write(": ");
    print_unsigned(total == 0u ? 100u
        : (uint32_t)(completed * 100u / total));
    console_write("%\n");
}

bool install_clear_sector_range(struct disk_device *disk, uint64_t lba,
    uint32_t sectors, const char *operation)
{
    __builtin_memset(install_transfer_buffer, 0,
        sizeof(install_transfer_buffer));
    uint32_t remaining = sectors;
    uint32_t completed = 0u;
    while (remaining > 0u) {
        uint32_t count = remaining > ATA_BOUNCE_SECTORS
            ? ATA_BOUNCE_SECTORS : remaining;
        if (!disk_transfer(disk, lba, count, install_transfer_buffer, true)) {
            return false;
        }
        lba += count;
        remaining -= count;
        completed += count;
        if ((completed % 16384u) == 0u || remaining == 0u) {
            install_report_progress(operation, completed, sectors);
        }
    }
    return true;
}

bool install_seed_default_config(void)
{
    struct book8_system_config defaults;
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    book8_config_defaults(&defaults);
    if (!book8_config_encode(&defaults, bytes)) {
        console_write("[INSTALL] could not encode the default desktop profile.\n");
        return false;
    }
    if (!fs_write_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes))) {
        console_write("[INSTALL] default desktop profile write failed: ");
        console_write(fs_error_text(fs_last_error()));
        console_write("\n");
        return false;
    }
    console_write("[INSTALL] default desktop profile created.\n");
    return true;
}

bool install_resize_fat32(struct disk_device *disk, uint32_t start_lba,
    uint32_t total_sectors)
{
    uint8_t boot[512];
    uint8_t sector[512];
    if (!disk_read_sector(disk, start_lba, boot)) {
        return false;
    }

    uint32_t old_total = read_u32_le(boot + 32u);
    uint32_t reserved = read_u16_le(boot + 14u);
    uint32_t fat_count = boot[16u];
    uint32_t old_fat_sectors = read_u32_le(boot + 36u);
    uint32_t sectors_per_cluster = boot[13u];
    uint32_t backup_boot = read_u16_le(boot + 50u);
    uint32_t fsinfo_sector = read_u16_le(boot + 48u);
    if (boot[510u] != 0x55u || boot[511u] != 0xaau
        || read_u16_le(boot + 11u) != 512u || old_total == 0u
        || old_total > total_sectors || reserved == 0u
        || fat_count != 2u || old_fat_sectors == 0u
        || sectors_per_cluster == 0u
        || (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0u
        || (uint64_t)reserved + (uint64_t)fat_count * old_fat_sectors
            >= old_total
        || backup_boot == 0u || backup_boot >= reserved
        || fsinfo_sector == 0u || fsinfo_sector >= reserved) {
        return false;
    }

    uint32_t new_fat_sectors = old_fat_sectors;
    bool fat_size_stable = false;
    uint64_t new_cluster_count = 0u;
    for (uint32_t pass = 0u; pass < 32u; ++pass) {
        uint64_t metadata_sectors = (uint64_t)reserved
            + (uint64_t)fat_count * new_fat_sectors;
        if (metadata_sectors >= total_sectors) {
            return false;
        }
        new_cluster_count = (total_sectors - metadata_sectors)
            / sectors_per_cluster;
        uint64_t needed = ((new_cluster_count + 2u) * 4u + 511u) / 512u;
        if (needed > UINT32_MAX) {
            return false;
        }
        if (needed == new_fat_sectors) {
            fat_size_stable = true;
            break;
        }
        new_fat_sectors = (uint32_t)needed;
    }
    if (!fat_size_stable || new_fat_sectors < old_fat_sectors
        || new_cluster_count < 65525u || new_cluster_count > 0x0ffffff5u
        || new_cluster_count + 2u
            > (uint64_t)new_fat_sectors * 512u / 4u) {
        return false;
    }

    uint64_t old_data_start = (uint64_t)start_lba + reserved
        + (uint64_t)fat_count * old_fat_sectors;
    uint64_t old_data_sectors = old_total - reserved
        - (uint64_t)fat_count * old_fat_sectors;
    uint64_t data_shift = (uint64_t)fat_count
        * (new_fat_sectors - old_fat_sectors);
    if (old_data_start + old_data_sectors + data_shift
        > (uint64_t)start_lba + total_sectors) {
        return false;
    }

    if (data_shift != 0u) {
        console_write("[INSTALL] relocating FAT32 data into expanded partition...\n");
        uint64_t remaining = old_data_sectors;
        uint64_t completed = 0u;
        while (remaining > 0u) {
            uint32_t count = remaining > ATA_BOUNCE_SECTORS
                ? ATA_BOUNCE_SECTORS : (uint32_t)remaining;
            uint64_t source_lba = old_data_start + remaining - count;
            if (!disk_transfer(disk, source_lba, count,
                    install_transfer_buffer, false)
                || !disk_transfer(disk, source_lba + data_shift, count,
                    install_transfer_buffer, true)) {
                return false;
            }
            remaining -= count;
            completed += count;
            if ((completed % 16384u) == 0u || remaining == 0u) {
                install_report_progress("FAT32 data relocation", completed,
                    old_data_sectors);
            }
        }
    }

    if (new_fat_sectors != old_fat_sectors) {
        console_write("[INSTALL] relocating FAT32 allocation table...\n");
        uint64_t old_second_fat = (uint64_t)start_lba + reserved
            + old_fat_sectors;
        uint64_t new_second_fat = (uint64_t)start_lba + reserved
            + new_fat_sectors;
        uint32_t remaining = old_fat_sectors;
        while (remaining > 0u) {
            uint32_t count = remaining > ATA_BOUNCE_SECTORS
                ? ATA_BOUNCE_SECTORS : remaining;
            uint64_t offset = remaining - count;
            if (!disk_transfer(disk, old_second_fat + offset, count,
                    install_transfer_buffer, false)
                || !disk_transfer(disk, new_second_fat + offset, count,
                    install_transfer_buffer, true)) {
                return false;
            }
            remaining -= count;
        }
        uint32_t added_fat_sectors = new_fat_sectors - old_fat_sectors;
        console_write("[INSTALL] initializing expanded FAT32 tables...\n");
        if (!install_clear_sector_range(disk,
                (uint64_t)start_lba + reserved + old_fat_sectors,
                added_fat_sectors, "FAT32 table 1")
            || !install_clear_sector_range(disk,
                (uint64_t)start_lba + reserved + new_fat_sectors
                    + old_fat_sectors,
                added_fat_sectors, "FAT32 table 2")) {
            return false;
        }
    }

    write_u32_le(boot + 28u, start_lba);
    write_u32_le(boot + 32u, total_sectors);
    write_u32_le(boot + 36u, new_fat_sectors);
    if (!disk_write_sector_unflushed(disk, start_lba, boot)
        || !disk_read_sector(disk, start_lba + backup_boot, sector)) {
        return false;
    }
    write_u32_le(sector + 28u, start_lba);
    write_u32_le(sector + 32u, total_sectors);
    write_u32_le(sector + 36u, new_fat_sectors);
    if (!disk_write_sector_unflushed(disk, start_lba + backup_boot, sector)) {
        return false;
    }

    uint64_t fsinfo_lba = (uint64_t)start_lba + fsinfo_sector;
    if (!disk_read_sector(disk, fsinfo_lba, sector)) {
        return false;
    }
    write_u32_le(sector + 488u, UINT32_MAX);
    if (!disk_write_sector_unflushed(disk, fsinfo_lba, sector)
        || !disk_read_sector(disk, fsinfo_lba + backup_boot, sector)) {
        return false;
    }
    write_u32_le(sector + 488u, UINT32_MAX);
    return disk_write_sector_unflushed(disk, fsinfo_lba + backup_boot, sector);
}

void install_set_mbr_partition(uint8_t *mbr, uint32_t index,
    uint8_t active, uint8_t type, uint32_t start_lba, uint32_t sectors)
{
    uint8_t *entry = mbr + 446u + index * 16u;
    for (uint32_t i = 0u; i < 16u; ++i) {
        entry[i] = 0u;
    }
    entry[0] = active;
    entry[1] = 0xffu;
    entry[2] = 0xffu;
    entry[3] = 0xffu;
    entry[4] = type;
    entry[5] = 0xffu;
    entry[6] = 0xffu;
    entry[7] = 0xffu;
    write_u32_le(entry + 8u, start_lba);
    write_u32_le(entry + 12u, sectors);
}

void install_command(const char *arguments)
{
    uint32_t disk_index;
    if (installer_image == NULL || installer_image_size < 512u
        || (installer_image_size & 511u) != 0u) {
        console_write("Installer payload is unavailable; boot the BOOK8 install ISO.\n");
        return;
    }
    console_write("[INSTALL] probing target disk list...\n");
    if (*arguments == '\0') {
        if (!select_install_disk(&disk_index)) {
            console_write("Installation cancelled; disk was not modified.\n");
            return;
        }
    } else if (!parse_disk_number(arguments, &disk_index)) {
        console_write("Usage: INSTALL (shows a numbered disk menu).\n");
        return;
    }
    if (disk_index >= disk_count || disks[disk_index].sectors == 0u) {
        console_write("Disk number is not present. No disk was modified.\n");
        return;
    }
    if (disks[disk_index].ahci && !disks[disk_index].lba48) {
        console_write("This AHCI disk lacks LBA48 support; installation is refused.\n");
        return;
    }

    uint64_t image_sectors = installer_image_size / 512u;
    const uint8_t *source_mbr = installer_image;
    uint64_t disk_sectors = disks[disk_index].sectors;
    if (source_mbr[510] != 0x55u || source_mbr[511] != 0xaau
        || source_mbr[446] != 0x80u || source_mbr[450] != 0x0cu) {
        console_write("Installer payload has no validated active FAT32 MBR partition.\n");
        return;
    }
    uint32_t source_start = read_u32_le(source_mbr + 454u);
    uint32_t source_sectors = read_u32_le(source_mbr + 458u);
    if (source_start != INSTALL_ROOT_START_LBA || source_sectors == 0u
        || (uint64_t)source_start + source_sectors > image_sectors
        || image_sectors > disk_sectors
        || disk_sectors > 0x100000000ull
        || disk_sectors < (uint64_t)INSTALL_ROOT_START_LBA
            + INSTALL_MIN_ROOT_SECTORS + INSTALL_SWAP_SECTORS) {
        console_write("Target disk needs at least a 2 GiB root plus 4 GiB raw swap and must fit MBR address limits.\n");
        return;
    }
    uint32_t root_sectors = (uint32_t)(disk_sectors
        - INSTALL_ROOT_START_LBA - INSTALL_SWAP_SECTORS);
    uint32_t swap_start = INSTALL_ROOT_START_LBA + root_sectors;
    const uint8_t *source_boot_sector = installer_image
        + (size_t)source_start * 512u;
    uint8_t cluster_sectors = source_boot_sector[13u];
    if (source_boot_sector[510u] != 0x55u || source_boot_sector[511u] != 0xaau
        || read_u16_le(source_boot_sector + 11u) != 512u
        || read_u32_le(source_boot_sector + 28u) != source_start
        || read_u32_le(source_boot_sector + 32u) == 0u
        || read_u32_le(source_boot_sector + 32u) > source_sectors
        || cluster_sectors == 0u
        || (cluster_sectors & (cluster_sectors - 1u)) != 0u
        || read_u16_le(source_boot_sector + 22u) != 0u
        || read_u32_le(source_boot_sector + 36u) == 0u) {
        console_write("Installer payload does not contain a valid FAT32 boot sector.\n");
        return;
    }
    console_write("[INSTALL] probing target disk...\n");
    if (!disk_read_sector(&disks[disk_index], 0u, sector_buffer)) {
        console_write("Could not read the target disk; installation was not started.\n");
        return;
    }
    if (!confirm_install_target(disk_index)) {
        console_write("Installation cancelled; disk was not modified.\n");
        return;
    }
    if (!select_install_apps()) {
        console_write("Installation cancelled; disk was not modified.\n");
        return;
    }

    console_write("Installing BIOS/UEFI BOOK8 image to ");
    console_write(disks[disk_index].name);
    console_write(". Do not power off during installation.\n");
    console_write("[INSTALL] copying installer image to target...\n");
    uint64_t lba = 0u;
    while (lba < image_sectors) {
        uint32_t count = image_sectors - lba > ATA_BOUNCE_SECTORS
            ? ATA_BOUNCE_SECTORS : (uint32_t)(image_sectors - lba);
        size_t bytes = (size_t)count * 512u;
        __builtin_memcpy(install_transfer_buffer,
            installer_image + (size_t)(lba * 512u), bytes);
        if (!disk_transfer(&disks[disk_index], lba, count,
                install_transfer_buffer, true)) {
            console_write("Install write failed at LBA ");
            print_unsigned(lba);
            console_write("; the disk may be partially installed.\n");
            return;
        }
        lba += count;
        if ((lba % 16384u) == 0u || lba == image_sectors) {
            install_report_progress("installer image copy", lba,
                image_sectors);
        }
    }
    console_write("[INSTALL] flushing copied image and expanding FAT32...\n");
    if (!disk_flush(&disks[disk_index])
        || !install_resize_fat32(&disks[disk_index], INSTALL_ROOT_START_LBA,
            root_sectors)) {
        console_write("FAT32 expansion failed; the target may be partially installed.\n");
        return;
    }
    console_write("[INSTALL] writing installed partition table...\n");
    if (!disk_read_sector(&disks[disk_index], 0u, sector_buffer)) {
        console_write("Could not read the target MBR after FAT32 expansion.\n");
        return;
    }
    install_set_mbr_partition(sector_buffer, 0u, 0x80u, 0x0cu,
        INSTALL_ROOT_START_LBA, root_sectors);
    install_set_mbr_partition(sector_buffer, 1u, 0u, 0x82u, swap_start,
        INSTALL_SWAP_SECTORS);
    sector_buffer[510] = 0x55u;
    sector_buffer[511] = 0xaau;
    if (!disk_write_sector_unflushed(&disks[disk_index], 0u, sector_buffer)
        || !disk_flush(&disks[disk_index])) {
        console_write("Could not write or flush the installed MBR.\n");
        return;
    }
    if (!disk_read_sector(&disks[disk_index], 0u, sector_buffer)
        || sector_buffer[510] != 0x55u || sector_buffer[511] != 0xaau
        || sector_buffer[450] != 0x0cu || sector_buffer[466] != 0x82u
        || read_u32_le(sector_buffer + 454u) != INSTALL_ROOT_START_LBA
        || read_u32_le(sector_buffer + 458u) != root_sectors
        || read_u32_le(sector_buffer + 470u) != swap_start
        || read_u32_le(sector_buffer + 474u) != INSTALL_SWAP_SECTORS) {
        console_write("Install verification failed at the MBR; do not reboot.\n");
        return;
    }
    if (!disk_read_sector(&disks[disk_index], INSTALL_ROOT_START_LBA,
            sector_buffer)
        || sector_buffer[510] != 0x55u || sector_buffer[511] != 0xaau
        || read_u32_le(sector_buffer + 32u) != root_sectors
        || read_u32_le(sector_buffer + 36u) == 0u
        || read_u32_le(sector_buffer + 28u) != INSTALL_ROOT_START_LBA) {
        console_write("Install verification failed at the expanded FAT32 boot sector.\n");
        return;
    }
    console_write("[INSTALL] mounting installed FAT32 root as A:...\n");
    uint32_t filesystem_disk = disk_index
        + (application_image != NULL ? 1u : 0u);
    if (!fs_mount_disk_as_root(filesystem_disk)) {
        console_write("The target was installed, but its FAT32 root could not be mounted; registry was not written.\n");
        return;
    }
    for (uint32_t i = 0u; i < BOOK8_MANIFEST_APP_COUNT; ++i) {
        char path[96];
        int length;
        if (book8_install_app_excluded[i] == 0u)
            continue;
        length = snprintf(path, sizeof(path), "A:\\BIN\\%s",
            book8_manifest_apps[i].target);
        if (length <= 0 || (size_t)length >= sizeof(path)
            || !fs_delete_entry(path)) {
            console_write("[INSTALL] could not remove deselected app ");
            console_write(book8_manifest_apps[i].name);
            console_write(".\n");
        }
    }
    registry_write_allowed = true;
    console_write("[INSTALL] seeding default desktop profile...\n");
    if (!install_seed_default_config()) {
        console_write("The target was installed, but the desktop profile could not be initialized.\n");
        return;
    }
    console_write("[INSTALL] writing initial registry state...\n");
    if (!reg_save()) {
        console_write("The target was installed, but initial registry save failed; retry with REG SAVE.\n");
        return;
    }
    console_write("BOOK8 installed and verified: FAT32 root plus 4 GiB raw swap. Remove the install medium and reboot.\n");
}
