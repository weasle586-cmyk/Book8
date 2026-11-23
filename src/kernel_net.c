#include "kernel_internal.h"

/* Kernel network stack glue, DHCP, netboot and settings API. */

struct book8_net *book8_kernel_network(void)
{
    return network_ready ? &kernel_net : NULL;
}

enum book8_net_result network_send(void *ctx, const void *frame, size_t size)
{
    (void)ctx;
    if (rtl8168_driver.descriptor == NULL
        || rtl8168_driver.descriptor->write == NULL)
        return BOOK8_NET_IO_ERROR;
    int32_t status = rtl8168_driver.descriptor->write(0u, frame, size);
    if (status == 0) {
        return BOOK8_NET_OK;
    }
    console_write("BOOK8-NET: TX ");
    struct b8_rtl8168_status_text text = {
        .status = (uint32_t)(status < 0 ? -status : status)
    };
    if (rtl8168_driver.descriptor->ioctl != NULL
        && rtl8168_driver.descriptor->ioctl(B8_RTL8168_IOCTL_STATUS_TEXT,
            &text) == 0 && text.text != NULL)
        console_write(text.text);
    else
        console_write("driver error");
    console_write("\n");
    return text.status == RTL8168_TX_BUSY
        ? BOOK8_NET_BUSY : BOOK8_NET_IO_ERROR;
}

enum book8_net_result network_receive(void *ctx, void *frame,
    size_t capacity, size_t *size)
{
    (void)ctx;
    if (rtl8168_driver.descriptor == NULL
        || rtl8168_driver.descriptor->read == NULL)
        return BOOK8_NET_IO_ERROR;
    int32_t result = rtl8168_driver.descriptor->read(0u, frame, capacity);
    if (result > 0) {
        *size = (size_t)result;
        return BOOK8_NET_OK;
    }
    if (result == 0) {
        return BOOK8_NET_EMPTY;
    }
    enum rtl8168_status status = (enum rtl8168_status)-result;
    if (status == RTL8168_RX_DROPPED || status == RTL8168_BUFFER_SMALL) {
        return BOOK8_NET_RX_DROPPED;
    }
    console_write("BOOK8-NET: RX ");
    struct b8_rtl8168_status_text text = {.status = (uint32_t)status};
    if (rtl8168_driver.descriptor->ioctl != NULL
        && rtl8168_driver.descriptor->ioctl(B8_RTL8168_IOCTL_STATUS_TEXT,
            &text) == 0 && text.text != NULL)
        console_write(text.text);
    else
        console_write("driver error");
    console_write("\n");
    return BOOK8_NET_IO_ERROR;
}

void network_rx_poll(void)
{
    if (network_ready) {
        if (!network_enabled_service())
            return;
        uint64_t started = read_tsc();
        enum book8_net_result result = book8_net_poll(&kernel_net);
        uint64_t spent = read_tsc() - started;
        ++poll_iterations;
        poll_tsc_total += spent;
        if (spent > poll_tsc_max) {
            poll_tsc_max = spent;
        }
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-NET: poll ");
            console_write(book8_net_result_string(result));
            console_write("\n");
        }
        network_dhcp_service();
        telemetry_service();
        push_agent_service();
    }
}

void network_print_ip(uint32_t ip)
{
    for (unsigned i = 0u; i < 4u; ++i) {
        if (i != 0u) {
            console_putc('.');
        }
        print_unsigned((ip >> (24u - i * 8u)) & 255u);
    }
}

