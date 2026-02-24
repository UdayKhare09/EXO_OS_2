/* drivers/storage/virtio_blk.h — VirtIO block device driver
 *
 * Supports both legacy (device 0x1001) and modern (0x1042) VirtIO block
 * devices. Probed via PCI, registers as a blkdev_t named "vda", "vdb", etc.
 */
#pragma once
#include <stdbool.h>

/* Initialise all VirtIO block devices found on the PCI bus.
 * Returns number of devices successfully registered (0 if none). */
int virtio_blk_init(void);
