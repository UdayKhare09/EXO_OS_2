/* net/ethernet.c — Ethernet frame handling (L2 demux + TX) */
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "lib/klog.h"
#include "lib/string.h"

const uint8_t ETH_BROADCAST[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ── RX handler: called from netdev_rx_dispatch via rx_callback ─────────── */
void eth_rx(netdev_t *dev, skbuff_t *skb) {
    if (skb->len < ETH_HLEN) {
        skb_free(skb);
        return;
    }

    eth_header_t *hdr = (eth_header_t *)skb->data;
    skb->mac_hdr = skb->data;

    uint16_t ethertype = ntohs(hdr->ethertype);

    /* Strip ethernet header, advance data to payload */
    skb_pull(skb, ETH_HLEN);
    skb->protocol = ethertype;

    switch (ethertype) {
    case ETH_P_IP:
        ip_rx(dev, skb);
        break;
    case ETH_P_ARP:
        arp_rx(dev, skb);
        break;
    default:
        /* Unknown protocol — drop */
        skb_free(skb);
        break;
    }
}

/* ── TX: prepend ethernet header and send ────────────────────────────────── */
int eth_tx(netdev_t *dev, skbuff_t *skb,
           const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype) {

    /* Push ethernet header in front of payload */
    eth_header_t *hdr = (eth_header_t *)skb_push(skb, ETH_HLEN);
    memcpy(hdr->dst, dst_mac, ETH_ALEN);
    memcpy(hdr->src, dev->mac, ETH_ALEN);
    hdr->ethertype = htons(ethertype);

    /* Send the complete frame via the NIC */
    int ret = netdev_transmit(dev, skb->data, skb->len);
    return ret;
}

/* ── Init ────────────────────────────────────────────────────────────────── */
void ethernet_init(void) {
    KLOG_INFO("ethernet: layer initialised\n");
}
