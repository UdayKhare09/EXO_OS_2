/* net/netutil.h — Network byte-order and checksum utilities
 *
 * Provides htons/ntohs/htonl/ntohl using GCC/Clang built-in byte-swap,
 * plus the standard Internet one's-complement checksum used by IP/ICMP/UDP/TCP.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── Byte-order conversion (host ↔ network) ──────────────────────────────── */
/* x86-64 is little-endian; network byte order is big-endian. */

static inline uint16_t htons(uint16_t h) { return __builtin_bswap16(h); }
static inline uint16_t ntohs(uint16_t n) { return __builtin_bswap16(n); }
static inline uint32_t htonl(uint32_t h) { return __builtin_bswap32(h); }
static inline uint32_t ntohl(uint32_t n) { return __builtin_bswap32(n); }

/* ── Internet checksum (RFC 1071) ─────────────────────────────────────────
 * One's-complement sum of 16-bit words.  Used by IPv4, ICMP, UDP, TCP.
 * `data` need not be aligned; `len` is in bytes (odd byte handled).
 * Returns the checksum in network byte order, ready to put in the header. */
static inline uint16_t inet_checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    /* Sum 16-bit words */
    while (len > 1) {
        uint16_t w = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        sum += w;
        p   += 2;
        len -= 2;
    }
    /* Odd trailing byte */
    if (len == 1)
        sum += (uint16_t)p[0];

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/* Incremental checksum: combine two partial checksums (e.g. pseudo-header + payload).
 * Each input is a one's-complement partial sum (NOT complemented).
 * Call with ~inet_checksum() inputs, then complement the result. */
static inline uint32_t inet_checksum_partial(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        uint16_t w = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        sum += w;
        p   += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint16_t)p[0];
    return sum;
}

static inline uint16_t inet_checksum_fold(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── IPv4 address helpers ─────────────────────────────────────────────────── */
/* Pack 4 octets into a network-byte-order uint32_t (a.b.c.d) */
#define IP4_ADDR(a, b, c, d)  \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Extract individual octets from a network-byte-order IPv4 address */
#define IP4_A(ip) ((uint8_t)((ip)       & 0xFF))
#define IP4_B(ip) ((uint8_t)(((ip)>> 8) & 0xFF))
#define IP4_C(ip) ((uint8_t)(((ip)>>16) & 0xFF))
#define IP4_D(ip) ((uint8_t)(((ip)>>24) & 0xFF))

/* Broadcast address */
#define IP4_BROADCAST  0xFFFFFFFFu

/* MAC address length */
#define ETH_ALEN  6

/* Ethernet frame size limits */
#define ETH_HLEN   14    /* sizeof(eth_header_t)       */
#define ETH_MTU    1500  /* max payload in standard eth */
#define ETH_FRAME_MAX (ETH_HLEN + ETH_MTU)

/* Common EtherType values */
#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806
#define ETH_P_IPV6 0x86DD

/* IP protocol numbers */
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
