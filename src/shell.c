#include "kernel_internal.h"

/* Interactive shell, command dispatch and registry seeding. */

void diag_marker(const char *tag)
{
    if (BOOK8_DIAG_MARKERS == 0) {
        return;
    }
    serial_write("BOOK8-DIAG: ");
    serial_write(tag);
    serial_write("\r\n");
    if (BOOK8_DIAG_MARKERS >= 2) {
        console_putc('[');
        for (const char *c = tag; *c != '\0' && *c != ' '; ++c) {
            console_putc(*c);
        }
        console_putc(']');
    }
}

char read_char(void)
{
    static bool first_poll = true;
    for (;;) {
        network_rx_poll();
        if (first_poll) {
            diag_marker("M3 read_char entered");
        }
        if (serial_ready()) {
            char value = (char)inb(COM1);
            return value == '\r' ? '\n' : value;
        }
        if (first_poll) {
            diag_marker("S serial polled");
        }
        uint32_t usb_key;
        if (usb_keyboard_get_key(&usb_key)) {
            switch (usb_key) {
            case BOOK8GUI_KEY_ENTER: return '\n';
            case BOOK8GUI_KEY_BACKSPACE: return '\b';
            case BOOK8GUI_KEY_DELETE: return '\b';
            case BOOK8GUI_KEY_ESCAPE: return '\x1b';
            case BOOK8GUI_KEY_TAB: return '\t';
            default:
                if (usb_key <= 0x7fu) {
                    return (char)usb_key;
                }
                break;
            }
        }
        if (first_poll) {
            diag_marker("U usb polled");
        }
        uint8_t status = inb(0x64u);
        if (first_poll) {
            diag_marker("P ps2 polled");
            first_poll = false;
        }
        if ((status & 1u) != 0u) {
            uint8_t scan = inb(0x60u);
            if ((status & 0x20u) != 0u) {
                continue;
            }
            char value = keyboard_char(scan);
            if (value != 0) {
                return value;
            }
        }
        __asm__ volatile ("pause");
    }
}

void print_prompt(void)
{
    for (size_t i = 0u; prompt_format[i] != '\0'; ++i) {
        if (prompt_format[i] != '$' || prompt_format[i + 1u] == '\0') {
            console_putc(prompt_format[i]);
            continue;
        }
        char code = prompt_format[++i];
        if (code == 'P' || code == 'p') {
            console_write(fs_current_path());
        } else if (code == 'G' || code == 'g') {
            console_putc('>');
        } else if (code == 'V' || code == 'v') {
            console_write("BOOK8");
        } else if (code == '$') {
            console_putc('$');
        } else {
            console_putc('$');
            console_putc(code);
        }
    }
    console_present();
}

void print_help(void)
{
    console_write("BOOK8 x86-64 commands:\n");
    console_write("  HELP ? VER CLS ECHO DATE TIME DISK READ WRITE MOUNT VOL DIR TYPE NVINFO\n");
    console_write("  WRITEFILE COPY DEL FORMAT FDISK INSTALL CD CHDIR\n");
    console_write("  CD CHDIR COPY DEL ERASE MD MKDIR RD RMDIR SET PATH PROMPT\n");
    console_write("  REM ATTRIB CHKDSK DEFRAG EDIT FDISK FORMAT MOVE SCANDISK SYS XCOPY\n");
    console_write("  PCIDUMP LSPCI HWDIAG PNPSCAN MEMDUMP <hex_addr> [len] IOPORT <in|out> <hex_port> [width|hex_val]\n");
    console_write("  REG QUERY <path> | REG ADD <path> <key> <value> | REG SAVE\n");
    console_write("  IOPORT out performs a privileged hardware write; PCI BAR sizes may be unknown unless safely measured.\n");
    console_write("  USBSTAT INPUTSTAT THREADS SWAPTEST USBREG LIBTEST RING3TEST SCHEDTEST SCHEDUSER SMPTEST VMTEST MSRGUARDTEST EXEC BOOK8GUI HALT REBOOT RESET\n");
    console_write("  PCIEPROBE <A2/A5..AF>|LTSSM|LINKWIDTH|LINKCAP|LINKSTATUS|STATUS|RETRY\n");
    console_write("  ACK <id> INTENT|RESULT (verify collector receipt before each ACK)\n");
    console_write("  NETSTAT NETCONFIG <ip> <mask> <gateway> | OFF | DHCP | RENEW\n");
    console_write("  TELEMETRY <target-ip> | TELEMETRY OFF (UDP 8080, once/second)\n");
    console_write("  NETBOOT <tftp-ip> <warm-kernel-file> (trusted images only)\n");
    console_write("  NETSAVE (save DHCP/static mode and fallback to A:\\BOOK8DHCP.CFG)\n");
    console_write("  RELAUNCH <kernel.elf> (restart a BOOK8 Multiboot2 ELF from A: in RAM)\n");
    console_write("FAT16/FAT32 + VFAT/LFN supported; NTFS root-file support is limited.\n");
    console_write("FORMAT/FDISK/INSTALL erase data; use disposable disks and read prompts carefully.\n");
    console_write("INSTALL creates a FAT32 root and 4 GiB raw swap partition after Y/N confirmations.\n");
    console_write("BOOK8GUI opens the framebuffer explorer and command window on request.\n");
}

