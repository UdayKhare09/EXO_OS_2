/* net/udp.c — UDP (RFC 768) */
#include "net/udp.h"
#include "net/ipv4.h"
#include "net/netutil.h"
#include "net/skbuff.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "lib/spinlock.h"
#include "mm/kmalloc.h"

/* ── IPv4 pseudo-header for checksum ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t src;
    uint32_t dst;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t udp_len;
} udp_pseudo_t;

/* ── bind table ──────────────────────────────────────────────────────────── */
static udp_bind_entry_t g_udp_binds[UDP_MAX_BINDS];
static spinlock_t       g_udp_lock;
static uint16_t         g_ephemeral_next = 49152;

void udp_init(void) {
    memset(g_udp_binds, 0, sizeof(g_udp_binds));
    spinlock_init(&g_udp_lock);
}

/* ── port binding ────────────────────────────────────────────────────────── */
int udp_bind_port(uint16_t port, uint32_t addr,
                  udp_rx_cb_t cb, void *ctx)
{
    spinlock_acquire(&g_udp_lock);

    /* check for existing binding */
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (g_udp_binds[i].active && g_udp_binds[i].port == port) {
            spinlock_release(&g_udp_lock);
            return -1;   /* EADDRINUSE */
        }
    }

    /* find free slot */
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!g_udp_binds[i].active) {
            g_udp_binds[i].port     = port;
            g_udp_binds[i].addr     = addr;
            g_udp_binds[i].callback = cb;
            g_udp_binds[i].ctx      = ctx;
            g_udp_binds[i].active   = 1;
            spinlock_release(&g_udp_lock);
            return 0;
        }
    }

    spinlock_release(&g_udp_lock);
    return -1;   /* no slots */
}

void udp_unbind_port(uint16_t port) {
    spinlock_acquire(&g_udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (g_udp_binds[i].active && g_udp_binds[i].port == port) {
            g_udp_binds[i].active = 0;
            break;
        }
    }
    spinlock_release(&g_udp_lock);
}

uint16_t udp_alloc_ephemeral(void) {
    spinlock_acquire(&g_udp_lock);
    uint16_t start = g_ephemeral_next;
    for (;;) {
        uint16_t port = g_ephemeral_next++;
        if (g_ephemeral_next > 65535)
            g_ephemeral_next = 49152;

        int used = 0;
        for (int i = 0; i < UDP_MAX_BINDS; i++) {
            if (g_udp_binds[i].active && g_udp_binds[i].port == port) {
                used = 1;
                break;
            }
        }
        if (!used) {
            spinlock_release(&g_udp_lock);
            return port;
        }
        if (g_ephemeral_next == start) {
            /* wrapped around - all ports used */
            spinlock_release(&g_udp_lock);
            return 0;
        }
    }
}

/* ── UDP checksum (RFC 768) ──────────────────────────────────────────────── */
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void *udp_seg, size_t udp_len)
{
    udp_pseudo_t pseudo;
    pseudo.src      = src_ip;   /* already network byte order */
    pseudo.dst      = dst_ip;   /* already network byte order */
    pseudo.zero     = 0;
    pseudo.protocol = IPPROTO_UDP;
    pseudo.udp_len  = htons((uint16_t)udp_len);

    uint32_t sum = inet_checksum_partial(&pseudo, sizeof(pseudo));
    sum += inet_checksum_partial(udp_seg, udp_len);
    return inet_checksum_fold(sum);
}

/* ── RX path ──────────────────────────────────────────────────────────────── */
void udp_rx(netdev_t *dev, skbuff_t *skb) {
    if (skb->len < sizeof(udp_header_t)) {
        KLOG_WARN("udp: runt packet (%u bytes)\n", skb->len);
        skb_free(skb);
        return;
    }

    udp_header_t *hdr = (udp_header_t *)skb->data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t udp_len  = ntohs(hdr->length);

    if (udp_len < sizeof(udp_header_t) || udp_len > skb->len) {
        KLOG_WARN("udp: bad length field %u (pkt %u)\n", udp_len, skb->len);
        skb_free(skb);
        return;
    }

    /* verify checksum (if non-zero) */
    if (hdr->checksum != 0) {
        uint16_t ck = udp_checksum(skb->src_ip, skb->dst_ip,
                                   skb->data, udp_len);
        if (ck != 0) {
            KLOG_WARN("udp: bad checksum, dropping\n");
            skb_free(skb);
            return;
        }
    }

    const void *payload    = (const uint8_t *)skb->data + sizeof(udp_header_t);
    size_t      payload_len = udp_len - sizeof(udp_header_t);

    skb->src_port = src_port;
    skb->dst_port = dst_port;

    /* dispatch to bound port */
    spinlock_acquire(&g_udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        udp_bind_entry_t *b = &g_udp_binds[i];
        if (!b->active) continue;
        if (b->port != dst_port) continue;
        if (b->addr != 0 && b->addr != skb->dst_ip) continue;

        udp_rx_cb_t cb  = b->callback;
        void       *ctx = b->ctx;
        spinlock_release(&g_udp_lock);

        if (cb)
            cb(skb, skb->src_ip, src_port, skb->dst_ip, dst_port,
               payload, payload_len, ctx);
        else
            skb_free(skb);
        return;
    }
    spinlock_release(&g_udp_lock);

    /* no binding — drop silently (future: ICMP port unreachable) */
    KLOG_DEBUG("udp: no binding for port %u, dropped\n", dst_port);
    skb_free(skb);
}

/* ── TX path ──────────────────────────────────────────────────────────────── */
int udp_tx(netdev_t *dev, uint32_t dst_ip,
           uint16_t src_port, uint16_t dst_port,
           const void *data, size_t len)
{
    size_t udp_total = sizeof(udp_header_t) + len;
    if (udp_total > 65535) return -1;

    skbuff_t *skb = skb_alloc(udp_total + 128);
    if (!skb) return -1;

    skb_reserve(skb, 128);   /* room for IP + ETH headers */

    void *buf = skb_put(skb, udp_total);
    udp_header_t *hdr = (udp_header_t *)buf;
    hdr->src_port  = htons(src_port);
    hdr->dst_port  = htons(dst_port);
    hdr->length    = htons((uint16_t)udp_total);
    hdr->checksum  = 0;

    if (data && len > 0)
        memcpy((uint8_t *)buf + sizeof(udp_header_t), data, len);

    /* compute checksum using dev's IP as source */
    uint32_t src_ip = dev->ip_addr;
    hdr->checksum = udp_checksum(src_ip, dst_ip, buf, udp_total);
    if (hdr->checksum == 0)
        hdr->checksum = 0xFFFF;   /* RFC 768: 0 means no checksum */

    skb->protocol = IPPROTO_UDP;
    return ip_tx(dev, skb, src_ip, dst_ip, IPPROTO_UDP);
}
