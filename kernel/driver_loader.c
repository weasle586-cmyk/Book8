#include "driver_loader.h"
#include "elf.h"
#include "vmm.h"
#include "../fs/fs.h"
#include "../net/sha256.h"
#include "../src/syscall.h"

#define DRIVER_PAGE 4096ull
#define DRIVER_MAX_SECTIONS 256u
#define DRIVER_MAX_PHDRS 64u
#define DRIVER_MAX_RELOCATIONS 65536u

typedef struct {
    uint8_t *bytes;
    uint32_t size;
} driver_file_t;

static bool add_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (a > UINT64_MAX - b)
        return false;
    *result = a + b;
    return true;
}

static bool range_ok(uint64_t offset, uint64_t length, uint64_t size)
{
    return offset <= size && length <= size - offset;
}

static bool file_read(const driver_file_t *file, uint64_t offset,
    void *destination, uint64_t length)
{
    if (file == 0 || destination == 0
        || !range_ok(offset, length, file->size))
        return false;
    const uint8_t *source = file->bytes + offset;
    uint8_t *output = destination;
    for (uint64_t i = 0u; i < length; ++i)
        output[i] = source[i];
    return true;
}

static bool string_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (*left++ != *right++)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool read_string(const driver_file_t *file, const Elf64_Shdr *strings,
    uint32_t offset, char *output, uint32_t capacity)
{
    if (offset >= strings->sh_size || capacity < 2u)
        return false;
    for (uint32_t i = 0u; i < capacity; ++i) {
        uint8_t character;
        if ((uint64_t)offset + i >= strings->sh_size
            || !file_read(file, strings->sh_offset + offset + i,
                &character, sizeof(character)))
            return false;
        output[i] = (char)character;
        if (character == 0u)
            return true;
    }
    return false;
}

static bool section_header(const driver_file_t *file, const Elf64_Ehdr *header,
    uint32_t index, Elf64_Shdr *section)
{
    uint64_t offset;
    if (index >= header->e_shnum
        || !add_u64(header->e_shoff,
            (uint64_t)index * sizeof(*section), &offset))
        return false;
    return file_read(file, offset, section, sizeof(*section));
}

static bool segment_contains(const Elf64_Phdr *segments, uint32_t count,
    uint64_t address, uint64_t length, uint32_t required_flags)
{
    uint64_t end;
    if (!add_u64(address, length, &end))
        return false;
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t segment_end;
        if (segments[i].p_type != PT_LOAD
            || (segments[i].p_flags & required_flags) != required_flags
            || !add_u64(segments[i].p_vaddr, segments[i].p_memsz,
                &segment_end))
            continue;
        if (address >= segments[i].p_vaddr && end <= segment_end)
            return true;
    }
    return length == 0u;
}

static uint32_t relocation_width(uint32_t type)
{
    switch (type) {
    case R_X86_64_NONE:
        return 0u;
    case R_X86_64_64:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
    case R_X86_64_RELATIVE:
        return 8u;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        return 4u;
    default:
        return UINT32_MAX;
    }
}