void network_status(void)
{
    console_write("MODE: ");
    console_write(network_settings.mode == BOOK8_NETWORK_DHCP
        ? book8_dhcp_state_string(dhcp_client.state) : "forced static");
    console_write(" DHCP rejected=");
    print_unsigned(dhcp_client.rejected);
    console_write(" lease-seconds=");
    print_unsigned(dhcp_client.lease_seconds);
    console_write("\n");
    console_write("BOOK8-NET: ");
    console_write(network_ready ? "polling stack ready\n" : "no supported NIC ready\n");
    console_write("IP=");
    network_print_ip(kernel_net.ip);
    console_write(" MASK=");
    network_print_ip(kernel_net.mask);
    console_write(" GATEWAY=");
    network_print_ip(kernel_net.gateway);
    console_write("\nRX/TX/invalid/ignored/RX-errors/TX-errors/ARP/echo/UDP=");
    const uint32_t values[] = {kernel_net.rx_frames, kernel_net.tx_frames,
        kernel_net.invalid_frames, kernel_net.ignored_frames, kernel_net.rx_errors,
        kernel_net.tx_errors, kernel_net.arp_replies, kernel_net.echo_replies,
        kernel_net.udp_received};
    for (unsigned i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (i != 0u) {
            console_putc('/');
        }
        print_unsigned(values[i]);
    }
    console_write("\n");
    console_write("TELEMETRY target=");
    network_print_ip(telemetry_target);
    console_write(" UDP=8080 interval=1000ms format=v2 (292 bytes) failures=");
    print_unsigned(telemetry_failures);
    console_write("\nCONFIG source=");
    console_write(network_config_source);
    console_write(" baked=" BOOK8_NET_DEFAULT_IP "/" BOOK8_NET_DEFAULT_MASK
        " gw=" BOOK8_NET_DEFAULT_GATEWAY " telemetry=" BOOK8_NET_DEFAULT_TELEMETRY);
    console_write("\nBUILD id=");
    print_unsigned(BOOK8_BUILD_ID);
    console_write(" relaunch=");
    print_unsigned(boot_relaunch_count);
    struct book8_push_stats push;
    book8_push_get_stats(&push);
    console_write(" PUSH ");
    console_write(push.listening ? "listening UDP=8081" : (BOOK8_PUSH ? "stopped" : "not built"));
    console_write(" accepted/rejected/challenges/busy=");
    print_unsigned(push.accepted);
    console_putc('/');
    print_unsigned(push.rejected);
    console_putc('/');
    print_unsigned(push.challenges);
    console_putc('/');
    print_unsigned(push.busy_replies);
    console_write("\n");
}

void network_configure_command(char *argument)
{
    if (ascii_equal(argument, "DHCP")) {
        struct book8_network_settings settings = network_settings;
        settings.mode = BOOK8_NETWORK_DHCP;
        if (book8_network_settings_apply(&settings, false))
            console_write("NETCONFIG: DHCP discovery started\n");
        return;
    }
    if (ascii_equal(argument, "RENEW")) {
        (void)book8_network_settings_renew();
        return;
    }
    uint32_t values[3] = {0};
    if (!ascii_equal(argument, "OFF")) {
        for (unsigned i = 0u; i < 3u; ++i) {
            char *token = skip_spaces(argument);
            argument = token;
            while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
                ++argument;
            }
            if (*argument != '\0') {
                *argument++ = '\0';
            }
            if (!book8_net_parse_ip(token, &values[i])) {
                console_write("NETCONFIG: expected <ip> <mask> <gateway> or OFF.\n");
                return;
            }
        }
        if (*skip_spaces(argument) != '\0') {
            console_write("NETCONFIG: unexpected trailing argument.\n");
            return;
        }
    }
    if (!network_ready) {
        console_write("NETCONFIG: no supported NIC ready.\n");
        return;
    }
    struct book8_net validation = {0};
    enum book8_net_result result = book8_net_configure(&validation,
        values[0], values[1], values[2]);
    if (result == BOOK8_NET_OK)
        result = book8_dhcp_stop(&dhcp_client);
    if (result == BOOK8_NET_OK)
        result = book8_net_configure(&kernel_net,
        values[0], values[1], values[2]);
    console_write("NETCONFIG: ");
    console_write(book8_net_result_string(result));
    console_write("\n");
    if (result == BOOK8_NET_OK) {
        network_settings.mode = BOOK8_NETWORK_STATIC;
        network_settings.ip = values[0];
        network_settings.mask = values[1];
        network_settings.gateway = values[2];
        network_config_source = values[0] == 0u ? "none" : "NETCONFIG";
        if (values[0] == 0u && telemetry_target != 0u) {
            enum book8_net_result stopped = net_telemetry_init(&kernel_net, 0u);
            console_write("TELEMETRY: ");
            console_write(book8_net_result_string(stopped));
            console_write("\n");
            if (stopped == BOOK8_NET_OK) {
                telemetry_target = 0u;
            }
        }
        network_status();
    }
}