void nvinfo_command(void)
{
    if (!nv_gpu.present || !nv_gpu.mapped) {
        console_write("NVINFO: GM204 telemetry unavailable (device not mapped).\n");
        return;
    }
    nv_driver_refresh_state();
    console_write("NVINFO: GM204 10DE:");
    print_hex8((uint8_t)(nv_gpu.device_id >> 8u));
    print_hex8((uint8_t)nv_gpu.device_id);
    console_write(" at ");
    print_hex8(nv_gpu.bus);
    console_putc(':');
    print_hex8(nv_gpu.slot);
    console_putc('.');
    print_hex8(nv_gpu.function);
    console_write(" BAR0 UC=0x");
    print_hex64(nv_gpu.bar0_phys);
    console_write(" BAR1 WB=0x");
    print_hex64(nv_gpu.bar1_phys);
    console_putc('\n');

    console_write("PMC_BOOT_0=0x");
    print_hex32(nv_gpu.boot0);
    console_write(" ChipRev=");
    print_unsigned(nv_gpu.chip_rev);
    console_write(" Foundry=0x");
    print_hex8(nv_gpu.foundry_id);
    console_write(" PMC_ENABLE=0x");
    print_hex32(nv_gpu.pmc_enable);
    console_write(" [PFIFO=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PFIFO) != 0u);
    console_write(" PGRAPH=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PGRAPH) != 0u);
    console_write(" PDISP=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PDISP) != 0u);
    console_write(" PCRTC=");
    print_unsigned((nv_gpu.pmc_enable & NV_PMC_ENABLE_PCRTC) != 0u);
    console_write("]\n");

    console_write("PMC_INTR_0=0x");
    print_hex32(nv_gpu.pmc_intr);
    console_write(" PMC_INTR_EN_0=0x");
    print_hex32(nv_gpu.pmc_intr_en_0);
    console_write(" PFB_CONFIG=0x");
    print_hex32(nv_gpu.pfb_config);
    console_write(" PFB_CSTATUS=0x");
    print_hex32(nv_gpu.pfb_cstatus);
    console_write(" DetectedVRAM_MB=");
    print_unsigned(nv_gpu.detected_vram_mb);
    console_putc('\n');

    console_write("PDISP head mask=0x");
    print_hex32(nv_gpu.pdisp_head_mask);
    console_write(" active head=");
    if (nv_gpu.pdisp_active_head == UINT32_MAX) {
        console_write("none");
    } else {
        print_unsigned(nv_gpu.pdisp_active_head);
        console_write(" timing total=");
        print_unsigned(nv_gpu.pdisp_htotal);
        console_putc('x');
        print_unsigned(nv_gpu.pdisp_vtotal);
    }
    console_putc('\n');
    if (nv_gpu.scanout.valid) {
        console_write("ScanoutBase=0x");
        print_hex64(nv_gpu.scanout.physical_base);
        console_write(" ");
        print_unsigned(nv_gpu.scanout.width);
        console_putc('x');
        print_unsigned(nv_gpu.scanout.height);
        console_write(" stride=");
        print_unsigned(nv_gpu.scanout.pitch);
        console_write(" bytes\n");
    } else {
        console_write("ScanoutBase=unavailable (no validated firmware framebuffer handoff)\n");
    }
}

void print_usb_poll_status(void)
{
    struct book8_usb_diagnostics diagnostics;
    book8_usb_get_diagnostics(&diagnostics);
    console_write("USB HID poll (active/completions/stalls): ");
    print_unsigned(diagnostics.active_hid_devices);
    console_putc('/');
    print_unsigned(diagnostics.uhci_interrupt_completions);
    console_putc('/');
    print_unsigned(diagnostics.uhci_interrupt_stalls);
    console_putc('\n');
    for (uint32_t i = 0u; i < diagnostics.uhci_diagnostic_count; ++i) {
        const struct book8_usb_uhci_diagnostic *detail =
            &diagnostics.uhci[i];
        console_write("UHCI[");
        print_unsigned(i);
        console_write("] PCI=");
        print_hex8((uint8_t)detail->bus);
        console_putc(':');
        print_hex8((uint8_t)detail->slot);
        console_putc('.');
        print_hex8((uint8_t)detail->function);
        console_write(" BAR=");
        print_hex32(detail->pci_bar);
        console_write(" CMD=");
        print_hex8((uint8_t)(detail->pci_command >> 8u));
        print_hex8((uint8_t)detail->pci_command);
        console_putc('\n');
        console_write("  io=");
        print_hex32(detail->io_base);
        console_write(" sts=");
        print_hex8((uint8_t)(detail->controller_status >> 8u));
        print_hex8((uint8_t)detail->controller_status);
        console_write(" frame=");
        print_hex8((uint8_t)(detail->frame_number >> 8u));
        print_hex8((uint8_t)detail->frame_number);
        console_write(" ports=");
        print_hex8((uint8_t)(detail->port_status[0] >> 8u));
        print_hex8((uint8_t)detail->port_status[0]);
        console_putc('/');
        print_hex8((uint8_t)(detail->port_status[1] >> 8u));
        print_hex8((uint8_t)detail->port_status[1]);
        console_putc('\n');
    }
    console_write("SWAP used/total slots: ");
    print_unsigned(swap_used_slots());
    console_putc('/');
    print_unsigned(swap_total_slots());
    console_write("  VMM resident/swapped pages: ");
    print_unsigned(vmm_page_count());
    console_putc('/');
    print_unsigned(vmm_swapped_page_count());
    console_putc('\n');
}

void print_input_status(void)
{
    uint64_t flags = irq_save();
    uint32_t queue_depth = (mouse_event_head - mouse_event_tail)
        & (MOUSE_EVENT_QUEUE_SIZE - 1u);
    uint32_t irq_count = ps2_mouse_irq_count;
    uint32_t timer_count = timer_irq_count;
    uint32_t packet_count = ps2_mouse_packet_count;
    uint32_t ps2_events = ps2_mouse_queue_events;
    uint32_t ps2_drops = ps2_mouse_queue_drops;
    uint32_t usb_reports = usb_mouse_report_count;
    uint32_t usb_events = usb_mouse_queue_events;
    uint32_t usb_drops = usb_mouse_queue_drops;
    irq_restore(flags);

    console_write("BOOK8-INPUT: IRQ12=");
    print_unsigned(irq_count);
    console_write(" IRQ0=");
    print_unsigned(timer_count);
    console_write(" PS2_PACKETS=");
    print_unsigned(packet_count);
    console_write(" PS2_QUEUED=");
    print_unsigned(ps2_events);
    console_write(" PS2_QUEUE_DROPS=");
    print_unsigned(ps2_drops);
    console_write(" USB_REPORTS=");
    print_unsigned(usb_reports);
    console_write(" USB_QUEUED=");
    print_unsigned(usb_events);
    console_write(" USB_QUEUE_DROPS=");
    print_unsigned(usb_drops);
    console_write(" QUEUE_DEPTH=");
    print_unsigned(queue_depth);
    console_putc('\n');
}

bool parse_integer(const char *text, uint8_t base, uint64_t *value)
{
    uint64_t parsed = 0u;
    bool has_digit = false;

    if (base == 16u && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    while (*text != '\0') {
        uint8_t digit;
        if (*text >= '0' && *text <= '9') {
            digit = (uint8_t)(*text - '0');
        } else if (base == 16u && *text >= 'a' && *text <= 'f') {
            digit = (uint8_t)(*text - 'a' + 10);
        } else if (base == 16u && *text >= 'A' && *text <= 'F') {
            digit = (uint8_t)(*text - 'A' + 10);
        } else {
            return false;
        }
        if (digit >= base || parsed > (UINT64_MAX - digit) / base) {
            return false;
        }
        parsed = parsed * base + digit;
        has_digit = true;
        ++text;
    }
    if (!has_digit) {
        return false;
    }
    *value = parsed;
    return true;
}

char *next_command_token(char **cursor)
{
    char *token = skip_spaces(*cursor);
    char *end = token;
    while (*end != '\0' && *end != ' ' && *end != '\t') {
        ++end;
    }
    if (*end != '\0') {
        *end++ = '\0';
    }
    *cursor = skip_spaces(end);
    return *token == '\0' ? NULL : token;
}

bool memdump_page_mapped(uint64_t address)
{
    uint64_t cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if ((cr0 & (1ull << 31u)) == 0u || (cr4 & (1ull << 12u)) != 0u) {
        return false;
    }

    uint64_t table_phys = cr3 & 0x000ffffffffff000ull;
    static const uint8_t shifts[] = {39u, 30u, 21u, 12u};
    for (uint32_t level = 0u; level < 4u; ++level) {
        if (table_phys >= 0x100000000ull - 4096u) {
            return false;
        }
        const volatile uint64_t *table =
            (const volatile uint64_t *)(uintptr_t)table_phys;
        uint16_t index = (uint16_t)((address >> shifts[level]) & 0x1ffu);
        uint64_t entry = table[index];
        if ((entry & 1u) == 0u) {
            return false;
        }
        if ((level == 1u || level == 2u) && (entry & (1ull << 7u)) != 0u) {
            return true;
        }
        if (level == 3u) {
            return true;
        }
        table_phys = entry & 0x000ffffffffff000ull;
    }
    return false;
}

bool memdump_range_mapped(uint64_t address, uint32_t length)
{
    if (length == 0u || address >= 0x100000000ull
        || length > 0x100000000ull - address) {
        return false;
    }
    uint64_t page = address & ~0xfffull;
    uint64_t last_page = (address + length - 1u) & ~0xfffull;
    for (;;) {
        if (!memdump_page_mapped(page)) {
            return false;
        }
        if (page == last_page) {
            return true;
        }
        page += 0x1000u;
    }
}

void memdump_command(char *arguments)
{
    char *address_text = next_command_token(&arguments);
    char *length_text = next_command_token(&arguments);
    uint64_t address, parsed_length = 64u;
    uint32_t length;

    if (address_text == NULL
        || !parse_integer(address_text, 16u, &address)
        || (length_text != NULL
            && !parse_integer(length_text,
                length_text[0] == '0' && (length_text[1] == 'x'
                    || length_text[1] == 'X') ? 16u : 10u,
                &parsed_length))
        || *arguments != '\0' || parsed_length == 0u || parsed_length > 256u) {
        console_write("Usage: MEMDUMP <hex_addr> [len (1-256, decimal)]\n");
        return;
    }
    length = (uint32_t)parsed_length;
    if (!memdump_range_mapped(address, length)) {
        console_write("MEMDUMP: range is unmapped or outside identity-mapped memory\n");
        return;
    }

    const volatile uint8_t *bytes = (const volatile uint8_t *)(uintptr_t)address;
    for (uint32_t offset = 0u; offset < length; offset += 16u) {
        uint32_t count = length - offset;
        if (count > 16u) {
            count = 16u;
        }
        print_hex32((uint32_t)(address + offset));
        console_write(": ");
        for (uint32_t i = 0u; i < 16u; ++i) {
            if (i < count) {
                print_hex8(bytes[offset + i]);
                console_putc(' ');
            } else {
                console_write("   ");
            }
        }
        console_write(" |");
        for (uint32_t i = 0u; i < count; ++i) {
            uint8_t byte = bytes[offset + i];
            console_putc(byte >= 32u && byte <= 126u ? (char)byte : '.');
        }
        console_write("|\n");
    }
}

void ioport_command(char *arguments)
{
    char *operation = next_command_token(&arguments);
    char *port_text = next_command_token(&arguments);
    char *option = next_command_token(&arguments);
    uint64_t port_value, data_value;

    if (operation == NULL || port_text == NULL
        || !parse_integer(port_text, 16u, &port_value)
        || port_value > 0xffffu) {
        console_write("Usage: IOPORT <in|out> <hex_port> [b|w|l for in; hex_val for out]\n");
        return;
    }
    if (ascii_equal(operation, "IN")) {
        char width = 'b';
        uint32_t value;
        if (*arguments != '\0') {
            console_write("Usage: IOPORT in <hex_port> [b|w|l]\n");
            return;
        }
        if (option != NULL) {
            if (option[1] != '\0'
                || (option[0] != 'b' && option[0] != 'B'
                    && option[0] != 'w' && option[0] != 'W'
                    && option[0] != 'l' && option[0] != 'L')) {
                console_write("IOPORT: read width must be b, w, or l\n");
                return;
            }
            width = option[0];
        }
        if (width == 'b' || width == 'B') {
            value = inb((uint16_t)port_value);
        } else if (width == 'w' || width == 'W') {
            value = inw((uint16_t)port_value);
        } else {
            value = inl((uint16_t)port_value);
        }
        console_write("IOPORT ");
        console_putc(width);
        console_write(" 0x");
        print_hex32((uint32_t)port_value);
        console_write(" = 0x");
        print_hex32(value);
        console_putc('\n');
    } else if (ascii_equal(operation, "OUT")) {
        if (option == NULL || *arguments != '\0'
            || !parse_integer(option, 16u, &data_value)
            || data_value > 0xffu) {
            console_write("Usage: IOPORT out <hex_port> <hex_val (00-FF)>\n");
            return;
        }
        outb((uint16_t)port_value, (uint8_t)data_value);
        console_write("IOPORT outb 0x");
        print_hex32((uint32_t)port_value);
        console_write(" <= 0x");
        print_hex8((uint8_t)data_value);
        console_write(" (write issued)\n");
    } else {
        console_write("IOPORT: operation must be in or out\n");
    }
}

bool same_pci_function(const struct book8_pcie_node *node,
    uint8_t bus, uint8_t slot, uint8_t function)
{
    return node->segment == 0u && node->bus == bus && node->slot == slot
        && node->function == function;
}

uint64_t pcie_dump_bar_size(const struct book8_pcie_node *node,
    uint8_t index, const char **cache)
{
    const struct book8_pcie_bar *resource = &node->bars[index];
    *cache = resource->io ? "I/O" : "not mapped";
    if (resource->cache_policy == BOOK8_PCIE_CACHE_UC) {
        *cache = "UC";
    } else if (resource->cache_policy == BOOK8_PCIE_CACHE_WC) {
        *cache = "WC";
    } else if (resource->cache_policy == BOOK8_PCIE_CACHE_WB) {
        *cache = "WB";
    }
    if (same_pci_function(node, rtl_nic.bus, rtl_nic.slot, rtl_nic.function)
        && rtl_nic.present && index < 6u) {
        if (rtl_nic.bars[index].mapped != NULL && !rtl_nic.bars[index].io) {
            *cache = "UC";
        }
        return rtl_nic.bars[index].size;
    }
    if (same_pci_function(node, hda_controller.bus, hda_controller.slot,
            hda_controller.function)
        && hda_controller.present && index == 0u
        && node->bars[index].base == hda_controller.mmio_base) {
        *cache = hda_controller.regs != NULL ? "UC" : "not mapped";
        return hda_controller.regs != NULL ? 0x4000u : 0u;
    }
    if (same_pci_function(node, nv_gpu.bus, nv_gpu.slot, nv_gpu.function)
        && nv_gpu.present) {
        if (index == 0u) {
            *cache = nv_gpu.bar0 != NULL ? "UC" : "not mapped";
            return nv_gpu.bar0_size;
        }
        if (index == 1u) {
            *cache = nv_gpu.bar1 != NULL ? "WB" : "not mapped";
            return nv_gpu.bar1_size;
        }
    }
    if (node->bus_device.driver != NULL && index == 0u
        && node->bus_device.driver->bar0_window_size != 0u) {
        *cache = "UC";
    }
    return resource->size;
}

void pcidump_command(void)
{
    console_write("PCI/PCIe devices: ");
    print_unsigned(pcie.node_count);
    console_write(" (bound drivers=");
    print_unsigned(pcie.driver_count);
    console_write(")\n");
    for (size_t i = 0u; i < pcie.node_count; ++i) {
        const struct book8_pcie_node *node = &pcie.nodes[i];
        uint16_t vendor = (uint16_t)node->id;
        uint16_t device = (uint16_t)(node->id >> 16u);
        uint32_t class_code = node->class_revision >> 8u;
        console_write(" ");
        if (node->segment != 0u) {
            print_hex32(node->segment);
            console_putc(':');
        }
        print_hex8(node->bus);
        console_putc(':');
        print_hex8(node->slot);
        console_putc('.');
        print_hex8(node->function);
        console_write(" ");
        print_hex8((uint8_t)(vendor >> 8u));
        print_hex8((uint8_t)vendor);
        console_putc(':');
        print_hex8((uint8_t)(device >> 8u));
        print_hex8((uint8_t)device);
        console_write(" class=");
        print_hex8((uint8_t)(class_code >> 16u));
        console_putc('/');
        print_hex8((uint8_t)(class_code >> 8u));
        console_write(" IRQ=");
        print_hex8(node->irq_line);
        console_write(" driver=");
        console_write(node->bus_device.driver != NULL
            ? node->bus_device.driver->name
            : node->driver != NULL ? node->driver->name : "none");
        console_putc('\n');
        for (uint8_t bar = 0u; bar < node->bar_count; ++bar) {
            const struct book8_pcie_bar *resource = &node->bars[bar];
            console_write("   BAR");
            print_unsigned(bar);
            if (!resource->valid) {
                console_write(" unassigned/upper-half\n");
                continue;
            }
            console_write(resource->io ? " I/O=" : " MMIO=");
            console_write("0x");
            print_hex64(resource->base);
            console_write(" size=");
            const char *cache;
            uint64_t size = pcie_dump_bar_size(node, bar, &cache);
            if (size != 0u) {
                console_write("0x");
                print_hex64(size);
            } else {
                console_write("unknown");
            }
            console_write(" cache=");
            console_write(cache);
            console_write(" prefetch=");
            console_write(resource->prefetchable ? "yes\n" : "no\n");
        }
    }
}

void pnp_scan_command(void)
{
    size_t old_pci_bound = pcie.driver_count;
    uint32_t old_usb_bound = book8_usb_bound_device_count();
    size_t pci_bound = book8_pcie_rescan(&pcie, bus_drivers,
        sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    book8_usb_rescan(&pcie);
    uint32_t usb_bound = book8_usb_bound_device_count();
    bool registry_sync_ok = book8_pcie_registry_sync(&pcie)
        && book8_usb_registry_sync();
    bool registry_ok = registry_sync_ok
        && (!registry_write_allowed || reg_save());

    console_write("PNPSCAN: PCI devices=");
    print_unsigned(pcie.node_count);
    console_write(" bound=");
    print_unsigned(pci_bound);
    console_write(" new=");
    print_unsigned(pci_bound > old_pci_bound
        ? pci_bound - old_pci_bound : 0u);
    console_write("; USB bound=");
    print_unsigned(usb_bound);
    console_write(" new=");
    print_unsigned(usb_bound > old_usb_bound
        ? usb_bound - old_usb_bound : 0u);
    if (!registry_sync_ok) {
        console_write(" (registry update failed)");
    } else if (!registry_write_allowed) {
        console_write(" (registry persistence deferred until installation)");
    } else if (!registry_ok) {
        console_write(" (registry update failed)");
    }
    console_putc('\n');
}

void registry_seed_program(const char *id, const char *name,
    const char *path, const char *app_class, uint32_t hidden,
    uint32_t system_component)
{
    char key[BOOK8_REGISTRY_KEY_MAX];
    char existing[BOOK8_REGISTRY_VALUE_MAX];
    int length = snprintf(key, sizeof(key),
        "HKLM\\SOFTWARE\\BOOK8\\Programs\\%s", id);

    if (length < 0 || (size_t)length >= sizeof(key)
        || reg_get_str(key, "Name", existing, sizeof(existing))) {
        return;
    }
    (void)reg_set_str(key, "Name", name);
    (void)reg_set_str(key, "Path", path);
    (void)reg_set_str(key, "AppClass", app_class);
    (void)reg_set_u32(key, "Hidden", hidden);
    (void)reg_set_u32(key, "SystemComponent", system_component);
}

void registry_seed_value(const char *path, const char *name,
    const char *value)
{
    char existing[BOOK8_REGISTRY_VALUE_MAX];
    if (!reg_get_str(path, name, existing, sizeof(existing))) {
        (void)reg_set_str(path, name, value);
    }
}

void registry_format_ipv4(uint32_t address, char *text, size_t capacity)
{
    (void)snprintf(text, capacity, "%u.%u.%u.%u",
        (unsigned)(address >> 24u), (unsigned)((address >> 16u) & 255u),
        (unsigned)((address >> 8u) & 255u), (unsigned)(address & 255u));
}

bool registry_parse_ipv4(const char *text, uint32_t *address)
{
    uint32_t result = 0u;
    for (uint32_t octet = 0u; octet < 4u; ++octet) {
        uint32_t value = 0u, digits = 0u;
        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (uint32_t)(*text++ - '0');
            if (value > 255u) { return false; }
            ++digits;
        }
        if (digits == 0u || (octet != 3u && *text++ != '.')
            || (octet == 3u && *text != '\0')) {
            return false;
        }
        result = (result << 8u) | value;
    }
    *address = result;
    return true;
}

void registry_migrate_legacy_settings(void)
{
    static const char *const settings_path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    char existing[BOOK8_REGISTRY_VALUE_MAX], value[BOOK8_REGISTRY_VALUE_MAX];
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    uint32_t size = 0u;
    struct book8_system_config config;
    if (!reg_get_str(settings_path, "KeyRate", existing, sizeof(existing))
        && fs_read_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes), &size)
        && book8_config_decode(&config, bytes, size)) {
        (void)reg_set_u32(settings_path, "KeyRate",
            config.values[BOOK8_CFG_REPEAT_RATE]);
        (void)reg_set_u32(settings_path, "KeyDelay",
            (config.values[BOOK8_CFG_KEY_DELAY] + 1u) * 250u);
        (void)reg_set_u32(settings_path, "MouseSpeed",
            config.values[BOOK8_CFG_POINTER_SPEED]);
        (void)reg_set_u32(settings_path, "DblClickMs",
            config.values[BOOK8_CFG_DOUBLE_CLICK]);
        (void)reg_set_str(settings_path, "Theme",
            config.values[BOOK8_CFG_DARK] != 0u ? "Dark" : "Light");
        (void)reg_set_u32(settings_path, "Accent",
            config.values[BOOK8_CFG_ACCENT]);
        (void)reg_set_u32(settings_path, "Volume",
            config.values[BOOK8_CFG_VOLUME]);
        (void)reg_set_u32(settings_path, "Tone",
            config.values[BOOK8_CFG_TONE]);
    }
    if (reg_get_str(settings_path, "NetMode", existing, sizeof(existing))) {
        return;
    }
    uint8_t network_bytes[BOOK8_NETWORK_SETTINGS_BYTES + 1u];
    struct book8_network_settings network;
    if (fs_read_file(BOOK8_NETWORK_SETTINGS_PATH, network_bytes,
            sizeof(network_bytes), &size)
        && book8_network_settings_decode(&network, network_bytes, size)) {
        (void)reg_set_str(settings_path, "NetMode",
            network.mode == BOOK8_NETWORK_DHCP ? "DHCP" : "Static");
        registry_format_ipv4(network.ip, value, sizeof(value));
        (void)reg_set_str(settings_path, "StaticIP", value);
        registry_format_ipv4(network.mask, value, sizeof(value));
        (void)reg_set_str(settings_path, "Netmask", value);
        registry_format_ipv4(network.gateway, value, sizeof(value));
        (void)reg_set_str(settings_path, "Gateway", value);
        registry_format_ipv4(network.dns, value, sizeof(value));
        (void)reg_set_str(settings_path, "DNS", value);
    }
}

void registry_seed_settings(void)
{
    static const char *const values[][2] = {
        {"Clock24H", "0"}, {"KeyDelay", "500"}, {"KeyRate", "25"},
        {"MouseSpeed", "100"}, {"DblClickMs", "400"},
        {"Theme", "Light"}, {"Accent", "0"}, {"OverscanX", "0"},
        {"OverscanY", "0"}, {"Volume", "75"}, {"UiBeep", "1"},
        {"Tone", "880"}, {"NetMode", "DHCP"},
        {"StaticIP", "192.168.50.208"}, {"Netmask", "255.255.255.0"},
        {"Gateway", "192.168.50.1"}, {"DNS", "0.0.0.0"}
    };
    const char *path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    for (uint32_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        registry_seed_value(path, values[i][0], values[i][1]);
    }
}

void registry_seed_file_associations(void)
{
    static const char *const values[][2] = {
        {".TXT", "editor"}, {".INI", "editor"}, {".CFG", "editor"},
        {".LOG", "editor"}, {".C", "codeedit"}, {".H", "codeedit"},
        {".ASM", "codeedit"}, {".PS1", "codeedit"}
    };
    const char *path = "HKLM\\SOFTWARE\\BOOK8\\FileAssoc";
    for (uint32_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        registry_seed_value(path, values[i][0], values[i][1]);
    }
}

void registry_seed_defaults(void)
{
    registry_migrate_legacy_settings();
    registry_seed_program("CommandShell", "Command Shell", "", "shell", 0u, 1u);
    registry_seed_program("FileExplorer", "File Explorer", "", "explorer", 0u, 1u);
    registry_seed_program("NoteEditor", "NoteEditor", "", "editor", 0u, 1u);
    registry_seed_program("TaskManager", "Task Manager", "", "taskmgr", 0u, 1u);
    registry_seed_program("SystemSettings", "System Settings", "", "settings", 0u, 1u);
    registry_seed_program("BOOK8Player", "BOOK8 Player", "", "player", 0u, 1u);
    registry_seed_program("CAT", "CAT", "A:\\CAT.ELF", "", 0u, 1u);
    for (uint32_t i = 0u; i < BOOK8_MANIFEST_APP_COUNT; ++i) {
        const struct book8_manifest_app *app = &book8_manifest_apps[i];
        char path[BOOK8_REGISTRY_VALUE_MAX];
        int length;
        if (book8_install_app_excluded[i] != 0u)
            continue;
        length = snprintf(path, sizeof(path), "A:\\BIN\\%s", app->target);
        if (length > 0 && (size_t)length < sizeof(path)) {
            registry_seed_program(app->key, app->name, path, app->app_class,
                app->hidden, app->system_component);
        }
    }
    registry_seed_settings();
    registry_seed_file_associations();
    if (!book8_session_seed_defaults()) {
        console_write("SESSION: unable to seed primary account in registry.\n");
    }
}

bool registry_print_value(const char *name, void *context)
{
    struct registry_query_context *query = context;
    char value[BOOK8_REGISTRY_VALUE_MAX];
    if (!reg_get_str(query->path, name, value, sizeof(value))) {
        return true;
    }
    console_write("  ");
    console_write(name);
    console_write(" = ");
    console_write(value);
    console_write("\n");
    return true;
}

bool registry_print_key(const char *name, void *context)
{
    struct registry_query_context *parent = context;
    struct registry_query_context child;
    char path[BOOK8_REGISTRY_KEY_MAX];
    int length = snprintf(path, sizeof(path), "%s\\%s", parent->path, name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return true;
    }
    console_write("  ");
    console_write(name);
    console_write("\\\n");
    child.path = path;
    (void)reg_enum_values(path, registry_print_value, &child);
    return true;
}

void registry_command(char *arguments)
{
    char *operation = skip_spaces(arguments);
    char *rest = operation;
    while (*rest != '\0' && *rest != ' ' && *rest != '\t') {
        ++rest;
    }
    if (*rest != '\0') {
        *rest++ = '\0';
        rest = skip_spaces(rest);
    }
    if (ascii_equal(operation, "SAVE") && *rest == '\0') {
        if (!registry_write_allowed) {
            console_write("REG: save deferred until INSTALL mounts the target root.\n");
            return;
        }
        console_write(reg_save() ? "REG: saved A:\\BOOK8REG.INI\n"
            : "REG: save failed.\n");
        return;
    }
    if (ascii_equal(operation, "QUERY") && *rest != '\0') {
        struct registry_query_context query = {.path = rest};
        console_write(rest);
        console_write("\n");
        size_t values = reg_enum_values(rest, registry_print_value, &query);
        size_t keys = reg_enum_keys(rest, registry_print_key, &query);
        if (values == 0u && keys == 0u) {
            console_write("  (no values or subkeys)\n");
        }
        return;
    }
    if (ascii_equal(operation, "ADD")) {
        char *path = rest;
        char *name = path;
        while (*name != '\0' && *name != ' ' && *name != '\t') {
            ++name;
        }
        if (*name == '\0') {
            goto invalid;
        }
        *name++ = '\0';
        name = skip_spaces(name);
        char *value = name;
        while (*value != '\0' && *value != ' ' && *value != '\t') {
            ++value;
        }
        if (*value == '\0') {
            goto invalid;
        }
        *value++ = '\0';
        value = skip_spaces(value);
        if (*path == '\0' || *name == '\0' || *value == '\0'
            || !reg_set_str(path, name, value)) {
            console_write("REG: invalid path, name or value.\n");
            return;
        }
        if (gui_active) {
            (void)book8gui_refresh_programs();
        }
        console_write("REG: value added in memory; use REG SAVE to persist.\n");
        return;
    }
invalid:
    console_write("Usage: REG QUERY <path> | REG ADD <path> <key> <value> | REG SAVE\n");
}

void print_date(void)
{
    struct clock_time clock = read_clock();
    print_unsigned(clock.year);
    console_putc('-');
    print_two_digits(clock.month);
    console_putc('-');
    print_two_digits(clock.day);
    console_putc('\n');
}

void print_time(void)
{
    struct clock_time clock = read_clock();
    print_two_digits(clock.hour);
    console_putc(':');
    print_two_digits(clock.minute);
    console_putc(':');
    print_two_digits(clock.second);
    console_putc('\n');
}

void run_read_command(char *arguments)
{
    char *device_text = skip_spaces(arguments);
    char *lba_text = device_text;
    while (*lba_text != '\0' && *lba_text != ' ' && *lba_text != '\t') {
        ++lba_text;
    }
    if (*lba_text != '\0') {
        *lba_text++ = '\0';
        lba_text = skip_spaces(lba_text);
    }
    char *extra = lba_text;
    while (*extra != '\0' && *extra != ' ' && *extra != '\t') {
        ++extra;
    }
    if (*extra != '\0') {
        *extra++ = '\0';
        extra = skip_spaces(extra);
    }
    if (*device_text == '\0' || *lba_text == '\0' || *extra != '\0') {
        console_write("Usage: READ <disk-number> <LBA>\n");
        return;
    }

    uint32_t device_index = 0u;
    uint64_t lba = 0u;
    for (const char *p = device_text; *p != '\0'; ++p) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (*p < '0' || *p > '9'
            || device_index > (UINT32_MAX - digit) / 10u) {
            console_write("Invalid disk number.\n");
            return;
        }
        device_index = device_index * 10u + digit;
    }
    for (const char *p = lba_text; *p != '\0'; ++p) {
        uint64_t digit = (uint64_t)(*p - '0');
        uint64_t maximum_lba = (1ull << 48u) - 1u;
        if (*p < '0' || *p > '9'
            || lba > (maximum_lba - digit) / 10u) {
            console_write("Invalid or unsupported LBA.\n");
            return;
        }
        lba = lba * 10u + digit;
    }
    if (device_index >= disk_count || lba >= disks[device_index].sectors) {
        console_write("Disk number or LBA is out of range.\n");
        return;
    }
    if (!disk_read_sector(&disks[device_index], lba, sector_buffer)) {
        console_write("Read failed; the disk was not modified.\n");
        return;
    }
    console_write("Sector ");
    print_unsigned(lba);
    console_write(" (first 32 bytes):");
    for (uint32_t i = 0u; i < 32u; ++i) {
        if ((i & 7u) == 0u) {
            console_putc('\n');
            console_write("  ");
        }
        print_hex8(sector_buffer[i]);
        console_putc(' ');
    }
    console_putc('\n');
}

void run_write_command(char *arguments)
{
    char *device_text = skip_spaces(arguments);
    char *cursor = device_text;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        console_write("Usage: WRITE <disk-number> <LBA> <text>\n");
        return;
    }
    *cursor++ = '\0';
    char *lba_text = skip_spaces(cursor);
    cursor = lba_text;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        console_write("Usage: WRITE <disk-number> <LBA> <text>\n");
        return;
    }
    *cursor++ = '\0';
    char *text = skip_spaces(cursor);
    if (*text == '\0') {
        console_write("Write data cannot be empty. Use 1-511 printable ASCII characters.\n");
        return;
    }

    uint32_t device_index = 0u;
    for (const char *p = device_text; *p != '\0'; ++p) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (*p < '0' || *p > '9'
            || device_index > (UINT32_MAX - digit) / 10u) {
            console_write("Invalid disk number.\n");
            return;
        }
        device_index = device_index * 10u + digit;
    }

    uint64_t lba = 0u;
    const uint64_t maximum_lba = (1ull << 48u) - 1u;
    for (const char *p = lba_text; *p != '\0'; ++p) {
        uint64_t digit = (uint64_t)(*p - '0');
        if (*p < '0' || *p > '9'
            || lba > (maximum_lba - digit) / 10u) {
            console_write("Invalid or unsupported LBA.\n");
            return;
        }
        lba = lba * 10u + digit;
    }
    if (device_index >= disk_count || lba >= disks[device_index].sectors) {
        console_write("Disk number or LBA is out of range.\n");
        return;
    }
    if (!disks[device_index].lba48
        && (disks[device_index].ahci || lba >= (1ull << 28u))) {
        console_write("This device does not support writes at the requested LBA.\n");
        return;
    }

    size_t text_length = 0u;
    while (text[text_length] != '\0') {
        if (text_length >= 511u || (uint8_t)text[text_length] < 0x20u
            || (uint8_t)text[text_length] > 0x7eu) {
            console_write("Write data must be 1-511 printable ASCII characters.\n");
            return;
        }
        ++text_length;
    }

    console_write("Disk ");
    print_unsigned(device_index);
    console_write(" (");
    console_write(disks[device_index].name);
    console_write("), LBA ");
    print_unsigned(lba);
    console_write(": text will be followed by zero bytes to fill one sector.\n");
    if (!confirm_text("WARNING: this overwrites one disk sector. Type YES to continue: ")) {
        console_write("Write cancelled. Disk was not modified.\n");
        return;
    }

    zero_memory(sector_buffer, sizeof(sector_buffer));
    for (size_t i = 0u; i < text_length; ++i) {
        sector_buffer[i] = (uint8_t)text[i];
    }
    if (!disk_write_sector(&disks[device_index], lba, sector_buffer)) {
        console_write("Write or cache flush failed; the disk may have accepted the sector. Verify with READ.\n");
        return;
    }
    console_write("One sector written and cache flush completed. Use READ to verify.\n");
}