static int32_t validate_elf(const driver_file_t *file, Elf64_Ehdr *header,
    Elf64_Phdr *segments, uint64_t *image_low, uint64_t *image_high)
{
    if (file->size < sizeof(*header)
        || !file_read(file, 0u, header, sizeof(*header))
        || header->e_ident[0] != 0x7fu || header->e_ident[1] != 'E'
        || header->e_ident[2] != 'L' || header->e_ident[3] != 'F'
        || header->e_ident[4] != ELFCLASS64
        || header->e_ident[5] != ELFDATA2LSB
        || header->e_ident[6] != EV_CURRENT
        || header->e_type != ET_DYN || header->e_machine != EM_X86_64
        || header->e_version != EV_CURRENT
        || header->e_ehsize != sizeof(*header)
        || header->e_phentsize != sizeof(Elf64_Phdr)
        || header->e_phnum == 0u || header->e_phnum > DRIVER_MAX_PHDRS
        || header->e_shentsize != sizeof(Elf64_Shdr)
        || header->e_shnum == 0u || header->e_shnum > DRIVER_MAX_SECTIONS
        || header->e_shstrndx >= header->e_shnum
        || !range_ok(header->e_phoff,
            (uint64_t)header->e_phnum * sizeof(Elf64_Phdr), file->size)
        || !range_ok(header->e_shoff,
            (uint64_t)header->e_shnum * sizeof(Elf64_Shdr), file->size))
        return -BOOK8_ENOEXEC;
    if (!file_read(file, header->e_phoff, segments,
            (uint64_t)header->e_phnum * sizeof(Elf64_Phdr)))
        return -BOOK8_ENOEXEC;

    *image_low = UINT64_MAX;
    *image_high = 0u;
    for (uint32_t i = 0u; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        uint64_t end, aligned_end;
        if (segment->p_type != PT_LOAD)
            continue;
        if (segment->p_filesz > segment->p_memsz
            || !range_ok(segment->p_offset, segment->p_filesz, file->size)
            || !add_u64(segment->p_vaddr, segment->p_memsz, &end)
            || !add_u64(end, DRIVER_PAGE - 1u, &aligned_end))
            return -BOOK8_ENOEXEC;
        uint64_t aligned_start = segment->p_vaddr & ~(DRIVER_PAGE - 1u);
        aligned_end &= ~(DRIVER_PAGE - 1u);
        if (aligned_start < *image_low)
            *image_low = aligned_start;
        if (aligned_end > *image_high)
            *image_high = aligned_end;
    }
    if (*image_low == UINT64_MAX || *image_high <= *image_low
        || *image_high - *image_low > B8_DRIVER_MAX_IMAGE_SIZE)
        return -BOOK8_ENOEXEC;
    return 0;
}

static int32_t find_imports(const driver_file_t *file,
    const Elf64_Ehdr *header, const Elf64_Shdr *sections,
    uint32_t *imports_count, uint64_t *imports_offset)
{
    Elf64_Shdr names;
    char name[64];
    bool found = false;
    if (!section_header(file, header, header->e_shstrndx, &names)
        || names.sh_type != SHT_STRTAB
        || !range_ok(names.sh_offset, names.sh_size, file->size))
        return -BOOK8_ENOEXEC;
    for (uint32_t i = 0u; i < header->e_shnum; ++i) {
        const Elf64_Shdr *section = &sections[i];
        if (section->sh_name >= names.sh_size
            || !read_string(file, &names, section->sh_name, name,
                sizeof(name)))
            return -BOOK8_ENOEXEC;
        if (!string_equal(name, B8_SHLIB_IMPORT_SECTION))
            continue;
        if (found || section->sh_type != SHT_PROGBITS
            || !range_ok(section->sh_offset, section->sh_size, file->size)
            || section->sh_size < sizeof(b8_sym_table_header_t))
            return -BOOK8_ENOEXEC;
        b8_sym_table_header_t table;
        if (!file_read(file, section->sh_offset, &table, sizeof(table))
            || table.abi_version != B8_SHLIB_ABI_VERSION
            || table.count > B8_SHLIB_MAX_SYMBOLS
            || sizeof(table) + (uint64_t)table.count
                * sizeof(b8_sym_import_t) != section->sh_size) {
            return -BOOK8_ENOEXEC;
        }
        for (uint32_t j = 0u; j < B8_SHLIB_MAGIC_SIZE; ++j) {
            if (table.magic[j] != B8_SHLIB_MAGIC[j])
                return -BOOK8_ENOEXEC;
        }
        *imports_count = table.count;
        *imports_offset = section->sh_offset + sizeof(table);
        found = true;
    }
    return found ? 0 : -BOOK8_ENOEXEC;
}

static int32_t import_lookup(const driver_file_t *file, uint32_t imports_count,
    uint64_t imports_offset, const char *name, b8_sym_import_t *result)
{
    bool found = false;
    for (uint32_t i = 0u; i < imports_count; ++i) {
        b8_sym_import_t candidate;
        if (!file_read(file, imports_offset
                + (uint64_t)i * sizeof(candidate), &candidate,
                sizeof(candidate)))
            return -BOOK8_ENOEXEC;
        if (!string_equal(candidate.name, name))
            continue;
        if (found)
            return -BOOK8_EINVAL;
        *result = candidate;
        found = true;
    }
    return found ? 0 : -BOOK8_ENOENT;
}

