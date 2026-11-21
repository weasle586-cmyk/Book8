#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/b8_driver.h"
#include "../include/b8_shlib.h"
#include "../include/syscall.h"
#include "../kernel/exec.h"
#include "../src/session.h"

#define IMAGE_CAPACITY 4096u
#define PROGRAM_HEADERS_OFFSET 64u
#define SECTION_HEADERS_OFFSET 1024u
#define STRINGS_OFFSET 1500u
#define EXPORTS_OFFSET 1600u
#define IMPORTS_OFFSET 1900u
#define DYNSTR_OFFSET 2300u
#define DYNSYM_OFFSET 2400u
#define RELA_OFFSET 2600u

_Static_assert(B8_DRIVER_ABI_VERSION == 2u, "driver ABI version");
_Static_assert(sizeof(b8_sym_table_header_t) == 12u, "symbol table header layout");
_Static_assert(sizeof(b8_sym_export_record_t) == 84u, "export record layout");
_Static_assert(sizeof(b8_sym_import_t) == 72u, "import record layout");

static uint8_t image_bytes[IMAGE_CAPACITY];
static file_handle_t image_file;
static uint32_t image_size;
static uint32_t close_count;
static uint8_t package_image_bytes[IMAGE_CAPACITY];
static uint32_t package_image_size;
static file_handle_t package_image_file;
static uint8_t library_image_bytes[IMAGE_CAPACITY];
static uint32_t library_image_size;
static file_handle_t library_image_file;
static uint8_t package_manifest_bytes[512];
static uint32_t package_manifest_size;
static file_handle_t package_manifest_file;
static bool package_files_enabled;
static bool expect_package_relocations;
static bool package_relocations_checked;
uint64_t boot_pml4[512];

void book8_scheduler_test_worker(void) {}
uint64_t book8_read_cr3(void) { return 0u; }
void book8_write_cr3(uint64_t value) { (void)value; }
void syscall_select_task(struct exec_task *task) { (void)task; }
static uint64_t task_read_value(struct exec_task *task, uint64_t address,
    uint32_t width)
{
    uint64_t pde = task->user_pd[(address - USER_BASE) >> 21u];
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & 0x000ffffffffff000ull);
    uint64_t pte = pt[(address >> 12u) & 511u];
    const uint8_t *frame = (const uint8_t *)(uintptr_t)
        (pte & 0x000ffffffffff000ull);
    uint64_t value = 0u;
    for (uint32_t i = 0u; i < width; ++i)
        value |= (uint64_t)frame[(address & 4095u) + i] << (i * 8u);
    return value;
}
long syscall_run_user(struct exec_task *task)
{
    if (expect_package_relocations) {
        uint64_t provider = (uint64_t)(uintptr_t)task->modules[0].exports[0].ptr;
        uint64_t pc32 = task_read_value(task, USER_BASE + 0x3020u, 4u);
        uint64_t plt32 = task_read_value(task, USER_BASE + 0x3024u, 4u);
        assert(task_read_value(task, USER_BASE + 0x3010u, 8u) == provider + 5u);
        assert((int32_t)pc32 == (int64_t)(provider - 4u
            - (USER_BASE + 0x3020u)));
        assert((int32_t)plt32 == (int64_t)(provider - 4u
            - (USER_BASE + 0x3024u)));
        assert(task_read_value(task, USER_BASE + 0x3030u, 8u) == provider);
        assert(task_read_value(task, USER_BASE + 0x3038u, 8u) == provider);
        assert(task_read_value(task, USER_BASE + 0x3040u, 8u)
            == USER_BASE + 0x1040u);
        assert(task_read_value(task, task->modules[0].load_bias + 0x2010u,
            8u) == task->modules[0].load_bias + 0x1040u);
        package_relocations_checked = true;
    }
    return 0;
}
bool swap_available(void) { return false; }
bool swap_alloc_slot(uint32_t *slot) { (void)slot; return false; }
bool swap_write_page(uint32_t slot, const void *page)
{ (void)slot; (void)page; return false; }
bool swap_read_page(uint32_t slot, void *page)
{ (void)slot; (void)page; return false; }
void swap_free_slot(uint32_t slot) { (void)slot; }
void *vmm_alloc_pages(uint32_t count)
{
    void *memory = 0;
    if (count == 0u || posix_memalign(&memory, 4096u,
            (size_t)count * 4096u) != 0)
        return 0;
    memset(memory, 0, (size_t)count * 4096u);
    return memory;
}
void vmm_free_pages(void *base, uint32_t count)
{ (void)count; free(base); }

