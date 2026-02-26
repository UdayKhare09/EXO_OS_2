/* net/tcp_output.c — TCP segment transmission + retransmit queue */
#include "net/tcp.h"
#include "net/ipv4.h"
#include "net/netutil.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── TCP checksum (pseudo-header + segment) ──────────────────────────────── */
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                      const void *seg, size_t len)
{
    tcp_pseudo_t ph;
    ph.src      = src_ip;   /* already network byte order */
    ph.dst      = dst_ip;   /* already network byte order */
    ph.zero     = 0;
    ph.protocol = IPPROTO_TCP;
    ph.tcp_len  = htons((uint16_t)len);

    uint32_t sum = inet_checksum_partial(&ph, sizeof(ph));
    sum += inet_checksum_partial(seg, len);
    return inet_checksum_fold(sum);
}

/* ── Send a TCP segment with optional data ───────────────────────────────── */
int tcp_send_segment(tcp_tcb_t *tcb, uint8_t flags,
                     const void *data, size_t len)
{
    size_t hdr_len  = sizeof(tcp_header_t);
    /* add MSS option on SYN */
    int syn_opts = (flags & TCP_SYN) ? 4 : 0;
    size_t opts_len = syn_opts;
    size_t seg_len  = hdr_len + opts_len + len;

    skbuff_t *skb = skb_alloc(seg_len + 128);
    if (!skb) return -1;

    skb_reserve(skb, 128);   /* headroom for IP + ETH */

    void *buf = skb_put(skb, seg_len);
    memset(buf, 0, hdr_len + opts_len);

    tcp_header_t *hdr = (tcp_header_t *)buf;
    hdr->src_port  = htons(tcb->local_port);
    hdr->dst_port  = htons(tcb->remote_port);
    hdr->seq       = htonl(tcb->snd_nxt);
    hdr->ack       = htonl(tcb->rcv_nxt);
    hdr->data_off  = TCP_DATA_OFF(hdr_len + opts_len);
    hdr->flags     = flags;
    hdr->window    = htons((uint16_t)(tcb->rcv_wnd < 65535 ?
                                      tcb->rcv_wnd : 65535));
    hdr->checksum  = 0;
    hdr->urgent_ptr = 0;

    /* MSS option (kind=2, length=4, mss value in big-endian) */
    if (syn_opts) {
        uint8_t *opts = (uint8_t *)buf + hdr_len;
        opts[0] = 2;     /* kind: MSS */
        opts[1] = 4;     /* length */
        opts[2] = (tcb->rcv_mss >> 8) & 0xFF;
        opts[3] = tcb->rcv_mss & 0xFF;
    }

    /* copy payload */
    if (data && len > 0)
        memcpy((uint8_t *)buf + hdr_len + opts_len, data, len);

    /* compute checksum */
    hdr->checksum = tcp_checksum(tcb->local_ip, tcb->remote_ip, buf, seg_len);

    /* advance snd_nxt: SYN and FIN consume 1 sequence number each */
    uint32_t seq_consumed = (uint32_t)len;
    if (flags & TCP_SYN) seq_consumed++;
    if (flags & TCP_FIN) seq_consumed++;
    tcb->snd_nxt += seq_consumed;

    /* enqueue for retransmit if this segment has data or is SYN/FIN */
    if (seq_consumed > 0) {
        tcp_rexmit_enqueue(tcb, ntohl(hdr->seq), data, len, flags);
    }

    skb->protocol = IPPROTO_TCP;

    KLOG_DEBUG("tcp: TX %s%s%s%s seq=%u ack=%u len=%u → %d.%d.%d.%d:%u\n",
         (flags & TCP_SYN) ? "S" : "",
         (flags & TCP_ACK) ? "A" : "",
         (flags & TCP_FIN) ? "F" : "",
         (flags & TCP_RST) ? "R" : "",
         ntohl(hdr->seq), ntohl(hdr->ack), (unsigned)len,
         IP4_A(tcb->remote_ip), IP4_B(tcb->remote_ip),
         IP4_C(tcb->remote_ip), IP4_D(tcb->remote_ip),
         tcb->remote_port);

    int rc = ip_tx(tcb->dev, skb, tcb->local_ip, tcb->remote_ip, IPPROTO_TCP);
    skb_free(skb);
    return rc;
}

/* ── Send a standalone RST (no TCB required) ─────────────────────────────── */
int tcp_send_rst(netdev_t *dev, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 uint32_t seq, uint32_t ack)
{
    size_t seg_len = sizeof(tcp_header_t);
    skbuff_t *skb = skb_alloc(seg_len + 128);
    if (!skb) return -1;

    skb_reserve(skb, 128);
    void *buf = skb_put(skb, seg_len);
    memset(buf, 0, seg_len);

    tcp_header_t *hdr = (tcp_header_t *)buf;
    hdr->src_port  = htons(src_port);
    hdr->dst_port  = htons(dst_port);
    hdr->seq       = htonl(seq);
    hdr->ack       = htonl(ack);
    hdr->data_off  = TCP_DATA_OFF(sizeof(tcp_header_t));
    hdr->flags     = TCP_RST | TCP_ACK;
    hdr->window    = 0;
    hdr->checksum  = 0;

    uint32_t src_ip = dev->ip_addr;
    hdr->checksum = tcp_checksum(src_ip, dst_ip, buf, seg_len);

    skb->protocol = IPPROTO_TCP;
    int rc = ip_tx(dev, skb, src_ip, dst_ip, IPPROTO_TCP);
    skb_free(skb);
    return rc;
}

