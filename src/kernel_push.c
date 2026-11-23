#include "kernel_internal.h"

/* Push agent and relaunch command execution. */

#if BOOK8_PUSH
uint64_t push_milliseconds(void *ctx)
{
    return tsc_hz != 0u ? netboot_milliseconds(ctx) : 0u;
}
#endif

#if BOOK8_PUSH
void push_entropy(void *ctx, uint8_t out[BOOK8_SHA256_BYTES])
{
    (void)ctx;
    uint32_t a, b, c, d;
    uint64_t words[4] = {read_tsc(), network_boot_tsc,
        ((uint64_t)kernel_net.rx_frames << 32u) | kernel_net.udp_received, poll_tsc_total};
    cpu_id(1u, &a, &b, &c, &d);
    if ((c & (1u << 30u)) != 0u) {
        for (unsigned i = 0u; i < 10u; ++i) {
            uint64_t value;
            uint8_t ok;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) :: "cc");
            if (ok != 0u) {
                words[3] ^= value;
                break;
            }
        }
    }
    __builtin_memcpy(out, words, sizeof(words));
}
#endif

void push_agent_start(void)
{
#if BOOK8_PUSH
    static const struct book8_push_platform platform = {
        .milliseconds = push_milliseconds, .entropy = push_entropy
    };
    if (!network_ready || !network_calibrate_clock()) {
        return;
    }
    enum book8_net_result result = book8_push_init(&kernel_net, book8_push_key,
        BOOK8_BUILD_ID, &platform);
    console_write("BOOK8-PUSH: update agent UDP 8081 ");
    console_write(book8_net_result_string(result));
    console_write("\n");
#endif
}

void push_flush_result(uint32_t milliseconds)
{
    if (tsc_hz == 0u) {
        return;
    }
    uint64_t deadline = read_tsc() + tsc_hz / 1000u * milliseconds;
    struct book8_push_stats stats;
    do {
        (void)book8_net_poll(&kernel_net);
        (void)book8_push_service();
        book8_push_get_stats(&stats);
        __asm__ volatile ("pause");
    } while (stats.result_unsent && read_tsc() < deadline);
    /* Give the NIC time to put the final frame on the wire. */
    uint64_t drain = read_tsc() + tsc_hz / 20u;
    while (read_tsc() < drain) {
        (void)book8_net_poll(&kernel_net);
    }
}

void push_finish(enum book8_push_status status, const char *message)
{
    console_write("BOOK8-PUSH: ");
    console_write(message);
    console_write("\n");
    telemetry_event(message);
    (void)book8_push_complete(status, message);
}

bool push_valid_83(const char *name)
{
    size_t base = 0u, extension = 0u;
    bool dot = false;
    for (const char *c = name; *c != '\0'; ++c) {
        if (*c == '.') {
            if (dot || base == 0u) {
                return false;
            }
            dot = true;
            continue;
        }
        bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
            || *c == '_' || *c == '-';
        if (!ok) {
            return false;
        }
        if (dot ? ++extension > 3u : ++base > 8u) {
            return false;
        }
    }
    return base != 0u && (!dot || extension != 0u);
}

const char *push_fetch(const struct book8_push_request *request, size_t capacity)
{
    static const struct book8_tftp_clock clock = {
        .milliseconds = netboot_milliseconds, .idle = netboot_idle
    };
    uint8_t *load = (uint8_t *)(uintptr_t)BOOK8_NETBOOT_ADDRESS;
    if (request->size > capacity) {
        return "image larger than staging window";
    }
    if (!netboot_memory_available()) {
        return "68..76 MiB staging RAM not verified free";
    }
    enum book8_net_result result = tftp_client_init(&kernel_net, request->source_ip,
        capacity, &clock);
    if (result == BOOK8_NET_OK) {
        console_write("BOOK8-PUSH: TFTP fetch ");
        console_write(request->remote);
        console_write("\n");
        result = tftp_fetch_kernel(request->remote, load);
    }
    if (result != BOOK8_NET_OK) {
        snprintf(push_message, sizeof(push_message), "TFTP %s (remote error %u)",
            book8_net_result_string(result), (unsigned)tftp_remote_error());
        return push_message;
    }
    if (tftp_fetched_size() != request->size) {
        snprintf(push_message, sizeof(push_message), "size mismatch: got %u expected %u",
            (unsigned)tftp_fetched_size(), (unsigned)request->size);
        return push_message;
    }
    uint8_t digest[BOOK8_SHA256_BYTES];
    book8_sha256(load, request->size, digest);
    if (!book8_digest_equal(digest, request->sha256, sizeof(digest))) {
        return "SHA-256 mismatch";
    }
    return NULL;
}

