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
#include "lib/timer.h"
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

/* ── waitq_wait_timeout ───────────────────────────────────────────────────── *
 * Block current task on wq until either:
 *   (a) waitq_wake_one/all() fires               → returns 0
 *   (b) timeout_ms milliseconds elapse           → returns 1
 *
 * A ktimer is armed at entry; if it fires before a wakup it removes the task
 * from the waitq and calls sched_unblock().  Either path ends in exactly one
 * sched_unblock() call, keeping the run-queue consistent.
 * ─────────────────────────────────────────────────────────────────────────── */

/* Per-call context structure (stack-allocated by caller) */
struct wq_timeout_ctx {
    waitq_t        *wq;
    task_t         *task;
    volatile int    fired;   /* 1 = we were woken by the timer, 0 = by waitq */
};

/* Timer callback — runs in IRQ context */
static void wq_timeout_cb(ktimer_t *timer, void *arg) {
    (void)timer;
    struct wq_timeout_ctx *ctx = (struct wq_timeout_ctx *)arg;

    /* Mark as timer-fired so the caller function can report the right code */
    ctx->fired = 1;

    /* Remove self from the waitq list.  The task may or may not still be
     * there — if waitq_wake_*() already removed it, we iterate and find
     * nothing; that is safe.                                               */
    uint64_t f = irq_save_cli();
    wq_lock(ctx->wq);
    task_t *prev = NULL;
    task_t *t    = ctx->wq->head;
    while (t) {
        if (t == ctx->task) {
            if (prev)             prev->next  = t->next;
            else                  ctx->wq->head = t->next;
            if (ctx->wq->tail == t) ctx->wq->tail = prev;
            t->next = NULL;
            break;
        }
        prev = t;
        t    = t->next;
    }
    wq_unlock(ctx->wq);
    irq_restore(f);

    /* Only unblock if the task is still in a blocking state; if
     * waitq_wake_*() already made it RUNNABLE we do nothing.             */
    if (ctx->task->state == TASK_BLOCKED)
        sched_unblock(ctx->task);
}

int waitq_wait_timeout(waitq_t *wq, uint64_t timeout_ms) {
    if (timeout_ms == 0) return 1;   /* instant timeout */

    task_t *cur = sched_current();
    if (!cur) return 1;

    struct wq_timeout_ctx ctx = {
        .wq   = wq,
        .task = cur,
        .fired = 0,
    };

    /* Arm the guard timer before enqueuing so there is no window where
     * the task is enqueued but the timer is not yet running.             */
    ktimer_t timer;
    ktimer_init(&timer, wq_timeout_cb, &ctx);
    ktimer_start(&timer, (uint32_t)(timeout_ms > 0xFFFFFFFFULL
                                    ? 0xFFFFFFFFU
                                    : (uint32_t)timeout_ms));

    /* Add self to waitq tail */
    uint64_t f = irq_save_cli();
    wq_lock(wq);
    cur->next = NULL;
    if (!wq->head) {
        wq->head = wq->tail = cur;
    } else {
        wq->tail->next = cur;
        wq->tail       = cur;
    }
    wq_unlock(wq);
    irq_restore(f);

    /* Sleep until woken (by waitq_wake_* or by the timer callback above) */
    sched_block();

    /* Cancel the timer — if it already fired and set ctx.fired=1 this is
     * a no-op; if it has not fired yet we prevent a spurious callback.   */
    ktimer_cancel(&timer);

    /* If we were woken by waitq_wake_*, the timer callback never ran and
     * we might still be in the waitq list (though usually we've already
     * been dequeued by the waker).  Remove defensively.                  */
    if (!ctx.fired) {
        f = irq_save_cli();
        wq_lock(wq);
        task_t *prev2 = NULL;
        task_t *t2    = wq->head;
        while (t2) {
            if (t2 == cur) {
                if (prev2)             prev2->next = cur->next;
                else                   wq->head    = cur->next;
                if (wq->tail == cur)   wq->tail    = prev2;
                cur->next = NULL;
                break;
            }
            prev2 = t2;
            t2    = t2->next;
        }
        wq_unlock(wq);
        irq_restore(f);
    }

    return ctx.fired;   /* 0 = woken by data, 1 = timed out */
}