file_handle_t *fs_open_any(const char *path)
{
    file_handle_t *file = &image_file;
    if (package_files_enabled) {
        if (strcmp(path, "A:\\APPS\\PKG\\APP.ELF") == 0) {
            file = &package_image_file;
            file->size = package_image_size;
        } else if (strcmp(path, "A:\\APPS\\PKG\\manifest.ini") == 0) {
            file = &package_manifest_file;
            file->size = package_manifest_size;
        } else if (strcmp(path, "A:\\APPS\\PKG\\lib\\test.b8s") == 0) {
            file = &library_image_file;
            file->size = library_image_size;
        } else {
            return 0;
        }
    } else {
        (void)path;
        file->size = image_size;
    }
    file->offset = 0u;
    file->used = true;
    return file;
}

bool fs_seek(file_handle_t *file, uint32_t offset)
{
    if (file == 0 || !file->used || offset > file->size)
        return false;
    file->offset = offset;
    return true;
}

int32_t fs_read(file_handle_t *file, void *buffer, uint32_t count)
{
    const uint8_t *source;
    if (file == &image_file) {
        source = image_bytes;
    } else if (file == &package_image_file) {
        source = package_image_bytes;
    } else if (file == &library_image_file) {
        source = library_image_bytes;
    } else if (file == &package_manifest_file) {
        source = package_manifest_bytes;
    } else {
        return -1;
    }
    if (!file->used || count > file->size - file->offset)
        return -1;
    memcpy(buffer, source + file->offset, count);
    file->offset += count;
    return (int32_t)count;
}

void fs_close(file_handle_t *file)
{
    assert(file == &image_file || file == &package_image_file
        || file == &library_image_file || file == &package_manifest_file);
    assert(file->used);
    file->used = false;
    ++close_count;
}

static uint32_t string_offset(const char *name)
{
    static const char strings[] =
        "\0.shstrtab\0.b8s_exports\0.b8s_imports\0.dynstr\0.dynsym\0.rela.dyn\0";
    for (uint32_t i = 0u; i < sizeof(strings);) {
        if (strcmp(strings + i, name) == 0) { return i; }
        i += (uint32_t)strlen(strings + i) + 1u;
    }
    assert(false);
    return 0u;
}

static void add_section(Elf64_Shdr *section, const char *name, uint32_t type,
    uint64_t offset, uint64_t size)
{
    *section = (Elf64_Shdr){
        .sh_name = string_offset(name), .sh_type = type,
        .sh_offset = offset, .sh_size = size
    };
}