bool netboot_overlap(uint64_t start, uint64_t length)
{
    uint64_t base = BOOK8_NETBOOT_ADDRESS;
    uint64_t end = base + BOOK8_TFTP_CAPACITY;
    return length != 0u && start < end
        && (length > UINT64_MAX - start || start + length > base);
}

bool netboot_memory_available(void)
{
    if (netboot_info_address == 0u
        || (uint64_t)(uintptr_t)__kernel_end > BOOK8_NETBOOT_ADDRESS) {
        return false;
    }
    const uint32_t *info = (const uint32_t *)(uintptr_t)netboot_info_address;
    uint32_t size = info[0];
    if (size < 16u || size > 0x100000000ull - netboot_info_address
        || netboot_overlap(netboot_info_address, size)) {
        return false;
    }
    bool usable = false;
    for (uint32_t offset = 8u; offset <= size - 8u;) {
        const uint8_t *bytes = (const uint8_t *)info + offset;
        const struct multiboot_tag *tag = (const struct multiboot_tag *)bytes;
        if (tag->size < 8u || tag->size > size - offset) {
            return false;
        }
        if (tag->type == MULTIBOOT_TAG_END) {
            return usable;
        }
        if (tag->type == MULTIBOOT_TAG_MMAP && tag->size >= 16u) {
            uint32_t entry_size = *(const uint32_t *)(bytes + 8u);
            if (entry_size < 24u || (tag->size - 16u) % entry_size != 0u) {
                return false;
            }
            for (uint32_t pos = 16u; pos < tag->size; pos += entry_size) {
                uint64_t start = *(const uint64_t *)(bytes + pos);
                uint64_t length = *(const uint64_t *)(bytes + pos + 8u);
                uint32_t type = *(const uint32_t *)(bytes + pos + 16u);
                if (type != 1u && netboot_overlap(start, length)) {
                    return false;
                }
                if (type == 1u && start <= BOOK8_NETBOOT_ADDRESS
                    && length <= UINT64_MAX - start
                    && start + length >= BOOK8_NETBOOT_ADDRESS + BOOK8_TFTP_CAPACITY) {
                    usable = true;
                }
            }
        } else if (tag->type == MULTIBOOT_TAG_MODULE
            && tag->size >= sizeof(struct multiboot_tag_module)) {
            const struct multiboot_tag_module *module = (const void *)bytes;
            if (module->end < module->start
                || netboot_overlap(module->start, module->end - module->start)) {
                return false;
            }
        } else if (tag->type == MULTIBOOT_TAG_FRAMEBUFFER && tag->size >= 32u) {
            const struct multiboot_tag_framebuffer *fb = (const void *)bytes;
            if (netboot_overlap(fb->address, (uint64_t)fb->pitch * fb->height)) {
                return false;
            }
        }
        uint32_t step = (tag->size + 7u) & ~7u;
        if (step < tag->size || step > size - offset) {
            return false;
        }
        offset += step;
    }
    return false;
}

uint64_t netboot_milliseconds(void *ctx)
{
    (void)ctx;
    uint64_t elapsed = read_tsc() - boot_tsc;
    return elapsed / tsc_hz * 1000u + elapsed % tsc_hz * 1000u / tsc_hz;
}

void netboot_idle(void *ctx)
{
    (void)ctx;
    __asm__ volatile ("pause" ::: "memory");
}

bool netboot_disable_dma(void)
{
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint8_t slot = 0u; slot < 32u; ++slot) {
            uint8_t count = 1u;
            for (uint8_t fn = 0u; fn < count; ++fn) {
                uint16_t vendor = (uint16_t)pci_read32((uint8_t)bus, slot, fn, 0u);
                if (vendor == 0xffffu || vendor == 0u) {
                    continue;
                }
                if (fn == 0u && (pci_read32((uint8_t)bus, slot, fn, 0x0cu)
                    & 0x800000u) != 0u) {
                    count = 8u;
                }
                uint32_t command = pci_read32((uint8_t)bus, slot, fn, 4u) & 0xffffu;
                driver_pci_write32(NULL, (uint8_t)bus, slot, fn, 4u,
                    (command & ~4u) | 0x400u);
                if ((pci_read32((uint8_t)bus, slot, fn, 4u) & 4u) != 0u) {
                    return false;
                }
            }
        }
    }
    return true;
}

