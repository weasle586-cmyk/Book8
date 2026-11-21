#ifndef BOOK8_ELF_H
#define BOOK8_ELF_H

#include <stdint.h>

#define EI_NIDENT   16
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define SHT_RELA    4
#define SHT_SYMTAB  2
#define SHT_PROGBITS 1
#define SHT_STRTAB  3
#define SHT_NOBITS  8
#define SHT_DYNSYM  11
#define SHT_REL     9
#define SHT_RELR    19
#define SHF_ALLOC   0x2ull
#define SHN_UNDEF   0u
#define SHN_ABS     0xfff1u
#define SHN_COMMON  0xfff2u
#define STB_GLOBAL  1u
#define STB_WEAK    2u
#define STT_NOTYPE  0u
#define STT_OBJECT  1u
#define STT_FUNC    2u
#define STT_SECTION 3u
#define R_X86_64_NONE      0u
#define R_X86_64_64        1u
#define R_X86_64_PC32      2u
#define R_X86_64_PLT32     4u
#define R_X86_64_GLOB_DAT  6u
#define R_X86_64_JUMP_SLOT 7u
#define R_X86_64_RELATIVE  8u
#define DT_NULL     0
#define DT_PLTRELSZ 2
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_TEXTREL  22
#define DT_JMPREL   23
#define DT_RELR     36
#define DT_RELRSZ   35
#define DT_PLTREL   20
#define PF_X        1u
#define PF_W        2u
#define PF_R        4u

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) Elf64_Shdr;

typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} __attribute__((packed)) Elf64_Dyn;

typedef struct {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} __attribute__((packed)) Elf64_Rela;

#define ELF64_R_SYM(info) ((uint32_t)((info) >> 32u))
#define ELF64_R_TYPE(info) ((uint32_t)(info))

#endif
