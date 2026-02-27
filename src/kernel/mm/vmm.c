#include "vmm.h"
#include "pmm.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "sched/task.h"
#include <stdint.h>
#include <stddef.h>

static uint64_t  hhdm_off     = 0;
static uintptr_t kernel_pml4  = 0;   /* physical address of kernel PML4 */

/* ── HHDM helpers ─────────────────────────────────────────────────────────── */
uintptr_t vmm_phys_to_virt(uintptr_t phys) { return phys + hhdm_off; }
uintptr_t vmm_virt_to_phys(uintptr_t virt) { return virt - hhdm_off; }

uintptr_t vmm_get_kernel_pml4(void) { return kernel_pml4; }

void vmm_init(uint64_t hhdm_offset, uintptr_t kpml4_phys) {
    hhdm_off    = hhdm_offset;
    kernel_pml4 = kpml4_phys;
    KLOG_INFO("VMM: HHDM offset=%p kernel PML4=%p\n",
              (void *)hhdm_off, (void *)kernel_pml4);
}

/* ── Page table helpers ───────────────────────────────────────────────────── */
uintptr_t vmm_alloc_page_table(void) {
    uintptr_t phys = pmm_alloc_pages(1);
    if (!phys) kpanic("VMM: cannot allocate page table page\n");
    memset((void *)vmm_phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* Return pointer to a PTE, allocating intermediate tables as needed.
 * Works on any PML4 (kernel or per-process).
 * If `user_hier` is true, intermediate table entries get VMM_USER set
 * so the CPU allows user-mode traversal. */
static uint64_t *get_pte_in(uintptr_t pml4_phys, uintptr_t virt,
                            bool alloc, bool user_hier) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t hier_flags = VMM_PRESENT | VMM_WRITE;
    if (user_hier) hier_flags |= VMM_USER;

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_phys);

    /* PML4 → PDPT */
    if (!(pml4[pml4_idx] & VMM_PRESENT)) {
        if (!alloc) return NULL;
        uintptr_t pdpt_phys = vmm_alloc_page_table();
        pml4[pml4_idx] = pdpt_phys | hier_flags;
    }
    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_idx] & VMM_PTE_ADDR_MASK);

    /* PDPT → PD */
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        if (!alloc) return NULL;
        uintptr_t pd_phys = vmm_alloc_page_table();
        pdpt[pdpt_idx] = pd_phys | hier_flags;
    }
    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_idx] & VMM_PTE_ADDR_MASK);

    /* PD → PT */
    if (!(pd[pd_idx] & VMM_PRESENT)) {
        if (!alloc) return NULL;
        uintptr_t pt_phys = vmm_alloc_page_table();
        pd[pd_idx] = pt_phys | hier_flags;
    }
    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_idx] & VMM_PTE_ADDR_MASK);

    return &pt[pt_idx];
}

/* Legacy wrapper: get PTE in the kernel PML4 (no VMM_USER on hierarchy) */
static uint64_t *get_pte(uintptr_t pml4_phys, uintptr_t virt, bool alloc) {
    return get_pte_in(pml4_phys, virt, alloc, false);
}

void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    uint64_t *pte = get_pte(kernel_pml4, virt, true);
    *pte = (phys & VMM_PTE_ADDR_MASK) | flags;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_unmap_page(uintptr_t virt) {
    uint64_t *pte = get_pte(kernel_pml4, virt, false);
    if (pte) { *pte = 0; }
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* MMIO: identity-map HHDM virtual region with cache-disable flags.
 * If size is 0 (BAR size-probe failure or unknown), map 64 KiB as a
 * safe minimum — the xHCI operational registers fit comfortably.        */
uintptr_t vmm_mmio_map(uintptr_t phys, size_t size) {
    uintptr_t virt = vmm_phys_to_virt(phys);
    if (size == 0) size = 0x10000;  /* 64 KiB default */
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page(virt + i * PAGE_SIZE,
                     phys + i * PAGE_SIZE,
                     VMM_MMIO);
    }
    return virt;
}

