/* net/dhcp.c — DHCP client: DORA handshake (RFC 2131)
 *
 * Runs as a kernel task. Sends DISCOVER via broadcast UDP 68→67,
 * waits for OFFER, sends REQUEST, waits for ACK, configures netdev.
 */
#include "net/dhcp.h"
#include "net/udp.h"
#include "net/ipv4.h"
#include "net/netutil.h"
#include "net/skbuff.h"
#include "drivers/net/netdev.h"
#include "sched/sched.h"
#include "sched/waitq.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static dhcp_state_t g_dhcp;
static waitq_t      g_dhcp_wq;

/* ── option builder helpers ──────────────────────────────────────────────── */
static uint8_t *dhcp_opt_byte(uint8_t *p, uint8_t code, uint8_t val) {
    *p++ = code; *p++ = 1; *p++ = val;
    return p;
}

static uint8_t *dhcp_opt_ip(uint8_t *p, uint8_t code, uint32_t ip_be) {
    *p++ = code; *p++ = 4;
    memcpy(p, &ip_be, 4); p += 4;
    return p;
}

static uint8_t *dhcp_opt_param_list(uint8_t *p) {
    *p++ = DHCP_OPT_PARAM_LIST;
    *p++ = 3;   /* length */
    *p++ = DHCP_OPT_SUBNET_MASK;
    *p++ = DHCP_OPT_ROUTER;
    *p++ = DHCP_OPT_DNS;
    return p;
}

/* ── option parser ───────────────────────────────────────────────────────── */
static void dhcp_parse_options(const uint8_t *opts, size_t len,
                               dhcp_state_t *st, uint8_t *msg_type_out)
{
    for (size_t i = 0; i < len; ) {
        uint8_t code = opts[i++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (i >= len) break;
        uint8_t olen = opts[i++];
        if (i + olen > len) break;

        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (olen >= 1) *msg_type_out = opts[i];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (olen >= 4) memcpy(&st->subnet_mask, &opts[i], 4);
            break;
        case DHCP_OPT_ROUTER:
            if (olen >= 4) memcpy(&st->gateway, &opts[i], 4);
            break;
        case DHCP_OPT_DNS:
            if (olen >= 4) memcpy(&st->dns, &opts[i], 4);
            break;
        case DHCP_OPT_LEASE_TIME:
            if (olen >= 4) {
                uint32_t lt;
                memcpy(&lt, &opts[i], 4);
                st->lease_time = ntohl(lt);
            }
            break;
        case DHCP_OPT_SERVER_ID:
            if (olen >= 4) memcpy(&st->server_ip, &opts[i], 4);
            break;
        }
        i += olen;
    }
}

/* ── Build + send a DHCP packet over raw UDP ─────────────────────────────── */
static int dhcp_send_packet(netdev_t *dev, uint8_t msg_type,
                            uint32_t req_ip_be, uint32_t server_ip_be)
{
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op    = DHCP_OP_REQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen  = DHCP_HLEN_ETH;
    pkt.xid   = htonl(g_dhcp.xid);
    pkt.flags = htons(0x8000);  /* broadcast flag */
    memcpy(pkt.chaddr, dev->mac, 6);
    pkt.magic = htonl(DHCP_MAGIC_COOKIE);

    /* build options */
    uint8_t *p = pkt.options;
    p = dhcp_opt_byte(p, DHCP_OPT_MSG_TYPE, msg_type);

    if (msg_type == DHCP_REQUEST) {
        p = dhcp_opt_ip(p, DHCP_OPT_REQUESTED_IP, req_ip_be);
        p = dhcp_opt_ip(p, DHCP_OPT_SERVER_ID, server_ip_be);
    }

    p = dhcp_opt_param_list(p);
    *p++ = DHCP_OPT_END;

    /* compute actual size (header + options used) */
    size_t pkt_len = (size_t)(p - (uint8_t *)&pkt);
    if (pkt_len < 300) pkt_len = 300;  /* minimum DHCP packet size */

    /* send via UDP 68 → 67 to broadcast 255.255.255.255 */
    uint32_t bcast = 0xFFFFFFFF;
    return udp_tx(dev, bcast, 68, 67, &pkt, pkt_len);
}

