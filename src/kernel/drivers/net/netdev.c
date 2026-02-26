/* drivers/net/netdev.c — Network device registry and RX dispatch */
#include "netdev.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "net/skbuff.h"

/* ── Global registry ─────────────────────────────────────────────────────── */
static netdev_t *g_netdevs[NETDEV_MAX];
static int       g_netdev_count  = 0;
static uint32_t  g_next_dev_id   = 1;
static netdev_rx_cb_t g_global_rx_cb = NULL;

/* ── Init ────────────────────────────────────────────────────────────────── */
void netdev_init(void) {
    g_netdev_count = 0;
    g_next_dev_id  = 1;
    g_global_rx_cb = NULL;
    KLOG_INFO("netdev: subsystem initialised\n");
}

/* ── Registry ────────────────────────────────────────────────────────────── */
int netdev_register(netdev_t *dev) {
    if (g_netdev_count >= NETDEV_MAX) {
        KLOG_WARN("netdev: registry full, cannot register '%s'\n", dev->name);
        return -1;
    }

    dev->dev_id = g_next_dev_id++;

    /* Read MAC from hardware */
    if (dev->ops->get_mac)
        dev->ops->get_mac(dev, dev->mac);

    /* Default MTU */
    if (dev->mtu == 0) dev->mtu = ETH_MTU;

    /* Init RX queue and wait queue */
    skb_queue_init(&dev->rx_queue);
    waitq_init(&dev->rx_wq);

    /* Set global rx callback if registered */
    dev->rx_callback = g_global_rx_cb;

    g_netdevs[g_netdev_count++] = dev;

    KLOG_INFO("netdev: registered '%s' (id=%u) MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
              dev->name, dev->dev_id,
              dev->mac[0], dev->mac[1], dev->mac[2],
              dev->mac[3], dev->mac[4], dev->mac[5]);
    return 0;
}

netdev_t *netdev_get(uint32_t dev_id) {
    for (int i = 0; i < g_netdev_count; i++)
        if (g_netdevs[i]->dev_id == dev_id)
            return g_netdevs[i];
    return NULL;
}

netdev_t *netdev_get_by_name(const char *name) {
    for (int i = 0; i < g_netdev_count; i++)
        if (strcmp(g_netdevs[i]->name, name) == 0)
            return g_netdevs[i];
    return NULL;
}

netdev_t *netdev_get_nth(int n) {
    if (n < 0 || n >= g_netdev_count) return NULL;
    return g_netdevs[n];
}

int netdev_count(void) {
    return g_netdev_count;
}

/* ── Transmit ────────────────────────────────────────────────────────────── */
int netdev_transmit(netdev_t *dev, const void *data, size_t len) {
    if (!dev || !dev->ops || !dev->ops->send_packet) return -1;
    return dev->ops->send_packet(dev, data, len);
}

/* ── RX dispatch ─────────────────────────────────────────────────────────── */
void netdev_rx_dispatch(netdev_t *dev, const void *data, size_t len) {
    if (!dev || !data || len == 0) return;

    skbuff_t *skb = skb_alloc(len);
    if (!skb) return;

    /* Copy frame data into skb payload area */
    void *payload = skb_put(skb, len);
    memcpy(payload, data, len);
    skb->dev = dev;
    skb->mac_hdr = skb->data;

    /* If a protocol handler is registered, call it directly.
     * Otherwise queue it and wake waiters for deferred processing. */
    if (dev->rx_callback) {
        dev->rx_callback(dev, skb);
    } else {
        skb_queue_push(&dev->rx_queue, skb);
        waitq_wake_all(&dev->rx_wq);
    }
}

/* ── Set global RX callback ──────────────────────────────────────────────── */
void netdev_set_rx_callback(netdev_rx_cb_t cb) {
    g_global_rx_cb = cb;
    /* Update all already-registered devices */
    for (int i = 0; i < g_netdev_count; i++)
        g_netdevs[i]->rx_callback = cb;
}