void network_boot_command(char *argument)
{
    char *server_text = skip_spaces(argument);
    char *filename = server_text;
    while (*filename != '\0' && *filename != ' ' && *filename != '\t') {
        ++filename;
    }
    if (*filename != '\0') {
        *filename++ = '\0';
    }
    filename = skip_spaces(filename);
    uint32_t server;
    if (!book8_net_parse_ip(server_text, &server) || *filename == '\0') {
        console_write("NETBOOT: expected <tftp-ip> <warm-kernel-file>.\n");
        return;
    }
    if (gui_active) {
        console_write("NETBOOT: close the desktop and use the CLI for handoff.\n");
        return;
    }
    if (!network_ready) {
        console_write("NETBOOT: no supported NIC ready.\n");
        return;
    }
    if (kernel_net.ip == 0u) {
        console_write("NETBOOT: configure IPv4 with NETCONFIG first.\n");
        return;
    }
    if (!netboot_memory_available()) {
        console_write("NETBOOT: 68..76 MiB is not verified free usable RAM.\n");
        return;
    }
    if (tsc_hz == 0u) {
        pit_clock_start();
        (void)pit_clock_ticks();
        tsc_calibrate();
    }
    if (tsc_hz == 0u) {
        console_write("NETBOOT: clock calibration failed.\n");
        return;
    }
    const struct book8_tftp_clock clock = {
        .milliseconds = netboot_milliseconds, .idle = netboot_idle
    };
    enum book8_net_result result = tftp_client_init(&kernel_net, server,
        BOOK8_TFTP_CAPACITY, &clock);
    uint8_t *load = (uint8_t *)(uintptr_t)BOOK8_NETBOOT_ADDRESS;
    if (result == BOOK8_NET_OK) {
        console_write("NETBOOT: fetching trusted warm image via TFTP...\n");
        result = tftp_fetch_kernel(filename, load);
    }
    struct book8_warm_header header;
    if (result == BOOK8_NET_OK) {
        result = book8_netboot_validate(load, tftp_fetched_size(),
            BOOK8_TFTP_CAPACITY, &header);
    }
    if (result != BOOK8_NET_OK) {
        console_write("NETBOOT: ");
        console_write(book8_net_result_string(result));
        if (result == BOOK8_NET_REMOTE_ERROR) {
            console_write(" code=");
            print_unsigned(tftp_remote_error());
        }
        console_write("\n");
        return;
    }
    for (size_t i = header.image_bytes; i < header.memory_bytes; ++i) {
        load[i] = 0u;
    }
    warm_info = (struct book8_warm_info){
        .magic = BOOK8_WARM_MAGIC, .version = 1u,
        .load_address = BOOK8_NETBOOT_ADDRESS, .image_bytes = header.image_bytes,
        .memory_bytes = header.memory_bytes
    };
    __asm__ volatile ("mov %%cr3, %0" : "=r"(warm_info.cr3));
    console_write("NETBOOT: validated; disabling DMA/interrupts for warm handoff.\n");
    __asm__ volatile ("cli" ::: "memory");
    outb(0x21u, 0xffu);
    outb(0xa1u, 0xffu);
    rtl_nic.regs[RTL8168_CR] = 0u;
    (void)rtl_nic.regs[RTL8168_CR];
    if (!netboot_disable_dma()) {
        console_write("NETBOOT: failed to disable PCI DMA; halting without handoff.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    network_ready = false;
    __asm__ volatile ("mfence" ::: "memory");
    book8_netboot_jump(BOOK8_NETBOOT_ADDRESS + header.entry_offset,
        (uint64_t)(uintptr_t)(netboot_stack + sizeof(netboot_stack)), &warm_info);
}

bool network_calibrate_clock(void)
{
    if (tsc_hz == 0u) {
        pit_clock_start();
        (void)pit_clock_ticks();
        tsc_calibrate();
    }
    return tsc_hz != 0u;
}

bool netcfg_valid(const struct book8_netcfg *config)
{
    char text[BOOK8_NETCFG_TEXT_MAX];
    struct book8_netcfg parsed;
    size_t length = book8_netcfg_format(config, text, sizeof(text));
    return length != 0u && book8_netcfg_parse(text, length, &parsed);
}

bool network_baked_config(struct book8_netcfg *config)
{
    *config = (struct book8_netcfg){0};
    if (BOOK8_NET_DEFAULT_IP[0] == '\0') {
        return false;
    }
    return book8_net_parse_ip(BOOK8_NET_DEFAULT_IP, &config->ip)
        && book8_net_parse_ip(BOOK8_NET_DEFAULT_MASK, &config->mask)
        && (BOOK8_NET_DEFAULT_GATEWAY[0] == '\0'
            || book8_net_parse_ip(BOOK8_NET_DEFAULT_GATEWAY, &config->gateway))
        && (BOOK8_NET_DEFAULT_TELEMETRY[0] == '\0'
            || book8_net_parse_ip(BOOK8_NET_DEFAULT_TELEMETRY, &config->telemetry))
        && netcfg_valid(config);
}

enum book8_net_result network_apply_config(const struct book8_netcfg *config,
    const char *source)
{
    enum book8_net_result result = book8_dhcp_stop(&dhcp_client);
    if (result != BOOK8_NET_OK)
        return result;
    result = book8_net_configure(&kernel_net, config->ip,
        config->mask, config->gateway);
    if (result != BOOK8_NET_OK) {
        return result;
    }
    network_config_source = source;
    network_settings.mode = BOOK8_NETWORK_STATIC;
    network_settings.ip = config->ip;
    network_settings.mask = config->mask;
    network_settings.gateway = config->gateway;
    network_settings.telemetry = config->telemetry;
    uint32_t target = config->telemetry;
    if (target != 0u && !network_calibrate_clock()) {
        console_write("BOOK8-NET: clock calibration failed; telemetry disabled.\n");
        target = 0u;
    }
    enum book8_net_result telemetry = net_telemetry_init(&kernel_net, target);
    if (telemetry == BOOK8_NET_OK) {
        telemetry_target = target;
        telemetry_last_tsc = read_tsc();
        telemetry_last_result = BOOK8_NET_OK;
    } else {
        console_write("BOOK8-NET: telemetry ");
        console_write(book8_net_result_string(telemetry));
        console_write("\n");
    }
    console_write("BOOK8-NET: config ");
    console_write(source);
    console_write(" IP=");
    network_print_ip(kernel_net.ip);
    console_write(" MASK=");
    network_print_ip(kernel_net.mask);
    console_write(" GATEWAY=");
    network_print_ip(kernel_net.gateway);
    console_write(" TELEMETRY=");
    network_print_ip(telemetry_target);
    console_write(telemetry_target != 0u ? ":8080\n" : "\n");
    return BOOK8_NET_OK;
}

uint64_t network_milliseconds(void)
{
    uint64_t ticks = read_tsc() - boot_tsc;
    return tsc_hz == 0u ? 0u : ticks / tsc_hz * 1000u
        + ticks % tsc_hz * 1000u / tsc_hz;
}

void network_dhcp_service(void)
{
    if (network_settings.mode != BOOK8_NETWORK_DHCP || tsc_hz == 0u)
        return;
    enum book8_net_result result = book8_dhcp_poll(&dhcp_client, network_milliseconds());
    if (result != BOOK8_NET_OK) {
        console_write("BOOK8-DHCP: ");
        console_write(book8_net_result_string(result));
        console_write("\n");
    }
    if (dhcp_generation == dhcp_client.generation)
        return;
    dhcp_generation = dhcp_client.generation;
    network_config_source = dhcp_client.fallback_active ? "DHCP timeout/static fallback"
        : kernel_net.ip != 0u ? "DHCP" : "DHCP waiting";
    result = net_telemetry_init(&kernel_net, 0u);
    if (result == BOOK8_NET_OK) {
        telemetry_target = 0u;
        result = net_telemetry_init(&kernel_net,
            kernel_net.ip != 0u ? network_settings.telemetry : 0u);
    }
    if (result == BOOK8_NET_OK)
        telemetry_target = kernel_net.ip != 0u ? network_settings.telemetry : 0u;
    else {
        console_write("BOOK8-DHCP: collector route unavailable: ");
        console_write(book8_net_result_string(result));
        console_write("\n");
    }
    char note[128];
    snprintf(note, sizeof(note), "DHCP %s ip=%08x mask=%08x gw=%08x dns=%08x seconds=%u",
        dhcp_client.fallback_active ? "fallback" : "lease", kernel_net.ip,
        kernel_net.mask, kernel_net.gateway, dhcp_client.dns,
        dhcp_client.lease_seconds);
    pcie_log(note);
    telemetry_last_tsc = 0u;
}

bool book8_network_settings_get(struct book8_network_status *status)
{
    if (status == NULL)
        return false;
    uint64_t now = tsc_hz != 0u ? network_milliseconds() : 0u;
    *status = (struct book8_network_status){.settings = network_settings,
        .assigned_ip = kernel_net.ip, .assigned_mask = kernel_net.mask,
        .assigned_gateway = kernel_net.gateway,
        .dns = network_settings.mode == BOOK8_NETWORK_STATIC
            ? network_settings.dns : dhcp_client.dns,
        .lease_seconds = dhcp_client.lease_seconds == UINT32_MAX ? UINT32_MAX
            : dhcp_client.expires > now ? (uint32_t)((dhcp_client.expires - now) / 1000u) : 0u,
        .state = network_suspended || cmdline_network_off ? "disabled"
            : dhcp_client.fallback_active ? "DHCP timeout/static fallback"
            : network_settings.mode == BOOK8_NETWORK_DHCP
            ? book8_dhcp_state_string(dhcp_client.state) : "forced static",
        .ready = network_ready && !network_suspended && !cmdline_network_off};
    return true;
}

bool book8_network_settings_apply(const struct book8_network_settings *settings, bool persist)
{
    if (!network_ready || !book8_network_settings_valid(settings) || cmdline_network_off) {
        console_write("BOOK8-NET: settings rejected (NIC unavailable, boot disabled or invalid values)\n");
        return false;
    }
    if (!network_calibrate_clock()) {
        console_write("BOOK8-NET: clock unavailable; settings rejected\n");
        return false;
    }
    if (persist) {
        uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
        if (!book8_network_settings_encode(settings, bytes)
            || !fs_write_file(BOOK8_NETWORK_SETTINGS_PATH, bytes, sizeof(bytes))) {
            console_write("BOOK8-NET: cannot save " BOOK8_NETWORK_SETTINGS_PATH "\n");
            return false;
        }
        static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
        char ip[16], mask[16], gateway[16], dns[16];
        registry_format_ipv4(settings->ip, ip, sizeof(ip));
        registry_format_ipv4(settings->mask, mask, sizeof(mask));
        registry_format_ipv4(settings->gateway, gateway, sizeof(gateway));
        registry_format_ipv4(settings->dns, dns, sizeof(dns));
        if (!reg_set_str(path, "NetMode",
                settings->mode == BOOK8_NETWORK_DHCP ? "DHCP" : "Static")
            || !reg_set_str(path, "StaticIP", ip)
            || !reg_set_str(path, "Netmask", mask)
            || !reg_set_str(path, "Gateway", gateway)
            || !reg_set_str(path, "DNS", dns) || !reg_save()) {
            console_write("BOOK8-NET: registry settings save failed\n");
            return false;
        }
    }
    if (book8_config_value(BOOK8_CFG_NETWORK) == 0u) {
        network_settings = *settings;
        console_write("BOOK8-NET: settings retained; network disabled in system settings\n");
        return true;
    }
    if (settings->mode == BOOK8_NETWORK_STATIC) {
        struct book8_netcfg config = {.ip = settings->ip, .mask = settings->mask,
            .gateway = settings->gateway, .telemetry = settings->telemetry};
        enum book8_net_result result = network_apply_config(&config, "forced static");
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-NET: static apply failed\n");
            return false;
        }
        dhcp_client.dns = settings->dns;
    } else {
        enum book8_net_result result = book8_dhcp_stop(&dhcp_client);
        if (result == BOOK8_NET_OK)
            result = net_telemetry_init(&kernel_net, 0u);
        if (result == BOOK8_NET_OK) {
            uint32_t xid = (uint32_t)read_tsc() ^ BOOK8_BUILD_ID
                ^ ((uint32_t)rtl_nic.mac[4] << 8u) ^ rtl_nic.mac[5];
            if (xid == 0u)
                xid = 1u;
            result = book8_dhcp_start(&dhcp_client, &kernel_net, network_milliseconds(),
                xid, settings->retry_ms, pcie_log);
            if (result == BOOK8_NET_OK)
                result = book8_dhcp_set_fallback(&dhcp_client,
                    settings->ip, settings->mask, settings->gateway);
        }
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-DHCP: start failed: ");
            console_write(book8_net_result_string(result));
            console_write("\n");
            return false;
        }
        telemetry_target = 0u;
        dhcp_generation = dhcp_client.generation;
        network_config_source = "DHCP waiting";
    }
    network_settings = *settings;
    return true;
}