bool relaunch_prepare_staged(size_t bytes, struct book8_relaunch_info *info,
    const char **error)
{
    enum book8_relaunch_result result = book8_relaunch_prepare(
        (const uint8_t *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, bytes, BOOK8_NETBOOT_ADDRESS,
        (const uint8_t *)(uintptr_t)netboot_info_address,
        (uint8_t *)(uintptr_t)RELAUNCH_PLAN_PHYSICAL, RELAUNCH_PLAN_PHYSICAL, info);
    *error = book8_relaunch_result_string(result);
    return result == BOOK8_RELAUNCH_OK;
}

void push_execute(const struct book8_push_request *request)
{
    switch (request->command) {
    case BOOK8_PUSH_STATUS: {
        uint64_t uptime = tsc_hz != 0u ? (read_tsc() - network_boot_tsc) / tsc_hz : 0u;
        snprintf(push_message, sizeof(push_message),
            "build %u up %us relaunch %u config %s telemetry-failures %u boot-update=1",
            (unsigned)BOOK8_BUILD_ID, (unsigned)uptime, (unsigned)boot_relaunch_count,
            network_config_source, (unsigned)telemetry_failures);
        (void)book8_push_complete(BOOK8_PUSH_OK, push_message);
        return;
    }
    case BOOK8_PUSH_CONFIG: {
        struct book8_netcfg config = {
            .ip = request->ip, .mask = request->mask,
            .gateway = request->gateway, .telemetry = request->telemetry
        };
        bool persist = (request->flags & BOOK8_PUSH_FLAG_PERSIST) != 0u;
        if (!netcfg_valid(&config)) {
            push_finish(BOOK8_PUSH_BAD_REQUEST, "push: invalid IPv4 configuration");
            return;
        }
        if (persist) {
            struct book8_network_settings saved = {.mode = BOOK8_NETWORK_STATIC,
                .ip = config.ip, .mask = config.mask, .gateway = config.gateway,
                .telemetry = config.telemetry, .retry_ms = network_settings.retry_ms};
            uint8_t bytes[BOOK8_NETWORK_SETTINGS_BYTES];
            if (!book8_network_settings_encode(&saved, bytes)
                || !fs_write_file(BOOK8_NETWORK_SETTINGS_PATH, bytes, sizeof(bytes))) {
                push_finish(BOOK8_PUSH_FAILED, "push: cannot write A:\\BOOK8DHCP.CFG");
                return;
            }
        }
        push_finish(BOOK8_PUSH_ACCEPTED, persist
            ? "push: static config saved to A:\\BOOK8DHCP.CFG; applying"
            : "push: config applying (runtime only)");
        push_flush_result(1000u);
        enum book8_net_result result = network_apply_config(&config,
            persist ? BOOK8_NETCFG_PATH : "push");
        if (result != BOOK8_NET_OK) {
            console_write("BOOK8-PUSH: config apply ");
            console_write(book8_net_result_string(result));
            console_write("\n");
        }
        return;
    }
    case BOOK8_PUSH_FILE: {
        if (!push_valid_83(request->local)) {
            push_finish(BOOK8_PUSH_BAD_REQUEST,
                "push: destination must be an 8.3 upper-case name");
            return;
        }
        const char *error = push_fetch(request, BOOK8_TFTP_CAPACITY);
        char path[4u + BOOK8_PUSH_LOCAL_MAX];
        snprintf(path, sizeof(path), "A:\\%s", request->local);
        if (error == NULL && !fs_write_file(path,
            (const void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, request->size)) {
            error = "cannot write file on A: (is a FAT volume mounted?)";
        }
        if (error != NULL) {
            char text[BOOK8_PUSH_MESSAGE_MAX];
            snprintf(text, sizeof(text), "push: FILE failed: %s", error);
            push_finish(BOOK8_PUSH_FAILED, text);
            return;
        }
        snprintf(push_message, sizeof(push_message), "push: wrote %s (%u bytes, SHA-256 ok)",
            path, (unsigned)request->size);
        push_finish(BOOK8_PUSH_OK, push_message);
        return;
    }
    case BOOK8_PUSH_INSTALL_KERNEL: {
        const char *error = push_fetch(request, 1024u * 1024u);
        struct book8_relaunch_info info;
        if (error == NULL) {
            const char *validation_error;
            if (!relaunch_prepare_staged(request->size, &info, &validation_error))
                error = validation_error;
        }
        if (error == NULL && !fs_replace_boot_kernel(
            (const void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, request->size))
            error = "boot file replacement failed; inspect console; do not reboot";
        block_cache_invalidate(NULL);
        if (error != NULL) {
            snprintf(push_message, sizeof(push_message), "push: INSTALL_KERNEL failed: %s", error);
            push_finish(BOOK8_PUSH_FAILED, push_message);
            return;
        }
        push_finish(BOOK8_PUSH_OK, "push: A:\\boot\\book8.elf persisted and readback verified; reboot separately");
        return;
    }
    case BOOK8_PUSH_KERNEL: {
        struct book8_relaunch_info info;
        const char *error = push_fetch(request, RELAUNCH_STAGE_CAPACITY);
        if (error == NULL) {
            const char *prepare_error;
            if (!relaunch_prepare_staged(request->size, &info, &prepare_error)) {
                error = prepare_error;
            }
        }
        if (error != NULL) {
            char text[BOOK8_PUSH_MESSAGE_MAX];
            snprintf(text, sizeof(text), "push: KERNEL rejected: %s", error);
            push_finish(BOOK8_PUSH_FAILED, text);
            return;
        }
        snprintf(push_message, sizeof(push_message),
            "push: kernel verified (%u bytes); relaunch #%u entry 0x%x",
            (unsigned)request->size, (unsigned)info.relaunch_count, (unsigned)info.entry);
        push_finish(BOOK8_PUSH_ACCEPTED, push_message);
        push_flush_result(1000u);
        relaunch_enter_now();
    }
    case BOOK8_PUSH_REBOOT:
        push_finish(BOOK8_PUSH_ACCEPTED, "push: rebooting");
        push_flush_result(1000u);
        reboot_system();
        return;
    default:
        push_finish(BOOK8_PUSH_BAD_REQUEST, "push: unknown command");
        return;
    }
}

void push_agent_service(void)
{
    struct book8_push_request request;
    if (push_executing) {
        return;
    }
    (void)book8_push_service();
    if (!book8_push_take(&request)) {
        return;
    }
    push_executing = true;
    console_write("BOOK8-PUSH: ");
    console_write(book8_push_command_name(request.command));
    console_write(" from ");
    network_print_ip(request.source_ip);
    console_write("\n");
    push_execute(&request);
    push_executing = false;
}

bool relaunch_disable_dma(void)
{
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint8_t slot = 0u; slot < 32u; ++slot) {
            uint8_t count = 1u;
            for (uint8_t fn = 0u; fn < count; ++fn) {
                uint16_t vendor = (uint16_t)pci_read32((uint8_t)bus, slot, fn, 0u);
                if (vendor == 0xffffu || vendor == 0u) {
                    continue;
                }
                uint32_t header = (pci_read32((uint8_t)bus, slot, fn, 0x0cu) >> 16u) & 0xffu;
                if (fn == 0u && (header & 0x80u) != 0u) {
                    count = 8u;
                }
                uint8_t class_code = (uint8_t)(pci_read32((uint8_t)bus, slot, fn, 8u) >> 24u);
                if ((header & 0x7fu) != 0u || class_code == 0x03u || class_code == 0x06u) {
                    continue;
                }
                uint16_t command = pci_read16((uint8_t)bus, slot, fn, 4u);
                if ((command & 4u) == 0u) {
                    continue;
                }
                pci_write16((uint8_t)bus, slot, fn, 4u, (uint16_t)(command & ~4u));
                if ((pci_read16((uint8_t)bus, slot, fn, 4u) & 4u) != 0u) {
                    return false;
                }
            }
        }
    }
    return true;
}

void smp_quiesce_for_relaunch(void)
{
    if (!local_apic_ready || smp_topology.cpu_count <= 1u) {
        return;
    }
    uint64_t expected = 0u;
    for (uint32_t cpu = 1u; cpu < smp_topology.cpu_count; ++cpu) {
        expected |= 1ull << cpu;
    }
    __atomic_store_n(&smp_relaunch_requested, 1u, __ATOMIC_RELEASE);
    uint64_t pending = expected;
    uint64_t mask = 0u;
    uint64_t started = pit_clock_ticks();
    for (;;) {
        mask = __atomic_load_n(&smp_relaunch_halted_mask, __ATOMIC_ACQUIRE);
        pending = expected & ~mask;
        if (pending == 0u) {
            return;
        }
        if (pit_clock_ticks() - started >= PIT_HZ * 4u) {
            break;
        }
        for (uint32_t cpu = 1u; cpu < smp_topology.cpu_count; ++cpu) {
            if ((pending & (1ull << cpu)) != 0u) {
                (void)book8_apic_send_ipi(smp_topology.apic_ids[cpu],
                    SMP_SCHEDULE_IPI_VECTOR);
            }
        }
        __asm__ volatile ("pause");
    }
    if (pending != 0u) {
        char text[128];
        snprintf(text, sizeof(text),
            "BOOK8-RELAUNCH: AP quiesce timed out (requested=%u mask=0x%llx pending=0x%llx); halting.\n",
            (unsigned)__atomic_load_n(&smp_relaunch_requested, __ATOMIC_ACQUIRE),
            (unsigned long long)mask,
            (unsigned long long)pending);
        console_write(text);
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}

void relaunch_enter_now(void)
{
    console_write("BOOK8-RELAUNCH: quiescing DMA/interrupts; entering new kernel\n");
    smp_quiesce_for_relaunch();
    __asm__ volatile ("cli" ::: "memory");
    outb(0x21u, 0xffu);
    outb(0xa1u, 0xffu);
    if (rtl_nic.regs != NULL) {
        rtl_nic.regs[RTL8168_CR] = 0u;
        (void)rtl_nic.regs[RTL8168_CR];
    }
    if (!relaunch_disable_dma()) {
        console_write("BOOK8-RELAUNCH: failed to disable PCI DMA; halting.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    network_ready = false;
    __asm__ volatile ("mfence" ::: "memory");
    book8_relaunch_enter((uint8_t *)(uintptr_t)RELAUNCH_PLAN_PHYSICAL, RELAUNCH_PLAN_PHYSICAL);
}

void relaunch_command(char *argument)
{
    char path[96];
    char *name = skip_spaces(argument);
    if (*name == '\0') {
        console_write("RELAUNCH: expected <kernel.elf> on A: (Multiboot2 ELF64).\n");
        return;
    }
    if (name[0] != '\0' && name[1] == ':') {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "A:\\%s", name);
    }
    if (!netboot_memory_available()) {
        console_write("RELAUNCH: 68..76 MiB is not verified free usable RAM.\n");
        return;
    }
    uint32_t size = 0u;
    if (!fs_read_file(path, (void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS,
        RELAUNCH_STAGE_CAPACITY, &size) || size == 0u) {
        console_write("RELAUNCH: cannot read ");
        console_write(path);
        console_write(" (root files only, at most 8 MiB - 64 KiB).\n");
        return;
    }
    if (!boot_hash_verify_path_memory(path,
            (const void *)(uintptr_t)BOOK8_NETBOOT_ADDRESS, size)) {
        console_write("RELAUNCH: boot hash verification failed.\n");
        return;
    }
    struct book8_relaunch_info info;
    const char *error;
    if (!relaunch_prepare_staged(size, &info, &error)) {
        console_write("RELAUNCH: ");
        console_write(error);
        console_write("\n");
        return;
    }
    char text[160];
    snprintf(text, sizeof(text),
        "RELAUNCH: %s %u bytes, %u segments 0x%x-0x%x, entry 0x%x, relaunch #%u\n",
        path, (unsigned)size, (unsigned)info.segments, (unsigned)info.image_low,
        (unsigned)info.image_high, (unsigned)info.entry, (unsigned)info.relaunch_count);
    console_write(text);
    relaunch_enter_now();
}
