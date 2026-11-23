#include "kernel_internal.h"

/* PCIe bootstrap, bus drivers, GPU/NIC/audio device setup. */

const uint8_t *driver_trust_digest(const char *path)
{
    for (size_t i = 0u;
        i < sizeof(book8_driver_trust_table)
            / sizeof(book8_driver_trust_table[0]); ++i) {
        if (ascii_equal(path, book8_driver_trust_table[i].path))
            return book8_driver_trust_table[i].digest;
    }
    return NULL;
}

int32_t load_trusted_driver(const char *path,
    struct b8_loaded_driver *loaded)
{
    const uint8_t *digest = driver_trust_digest(path);
    if (digest == NULL)
        return -BOOK8_EPERM;
    int32_t status = b8_driver_load(path, digest, b8_kernel_hal_exports,
        sizeof(b8_kernel_hal_exports) / sizeof(b8_kernel_hal_exports[0]),
        loaded);
    char message[112];
    if (status == 0) {
        (void)snprintf(message, sizeof(message),
            "BOOK8-DRIVER: loaded %s\r\n", path);
    } else {
        (void)snprintf(message, sizeof(message),
            "BOOK8-DRIVER: load failed %s status=%ld\r\n", path,
            (long)status);
    }
    serial_write(message);
    return status;
}

bool usb_host_module_load(void)
{
    int32_t status = load_trusted_driver("A:\\DRIVERS\\USBHOST.DRV",
        &usb_host_driver);
    if (status != 0)
        return false;
    const struct book8_usb_module_api *api = NULL;
    status = usb_host_driver.descriptor->ioctl(BOOK8_USB_IOCTL_GET_API, &api);
    if (status != 0 || api == NULL
        || api->abi_version != BOOK8_USB_MODULE_API_VERSION
        || api->struct_size < sizeof(*api)) {
        serial_write("BOOK8-USB: module API validation failed\r\n");
        b8_driver_unload(&usb_host_driver);
        return false;
    }
    usb_host_api = api;
    return true;
}

bool hda_module_load(void)
{
    int32_t status = load_trusted_driver("A:\\DRIVERS\\AC97.DRV",
        &hda_driver);
    if (status != 0)
        return false;
    if (hda_driver.descriptor->ioctl == NULL) {
        serial_write("BOOK8-HDA: module API ioctl unavailable\r\n");
        b8_driver_unload(&hda_driver);
        return false;
    }
    const struct b8_hda_module_api *api = NULL;
    status = hda_driver.descriptor->ioctl(B8_HDA_IOCTL_GET_API, &api);
    if (status != 0 || api == NULL
        || api->abi_version != B8_HDA_MODULE_API_VERSION
        || api->struct_size < sizeof(*api)
        || api->get_device == NULL || api->is_available == NULL
        || api->get_volume == NULL || api->get_mute == NULL
        || api->set_volume == NULL || api->set_mute == NULL
        || api->play_wav == NULL || api->pause == NULL
        || api->stop == NULL || api->seek == NULL
        || api->get_stream_info == NULL || api->service == NULL
        || api->status_name == NULL || api->stream_result_name == NULL) {
        serial_write("BOOK8-HDA: module API validation failed\r\n");
        b8_driver_unload(&hda_driver);
        return false;
    }
    hda_api = api;
    hda_api->get_device(&hda_controller);
    return true;
}

volatile uint8_t *pcie_map(uint64_t phys, uint64_t size)
{
    return driver_map_mmio(NULL, phys, size, true);
}

void pcie_log(const char *text)
{
    console_write(text);
    console_write("\n");
    if (pcie_event_tail - pcie_event_head < PCIE_BOOT_EVENTS) {
        snprintf(pcie_boot_events[pcie_event_tail % PCIE_BOOT_EVENTS],
            BOOK8_TELEMETRY_LOG_SIZE, "%s", text);
        ++pcie_event_tail;
    } else {
        if (pcie_event_dropped == 0u)
            console_write("PCIE1 ERROR boot telemetry queue full; events lost\n");
        ++pcie_event_dropped;
    }
}

