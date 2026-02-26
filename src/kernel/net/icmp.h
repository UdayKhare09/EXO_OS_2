/* net/icmp.h — ICMP (RFC 792) */
#pragma once
#include <stdint.h>
#include "net/skbuff.h"
#include "drivers/net/netdev.h"

/* ── ICMP header ─────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

/* ICMP types */
#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8
#define ICMP_DEST_UNREACH  3
#define ICMP_TIME_EXCEEDED 11

/* ── RX handler ──────────────────────────────────────────────────────────── */
void icmp_rx(netdev_t *dev, skbuff_t *skb);

/* ── Send echo request (ping) ────────────────────────────────────────────── */
int icmp_send_echo(netdev_t *dev, uint32_t dst_ip,
                   uint16_t id, uint16_t seq,
                   const void *payload, size_t payload_len);

/* ── Ping state — for tracking echo replies ──────────────────────────────── */
typedef struct {
    uint32_t target_ip;
    uint16_t id;
    uint16_t last_seq;
    volatile int reply_received;
    uint64_t send_tick;
    uint64_t recv_tick;
} ping_state_t;

extern ping_state_t g_ping_state;

/* ── Init ────────────────────────────────────────────────────────────────── */
void icmp_init(void);
