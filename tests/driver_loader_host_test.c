#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"
#include "../drivers/hda_module.h"
#include "../drivers/rtl8168_module.h"
#include "../fs/fs.h"
#include "../kernel/driver_loader.h"
#include "../kernel/vmm.h"
#include "../net/sha256.h"
#include "../src/syscall.h"
#include "../src/usb.h"

static FILE *module_file;
static file_handle_t module_handle;

file_handle_t *fs_open_any(const char *path)
{
    (void)path;
    module_file = fopen(getenv("BOOK8_TEST_DRIVER"), "rb");
    if (module_file == NULL)
        return NULL;
    assert(fseek(module_file, 0, SEEK_END) == 0);
    long size = ftell(module_file);
    assert(size > 0 && size <= UINT32_MAX);
    rewind(module_file);
    module_handle = (file_handle_t){.used = true, .size = (uint32_t)size};
    return &module_handle;
}

int32_t fs_read(file_handle_t *file, void *buffer, uint32_t count)
{
    assert(file == &module_handle && file->used);
    size_t read = fread(buffer, 1u, count, module_file);
    if (read == 0u && ferror(module_file))
        return -1;
    return (int32_t)read;
}

bool fs_seek(file_handle_t *file, uint32_t offset)
{
    assert(file == &module_handle && file->used);
    return fseek(module_file, (long)offset, SEEK_SET) == 0;
}

void fs_close(file_handle_t *file)
{
    assert(file == &module_handle && file->used);
    fclose(module_file);
    module_file = NULL;
    file->used = false;
}

