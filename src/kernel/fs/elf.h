/* fs/elf.h — ELF64 loader definitions
 *
 * Defines ELF64 structures and the elf_load() interface for loading
 * ELF executables into a process address space.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── ELF64 magic and identification ──────────────────────────────────────── */
#define EI_NIDENT   16
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1
#define ELFOSABI_NONE 0
#define ET_EXEC     2
#define ET_DYN      3       /* PIE executables */
#define EM_X86_64   62

/* ── ELF64 header ────────────────────────────────────────────────────────── */
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

/* ── Program header ──────────────────────────────────────────────────────── */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6
#define PT_TLS      7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

#define PF_X        (1 << 0)
#define PF_W        (1 << 1)
#define PF_R        (1 << 2)

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

/* ── Section header (for future use) ─────────────────────────────────────── */
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

/* ── Result structure from elf_load ──────────────────────────────────────── */
typedef struct {
    uint64_t entry;       /* entry point virtual address     */
    uint64_t brk_start;   /* first address past loaded data  */
    uint64_t phdr_vaddr;  /* vaddr of loaded PHDR (for auxv) */
    uint64_t load_base;   /* load base for ET_DYN            */
    uint16_t phdr_count;  /* number of program headers       */
    uint16_t phdr_size;   /* size of each phdr entry         */
    uint8_t  has_interp;  /* PT_INTERP present               */
    char     interp_path[256];
} elf_info_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Load an ELF64 executable from a memory buffer into the given address space.
 *
 * @param data        Pointer to the ELF file in kernel memory.
 * @param data_size   Size of the ELF file.
 * @param pml4_phys   Physical address of the target PML4.
 * @param load_bias   Load base for ET_DYN images (0 = default)
 * @param out         Filled with entry point and segment info.
 * @return 0 on success, negative errno on error.
 */
int elf_load(const void *data, uint64_t data_size,
             uintptr_t pml4_phys, uint64_t load_bias, elf_info_t *out);

/* Validate an ELF64 header. Returns 0 if valid, negative errno otherwise. */
int elf_validate(const Elf64_Ehdr *ehdr);
