/* net/tcp_input.c — TCP segment receive processing (RFC 793) */
#include "net/tcp.h"
#include "net/socket.h"   /* socket_t, wq_rx — woken when data/FIN/RST arrives */
#include "net/ipv4.h"
#include "net/netutil.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"

/* ── data delivery into RX ring buffer ───────────────────────────────────── */
static void tcp_deliver_data(tcp_tcb_t *tcb, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint32_t next_head = (tcb->rx_head + 1) % tcb->rx_size;
        if (next_head == tcb->rx_tail) {
            /* buffer full — drop remaining */
            KLOG_WARN("tcp: rx buffer full, dropping %u bytes\n",
                 (unsigned)(len - i));
            break;
        }
        tcb->rx_buf[tcb->rx_head] = data[i];
        tcb->rx_head = next_head;
    }
    tcb->rcv_nxt += (uint32_t)len;
    tcb->rcv_wnd  = tcp_rx_free(tcb);

    /* wake recv() waiters */
    waitq_wake_all(&tcb->wq_recv);

    /* wake any task blocked in poll()/select() on this socket */
    if (tcb->socket)
        waitq_wake_all(&((socket_t *)tcb->socket)->wq_rx);
}

/* ── push completed child connection to listener accept queue ────────────── */
static void tcp_accept_enqueue(tcp_tcb_t *listener, tcp_tcb_t *child) {
    int next = (listener->accept_head + 1) % SOMAXCONN;
    if (next == listener->accept_tail) {
        KLOG_WARN("tcp: accept queue full, dropping connection\n");
        tcp_tcb_free(child);
        return;
    }
    listener->accept_queue[listener->accept_head] = child;
    listener->accept_head = next;
    waitq_wake_one(&listener->wq_accept);
}

/* ── handle incoming SYN on a LISTEN socket ──────────────────────────────── */
static void tcp_handle_syn(netdev_t *dev, tcp_tcb_t *listener,
                           tcp_header_t *hdr, uint32_t src_ip,
                           uint16_t src_port, size_t seg_data_len)
{
    (void)seg_data_len;

    tcp_tcb_t *child = tcp_tcb_alloc();
    if (!child) {
        KLOG_ERR("tcp: no free TCB for incoming SYN\n");
        return;
    }

    child->local_ip    = dev->ip_addr;
    child->local_port  = listener->local_port;
    child->remote_ip   = src_ip;
    child->remote_port = src_port;
    child->dev         = dev;
    child->listener    = listener;

    child->irs     = ntohl(hdr->seq);
    child->rcv_nxt = child->irs + 1;
    child->snd_nxt = child->iss;
    child->snd_una = child->iss;
    child->snd_wnd = ntohs(hdr->window);

    /* parse MSS option from SYN */
    size_t hdr_len = TCP_HDR_LEN(hdr);
    if (hdr_len > sizeof(tcp_header_t)) {
        const uint8_t *opts = (const uint8_t *)hdr + sizeof(tcp_header_t);
        size_t opts_len = hdr_len - sizeof(tcp_header_t);
        for (size_t i = 0; i < opts_len; ) {
            if (opts[i] == 0) break;       /* end of options */
            if (opts[i] == 1) { i++; continue; }  /* NOP */
            if (i + 1 >= opts_len) break;
            uint8_t kind = opts[i];
            uint8_t olen = opts[i + 1];
            if (olen < 2 || i + olen > opts_len) break;
            if (kind == 2 && olen == 4) {
                child->snd_mss = (opts[i+2] << 8) | opts[i+3];
                if (child->snd_mss < 536) child->snd_mss = 536;
            }
            i += olen;
        }
    }

    child->state = TCP_SYN_RECEIVED;

    /* send SYN+ACK */
    tcp_send_segment(child, TCP_SYN | TCP_ACK, NULL, 0);
    tcp_rexmit_timer_reset(child);

    KLOG_DEBUG("tcp: SYN from %d.%d.%d.%d:%u → SYN_RECEIVED\n",
         IP4_A(src_ip), IP4_B(src_ip),
         IP4_C(src_ip), IP4_D(src_ip), src_port);
}

