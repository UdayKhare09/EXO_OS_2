/* arch/x86_64/pci.h — PCI configuration space read/write + enumeration */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Max devices we track */
#define PCI_MAX_DEVICES  64

/* BAR types */
#define PCI_BAR_MMIO  0
#define PCI_BAR_IO    1

typedef struct {
    uint64_t base;   /* mapped base address (physical/IO port) */
    uint64_t size;   /* size in bytes                          */
    int      type;   /* PCI_BAR_MMIO or PCI_BAR_IO            */
} pci_bar_t;

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint8_t  class, subclass, prog_if, revision;
    uint8_t  header_type;
    uint8_t  irq_line;
    pci_bar_t bars[6];
} pci_device_t;

/* Enumerate all PCI devices; returns count */
int pci_enumerate(pci_device_t *out, int max);

/* Config space read/write (raw, any offset) */
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t  v);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

/* Enable bus-mastering + memory space for a device */
void pci_enable_device(const pci_device_t *dev);

/* Find first device with given vendor/device IDs; returns NULL if not found */
pci_device_t *pci_find(uint16_t vendor, uint16_t device);

/* Walk PCI capability list; type = PCI cap ID;
 * returns config offset of the capability or 0 if not found */
uint8_t pci_find_cap(const pci_device_t *d, uint8_t cap_id);