static void make_image(uint16_t type, uint32_t exports_count,
    bool include_imports)
{
    Elf64_Ehdr header = {0};
    Elf64_Phdr programs[2] = {
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_offset = 512u,
            .p_vaddr = 0x1000u, .p_filesz = 256u, .p_memsz = 512u},
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_W, .p_offset = 768u,
            .p_vaddr = 0x2000u, .p_filesz = 128u, .p_memsz = 256u}
    };
    static const char strings[] =
        "\0.shstrtab\0.b8s_exports\0.b8s_imports\0.dynstr\0.dynsym\0.rela.dyn\0";
    uint16_t section_count = include_imports ? 4u : 3u;
    uint64_t export_size = sizeof(b8_sym_table_header_t)
        + (uint64_t)exports_count * sizeof(b8_sym_export_record_t);

    memset(image_bytes, 0, sizeof(image_bytes));
    memset(&image_file, 0, sizeof(image_file));
    image_size = IMPORTS_OFFSET + sizeof(b8_sym_table_header_t)
        + sizeof(b8_sym_import_t);
    image_file.size = image_size;
    image_file.used = true;
    header.e_ident[0] = 0x7fu; header.e_ident[1] = 'E';
    header.e_ident[2] = 'L'; header.e_ident[3] = 'F';
    header.e_ident[4] = ELFCLASS64; header.e_ident[5] = ELFDATA2LSB;
    header.e_ident[6] = EV_CURRENT; header.e_type = type;
    header.e_machine = EM_X86_64; header.e_version = EV_CURRENT;
    header.e_entry = 0x1040u; header.e_phoff = PROGRAM_HEADERS_OFFSET;
    header.e_shoff = SECTION_HEADERS_OFFSET; header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr); header.e_phnum = 2u;
    header.e_shentsize = sizeof(Elf64_Shdr); header.e_shnum = section_count;
    header.e_shstrndx = 1u;
    memcpy(image_bytes, &header, sizeof(header));
    memcpy(image_bytes + PROGRAM_HEADERS_OFFSET, programs, sizeof(programs));

    Elf64_Shdr *sections = (Elf64_Shdr *)(image_bytes + SECTION_HEADERS_OFFSET);
    add_section(&sections[1], ".shstrtab", SHT_STRTAB, STRINGS_OFFSET,
        sizeof(strings));
    add_section(&sections[2], B8_SHLIB_EXPORT_SECTION, SHT_PROGBITS,
        EXPORTS_OFFSET, export_size);
    if (include_imports) {
        add_section(&sections[3], B8_SHLIB_IMPORT_SECTION, SHT_PROGBITS,
            IMPORTS_OFFSET, sizeof(b8_sym_table_header_t)
                + sizeof(b8_sym_import_t));
    }
    memcpy(image_bytes + STRINGS_OFFSET, strings, sizeof(strings));

    b8_sym_table_header_t export_header = {
        .abi_version = B8_SHLIB_ABI_VERSION, .count = exports_count
    };
    memcpy(export_header.magic, B8_SHLIB_MAGIC, B8_SHLIB_MAGIC_SIZE);
    memcpy(image_bytes + EXPORTS_OFFSET, &export_header, sizeof(export_header));
    for (uint32_t i = 0u; i < exports_count; ++i) {
        b8_sym_export_record_t record = {0};
        snprintf(record.name, sizeof(record.name), "symbol%u", i);
        record.value = i == 1u ? 0x2000u : 0x1040u;
        record.type = i == 1u ? B8_SYM_DATA : B8_SYM_FUNC;
        record.version_major = 1u;
        record.version_minor = i;
        memcpy(image_bytes + EXPORTS_OFFSET + sizeof(export_header)
            + i * sizeof(record), &record, sizeof(record));
    }
    if (include_imports) {
        b8_sym_table_header_t import_header = {
            .abi_version = B8_SHLIB_ABI_VERSION, .count = 1u
        };
        b8_sym_import_t import_record = {0};
        memcpy(import_record.name, "shared_widget", sizeof("shared_widget"));
        import_record.version_major = 1u;
        import_record.version_minor = 2u;
        memcpy(import_header.magic, B8_SHLIB_MAGIC, B8_SHLIB_MAGIC_SIZE);
        memcpy(image_bytes + IMPORTS_OFFSET, &import_header,
            sizeof(import_header));
        memcpy(image_bytes + IMPORTS_OFFSET + sizeof(import_header),
            &import_record, sizeof(import_record));
    }
}

