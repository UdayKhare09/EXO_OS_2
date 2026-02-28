/* fs/elf.c — ELF64 loader
 *
 * Loads statically-linked ELF64 executables into a user process
 * address space by mapping PT_LOAD segments.
 */
#include "elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "fs/vfs.h"        /* for errno constants */

#include <stdint.h>
#include <stddef.h>

/* ── Validate ELF64 header ───────────────────────────────────────────────── */
int elf_validate(const Elf64_Ehdr *ehdr) {
    if (!ehdr) return -EFAULT;

    /* Magic check */
    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        KLOG_WARN("elf: bad magic\n");
        return -ENOEXEC;
    }

    /* Must be 64-bit, little-endian */
    if (ehdr->e_ident[4] != ELFCLASS64) {
        KLOG_WARN("elf: not 64-bit\n");
        return -ENOEXEC;
    }
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        KLOG_WARN("elf: not little-endian\n");
        return -ENOEXEC;
    }

    /* Must be executable or shared object (PIE) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        KLOG_WARN("elf: not executable (type=%u)\n", ehdr->e_type);
        return -ENOEXEC;
    }

    /* Must be x86-64 */
    if (ehdr->e_machine != EM_X86_64) {
        KLOG_WARN("elf: not x86_64 (machine=%u)\n", ehdr->e_machine);
        return -ENOEXEC;
    }

    /* Sanity-check program header size */
    if (ehdr->e_phentsize < sizeof(Elf64_Phdr)) {
        KLOG_WARN("elf: phentsize too small (%u)\n", ehdr->e_phentsize);
        return -ENOEXEC;
    }

    return 0;
}

/* ── Load ELF64 segments into address space ──────────────────────────────── */
int elf_load(const void *data, uint64_t data_size,
             uintptr_t pml4_phys, uint64_t load_bias, elf_info_t *out) {
    if (!data || !out) return -EFAULT;
    if (data_size < sizeof(Elf64_Ehdr)) return -ENOEXEC;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    int r = elf_validate(ehdr);
    if (r < 0) return r;

    /* Validate phdr offset */
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > data_size) {
        KLOG_WARN("elf: phdr table out of bounds\n");
        return -ENOEXEC;
    }

    const uint8_t *raw = (const uint8_t *)data;
    uint64_t brk_end = 0;   /* track highest loaded address */
    uint64_t dyn_bias = (ehdr->e_type == ET_DYN)
                      ? (load_bias ? load_bias : USER_SPACE_START)
                      : 0;

    out->entry      = ehdr->e_entry + dyn_bias;
    out->phdr_count = ehdr->e_phnum;
    out->phdr_size  = ehdr->e_phentsize;
    out->phdr_vaddr = 0;
    out->load_base  = dyn_bias;
    out->has_interp = 0;
    out->interp_path[0] = '\0';

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(raw + ehdr->e_phoff
                                                       + i * ehdr->e_phentsize);

        if (phdr->p_type == PT_PHDR) {
            out->phdr_vaddr = phdr->p_vaddr + dyn_bias;
            continue;
        }

        if (phdr->p_type == PT_INTERP) {
            if (phdr->p_filesz == 0 || phdr->p_filesz >= sizeof(out->interp_path)) {
                KLOG_WARN("elf: PT_INTERP invalid size %llu\n", phdr->p_filesz);
                return -ENOEXEC;
            }
            if (phdr->p_offset + phdr->p_filesz > data_size) {
                KLOG_WARN("elf: PT_INTERP out of bounds\n");
                return -ENOEXEC;
            }
            memcpy(out->interp_path, raw + phdr->p_offset, phdr->p_filesz);
            out->interp_path[phdr->p_filesz - 1] = '\0';
            out->has_interp = 1;
            continue;
        }

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Validate segment bounds in file */
        if (phdr->p_offset + phdr->p_filesz > data_size) {
            KLOG_WARN("elf: segment %u file data out of bounds\n", i);
            return -ENOEXEC;
        }

        uint64_t seg_vaddr = phdr->p_vaddr + dyn_bias;

        /* Segment vaddr must be in user space */
        if (seg_vaddr < USER_SPACE_START || seg_vaddr >= USER_SPACE_END) {
            KLOG_WARN("elf: segment %u vaddr %p out of user space\n",
                      i, (void *)seg_vaddr);
            return -ENOEXEC;
        }

        /* Determine page flags */
        uint64_t pflags = VMM_PRESENT | VMM_USER;
        if (phdr->p_flags & PF_W) pflags |= VMM_WRITE;
        if (!(phdr->p_flags & PF_X)) pflags |= VMM_NX;

        /* Map pages covering [vaddr, vaddr+memsz), page-aligned */
        uint64_t seg_start = seg_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end   = (seg_vaddr + phdr->p_memsz + PAGE_SIZE - 1)
                             & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            /* Check if page already mapped (overlapping segments) */
            uint64_t *pte = vmm_get_pte(pml4_phys, va);
            if (pte && (*pte & VMM_PRESENT))
                continue;

            uintptr_t pg = pmm_alloc_pages(1);
            if (!pg) {
                KLOG_WARN("elf: OOM mapping segment %u\n", i);
                return -ENOMEM;
            }
            memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
            vmm_map_page_in(pml4_phys, va, pg, pflags);
        }

        /* Copy file data into mapped pages */
        uint64_t copied = 0;
        while (copied < phdr->p_filesz) {
            uint64_t va = seg_vaddr + copied;
            uint64_t page_off = va & (PAGE_SIZE - 1);
            uint64_t chunk = PAGE_SIZE - page_off;
            if (chunk > phdr->p_filesz - copied)
                chunk = phdr->p_filesz - copied;

            /* Look up physical page via PTE */
            uint64_t *pte = vmm_get_pte(pml4_phys, va);
            if (!pte || !(*pte & VMM_PRESENT)) {
                KLOG_WARN("elf: page not mapped at %p during copy\n", (void *)va);
                return -EFAULT;
            }

            uintptr_t phys = *pte & VMM_PTE_ADDR_MASK;
            uint8_t *dst = (uint8_t *)vmm_phys_to_virt(phys) + page_off;
            memcpy(dst, raw + phdr->p_offset + copied, chunk);
            copied += chunk;
        }

        /* Track highest address for brk */
        uint64_t seg_top = seg_vaddr + phdr->p_memsz;
        if (seg_top > brk_end) brk_end = seg_top;

    }

    /* brk starts at page-aligned end of loaded segments */
    out->brk_start = (brk_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    /* Many static binaries (including BusyBox/musl) omit PT_PHDR.
     * In that case, derive AT_PHDR from the load segment that contains
     * e_phoff in the file image. */
    if (!out->phdr_vaddr) {
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            const Elf64_Phdr *phdr = (const Elf64_Phdr *)(raw + ehdr->e_phoff
                                                           + i * ehdr->e_phentsize);
            if (phdr->p_type != PT_LOAD)
                continue;
            if (ehdr->e_phoff < phdr->p_offset)
                continue;
            uint64_t delta = ehdr->e_phoff - phdr->p_offset;
            if (delta >= phdr->p_filesz)
                continue;
            out->phdr_vaddr = phdr->p_vaddr + dyn_bias + delta;
            break;
        }
    }
    return 0;
}
