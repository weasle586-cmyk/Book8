#include "kernel_internal.h"

/* Kernel entry point and subsystem initialization order. */

void kernel_main(uint32_t magic, uint32_t info_address)
{
    book8_boot_log_begin(BOOK8_DIAG_BOOT_LOG != 0);
    serial_init();
    serial_write("BOOK8-BOOT: kernel_main entered\r\n");
    boot_tsc = read_tsc();
    netboot_info_address = info_address;
    syscall_init();
    screen.pixels = NULL;
    screen.pitch_bytes = 0u;
    screen.width = 0u;
    screen.height = 0u;
    screen.scanout_pitch_bytes = 0u;
    screen.scanout_width = 0u;
    screen.scanout_height = 0u;
    screen.bytes_per_pixel = 0u;
    screen.red_position = 0u;
    screen.red_mask_size = 0u;
    screen.green_position = 0u;
    screen.green_mask_size = 0u;
    screen.blue_position = 0u;
    screen.blue_mask_size = 0u;
    use_vga = false;
    cursor_x = 0u;
    cursor_y = 0u;
    vga_cursor = 0u;
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC || info_address == 0u) {
        console_write("BOOK8: invalid Multiboot2 boot handoff.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    const uint32_t *info = (const uint32_t *)(uintptr_t)info_address;
    uint32_t total_size = info[0];
    bool framebuffer_tag_seen = false;
    smp_trampoline_page_usable = false;
    smp_trampoline_page_conflict =
        (uint64_t)info_address < 0x9000u
        && (uint64_t)info_address + total_size > SMP_TRAMPOLINE_PHYS;
    autostart_desktop = true;
    initialization_complete = false;
    cmdline_network_off = false;
    boot_relaunch_count = 0u;
    startup_quiet = false;
    if (total_size < 16u) {
        console_write("BOOK8: invalid Multiboot2 information block.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)((const uint8_t *)info + offset);
        if (tag->type == MULTIBOOT_TAG_END || tag->size < sizeof(*tag)
            || tag->size > total_size - offset) {
            break;
        }
        if (tag->type == MULTIBOOT_TAG_CMDLINE
            && tag->size > sizeof(*tag)) {
            const char *cmdline = (const char *)tag + sizeof(*tag);
            uint32_t cmdline_size = tag->size - (uint32_t)sizeof(*tag);
            if (cmdline_has_option(cmdline, cmdline_size,
                "book8.autostart=0")) {
                autostart_desktop = false;
            }
            cmdline_network_off = cmdline_has_option(cmdline, cmdline_size,
                "book8.net=off");
            boot_relaunch_count = book8_relaunch_count(cmdline, cmdline_size);
            book8_splash_cycles = book8_splash_cycles_option(cmdline,
                cmdline_size);
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            break;
        }
        offset += next;
    }
    startup_quiet = autostart_desktop;

    pcie_bootstrap();
    smbios_discover((const uint8_t *)info, total_size);
    acpi_discover((const uint8_t *)info, total_size);
    if (!acpi_s5_available) {
        acpi_discover_firmware();
    }
    book8_pcie_enumerate(&pcie);

    for (uint32_t offset = 8u;
        offset <= total_size - sizeof(struct multiboot_tag);) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)((const uint8_t *)info + offset);
        if (tag->type == MULTIBOOT_TAG_END || tag->size < sizeof(*tag)
            || tag->size > total_size - offset) {
            break;
        }
        if (tag->type == MULTIBOOT_TAG_MMAP && tag->size >= 16u) {
            const uint32_t *words = (const uint32_t *)tag;
            uint32_t entry_size = words[2];
            uint64_t total = 0u;
            for (uint32_t pos = 16u; entry_size >= 24u
                && pos + entry_size <= tag->size; pos += entry_size) {
                const uint8_t *entry = (const uint8_t *)tag + pos;
                const uint64_t *fields = (const uint64_t *)entry;
                uint64_t region_end = fields[0] + fields[1];
                if (*(const uint32_t *)(entry + 16u) == 1u
                    && fields[0] <= SMP_TRAMPOLINE_PHYS
                    && region_end >= SMP_TRAMPOLINE_PHYS + 4096u)
                    smp_trampoline_page_usable = true;
                if (control_memory_count < 12u) {
                    control_memory_map[control_memory_count].base = fields[0];
                    control_memory_map[control_memory_count].size = fields[1];
                    control_memory_map[control_memory_count].type =
                        *(const uint32_t *)(entry + 16u);
                    ++control_memory_count;
                }
                if (*(const uint32_t *)(entry + 16u) == 1u) {
                    total += fields[1] / 1024u;
                }
            }
            if (total != 0u) {
                memory_total_kb = total;
            }
        } else if (tag->type == MULTIBOOT_TAG_BASIC_MEMINFO
            && tag->size >= 16u && memory_total_kb == 0u) {
            const uint32_t *words = (const uint32_t *)tag;
            memory_total_kb = (uint64_t)words[2] + words[3] + 1024u;
        } else if (tag->type == MULTIBOOT_TAG_MODULE
            && tag->size >= sizeof(struct multiboot_tag_module)) {
            const struct multiboot_tag_module *module =
                (const struct multiboot_tag_module *)tag;
            if (module->start < SMP_TRAMPOLINE_PHYS + 4096u
                && module->end > SMP_TRAMPOLINE_PHYS)
                smp_trampoline_page_conflict = true;
            const char *name_end = module->command_line;
            const char *tag_end = (const char *)tag + tag->size;
            while (name_end < tag_end && *name_end != '\0') {
                ++name_end;
            }
            if (name_end < tag_end && installer_image == NULL
                && ascii_equal(module->command_line, "book8-installer")
                && module->end > module->start) {
                installer_image = (const uint8_t *)(uintptr_t)module->start;
                installer_image_size = module->end - module->start;
            }
            if (name_end < tag_end && application_image == NULL
                && ascii_equal(module->command_line, "book8-apps")
                && module->end > module->start) {
                uint64_t size = module->end - module->start;
                if ((size & 511u) == 0u && size <= 16u * 1024u * 1024u) {
                    application_image = (uint8_t *)(uintptr_t)module->start;
                    application_image_size = size;
                } else {
                    console_write("BOOK8-APPS: invalid application module size.\n");
                }
            }
            if (name_end < tag_end && splash_image == NULL
                && ascii_equal(module->command_line, "book8-splash")
                && module->end > module->start) {
                splash_image = (const uint8_t *)(uintptr_t)module->start;
                splash_image_size = module->end - module->start;
            }
            if (name_end < tag_end
                && ascii_equal(module->command_line, "book8-hashes")
                && module->end > module->start) {
                if (!boot_hash_manifest_load(
                        (const uint8_t *)(uintptr_t)module->start,
                        module->end - module->start)) {
                    serial_write("BOOK8-HASH: invalid manifest module\r\n");
                }
            }
        } else if (tag->type == MULTIBOOT_TAG_FRAMEBUFFER
            && tag->size >= MULTIBOOT_FRAMEBUFFER_RGB_TAG_SIZE) {
            framebuffer_tag_seen = true;
            const struct multiboot_tag_framebuffer *fb =
                (const struct multiboot_tag_framebuffer *)tag;
            uint8_t pixel_bytes =
                (uint8_t)((fb->bits_per_pixel + 7u) / 8u);
            if (fb->framebuffer_type == 1u
                && (fb->bits_per_pixel == 24u
                    || fb->bits_per_pixel == 32u)
                && (fb->address >> 32u) == 0u && fb->width != 0u
                && fb->height != 0u
                && (uint64_t)fb->pitch
                    >= (uint64_t)fb->width * pixel_bytes
                && (uint64_t)fb->pitch * fb->height
                    <= FRAMEBUFFER_MAX_BYTES
                && (uint64_t)fb->pitch * fb->height
                    <= 0x100000000ull - fb->address
                && fb->red_position < 32u && fb->green_position < 32u
                && fb->blue_position < 32u) {
                if (fb->red_mask_size == 0u || fb->red_mask_size > 8u
                    || fb->green_mask_size == 0u
                    || fb->green_mask_size > 8u
                    || fb->blue_mask_size == 0u
                    || fb->blue_mask_size > 8u
                    || fb->red_position + fb->red_mask_size
                        > fb->bits_per_pixel
                    || fb->green_position + fb->green_mask_size
                        > fb->bits_per_pixel
                    || fb->blue_position + fb->blue_mask_size
                        > fb->bits_per_pixel) {
                    screen.pixels = NULL;
                    use_vga = true;
                    break;
                }
                uint32_t red_mask =
                    ((1u << fb->red_mask_size) - 1u) << fb->red_position;
                uint32_t green_mask =
                    ((1u << fb->green_mask_size) - 1u) << fb->green_position;
                uint32_t blue_mask =
                    ((1u << fb->blue_mask_size) - 1u) << fb->blue_position;
                if ((red_mask & green_mask) != 0u
                    || (red_mask & blue_mask) != 0u
                    || (green_mask & blue_mask) != 0u) {
                    screen.pixels = NULL;
                    use_vga = true;
                    break;
                }
                /* boot.S identity maps physical 0..4 GiB; the validated range
                 * is therefore also the framebuffer's kernel virtual mapping. */
                screen.pixels = (volatile uint8_t *)(uintptr_t)fb->address;
                screen.pitch_bytes = fb->pitch;
                screen.width = fb->width;
                screen.height = fb->height;
                screen.scanout_pitch_bytes = fb->pitch;
                screen.scanout_width = fb->width;
                screen.scanout_height = fb->height;
                screen.bytes_per_pixel = pixel_bytes;
                screen.red_position = fb->red_position;
                screen.red_mask_size = fb->red_mask_size;
                screen.green_position = fb->green_position;
                screen.green_mask_size = fb->green_mask_size;
                screen.blue_position = fb->blue_position;
                screen.blue_mask_size = fb->blue_mask_size;
                use_vga = false;
                if (!fb_init(&fbdev, (void *)screen.pixels, screen.width,
                        screen.height, screen.pitch_bytes,
                        &(struct fb_format){
                            .bpp = fb->bits_per_pixel,
                            .red_pos = fb->red_position,
                            .red_size = fb->red_mask_size,
                            .green_pos = fb->green_position,
                            .green_size = fb->green_mask_size,
                            .blue_pos = fb->blue_position,
                            .blue_size = fb->blue_mask_size})) {
                    screen.pixels = NULL;
                    use_vga = true;
                } else if ((uint64_t)fbdev.pitch * fbdev.height
                    <= GUI_BACKBUFFER_BYTES) {
                    fb_set_backbuffer(&fbdev, gui_backbuffer);
                }
            } else {
                /* Some real BIOS/VBE implementations advertise a framebuffer but
                 * do not support the requested handshake or pixel layout.
                 * Falling back to VGA text avoids a hard lockup on unsupported
                 * mode negotiation. */
                screen.pixels = NULL;
                use_vga = true;
            }
            break;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total_size - offset) {
            break;
        }
        offset += next;
    }

    if (boot_hash_manifest_valid()) {
        char hash_text[96];
        snprintf(hash_text, sizeof(hash_text),
            "BOOK8-HASH: manifest loaded %u entries\r\n",
            boot_hash_manifest_count());
        serial_write(hash_text);
    } else {
        serial_write("BOOK8-HASH: no usable manifest module\r\n");
    }

    smp_trampoline_page_usable &= !smp_trampoline_page_conflict;
    if (vmm_init(info))
        serial_write("BOOK8-VMM: kernel page arena ready\r\n");
    else
        serial_write("BOOK8-VMM: unavailable; kernel continues without arena\r\n");
    startup_quiet = autostart_desktop && installer_image == NULL
        && screen.pixels != NULL;
    char handoff[128];
    if (screen.pixels != NULL) {
        snprintf(handoff, sizeof(handoff),
            "BOOK8 framebuffer handoff: %ux%ux%u pitch=%u address=0x%08x",
            screen.width, screen.height, screen.bytes_per_pixel * 8u,
            screen.pitch_bytes, (uint32_t)(uintptr_t)screen.pixels);
    } else {
        snprintf(handoff, sizeof(handoff), "BOOK8 framebuffer handoff: %s",
            framebuffer_tag_seen ? "tag rejected; VGA fallback."
                : "tag missing; VGA fallback.");
    }
    if (startup_quiet) {
        serial_write(handoff);
        serial_write("\r\n");
    } else {
        console_write(handoff);
        console_write("\n");
    }

    if (screen.pixels == NULL) {
        use_vga = true;
        VGA_MEMORY[0] = 0x0f4fu;
        VGA_MEMORY[1] = 0x0f42u;
        VGA_MEMORY[2] = 0x0f41u;
        VGA_MEMORY[3] = 0x0f4du;
        VGA_MEMORY[4] = 0x0f45u;
        VGA_MEMORY[5] = 0x0f42u;
        VGA_MEMORY[6] = 0x0f41u;
        VGA_MEMORY[7] = 0x0f43u;
        VGA_MEMORY[8] = 0x0f4bu;
        VGA_MEMORY[9] = 0x0f3au;
        VGA_MEMORY[10] = 0x0f20u;
        VGA_MEMORY[11] = 0x0f56u;
        VGA_MEMORY[12] = 0x0f47u;
        VGA_MEMORY[13] = 0x0f41u;
        VGA_MEMORY[14] = 0x0f20u;
        VGA_MEMORY[15] = 0x0f20u;
        console_clear();
        console_write("BOOK8: VGA fallback active; framebuffer rejected or missing.\n");
    }
    if (screen.pixels != NULL) {
        (void)ps2_mouse_initialize();
    }
    discover_storage();
    network_early_telemetry();
    register_block_devices();
    swap_init(memory_total_kb * 1024u);
    if (swap_available()) {
        serial_write("BOOK8-SWAP: raw partition enabled, slots=");
        char swap_slots_text[24];
        snprintf(swap_slots_text, sizeof(swap_slots_text), "%u",
            swap_total_slots());
        serial_write(swap_slots_text);
        serial_write("\r\n");
    } else {
        serial_write("BOOK8-SWAP: no valid raw partition; swapping disabled\r\n");
    }
    ps2_keyboard_controller_ready = ps2_keyboard_initialize();
    pointer_x = screen.width != 0u ? (int32_t)(screen.width / 2u) : 0;
    pointer_y = screen.height != 0u ? (int32_t)(screen.height / 2u) : 0;
    pointer_buttons = 0u;
    usb_key_queue_head = 0u;
    usb_key_queue_tail = 0u;
    usb_previous_keyboard_report = (struct book8_usb_keyboard_report){0};
    usb_keyboard_leds = 0u;
    fs_initialize(filesystem_read_sector, filesystem_write_sector,
        filesystem_disk_size, filesystem_print, confirm_text);
    serial_write("BOOK8-BOOT: filesystem initialization complete\r\n");
    fs_set_drive_resolver(exec_drive_extent);
    registry_write_allowed = installer_image == NULL;
    reg_set_storage(fs_read_file, registry_write_file);
    serial_write("BOOK8-BOOT: registry load started\r\n");
    bool registry_loaded = reg_load();
    serial_write("BOOK8-BOOT: registry load finished\r\n");
    if (!registry_loaded) {
        console_write("REGISTRY: file unavailable or invalid; using defaults.\n");
    }
    registry_seed_defaults();
    uint64_t session_seed = pit_clock_ticks() ^ read_tsc()
        ^ (uint64_t)(uintptr_t)screen.pixels;
    if (!book8_session_initialize(session_seed)) {
        console_write("SESSION: initialization failed; desktop sign-in will remain locked.\n");
    }
    bool usb_host_module_ready = usb_host_module_load();
    bool rtl8168_module_ready = load_trusted_driver(
        "A:\\DRIVERS\\RTL8168.DRV", &rtl8168_driver) == 0;
    bool hda_module_ready = hda_module_load();
    (void)book8_pcie_bind_bus_drivers(&pcie, bus_drivers,
        sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    if (usb_host_module_ready) {
        book8_usb_register_bus_drivers(bus_drivers,
            sizeof(bus_drivers) / sizeof(bus_drivers[0]));
        /* Autostart skips the shell's PNPSCAN, so perform the same PCI/USB
         * rescan here. Otherwise UHCI/OHCI HID devices are not enumerated and
         * the desktop receives keyboard events but no mouse events. */
        (void)book8_pcie_rescan(&pcie, bus_drivers,
            sizeof(bus_drivers) / sizeof(bus_drivers[0]));
        book8_usb_rescan(&pcie);
        serial_write("BOOK8-USB: module initialized\r\n");
    } else {
        serial_write("BOOK8-USB: module unavailable; USB input disabled\r\n");
    }
    if (rtl8168_module_ready) {
        bool rtl8168_found = false;
        for (size_t i = 0u; i < pcie.node_count; ++i) {
            const struct book8_pcie_node *node = &pcie.nodes[i];
            if (node->segment == 0u
                && node->bus_device.vendor_id == RTL8168_VENDOR_ID
                && node->bus_device.device_id == RTL8168_DEVICE_ID
                && (node->bus_device.class_code & 0xff0000u) == 0x020000u) {
                rtl8168_found = true;
                break;
            }
        }
        if (!rtl8168_found)
            serial_write("BOOK8-RTL8168: module loaded; no matching NIC found\r\n");
    } else {
        serial_write("BOOK8-RTL8168: module unavailable; network disabled\r\n");
    }
    if (hda_module_ready) {
        bool hda_found = false;
        for (size_t i = 0u; i < pcie.node_count; ++i) {
            const struct book8_pcie_node *node = &pcie.nodes[i];
            if (node->segment == 0u
                && node->bus_device.vendor_id == HDA_ATI_VENDOR_ID
                && node->bus_device.device_id == HDA_ATI_DEVICE_ID
                && (node->bus_device.class_code & 0xffff00u) == 0x040300u) {
                hda_found = true;
                break;
            }
        }
        if (!hda_found)
            serial_write("BOOK8-HDA: module loaded; no matching controller found\r\n");
        else if (!hda_controller.present)
            serial_write("BOOK8-HDA: matching controller initialization failed; audio disabled\r\n");
    } else {
        serial_write("BOOK8-HDA: module unavailable; audio disabled\r\n");
    }
    serial_write("BOOK8-BOOT: USB initialization complete\r\n");
    serial_write("BOOK8-BOOT: PCI registry synchronization started\r\n");
    bool pcie_registry_ok = book8_pcie_registry_sync(&pcie);
    serial_write("BOOK8-BOOT: PCI registry synchronization finished\r\n");
    bool usb_registry_ok = false;
    if (pcie_registry_ok) {
        serial_write("BOOK8-BOOT: USB registry synchronization started\r\n");
        usb_registry_ok = book8_usb_registry_sync();
        serial_write("BOOK8-BOOT: USB registry synchronization finished\r\n");
    }
    bool registry_saved = !registry_write_allowed;
    if (pcie_registry_ok && usb_registry_ok && registry_write_allowed) {
        serial_write("BOOK8-BOOT: registry save started\r\n");
        registry_saved = reg_save();
        serial_write("BOOK8-BOOT: registry save finished\r\n");
    }
    if (!pcie_registry_ok || !usb_registry_ok || !registry_saved) {
        console_write("REGISTRY: initial hardware sync/save failed.\n");
    } else if (!registry_write_allowed) {
        console_write("REGISTRY: persistent save deferred until INSTALL mounts the target root.\n");
    }
    serial_write("BOOK8-BOOT: registry initialization complete\r\n");
#if BOOK8_DIAG_BOOT_LOG
    if (!book8_boot_log_mount(application_image != NULL ? 1u : 0u)) {
        console_write("BOOK8-NVLOG: ERROR "); console_write(book8_boot_log_error()); console_write("\n");
    } else {
        console_write("BOOK8-NVLOG: writing initialization checkpoints to ");
        console_write(book8_boot_log_path()); console_write("\n");
    }
#endif
    network_boot_configure();
    if (pcie.target_present && !network_calibrate_clock()) {
        pcie_log("PCIE1 ERROR clock calibration unavailable; D0 transitions may fail");
    }
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: BEGIN PCIe target preparation\n");
#endif
    (void)book8_pcie_prepare_target(&pcie);
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: END PCIe target preparation; BEGIN NVIDIA probe\n");
#endif
    nv_gpu_init();
    record_nv_gpu_bars();
    if (!book8_pcie_registry_sync(&pcie) || !nv_registry_sync()
        || (registry_write_allowed && !reg_save())) {
        console_write("REGISTRY: post-GPU BAR sync/save failed.\n");
    }
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: END NVIDIA probe\n");
#endif
    hwdiag_init(&nv_gpu);
    initialization_complete = true;
    serial_write("BOOK8-BOOT: initialization complete; entering shell\r\n");
#if BOOK8_DIAG_BOOT_LOG
    console_write("BOOK8-NVLOG: END initialization; entering shell\n");
    if (!book8_boot_log_finish()) {
        console_write("BOOK8-NVLOG: ERROR "); console_write(book8_boot_log_error()); console_write("\n");
    } else {
        console_write("BOOK8-NVLOG: saved "); console_write(book8_boot_log_path()); console_write("\n");
    }
#endif
    shell();
}