static void add_relocation_sections(uint8_t *bytes, uint32_t *size,
    bool imported_symbol, bool library_relative)
{
    Elf64_Ehdr *header = (Elf64_Ehdr *)bytes;
    Elf64_Shdr *sections = (Elf64_Shdr *)(bytes + SECTION_HEADERS_OFFSET);
    Elf64_Sym symbols[2] = {0};
    Elf64_Rela relas[6] = {0};
    const char dynstr[] = "\0shared_widget\0";
    uint32_t symbol_count = imported_symbol ? 2u : 1u;
    uint32_t rela_count = library_relative ? 1u : (imported_symbol ? 6u : 0u);
    uint32_t dynstr_size = imported_symbol ? sizeof(dynstr) : 1u;

    memcpy(bytes + DYNSTR_OFFSET, dynstr, dynstr_size);
    if (imported_symbol) {
        symbols[1] = (Elf64_Sym){
            .st_name = 1u,
            .st_info = (STB_GLOBAL << 4u) | STT_FUNC,
            .st_shndx = SHN_UNDEF
        };
        relas[0] = (Elf64_Rela){
            .r_offset = USER_BASE + 0x3010u,
            .r_info = ((uint64_t)1u << 32u) | R_X86_64_64,
            .r_addend = 5
        };
        relas[1] = (Elf64_Rela){
            .r_offset = USER_BASE + 0x3020u,
            .r_info = ((uint64_t)1u << 32u) | R_X86_64_PC32,
            .r_addend = -4
        };
        relas[2] = (Elf64_Rela){
            .r_offset = USER_BASE + 0x3024u,
            .r_info = ((uint64_t)1u << 32u) | R_X86_64_PLT32,
            .r_addend = -4
        };
        relas[3] = (Elf64_Rela){
            .r_offset = USER_BASE + 0x3030u,
            .r_info = ((uint64_t)1u << 32u) | R_X86_64_GLOB_DAT
        };
        relas[4] = (Elf64_Rela){
            .r_offset = USER_BASE + 0x3038u,
            .r_info = ((uint64_t)1u << 32u) | R_X86_64_JUMP_SLOT
        };
        relas[5] = (Elf64_Rela){
            .r_offset = USER_BASE + 0x3040u,
            .r_info = R_X86_64_RELATIVE,
            .r_addend = USER_BASE + 0x1040u
        };
    } else if (library_relative) {
        relas[0] = (Elf64_Rela){
            .r_offset = 0x2010u,
            .r_info = R_X86_64_RELATIVE,
            .r_addend = 0x1040
        };
    }
    memcpy(bytes + DYNSYM_OFFSET, symbols,
        (size_t)symbol_count * sizeof(symbols[0]));
    if (rela_count != 0u) {
        memcpy(bytes + RELA_OFFSET, relas,
            (size_t)rela_count * sizeof(relas[0]));
    }
    sections[4] = (Elf64_Shdr){
        .sh_name = string_offset(".dynstr"), .sh_type = SHT_STRTAB,
        .sh_offset = DYNSTR_OFFSET, .sh_size = dynstr_size,
        .sh_addralign = 1u
    };
    sections[5] = (Elf64_Shdr){
        .sh_name = string_offset(".dynsym"), .sh_type = SHT_DYNSYM,
        .sh_offset = DYNSYM_OFFSET,
        .sh_size = (uint64_t)symbol_count * sizeof(Elf64_Sym),
        .sh_link = 4u, .sh_addralign = 8u, .sh_entsize = sizeof(Elf64_Sym)
    };
    if (rela_count != 0u) {
        sections[6] = (Elf64_Shdr){
            .sh_name = string_offset(".rela.dyn"), .sh_type = SHT_RELA,
            .sh_flags = SHF_ALLOC, .sh_offset = RELA_OFFSET,
            .sh_size = (uint64_t)rela_count * sizeof(Elf64_Rela),
            .sh_link = 5u, .sh_addralign = 8u,
            .sh_entsize = sizeof(Elf64_Rela)
        };
        header->e_shnum = 7u;
        *size = RELA_OFFSET + rela_count * sizeof(Elf64_Rela);
    } else {
        header->e_shnum = 6u;
        *size = DYNSYM_OFFSET + symbol_count * sizeof(Elf64_Sym);
    }
}

static void inspect_rejected(void)
{
    struct elf_image parsed;
    assert(elf_inspect(&image_file, &parsed) == -BOOK8_ENOEXEC);
}