bool network_enabled_service(void)
{
    if (cmdline_network_off || book8_config_value(BOOK8_CFG_NETWORK) == 0u) {
        if (!network_suspended) {
            enum book8_net_result result = book8_dhcp_stop(&dhcp_client);
            if (result == BOOK8_NET_OK)
                result = book8_net_configure(&kernel_net, 0u, 0u, 0u);
            if (result == BOOK8_NET_OK)
                result = net_telemetry_init(&kernel_net, 0u);
            if (result != BOOK8_NET_OK) {
                console_write("BOOK8-NET: suspend failed: ");
                console_write(book8_net_result_string(result));
                console_write("\n");
                return false;
            }
            telemetry_target = 0u;
            network_suspended = true;
            network_config_source = "disabled";
        }
        return false;
    }
    if (network_suspended) {
        struct book8_network_settings settings = network_settings;
        if (!book8_network_settings_apply(&settings, false))
            return false;
        network_suspended = false;
    }
    return true;
}

bool book8_network_settings_renew(void)
{
    if (!network_ready || network_suspended || cmdline_network_off
        || book8_config_value(BOOK8_CFG_NETWORK) == 0u
        || network_settings.mode != BOOK8_NETWORK_DHCP) {
        console_write("BOOK8-DHCP: renew requires DHCP mode and ready NIC\n");
        return false;
    }
    enum book8_net_result result = book8_dhcp_renew(&dhcp_client, network_milliseconds());
    if (result != BOOK8_NET_OK) {
        console_write("BOOK8-DHCP: renew failed\n");
        return false;
    }
    return true;
}