static int32_t provider_lookup(const b8_sym_export_t *providers,
    uint32_t provider_count, const b8_sym_import_t *imported,
    uintptr_t *address)
{
    const b8_sym_export_t *match = 0;
    for (uint32_t i = 0u; i < provider_count; ++i) {
        const b8_sym_export_t *candidate = &providers[i];
        if (!string_equal(candidate->name, imported->name))
            continue;
        if (match != 0)
            return -BOOK8_EINVAL;
        match = candidate;
    }
    if (match == 0)
        return -BOOK8_ENOENT;
    if (match->ptr == 0 || match->version_major != imported->version_major
        || match->version_minor < imported->version_minor)
        return -BOOK8_EINVAL;
    *address = (uintptr_t)match->ptr;
    return 0;
}

static bool add_signed(uint64_t base, int64_t addend, uint64_t *result)
{
    if (addend >= 0)
        return add_u64(base, (uint64_t)addend, result);
    uint64_t magnitude = (uint64_t)(-(addend + 1)) + 1u;
    if (base < magnitude)
        return false;
    *result = base - magnitude;
    return true;
}

static int32_t resolve_symbol(const driver_file_t *file,
    const Elf64_Shdr *symbols, const Elf64_Shdr *strings,
    uint32_t symbol_index, uint64_t bias, uint32_t imports_count,
    uint64_t imports_offset, const b8_sym_export_t *providers,
    uint32_t provider_count, uint64_t *result)
{
    Elf64_Sym symbol;
    if (symbol_index >= symbols->sh_size / sizeof(symbol)
        || !file_read(file, symbols->sh_offset
            + (uint64_t)symbol_index * sizeof(symbol), &symbol,
            sizeof(symbol)))
        return -BOOK8_ENOEXEC;
    if (symbol.st_shndx == SHN_ABS) {
        *result = symbol.st_value;
        return 0;
    }
    if (symbol.st_shndx != SHN_UNDEF) {
        if (symbol.st_shndx >= UINT16_MAX
            || symbol.st_value > UINT64_MAX - bias)
            return -BOOK8_ENOEXEC;
        *result = bias + symbol.st_value;
        return 0;
    }
    if ((symbol.st_info >> 4u) != STB_GLOBAL
        && (symbol.st_info >> 4u) != STB_WEAK)
        return -BOOK8_ENOEXEC;
    char name[B8_SYM_NAME_SIZE];
    if (!read_string(file, strings, symbol.st_name, name, sizeof(name)))
        return -BOOK8_ENOEXEC;
    b8_sym_import_t imported = {0};
    int32_t status = import_lookup(file, imports_count, imports_offset, name,
        &imported);
    if (status != 0)
        return status;
    uintptr_t provider;
    status = provider_lookup(providers, provider_count, &imported, &provider);
    if (status != 0)
        return status;
    *result = provider;
    return 0;
}

static bool write_image(void *image, uint64_t image_low,
    uint64_t image_high, uint64_t bias, uint64_t target, uint64_t value,
    uint32_t width, const Elf64_Phdr *segments, uint32_t segment_count)
{
    if (target > UINT64_MAX - bias
        || !segment_contains(segments, segment_count, target, width, PF_W)
        || target < image_low || target > image_high - width)
        return false;
    uint8_t *destination = (uint8_t *)image + (target - image_low);
    for (uint32_t i = 0u; i < width; ++i)
        destination[i] = (uint8_t)(value >> (i * 8u));
    return true;
}