static void relocation_rejection_test(void)
{
    struct elf_image parsed;
    make_image(ET_DYN, 0u, false);
    add_relocation_sections(image_bytes, &image_size, false, true);
    image_file.size = image_size;
    long relocation_status = elf_inspect(&image_file, &parsed);
    if (relocation_status != 0)
        fprintf(stderr, "relative relocation inspect status: %ld\n",
            relocation_status);
    assert(relocation_status == 0);
    Elf64_Rela *rela = (Elf64_Rela *)(image_bytes + RELA_OFFSET);
    rela[0].r_info = 0xffu;
    assert(elf_inspect(&image_file, &parsed) == -BOOK8_ENOTSUP);
    rela[0].r_info = ((uint64_t)1u << 32u) | R_X86_64_RELATIVE;
    assert(elf_inspect(&image_file, &parsed) == -BOOK8_ENOEXEC);

    make_image(ET_DYN, 0u, false);
    Elf64_Ehdr *header = (Elf64_Ehdr *)image_bytes;
    Elf64_Shdr *sections = (Elf64_Shdr *)(image_bytes + SECTION_HEADERS_OFFSET);
    header->e_shnum = 4u;
    sections[3] = (Elf64_Shdr){
        .sh_type = SHT_RELA, .sh_offset = 1200u, .sh_size = 24u
    };
    assert(elf_inspect(&image_file, &parsed) == 0);
    sections[3].sh_type = SHT_RELR;
    sections[3].sh_size = 8u;
    sections[3].sh_flags = SHF_ALLOC;
    assert(elf_inspect(&image_file, &parsed) == -BOOK8_ENOTSUP);
    sections[3].sh_type = SHT_REL;
    assert(elf_inspect(&image_file, &parsed) == -BOOK8_ENOTSUP);

    make_image(ET_DYN, 0u, false);
    header = (Elf64_Ehdr *)image_bytes;
    header->e_phnum = 3u;
    Elf64_Phdr *programs = (Elf64_Phdr *)(image_bytes
        + PROGRAM_HEADERS_OFFSET);
    programs[2] = (Elf64_Phdr){
        .p_type = PT_DYNAMIC, .p_offset = 2100u,
        .p_filesz = 3u * sizeof(Elf64_Dyn),
        .p_memsz = 3u * sizeof(Elf64_Dyn)
    };
    Elf64_Dyn dynamic[3] = {
        {.d_tag = DT_RELASZ, .d_un.d_val = 24u},
        {.d_tag = DT_RELAENT, .d_un.d_val = sizeof(Elf64_Rela)},
        {.d_tag = DT_NULL, .d_un.d_val = 0u}
    };
    memcpy(image_bytes + 2100u, dynamic, sizeof(dynamic));
    image_size = 2100u + sizeof(dynamic);
    image_file.size = image_size;
    assert(elf_inspect(&image_file, &parsed) == -BOOK8_ENOTSUP);
    dynamic[0].d_un.d_val = 0u;
    memcpy(image_bytes + 2100u, dynamic, sizeof(dynamic));
    assert(elf_inspect(&image_file, &parsed) == 0);
}

static void package_fixture_prepare(bool main_imports, bool library_imports)
{
    static const char manifest[] =
        "[Package]\n"
        "name=PKG\n"
        "version=1.0.0\n"
        "entry=app.elf\n"
        "[Dependencies]\n"
        "libs=test.b8s\n";
    make_image(ET_EXEC, 0u, main_imports);
    Elf64_Ehdr *header = (Elf64_Ehdr *)image_bytes;
    Elf64_Phdr *programs = (Elf64_Phdr *)(image_bytes
        + PROGRAM_HEADERS_OFFSET);
    header->e_entry = USER_BASE + 0x1040u;
    programs[0].p_vaddr = USER_BASE + 0x1000u;
    programs[1].p_vaddr = USER_BASE + 0x3000u;
    package_image_size = image_size;
    memcpy(package_image_bytes, image_bytes, image_size);

    make_image(ET_DYN, 1u, library_imports);
    b8_sym_export_record_t exported;
    memcpy(&exported, image_bytes + EXPORTS_OFFSET
        + sizeof(b8_sym_table_header_t), sizeof(exported));
    memcpy(exported.name, "shared_widget", sizeof("shared_widget"));
    exported.version_minor = 2u;
    memcpy(image_bytes + EXPORTS_OFFSET + sizeof(b8_sym_table_header_t),
        &exported, sizeof(exported));
    library_image_size = image_size;
    memcpy(library_image_bytes, image_bytes, image_size);

    package_manifest_size = sizeof(manifest) - 1u;
    memcpy(package_manifest_bytes, manifest, package_manifest_size);
    package_files_enabled = true;
}