bool pcie_delay_ms(uint32_t milliseconds)
{
    if (tsc_hz == 0u) {
        console_write("PCIE1 ERROR D0 delay requires a calibrated clock\n");
        return false;
    }
    uint64_t start = read_tsc();
    uint64_t ticks = tsc_hz / 1000u * milliseconds;
    while (read_tsc() - start < ticks) {
        __asm__ volatile ("pause");
    }
    return true;
}

void pcie_bootstrap(void)
{
    const struct book8_pcie_platform platform = {
        .legacy_read = pci_legacy_read32,
        .legacy_write = pci_legacy_write32,
        .legacy_write16 = pci_legacy_write16,
        .map = pcie_map, .log = pcie_log, .delay_ms = pcie_delay_ms,
        .read32_safe = book8_read32_safe, .write32_safe = book8_write32_safe,
        .irq_save = irq_save, .irq_restore = irq_restore
    };
    interrupt_table_initialize();
    book8_pcie_init(&pcie, &platform);
    pcie_log("PCIE1 INIT after core CPU setup; early events buffered until UDP is ready");
    pcie_log("PCIE1 PMC SKIP unverified MSR c0012121; no PMC prerequisite");
}

bool nv_maxwell_bus_probe(struct book8_bus_device *device)
{
    struct book8_pcie_node *node = device->private_data;
    uint32_t command;

    if (node == NULL || node->segment != 0u
        || device->mapped_mmio == NULL)
        return false;
    command = book8_pcie_read32(&pcie, node->segment, node->bus,
        node->slot, node->function, 4u);
    return command != UINT32_MAX && (command & 6u) == 6u;
}

void nv_maxwell_bus_remove(struct book8_bus_device *device)
{
    if (device->bus == nv_gpu.bus && device->device == nv_gpu.slot
        && device->function == nv_gpu.function) {
        const struct book8_pcie_node *node = device->private_data;
        if (node != NULL) {
            struct b8_driver_device driver_device = {
                .vendor_id = device->vendor_id,
                .device_id = device->device_id,
                .bus = device->bus,
                .device = device->device,
                .function = device->function,
                .programming_interface = (uint8_t)device->class_code,
                .class_code = device->class_code,
                .bar0_base = node->bars[0].base,
                .bar1_base = node->bars[1].base,
            };
            b8_driver_remove(&nv_driver, &driver_device, &nv_gpu);
        }
        nv_gpu = (struct nv_gpu){0};
    }
}

bool usb_hid_bus_probe(struct book8_bus_device *device)
{
    return (device->class_code >> 16u) == 3u;
}

bool usb_storage_bus_probe(struct book8_bus_device *device)
{
    return (device->class_code >> 16u) == 8u;
}

bool usb_hub_bus_probe(struct book8_bus_device *device)
{
    return (device->class_code >> 16u) == 9u;
}

bool usb_host_bus_probe(struct book8_bus_device *device)
{
    uint8_t programming_interface = (uint8_t)device->class_code;
    return (device->class_code & 0xffff00u) == 0x0c0300u
        && (programming_interface == 0x00u
            || programming_interface == 0x10u
            || programming_interface == 0x20u);
}

void disable_bus_master(struct book8_bus_device *device)
{
    uint32_t command;

    if (device->bus_type != BOOK8_BUS_PCI || device->segment != 0u) {
        return;
    }
    command = book8_pcie_read32(&pcie, device->segment, device->bus,
        device->device, device->function, 4u);
    if (command != UINT32_MAX) {
        book8_pcie_write16(&pcie, device->segment, device->bus,
            device->device, device->function, 4u,
            (uint16_t)(command & ~(1u << 2u)));
    }
}

void rtl8168_report_status(const char *operation, int32_t status)
{
    struct b8_rtl8168_status_text text = {
        .status = (uint32_t)(status < 0 ? -status : status)
    };
    console_write("BOOK8-RTL8168: ");
    console_write(operation);
    console_write(": ");
    if (rtl8168_driver.descriptor != NULL
        && rtl8168_driver.descriptor->ioctl != NULL
        && rtl8168_driver.descriptor->ioctl(B8_RTL8168_IOCTL_STATUS_TEXT,
            &text) == 0 && text.text != NULL) {
        console_write(text.text);
    } else {
        print_signed(status);
    }
    console_write("\n");
}