void network_early_telemetry(void)
{
    if (!network_ready || cmdline_network_off) {
        pcie_log("PCIE1 PULSE skipped: NIC unavailable or book8.net=off");
        return;
    }
    struct book8_netcfg config = {0};
    (void)network_baked_config(&config);
    network_settings.ip = config.ip;
    network_settings.mask = config.mask;
    network_settings.gateway = config.gateway;
    (void)book8_net_parse_ip(BOOK8_NET_DEFAULT_TELEMETRY, &network_settings.telemetry);
    if (!book8_network_settings_apply(&network_settings, false))
        pcie_log("PCIE1 PULSE waiting: DHCP start unavailable");
    uint32_t id = pci_read32(1u, 0u, 0u, 0u);
    uint32_t command = id == BOOK8_PCIE_TARGET_ID
        ? pci_read32(1u, 0u, 0u, 4u) : UINT32_MAX;
    char text[128];
    snprintf(text, sizeof(text),
        "PCIE1 PULSE id=%08x cmd=%08x lane=unknown", id, command);
    pcie_log(text);
    pcie_log("PCIE1 PULSE queued until DHCP lease or explicit static configuration");
}

void network_save_command(void)
{
    uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
    struct book8_network_settings saved = network_settings;
    if (saved.mode == BOOK8_NETWORK_STATIC)
        saved.telemetry = telemetry_target;
    if (!book8_network_settings_encode(&saved, bytes)
        || !fs_write_file(BOOK8_NETWORK_SETTINGS_PATH, bytes, sizeof(bytes))) {
        console_write("NETSAVE: cannot write " BOOK8_NETWORK_SETTINGS_PATH "\n");
        return;
    }
    console_write("NETSAVE: wrote mode/settings to " BOOK8_NETWORK_SETTINGS_PATH "\n");
}

