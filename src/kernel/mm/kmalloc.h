#pragma once
#include <stddef.h>
#include <stdint.h>

/* Initialise the kernel slab allocator (call once after VMM is up) */
void kmalloc_init(void);

/* Allocate at least `size` bytes of kernel memory (returns NULL on OOM).
 * Allocations >= 4096 bytes are backed directly by PMM whole pages.
 * Minimum alignment is 8 bytes. */
void *kmalloc(size_t size);

/* Free a pointer returned by kmalloc. */
void kfree(void *ptr);

/* Zero-initialised kmalloc */
static inline void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) {
        /* memset via byte loop — avoids depending on string.h here */
        for (size_t i = 0; i < size; i++) ((char *)p)[i] = 0;
    }
    return p;
}