void set_environment(char *argument)
{
    char *equals = argument;
    while (*equals != '\0' && *equals != '=') {
        ++equals;
    }
    if (*equals == '\0') {
        if (*argument == '\0') {
            for (uint32_t i = 0u; i < 16u; ++i) {
                if (environment[i].name[0] != '\0') {
                    console_write(environment[i].name);
                    console_putc('=');
                    console_write(environment[i].value);
                    console_putc('\n');
                }
            }
        } else {
            console_write("Usage: SET [NAME=VALUE]\n");
        }
        return;
    }

    *equals = '\0';
    if (*argument == '\0') {
        console_write("Environment variable name cannot be empty.\n");
        return;
    }
    if (ascii_equal(argument, "PATH")) {
        copy_string(search_path, sizeof(search_path), equals + 1);
        console_write("PATH=");
        console_write(search_path);
        console_putc('\n');
        return;
    }
    for (uint32_t i = 0u; i < 16u; ++i) {
        if (environment[i].name[0] == '\0'
            || ascii_equal(environment[i].name, argument)) {
            copy_string(environment[i].name, sizeof(environment[i].name), argument);
            copy_string(environment[i].value, sizeof(environment[i].value), equals + 1);
            return;
        }
    }
    console_write("Environment table is full.\n");
}