/* ── Send a pure ACK ─────────────────────────────────────────────────────── */
void tcp_send_ack(tcp_tcb_t *tcb) {
    /* pure ACK — no data, no SYN/FIN, so no retransmit */
    size_t seg_len = sizeof(tcp_header_t);
    skbuff_t *skb = skb_alloc(seg_len + 128);
    if (!skb) return;

    skb_reserve(skb, 128);
    void *buf = skb_put(skb, seg_len);
    memset(buf, 0, seg_len);

    tcp_header_t *hdr = (tcp_header_t *)buf;
    hdr->src_port  = htons(tcb->local_port);
    hdr->dst_port  = htons(tcb->remote_port);
    hdr->seq       = htonl(tcb->snd_nxt);
    hdr->ack       = htonl(tcb->rcv_nxt);
    hdr->data_off  = TCP_DATA_OFF(sizeof(tcp_header_t));
    hdr->flags     = TCP_ACK;
    hdr->window    = htons((uint16_t)(tcb->rcv_wnd < 65535 ?
                                      tcb->rcv_wnd : 65535));
    hdr->checksum  = 0;

    hdr->checksum = tcp_checksum(tcb->local_ip, tcb->remote_ip,
                                 buf, seg_len);

    skb->protocol = IPPROTO_TCP;
    ip_tx(tcb->dev, skb, tcb->local_ip, tcb->remote_ip, IPPROTO_TCP);
    skb_free(skb);

    tcb->delack_pending = 0;
    ktimer_cancel(&tcb->delack_timer);
}

/* ── Flush TX buffer: send data from tx_buf up to window ─────────────────── */
int tcp_output(tcp_tcb_t *tcb) {
    if (tcb->state != TCP_ESTABLISHED &&
        tcb->state != TCP_CLOSE_WAIT)
        return 0;

    int sent = 0;
    while (tcb->tx_tail != tcb->tx_head) {
        /* how much data is pending? */
        uint32_t pending;
        if (tcb->tx_head >= tcb->tx_tail)
            pending = tcb->tx_head - tcb->tx_tail;
        else
            pending = tcb->tx_size - tcb->tx_tail + tcb->tx_head;

        if (pending == 0) break;

        /* limit by MSS and sender window */
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t usable = (tcb->snd_wnd > in_flight) ?
                          (tcb->snd_wnd - in_flight) : 0;
        if (usable == 0) break;

        uint32_t chunk = pending;
        if (chunk > tcb->snd_mss) chunk = tcb->snd_mss;
        if (chunk > usable)       chunk = usable;

        /* copy data out of ring buffer */
        uint8_t seg_data[TCP_MSS_DEFAULT];
        for (uint32_t i = 0; i < chunk; i++) {
            seg_data[i] = tcb->tx_buf[(tcb->tx_tail + i) % tcb->tx_size];
        }

        uint8_t flags = TCP_ACK | TCP_PSH;
        int rc = tcp_send_segment(tcb, flags, seg_data, chunk);
        if (rc < 0) break;

        tcb->tx_tail = (tcb->tx_tail + chunk) % tcb->tx_size;
        sent += chunk;

        /* start retransmit timer if not already running */
        tcp_rexmit_timer_reset(tcb);
    }

    return sent;
}

/* ── Retransmit queue: enqueue ───────────────────────────────────────────── */
void tcp_rexmit_enqueue(tcp_tcb_t *tcb, uint32_t seq,
                        const void *data, size_t len, uint8_t flags)
{
    tcp_rexmit_entry_t *ent = kzalloc(sizeof(tcp_rexmit_entry_t));
    if (!ent) return;

    ent->seq_start = seq;
    /* SYN/FIN consume 1 seq # each */
    uint32_t consumed = (uint32_t)len;
    if (flags & TCP_SYN) consumed++;
    if (flags & TCP_FIN) consumed++;
    ent->seq_end   = seq + consumed;
    ent->flags     = flags;
    ent->retries   = 0;
    ent->tx_tick   = 0;  /* TODO: read jiffies */

    if (len > 0) {
        ent->data = kmalloc(len);
        if (ent->data)
            memcpy(ent->data, data, len);
        ent->data_len = len;
    }

    /* append to tail */
    ent->next = NULL;
    if (!tcb->rexmit_head) {
        tcb->rexmit_head = ent;
    } else {
        tcp_rexmit_entry_t *p = tcb->rexmit_head;
        while (p->next) p = p->next;
        p->next = ent;
    }
}

/* ── Retransmit queue: remove ACK'd segments ─────────────────────────────── */
void tcp_rexmit_ack(tcp_tcb_t *tcb, uint32_t ack) {
    while (tcb->rexmit_head) {
        tcp_rexmit_entry_t *ent = tcb->rexmit_head;
        if (tcp_seq_le(ent->seq_end, ack)) {
            /* fully acknowledged */
            tcb->rexmit_head = ent->next;
            if (ent->data) kfree(ent->data);
            kfree(ent);
        } else {
            break;
        }
    }

    /* if nothing left to retransmit, stop the timer */
    if (!tcb->rexmit_head)
        tcp_rexmit_timer_stop(tcb);
}

/* ── Retransmit queue: purge all ─────────────────────────────────────────── */
void tcp_rexmit_purge(tcp_tcb_t *tcb) {
    tcp_rexmit_entry_t *ent = tcb->rexmit_head;
    while (ent) {
        tcp_rexmit_entry_t *next = ent->next;
        if (ent->data) kfree(ent->data);
        kfree(ent);
        ent = next;
    }
    tcb->rexmit_head = NULL;
}
