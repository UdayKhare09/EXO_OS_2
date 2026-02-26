/* net/udp.h — UDP (RFC 768) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net/skbuff.h"
#include "drivers/net/netdev.h"

/* ── UDP header ──────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;        /* header + payload */
    uint16_t checksum;
} udp_header_t;

/* ── RX callback — socket layer registers per-port handlers ──────────────── */
typedef void (*udp_rx_cb_t)(skbuff_t *skb, uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            const void *payload, size_t payload_len,
                            void *ctx);

/* ── Port binding table ──────────────────────────────────────────────────── */
#define UDP_MAX_BINDS  64

typedef struct {
    uint16_t    port;         /* host byte order */
    uint32_t    addr;         /* INADDR_ANY (0) or specific, host order */
    udp_rx_cb_t callback;
    void       *ctx;          /* opaque — usually socket_t* */
    int         active;
} udp_bind_entry_t;

/* ── API ─────────────────────────────────────────────────────────────────── */
void udp_init(void);

/* Receive handler — called from ip_rx() */
void udp_rx(netdev_t *dev, skbuff_t *skb);

/* Send a UDP datagram */
int  udp_tx(netdev_t *dev, uint32_t dst_ip,
            uint16_t src_port, uint16_t dst_port,
            const void *data, size_t len);

/* Port binding for receive dispatch */
int  udp_bind_port(uint16_t port, uint32_t addr,
                   udp_rx_cb_t cb, void *ctx);
void udp_unbind_port(uint16_t port);

/* Allocate ephemeral port (49152–65535) */
uint16_t udp_alloc_ephemeral(void);
