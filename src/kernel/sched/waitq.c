/*
 * waitq.c — wait queue implementation for EXO_OS
 *
 * A wait queue is a simple FIFO of blocked tasks.
 * Tasks add themselves via waitq_wait() (which calls sched_block()).
 * Wakers call waitq_wake_one/all() to move tasks back to RUNNABLE queues.
 */
#include "waitq.h"
#include "task.h"
#include "sched.h"
#include "arch/x86_64/cpu.h"
#include <stddef.h>

/* ── Spinlock helpers ────────────────────────────────────────────────────── */

static inline void wq_lock(waitq_t *wq) {
    while (__atomic_test_and_set(&wq->lock, __ATOMIC_ACQUIRE));
}
static inline void wq_unlock(waitq_t *wq) {
    __atomic_clear(&wq->lock, __ATOMIC_RELEASE);
}

/* Save/restore IRQ flag to prevent local CPU timer ISR from deadlocking */
static inline uint64_t irq_save_cli(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory");
}

/* ── API ──────────────────────────────────────────────────────────────────── */

void waitq_init(waitq_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
    wq->lock = 0;
}

void waitq_wait(waitq_t *wq) {
    task_t *cur = sched_current();
    if (!cur) return;

    uint64_t f = irq_save_cli();
    wq_lock(wq);

    /* Enqueue self at tail */
    cur->next = NULL;
    if (!wq->head) {
        wq->head = wq->tail = cur;
    } else {
        wq->tail->next = cur;
        wq->tail       = cur;
    }

    wq_unlock(wq);
    irq_restore(f);

    /* Block this task — scheduler will pick another */
    sched_block();
}

void waitq_wake_one(waitq_t *wq) {
    wq_lock(wq);
    task_t *t = wq->head;
    if (t) {
        wq->head = t->next;
        if (!wq->head) wq->tail = NULL;
        t->next = NULL;
    }
    wq_unlock(wq);

    if (t) sched_unblock(t);
}

void waitq_wake_all(waitq_t *wq) {
    wq_lock(wq);
    task_t *list = wq->head;
    wq->head = wq->tail = NULL;
    wq_unlock(wq);

    /* Unblock all tasks in the list */
    while (list) {
        task_t *t = list;
        list = t->next;
        t->next = NULL;
        sched_unblock(t);
    }
}
