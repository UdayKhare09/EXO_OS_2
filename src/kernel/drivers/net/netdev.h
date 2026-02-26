/* drivers/net/netdev.h — Network device abstraction layer
 *
 * Follows the same vtable + registry + private-data pattern as blkdev_t.
 * NIC drivers implement netdev_ops_t and register via netdev_register().
 * The protocol stack calls netdev_transmit() and receives packets via
 * the rx_callback set during netdev_register().
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net/netutil.h"
#include "net/skbuff.h"
#include "sched/waitq.h"

/* ── Forward declarations ────────────────────────────────────────────────── */
typedef struct netdev netdev_t;

/* ── Network device ops vtable ───────────────────────────────────────────── */
typedef struct netdev_ops {
    /* Transmit a packet (skb->data points to ethernet frame including header).
     * Returns 0 on success, -1 on error.  Caller frees the skb. */
    int (*send_packet)(netdev_t *dev, const void *data, size_t len);

    /* Get the 6-byte MAC address */
    void (*get_mac)(netdev_t *dev, uint8_t mac_out[6]);

    /* Return true if link is up */
    bool (*link_up)(netdev_t *dev);
} netdev_ops_t;

/* RX callback: called by netdev_rx_dispatch from NIC ISR / bottom-half.
 * The protocol stack registers this during init. */
typedef void (*netdev_rx_cb_t)(netdev_t *dev, skbuff_t *skb);

/* ── Network device descriptor ───────────────────────────────────────────── */
#define NETDEV_MAX      8
#define NETDEV_NAME_MAX 16

struct netdev {
    char            name[NETDEV_NAME_MAX];   /* e.g. "eth0", "vnet0"       */
    uint32_t        dev_id;                  /* unique ID from registry     */
    netdev_ops_t   *ops;                     /* driver vtable               */
    void           *priv;                    /* driver-private data         */

    /* Hardware info */
    uint8_t         mac[ETH_ALEN];           /* MAC address                 */
    uint32_t        mtu;                     /* default 1500                */
    bool            link;                    /* link status                 */

    /* IPv4 configuration (set by DHCP or static config) */
    uint32_t        ip_addr;                 /* network byte order          */
    uint32_t        netmask;                 /* network byte order          */
    uint32_t        gateway;                 /* network byte order          */
    uint32_t        dns;                     /* primary DNS server          */

    /* RX path */
    netdev_rx_cb_t  rx_callback;             /* protocol stack entry point  */
    skb_queue_t     rx_queue;                /* ISR → bottom-half queue     */
    waitq_t         rx_wq;                   /* wake tasks waiting on rx    */
};

/* ── Registry ────────────────────────────────────────────────────────────── */

/* Register a network device.  Assigns dev_id, reads MAC via ops->get_mac.
 * Returns 0 on success, -1 if registry full. */
int netdev_register(netdev_t *dev);

/* Look up by numeric ID. Returns NULL if not found. */
netdev_t *netdev_get(uint32_t dev_id);

/* Look up by name (e.g. "eth0"). Returns NULL if not found. */
netdev_t *netdev_get_by_name(const char *name);

/* Return nth registered device (0-based). Returns NULL if oob. */
netdev_t *netdev_get_nth(int n);

/* Return total number of registered network devices. */
int netdev_count(void);

/* ── Transmit ────────────────────────────────────────────────────────────── */

/* Transmit an ethernet frame via the device.  Caller's skb is NOT freed. */
int netdev_transmit(netdev_t *dev, const void *data, size_t len);

/* ── RX dispatch (called from NIC ISR / bottom-half) ─────────────────────── */

/* Allocate a skbuff, copy `data[len]` into it, and push to the device's
 * rx_queue.  Wakes rx_wq and calls rx_callback if set. */
void netdev_rx_dispatch(netdev_t *dev, const void *data, size_t len);

/* Set the global RX callback for all devices (called by protocol stack init) */
void netdev_set_rx_callback(netdev_rx_cb_t cb);

/* ── Init ────────────────────────────────────────────────────────────────── */
void netdev_init(void);