/* ── Per-process address space management ─────────────────────────────────── */

uintptr_t vmm_create_address_space(void) {
    uintptr_t new_pml4_phys = vmm_alloc_page_table();
    if (!new_pml4_phys) return 0;

    uint64_t *new_pml4 = (uint64_t *)vmm_phys_to_virt(new_pml4_phys);
    uint64_t *kern_pml4 = (uint64_t *)vmm_phys_to_virt(kernel_pml4);

    /* Mirror kernel-half PML4 entries (indices 256–511).
     * The user-half (0–255) is already zeroed by vmm_alloc_page_table(). */
    for (int i = KERNEL_PML4_START; i < 512; i++) {
        new_pml4[i] = kern_pml4[i];
    }

    return new_pml4_phys;
}

/* Recursively free page tables in the user-half of an address space */
static void free_pt_level(uintptr_t table_phys, int level) {
    uint64_t *table = (uint64_t *)vmm_phys_to_virt(table_phys);

    for (int i = 0; i < 512; i++) {
        if (!(table[i] & VMM_PRESENT)) continue;

        uintptr_t child_phys = table[i] & VMM_PTE_ADDR_MASK;

        if (level == 1) {
            /* Level 1 = PT: entries are leaf 4 KiB pages.
             * Validate physical address before freeing — a corrupt or stale
             * PTE might contain garbage that would trip the PMM bounds check. */
            if (!pmm_phys_valid(child_phys, 1)) {
                KLOG_WARN("VMM: free_pt_level: skipping bad leaf PTE phys=0x%lx\n",
                          (unsigned long)child_phys);
                continue;
            }
            pmm_free_pages(child_phys, 1);
        } else if (table[i] & VMM_HUGE) {
            /* Huge page leaf — do NOT recurse: child_phys is a data page,
             * not a page table.  Free the underlying pages directly.
             *   level 2 (PD)   → 2 MiB = 512 × 4 KiB pages
             *   level 3 (PDPT) → 1 GiB = 512 × 512 × 4 KiB pages        */
            size_t npages = (level == 2) ? 512 : (512 * 512);
            pmm_free_pages(child_phys, npages);
        } else {
            /* Recurse into PDPT (3) → PD (2) → PT (1) */
            free_pt_level(child_phys, level - 1);
            /* Free the child table page itself */
            pmm_free_pages(child_phys, 1);
        }
    }
}

void vmm_destroy_address_space(uintptr_t pml4_phys) {
    if (!pml4_phys || pml4_phys == kernel_pml4) return;

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_phys);

    /* Only free user-half entries (indices 0–255).
     * Kernel-half entries are shared references — DO NOT free them. */
    for (int i = 0; i < KERNEL_PML4_START; i++) {
        if (!(pml4[i] & VMM_PRESENT)) continue;

        uintptr_t pdpt_phys = pml4[i] & VMM_PTE_ADDR_MASK;
        free_pt_level(pdpt_phys, 3); /* PDPT → PD → PT → pages */
        pmm_free_pages(pdpt_phys, 1);
    }

    /* Free the PML4 page itself */
    pmm_free_pages(pml4_phys, 1);
}

void vmm_map_page_in(uintptr_t pml4_phys, uintptr_t virt,
                     uintptr_t phys, uint64_t flags) {
    /* For user-half addresses, set VMM_USER on hierarchy automatically */
    bool user = (virt < USER_SPACE_END);
    if (user) flags |= VMM_USER;

    uint64_t *pte = get_pte_in(pml4_phys, virt, true, user);
    if (!pte) kpanic("VMM: cannot map page in pml4=%p virt=%p\n",
                     (void *)pml4_phys, (void *)virt);
    *pte = (phys & VMM_PTE_ADDR_MASK) | flags;
}