void run_filesystem_command(const char *command)
{
    console_write(command);
    console_write(": no filesystem is mounted. FAT/VFAT/LFN and NTFS support is not implemented.\n");
}

void run_external_command(const char *command)
{
    console_write(command);
    console_write(": utility is not implemented. No disk operation was performed.\n");
}

void run_exec(char *line)
{
    char *argv[EXEC_MAX_ARGS];
    int argc = 0;
    long status;

    while (*line != '\0' && argc < (int)EXEC_MAX_ARGS) {
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ') {
            ++line;
        }
        if (*line != '\0') {
            *line++ = '\0';
            line = skip_spaces(line);
        }
    }
    if (*line != '\0') {
        console_write("EXEC: too many arguments (maximum 16)\n");
        return;
    }
    if (argc == 0) {
        console_write("usage: EXEC <path> [args]\n");
        return;
    }
    status = exec_run(argv[0], argc, argv);
    stdio_set_output(console_putc);
    if (status == -2) {
        printf("EXEC: %s: file not found\n", argv[0]);
    } else if (status < 0 && status > -4096) {
        printf("EXEC: cannot execute %s (error %d)\n", argv[0], (int)-status);
    } else {
        printf("EXEC: %s exited with status %d\n", argv[0], (int)status);
    }
    if (book8gui_utility_pending()) {
        if (status != 0) { book8gui_cancel_pending_utility(); }
        else if (gui_active) { (void)book8gui_open_pending_utility(); }
        else { gui_launch(false); }
    }
}

