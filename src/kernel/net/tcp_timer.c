/* net/tcp_timer.c — TCP timers: retransmit, delayed ACK, TIME_WAIT */
#include "net/tcp.h"
#include "net/ipv4.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── Constants ───────────────────────────────────────────────────────────── */
#define TCP_RTO_MIN      200     /* minimum RTO (ms) */
#define TCP_RTO_MAX      60000   /* maximum RTO (ms) — RFC 6298 */
#define TCP_DELACK_MS    40      /* delayed ACK timeout */
#define TCP_TIMEWAIT_MS  60000   /* 2×MSL = 60s */
#define TCP_MAX_REXMIT   8       /* max retransmissions before abort */

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

    if (ent->retries >= TCP_MAX_REXMIT) {
        KLOG_ERR("tcp: max retransmits reached, aborting connection\n");
        tcb->so_error = -1;  /* ETIMEDOUT */
        tcb->state = TCP_CLOSED;
        waitq_wake_all(&tcb->wq_connect);
        waitq_wake_all(&tcb->wq_recv);
        waitq_wake_all(&tcb->wq_send);
        spinlock_release(&tcb->lock);
        return;
    }

    /* retransmit the first unacknowledged segment */
    KLOG_DEBUG("tcp: retransmit seq=%u (retry %u, rto=%u ms)\n",
         ent->seq_start, ent->retries + 1, tcb->rto);

    /* build and send the segment again */
    size_t hdr_len = sizeof(tcp_header_t);
    int syn_opts = (ent->flags & TCP_SYN) ? 4 : 0;
    size_t seg_len = hdr_len + syn_opts + ent->data_len;

    skbuff_t *skb = skb_alloc(seg_len + 128);
    if (skb) {
        skb_reserve(skb, 128);
        void *buf = skb_put(skb, seg_len);
        memset(buf, 0, hdr_len + syn_opts);

        tcp_header_t *hdr = (tcp_header_t *)buf;
        hdr->src_port  = htons(tcb->local_port);
        hdr->dst_port  = htons(tcb->remote_port);
        hdr->seq       = htonl(ent->seq_start);
        hdr->ack       = htonl(tcb->rcv_nxt);
        hdr->data_off  = TCP_DATA_OFF(hdr_len + syn_opts);
        hdr->flags     = ent->flags;
        if (!(ent->flags & TCP_SYN) || (ent->flags & TCP_ACK))
            hdr->flags |= TCP_ACK;
        hdr->window    = htons((uint16_t)(tcb->rcv_wnd < 65535 ?
                                          tcb->rcv_wnd : 65535));
        hdr->checksum  = 0;

        /* MSS option for SYN retransmits */
        if (syn_opts) {
            uint8_t *opts = (uint8_t *)buf + hdr_len;
            opts[0] = 2;
            opts[1] = 4;
            opts[2] = (tcb->rcv_mss >> 8) & 0xFF;
            opts[3] = tcb->rcv_mss & 0xFF;
        }

        if (ent->data && ent->data_len > 0)
            memcpy((uint8_t *)buf + hdr_len + syn_opts,
                   ent->data, ent->data_len);

        hdr->checksum = tcp_checksum(tcb->local_ip, tcb->remote_ip,
                                     buf, seg_len);
        skb->protocol = IPPROTO_TCP;
        ip_tx(tcb->dev, skb, tcb->local_ip, tcb->remote_ip, IPPROTO_TCP);
        skb_free(skb);
    }

    ent->retries++;

    /* exponential backoff (RFC 6298 §5.5) */
    tcb->rto *= 2;
    if (tcb->rto > TCP_RTO_MAX) tcb->rto = TCP_RTO_MAX;

    /* restart timer */
    ktimer_start(&tcb->rexmit_timer, tcb->rto);

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
