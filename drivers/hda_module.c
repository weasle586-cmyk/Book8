#include "hda_module.h"
#include "../include/b8_shlib.h"
#include "../include/syscall.h"

extern uint32_t b8_hal_pci_read32(uint8_t, uint8_t, uint8_t, uint8_t);
extern void b8_hal_pci_write16(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
extern volatile uint8_t *b8_hal_map_mmio(uint64_t, uint64_t, uint32_t);
extern int32_t b8_hal_dma_address(const void *, size_t, uint64_t *);
extern void b8_hal_relax(void);
extern void b8_hal_log(const char *);

struct hda_import_table {
    b8_sym_table_header_t header;
    b8_sym_import_t imports[6];
};

__attribute__((used, section(B8_SHLIB_IMPORT_SECTION)))
static const struct hda_import_table hda_imports = {
    .header = {
        .magic = {'B', '8', 'S', '1'},
        .abi_version = B8_SHLIB_ABI_VERSION,
        .count = 6u,
    },
    .imports = {
        {"b8_hal_pci_read32", 1u, 0u},
        {"b8_hal_pci_write16", 1u, 0u},
        {"b8_hal_map_mmio", 1u, 0u},
        {"b8_hal_dma_address", 1u, 0u},
        {"b8_hal_relax", 1u, 0u},
        {"b8_hal_log", 1u, 0u},
    },
};

static struct hda_device hda_controller;
static struct hda_dma hda_dma;
static struct hda_stream hda_playback;

static uint32_t module_pci_read32(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset)
{
    (void)ctx;
    return b8_hal_pci_read32(bus, slot, function, offset);
}

static void module_pci_write16(void *ctx, uint8_t bus, uint8_t slot,
    uint8_t function, uint8_t offset, uint16_t value)
{
    (void)ctx;
    b8_hal_pci_write16(bus, slot, function, offset, value);
}

static volatile uint8_t *module_map_mmio(void *ctx, uint64_t physical,
    uint64_t size, bool uncached)
{
    (void)ctx;
    return b8_hal_map_mmio(physical, size, uncached ? 1u : 0u);
}

static bool module_dma_address(void *ctx, const void *pointer, size_t size,
    uint64_t *physical)
{
    (void)ctx;
    return b8_hal_dma_address(pointer, size, physical) != 0;
}

static void module_relax(void *ctx)
{
    (void)ctx;
    b8_hal_relax();
}

static void module_log(void *ctx, const char *text)
{
    (void)ctx;
    b8_hal_log(text);
}

static const struct hda_platform hda_platform = {
    .pci_read32 = module_pci_read32,
    .pci_write16 = module_pci_write16,
    .map_mmio = module_map_mmio,
    .dma_address = module_dma_address,
    .relax = module_relax,
    .log = module_log,
};

static int32_t hda_module_init(void)
{
    hda_controller = (struct hda_device){0};
    hda_dma = (struct hda_dma){0};
    hda_playback = (struct hda_stream){0};
    return 0;
}

static int32_t hda_module_shutdown(void)
{
    hda_stream_stop(&hda_controller, &hda_playback);
    return 0;
}

static int32_t hda_module_probe(const struct b8_driver_device *device,
    void *state)
{
    (void)state;
    if (device == NULL || device->vendor_id != HDA_ATI_VENDOR_ID
        || device->device_id != HDA_ATI_DEVICE_ID
        || (device->class_code & 0xffff00u) != 0x040300u)
        return -BOOK8_ENODEV;
    enum hda_status status = hda_init(&hda_platform, &hda_controller,
        &hda_dma, device->bus, device->device, device->function);
    return status == HDA_OK ? 0 : -(int32_t)status;
}

static void hda_module_remove(const struct b8_driver_device *device,
    void *state)
{
    (void)state;
    if (device == NULL || hda_controller.bus != device->bus
        || hda_controller.slot != device->device
        || hda_controller.function != device->function)
        return;
    hda_stream_stop(&hda_controller, &hda_playback);
    hda_controller = (struct hda_device){0};
    hda_playback = (struct hda_stream){0};
}

static void hda_api_get_device(struct hda_device *device)
{
    if (device != NULL)
        *device = hda_controller;
}

static bool hda_api_is_available(void)
{
    return hda_is_available(&hda_controller);
}

static uint8_t hda_api_get_volume(void)
{
    return hda_get_volume(&hda_controller);
}

static bool hda_api_get_mute(void)
{
    return hda_get_mute(&hda_controller);
}

static int32_t hda_api_set_volume(uint8_t percent)
{
    return (int32_t)hda_set_volume(&hda_platform, &hda_controller, percent);
}

static int32_t hda_api_set_mute(bool muted)
{
    return (int32_t)hda_set_mute(&hda_platform, &hda_controller, muted);
}

static int32_t hda_api_play_wav(const uint8_t *wav, uint32_t size)
{
    return (int32_t)hda_stream_play_wav(&hda_platform, &hda_controller,
        &hda_playback, wav, size);
}

static int32_t hda_api_pause(bool paused)
{
    return (int32_t)hda_stream_pause(&hda_platform, &hda_controller,
        &hda_playback, paused);
}

static void hda_api_stop(void)
{
    hda_stream_stop(&hda_controller, &hda_playback);
}

static int32_t hda_api_seek(uint32_t position_ms)
{
    bool was_paused = hda_playback.paused;
    enum hda_stream_result result = hda_stream_seek(&hda_platform,
        &hda_controller, &hda_playback, position_ms);
    if (result == HDA_STREAM_OK && was_paused && hda_playback.active)
        result = hda_stream_pause(&hda_platform, &hda_controller,
            &hda_playback, true);
    return (int32_t)result;
}

static void hda_api_get_stream_info(struct hda_stream_info *info)
{
    if (info != NULL)
        hda_stream_get_info(&hda_playback, info);
}

static int32_t hda_api_service(void)
{
    return (int32_t)hda_stream_service(&hda_platform, &hda_controller,
        &hda_playback);
}

static const char *hda_api_status_name(int32_t status)
{
    return hda_status_name((enum hda_status)status);
}

static const char *hda_api_stream_result_name(int32_t result)
{
    return hda_stream_result_name((enum hda_stream_result)result);
}

static const struct b8_hda_module_api hda_module_api = {
    .abi_version = B8_HDA_MODULE_API_VERSION,
    .struct_size = sizeof(struct b8_hda_module_api),
    .get_device = hda_api_get_device,
    .is_available = hda_api_is_available,
    .get_volume = hda_api_get_volume,
    .get_mute = hda_api_get_mute,
    .set_volume = hda_api_set_volume,
    .set_mute = hda_api_set_mute,
    .play_wav = hda_api_play_wav,
    .pause = hda_api_pause,
    .stop = hda_api_stop,
    .seek = hda_api_seek,
    .get_stream_info = hda_api_get_stream_info,
    .service = hda_api_service,
    .status_name = hda_api_status_name,
    .stream_result_name = hda_api_stream_result_name,
};

static int32_t hda_module_ioctl(uint32_t command, void *argument)
{
    if (command != B8_HDA_IOCTL_GET_API || argument == NULL)
        return -BOOK8_EINVAL;
    *(const struct b8_hda_module_api **)argument = &hda_module_api;
    return 0;
}

static const b8_driver_t hda_module_descriptor = {
    .abi_version = B8_DRIVER_ABI_VERSION,
    .struct_size = sizeof(b8_driver_t),
    .version_major = 1u,
    .vendor_id = HDA_ATI_VENDOR_ID,
    .product_id = HDA_ATI_DEVICE_ID,
    .pci_class = 0x04u,
    .pci_subclass = 0x03u,
    .init = hda_module_init,
    .shutdown = hda_module_shutdown,
    .probe = hda_module_probe,
    .remove = hda_module_remove,
    .ioctl = hda_module_ioctl,
};

__attribute__((visibility("default")))
const b8_driver_t *b8_driver_entry(void)
{
    return &hda_module_descriptor;
}
