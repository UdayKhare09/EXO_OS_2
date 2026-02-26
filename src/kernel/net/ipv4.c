/* net/ipv4.c — IPv4 layer (RFC 791) */
#include "ipv4.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "sched/sched.h"

static uint16_t g_ip_id = 1;

void ipv4_init(void) {
    g_ip_id = 1;
    KLOG_INFO("ipv4: layer initialised\n");
}

/* ── Routing ─────────────────────────────────────────────────────────────── */
netdev_t *ip_route(uint32_t dst_ip, uint32_t *next_hop) {
    /* Check all interfaces for a matching subnet */
    int n = netdev_count();
    for (int i = 0; i < n; i++) {
        netdev_t *dev = netdev_get_nth(i);
        if (!dev || dev->ip_addr == 0) continue;

        if ((dst_ip & dev->netmask) == (dev->ip_addr & dev->netmask)) {
            /* On-link: send directly */
            if (next_hop) *next_hop = dst_ip;
            return dev;
        }
    }

    /* Not on any local subnet — use default gateway */
    for (int i = 0; i < n; i++) {
        netdev_t *dev = netdev_get_nth(i);
        if (!dev || dev->gateway == 0) continue;
        if (next_hop) *next_hop = dev->gateway;
        return dev;
    }

    return NULL;  /* no route */
}

/* ── RX handler ──────────────────────────────────────────────────────────── */
void ip_rx(netdev_t *dev, skbuff_t *skb) {
    if (skb->len < sizeof(ip_header_t)) {
        skb_free(skb);
        return;
    }

    ip_header_t *ip = (ip_header_t *)skb->data;

    /* Version check */
    uint8_t version = (ip->ihl_ver >> 4) & 0x0F;
    if (version != IP_VERSION) {
        skb_free(skb);
        return;
    }

    /* Header length */
    uint8_t ihl = ip->ihl_ver & 0x0F;
    size_t hdr_len = (size_t)ihl * 4;
    if (hdr_len < 20 || hdr_len > skb->len) {
        skb_free(skb);
        return;
    }

    /* Verify header checksum */
    uint16_t saved_csum = ip->checksum;
    ip->checksum = 0;
    uint16_t computed = inet_checksum(ip, hdr_len);
    ip->checksum = saved_csum;
    if (computed != saved_csum) {
        KLOG_WARN("ipv4: bad checksum (got 0x%04x, computed 0x%04x)\n",
                  saved_csum, computed);
        skb_free(skb);
        return;
    }

    /* Check destination: must be our IP, broadcast, or multicast */
    uint32_t dst = ip->dst_ip;
    if (dst != dev->ip_addr &&
        dst != IP4_BROADCAST &&
        dst != 0 &&
        (dst & dev->netmask) != (IP4_BROADCAST & dev->netmask)) {
        /* Not for us — drop (no forwarding) */
        skb_free(skb);
        return;
    }

    /* Record addresses in skb for transport layer */
    skb->src_ip = ip->src_ip;
    skb->dst_ip = ip->dst_ip;
    skb->network_hdr = skb->data;

    /* Strip IP header */
    skb_pull(skb, hdr_len);
    skb->transport_hdr = skb->data;

    /* Demux by protocol */
    switch (ip->protocol) {
    case IPPROTO_ICMP:
        icmp_rx(dev, skb);
        break;
    case IPPROTO_UDP:
        udp_rx(dev, skb);
        break;
    case IPPROTO_TCP:
        tcp_rx(dev, skb);
        break;
    default:
        skb_free(skb);
        break;
    }
}

/* ── TX: build IP header and send ────────────────────────────────────────── */
int ip_tx(netdev_t *dev, skbuff_t *skb,
          uint32_t src_ip, uint32_t dst_ip, uint8_t protocol) {

    /* Prepend IP header */
    ip_header_t *ip = (ip_header_t *)skb_push(skb, sizeof(ip_header_t));

    ip->ihl_ver   = (IP_VERSION << 4) | IP_IHL_MIN;
    ip->tos       = 0;
    ip->total_len = htons((uint16_t)skb->len);
    ip->id        = htons(g_ip_id++);
    ip->frag_off  = htons(IP_FLAG_DF);  /* Don't Fragment */
    ip->ttl       = IP_TTL_DEFAULT;
    ip->protocol  = protocol;
    ip->checksum  = 0;
    ip->src_ip    = src_ip;
    ip->dst_ip    = dst_ip;

    /* Compute checksum over just the IP header */
    ip->checksum = inet_checksum(ip, sizeof(ip_header_t));

    /* Resolve next-hop MAC via ARP */
    uint32_t next_hop;
    if ((dst_ip & dev->netmask) == (dev->ip_addr & dev->netmask))
        next_hop = dst_ip;
    else
        next_hop = dev->gateway;

    uint8_t dst_mac[ETH_ALEN];

    /* Broadcast IP → broadcast MAC */
    if (dst_ip == IP4_BROADCAST ||
        dst_ip == ((dev->ip_addr & dev->netmask) | ~dev->netmask)) {
        memcpy(dst_mac, ETH_BROADCAST, ETH_ALEN);
    } else {
        /* Try ARP resolve with retries */
        int tries = 3;
        while (arp_resolve(dev, next_hop, dst_mac) < 0 && --tries > 0) {
            /* Wait a bit for ARP reply */
            sched_sleep(50);
        }
        if (tries == 0) {
            KLOG_WARN("ipv4: ARP failed for %u.%u.%u.%u\n",
                      IP4_A(next_hop), IP4_B(next_hop),
                      IP4_C(next_hop), IP4_D(next_hop));
            return -1;
        }
    }

    return eth_tx(dev, skb, dst_mac, ETH_P_IP);
}
