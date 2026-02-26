/* lib/timer.h — Kernel software timer facility
 *
 * Callback-based timers driven by the APIC scheduler tick (~1 ms).
 * The timer subsystem is polled from sched_tick() context.
 *
 * Usage:
 *   ktimer_t tmr;
 *   ktimer_init(&tmr, my_callback, my_arg);
 *   ktimer_start(&tmr, 5000);   // fire after 5000 ms
 *   ktimer_cancel(&tmr);        // cancel before it fires
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct ktimer ktimer_t;
typedef void (*ktimer_cb_t)(ktimer_t *timer, void *arg);

struct ktimer {
    uint64_t     deadline;    /* absolute jiffies tick when timer fires      */
    ktimer_cb_t  callback;    /* function to call (in IRQ context!)          */
    void        *arg;         /* user argument                               */
    bool         active;      /* is this timer in the active list?           */
    ktimer_t    *next;        /* linked list (internal)                      */
};

/* Initialise a timer structure (does not start it) */
void ktimer_init(ktimer_t *t, ktimer_cb_t cb, void *arg);

/* Start/restart a timer to fire after `ms` milliseconds from now.
 * If already active, it is re-armed with the new deadline.
 * Safe to call from any context. */
void ktimer_start(ktimer_t *t, uint32_t ms);

/* Cancel a pending timer. No-op if not active.
 * Safe to call from any context. */
void ktimer_cancel(ktimer_t *t);

/* Check if a timer is active (pending, not yet fired) */
static inline bool ktimer_active(const ktimer_t *t) { return t->active; }

/* Called from scheduler tick (IRQ context, ~1 ms).
 * Fires all expired timers. Must be called with jiffies updated. */
void ktimer_tick(uint64_t current_ticks);

/* Initialise the timer subsystem (call once at boot) */
void ktimer_subsys_init(void);
