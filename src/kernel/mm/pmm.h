#pragma once
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12

/* Memory map entry types — must match Limine protocol exactly:
 *   LIMINE_MEMMAP_USABLE                 0
 *   LIMINE_MEMMAP_RESERVED               1
 *   LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
 *   LIMINE_MEMMAP_ACPI_NVS               3
 *   LIMINE_MEMMAP_BAD_MEMORY             4
 *   LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
 *   LIMINE_MEMMAP_KERNEL_AND_MODULES     6
 *   LIMINE_MEMMAP_FRAMEBUFFER            7
 */
#define MEMMAP_USABLE                 0
#define MEMMAP_RESERVED               1
#define MEMMAP_ACPI_RECLAIMABLE       2
#define MEMMAP_ACPI_NVS               3
#define MEMMAP_BAD_MEMORY             4
#define MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define MEMMAP_KERNEL_AND_MODULES     6
#define MEMMAP_FRAMEBUFFER            7

typedef struct {
    uint64_t base;
    uint64_t length;
    uint64_t type;
} pmm_memmap_entry_t;

/* Initialise PMM from Limine memory map */
void pmm_init(pmm_memmap_entry_t *entries, uint64_t entry_count,
              uint64_t hhdm_offset);

/* Allocate N contiguous physical pages. Returns physical address or 0 on OOM */
uintptr_t pmm_alloc_pages(size_t count);

/* Free N contiguous physical pages */
void pmm_free_pages(uintptr_t phys, size_t count);

/* Statistics */
void pmm_print_stats(void);
