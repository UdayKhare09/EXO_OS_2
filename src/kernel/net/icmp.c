/* net/icmp.c — ICMP echo + unreachable (RFC 792) */
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/socket.h"
#include "net/netutil.h"
#include "net/skbuff.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── global ping tracker ─────────────────────────────────────────────────── */
ping_state_t g_ping_state;

void icmp_init(void) {
    memset(&g_ping_state, 0, sizeof(g_ping_state));
}

/* ── helpers ──────────────────────────────────────────────────────────────── */
static uint16_t icmp_checksum(const void *data, size_t len) {
    return inet_checksum(data, len);
}

/* ── RX path ──────────────────────────────────────────────────────────────── */
void icmp_rx(netdev_t *dev, skbuff_t *skb) {
    if (skb->len < sizeof(icmp_header_t)) {
        skb_free(skb);
        return;
    }

    icmp_header_t *hdr = (icmp_header_t *)skb->data;

    /* verify checksum over entire ICMP message */
    if (icmp_checksum(skb->data, skb->len) != 0) {
        KLOG_WARN("icmp: bad checksum, dropping\n");
        skb_free(skb);
        return;
    }

    /* Feed raw ICMP sockets (e.g. BusyBox ping). */
    socket_deliver_icmp_rx(skb);

    switch (hdr->type) {
    case ICMP_ECHO_REQUEST: {
        /* build echo reply — swap src/dst, type → 0 */
        size_t total = skb->len;
        skbuff_t *reply = skb_alloc(total + 64);
        if (!reply) { skb_free(skb); return; }

        skb_reserve(reply, 64);           /* room for IP + ETH headers */
        void *payload = skb_put(reply, total);
        memcpy(payload, skb->data, total);

        icmp_header_t *rhdr = (icmp_header_t *)reply->data;
        rhdr->type     = ICMP_ECHO_REPLY;
        rhdr->code     = 0;
        rhdr->checksum = 0;
        rhdr->checksum = icmp_checksum(reply->data, total);

        reply->protocol = IPPROTO_ICMP;
        ip_tx(dev, reply, dev->ip_addr, skb->src_ip, IPPROTO_ICMP);

        KLOG_DEBUG("icmp: echo reply → %d.%d.%d.%d\n",
             IP4_A(skb->src_ip), IP4_B(skb->src_ip),
             IP4_C(skb->src_ip), IP4_D(skb->src_ip));
        break;
    }
    case ICMP_ECHO_REPLY: {
        uint16_t id  = ntohs(hdr->id);
        uint16_t seq = ntohs(hdr->seq);

        KLOG_INFO("icmp: echo reply from %d.%d.%d.%d  id=%u seq=%u\n",
             IP4_A(skb->src_ip), IP4_B(skb->src_ip),
             IP4_C(skb->src_ip), IP4_D(skb->src_ip),
             id, seq);

        /* update ping state if this matches our outstanding request */
        if (skb->src_ip == g_ping_state.target_ip &&
            id == g_ping_state.id &&
            seq == g_ping_state.last_seq) {
            /* TODO: read tick counter for RTT */
            g_ping_state.reply_received = 1;
        }
        break;
    }
    case ICMP_DEST_UNREACH:
        KLOG_WARN("icmp: destination unreachable code=%u from %d.%d.%d.%d\n",
             hdr->code,
             IP4_A(skb->src_ip), IP4_B(skb->src_ip),
             IP4_C(skb->src_ip), IP4_D(skb->src_ip));
        break;
    case ICMP_TIME_EXCEEDED:
        KLOG_WARN("icmp: time exceeded code=%u from %d.%d.%d.%d\n",
             hdr->code,
             IP4_A(skb->src_ip), IP4_B(skb->src_ip),
             IP4_C(skb->src_ip), IP4_D(skb->src_ip));
        break;
    default:
        KLOG_DEBUG("icmp: unhandled type=%u code=%u\n",
             hdr->type, hdr->code);
        break;
    }

    skb_free(skb);
}

/* ── TX: send echo request ───────────────────────────────────────────────── */
int icmp_send_echo(netdev_t *dev, uint32_t dst_ip,
                   uint16_t id, uint16_t seq,
                   const void *payload, size_t payload_len)
{
    size_t icmp_len = sizeof(icmp_header_t) + payload_len;
    skbuff_t *skb = skb_alloc(icmp_len + 128);
    if (!skb) return -1;

    skb_reserve(skb, 128);               /* IP + ETH headroom */

    void *buf = skb_put(skb, icmp_len);
    icmp_header_t *hdr = (icmp_header_t *)buf;
    hdr->type     = ICMP_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = htons(id);
    hdr->seq      = htons(seq);

    if (payload && payload_len > 0)
        memcpy((uint8_t *)buf + sizeof(icmp_header_t), payload, payload_len);

    hdr->checksum = icmp_checksum(buf, icmp_len);

    skb->protocol = IPPROTO_ICMP;

    /* record in ping state */
    g_ping_state.target_ip     = dst_ip;
    g_ping_state.id            = id;
    g_ping_state.last_seq      = seq;
    g_ping_state.reply_received = 0;

    KLOG_DEBUG("icmp: echo request → %d.%d.%d.%d id=%u seq=%u\n",
         IP4_A(dst_ip), IP4_B(dst_ip),
         IP4_C(dst_ip), IP4_D(dst_ip), id, seq);

    return ip_tx(dev, skb, dev->ip_addr, dst_ip, IPPROTO_ICMP);
}