bool rtl8168_bus_probe(struct book8_bus_device *device)
{
    struct book8_pcie_node *node = device->private_data;
    if (node == NULL || node->segment != 0u
        || rtl8168_driver.descriptor == NULL) {
        return false;
    }
    if (!rtl_nic.running) {
        const struct b8_driver_device module_device = {
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .bus = device->bus,
            .device = device->device,
            .function = device->function,
            .class_code = device->class_code,
            .bar0_base = node->bars[0].base,
            .bar1_base = node->bars[1].base,
        };
        int32_t status = b8_driver_probe(&rtl8168_driver, &module_device,
            &rtl_nic);
        if (status != 0) {
            rtl8168_report_status("probe/init failed", status);
            return false;
        }
        status = rtl8168_driver.descriptor->ioctl(
            B8_RTL8168_IOCTL_START, &rtl_dma);
        if (status != 0) {
            rtl8168_report_status("start failed", status);
            b8_driver_remove(&rtl8168_driver, &module_device, &rtl_nic);
            return false;
        }
        console_write("BOOK8-RTL8168: PCI ");
        print_unsigned(rtl_nic.bus);
        console_putc(':');
        print_unsigned(rtl_nic.slot);
        console_putc('.');
        print_unsigned(rtl_nic.function);
        console_write(" vendor/device=10EC:8168\n");
        console_write("BOOK8-RTL8168: MAC=");
        for (uint8_t i = 0u; i < 6u; ++i) {
            if (i != 0u)
                console_putc(':');
            print_hex8(rtl_nic.mac[i]);
        }
        console_write("\nBOOK8-RTL8168: polling TX/RX ready; 8 descriptors per ring\n");
        const struct book8_net_transport transport = {
            .ctx = &rtl_nic, .send = network_send, .receive = network_receive
        };
        enum book8_net_result result = book8_net_init(&kernel_net,
            &transport, rtl_nic.mac);
        network_ready = result == BOOK8_NET_OK;
        console_write("BOOK8-NET: ");
        console_write(network_ready ? "stack ready; IPv4 unconfigured"
            : book8_net_result_string(result));
        console_write("\n");
    }
    if (!rtl_nic.present || !rtl_nic.running
        || rtl_nic.bus != node->bus || rtl_nic.slot != node->slot
        || rtl_nic.function != node->function) {
        return false;
    }
    device->mapped_mmio = rtl_nic.regs;
    device->mmio_base = 0u;
    device->mmio_size = 0u;
    for (uint8_t bar = 0u; bar < 6u; ++bar) {
        if (rtl_nic.bars[bar].mapped == rtl_nic.regs) {
            device->mmio_base = rtl_nic.bars[bar].phys;
            device->mmio_size = rtl_nic.bars[bar].size;
            node->bars[bar].mapped = true;
            node->bars[bar].mapped_size = rtl_nic.bars[bar].size;
            node->bars[bar].cache_policy = BOOK8_PCIE_CACHE_UC;
            break;
        }
    }
    return device->mmio_size != 0u;
}

void rtl8168_bus_remove(struct book8_bus_device *device)
{
    if (rtl_nic.bus != device->bus || rtl_nic.slot != device->device
        || rtl_nic.function != device->function) {
        return;
    }
    struct b8_driver_device module_device = {
        .vendor_id = device->vendor_id,
        .device_id = device->device_id,
        .bus = device->bus,
        .device = device->device,
        .function = device->function,
    };
    b8_driver_remove(&rtl8168_driver, &module_device, &rtl_nic);
    network_ready = false;
    kernel_net = (struct book8_net){0};
}