/* ── main TCP RX path — called from ip_rx() ──────────────────────────────── */
void tcp_rx(netdev_t *dev, skbuff_t *skb) {
    if (skb->len < sizeof(tcp_header_t)) {
        skb_free(skb);
        return;
    }

    tcp_header_t *hdr = (tcp_header_t *)skb->data;
    size_t hdr_len = TCP_HDR_LEN(hdr);
    if (hdr_len < sizeof(tcp_header_t) || hdr_len > skb->len) {
        skb_free(skb);
        return;
    }

    /* verify checksum */
    uint16_t ck = tcp_checksum(skb->src_ip, skb->dst_ip,
                               skb->data, skb->len);
    if (ck != 0) {
        KLOG_WARN("tcp: bad checksum, dropping\n");
        skb_free(skb);
        return;
    }

    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seg_seq  = ntohl(hdr->seq);
    uint32_t seg_ack  = ntohl(hdr->ack);
    uint8_t  flags    = hdr->flags;

    const uint8_t *seg_data = (const uint8_t *)skb->data + hdr_len;
    size_t seg_data_len     = skb->len - hdr_len;

    KLOG_DEBUG(
         "tcp: RX %s%s%s%s from %d.%d.%d.%d:%u seq=%u ack=%u len=%u\n",
         (flags & TCP_SYN) ? "S" : "",
         (flags & TCP_ACK) ? "A" : "",
         (flags & TCP_FIN) ? "F" : "",
         (flags & TCP_RST) ? "R" : "",
         IP4_A(skb->src_ip), IP4_B(skb->src_ip),
         IP4_C(skb->src_ip), IP4_D(skb->src_ip),
         src_port, seg_seq, seg_ack, (unsigned)seg_data_len);

    /* ── lookup TCB: exact 4-tuple first, then LISTEN ────────────────────── */
    uint32_t local_ip = dev->ip_addr;
    tcp_tcb_t *tcb = tcp_tcb_lookup(local_ip, dst_port,
                                    skb->src_ip, src_port);

    if (!tcb) {
        /* check for LISTEN socket */
        tcp_tcb_t *listener = tcp_tcb_lookup_listen(dst_port);
        if (listener && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcp_handle_syn(dev, listener, hdr, skb->src_ip,
                           src_port, seg_data_len);
            skb_free(skb);
            return;
        }

        /* no TCB and no listener — send RST */
        if (!(flags & TCP_RST)) {
            uint32_t rst_seq = 0, rst_ack = 0;
            if (flags & TCP_ACK) {
                rst_seq = seg_ack;
            } else {
                uint32_t seg_len = seg_data_len;
                if (flags & TCP_SYN) seg_len++;
                if (flags & TCP_FIN) seg_len++;
                rst_ack = seg_seq + seg_len;
            }
            tcp_send_rst(dev, skb->src_ip, dst_port, src_port,
                         rst_seq, rst_ack);
        }
        skb_free(skb);
        return;
    }

    /* ── per-state processing ────────────────────────────────────────────── */
    spinlock_acquire(&tcb->lock);

    /* RST handling (all states) */
    if (flags & TCP_RST) {
        KLOG_INFO("tcp: RST received in state %s\n",
             tcp_state_name(tcb->state));
        tcb->so_error = -ECONNRESET;
        tcb->state = TCP_CLOSED;
        waitq_wake_all(&tcb->wq_connect);
        waitq_wake_all(&tcb->wq_recv);
        waitq_wake_all(&tcb->wq_send);
        /* also wake any poll() waiter so it can report POLLERR/POLLHUP */
        if (tcb->socket)
            waitq_wake_all(&((socket_t *)tcb->socket)->wq_rx);
        spinlock_release(&tcb->lock);
        skb_free(skb);
        return;
    }

    switch (tcb->state) {
    case TCP_SYN_SENT:
        /* expecting SYN+ACK */
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (seg_ack != tcb->snd_nxt) {
                /* bad ACK — send RST */
                spinlock_release(&tcb->lock);
                tcp_send_rst(dev, skb->src_ip, dst_port, src_port,
                             seg_ack, 0);
                skb_free(skb);
                return;
            }
            tcb->irs     = seg_seq;
            tcb->rcv_nxt = seg_seq + 1;
            tcb->snd_una = seg_ack;
            tcb->snd_wnd = ntohs(hdr->window);

            /* parse MSS from SYN+ACK options */
            if (hdr_len > sizeof(tcp_header_t)) {
                const uint8_t *opts = (const uint8_t *)hdr + sizeof(tcp_header_t);
                size_t opts_len = hdr_len - sizeof(tcp_header_t);
                for (size_t i = 0; i < opts_len; ) {
                    if (opts[i] == 0) break;
                    if (opts[i] == 1) { i++; continue; }
                    if (i + 1 >= opts_len) break;
                    uint8_t olen = opts[i + 1];
                    if (olen < 2 || i + olen > opts_len) break;
                    if (opts[i] == 2 && olen == 4) {
                        tcb->snd_mss = (opts[i+2] << 8) | opts[i+3];
                        if (tcb->snd_mss < 536) tcb->snd_mss = 536;
                    }
                    i += olen;
                }
            }

            /* retransmit queue: SYN is acked */
            tcp_rexmit_ack(tcb, seg_ack);

            tcb->state = TCP_ESTABLISHED;
            KLOG_INFO("tcp: ESTABLISHED with %d.%d.%d.%d:%u\n",
                 IP4_A(tcb->remote_ip), IP4_B(tcb->remote_ip),
                 IP4_C(tcb->remote_ip), IP4_D(tcb->remote_ip),
                 tcb->remote_port);

            /* send ACK for SYN+ACK */
            tcp_send_ack(tcb);
            waitq_wake_all(&tcb->wq_connect);
        }
        break;

    case TCP_SYN_RECEIVED:
        if (flags & TCP_ACK) {
            if (seg_ack == tcb->snd_nxt) {
                tcb->snd_una = seg_ack;
                tcp_rexmit_ack(tcb, seg_ack);
                tcb->state = TCP_ESTABLISHED;
                KLOG_INFO(
                     "tcp: ESTABLISHED (passive) with %d.%d.%d.%d:%u\n",
                     IP4_A(tcb->remote_ip), IP4_B(tcb->remote_ip),
                     IP4_C(tcb->remote_ip), IP4_D(tcb->remote_ip),
                     tcb->remote_port);

                /* enqueue to parent listener's accept queue */
                if (tcb->listener)
                    tcp_accept_enqueue(tcb->listener, tcb);
            }
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT: {
        /* ── ACK processing ──────────────────────────────────────────── */
        if (flags & TCP_ACK) {
            if (tcp_seq_gt(seg_ack, tcb->snd_una) &&
                tcp_seq_le(seg_ack, tcb->snd_nxt)) {
                tcb->snd_una = seg_ack;
                tcp_rexmit_ack(tcb, seg_ack);
                tcb->snd_wnd = ntohs(hdr->window);
                waitq_wake_all(&tcb->wq_send);
            }
        }

        /* state transitions on ACK */
        if (tcb->state == TCP_FIN_WAIT_1 &&
            tcb->snd_una == tcb->snd_nxt) {
            tcb->state = TCP_FIN_WAIT_2;
            KLOG_DEBUG("tcp: → FIN_WAIT_2\n");
        }

        /* ── segment sequence validation (simplified) ────────────────── */
        if (seg_data_len > 0 || (flags & TCP_FIN)) {
            if (tcp_seq_lt(seg_seq, tcb->rcv_nxt)) {
                /* old data or duplicate — ACK and drop */
                tcp_send_ack(tcb);
                spinlock_release(&tcb->lock);
                skb_free(skb);
                return;
            }
            if (seg_seq != tcb->rcv_nxt) {
                /* out of order — ACK and drop (no reordering buffer) */
                tcp_send_ack(tcb);
                spinlock_release(&tcb->lock);
                skb_free(skb);
                return;
            }
        }

        /* ── data delivery ───────────────────────────────────────────── */
        if (seg_data_len > 0) {
            tcp_deliver_data(tcb, seg_data, seg_data_len);
            tcp_delack_schedule(tcb);
        }

        /* ── FIN processing ──────────────────────────────────────────── */
        if (flags & TCP_FIN) {
            tcb->rcv_nxt++;
            tcb->fin_received = 1;
            tcp_send_ack(tcb);

            switch (tcb->state) {
            case TCP_ESTABLISHED:
                tcb->state = TCP_CLOSE_WAIT;
                KLOG_DEBUG("tcp: → CLOSE_WAIT\n");
                waitq_wake_all(&tcb->wq_recv);  /* EOF for reader */
                /* wake poll() waiter — FIN sets POLLIN+POLLHUP */
                if (tcb->socket)
                    waitq_wake_all(&((socket_t *)tcb->socket)->wq_rx);
                break;
            case TCP_FIN_WAIT_1:
                /* simultaneous close */
                tcb->state = TCP_CLOSING;
                KLOG_DEBUG("tcp: → CLOSING\n");
                break;
            case TCP_FIN_WAIT_2:
                tcb->state = TCP_TIME_WAIT;
                KLOG_DEBUG("tcp: → TIME_WAIT\n");
                tcp_timewait_start(tcb);
                break;
            default:
                break;
            }
        }
        break;
    }

    case TCP_CLOSING:
        if (flags & TCP_ACK) {
            if (tcb->snd_una == tcb->snd_nxt) {
                tcb->state = TCP_TIME_WAIT;
                KLOG_DEBUG("tcp: → TIME_WAIT\n");
                tcp_timewait_start(tcb);
            }
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            if (tcb->snd_una == tcb->snd_nxt) {
                KLOG_DEBUG("tcp: LAST_ACK done → CLOSED\n");
                tcb->state = TCP_CLOSED;
                spinlock_release(&tcb->lock);
                tcp_tcb_free(tcb);
                skb_free(skb);
                return;
            }
        }
        break;

    case TCP_TIME_WAIT:
        /* ACK any segment to keep peer happy, restart 2MSL timer */
        tcp_send_ack(tcb);
        tcp_timewait_start(tcb);
        break;

    default:
        break;
    }

    spinlock_release(&tcb->lock);
    skb_free(skb);
}
