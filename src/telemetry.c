#include "kernel_internal.h"

/* Telemetry sampling, probe commands and health reporting. */

void telemetry_event(const char *text)
{
    size_t i = 0u;
    for (; text[i] != '\0' && i + 1u < sizeof(telemetry_note); ++i) {
        telemetry_note[i] = text[i];
    }
    telemetry_note[i] = '\0';
    telemetry_note_packets = 10u;
}

uint32_t health_flags(uint8_t phy)
{
    struct book8_push_stats push;
    book8_push_get_stats(&push);
    uint32_t flags = 0u;
    flags |= kernel_net.ip != 0u ? BOOK8_HEALTH_IPV4 : 0u;
    flags |= ascii_equal(network_config_source, "baked") ? BOOK8_HEALTH_CONFIG_BAKED : 0u;
    flags |= ascii_equal(network_config_source, BOOK8_NETCFG_PATH)
        ? BOOK8_HEALTH_CONFIG_FILE : 0u;
    flags |= push.listening ? BOOK8_HEALTH_PUSH_AGENT : 0u;
    flags |= (phy & RTL8168_PHY_LINK) != 0u ? BOOK8_HEALTH_PHY_LINK : 0u;
    flags |= ide_controller_count + ahci_controller_count != 0u ? BOOK8_HEALTH_STORAGE : 0u;
    flags |= book8_usb_keyboard_active() ? BOOK8_HEALTH_USB_KEYBOARD : 0u;
    flags |= book8_usb_mouse_active() ? BOOK8_HEALTH_USB_MOUSE : 0u;
    flags |= ps2_keyboard_controller_ready ? BOOK8_HEALTH_PS2_KEYBOARD : 0u;
    flags |= ps2_mouse_available ? BOOK8_HEALTH_PS2_MOUSE : 0u;
    flags |= screen.pixels != NULL ? BOOK8_HEALTH_FRAMEBUFFER : 0u;
    flags |= pci_sound_present ? BOOK8_HEALTH_PCI_SOUND : 0u;
    flags |= acpi_s5_available ? BOOK8_HEALTH_ACPI_S5 : 0u;
    flags |= boot_relaunch_count != 0u ? BOOK8_HEALTH_RELAUNCHED : 0u;
    return flags;
}

