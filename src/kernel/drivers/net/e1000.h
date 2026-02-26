/* drivers/net/e1000.h — Intel 82540EM (e1000) Gigabit Ethernet driver
 *
 * Supports Intel 82540EM (PCI 8086:100E) used by QEMU -device e1000.
 * Uses MMIO BAR0, descriptor-ring RX/TX, interrupt-driven receive.
 * Registers as a netdev_t named "eth0", "eth1", etc.
 */
#pragma once
#include <stdbool.h>

/* Initialise all Intel e1000 devices found on the PCI bus.
 * Returns number of devices successfully registered (0 if none). */
int e1000_init(void);