void *vmm_alloc_pages(uint32_t count)
{
    void *memory = mmap(NULL, (size_t)count * 4096u,
        PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
}

void *vmm_alloc_contiguous_pages(uint32_t count)
{
    return vmm_alloc_pages(count);
}

void vmm_free_pages(void *memory, uint32_t count)
{
    if (memory != NULL)
        assert(munmap(memory, (size_t)count * 4096u) == 0);
}

static uint32_t hal_pci_read32(uint8_t bus, uint8_t device,
    uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0xffffffffu;
}

static void hal_pci_write32(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint32_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    (void)value;
}

static void hal_pci_write16(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint16_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    (void)value;
}

static volatile uint8_t *hal_map_mmio(uint64_t physical, uint64_t size,
    uint32_t uncached)
{
    (void)physical;
    (void)size;
    (void)uncached;
    return NULL;
}

static void hal_unmap_mmio(const volatile void *pointer, uint64_t size)
{
    (void)pointer;
    (void)size;
}

static void hal_relax(void)
{
}

static int32_t hal_dma_address(const void *address, size_t size,
    uint64_t *physical)
{
    (void)address;
    (void)size;
    (void)physical;
    return -1;
}

static void hal_log(const char *text)
{
    assert(text != NULL);
}

static void hal_serial_putc(char value)
{
    (void)value;
}

static bool hal_reg_delete_tree(const char *path)
{
    (void)path;
    return true;
}

static bool hal_reg_set_str(const char *path, const char *name,
    const char *value)
{
    (void)path;
    (void)name;
    (void)value;
    return true;
}

static bool hal_reg_set_u32(const char *path, const char *name,
    uint32_t value)
{
    (void)path;
    (void)name;
    (void)value;
    return true;
}

static int hal_snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

static int32_t hal_framebuffer_info(struct b8_framebuffer_info *info)
{
    (void)info;
    return -1;
}

static const b8_sym_export_t hal_exports[] = {
    {.name = "b8_hal_pci_read32", .ptr = (void *)(uintptr_t)hal_pci_read32,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_pci_write32", .ptr = (void *)(uintptr_t)hal_pci_write32,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_pci_write16", .ptr = (void *)(uintptr_t)hal_pci_write16,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_map_mmio", .ptr = (void *)(uintptr_t)hal_map_mmio,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_unmap_mmio", .ptr = (void *)(uintptr_t)hal_unmap_mmio,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_relax", .ptr = (void *)(uintptr_t)hal_relax,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_dma_address", .ptr = (void *)(uintptr_t)hal_dma_address,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_serial_putc", .ptr = (void *)(uintptr_t)hal_serial_putc,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_reg_delete_tree",
        .ptr = (void *)(uintptr_t)hal_reg_delete_tree,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_reg_set_str",
        .ptr = (void *)(uintptr_t)hal_reg_set_str,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_reg_set_u32",
        .ptr = (void *)(uintptr_t)hal_reg_set_u32,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_snprintf", .ptr = (void *)(uintptr_t)hal_snprintf,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_log", .ptr = (void *)(uintptr_t)hal_log,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
    {.name = "b8_hal_framebuffer_info",
        .ptr = (void *)(uintptr_t)hal_framebuffer_info,
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 0u},
};

int main(void)
{
    const char *path = getenv("BOOK8_TEST_DRIVER");
    const char *on_disk_path = getenv("BOOK8_TEST_DRIVER_PATH");
    const char *kind = getenv("BOOK8_TEST_DRIVER_KIND");
    assert(path != NULL);
    assert(on_disk_path != NULL);
    assert(kind != NULL);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0);
    rewind(file);
    uint8_t *bytes = malloc((size_t)size);
    assert(bytes != NULL);
    assert(fread(bytes, 1u, (size_t)size, file) == (size_t)size);
    fclose(file);
    uint8_t digest[BOOK8_SHA256_BYTES];
    book8_sha256(bytes, (size_t)size, digest);
    free(bytes);

    struct b8_loaded_driver loaded;
    uint8_t invalid_digest[BOOK8_SHA256_BYTES];
    memcpy(invalid_digest, digest, sizeof(digest));
    invalid_digest[0] ^= 0x80u;
    assert(b8_driver_load(on_disk_path, invalid_digest,
        hal_exports, sizeof(hal_exports) / sizeof(hal_exports[0]),
        &loaded) == -BOOK8_EPERM);
    assert(loaded.descriptor == NULL && loaded.image == NULL);

    assert(b8_driver_load(on_disk_path, digest,
        hal_exports, sizeof(hal_exports) / sizeof(hal_exports[0]),
        &loaded) == 0);
    assert(loaded.initialized && loaded.descriptor != NULL);
    if (strcmp(kind, "nvidia") == 0) {
        assert(loaded.descriptor->vendor_id == 0x10deu);
        const struct b8_graphics_ops *graphics = loaded.descriptor->graphics();
        assert(graphics != NULL && graphics->abi_version == 1u);
        assert(graphics->fill_rect != NULL && graphics->copy_rect != NULL);
        struct b8_driver_device absent = {.vendor_id = 0xffffu};
        assert(b8_driver_probe(&loaded, &absent, NULL) == -BOOK8_ENODEV);
    } else if (strcmp(kind, "rtl8168") == 0) {
        assert(loaded.descriptor->vendor_id == RTL8168_VENDOR_ID);
        assert(loaded.descriptor->product_id == RTL8168_DEVICE_ID);
        struct b8_driver_device absent = {.vendor_id = 0xffffu};
        assert(b8_driver_probe(&loaded, &absent, NULL) == -BOOK8_ENODEV);
    } else if (strcmp(kind, "usb") == 0) {
        assert(loaded.descriptor->ioctl != NULL);
        const struct book8_usb_module_api *api = NULL;
        assert(loaded.descriptor->ioctl(BOOK8_USB_IOCTL_GET_API, &api) == 0);
        assert(api != NULL
            && api->abi_version == BOOK8_USB_MODULE_API_VERSION
            && api->struct_size >= sizeof(*api));
        assert(api->initialize != NULL && api->initialize_from_pci != NULL
            && api->poll_mouse != NULL && api->poll_keyboard != NULL);
    } else if (strcmp(kind, "hda") == 0) {
        assert(loaded.descriptor->vendor_id == HDA_ATI_VENDOR_ID);
        assert(loaded.descriptor->product_id == HDA_ATI_DEVICE_ID);
        assert(loaded.descriptor->ioctl != NULL);
        const struct b8_hda_module_api *api = NULL;
        assert(loaded.descriptor->ioctl(B8_HDA_IOCTL_GET_API, &api) == 0);
        assert(api != NULL
            && api->abi_version == B8_HDA_MODULE_API_VERSION
            && api->struct_size >= sizeof(*api));
        assert(api->get_device != NULL && api->is_available != NULL
            && api->get_volume != NULL && api->get_mute != NULL
            && api->set_volume != NULL && api->set_mute != NULL
            && api->play_wav != NULL && api->pause != NULL
            && api->stop != NULL && api->seek != NULL
            && api->get_stream_info != NULL && api->service != NULL
            && api->status_name != NULL
            && api->stream_result_name != NULL);
        struct b8_driver_device absent = {.vendor_id = 0xffffu};
        assert(b8_driver_probe(&loaded, &absent, NULL) == -BOOK8_ENODEV);
    } else {
        assert(!"unknown module kind");
    }
    b8_driver_unload(&loaded);
    assert(loaded.descriptor == NULL && loaded.image == NULL);

    printf("PASS: %s module load, imports, init and unload\n", kind);
    return 0;
}
