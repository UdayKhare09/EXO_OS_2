#pragma once

#include <stdint.h>
#include "fs/vfs.h"

void pagecache_init(void);

/* Returns physical page and acquires a borrow + PMM page ref for caller. */
uintptr_t pagecache_lookup(vnode_t *vn, uint64_t offset);

/* Insert a page into cache as ownership reference (must be page-aligned). */
int pagecache_insert(vnode_t *vn, uint64_t offset, uintptr_t phys_page);

/* Drop one borrow previously acquired by lookup/get_or_read. */
void pagecache_release(vnode_t *vn, uint64_t offset);

/* Lookup or read one file page into cache. Returns phys page + temp ref. */
uintptr_t pagecache_get_or_read(vnode_t *vn, uint64_t offset);
