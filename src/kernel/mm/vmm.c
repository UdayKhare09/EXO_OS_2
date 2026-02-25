#include "vmm.h"
#include "pmm.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include <stdint.h>

static uint64_t  hhdm_off     = 0;
static uintptr_t kernel_pml4  = 0;   /* physial address of kernel PML4 */

/* ── HHDM helpers ─────────────────────────────────────────────────────────── */
uintptr_t vmm_phys_to_virt(uintptr_t phys) { return phys + hhdm_off; }
uintptr_t vmm_virt_to_phys(uintptr_t virt) { return virt - hhdm_off; }

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

/* Return pointer to a PTE, allocating intermediate tables as needed */
static uint64_t *get_pte(uintptr_t pml4_phys, uintptr_t virt, bool alloc) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_phys);

    /* PML4 → PDPT */
    if (!(pml4[pml4_idx] & VMM_PRESENT)) {
        if (!alloc) return NULL;
        uintptr_t pdpt_phys = vmm_alloc_page_table();
        pml4[pml4_idx] = pdpt_phys | VMM_PRESENT | VMM_WRITE;
    }
    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_idx] & ~0xFFFULL);

    /* PDPT → PD */
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        if (!alloc) return NULL;
        uintptr_t pd_phys = vmm_alloc_page_table();
        pdpt[pdpt_idx] = pd_phys | VMM_PRESENT | VMM_WRITE;
    }
    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);

    /* PD → PT */
    if (!(pd[pd_idx] & VMM_PRESENT)) {
        if (!alloc) return NULL;
        uintptr_t pt_phys = vmm_alloc_page_table();
        pd[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITE;
    }
    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_idx] & ~0xFFFULL);

    return &pt[pt_idx];
}

void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    uint64_t *pte = get_pte(kernel_pml4, virt, true);
    *pte = (phys & ~0xFFFULL) | flags;
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
