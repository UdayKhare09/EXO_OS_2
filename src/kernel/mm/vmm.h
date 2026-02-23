#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Page flags (x86_64 PTE bits) */
#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITE     (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_PWT       (1ULL << 3)
#define VMM_PCD       (1ULL << 4)   /* cache-disable (use for MMIO) */
#define VMM_HUGE      (1ULL << 7)   /* 2 MiB huge page              */
#define VMM_NX        (1ULL << 63)  /* no-execute                   */

#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITE)
#define VMM_MMIO       (VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT)

/* Initialise VMM with HHDM offset */
void vmm_init(uint64_t hhdm_offset, uintptr_t kernel_pml4_phys);

/* Convert physical address to HHDM virtual address */
uintptr_t vmm_phys_to_virt(uintptr_t phys);

/* Convert HHDM virtual address back to physical */
uintptr_t vmm_virt_to_phys(uintptr_t virt);

/* Map a physical page to a virtual address in the current PML4 */
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Unmap a virtual page */
void vmm_unmap_page(uintptr_t virt);

/* Map a contiguous MMIO region (returns virtual address) */
uintptr_t vmm_mmio_map(uintptr_t phys, size_t size);

/* Allocate and zero a page for page-table use */
uintptr_t vmm_alloc_page_table(void);
