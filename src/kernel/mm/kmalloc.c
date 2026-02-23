/*
 * kmalloc.c — simple slab allocator for the kernel
 *
 * Size classes (user-visible): 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.
 * Allocations > 2048 bytes fall back to whole PMM pages.
 *
 * Layout of each allocated block:
 *   [8-byte header][...user data...]
 *   header = class index (0-7)  for slab allocs
 *   header = (npages<<32)|0xFFFFFFFF  for large allocs
 *
 * Free blocks reuse the first 8 bytes as a "next" freelist pointer.
 * The same 8 bytes become the header when the block is allocated,
 * so there is no wasted memory per block.
 */
#include "kmalloc.h"
#include "pmm.h"
#include "vmm.h"
#include "lib/klog.h"
#include <stdint.h>
#include <stddef.h>

#define NCLASSES 8
static const size_t class_sizes[NCLASSES] = {16, 32, 64, 128, 256, 512, 1024, 2048};

#define LARGE_MAGIC  0xFFFFFFFFUL

/* Freelist: first 8 bytes of each free block = next pointer */
typedef struct slab_node { struct slab_node *next; } slab_node_t;
static slab_node_t *freelists[NCLASSES];

/* Ticket-less test-and-set spinlock */
static volatile int kmalloc_lock = 0;
static inline void km_lock(void) {
    while (__atomic_test_and_set(&kmalloc_lock, __ATOMIC_ACQUIRE));
}
static inline void km_unlock(void) {
    __atomic_clear(&kmalloc_lock, __ATOMIC_RELEASE);
}

/* Refill a slab class with one fresh PMM page */
static void refill_slab(int cls) {
    uintptr_t phys = pmm_alloc_pages(1);
    if (!phys) return;

    size_t block_sz = class_sizes[cls] + 8;   /* 8-byte header included */
    size_t n        = PAGE_SIZE / block_sz;
    uint8_t *base   = (uint8_t *)vmm_phys_to_virt(phys);

    /* Chain all blocks into the freelist */
    for (size_t i = 0; i < n; i++) {
        slab_node_t *node = (slab_node_t *)(base + i * block_sz);
        node->next        = freelists[cls];
        freelists[cls]    = node;
    }
}

void kmalloc_init(void) {
    for (int i = 0; i < NCLASSES; i++) {
        freelists[i] = NULL;
        refill_slab(i);   /* Pre-warm each class with one page */
    }
    KLOG_INFO("kmalloc: slab allocator ready (%d size classes)\n", NCLASSES);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Find the smallest class that fits */
    int cls = -1;
    for (int i = 0; i < NCLASSES; i++) {
        if (size <= class_sizes[i]) { cls = i; break; }
    }

    if (cls < 0) {
        /* Large allocation: whole PMM pages, 8-byte header at start */
        size_t total  = size + 8;
        size_t npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t phys = pmm_alloc_pages(npages);
        if (!phys) return NULL;
        uint64_t *hdr = (uint64_t *)vmm_phys_to_virt(phys);
        *hdr = ((uint64_t)npages << 32) | LARGE_MAGIC;
        return hdr + 1;
    }

    km_lock();
    if (!freelists[cls]) refill_slab(cls);
    slab_node_t *node = freelists[cls];
    if (!node) { km_unlock(); return NULL; }
    freelists[cls] = node->next;
    km_unlock();

    /* Write class index into the block's header bytes, return user ptr */
    uint64_t *hdr = (uint64_t *)node;
    *hdr = (uint64_t)cls;
    return hdr + 1;
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t *hdr = (uint64_t *)ptr - 1;
    uint32_t magic = (uint32_t)(*hdr & 0xFFFFFFFFUL);

    if (magic == (uint32_t)LARGE_MAGIC) {
        uint32_t npages = (uint32_t)(*hdr >> 32);
        uintptr_t virt  = (uintptr_t)hdr;
        uintptr_t phys  = vmm_virt_to_phys(virt);
        pmm_free_pages(phys, npages);
        return;
    }

    int cls = (int)magic;
    if (cls < 0 || cls >= NCLASSES) return;   /* corrupt header guard */

    slab_node_t *node = (slab_node_t *)hdr;
    km_lock();
    node->next     = freelists[cls];
    freelists[cls] = node;
    km_unlock();
}
