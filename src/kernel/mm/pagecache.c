#include "mm/pagecache.h"

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"

#include <stddef.h>

#define PAGECACHE_BUCKETS 1024

typedef struct pagecache_entry {
    vnode_t *vn;
    uint64_t offset;
    uintptr_t phys_page;
    uint32_t refcount;
    struct pagecache_entry *hash_next;
} pagecache_entry_t;

static pagecache_entry_t *g_buckets[PAGECACHE_BUCKETS];
static volatile int g_pagecache_lock = 0;

static inline void pagecache_lock(void) {
    while (__atomic_test_and_set(&g_pagecache_lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static inline void pagecache_unlock(void) {
    __atomic_clear(&g_pagecache_lock, __ATOMIC_RELEASE);
}

static inline uint64_t page_align(uint64_t v) {
    return v & ~(uint64_t)(PAGE_SIZE - 1);
}

static inline size_t pagecache_hash(vnode_t *vn, uint64_t offset) {
    uint64_t key = ((uint64_t)(uintptr_t)vn >> 3) ^ (offset >> 12);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (size_t)(key & (PAGECACHE_BUCKETS - 1));
}

static pagecache_entry_t *pagecache_find_locked(vnode_t *vn, uint64_t offset) {
    size_t idx = pagecache_hash(vn, offset);
    pagecache_entry_t *it = g_buckets[idx];
    while (it) {
        if (it->vn == vn && it->offset == offset)
            return it;
        it = it->hash_next;
    }
    return NULL;
}

void pagecache_init(void) {
    memset(g_buckets, 0, sizeof(g_buckets));
    __atomic_store_n(&g_pagecache_lock, 0, __ATOMIC_RELEASE);
    KLOG_INFO("pagecache: initialized (%u buckets)\n", PAGECACHE_BUCKETS);
}

uintptr_t pagecache_lookup(vnode_t *vn, uint64_t offset) {
    if (!vn) return 0;
    offset = page_align(offset);

    pagecache_lock();
    pagecache_entry_t *e = pagecache_find_locked(vn, offset);
    if (!e) {
        pagecache_unlock();
        return 0;
    }

    e->refcount++;
    uintptr_t phys = e->phys_page;
    pagecache_unlock();

    pmm_page_ref(phys);
    return phys;
}

int pagecache_insert(vnode_t *vn, uint64_t offset, uintptr_t phys_page) {
    if (!vn || !phys_page) return -1;
    offset = page_align(offset);

    pagecache_entry_t *e = kmalloc(sizeof(*e));
    if (!e) return -1;

    e->vn = vn;
    e->offset = offset;
    e->phys_page = phys_page;
    e->refcount = 0;
    e->hash_next = NULL;

    pagecache_lock();
    pagecache_entry_t *exist = pagecache_find_locked(vn, offset);
    if (exist) {
        pagecache_unlock();
        kfree(e);
        return 1;
    }

    size_t idx = pagecache_hash(vn, offset);
    e->hash_next = g_buckets[idx];
    g_buckets[idx] = e;
    pagecache_unlock();

    vfs_vnode_get(vn);
    return 0;
}

void pagecache_release(vnode_t *vn, uint64_t offset) {
    if (!vn) return;
    offset = page_align(offset);

    pagecache_lock();
    pagecache_entry_t *e = pagecache_find_locked(vn, offset);
    if (!e || e->refcount == 0) {
        pagecache_unlock();
        return;
    }

    e->refcount--;
    pagecache_unlock();
}

uintptr_t pagecache_get_or_read(vnode_t *vn, uint64_t offset) {
    if (!vn || !vn->ops || !vn->ops->read) return 0;
    offset = page_align(offset);

    uintptr_t phys = pagecache_lookup(vn, offset);
    if (phys) return phys;

    uintptr_t new_phys = pmm_alloc_pages(1);
    if (!new_phys) return 0;

    void *dst = (void *)vmm_phys_to_virt(new_phys);
    memset(dst, 0, PAGE_SIZE);

    if (offset < vn->size) {
        size_t to_read = (size_t)(vn->size - offset);
        if (to_read > PAGE_SIZE) to_read = PAGE_SIZE;
        ssize_t rd = vn->ops->read(vn, dst, to_read, offset);
        if (rd < 0) {
            pmm_page_unref(new_phys);
            return 0;
        }
    }

    int ins = pagecache_insert(vn, offset, new_phys);
    if (ins < 0) {
        pmm_page_unref(new_phys);
        return 0;
    }
    if (ins > 0) {
        pmm_page_unref(new_phys);
    }

    return pagecache_lookup(vn, offset);
}