uintptr_t vmm_unmap_page_in(uintptr_t pml4_phys, uintptr_t virt) {
    bool user = (virt < USER_SPACE_END);
    uint64_t *pte = get_pte_in(pml4_phys, virt, false, user);
    if (!pte || !(*pte & VMM_PRESENT)) return 0;

    uintptr_t phys = *pte & VMM_PTE_ADDR_MASK;
    *pte = 0;
    return phys;
}

uint64_t *vmm_get_pte(uintptr_t pml4_phys, uintptr_t virt) {
    bool user = (virt < USER_SPACE_END);
    uint64_t *pte = get_pte_in(pml4_phys, virt, false, user);
    if (!pte || !(*pte & VMM_PRESENT)) return NULL;
    return pte;
}

/* ── COW clone ────────────────────────────────────────────────────────────── */

/* Clone a single page table (PT, level 1) with COW semantics:
 * mark all present entries read-only + COW in both source and clone. */
static uintptr_t clone_pt(uintptr_t src_pt_phys) {
    uintptr_t dst_pt_phys = vmm_alloc_page_table();
    if (!dst_pt_phys) return 0;

    uint64_t *src = (uint64_t *)vmm_phys_to_virt(src_pt_phys);
    uint64_t *dst = (uint64_t *)vmm_phys_to_virt(dst_pt_phys);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & VMM_PRESENT)) { dst[i] = 0; continue; }

        /* Mark source as COW + read-only */
        src[i] = (src[i] & ~VMM_WRITE) | VMM_COW;
        /* Clone with same COW read-only flags */
        dst[i] = src[i];
    }

    return dst_pt_phys;
}

/* Clone a PD (level 2): clone each child PT */
static uintptr_t clone_pd(uintptr_t src_pd_phys) {
    uintptr_t dst_pd_phys = vmm_alloc_page_table();
    if (!dst_pd_phys) return 0;

    uint64_t *src = (uint64_t *)vmm_phys_to_virt(src_pd_phys);
    uint64_t *dst = (uint64_t *)vmm_phys_to_virt(dst_pd_phys);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & VMM_PRESENT)) { dst[i] = 0; continue; }
        if (src[i] & VMM_HUGE) {
            /* 2 MiB huge page leaf — mark COW in both src and clone */
            src[i] = (src[i] & ~VMM_WRITE) | VMM_COW;
            dst[i] = src[i];
            continue;
        }
        uintptr_t child = clone_pt(src[i] & VMM_PTE_ADDR_MASK);
        if (!child) return 0; /* OOM — caller should handle cleanup */
        /* Preserve hierarchy flags (PRESENT|WRITE|USER) */
        dst[i] = child | (src[i] & ~VMM_PTE_ADDR_MASK);
    }

    return dst_pd_phys;
}

/* Clone a PDPT (level 3): clone each child PD */
static uintptr_t clone_pdpt(uintptr_t src_pdpt_phys) {
    uintptr_t dst_pdpt_phys = vmm_alloc_page_table();
    if (!dst_pdpt_phys) return 0;

    uint64_t *src = (uint64_t *)vmm_phys_to_virt(src_pdpt_phys);
    uint64_t *dst = (uint64_t *)vmm_phys_to_virt(dst_pdpt_phys);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & VMM_PRESENT)) { dst[i] = 0; continue; }
        if (src[i] & VMM_HUGE) {
            /* 1 GiB huge page leaf — mark COW in both src and clone */
            src[i] = (src[i] & ~VMM_WRITE) | VMM_COW;
            dst[i] = src[i];
            continue;
        }
        uintptr_t child = clone_pd(src[i] & VMM_PTE_ADDR_MASK);
        if (!child) return 0;
        dst[i] = child | (src[i] & ~VMM_PTE_ADDR_MASK);
    }

    return dst_pdpt_phys;
}

