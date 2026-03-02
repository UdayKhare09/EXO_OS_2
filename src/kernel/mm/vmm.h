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
#define VMM_ACCESSED  (1ULL << 5)
#define VMM_DIRTY     (1ULL << 6)
#define VMM_HUGE      (1ULL << 7)   /* 2 MiB huge page              */
#define VMM_GLOBAL    (1ULL << 8)
#define VMM_COW       (1ULL << 9)   /* software: copy-on-write       */
#define VMM_NX        (1ULL << 63)  /* no-execute                   */
/* Write-combining: uses PAT[1] after cpu_pat_init() programs IA32_PAT.
 * Requires PWT=1, PCD=0 in the PTE — same as VMM_PWT.                     */
#define VMM_WC        VMM_PWT       /* write-combining (PAT[1])      */

#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITE)
#define VMM_MMIO       (VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT)
#define VMM_USER_RO    (VMM_PRESENT | VMM_USER)
#define VMM_USER_RW    (VMM_PRESENT | VMM_WRITE | VMM_USER)
#define VMM_USER_RX    (VMM_PRESENT | VMM_USER)             /* no NX */
#define VMM_USER_RWX   (VMM_PRESENT | VMM_WRITE | VMM_USER) /* no NX */

/* User-space virtual address boundaries (canonical lower-half) */
#define USER_SPACE_START  0x0000000000400000ULL  /* 4 MiB (avoid null pages)  */
#define USER_SPACE_END    0x00007FFFFFFFF000ULL  /* top of canonical lower half */
#define USER_STACK_TOP    0x00007FFFFFF00000ULL  /* default user stack top     */
#define USER_MMAP_BASE    0x0000200000000000ULL  /* mmap region start          */
#define USER_HEAP_BASE    0x0000000010000000ULL  /* default brk start (256 MiB)*/

/* PTE mask to extract physical address from a page table entry */
#define VMM_PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Number of entries in upper-half (kernel) PML4: indices 256–511 */
#define KERNEL_PML4_START  256

/* Initialise VMM with HHDM offset */
void vmm_init(uint64_t hhdm_offset, uintptr_t kernel_pml4_phys);

/* Convert physical address to HHDM virtual address */
uintptr_t vmm_phys_to_virt(uintptr_t phys);

/* Convert HHDM virtual address back to physical */
uintptr_t vmm_virt_to_phys(uintptr_t virt);

/* Map a physical page to a virtual address in the kernel PML4 */
void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Unmap a virtual page from the kernel PML4 */
void vmm_unmap_page(uintptr_t virt);

/* Map a contiguous MMIO region (returns virtual address) */
uintptr_t vmm_mmio_map(uintptr_t phys, size_t size);

/* Allocate and zero a page for page-table use */
uintptr_t vmm_alloc_page_table(void);

/* ── Per-process address space management ─────────────────────────────────── */

/* Create a new address space: fresh PML4 with kernel-half mirrored.
 * Returns the physical address of the new PML4 (0 on OOM). */
uintptr_t vmm_create_address_space(void);

/* Destroy a user address space: free all user-half page tables and
 * mapped physical pages. Does NOT free kernel-half pages. */
void vmm_destroy_address_space(uintptr_t pml4_phys);

/* Map a page in a specific address space (not the current kernel PML4).
 * For user-half mappings, VMM_USER is automatically added to flags. */
void vmm_map_page_in(uintptr_t pml4_phys, uintptr_t virt,
                     uintptr_t phys, uint64_t flags);

/* Unmap a page in a specific address space. Returns the physical address
 * that was mapped (0 if nothing was mapped). Does NOT free the phys page. */
uintptr_t vmm_unmap_page_in(uintptr_t pml4_phys, uintptr_t virt);

/* Clone a user address space with copy-on-write.
 * Returns the physical address of the new PML4 (0 on OOM). */
uintptr_t vmm_clone_address_space(uintptr_t src_pml4_phys);

/* Get the PTE for a virtual address in a given PML4, without allocating.
 * Returns NULL if not mapped. */
uint64_t *vmm_get_pte(uintptr_t pml4_phys, uintptr_t virt);

/* Handle a page fault. Returns true if handled (COW / demand-page),
 * false if the fault is a real violation. */
bool vmm_handle_page_fault(uintptr_t pml4_phys, uintptr_t fault_addr,
                           uint64_t error_code);

/* Get the kernel PML4 physical address */
uintptr_t vmm_get_kernel_pml4(void);
