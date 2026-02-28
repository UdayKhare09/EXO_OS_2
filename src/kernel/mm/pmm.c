#include "pmm.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include <stdint.h>

/* Bitmap: 1 bit per physical page.
 * 0 = free, 1 = used.
 * The bitmap itself is stored in the first suitable free region. */

static uint8_t  *bitmap      = NULL;
static uint32_t *page_refs   = NULL;
static uint64_t  bitmap_pages = 0;   /* total pages tracked     */
static uint64_t  hhdm_base    = 0;   /* higher-half direct map  */
static uint64_t  free_pages   = 0;
static uint64_t  usable_pages = 0;   /* total usable (RAM) pages */


static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |=  (uint8_t)(1 << (page % 8));
}
static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= (uint8_t)~(1 << (page % 8));
}
static inline int bitmap_test(uint64_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

static inline int page_in_bounds(uint64_t page) {
    return page < bitmap_pages;
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
    uint64_t refs_bytes   = bitmap_pages * sizeof(uint32_t);
    uint64_t bitmap_span  = (bitmap_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t refs_span    = (refs_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t meta_span    = bitmap_span + refs_span;

    /* Find a free region large enough to hold bitmap + refcounts */
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i].type != MEMMAP_USABLE) continue;
        if (entries[i].length < meta_span) continue;

        /* Place metadata here */
        bitmap = (uint8_t *)(entries[i].base + hhdm_base);
        page_refs = (uint32_t *)(entries[i].base + bitmap_span + hhdm_base);
        break;
    }
    if (!bitmap || !page_refs) {
        kpanic("PMM: no region large enough for metadata (bitmap+refs)\n");
    }

    /* Mark everything used initially */
    memset(bitmap, 0xFF, bitmap_bytes);
    memset(page_refs, 1, refs_bytes);

    /* Mark USABLE pages as free */
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i].type != MEMMAP_USABLE) continue;
        uint64_t start_page = entries[i].base / PAGE_SIZE;
        uint64_t num_pages  = entries[i].length / PAGE_SIZE;
        usable_pages += num_pages;
        for (uint64_t p = start_page; p < start_page + num_pages; p++) {
            bitmap_clear(p);
            page_refs[p] = 0;
            free_pages++;
        }
    }

    /* Mark metadata pages themselves as used */
    uint64_t bm_phys  = (uint64_t)bitmap - hhdm_base;
    uint64_t bm_start = bm_phys / PAGE_SIZE;
    uint64_t bm_pages = bitmap_span / PAGE_SIZE;
    for (uint64_t p = bm_start; p < bm_start + bm_pages; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            page_refs[p] = 1;
            free_pages--;
        }
    }

    uint64_t rf_phys  = (uint64_t)page_refs - hhdm_base;
    uint64_t rf_start = rf_phys / PAGE_SIZE;
    uint64_t rf_pages = refs_span / PAGE_SIZE;
    for (uint64_t p = rf_start; p < rf_start + rf_pages; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            page_refs[p] = 1;
            free_pages--;
        }
    }

    /* Mark page 0 as used (avoid null phys) */
    if (!bitmap_test(0)) {
        bitmap_set(0);
        page_refs[0] = 1;
        free_pages--;
    }

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
                    page_refs[i] = 1;
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
    if (start + count > bitmap_pages) {
        KLOG_WARN("PMM: pmm_free_pages: phys=0x%lx count=%zu out of bounds "
                  "(start_page=%llu bitmap_pages=%llu) — double-free or bad PTE?\n",
                  (unsigned long)phys, count,
                  (unsigned long long)start,
                  (unsigned long long)bitmap_pages);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        (void)pmm_page_unref((start + i) * PAGE_SIZE);
    }
}

uint32_t pmm_page_ref(uintptr_t phys) {
    if (!bitmap || !page_refs) return 0;
    uint64_t page = phys / PAGE_SIZE;
    if (!page_in_bounds(page)) return 0;
    if (!bitmap_test(page)) {
        KLOG_WARN("PMM: pmm_page_ref on free page phys=0x%lx\n",
                  (unsigned long)phys);
        return 0;
    }
    return __atomic_add_fetch(&page_refs[page], 1, __ATOMIC_RELAXED);
}

uint32_t pmm_page_unref(uintptr_t phys) {
    if (!bitmap || !page_refs) return 0;
    uint64_t page = phys / PAGE_SIZE;
    if (!page_in_bounds(page)) return 0;

    uint32_t old = __atomic_load_n(&page_refs[page], __ATOMIC_RELAXED);
    if (old == 0) {
        KLOG_WARN("PMM: pmm_page_unref underflow phys=0x%lx\n",
                  (unsigned long)phys);
        return 0;
    }

    uint32_t now = __atomic_sub_fetch(&page_refs[page], 1, __ATOMIC_RELAXED);
    if (now == 0 && bitmap_test(page)) {
        bitmap_clear(page);
        __atomic_add_fetch(&free_pages, 1, __ATOMIC_RELAXED);
    }
    return now;
}

uint32_t pmm_page_getref(uintptr_t phys) {
    if (!bitmap || !page_refs) return 0;
    uint64_t page = phys / PAGE_SIZE;
    if (!page_in_bounds(page)) return 0;
    return __atomic_load_n(&page_refs[page], __ATOMIC_RELAXED);
}

void pmm_print_stats(void) {
    KLOG_INFO("PMM: %llu pages free (%llu MB)\n",
              free_pages, free_pages * PAGE_SIZE / (1024*1024));
}

uint64_t pmm_get_total_pages(void) { return usable_pages; }
uint64_t pmm_get_free_pages(void)  { return free_pages; }

int pmm_phys_valid(uintptr_t phys, size_t count) {
    if (!bitmap || count == 0) return 0;
    uint64_t start = phys / PAGE_SIZE;
    return (start + count <= bitmap_pages) ? 1 : 0;
}