void print_threads(void)
{
    struct sched_thread_info threads[SCHED_MAX_THREADS];
    uint32_t count = sched_snapshot(threads, SCHED_MAX_THREADS);

    console_write("TID  NAME                             STATE    CPU TICKS\n");
    for (uint32_t i = 0u; i < count && i < SCHED_MAX_THREADS; ++i) {
        print_unsigned(threads[i].tid);
        console_write("  ");
        console_write(threads[i].name);
        console_write("  ");
        console_write(sched_state_name(threads[i].state));
        console_write("  ");
        print_unsigned(threads[i].cpu_ticks);
        console_putc('\n');
    }
}

void run_command(char *line)
{
    char *command = skip_spaces(line);
    char *argument = command;
    while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
        ++argument;
    }
    if (*argument != '\0') {
        *argument++ = '\0';
        argument = skip_spaces(argument);
    }

    if (*command == '\0' || ascii_equal(command, "REM")) {
        return;
    }
    if (ascii_equal(command, "REG")) {
        registry_command(argument);
        return;
    }
    if (fs_handle_command(command, argument)) {
        return;
    }
    if (ascii_equal(command, "HELP") || ascii_equal(command, "?")) {
        print_help();
    } else if (ascii_equal(command, "PCIDUMP") || ascii_equal(command, "LSPCI")) {
        pcidump_command();
    } else if (ascii_equal(command, "HWDIAG")) {
        hwdiag_command();
    } else if (ascii_equal(command, "NVINFO")) {
        nvinfo_command();
    } else if (ascii_equal(command, "PNPSCAN") || ascii_equal(command, "RESCAN")) {
        pnp_scan_command();
    } else if (ascii_equal(command, "MEMDUMP")) {
        memdump_command(argument);
    } else if (ascii_equal(command, "IOPORT")) {
        ioport_command(argument);
    } else if (ascii_equal(command, "MSRGUARDTEST")) {
        msr_guard_test();
    } else if (ascii_equal(command, "PCIEPROBE") || ascii_equal(command, "ACK")) {
        probe_command(argument, ascii_equal(command, "ACK"));
    } else if (ascii_equal(command, "VER")) {
        console_write("BOOK8 x86-64 experimental kernel 0.2\n");
    } else if (ascii_equal(command, "CLS")) {
        console_clear();
    } else if (ascii_equal(command, "ECHO")) {
        console_write(argument);
        console_putc('\n');
    } else if (ascii_equal(command, "DATE")) {
        print_date();
    } else if (ascii_equal(command, "TIME")) {
        print_time();
    } else if (ascii_equal(command, "USBSTAT")) {
        print_usb_poll_status();
    } else if (ascii_equal(command, "INPUTSTAT")) {
        print_input_status();
    } else if (ascii_equal(command, "THREADS")) {
        print_threads();
    } else if (ascii_equal(command, "NETSTAT")) {
        network_status();
    } else if (ascii_equal(command, "NETCONFIG")) {
        network_configure_command(argument);
    } else if (ascii_equal(command, "TELEMETRY")) {
        telemetry_command(argument);
    } else if (ascii_equal(command, "NETBOOT")) {
        network_boot_command(argument);
    } else if (ascii_equal(command, "NETSAVE")) {
        network_save_command();
    } else if (ascii_equal(command, "RELAUNCH")) {
        relaunch_command(argument);
    } else if (ascii_equal(command, "USBREG")) {
        console_write(book8_usb_diag_log());
    } else if (ascii_equal(command, "LIBTEST")) {
        int failed;
        stdio_set_output(console_putc);
        failed = libc_selftest(console_write);
        printf("libc self-test: %d checks, %d failed\n", libc_selftest_checks(), failed);
    } else if (ascii_equal(command, "RING3TEST")) {
        long status = syscall_ring3_smoke();
        if (status == 0) {
            console_write("RING3TEST: PASS\n");
        } else {
            stdio_set_output(console_putc);
            printf("RING3TEST: FAIL status=%d\n", (int)status);
        }
    } else if (ascii_equal(command, "SCHEDTEST")) {
        scheduler_test(false);
    } else if (ascii_equal(command, "SCHEDUSER")) {
        scheduler_test(true);
    } else if (ascii_equal(command, "SMPTEST")) {
        smp_scheduler_test();
    } else if (ascii_equal(command, "VMTEST")) {
        uint64_t flags = irq_save();
        bool passed = exec_address_space_test();
        irq_restore(flags);
        console_write(passed ? "VMTEST: PASS\n" : "VMTEST: FAIL\n");
    } else if (ascii_equal(command, "SWAPTEST")) {
        bool passed = vmm_swap_selftest();
        console_write(passed ? "SWAPTEST: PASS\n" : "SWAPTEST: FAIL\n");
    } else if (ascii_equal(command, "EXEC")) {
        run_exec(argument);
    } else if (ascii_equal(command, "SET")) {
        set_environment(argument);
    } else if (ascii_equal(command, "PATH")) {
        if (*argument != '\0') {
            copy_string(search_path, sizeof(search_path), argument);
        }
        console_write("PATH=");
        console_write(search_path);
        console_putc('\n');
    } else if (ascii_equal(command, "PROMPT")) {
        if (*argument != '\0') {
            copy_string(prompt_format, sizeof(prompt_format), argument);
        }
    } else if (ascii_equal(command, "CD") || ascii_equal(command, "CHDIR")) {
        if (*argument == '\0') {
            console_write(fs_current_path());
            console_putc('\n');
        } else {
            run_filesystem_command(command);
        }
    } else if (ascii_equal(command, "DISK")) {
        print_disk_inventory();
    } else if (ascii_equal(command, "INSTALL")) {
        install_command(argument);
    } else if (ascii_equal(command, "BOOK8GUI")) {
        gui_launch(false);
    } else if (ascii_equal(command, "VOL")) {
        console_write("No filesystem volume is mounted. Run DISK to inspect ATA devices.\n");
    } else if (ascii_equal(command, "READ")) {
        run_read_command(argument);
    } else if (ascii_equal(command, "WRITE")) {
        run_write_command(argument);
    } else if (ascii_equal(command, "DIR") || ascii_equal(command, "TYPE")
        || ascii_equal(command, "COPY") || ascii_equal(command, "DEL")
        || ascii_equal(command, "ERASE") || ascii_equal(command, "MD")
        || ascii_equal(command, "MKDIR") || ascii_equal(command, "RD")
        || ascii_equal(command, "RMDIR")) {
        run_filesystem_command(command);
    } else if (ascii_equal(command, "ATTRIB") || ascii_equal(command, "CHKDSK")
        || ascii_equal(command, "DEFRAG") || ascii_equal(command, "EDIT")
        || ascii_equal(command, "FDISK") || ascii_equal(command, "FORMAT")
        || ascii_equal(command, "MOVE") || ascii_equal(command, "SCANDISK")
        || ascii_equal(command, "SYS") || ascii_equal(command, "XCOPY")) {
        run_external_command(command);
    } else if (ascii_equal(command, "EXIT") || ascii_equal(command, "QUIT")
        || ascii_equal(command, "BYE")) {
        console_write("BOOK8 shutdown requested.\n");
        acpi_shutdown();
    } else if (ascii_equal(command, "HALT")) {
        console_write("BOOK8 halted.\n");
        acpi_shutdown();
    } else if (ascii_equal(command, "REBOOT") || ascii_equal(command, "RESET")) {
        console_write("BOOK8 reboot requested.\n");
        reboot_system();
    } else {
        console_write("Bad command or file name.\n");
    }
}

