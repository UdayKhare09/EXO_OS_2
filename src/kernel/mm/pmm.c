#include "pmm.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include <stdint.h>

/* Bitmap: 1 bit per physical page.
 * 0 = free, 1 = used.
 * The bitmap itself is stored in the first suitable free region. */

static uint8_t  *bitmap      = NULL;
static uint64_t  bitmap_pages = 0;   /* total pages tracked     */
static uint64_t  hhdm_base    = 0;   /* higher-half direct map  */
static uint64_t  free_pages   = 0;

static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |=  (uint8_t)(1 << (page % 8));
}
static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= (uint8_t)~(1 << (page % 8));
}
static inline int bitmap_test(uint64_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

void pmm_init(pmm_memmap_entry_t *entries, uint64_t entry_count,
              uint64_t hhdm_offset) {
    hhdm_base = hhdm_offset;

    /* Find highest physical address to size the bitmap */
    uint64_t highest = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        uint64_t end = entries[i].base + entries[i].length;
        if (end > highest) highest = end;
    }
    bitmap_pages = highest / PAGE_SIZE;
    uint64_t bitmap_bytes = (bitmap_pages + 7) / 8;

    /* Find a free region large enough to hold the bitmap */
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i].type != MEMMAP_USABLE) continue;
        if (entries[i].length < bitmap_bytes) continue;
        /* Place bitmap here */
        bitmap = (uint8_t *)(entries[i].base + hhdm_base);
        break;
    }
    if (!bitmap) kpanic("PMM: no region large enough for bitmap\n");

    /* Mark everything used initially */
    memset(bitmap, 0xFF, bitmap_bytes);

    /* Mark USABLE pages as free */
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i].type != MEMMAP_USABLE) continue;
        uint64_t start_page = entries[i].base / PAGE_SIZE;
        uint64_t num_pages  = entries[i].length / PAGE_SIZE;
        for (uint64_t p = start_page; p < start_page + num_pages; p++) {
            bitmap_clear(p);
            free_pages++;
        }
    }

    /* Mark the bitmap pages themselves as used */
    uint64_t bm_phys  = (uint64_t)bitmap - hhdm_base;
    uint64_t bm_start = bm_phys / PAGE_SIZE;
    uint64_t bm_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = bm_start; p < bm_start + bm_pages; p++) {
        if (!bitmap_test(p)) { bitmap_set(p); free_pages--; }
    }

    /* Mark page 0 as used (avoid null phys) */
    if (!bitmap_test(0)) { bitmap_set(0); free_pages--; }

    KLOG_INFO("PMM: %llu MB free / %llu total pages (bitmap @ %p)\n",
              free_pages * PAGE_SIZE / (1024*1024),
              bitmap_pages, (void *)bitmap);
}

uintptr_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    uint64_t run = 0;
    uint64_t start = 0;
    for (uint64_t p = 1; p < bitmap_pages; p++) {
        if (!bitmap_test(p)) {
            if (run == 0) start = p;
            run++;
            if (run == count) {
                for (uint64_t i = start; i < start + count; i++) {
                    bitmap_set(i);
                }
                free_pages -= count;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0; /* OOM */
}

void pmm_free_pages(uintptr_t phys, size_t count) {
    uint64_t start = phys / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (bitmap_test(start + i)) {
            bitmap_clear(start + i);
            free_pages++;
        }
    }
}

void pmm_print_stats(void) {
    KLOG_INFO("PMM: %llu pages free (%llu MB)\n",
              free_pages, free_pages * PAGE_SIZE / (1024*1024));
}
