#include "rtl8168_module.h"
#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"
#include "../include/syscall.h"

extern uint32_t b8_hal_pci_read32(uint8_t, uint8_t, uint8_t, uint8_t);
extern void b8_hal_pci_write32(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
extern void b8_hal_pci_write16(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
extern volatile uint8_t *b8_hal_map_mmio(uint64_t, uint64_t, uint32_t);
extern void b8_hal_relax(void);
extern int32_t b8_hal_dma_address(const void *, size_t, uint64_t *);

struct rtl8168_import_table {
    b8_sym_table_header_t header;
    b8_sym_import_t imports[6];
};

__attribute__((used, section(B8_SHLIB_IMPORT_SECTION)))
static const struct rtl8168_import_table rtl8168_imports = {
    .header = {
        .magic = {'B', '8', 'S', '1'},
        .abi_version = B8_SHLIB_ABI_VERSION,
        .count = 6u,
    },
    .imports = {
        {"b8_hal_pci_read32", 1u, 0u},
        {"b8_hal_pci_write32", 1u, 0u},
        {"b8_hal_pci_write16", 1u, 0u},
        {"b8_hal_map_mmio", 1u, 0u},
        {"b8_hal_relax", 1u, 0u},
        {"b8_hal_dma_address", 1u, 0u},
    },
};

static struct rtl8168_platform rtl8168_platform;
static struct rtl8168_device *rtl8168_device;

static uint32_t module_pci_read32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset)
{
    (void)ctx;
    return b8_hal_pci_read32(bus, slot, function, offset);
}

static void module_pci_write32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint32_t value)
{
    (void)ctx;
    b8_hal_pci_write32(bus, slot, function, offset, value);
}

static volatile uint8_t *module_map_mmio(void *ctx, uint64_t physical,
    uint64_t size, bool uncached)
{
    (void)ctx;
    return b8_hal_map_mmio(physical, size, uncached ? 1u : 0u);
}

static void module_relax(void *ctx)
{
    (void)ctx;
    b8_hal_relax();
}

static bool module_dma_address(void *ctx, const void *address, size_t size,
    uint64_t *physical)
{
    (void)ctx;
    return b8_hal_dma_address(address, size, physical) != 0;
}

static int32_t rtl8168_module_init(void)
{
    rtl8168_platform = (struct rtl8168_platform){
        .pci_read32 = module_pci_read32,
        .pci_write32 = module_pci_write32,
        .map_mmio = module_map_mmio,
        .relax = module_relax,
        .dma_address = module_dma_address,
    };
    return 0;
}

static int32_t rtl8168_module_probe(const struct b8_driver_device *device,
    void *state)
{
    if (device == NULL || state == NULL || device->vendor_id != RTL8168_VENDOR_ID
        || device->device_id != RTL8168_DEVICE_ID
        || ((device->class_code >> 16u) & 0xffu) != 0x02u)
        return -BOOK8_ENODEV;
    rtl8168_device = state;
    enum rtl8168_status status = rtl8168_probe_device(&rtl8168_platform,
        rtl8168_device, device->bus, device->device, device->function);
    if (status == RTL8168_OK)
        status = rtl8168_init(&rtl8168_platform, rtl8168_device);
    return status == RTL8168_OK ? 0 : -(int32_t)status;
}

static void rtl8168_module_remove(const struct b8_driver_device *device,
    void *state)
{
    struct rtl8168_device *nic = state;
    if (device == NULL || nic == NULL || !nic->present
        || nic->bus != device->bus || nic->slot != device->device
        || nic->function != device->function)
        return;
    if (nic->regs != NULL)
        nic->regs[RTL8168_CR] = 0u;
    uint32_t command = b8_hal_pci_read32(nic->bus, nic->slot,
        nic->function, 4u);
    if (command != UINT32_MAX)
        b8_hal_pci_write16(nic->bus, nic->slot, nic->function, 4u,
            (uint16_t)(command & 0xffffu & ~(1u << 2u)));
    *nic = (struct rtl8168_device){0};
    rtl8168_device = NULL;
}

static int32_t rtl8168_module_read(uint64_t offset, void *buffer, size_t size)
{
    (void)offset;
    if (rtl8168_device == NULL || buffer == NULL || size > INT32_MAX)
        return -BOOK8_EINVAL;
    size_t received = 0u;
    enum rtl8168_status status = rtl8168_receive(rtl8168_device, buffer,
        size, &received);
    if (status == RTL8168_OK)
        return (int32_t)received;
    if (status == RTL8168_RX_EMPTY)
        return 0;
    return -(int32_t)status;
}

static int32_t rtl8168_module_write(uint64_t offset, const void *buffer,
    size_t size)
{
    (void)offset;
    if (rtl8168_device == NULL)
        return -BOOK8_ENODEV;
    enum rtl8168_status status = rtl8168_transmit(&rtl8168_platform,
        rtl8168_device, buffer, size);
    return status == RTL8168_OK ? 0 : -(int32_t)status;
}

static int32_t rtl8168_module_ioctl(uint32_t command, void *argument)
{
    if (rtl8168_device == NULL)
        return -BOOK8_ENODEV;
    switch (command) {
    case B8_RTL8168_IOCTL_START:
        if (argument == NULL)
            return -BOOK8_EINVAL;
        return -(int32_t)rtl8168_start(&rtl8168_platform, rtl8168_device,
            argument);
    case B8_RTL8168_IOCTL_PHY_STATUS:
        if (argument == NULL)
            return -BOOK8_EINVAL;
        *(uint8_t *)argument = rtl8168_phy_status(rtl8168_device);
        return 0;
    case B8_RTL8168_IOCTL_RING_STATE:
        if (argument == NULL)
            return -BOOK8_EINVAL;
        rtl8168_ring_state(rtl8168_device,
            &((struct b8_rtl8168_ring_state *)argument)->tx_pending,
            &((struct b8_rtl8168_ring_state *)argument)->rx_ready);
        return 0;
    case B8_RTL8168_IOCTL_STATUS_TEXT:
        if (argument == NULL)
            return -BOOK8_EINVAL;
        ((struct b8_rtl8168_status_text *)argument)->text =
            rtl8168_status_string((enum rtl8168_status)
                ((struct b8_rtl8168_status_text *)argument)->status);
        return 0;
    default:
        return -BOOK8_ENOTSUP;
    }
}

static const b8_driver_t rtl8168_module_descriptor = {
    .abi_version = B8_DRIVER_ABI_VERSION,
    .struct_size = sizeof(b8_driver_t),
    .version_major = 1u,
    .vendor_id = RTL8168_VENDOR_ID,
    .product_id = RTL8168_DEVICE_ID,
    .pci_class = 0x02u,
    .init = rtl8168_module_init,
    .probe = rtl8168_module_probe,
    .remove = rtl8168_module_remove,
    .read = rtl8168_module_read,
    .write = rtl8168_module_write,
    .ioctl = rtl8168_module_ioctl,
};

__attribute__((visibility("default")))
const b8_driver_t *b8_driver_entry(void)
{
    return &rtl8168_module_descriptor;
}
