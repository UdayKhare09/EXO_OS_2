/* net/tcp.c — TCP core: TCB management, ISN generation, port allocation */
#include "net/tcp.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── global TCB table ────────────────────────────────────────────────────── */
static tcp_tcb_t  g_tcbs[TCP_MAX_TCB];
static spinlock_t g_tcp_lock;
static uint16_t   g_tcp_ephemeral_next = 49152;

/* simple ISN counter — incremented per connection (not cryptographically
 * secure, but sufficient for a hobby OS) */
static volatile uint32_t g_isn_counter = 0x12345678;

static uint32_t tcp_generate_isn(void) {
    /* every new connection bumps by ~64K to reduce collisions */
    return __sync_fetch_and_add(&g_isn_counter, 64000);
}

/* ── state name ──────────────────────────────────────────────────────────── */
const char *tcp_state_name(tcp_state_t st) {
    static const char *names[] = {
        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED",
        "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
        "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
    };
    if ((unsigned)st > TCP_TIME_WAIT) return "???";
    return names[st];
}

/* ── init ────────────────────────────────────────────────────────────────── */
void tcp_init(void) {
    memset(g_tcbs, 0, sizeof(g_tcbs));
    spinlock_init(&g_tcp_lock);
    KLOG_INFO("tcp: TCP initialised (%d TCB slots)\n", TCP_MAX_TCB);
}

/* ── allocate a TCB ──────────────────────────────────────────────────────── */
tcp_tcb_t *tcp_tcb_alloc(void) {
    spinlock_acquire(&g_tcp_lock);
    for (int i = 0; i < TCP_MAX_TCB; i++) {
        if (!g_tcbs[i].active) {
            tcp_tcb_t *tcb = &g_tcbs[i];
            memset(tcb, 0, sizeof(*tcb));
            tcb->active   = 1;
            tcb->state    = TCP_CLOSED;
            tcb->rto      = 1000;     /* initial RTO = 1s (RFC 6298) */
            tcb->rcv_mss  = TCP_MSS_DEFAULT;
            tcb->snd_mss  = TCP_MSS_DEFAULT;
            tcb->rcv_wnd  = TCP_WINDOW_DEFAULT;
            tcb->iss      = tcp_generate_isn();
            spinlock_init(&tcb->lock);
            waitq_init(&tcb->wq_connect);
            waitq_init(&tcb->wq_accept);
            waitq_init(&tcb->wq_recv);
            waitq_init(&tcb->wq_send);

            /* allocate ring buffers */
            tcb->rx_buf  = kzalloc(TCP_RX_BUFSZ);
            tcb->tx_buf  = kzalloc(TCP_TX_BUFSZ);
            tcb->rx_size = TCP_RX_BUFSZ;
            tcb->tx_size = TCP_TX_BUFSZ;

            if (!tcb->rx_buf || !tcb->tx_buf) {
                if (tcb->rx_buf) kfree(tcb->rx_buf);
                if (tcb->tx_buf) kfree(tcb->tx_buf);
                tcb->active = 0;
                spinlock_release(&g_tcp_lock);
                return NULL;
            }

            tcp_timer_init(tcb);

            spinlock_release(&g_tcp_lock);
            return tcb;
        }
    }
    spinlock_release(&g_tcp_lock);
    return NULL;
}

/* ── free a TCB ──────────────────────────────────────────────────────────── */
void tcp_tcb_free(tcp_tcb_t *tcb) {
    if (!tcb) return;

    spinlock_acquire(&tcb->lock);

    /* cancel all timers */
    tcp_rexmit_timer_stop(tcb);
    ktimer_cancel(&tcb->delack_timer);
    ktimer_cancel(&tcb->timewait_timer);

    /* purge retransmit queue */
    tcp_rexmit_purge(tcb);

    /* free buffers */
    if (tcb->rx_buf) { kfree(tcb->rx_buf); tcb->rx_buf = NULL; }
    if (tcb->tx_buf) { kfree(tcb->tx_buf); tcb->tx_buf = NULL; }

    tcb->state  = TCP_CLOSED;
    tcb->active = 0;

    /* wake any waiters so they see the closed state */
    waitq_wake_all(&tcb->wq_connect);
    waitq_wake_all(&tcb->wq_accept);
    waitq_wake_all(&tcb->wq_recv);
    waitq_wake_all(&tcb->wq_send);

    spinlock_release(&tcb->lock);
}

/* ── lookup: exact 4-tuple match ─────────────────────────────────────────── */
tcp_tcb_t *tcp_tcb_lookup(uint32_t local_ip, uint16_t local_port,
                          uint32_t remote_ip, uint16_t remote_port)
{
    spinlock_acquire(&g_tcp_lock);
    for (int i = 0; i < TCP_MAX_TCB; i++) {
        tcp_tcb_t *t = &g_tcbs[i];
        if (!t->active) continue;
        if (t->local_port  == local_port  &&
            t->remote_port == remote_port &&
            (t->local_ip == 0 || t->local_ip == local_ip) &&
            t->remote_ip   == remote_ip) {
            spinlock_release(&g_tcp_lock);
            return t;
        }
    }
    spinlock_release(&g_tcp_lock);
    return NULL;
}

/* ── lookup: LISTEN socket by port ───────────────────────────────────────── */
tcp_tcb_t *tcp_tcb_lookup_listen(uint16_t local_port) {
    spinlock_acquire(&g_tcp_lock);
    for (int i = 0; i < TCP_MAX_TCB; i++) {
        tcp_tcb_t *t = &g_tcbs[i];
        if (!t->active) continue;
        if (t->state == TCP_LISTEN && t->local_port == local_port) {
            spinlock_release(&g_tcp_lock);
            return t;
        }
    }
    spinlock_release(&g_tcp_lock);
    return NULL;
}

/* ── ephemeral port allocation ───────────────────────────────────────────── */
uint16_t tcp_alloc_ephemeral(void) {
    spinlock_acquire(&g_tcp_lock);
    uint16_t start = g_tcp_ephemeral_next;
    for (;;) {
        uint16_t port = g_tcp_ephemeral_next++;
        if (g_tcp_ephemeral_next > 65535)
            g_tcp_ephemeral_next = 49152;

        int used = 0;
        for (int i = 0; i < TCP_MAX_TCB; i++) {
            if (g_tcbs[i].active && g_tcbs[i].local_port == port) {
                used = 1;
                break;
            }
        }
        if (!used) {
            spinlock_release(&g_tcp_lock);
            return port;
        }
        if (g_tcp_ephemeral_next == start) {
            spinlock_release(&g_tcp_lock);
            return 0;  /* exhausted */
        }
    }
}