static void package_relocation_fixture_prepare(void)
{
    package_fixture_prepare(true, false);
    make_image(ET_EXEC, 0u, true);
    Elf64_Ehdr *header = (Elf64_Ehdr *)image_bytes;
    Elf64_Phdr *programs = (Elf64_Phdr *)(image_bytes
        + PROGRAM_HEADERS_OFFSET);
    header->e_entry = USER_BASE + 0x1040u;
    programs[0].p_vaddr = USER_BASE + 0x1000u;
    programs[1].p_vaddr = USER_BASE + 0x3000u;
    add_relocation_sections(image_bytes, &image_size, true, false);
    package_image_size = image_size;
    memcpy(package_image_bytes, image_bytes, image_size);

    make_image(ET_DYN, 1u, false);
    add_relocation_sections(image_bytes, &image_size, false, true);
    b8_sym_export_record_t exported;
    memcpy(&exported, image_bytes + EXPORTS_OFFSET
        + sizeof(b8_sym_table_header_t), sizeof(exported));
    memcpy(exported.name, "shared_widget", sizeof("shared_widget"));
    exported.version_minor = 2u;
    memcpy(image_bytes + EXPORTS_OFFSET + sizeof(b8_sym_table_header_t),
        &exported, sizeof(exported));
    library_image_size = image_size;
    memcpy(library_image_bytes, image_bytes, image_size);
}

static void package_loader_test(void)
{
    const char *path = "A:\\APPS\\PKG\\APP.ELF";
    long status;

    package_fixture_prepare(false, false);
    close_count = 0u;
    status = exec_run(path, 0, NULL);
    if (status != 0)
        fprintf(stderr, "package load status: %ld\n", status);
    assert(status == 0);
    assert(close_count == 5u && exec_current_pid() == 1u);

    package_fixture_prepare(true, false);
    close_count = 0u;
    assert(exec_run(path, 0, NULL) == 0);
    assert(close_count == 5u && exec_current_pid() == 1u);

    package_fixture_prepare(true, false);
    b8_sym_import_t *imported = (b8_sym_import_t *)(package_image_bytes
        + IMPORTS_OFFSET + sizeof(b8_sym_table_header_t));
    imported->version_minor = 3u;
    close_count = 0u;
    assert(exec_run(path, 0, NULL) == -BOOK8_EINVAL);
    assert(close_count == 3u && exec_current_pid() == 1u);

    package_fixture_prepare(false, true);
    close_count = 0u;
    assert(exec_run(path, 0, NULL) == -BOOK8_EINVAL);
    assert(close_count == 4u && exec_current_pid() == 1u);

    package_relocation_fixture_prepare();
    close_count = 0u;
    package_relocations_checked = false;
    expect_package_relocations = true;
    status = exec_run(path, 0, NULL);
    if (status != 0)
        fprintf(stderr, "relocated package status: %ld\n", status);
    assert(status == 0);
    expect_package_relocations = false;
    assert(package_relocations_checked);
    assert(close_count == 5u && exec_current_pid() == 1u);
    package_files_enabled = false;
}