static int32_t apply_relocations(const driver_file_t *file,
    const Elf64_Ehdr *header, const Elf64_Phdr *segments,
    const Elf64_Shdr *sections, uint64_t image_low, uint64_t image_high,
    uint64_t bias, void *image, uint32_t imports_count,
    uint64_t imports_offset, const b8_sym_export_t *providers,
    uint32_t provider_count)
{
    uint32_t relocation_count = 0u;
    for (uint32_t i = 0u; i < header->e_shnum; ++i) {
        const Elf64_Shdr *relocations = &sections[i];
        if (relocations->sh_type != SHT_REL && relocations->sh_type != SHT_RELA
            && relocations->sh_type != SHT_RELR)
            continue;
        if (relocations->sh_size == 0u
            || (relocations->sh_flags & SHF_ALLOC) == 0u)
            continue;
        if (relocations->sh_type != SHT_RELA)
            return -BOOK8_ENOTSUP;
        if (relocations->sh_entsize != sizeof(Elf64_Rela)
            || relocations->sh_size % sizeof(Elf64_Rela) != 0u
            || relocations->sh_link >= header->e_shnum)
            return -BOOK8_ENOEXEC;
        const Elf64_Shdr *symbols = &sections[relocations->sh_link];
        if (symbols->sh_type != SHT_DYNSYM
            || symbols->sh_entsize != sizeof(Elf64_Sym)
            || symbols->sh_size % sizeof(Elf64_Sym) != 0u
            || symbols->sh_link >= header->e_shnum)
            return -BOOK8_ENOEXEC;
        const Elf64_Shdr *strings = &sections[symbols->sh_link];
        if (strings->sh_type != SHT_STRTAB
            || !range_ok(strings->sh_offset, strings->sh_size, file->size))
            return -BOOK8_ENOEXEC;
        uint64_t count = relocations->sh_size / sizeof(Elf64_Rela);
        if (count > DRIVER_MAX_RELOCATIONS - relocation_count)
            return -BOOK8_ENOEXEC;
        relocation_count += (uint32_t)count;
        for (uint64_t j = 0u; j < count; ++j) {
            Elf64_Rela rela;
            if (!file_read(file, relocations->sh_offset
                    + j * sizeof(rela), &rela, sizeof(rela)))
                return -BOOK8_ENOEXEC;
            uint32_t type = ELF64_R_TYPE(rela.r_info);
            uint32_t symbol_index = ELF64_R_SYM(rela.r_info);
            uint32_t width = relocation_width(type);
            if (width == UINT32_MAX)
                return -BOOK8_ENOTSUP;
            if (type == R_X86_64_NONE)
                continue;
            uint64_t result, symbol_value;
            if (type == R_X86_64_RELATIVE) {
                if (symbol_index != 0u || !add_signed(bias, rela.r_addend,
                        &result))
                    return -BOOK8_ENOEXEC;
            } else {
                int32_t status = resolve_symbol(file, symbols, strings,
                    symbol_index, bias, imports_count, imports_offset,
                    providers, provider_count, &symbol_value);
                if (status != 0)
                    return status;
                switch (type) {
                case R_X86_64_64:
                    if (!add_signed(symbol_value, rela.r_addend, &result))
                        return -BOOK8_ENOEXEC;
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32: {
                    uint64_t sum, distance;
                    if (!add_signed(symbol_value, rela.r_addend, &sum))
                        return -BOOK8_ENOEXEC;
                    uint64_t place;
                    if (!add_u64(bias, rela.r_offset, &place))
                        return -BOOK8_ENOEXEC;
                    if (sum >= place) {
                        distance = sum - place;
                        if (distance > INT32_MAX)
                            return -BOOK8_ENOEXEC;
                        result = (uint32_t)distance;
                    } else {
                        distance = place - sum;
                        if (distance > (uint64_t)INT32_MAX + 1u)
                            return -BOOK8_ENOEXEC;
                        result = (uint32_t)(-(int64_t)distance);
                    }
                    break;
                }
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                    result = symbol_value;
                    break;
                default:
                    return -BOOK8_ENOTSUP;
                }
            }
            if (!write_image(image, image_low, image_high, bias,
                    rela.r_offset, result, width, segments, header->e_phnum))
                return -BOOK8_ENOEXEC;
        }
    }
    return 0;
}

