/* lib/spinlock.h — Simple ticket-less test-and-set spinlock
 *
 * Uses gcc/clang __atomic builtins.  Same pattern as waitq.c's wq_lock/unlock.
 * Include this wherever you need spinlock_t / spinlock_init / acquire / release.
 */
#pragma once
#include <stdint.h>

typedef volatile int spinlock_t;

static inline void spinlock_init(spinlock_t *lk) {
    __atomic_clear(lk, __ATOMIC_RELAXED);
}

static inline void spinlock_acquire(spinlock_t *lk) {
    while (__atomic_test_and_set(lk, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
}

static inline void spinlock_release(spinlock_t *lk) {
    __atomic_clear(lk, __ATOMIC_RELEASE);
}