void network_boot_configure(void)
{
    network_boot_tsc = read_tsc();
    if (boot_relaunch_count != 0u) {
        char text[64];
        snprintf(text, sizeof(text), "relaunched kernel #%u build %u",
            (unsigned)boot_relaunch_count, (unsigned)BOOK8_BUILD_ID);
        console_write("BOOK8-RELAUNCH: ");
        console_write(text);
        console_write("\n");
        telemetry_event(text);
    }
    if (!network_ready) {
        return;
    }
    push_agent_start();
    if (cmdline_network_off) {
        console_write("BOOK8-NET: book8.net=off; baked/file config skipped.\n");
        return;
    }
    {
        static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
        struct book8_network_settings saved = network_settings;
        char mode[BOOK8_REGISTRY_VALUE_MAX], text[BOOK8_REGISTRY_VALUE_MAX];
        bool have_registry = reg_get_str(path, "NetMode", mode, sizeof(mode));
        if (have_registry) {
            if (strcmp(mode, "DHCP") == 0) {
                saved.mode = BOOK8_NETWORK_DHCP;
            } else if (strcmp(mode, "Static") == 0) {
                saved.mode = BOOK8_NETWORK_STATIC;
            } else {
                console_write("BOOK8-NET: invalid registry mode; retaining DHCP defaults\n");
                return;
            }
            if (reg_get_str(path, "StaticIP", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.ip)) {
                console_write("BOOK8-NET: invalid registry StaticIP\n");
                return;
            }
            if (reg_get_str(path, "Netmask", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.mask)) {
                console_write("BOOK8-NET: invalid registry Netmask\n");
                return;
            }
            if (reg_get_str(path, "Gateway", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.gateway)) {
                console_write("BOOK8-NET: invalid registry Gateway\n");
                return;
            }
            if (reg_get_str(path, "DNS", text, sizeof(text))
                && !registry_parse_ipv4(text, &saved.dns)) {
                console_write("BOOK8-NET: invalid registry DNS\n");
                return;
            }
            if (!book8_network_settings_apply(&saved, false)) {
                console_write("BOOK8-NET: registry network settings failed to apply\n");
            }
            return;
        }
    }
    uint8_t settings_bytes[BOOK8_NETWORK_SETTINGS_BYTES + 1u];
    uint32_t settings_size = 0u;
    if (fs_read_file(BOOK8_NETWORK_SETTINGS_PATH, settings_bytes, sizeof(settings_bytes), &settings_size)) {
        struct book8_network_settings saved;
        if (book8_network_settings_decode(&saved, settings_bytes, settings_size)) {
            if (!book8_network_settings_apply(&saved, false))
                console_write("BOOK8-NET: saved network mode failed to apply\n");
            return;
        }
        console_write("BOOK8-NET: malformed " BOOK8_NETWORK_SETTINGS_PATH "; retaining DHCP defaults\n");
        return;
    }
    static char text[BOOK8_NETCFG_TEXT_MAX];
    struct book8_netcfg config;
    const char *source = NULL;
    uint32_t size = 0u;
    if (fs_read_file(BOOK8_NETCFG_PATH, text, sizeof(text), &size)) {
        if (book8_netcfg_parse(text, size, &config)) {
            source = BOOK8_NETCFG_PATH;
        } else {
            console_write("BOOK8-NET: ignoring malformed " BOOK8_NETCFG_PATH "\n");
        }
    }
    if (source == NULL) {
        console_write("BOOK8-NET: DHCP default; static fallback only after 30-second timeout\n");
        return;
    }
    struct book8_network_settings legacy = {.mode = BOOK8_NETWORK_DHCP,
        .ip = config.ip, .mask = config.mask, .gateway = config.gateway,
        .telemetry = config.telemetry, .retry_ms = network_settings.retry_ms};
    if (!book8_network_settings_apply(&legacy, false))
        console_write("BOOK8-NET: legacy DHCP fallback settings failed to apply\n");
    else
        console_write("BOOK8-NET: legacy static values loaded as DHCP timeout fallback only\n");
}
