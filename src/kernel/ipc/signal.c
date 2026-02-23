/*
 * signal.c — per-task signal delivery for EXO_OS
 *
 * Design:
 *   • A task has a volatile `sig_pending` bitmask and a `sig_handlers[]` table.
 *   • signal_send() atomically ORs the bit and calls sched_unblock() so a
 *     sleeping task wakes up to dispatch the signal.
 *   • signal_dispatch() is called in *task context* (not ISR) so handlers can
 *     safely call KLOG_INFO, acquire locks, etc.
 *   • SIGKILL alone is also handled by the scheduler (marks task DEAD before
 *     it runs, so the task never executes another instruction).
 */
#include "signal.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include <stddef.h>

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

sig_handler_t *signal_table_alloc(void) {
    sig_handler_t *tbl = kmalloc(NSIGS * sizeof(sig_handler_t));
    if (!tbl) return NULL;
    for (int i = 0; i < NSIGS; i++) tbl[i] = SIG_DFL;
    return tbl;
}

void signal_table_free(sig_handler_t *tbl) {
    kfree(tbl);
}

/* ── Handler management ──────────────────────────────────────────────────── */

void signal_set(task_t *t, int sig, sig_handler_t handler) {
    if (!t || sig <= 0 || sig >= NSIGS) return;
    if (sig == SIGKILL) return;   /* uncatchable */
    if (!t->sig_handlers) return;
    t->sig_handlers[sig] = handler;
}

sig_handler_t signal_get(task_t *t, int sig) {
    if (!t || sig <= 0 || sig >= NSIGS || !t->sig_handlers) return SIG_DFL;
    return t->sig_handlers[sig];
}

/* ── Delivery ────────────────────────────────────────────────────────────── */

void signal_send(task_t *t, int sig) {
    if (!t || sig <= 0 || sig >= NSIGS) return;
    /* Atomically set the pending bit */
    __atomic_or_fetch(&t->sig_pending, (1u << sig), __ATOMIC_SEQ_CST);
    /* Wake the task so it dispatches the signal from task context */
    sched_unblock(t);
}

void signal_dispatch(task_t *t) {
    if (!t) return;

    /* Atomically snapshot and clear all pending bits */
    uint32_t pending = __atomic_exchange_n(&t->sig_pending, 0u, __ATOMIC_SEQ_CST);
    if (!pending) return;

    for (int sig = 1; sig < NSIGS; sig++) {
        if (!(pending & (1u << sig))) continue;

        /* SIGKILL: mark dead — scheduler will clean up before next run */
        if (sig == SIGKILL) {
            t->state = TASK_DEAD;
            return;
        }

        if (!t->sig_handlers) continue;
        sig_handler_t h = t->sig_handlers[sig];
        if (h == SIG_DFL || h == SIG_IGN) continue;
        h(sig);   /* call handler in current (task) context */
    }
}