/* ── UDP RX callback for port 68 ─────────────────────────────────────────── */
static void dhcp_rx_callback(skbuff_t *skb, uint32_t src_ip,
                             uint16_t src_port, uint32_t dst_ip,
                             uint16_t dst_port,
                             const void *payload, size_t payload_len,
                             void *ctx)
{
    (void)src_ip; (void)src_port; (void)dst_ip; (void)dst_port; (void)ctx;

    if (payload_len < sizeof(dhcp_packet_t) - 312)  {
        skb_free(skb);
        return;
    }

    const dhcp_packet_t *pkt = (const dhcp_packet_t *)payload;

    /* validate magic */
    if (ntohl(pkt->magic) != DHCP_MAGIC_COOKIE) {
        skb_free(skb);
        return;
    }

    /* validate xid */
    if (ntohl(pkt->xid) != g_dhcp.xid) {
        skb_free(skb);
        return;
    }

    uint8_t msg_type = 0;
    size_t opts_offset = (size_t)((const uint8_t *)pkt->options - (const uint8_t *)pkt);
    size_t opts_len = (payload_len > opts_offset) ? (payload_len - opts_offset) : 0;
    dhcp_parse_options(pkt->options, opts_len, &g_dhcp, &msg_type);

    if (msg_type == DHCP_OFFER && g_dhcp.state == 1) {
        g_dhcp.offered_ip = pkt->yiaddr;  /* already in network order */
        g_dhcp.state = 2;
        KLOG_INFO("dhcp: OFFER: %d.%d.%d.%d\n",
             IP4_A(pkt->yiaddr), IP4_B(pkt->yiaddr),
             IP4_C(pkt->yiaddr), IP4_D(pkt->yiaddr));
        waitq_wake_all(&g_dhcp_wq);
    } else if (msg_type == DHCP_ACK && g_dhcp.state == 2) {
        g_dhcp.state = 3;
        KLOG_INFO("dhcp: ACK: lease for %d.%d.%d.%d (%u sec)\n",
             IP4_A(pkt->yiaddr), IP4_B(pkt->yiaddr),
             IP4_C(pkt->yiaddr), IP4_D(pkt->yiaddr),
             g_dhcp.lease_time);
        waitq_wake_all(&g_dhcp_wq);
    } else if (msg_type == DHCP_NAK) {
        KLOG_WARN("dhcp: NAK received\n");
        g_dhcp.state = 0;
        waitq_wake_all(&g_dhcp_wq);
    }

    skb_free(skb);
}

/* ── DORA handshake ──────────────────────────────────────────────────────── */
int dhcp_discover(netdev_t *dev) {
    memset(&g_dhcp, 0, sizeof(g_dhcp));
    g_dhcp.dev = dev;
    /* simple XID from MAC bytes */
    g_dhcp.xid = (uint32_t)dev->mac[2] << 24 |
                 (uint32_t)dev->mac[3] << 16 |
                 (uint32_t)dev->mac[4] << 8  |
                 (uint32_t)dev->mac[5];
    g_dhcp.state = 1;

    /* bind UDP port 68 for replies */
    int rc = udp_bind_port(68, 0, dhcp_rx_callback, NULL);
    if (rc < 0) {
        KLOG_ERR("dhcp: failed to bind port 68\n");
        return -1;
    }

    /* ── DISCOVER ──────────────────────────────────────────────────────── */
    KLOG_INFO("dhcp: sending DISCOVER on %s\n", dev->name);
    dhcp_send_packet(dev, DHCP_DISCOVER, 0, 0);

    /* wait for OFFER (with timeout) */
    for (int tries = 0; tries < 5 && g_dhcp.state == 1; tries++) {
        sched_sleep(1000);
    }

    if (g_dhcp.state != 2) {
        KLOG_ERR("dhcp: no OFFER received\n");
        udp_unbind_port(68);
        return -1;
    }

    /* ── REQUEST ───────────────────────────────────────────────────────── */
    KLOG_INFO("dhcp: sending REQUEST for %d.%d.%d.%d\n",
         IP4_A(g_dhcp.offered_ip),
         IP4_B(g_dhcp.offered_ip),
         IP4_C(g_dhcp.offered_ip),
         IP4_D(g_dhcp.offered_ip));

    dhcp_send_packet(dev, DHCP_REQUEST,
                     g_dhcp.offered_ip, g_dhcp.server_ip);

    /* wait for ACK */
    for (int tries = 0; tries < 5 && g_dhcp.state == 2; tries++) {
        sched_sleep(1000);
    }

    udp_unbind_port(68);

    if (g_dhcp.state != 3) {
        KLOG_ERR("dhcp: no ACK received\n");
        return -1;
    }

    /* ── Configure netdev ─────────────────────────────────────────────── */
    dev->ip_addr  = g_dhcp.offered_ip;   /* already network byte order */
    dev->netmask  = g_dhcp.subnet_mask;
    dev->gateway  = g_dhcp.gateway;
    dev->dns      = g_dhcp.dns;
    dev->link     = true;               /* DHCP success confirms link is up */

    KLOG_INFO("dhcp: configured %s: ip=%d.%d.%d.%d mask=%d.%d.%d.%d gw=%d.%d.%d.%d\n",
         dev->name,
         IP4_A(dev->ip_addr), IP4_B(dev->ip_addr),
         IP4_C(dev->ip_addr), IP4_D(dev->ip_addr),
         IP4_A(dev->netmask), IP4_B(dev->netmask),
         IP4_C(dev->netmask), IP4_D(dev->netmask),
         IP4_A(dev->gateway), IP4_B(dev->gateway),
         IP4_C(dev->gateway), IP4_D(dev->gateway));

    /* send a gratuitous ARP so the network knows us */
    extern void arp_gratuitous(netdev_t *dev);
    arp_gratuitous(dev);

    return 0;
}

void dhcp_init(void) {
    waitq_init(&g_dhcp_wq);
    memset(&g_dhcp, 0, sizeof(g_dhcp));
}