bool hda_bus_probe(struct book8_bus_device *device)
{
    struct book8_pcie_node *node = device->private_data;
    if (node == NULL || node->segment != 0u || hda_api == NULL
        || hda_driver.descriptor == NULL) {
        return false;
    }
    if (!hda_controller.present) {
        const struct b8_driver_device module_device = {
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .bus = device->bus,
            .device = device->device,
            .function = device->function,
            .class_code = device->class_code,
            .bar0_base = node->bars[0].base,
        };
        int32_t status = b8_driver_probe(&hda_driver, &module_device, NULL);
        if (status != 0) {
            serial_write("BOOK8-HDA: probe failed: ");
            serial_write(hda_api->status_name(-status));
            serial_write("\r\n");
            return false;
        }
        hda_api->get_device(&hda_controller);
    }
    if (hda_controller.bus != node->bus
        || hda_controller.slot != node->slot
        || hda_controller.function != node->function
        || hda_controller.regs == NULL) {
        return false;
    }
    device->mapped_mmio = hda_controller.regs;
    device->mmio_base = hda_controller.mmio_base;
    device->mmio_size = 0x4000u;
    node->bars[0].mapped = true;
    node->bars[0].mapped_size = device->mmio_size;
    node->bars[0].cache_policy = BOOK8_PCIE_CACHE_UC;
    return true;
}

void hda_bus_remove(struct book8_bus_device *device)
{
    if (hda_controller.bus != device->bus
        || hda_controller.slot != device->device
        || hda_controller.function != device->function) {
        return;
    }
    disable_bus_master(device);
    if (hda_api != NULL && hda_driver.descriptor != NULL) {
        const struct b8_driver_device module_device = {
            .vendor_id = device->vendor_id,
            .device_id = device->device_id,
            .bus = device->bus,
            .device = device->device,
            .function = device->function,
            .class_code = device->class_code,
        };
        b8_driver_remove(&hda_driver, &module_device, NULL);
    }
    hda_controller = (struct hda_device){0};
}

bool nvidia_hda_bus_probe(struct book8_bus_device *device)
{
    return device->bus_type == BOOK8_BUS_PCI
        && device->vendor_id == NV_VENDOR_ID
        && device->device_id == 0x0fbbu
        && (device->class_code & 0xffff00u) == 0x040300u;
}

void nv_gpu_init(void)
{
    nv_graphics = NULL;
    nv_gpu = (struct nv_gpu){0};
    (void)book8_pcie_bind_bus_drivers(&pcie, bus_drivers,
        sizeof(bus_drivers) / sizeof(bus_drivers[0]));
    int32_t status = load_trusted_driver("A:\\DRIVERS\\NVMAXWEL.DRV",
        &nv_driver);
    if (status != 0) {
        console_write("NVDRV: trusted module unavailable; status=");
        print_signed(status);
        console_putc('\n');
        char status_text[64];
        (void)snprintf(status_text,
            sizeof(status_text),
            "NVDRV: trusted module unavailable; status=%ld",
            (long)status);
        serial_write(status_text);
        serial_write("\r\n");
        return;
    }
    if (nv_driver.descriptor->graphics != NULL) {
        nv_graphics = nv_driver.descriptor->graphics();
    }
    pcie_log("NVDRV: module digest verified, symbols resolved and init complete");
    serial_write("NVDRV: module digest verified, symbols resolved and init complete\r\n");
    bool matched_device = false;
    for (size_t i = 0u; i < pcie.node_count; ++i) {
        const struct book8_pcie_node *node = &pcie.nodes[i];
        const b8_driver_t *descriptor = nv_driver.descriptor;
        const struct book8_bus_device *bus_device = &node->bus_device;
        if (node->segment != 0u || bus_device->bus_type != BOOK8_BUS_PCI
            || (descriptor->vendor_id != 0u
                && bus_device->vendor_id != descriptor->vendor_id)
            || (descriptor->product_id != 0u
                && bus_device->device_id != descriptor->product_id)
            || (descriptor->pci_class != 0u
                && (uint8_t)(bus_device->class_code >> 16u)
                    != descriptor->pci_class)
            || (bus_device->driver != &bus_drivers[0]))
            continue;
        if (node->id == BOOK8_PCIE_TARGET_ID
            && (!pcie.target_ready || node->bus != 1u || node->slot != 0u
                || node->function != 0u))
            continue;
        matched_device = true;
        if (node->id == BOOK8_PCIE_TARGET_ID
            && (node->bars[0].base != NV_GM204_BAR0_PHYS
                || node->bars[1].base != NV_GM204_BAR1_PHYS)) {
            pcie_log("PCIE1 ERROR GM204 verified BAR0/BAR1 base mismatch; GPU MMIO blocked");
            continue;
        }
        struct b8_driver_device device = {
            .vendor_id = bus_device->vendor_id,
            .device_id = bus_device->device_id,
            .bus = bus_device->bus,
            .device = bus_device->device,
            .function = bus_device->function,
            .programming_interface = (uint8_t)bus_device->class_code,
            .class_code = bus_device->class_code,
            .bar0_base = node->bars[0].base,
            .bar1_base = node->bars[1].base,
        };
        status = b8_driver_probe(&nv_driver, &device, &nv_gpu);
        if (status == 0) {
            pcie_log("NVDRV: supported PCI device initialized");
            break;
        }
        console_write("NVDRV: matching device probe failed; status=");
        print_signed(status);
        console_putc('\n');
    }
    if (!matched_device) {
        pcie_log("NVDRV: module loaded and initialized; no matching GPU found");
        serial_write("NVDRV: module loaded and initialized; no matching GPU found\r\n");
    }
    if (!nv_gpu.present && pcie.target_present && !pcie.target_ready) {
        for (size_t i = 0u; i < pcie.node_count; ++i) {
            const struct book8_pcie_node *node = &pcie.nodes[i];

            if (node->id != BOOK8_PCIE_TARGET_ID || node->segment != 0u
                || node->bus != 1u || node->slot != 0u
                || node->function != 0u)
                continue;
            nv_gpu = (struct nv_gpu){.present = true,
                .device_id = NV_DEVICE_GTX970, .bus = node->bus,
                .slot = node->slot, .function = node->function,
                .bar0_phys = node->bars[0].base,
                .bar1_phys = node->bars[1].base};
            return;
        }
    }
}