uint32_t tsc_to_us(uint64_t ticks)
{
    if (tsc_hz == 0u) {
        return 0u;
    }
    uint64_t us = ticks / tsc_hz * 1000000u + ticks % tsc_hz * 1000000u / tsc_hz;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

bool telemetry_sample(const char *pulse)
{
    if (!initialization_complete || telemetry_target == 0u
        || kernel_net.ip == 0u || tsc_hz == 0u) {
        return false;
    }
    uint64_t now = read_tsc();
    bool pcie_pending = pcie_event_head != pcie_event_tail
        || pcie_event_dropped != 0u;
    uint64_t interval = pcie_pending && telemetry_last_result == BOOK8_NET_OK
        ? tsc_hz / 100u : tsc_hz;
    if (pulse == NULL && now - telemetry_last_tsc < interval) {
        return false;
    }
    telemetry_last_tsc = now;
    heap_stats_t heap;
    heap_get_stats(&heap);
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    struct net_telemetry_metrics metrics = {
        .cpu_flags = (gui_active ? BOOK8_TELEMETRY_CPU_DESKTOP : 0u)
            | ((flags & 0x200u) ? BOOK8_TELEMETRY_CPU_IRQ_ENABLED : 0u)
            | (rtl_nic.running ? BOOK8_TELEMETRY_CPU_NIC_RUNNING : 0u),
        .cpu_mhz = (uint32_t)(tsc_hz / 1000000u),
        .memory_total_bytes = memory_total_kb * 1024u,
        .kernel_reserved_bytes = (uint64_t)(uintptr_t)__kernel_end,
        .heap_used_bytes = heap.used, .heap_total_bytes = heap.total,
        .tx_packets = rtl_nic.tx_packets, .rx_packets = rtl_nic.rx_packets,
        .tx_dropped = rtl_nic.tx_dropped, .rx_dropped = rtl_nic.rx_dropped,
        .tx_timeouts = rtl_nic.tx_timeouts
    };
    uint8_t phy = 0u;
    struct b8_rtl8168_ring_state ring_state = {0};
    if (rtl8168_driver.descriptor != NULL
        && rtl8168_driver.descriptor->ioctl != NULL) {
        (void)rtl8168_driver.descriptor->ioctl(
            B8_RTL8168_IOCTL_PHY_STATUS, &phy);
        (void)rtl8168_driver.descriptor->ioctl(
            B8_RTL8168_IOCTL_RING_STATE, &ring_state);
    }
    struct book8_push_stats push;
    book8_push_get_stats(&push);
    struct net_telemetry_extended extended = {
        .build_id = BOOK8_BUILD_ID, .health_flags = health_flags(phy),
        .relaunch_count = boot_relaunch_count, .phy_status = phy,
        .tx_ring_pending = ring_state.tx_pending,
        .rx_ring_ready = ring_state.rx_ready,
        .net_rx_frames = kernel_net.rx_frames, .net_tx_frames = kernel_net.tx_frames,
        .net_invalid_frames = kernel_net.invalid_frames,
        .net_ignored_frames = kernel_net.ignored_frames,
        .net_rx_errors = kernel_net.rx_errors, .net_tx_errors = kernel_net.tx_errors,
        .arp_replies = kernel_net.arp_replies, .echo_replies = kernel_net.echo_replies,
        .udp_received = kernel_net.udp_received,
        .arp_entries = book8_net_arp_entries(&kernel_net),
        .poll_iterations = poll_iterations,
        .poll_avg_us = poll_iterations != 0u
            ? tsc_to_us(poll_tsc_total / poll_iterations) : 0u,
        .poll_max_us = tsc_to_us(poll_tsc_max),
        .telemetry_failures = telemetry_failures,
        .push_accepted = push.accepted, .push_rejected = push.rejected
    };
    poll_iterations = 0u;
    poll_tsc_total = 0u;
    poll_tsc_max = 0u;
    uint64_t elapsed = now - boot_tsc;
    uint64_t timestamp = elapsed / tsc_hz * 1000u
        + elapsed % tsc_hz * 1000u / tsc_hz;
    const char *note = gui_active ? "desktop service" : "CLI service";
    if (telemetry_note_packets != 0u && !pcie_pending && pulse == NULL) {
        --telemetry_note_packets;
        note = telemetry_note;
    }
    bool pcie_event = pulse == NULL && pcie_event_head != pcie_event_tail;
    bool pcie_overflow = pulse == NULL && !pcie_event && pcie_event_dropped != 0u;
    char pcie_overflow_note[BOOK8_TELEMETRY_LOG_SIZE];
    if (pcie_event) {
        note = pcie_boot_events[pcie_event_head % PCIE_BOOT_EVENTS];
    } else if (pcie_overflow) {
        snprintf(pcie_overflow_note, sizeof(pcie_overflow_note),
            "PCIE1 ERROR telemetry events lost: %u", pcie_event_dropped);
        note = pcie_overflow_note;
    }
    if (pulse != NULL)
        note = pulse;
    enum book8_net_result result = net_telemetry_send_v2(timestamp, &metrics,
        &extended, note);
    if (result == BOOK8_NET_OK) {
        if (pcie_event)
            ++pcie_event_head;
        else if (pcie_overflow)
            pcie_event_dropped = 0u;
    }
    if (result != BOOK8_NET_OK) {
        ++telemetry_failures;
    }
    /* Report transitions only: an absent collector must not flood the console. */
    if (result != telemetry_last_result) {
        console_write("BOOK8-TELEMETRY: ");
        console_write(book8_net_result_string(result));
        console_write("\n");
        telemetry_last_result = result;
    }
    return result == BOOK8_NET_OK;
}

void telemetry_service(void)
{
    telemetry_sample(NULL);
}

bool probe_telemetry_available(void)
{
    return network_ready && telemetry_target == 0xc0a83211u
        && kernel_net.ip != 0u && tsc_hz != 0u;
}

bool probe_emit(const char *record)
{
    if (!probe_telemetry_available()) {
        console_write("PROBE1 ERROR requires active telemetry to 192.168.50.17:8080\n");
        return false;
    }
    return telemetry_sample(record);
}

bool probe_hex(const char *text, uint32_t *value)
{
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    if (*text == '\0')
        return false;
    uint32_t result = 0u;
    for (; *text != '\0'; ++text) {
        uint32_t digit;
        if (*text >= '0' && *text <= '9')
            digit = (uint32_t)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (uint32_t)(*text - 'a') + 10u;
        else if (*text >= 'A' && *text <= 'F')
            digit = (uint32_t)(*text - 'A') + 10u;
        else
            return false;
        if (result > (UINT32_MAX - digit) / 16u)
            return false;
        result = result * 16u + digit;
    }
    *value = result;
    return true;
}

void probe_command(char *argument, bool ack)
{
    if (!manual_probe_initialized) {
        const struct book8_probe_ops ops = {
            .emit = probe_emit, .read = book8_read32_safe, .write = book8_write32_safe
        };
        book8_probe_init(&manual_probe, &pcie, &ops);
        manual_probe_initialized = true;
    }
    char *extra = argument;
    while (*extra != '\0' && *extra != ' ' && *extra != '\t')
        ++extra;
    if (*extra != '\0') {
        *extra++ = '\0';
        extra = skip_spaces(extra);
    }
    if (ack) {
        uint32_t id;
        enum book8_probe_phase phase = ascii_equal(extra, "INTENT")
            ? BOOK8_PROBE_INTENT : ascii_equal(extra, "RESULT")
            ? BOOK8_PROBE_RESULT : BOOK8_PROBE_IDLE;
        if (!parse_disk_number(argument, &id) || phase == BOOK8_PROBE_IDLE) {
            console_write("PROBE1 ERROR expected ACK <decimal-id> INTENT|RESULT\n");
            return;
        }
        if (phase == BOOK8_PROBE_INTENT && !probe_telemetry_available()) {
            manual_probe.sent = false;
            console_write("PROBE1 ERROR telemetry unavailable; no access; restore config then RETRY\n");
            return;
        }
        if (book8_probe_ack(&manual_probe, id, phase)
            && manual_probe.phase == BOOK8_PROBE_IDLE)
            console_write("PROBE1 ACK accepted; enter next index individually; no automatic advance\n");
    } else if (ascii_equal(argument, "STATUS") && *extra == '\0') {
        if (manual_probe.phase != BOOK8_PROBE_IDLE) {
            console_write(manual_probe.record);
            console_write("\n");
        }
    } else if (ascii_equal(argument, "RETRY") && *extra == '\0') {
        (void)book8_probe_retry(&manual_probe);
    } else if ((ascii_equal(argument, "LINKCAP") || ascii_equal(argument, "LINKSTATUS"))
        && *extra == '\0') {
        (void)book8_probe_begin_link(&manual_probe, ascii_equal(argument, "LINKSTATUS"));
    } else if ((ascii_equal(argument, "LTSSM") || ascii_equal(argument, "LINKWIDTH"))
        && *extra == '\0') {
        (void)book8_probe_begin(&manual_probe, ascii_equal(argument, "LTSSM")
            ? BOOK8_RS780_LC_STATE0 : BOOK8_RS780_LINK_WIDTH, 0u);
    } else {
        uint32_t index, mask = 0u;
        if (!probe_hex(argument, &index) || (*extra != '\0' && !probe_hex(extra, &mask))) {
            console_write("PROBE1 ERROR expected PCIEPROBE <A2/A5..AF>|LTSSM|LINKWIDTH|LINKCAP|LINKSTATUS|STATUS|RETRY\n");
            return;
        }
        (void)book8_probe_begin(&manual_probe, index, mask);
    }
    char text[128];
    snprintf(text, sizeof(text), "PROBE1 state=%s id=%u submitted=%u poisoned=%u\n",
        manual_probe.phase == BOOK8_PROBE_IDLE ? "IDLE"
            : manual_probe.phase == BOOK8_PROBE_INTENT ? "INTENT" : "RESULT",
        manual_probe.id, manual_probe.sent ? 1u : 0u, manual_probe.poisoned ? 1u : 0u);
    console_write(text);
    if (manual_probe.phase == BOOK8_PROBE_IDLE) {
        snprintf(text, sizeof(text), "PROBE1 acknowledged LTSSM-L0=%u LINKCAP=%u LINKSTATUS=%u link-preflight=%u; GPU-MMIO blocked\n",
            manual_probe.ltssm_acknowledged ? 1u : 0u,
            manual_probe.link_cap_acknowledged ? 1u : 0u,
            manual_probe.link_status_acknowledged ? 1u : 0u,
            book8_probe_link_verified(&manual_probe) ? 1u : 0u);
        console_write(text);
        if (manual_probe.link_cap_acknowledged && manual_probe.link_status_acknowledged) {
            uint32_t status = manual_probe.link_status >> 16u;
            snprintf(text, sizeof(text),
                "PROBE1 max-speed-code=%u max-width=x%u speed-code=%u width=x%u training=%u DLL-active=%u DLL-reporting=%u\n",
                manual_probe.link_cap & 0xfu, (manual_probe.link_cap >> 4u) & 0x3fu,
                status & 0xfu, (status >> 4u) & 0x3fu, (status >> 11u) & 1u,
                (status >> 13u) & 1u, (manual_probe.link_cap >> 20u) & 1u);
            console_write(text);
        }
    }
    if (manual_probe.phase != BOOK8_PROBE_IDLE)
        console_write("PROBE1 verify collector record, then ACK matching id and phase; RETRY sends record only\n");
}

void telemetry_command(char *argument)
{
    uint32_t target = 0u;
    if (!ascii_equal(argument, "OFF")
        && (!book8_net_parse_ip(argument, &target) || target == 0u)) {
        console_write("TELEMETRY: expected <target-ip> or OFF.\n");
        return;
    }
    if (target != 0u && !network_ready) {
        console_write("TELEMETRY: no supported NIC ready.\n");
        return;
    }
    enum book8_net_result result = net_telemetry_init(&kernel_net, target);
    if (result == BOOK8_NET_OK && target != 0u && tsc_hz == 0u) {
        pit_clock_start();
        (void)pit_clock_ticks();
        tsc_calibrate();
        if (tsc_hz == 0u) {
            enum book8_net_result stopped = net_telemetry_init(&kernel_net, 0u);
            console_write("TELEMETRY: clock calibration failed; stream stopped: ");
            console_write(book8_net_result_string(stopped));
            console_write("\n");
            telemetry_target = 0u;
            return;
        }
    }
    if (result == BOOK8_NET_OK) {
        telemetry_target = target;
        network_settings.telemetry = target;
        telemetry_last_tsc = read_tsc();
    }
    console_write("TELEMETRY: ");
    console_write(book8_net_result_string(result));
    console_write("\n");
}