void shell(void)
{
    char line[MAX_LINE];
    pit_clock_start();
    (void)pit_clock_ticks();
    if (tsc_hz == 0u)
        tsc_calibrate();
    interrupt_table_initialize();
    smp_runtime_initialize();
    if (autostart_desktop && installer_image == NULL
        && screen.pixels != NULL) {
        serial_write("BOOK8-BOOT: automatic desktop handoff\r\n");
        startup_quiet = true;
        console_clear();
        play_boot_splash();
        gui_launch(true);
        startup_quiet = false;
    } else {
        play_boot_splash();
    }

    console_write("BOOK8 x86-64 command interpreter 0.2\n");
    console_write("IDE PIO + AHCI SATA. Type HELP for commands.\n");
    console_write(ps2_keyboard_controller_ready
        ? "PS/2 keyboard controller configured for polling.\n"
        : "PS/2 keyboard controller unavailable; serial input remains available.\n");
    console_write(ps2_mouse_available
        ? "PS/2 mouse ready.\n"
        : "PS/2 mouse unavailable.\n");
    if (book8_usb_keyboard_active()) {
        console_write("USB HID keyboard active.\n");
    } else {
        console_write(ps2_keyboard_controller_ready
            ? "USB HID keyboard unavailable; PS/2/serial fallback enabled.\n"
            : "USB HID keyboard unavailable; serial fallback enabled.\n");
    }
    console_write(installer_image != NULL
        ? "Installer payload loaded; use INSTALL to choose a target disk.\n"
        : "To install BOOK8, boot the BOOK8 Installer entry from the install ISO.\n");
    console_write("USB host controllers (total/UHCI/OHCI/EHCI/xHCI): ");
    print_unsigned(book8_usb_detected_controller_count());
    console_write("/");
    print_unsigned(book8_usb_uhci_controller_count());
    console_write("/");
    print_unsigned(book8_usb_ohci_controller_count());
    console_write("/");
    print_unsigned(book8_usb_ehci_controller_count());
    console_write("/");
    print_unsigned(book8_usb_xhci_controller_count());
    console_write(book8_usb_mouse_active()
        ? "; USB HID mouse active (UHCI/OHCI).\n"
        : "; USB HID mouse unavailable (no UHCI/OHCI boot mouse found; xHCI unsupported).\n");
    struct book8_usb_diagnostics usb_diagnostics;
    book8_usb_get_diagnostics(&usb_diagnostics);
    console_write("OHCI started/ports/connected/HID/hubs/reports/errors: ");
    print_unsigned(usb_diagnostics.ohci_started);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_connected_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_hid_devices);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_hub_devices);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_reports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_errors);
    console_write("; reset/control/ownership failures: ");
    print_unsigned(usb_diagnostics.ohci_reset_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_control_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_ownership_failures);
    console_write("; last CC/stage/retries: ");
    print_unsigned(usb_diagnostics.ohci_last_cc);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_last_stage);
    console_putc('/');
    print_unsigned(usb_diagnostics.ohci_control_retries);
    console_write("; EHCI released to OHCI: ");
    print_unsigned(usb_diagnostics.ehci_released_to_ohci);
    console_putc('\n');
    console_write("USB enumeration (connected/reset/addressed/boot-HID/active): ");
    print_unsigned(usb_diagnostics.connected_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.reset_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.addressed_devices);
    console_putc('/');
    print_unsigned(usb_diagnostics.boot_hid_interfaces);
    console_putc('/');
    print_unsigned(usb_diagnostics.active_hid_devices);
    console_write("; UHCI running/schedule failures: ");
    print_unsigned(usb_diagnostics.uhci_running_controllers);
    console_putc('/');
    print_unsigned(usb_diagnostics.uhci_schedule_failures);
    console_write("; HID interrupt completions: ");
    print_unsigned(usb_diagnostics.uhci_interrupt_completions);
    console_write("; UHCI stalled HID schedules: ");
    print_unsigned(usb_diagnostics.uhci_interrupt_stalls);
    console_write("; EHCI legacy handoffs: ");
    print_unsigned(usb_diagnostics.ehci_legacy_handoffs);
    console_write("; handoff failures: ");
    print_unsigned(usb_diagnostics.ehci_legacy_handoff_failures);
    console_write("; handoffs skipped without UHCI: ");
    print_unsigned(usb_diagnostics.ehci_legacy_handoff_skipped);
    console_write("; EHCI powered/companion ports: ");
    print_unsigned(usb_diagnostics.ehci_powered_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ehci_companion_ports);
    console_write("; companion UHCI mapped/unmapped: ");
    print_unsigned(usb_diagnostics.ehci_uhci_companion_ports);
    console_putc('/');
    print_unsigned(usb_diagnostics.ehci_unmapped_companion_ports);
    console_write("; companion mappings/static fallbacks: ");
    print_unsigned(usb_diagnostics.ehci_companion_mappings);
    console_putc('/');
    print_unsigned(usb_diagnostics.ehci_route_fallbacks);
    console_putc('\n');
    console_write("USB PCI command enable failures: ");
    print_unsigned(usb_diagnostics.pci_enable_failures);
    console_putc('\n');
    console_write("USB descriptor failures (device/config/HID-request/non-boot): ");
    print_unsigned(usb_diagnostics.device_descriptor_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.configuration_descriptor_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.hid_control_failures);
    console_putc('/');
    print_unsigned(usb_diagnostics.non_boot_hid_devices);
    console_putc('\n');

    bool diag_first_prompt = true;
    bool diag_first_key = true;
    for (;;) {
        __asm__ volatile ("sti" : : : "memory");
        uint32_t length = 0u;
        if (diag_first_prompt) {
            diag_marker("M1 shell loop top");
        }
        print_prompt();
        if (diag_first_prompt) {
            diag_marker("M2 prompt printed");
            diag_first_prompt = false;
        }
        for (;;) {
            char value = read_char();
            if (diag_first_key) {
                diag_marker("K first key");
                diag_first_key = false;
            }
            if (value == '\r' || value == '\n') {
                console_putc('\n');
                line[length] = '\0';
                break;
            }
            if ((value == '\b' || value == 0x7f) && length > 0u) {
                --length;
                console_putc('\b');
            } else if (value >= 0x20 && value <= 0x7e
                && length + 1u < sizeof(line)) {
                line[length++] = value;
                console_putc(value);
            }
        }
        run_command(line);
    }
}