static int32_t find_entry(const driver_file_t *file, const Elf64_Ehdr *header,
    const Elf64_Phdr *segments, uint64_t bias, const Elf64_Shdr *sections,
    uintptr_t *entry)
{
    bool found = false;
    for (uint32_t i = 0u; i < header->e_shnum; ++i) {
        const Elf64_Shdr *symbols = &sections[i];
        if (symbols->sh_type != SHT_DYNSYM)
            continue;
        if (symbols->sh_entsize != sizeof(Elf64_Sym)
            || symbols->sh_size % sizeof(Elf64_Sym) != 0u
            || symbols->sh_link >= header->e_shnum)
            return -BOOK8_ENOEXEC;
        const Elf64_Shdr *strings = &sections[symbols->sh_link];
        if (strings->sh_type != SHT_STRTAB
            || !range_ok(strings->sh_offset, strings->sh_size, file->size))
            return -BOOK8_ENOEXEC;
        uint64_t count = symbols->sh_size / sizeof(Elf64_Sym);
        for (uint64_t j = 0u; j < count; ++j) {
            Elf64_Sym symbol;
            char name[B8_SYM_NAME_SIZE];
            if (!file_read(file, symbols->sh_offset + j * sizeof(symbol),
                    &symbol, sizeof(symbol)))
                return -BOOK8_ENOEXEC;
            if ((symbol.st_info & 0x0fu) != STT_FUNC
                || symbol.st_shndx == SHN_UNDEF)
                continue;
            if (!read_string(file, strings, symbol.st_name, name,
                    sizeof(name)))
                continue;
            if (!string_equal(name, B8_DRIVER_ENTRY_SYMBOL))
                continue;
            if (found || symbol.st_value > UINT64_MAX - bias
                || !segment_contains(segments, header->e_phnum,
                    symbol.st_value, 1u, PF_X))
                return -BOOK8_ENOEXEC;
            *entry = (uintptr_t)(bias + symbol.st_value);
            found = true;
        }
    }
    return found ? 0 : -BOOK8_ENOEXEC;
}

static void release_image(struct b8_loaded_driver *loaded)
{
    if (loaded->image != 0 && loaded->image_pages != 0u)
        vmm_free_pages(loaded->image, loaded->image_pages);
    loaded->image = 0;
    loaded->image_pages = 0u;
    loaded->descriptor = 0;
    loaded->initialized = false;
}

int32_t b8_driver_load(const char *path, const uint8_t expected_digest[32],
    const b8_sym_export_t *providers, uint32_t provider_count,
    struct b8_loaded_driver *loaded)
{
    if (path == 0 || expected_digest == 0 || loaded == 0
        || (provider_count != 0u && providers == 0)
        || provider_count > B8_DRIVER_MAX_PROVIDERS)
        return -BOOK8_EINVAL;
    *loaded = (struct b8_loaded_driver){0};
    file_handle_t *handle = fs_open_any(path);
    if (handle == 0)
        return -BOOK8_ENOENT;
    uint32_t file_size = handle->size;
    if (file_size < sizeof(Elf64_Ehdr)
        || file_size > B8_DRIVER_MAX_IMAGE_SIZE) {
        fs_close(handle);
        return -BOOK8_ENOEXEC;
    }
    uint32_t file_pages = (file_size + (uint32_t)DRIVER_PAGE - 1u)
        / (uint32_t)DRIVER_PAGE;
    uint8_t *file_memory = vmm_alloc_pages(file_pages);
    if (file_memory == 0) {
        fs_close(handle);
        return -BOOK8_ENOMEM;
    }
    bool read_ok = fs_seek(handle, 0u)
        && fs_read(handle, file_memory, file_size) == (int32_t)file_size;
    fs_close(handle);
    if (!read_ok) {
        vmm_free_pages(file_memory, file_pages);
        return -BOOK8_EIO;
    }
    driver_file_t file = {.bytes = file_memory, .size = file_size};
    uint8_t digest[BOOK8_SHA256_BYTES];
    book8_sha256(file.bytes, file.size, digest);
    if (!book8_digest_equal(digest, expected_digest, sizeof(digest))) {
        vmm_free_pages(file_memory, file_pages);
        return -BOOK8_EPERM;
    }

