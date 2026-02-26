/* net/dhcp.h — DHCP client (RFC 2131) */
#pragma once
#include <stdint.h>
#include "drivers/net/netdev.h"

/* ── DHCP message structure ──────────────────────────────────────────────── */
#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_OP_REQUEST   1
#define DHCP_OP_REPLY     2

#define DHCP_HTYPE_ETH    1
#define DHCP_HLEN_ETH     6

/* DHCP message types (option 53) */
#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_DECLINE      4
#define DHCP_ACK          5
#define DHCP_NAK          6
#define DHCP_RELEASE      7

/* DHCP option codes */
#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS           6
#define DHCP_OPT_HOSTNAME      12
#define DHCP_OPT_REQUESTED_IP  50
#define DHCP_OPT_LEASE_TIME    51
#define DHCP_OPT_MSG_TYPE      53
#define DHCP_OPT_SERVER_ID     54
#define DHCP_OPT_PARAM_LIST    55
#define DHCP_OPT_END           255

/* DHCP packet (fixed header, options follow) */
typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;     /* client IP (if already known) */
    uint32_t yiaddr;     /* "your" IP (offered by server) */
    uint32_t siaddr;     /* server IP */
    uint32_t giaddr;     /* gateway agent IP */
    uint8_t  chaddr[16]; /* client hardware address */
    uint8_t  sname[64];  /* server host name */
    uint8_t  file[128];  /* boot file name */
    uint32_t magic;      /* magic cookie 0x63825363 */
    uint8_t  options[312]; /* DHCP options */
} dhcp_packet_t;

/* ── DHCP client state ───────────────────────────────────────────────────── */
typedef struct {
    netdev_t *dev;
    uint32_t  offered_ip;
    uint32_t  server_ip;
    uint32_t  subnet_mask;
    uint32_t  gateway;
    uint32_t  dns;
    uint32_t  lease_time;
    uint32_t  xid;
    int       state;  /* 0=idle, 1=discovering, 2=requesting, 3=bound */
} dhcp_state_t;

/* ── API ─────────────────────────────────────────────────────────────────── */
int  dhcp_discover(netdev_t *dev);
void dhcp_init(void);
