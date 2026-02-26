/* net/net_init.c — Network subsystem initialisation
 *
 * Called as a kernel task from kmain().  Initialises all protocol layers
 * bottom-up, probes NIC drivers, and starts DHCP.
 */
#include "net/netutil.h"
#include "net/skbuff.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "net/dhcp.h"
#include "net/socket.h"
#include "drivers/net/netdev.h"
#include "drivers/net/virtio_net.h"
#include "drivers/net/e1000.h"
#include "lib/timer.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "sched/task.h"

/* ── global RX callback: Ethernet demux ──────────────────────────────────── */
static void net_rx_handler(netdev_t *dev, skbuff_t *skb)
{
    skb->dev = dev;
    eth_rx(dev, skb);
}

/* ── Initialisation task entry point ─────────────────────────────────────── */
void net_init_task(void *arg) {
    (void)arg;
    KLOG_INFO("net: === network subsystem starting ===\n");

    /* 1. Timer subsystem */
    ktimer_subsys_init();

    /* 2. Protocol layers (bottom-up) */
    icmp_init();
    udp_init();
    tcp_init();
    socket_init();
    dhcp_init();

    /* 3. Register global RX callback */
    netdev_set_rx_callback(net_rx_handler);

    /* 4. Probe NIC drivers */
    KLOG_INFO("net: probing NIC drivers...\n");
    virtio_net_init();
    e1000_init();

    /* 5. Run DHCP on first available device */
    netdev_t *dev = netdev_get_nth(0);
    if (dev) {
        KLOG_INFO("net: starting DHCP on %s\n", dev->name);
        int rc = dhcp_discover(dev);
        if (rc < 0) {
            KLOG_WARN("net: DHCP failed, using static fallback\n");
            /* fallback: QEMU user-mode default */
            dev->ip_addr = IP4_ADDR(10, 0, 2, 15);
            dev->netmask = IP4_ADDR(255, 255, 255, 0);
            dev->gateway = IP4_ADDR(10, 0, 2, 2);
            dev->dns     = IP4_ADDR(10, 0, 2, 3);
        }
    } else {
        KLOG_WARN("net: no network devices found\n");
    }

    KLOG_INFO("net: === network subsystem ready ===\n");

    /* task exits cleanly */
    sched_task_exit();
}
