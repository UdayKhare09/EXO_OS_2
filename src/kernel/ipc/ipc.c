/*
 * ipc.c — message-passing IPC for EXO_OS
 *
 * Each task owns an `ipc_mailbox_t` (a ring-buffer of `IPC_QUEUE_CAP` messages
 * protected by a spinlock).
 *
 * ipc_send():
 *   Looks up the destination task by TID, locks its mailbox, enqueues the
 *   message, then calls sched_unblock() to wake a sleeping receiver.
 *
 * ipc_recv():
 *   Tries to dequeue a message.  If the mailbox is empty it calls sched_block()
 *   to yield the CPU.  When it is woken up (by ipc_send or signal_send) it
 *   dispatches any pending signals first — handlers run here in task context —
 *   then loops back to check for a message.
 *
 * Cross-CPU safety:
 *   The mailbox spinlock protects the ring-buffer from concurrent access.
 *   sched_unblock() is protected by the per-CPU run-queue spinlock inside
 *   sched.c.
 */
#include "ipc.h"
#include "signal.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include <stddef.h>
#include <stdint.h>

/* ── Mailbox definition ──────────────────────────────────────────────────── */

struct ipc_mailbox {
    ipc_msg_t    buf[IPC_QUEUE_CAP];   /* ring buffer                         */
    uint32_t     head;                 /* next read index                     */
    uint32_t     tail;                 /* next write index                    */
    uint32_t     count;                /* number of messages currently stored */
    volatile int lock;                 /* spinlock                            */
    task_t      *owner;               /* task that owns this mailbox         */
};

/* ── Spinlock helpers ────────────────────────────────────────────────────── */

static inline void mb_lock(struct ipc_mailbox *mb) {
    while (__atomic_test_and_set(&mb->lock, __ATOMIC_ACQUIRE));
}
static inline void mb_unlock(struct ipc_mailbox *mb) {
    __atomic_clear(&mb->lock, __ATOMIC_RELEASE);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

struct ipc_mailbox *ipc_mailbox_create(task_t *owner) {
    struct ipc_mailbox *mb = kmalloc(sizeof(struct ipc_mailbox));
    if (!mb) return NULL;
    /* zero it out field by field (no memset dependency) */
    for (size_t i = 0; i < sizeof(*mb); i++) ((char *)mb)[i] = 0;
    mb->owner = owner;
    return mb;
}

void ipc_mailbox_destroy(struct ipc_mailbox *mb) {
    kfree(mb);
}

/* ── Send ────────────────────────────────────────────────────────────────── */

int ipc_send(uint32_t dest_tid, const ipc_msg_t *msg) {
    task_t *dest = task_lookup(dest_tid);
    if (!dest || dest->state == TASK_DEAD) return -1;

    struct ipc_mailbox *mb = dest->mailbox;
    if (!mb) return -1;

    mb_lock(mb);
    if (mb->count >= IPC_QUEUE_CAP) {
        mb_unlock(mb);
        KLOG_WARN("ipc: mailbox full for tid=%u (dropped msg)\n", dest_tid);
        return -1;
    }
    mb->buf[mb->tail] = *msg;
    mb->tail = (mb->tail + 1) % IPC_QUEUE_CAP;
    mb->count++;
    mb_unlock(mb);

    /* Wake the receiver if it's blocked waiting for a message */
    sched_unblock(dest);
    return 0;
}

/* ── Receive (blocking) ──────────────────────────────────────────────────── */

int ipc_recv(ipc_msg_t *out) {
    task_t *cur = sched_current();
    if (!cur || !cur->mailbox) return -1;
    struct ipc_mailbox *mb = cur->mailbox;

    for (;;) {
        /* Try to dequeue */
        mb_lock(mb);
        if (mb->count > 0) {
            *out = mb->buf[mb->head];
            mb->head = (mb->head + 1) % IPC_QUEUE_CAP;
            mb->count--;
            mb_unlock(mb);
            return 0;
        }
        mb_unlock(mb);

        /* Check for pending signals before sleeping — handle them now */
        if (cur->sig_pending) {
            signal_dispatch(cur);   /* runs handlers in task context */
            return -1;              /* tell caller: interrupted */
        }

        /* Sleep until woken by ipc_send or signal_send */
        sched_block();

        /* We were woken — deliver any pending signals before re-checking */
        if (cur->sig_pending) {
            signal_dispatch(cur);
            return -1;
        }
    }
}

/* ── Receive (non-blocking) ──────────────────────────────────────────────── */

int ipc_try_recv(ipc_msg_t *out) {
    task_t *cur = sched_current();
    if (!cur || !cur->mailbox) return -1;
    struct ipc_mailbox *mb = cur->mailbox;

    mb_lock(mb);
    if (mb->count == 0) { mb_unlock(mb); return -1; }
    *out = mb->buf[mb->head];
    mb->head = (mb->head + 1) % IPC_QUEUE_CAP;
    mb->count--;
    mb_unlock(mb);
    return 0;
}