int main(void)
{
    struct elf_image parsed;
    assert(book8_session_initialize(0x454c4642u));
    assert(book8_session_login("admin", "default"));

    make_image(ET_DYN, 2u, true);
    b8_sym_export_record_t second;
    snprintf(second.name, sizeof(second.name), "symbol1");
    second.value = 0x2000u; second.type = B8_SYM_DATA;
    second.version_major = 1u; second.version_minor = 1u;
    memcpy(image_bytes + EXPORTS_OFFSET + sizeof(b8_sym_table_header_t)
        + sizeof(b8_sym_export_record_t), &second, sizeof(second));
    assert(elf_inspect(&image_file, &parsed) == 0);
    assert(parsed.type == ET_DYN && parsed.exports_count == 2u
        && parsed.imports_count == 1u);
    assert(elf_require_executable(&parsed) == -BOOK8_ENOEXEC);
    assert(exec_scheduler_test_start());
    struct exec_task *resolver_task = exec_scheduler_test_current();
    assert(resolver_task != 0);
    b8_sym_export_t provider = {
        .name = "shared_widget", .ptr = (void *)(uintptr_t)(USER_BASE + 0x1040u),
        .type = B8_SYM_FUNC, .version_major = 1u, .version_minor = 2u
    };
    resolver_task->modules[0].exports = &provider;
    resolver_task->modules[0].export_count = 1u;
    resolver_task->module_count = 1u;
    assert(elf_resolve_imports(&image_file, &parsed) == 0);
    provider.version_major = 2u;
    assert(elf_resolve_imports(&image_file, &parsed) == -BOOK8_EINVAL);
    resolver_task->module_count = 0u;
    assert(elf_resolve_imports(&image_file, &parsed) == -BOOK8_ENOENT);
    assert(!exec_scheduler_test_finish());

    make_image(ET_DYN, 1u, false);
    ((b8_sym_table_header_t *)(image_bytes + EXPORTS_OFFSET))->magic[0] = 'X';
    inspect_rejected();
    make_image(ET_DYN, 1u, false);
    ((b8_sym_table_header_t *)(image_bytes + EXPORTS_OFFSET))->count = 2u;
    inspect_rejected();
    make_image(ET_DYN, 1u, false);
    b8_sym_export_record_t bad_type;
    memcpy(&bad_type, image_bytes + EXPORTS_OFFSET
        + sizeof(b8_sym_table_header_t), sizeof(bad_type));
    bad_type.value = 0x2000u;
    bad_type.type = B8_SYM_FUNC;
    memcpy(image_bytes + EXPORTS_OFFSET + sizeof(b8_sym_table_header_t),
        &bad_type, sizeof(bad_type));
    inspect_rejected();
    make_image(ET_DYN, 2u, false);
    b8_sym_export_record_t duplicate;
    memcpy(&duplicate, image_bytes + EXPORTS_OFFSET
        + sizeof(b8_sym_table_header_t), sizeof(duplicate));
    memcpy(duplicate.name, "symbol0", sizeof("symbol0"));
    memcpy(image_bytes + EXPORTS_OFFSET + sizeof(b8_sym_table_header_t)
        + sizeof(duplicate), &duplicate, sizeof(duplicate));
    inspect_rejected();
    make_image(ET_DYN, 1u, false);
    ((Elf64_Ehdr *)image_bytes)->e_shnum = 2u;
    inspect_rejected();
    relocation_rejection_test();

    make_image(ET_DYN, 0u, false);
    close_count = 0u;
    assert(exec_run("A:\\APP.ELF", 0, NULL) == -BOOK8_ENOEXEC);
    assert(close_count == 1u && exec_current_pid() == 1u);

    package_loader_test();
    make_image(ET_EXEC, 0u, false);
    ((Elf64_Ehdr *)image_bytes)->e_entry = USER_BASE + 0x40u;
    Elf64_Phdr *exec_programs =
        (Elf64_Phdr *)(image_bytes + PROGRAM_HEADERS_OFFSET);
    exec_programs[0].p_vaddr = USER_BASE;
    exec_programs[1].p_vaddr = USER_BASE + 0x2000u;
    assert(elf_inspect(&image_file, &parsed) == 0);
    assert(elf_require_executable(&parsed) == 0);
    puts("PASS: ET_DYN loading, x86-64 symbol resolution and RELA relocations");
    return 0;
}
