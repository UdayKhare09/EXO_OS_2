/* lib/timer.c — Kernel software timer facility
 *
 * Simple sorted linked-list of timers.  Polled every ~1 ms from
 * the scheduler tick (APIC vector 0x20).  Keeps a spinlock to be
 * safe from concurrent start/cancel calls on different CPUs.
 */
#include "timer.h"
#include "lib/klog.h"
#include <stddef.h>

/* ── Spinlock (matches pattern used in waitq / ipc) ──────────────────────── */
static volatile int g_timer_lock = 0;

static inline void timer_lock(void) {
    while (__atomic_test_and_set(&g_timer_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
}
static inline void timer_unlock(void) {
    __atomic_clear(&g_timer_lock, __ATOMIC_RELEASE);
}

/* ── Timer list (sorted by deadline, ascending) ──────────────────────────── */
static ktimer_t *g_timer_head = NULL;

/* ── Subsystem init ──────────────────────────────────────────────────────── */
void ktimer_subsys_init(void) {
    g_timer_head = NULL;
    KLOG_INFO("ktimer: subsystem initialised\n");
}

/* ── Init a timer struct ─────────────────────────────────────────────────── */
void ktimer_init(ktimer_t *t, ktimer_cb_t cb, void *arg) {
    t->deadline = 0;
    t->callback = cb;
    t->arg      = arg;
    t->active   = false;
    t->next     = NULL;
}

/* ── Remove from list (must hold lock) ───────────────────────────────────── */
static void remove_locked(ktimer_t *t) {
    if (!t->active) return;
    ktimer_t **pp = &g_timer_head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            t->next   = NULL;
            t->active = false;
            return;
        }
        pp = &(*pp)->next;
    }
    t->active = false;
}

/* ── Insert sorted (must hold lock) ─────────────────────────────────────── */
static void insert_locked(ktimer_t *t) {
    ktimer_t **pp = &g_timer_head;
    while (*pp && (*pp)->deadline <= t->deadline)
        pp = &(*pp)->next;
    t->next   = *pp;
    *pp       = t;
    t->active = true;
}

/* ── Public API: start ───────────────────────────────────────────────────── */
void ktimer_start(ktimer_t *t, uint32_t ms) {
    /* We need the current tick.  Import from sched. */
    extern uint64_t sched_get_ticks(void);
    uint64_t now = sched_get_ticks();

    timer_lock();
    if (t->active)
        remove_locked(t);
    t->deadline = now + (uint64_t)ms;
    insert_locked(t);
    timer_unlock();
}

/* ── Public API: cancel ──────────────────────────────────────────────────── */
void ktimer_cancel(ktimer_t *t) {
    timer_lock();
    remove_locked(t);
    timer_unlock();
}

/* ── Tick handler (called from sched_tick / IRQ context) ─────────────────── */
void ktimer_tick(uint64_t current_ticks) {
    /* Fast path: nothing pending */
    if (!g_timer_head) return;

    timer_lock();

    /* Collect all expired timers (list is sorted, so we stop early) */
    while (g_timer_head && g_timer_head->deadline <= current_ticks) {
        ktimer_t *t = g_timer_head;
        g_timer_head = t->next;
        t->next   = NULL;
        t->active = false;

        /* Fire callback with lock released to avoid deadlock */
        timer_unlock();
        if (t->callback)
            t->callback(t, t->arg);
        timer_lock();
    }

    timer_unlock();
}
