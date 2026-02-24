/* drivers/storage/ahci.h — AHCI/SATA host controller driver
 *
 * Scans PCI for AHCI controllers (class=0x01, subclass=0x06),
 * probes attached drives, and registers each as a blkdev_t.
 */
#pragma once
#include <stdbool.h>

/* Detect and initialise all AHCI controllers on the PCI bus.
 * Returns the number of drives registered as block devices (0 if none). */
int ahci_init(void);
