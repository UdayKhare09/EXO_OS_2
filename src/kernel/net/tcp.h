/* net/tcp.h — TCP (RFC 793 / 7414) — state machine, TCB, API
 *
 * Layout:
 *   tcp.h        — shared structures and declarations
 *   tcp.c        — TCB management, state machine core, port binding
 *   tcp_input.c  — segment processing (SYN, ACK, FIN, data)
 *   tcp_output.c — segment transmission, retransmit queue
 *   tcp_timer.c  — retransmit, delayed-ACK, TIME_WAIT, keepalive
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net/skbuff.h"
#include "net/netutil.h"
#include "net/socket_defs.h"
#include "drivers/net/netdev.h"
#include "sched/waitq.h"
#include "lib/timer.h"
#include "lib/spinlock.h"

/* ── TCP header (RFC 793 §3.1) ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;    /* upper 4 bits = header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

/* TCP flags */
#define TCP_FIN   0x01
#define TCP_SYN   0x02
#define TCP_RST   0x04
#define TCP_PSH   0x08
#define TCP_ACK   0x10
#define TCP_URG   0x20

/* header length helpers */
#define TCP_HDR_LEN(h)    (((h)->data_off >> 4) * 4)
#define TCP_DATA_OFF(len) (((len) / 4) << 4)

/* ── TCP states (RFC 793 §3.2) ───────────────────────────────────────────── */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

/* ── Retransmit queue entry ──────────────────────────────────────────────── */
typedef struct tcp_rexmit_entry {
    struct tcp_rexmit_entry *next;
    uint32_t   seq_start;     /* start sequence number */
    uint32_t   seq_end;       /* end sequence number (exclusive) */
    uint8_t   *data;          /* payload copy */
    size_t     data_len;
    uint8_t    flags;         /* TCP flags originally sent */
    uint8_t    retries;       /* number of retransmissions so far */
    uint64_t   tx_tick;       /* tick when last sent */
} tcp_rexmit_entry_t;

/* ── TCP Control Block (TCB) ─────────────────────────────────────────────── */
#define TCP_MAX_TCB     128
#define TCP_RX_BUFSZ    (64 * 1024)
#define TCP_TX_BUFSZ    (64 * 1024)
#define TCP_MSS_DEFAULT 1460
#define TCP_WINDOW_DEFAULT (TCP_RX_BUFSZ)

typedef struct tcp_tcb {
    /* identity — 4-tuple */
    uint32_t    local_ip;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;

    /* state machine */
    tcp_state_t state;
    spinlock_t  lock;

    /* send sequence space */
    uint32_t    snd_una;      /* oldest unacknowledged */
    uint32_t    snd_nxt;      /* next to send */
    uint32_t    snd_wnd;      /* peer's advertised window */
    uint32_t    iss;          /* initial send sequence */

    /* receive sequence space */
    uint32_t    rcv_nxt;      /* next expected */
    uint32_t    rcv_wnd;      /* our advertised window */
    uint32_t    irs;          /* initial receive sequence */

    /* MSS */
    uint16_t    snd_mss;      /* peer's MSS (from SYN) */
    uint16_t    rcv_mss;      /* our MSS to advertise */

    /* receive ring buffer */
    uint8_t    *rx_buf;
    uint32_t    rx_head;      /* write position */
    uint32_t    rx_tail;      /* read position */
    uint32_t    rx_size;      /* buffer capacity */

    /* send buffer */
    uint8_t    *tx_buf;
    uint32_t    tx_head;      /* write position */
    uint32_t    tx_tail;      /* read position (= snd_una mapped) */
    uint32_t    tx_size;      /* buffer capacity */

    /* retransmit queue */
    tcp_rexmit_entry_t *rexmit_head;

    /* timers */
    ktimer_t    rexmit_timer;     /* retransmit timer */
    ktimer_t    delack_timer;     /* delayed ACK timer */
    ktimer_t    timewait_timer;   /* TIME_WAIT 2MSL timer */
    uint32_t    rto;              /* current retransmit timeout (ms) */
    uint32_t    srtt;             /* smoothed RTT (ms << 3) */
    uint32_t    rttvar;           /* RTT variance (ms << 2) */

    /* wait queues */
    waitq_t     wq_connect;       /* connect() blocks here */
    waitq_t     wq_accept;        /* accept() blocks here */
    waitq_t     wq_recv;          /* recv() blocks here */
    waitq_t     wq_send;          /* send() blocks when window full */

    /* listen backlog */
    struct tcp_tcb *accept_queue[SOMAXCONN];
    int         accept_head;
    int         accept_tail;
    int         backlog;

    /* parent listener (for SYN_RECEIVED connections) */
    struct tcp_tcb *listener;

    /* associated netdev */
    netdev_t   *dev;

    /* flags */
    uint8_t     fin_sent : 1;
    uint8_t     fin_received : 1;
    uint8_t     delack_pending : 1;
    uint8_t     user_closed : 1;

    /* error */
    int         so_error;

    /* socket reference (opaque, set by socket layer) */
    void       *socket;

    /* active flag */
    int         active;
} tcp_tcb_t;

