/* drivers/net/virtio_net.h — VirtIO network device driver
 *
 * Supports legacy VirtIO (device 0x1000/0x1041) network interface.
 * Probed via PCI, registers as a netdev_t named "vnet0", "vnet1", etc.
 */
#pragma once
#include <stdbool.h>

/* Initialise all VirtIO network devices found on the PCI bus.
 * Returns number of devices successfully registered (0 if none). */
int virtio_net_init(void);