    Elf64_Ehdr header;
    Elf64_Phdr segments[DRIVER_MAX_PHDRS];
    Elf64_Shdr sections[DRIVER_MAX_SECTIONS];
    uint64_t image_low, image_high;
    int32_t status = validate_elf(&file, &header, segments, &image_low,
        &image_high);
    if (status != 0)
        goto failed_file;
    if (!file_read(&file, header.e_shoff, sections,
            (uint64_t)header.e_shnum * sizeof(Elf64_Shdr))) {
        status = -BOOK8_ENOEXEC;
        goto failed_file;
    }
    uint32_t imports_count = 0u;
    uint64_t imports_offset = 0u;
    status = find_imports(&file, &header, sections, &imports_count,
        &imports_offset);
    if (status != 0)
        goto failed_file;

    uint64_t image_bytes = image_high - image_low;
    uint32_t image_pages = (uint32_t)((image_bytes + DRIVER_PAGE - 1u)
        / DRIVER_PAGE);
    void *image = vmm_alloc_contiguous_pages(image_pages);
    if (image == 0) {
        status = -BOOK8_ENOMEM;
        goto failed_file;
    }
    uint64_t bias = (uint64_t)(uintptr_t)image - image_low;
    for (uint32_t i = 0u; i < header.e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        if (segment->p_type != PT_LOAD)
            continue;
        uint64_t offset = segment->p_vaddr - image_low;
        if (segment->p_filesz != 0u)
            file_read(&file, segment->p_offset,
                (uint8_t *)image + offset, segment->p_filesz);
    }
    status = apply_relocations(&file, &header, segments, sections, image_low,
        image_high, bias, image, imports_count, imports_offset, providers,
        provider_count);
    if (status != 0) {
        vmm_free_pages(image, image_pages);
        goto failed_file;
    }
    uintptr_t entry_address;
    status = find_entry(&file, &header, segments, bias, sections,
        &entry_address);
    if (status != 0) {
        vmm_free_pages(image, image_pages);
        goto failed_file;
    }
    vmm_free_pages(file_memory, file_pages);

    b8_driver_entry_fn entry = (b8_driver_entry_fn)entry_address;
    const b8_driver_t *descriptor = entry();
    if (descriptor == 0 || descriptor->abi_version != B8_DRIVER_ABI_VERSION
        || descriptor->struct_size < sizeof(*descriptor)
        || descriptor->init == 0) {
        vmm_free_pages(image, image_pages);
        return -BOOK8_ENOEXEC;
    }
    loaded->image = image;
    loaded->image_pages = image_pages;
    loaded->descriptor = descriptor;
    status = descriptor->init();
    if (status != 0) {
        if (descriptor->shutdown != 0)
            (void)descriptor->shutdown();
        release_image(loaded);
        return status < 0 ? status : -BOOK8_ENODEV;
    }
    loaded->initialized = true;
    return 0;

failed_file:
    vmm_free_pages(file_memory, file_pages);
    return status;
}

void b8_driver_unload(struct b8_loaded_driver *loaded)
{
    if (loaded == 0)
        return;
    if (loaded->initialized && loaded->descriptor != 0
        && loaded->descriptor->shutdown != 0)
        (void)loaded->descriptor->shutdown();
    release_image(loaded);
}

int32_t b8_driver_probe(struct b8_loaded_driver *loaded,
    const struct b8_driver_device *device, void *state)
{
    if (loaded == 0 || !loaded->initialized || loaded->descriptor == 0
        || loaded->descriptor->probe == 0 || device == 0)
        return -BOOK8_EINVAL;
    return loaded->descriptor->probe(device, state);
}

void b8_driver_remove(struct b8_loaded_driver *loaded,
    const struct b8_driver_device *device, void *state)
{
    if (loaded != 0 && loaded->initialized && loaded->descriptor != 0
        && loaded->descriptor->remove != 0 && device != 0)
        loaded->descriptor->remove(device, state);
}