uintptr_t vmm_clone_address_space(uintptr_t src_pml4_phys) {
    uintptr_t dst_pml4_phys = vmm_alloc_page_table();
    if (!dst_pml4_phys) return 0;

    uint64_t *src = (uint64_t *)vmm_phys_to_virt(src_pml4_phys);
    uint64_t *dst = (uint64_t *)vmm_phys_to_virt(dst_pml4_phys);

    /* Clone user-half (indices 0–255) with COW */
    for (int i = 0; i < KERNEL_PML4_START; i++) {
        if (!(src[i] & VMM_PRESENT)) { dst[i] = 0; continue; }
        uintptr_t child = clone_pdpt(src[i] & VMM_PTE_ADDR_MASK);
        if (!child) {
            /* OOM: clean up what we allocated so far */
            vmm_destroy_address_space(dst_pml4_phys);
            return 0;
        }
        dst[i] = child | (src[i] & ~VMM_PTE_ADDR_MASK);
    }

    /* Mirror kernel-half (indices 256–511) — shared, not cloned */
    uint64_t *kern = (uint64_t *)vmm_phys_to_virt(kernel_pml4);
    for (int i = KERNEL_PML4_START; i < 512; i++) {
        dst[i] = kern[i];
    }

    /* Flush TLB for the source to pick up the new COW read-only PTEs */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3"
                     ::: "rax", "memory");

    return dst_pml4_phys;
}

/* ── Page fault handler ───────────────────────────────────────────────────── */

/* Look up the VMA covering fault_addr in the current task's VMA list.
 * Returns NULL if no VMA covers the address. */
static vma_t *find_vma_for(uintptr_t fault_addr) {
    extern task_t *sched_current(void);
    task_t *cur = sched_current();
    if (!cur) return NULL;

    vma_t *v = cur->vma_list;
    while (v) {
        if (fault_addr >= v->start && fault_addr < v->end)
            return v;
        v = v->next;
    }
    return NULL;
}

bool vmm_handle_page_fault(uintptr_t pml4_phys, uintptr_t fault_addr,
                           uint64_t error_code) {
    bool user = (fault_addr < USER_SPACE_END);

    /* ── Case 1: COW — write fault on a present, COW-marked page ─────── */
    if (error_code & 0x2) {  /* write fault */
        uint64_t *pte = get_pte_in(pml4_phys, fault_addr, false, user);
        if (pte && (*pte & VMM_PRESENT) && (*pte & VMM_COW)) {
            uintptr_t old_phys = *pte & VMM_PTE_ADDR_MASK;
            uint64_t  old_flags = *pte & ~VMM_PTE_ADDR_MASK;

            uintptr_t new_phys = pmm_alloc_pages(1);
            if (!new_phys) return false;

            memcpy((void *)vmm_phys_to_virt(new_phys),
                   (void *)vmm_phys_to_virt(old_phys),
                   PAGE_SIZE);

            uint64_t new_flags = (old_flags & ~VMM_COW) | VMM_WRITE;
            *pte = (new_phys & VMM_PTE_ADDR_MASK) | new_flags;
            __asm__ volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
            return true;
        }
    }

    /* ── Case 2: Demand paging — non-present page within a valid VMA ──── */
    if (!(error_code & 0x1)) {  /* page not present */
        vma_t *vma = find_vma_for(fault_addr);
        if (!vma) return false;  /* no VMA → real fault */

        uintptr_t page_addr = fault_addr & ~(PAGE_SIZE - 1);
        uintptr_t phys = pmm_alloc_pages(1);
        if (!phys) return false;

        memset((void *)vmm_phys_to_virt(phys), 0, PAGE_SIZE);

        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (vma->flags & VMA_WRITE)  flags |= VMM_WRITE;
        if (!(vma->flags & VMA_EXEC)) flags |= VMM_NX;

        vmm_map_page_in(pml4_phys, page_addr, phys, flags);
        __asm__ volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
        return true;
    }

    return false;
}