/* ── TCP pseudo-header for checksum ──────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t src;
    uint32_t dst;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} tcp_pseudo_t;

/* ── tcp.c — core ────────────────────────────────────────────────────────── */
void        tcp_init(void);
tcp_tcb_t  *tcp_tcb_alloc(void);
void        tcp_tcb_free(tcp_tcb_t *tcb);
tcp_tcb_t  *tcp_tcb_lookup(uint32_t local_ip, uint16_t local_port,
                           uint32_t remote_ip, uint16_t remote_port);
tcp_tcb_t  *tcp_tcb_lookup_listen(uint16_t local_port);
uint16_t    tcp_alloc_ephemeral(void);
const char *tcp_state_name(tcp_state_t st);

/* ── tcp_input.c — receive path ──────────────────────────────────────────── */
void tcp_rx(netdev_t *dev, skbuff_t *skb);

/* ── tcp_output.c — transmit path ────────────────────────────────────────── */
int  tcp_send_segment(tcp_tcb_t *tcb, uint8_t flags,
                      const void *data, size_t len);
int  tcp_send_rst(netdev_t *dev, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint32_t seq, uint32_t ack);
void tcp_send_ack(tcp_tcb_t *tcb);
int  tcp_output(tcp_tcb_t *tcb);  /* flush TX buffer */
void tcp_rexmit_enqueue(tcp_tcb_t *tcb, uint32_t seq,
                        const void *data, size_t len, uint8_t flags);
void tcp_rexmit_ack(tcp_tcb_t *tcb, uint32_t ack);
void tcp_rexmit_purge(tcp_tcb_t *tcb);

uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                      const void *seg, size_t len);

/* ── tcp_timer.c — timer callbacks ───────────────────────────────────────── */
void tcp_timer_init(tcp_tcb_t *tcb);
void tcp_rexmit_timer_reset(tcp_tcb_t *tcb);
void tcp_rexmit_timer_stop(tcp_tcb_t *tcb);
void tcp_delack_schedule(tcp_tcb_t *tcb);
void tcp_timewait_start(tcp_tcb_t *tcb);

/* ── sequence number helpers ─────────────────────────────────────────────── */
static inline int32_t tcp_seq_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}
/* a < b  in sequence space */
static inline bool tcp_seq_lt(uint32_t a, uint32_t b) {
    return tcp_seq_diff(a, b) < 0;
}
/* a <= b */
static inline bool tcp_seq_le(uint32_t a, uint32_t b) {
    return tcp_seq_diff(a, b) <= 0;
}
/* a > b */
static inline bool tcp_seq_gt(uint32_t a, uint32_t b) {
    return tcp_seq_diff(a, b) > 0;
}
/* a >= b */
static inline bool tcp_seq_ge(uint32_t a, uint32_t b) {
    return tcp_seq_diff(a, b) >= 0;
}

/* ring buffer byte count */
static inline uint32_t tcp_rx_available(tcp_tcb_t *tcb) {
    return (tcb->rx_head - tcb->rx_tail) % tcb->rx_size;
}
static inline uint32_t tcp_rx_free(tcp_tcb_t *tcb) {
    return tcb->rx_size - 1 - tcp_rx_available(tcb);
}
