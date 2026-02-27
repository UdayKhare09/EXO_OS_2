/* net/tcp_timer.c — TCP timers: retransmit, delayed ACK, TIME_WAIT */
#include "net/tcp.h"
#include "net/ipv4.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── Constants ───────────────────────────────────────────────────────────── */
#define TCP_DELACK_MS    40      /* delayed ACK timeout */
#define TCP_TIMEWAIT_MS  60000   /* 2×MSL = 60s */

/* ── Retransmit timer callback (IRQ context) ─────────────────────────────── */
static void tcp_rexmit_timeout(ktimer_t *timer, void *arg) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)arg;
    spinlock_acquire(&tcb->lock);

    if (tcb->state == TCP_CLOSED || !tcb->active) {
        spinlock_release(&tcb->lock);
        return;
    }

    tcp_rexmit_entry_t *ent = tcb->rexmit_head;
    if (!ent) {
        spinlock_release(&tcb->lock);
        return;
    }

    (void)ent;
    KLOG_DEBUG("tcp: retransmit timer expired; aborting without retry\n");
    tcb->so_error = -1;  /* ETIMEDOUT */
    tcb->state = TCP_CLOSED;
    waitq_wake_all(&tcb->wq_connect);
    waitq_wake_all(&tcb->wq_recv);
    waitq_wake_all(&tcb->wq_send);

    spinlock_release(&tcb->lock);
}

/* ── Delayed ACK callback ────────────────────────────────────────────────── */
static void tcp_delack_timeout(ktimer_t *timer, void *arg) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)arg;
    spinlock_acquire(&tcb->lock);

    if (tcb->delack_pending && tcb->state != TCP_CLOSED) {
        tcp_send_ack(tcb);
    }

    spinlock_release(&tcb->lock);
}

/* ── TIME_WAIT callback ──────────────────────────────────────────────────── */
static void tcp_timewait_timeout(ktimer_t *timer, void *arg) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)arg;
    KLOG_DEBUG("tcp: TIME_WAIT expired → CLOSED\n");
    tcp_tcb_free(tcb);
}

/* ── Initialise timers for a new TCB ─────────────────────────────────────── */
void tcp_timer_init(tcp_tcb_t *tcb) {
    ktimer_init(&tcb->rexmit_timer,   tcp_rexmit_timeout,   tcb);
    ktimer_init(&tcb->delack_timer,   tcp_delack_timeout,   tcb);
    ktimer_init(&tcb->timewait_timer, tcp_timewait_timeout,  tcb);
}

/* ── Public timer controls ───────────────────────────────────────────────── */
void tcp_rexmit_timer_reset(tcp_tcb_t *tcb) {
    ktimer_start(&tcb->rexmit_timer, tcb->rto);
}

void tcp_rexmit_timer_stop(tcp_tcb_t *tcb) {
    ktimer_cancel(&tcb->rexmit_timer);
}

void tcp_delack_schedule(tcp_tcb_t *tcb) {
    if (!tcb->delack_pending) {
        tcb->delack_pending = 1;
        ktimer_start(&tcb->delack_timer, TCP_DELACK_MS);
    }
}

void tcp_timewait_start(tcp_tcb_t *tcb) {
    ktimer_start(&tcb->timewait_timer, TCP_TIMEWAIT_MS);
}
