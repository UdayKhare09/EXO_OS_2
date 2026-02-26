/* net/ethernet.h — Ethernet (L2) frame handling */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net/netutil.h"
#include "net/skbuff.h"
#include "drivers/net/netdev.h"

/* ── Ethernet header ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;          /* big-endian */
} eth_header_t;

/* Broadcast MAC */
extern const uint8_t ETH_BROADCAST[ETH_ALEN];

/* ── RX: called from netdev rx_callback with raw ethernet frame ─────────── */
void eth_rx(netdev_t *dev, skbuff_t *skb);

/* ── TX: build ethernet frame and transmit ───────────────────────────────── */
/* Prepends ethernet header to `skb` and sends via `dev`.
 * `skb->data` should point to the payload (e.g., IP packet).
 * `dst_mac` is the destination MAC address.
 * `ethertype` is the protocol (ETH_P_IP, ETH_P_ARP, etc.) in HOST order. */
int eth_tx(netdev_t *dev, skbuff_t *skb,
           const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype);

/* ── Init ────────────────────────────────────────────────────────────────── */
void ethernet_init(void);
