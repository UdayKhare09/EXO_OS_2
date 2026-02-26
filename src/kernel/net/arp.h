/* net/arp.h — Address Resolution Protocol (RFC 826) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "net/netutil.h"
#include "net/skbuff.h"
#include "drivers/net/netdev.h"

/* ── ARP header ──────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t hw_type;       /* 0x0001 = Ethernet */
    uint16_t proto_type;    /* 0x0800 = IPv4     */
    uint8_t  hw_len;        /* 6 for Ethernet    */
    uint8_t  proto_len;     /* 4 for IPv4        */
    uint16_t opcode;        /* 1=request, 2=reply */
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;     /* network byte order */
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;     /* network byte order */
} arp_header_t;

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

/* ── ARP cache entry ─────────────────────────────────────────────────────── */
#define ARP_CACHE_SIZE    64
#define ARP_CACHE_TTL_MS  (5 * 60 * 1000)  /* 5 minutes */

typedef struct {
    uint32_t ip;              /* network byte order; 0 = empty */
    uint8_t  mac[ETH_ALEN];
    uint64_t timestamp;       /* jiffies when entry was created  */
    bool     valid;
} arp_entry_t;

/* ── RX: process an incoming ARP packet ──────────────────────────────────── */
void arp_rx(netdev_t *dev, skbuff_t *skb);

/* ── Resolve an IP to MAC.  Returns 0 and fills mac_out if cached.
 * Returns -1 if not cached (sends ARP request; caller should retry). */
int arp_resolve(netdev_t *dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]);

/* ── Send a gratuitous ARP (announce our IP) ─────────────────────────────── */
void arp_gratuitous(netdev_t *dev);

/* ── Force-add a static ARP entry ────────────────────────────────────────── */
void arp_add_entry(uint32_t ip, const uint8_t mac[ETH_ALEN]);

/* ── Get the ARP table for display ───────────────────────────────────────── */
int arp_get_table(arp_entry_t *out, int max);

/* ── Init ────────────────────────────────────────────────────────────────── */
void arp_init(void);