void record_nv_gpu_bars(void)
{
    if (!nv_gpu.present) {
        return;
    }
    for (size_t i = 0u; i < pcie.node_count; ++i) {
        struct book8_pcie_node *node = &pcie.nodes[i];
        if (node->segment != 0u || node->bus != nv_gpu.bus
            || node->slot != nv_gpu.slot
            || node->function != nv_gpu.function) {
            continue;
        }
        if (nv_gpu.bar0 != NULL) {
            node->bars[0].mapped = true;
            node->bars[0].mapped_size = nv_gpu.bar0_size;
            node->bars[0].cache_policy = BOOK8_PCIE_CACHE_UC;
            node->bus_device.mapped_mmio = nv_gpu.bar0;
            node->bus_device.mmio_base = nv_gpu.bar0_phys;
            node->bus_device.mmio_size = nv_gpu.bar0_size;
        }
        if (nv_gpu.bar1 != NULL) {
            node->bars[1].mapped = true;
            node->bars[1].mapped_size = nv_gpu.bar1_size;
            node->bars[1].cache_policy = BOOK8_PCIE_CACHE_WB;
        }
        return;
    }
}

void nv_driver_refresh_state(void)
{
    if (nv_driver.descriptor != NULL
        && nv_driver.descriptor->refresh != NULL)
        nv_driver.descriptor->refresh(&nv_gpu);
}

bool nv_registry_sync(void)
{
    static const char path[] = "HKLM\\HARDWARE\\DEVICEMAP\\VIDEO\\NVIDIA0";

    if (!nv_gpu.present || !nv_gpu.mapped) {
        return true;
    }
    nv_driver_refresh_state();
    return reg_set_str(path, "Arch", "GM204")
        && reg_set_u32(path, "ChipRev", nv_gpu.chip_rev)
        && reg_set_u32(path, "PMC_BOOT_0", nv_gpu.boot0)
        && reg_set_u32(path, "PMC_ENABLE", nv_gpu.pmc_enable)
        && reg_set_u32(path, "DetectedVRAM_MB", nv_gpu.detected_vram_mb)
        && reg_set_u32(path, "ScanoutBase",
            nv_gpu.scanout.valid ? (uint32_t)nv_gpu.scanout.physical_base : 0u)
        && reg_set_u32(path, "BAR0_UC", (uint32_t)nv_gpu.bar0_phys)
        && reg_set_u32(path, "BAR1_WB", (uint32_t)nv_gpu.bar1_phys);
}
