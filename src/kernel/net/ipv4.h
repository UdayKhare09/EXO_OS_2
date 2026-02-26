/* net/ipv4.h — IPv4 layer (RFC 791) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net/netutil.h"
#include "net/skbuff.h"
#include "drivers/net/netdev.h"

/* ── IPv4 header ─────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  ihl_ver;       /* version (4 bits) + IHL (4 bits) */
    uint8_t  tos;
    uint16_t total_len;     /* big-endian */
    uint16_t id;
    uint16_t frag_off;      /* flags + fragment offset */
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;        /* network byte order */
    uint32_t dst_ip;        /* network byte order */
} ip_header_t;

#define IP_VERSION   4
#define IP_IHL_MIN   5      /* minimum IHL (no options) = 20 bytes */
#define IP_TTL_DEFAULT 64

/* Flags in frag_off */
#define IP_FLAG_DF  0x4000
#define IP_FLAG_MF  0x2000
#define IP_FRAG_OFF_MASK 0x1FFF

/* ── RX: process an incoming IP packet ───────────────────────────────────── */
void ip_rx(netdev_t *dev, skbuff_t *skb);

/* ── TX: build IP header and transmit ────────────────────────────────────── */
/* `skb->data` should point to the transport-layer payload.
 * This function prepends an IP header and passes to ethernet for TX.
 * `protocol` is IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMP. */
int ip_tx(netdev_t *dev, skbuff_t *skb,
          uint32_t src_ip, uint32_t dst_ip, uint8_t protocol);

/* ── Routing helper ──────────────────────────────────────────────────────── */
/* Find the netdev + next-hop for a given destination IP.
 * Returns the netdev_t to use, and writes the next-hop IP to `next_hop`.
 * If dst is on the local subnet, next_hop = dst.
 * Otherwise next_hop = gateway. */
netdev_t *ip_route(uint32_t dst_ip, uint32_t *next_hop);

/* ── Init ────────────────────────────────────────────────────────────────── */
void ipv4_init(void);
