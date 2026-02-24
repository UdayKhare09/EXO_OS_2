/* arch/x86_64/pci.c — PCI type-0 config space via I/O port 0xCF8/0xCFC */
#include "pci.h"
#include "cpu.h"
#include "mm/vmm.h"
#include "lib/klog.h"
#include <stddef.h>

/* Global discovered device table */
static pci_device_t g_pci_devs[PCI_MAX_DEVICES];
static int          g_pci_count = 0;

/* ── Config space I/O ─────────────────────────────────────────────────────── */
static inline uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(fn  & 0x07) <<  8)
         | (off & 0xFC);
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, pci_addr(bus, dev, fn, off));
    return inb(0xCFC + (off & 3));
}
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, pci_addr(bus, dev, fn, off));
    return inw(0xCFC + (off & 2));
}
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, pci_addr(bus, dev, fn, off));
    return inl(0xCFC);
}
void pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t v) {
    outl(0xCF8, pci_addr(bus, dev, fn, off));
    outb(0xCFC + (off & 3), v);
}
void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v) {
    outl(0xCF8, pci_addr(bus, dev, fn, off));
    outw(0xCFC + (off & 2), v);
}
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8, pci_addr(bus, dev, fn, off));
    outl(0xCFC, v);
}

/* ── BAR decode ───────────────────────────────────────────────────────────── */
static void read_bars(pci_device_t *d) {
    for (int i = 0; i < 6; i++) {
        uint8_t off = 0x10 + i * 4;
        uint32_t val = pci_read32(d->bus, d->dev, d->fn, off);
        if (val == 0) continue;

        if (val & 1) {
            /* I/O BAR */
            d->bars[i].type = PCI_BAR_IO;
            d->bars[i].base = val & ~0x3u;
            /* probe size */
            pci_write32(d->bus, d->dev, d->fn, off, 0xFFFFFFFF);
            uint32_t sz = pci_read32(d->bus, d->dev, d->fn, off);
            pci_write32(d->bus, d->dev, d->fn, off, val);
            d->bars[i].size = (~(sz & ~0x3u) + 1) & 0xFFFF;
        } else {
            /* MMIO BAR */
            int bar_i = i;   /* save before potential i++ for 64-bit */
            d->bars[bar_i].type = PCI_BAR_MMIO;
            int type64 = (val >> 1) & 3;
            d->bars[bar_i].base = val & ~0xFu;
            if (type64 == 2 && i < 5) {
                uint32_t hi = pci_read32(d->bus, d->dev, d->fn, off + 4);
                d->bars[bar_i].base |= (uint64_t)hi << 32;
                i++;  /* skip high dword slot in outer loop */
            }
            /* probe size (32-bit lower probe is fine for BARs < 4 GiB) */
            pci_write32(d->bus, d->dev, d->fn, off, 0xFFFFFFFF);
            uint32_t sz = pci_read32(d->bus, d->dev, d->fn, off);
            pci_write32(d->bus, d->dev, d->fn, off, val);
            if (sz == 0 || sz == 0xFFFFFFFF) { d->bars[bar_i].size = 0; continue; }
            d->bars[bar_i].size = (uint64_t)(~(sz & ~0xFu) + 1);
        }
    }
}

/* ── Enumeration ──────────────────────────────────────────────────────────── */
static int probe_fn(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t vid_did = pci_read32(bus, dev, fn, 0);
    if ((vid_did & 0xFFFF) == 0xFFFF) return 0;
    if (g_pci_count >= PCI_MAX_DEVICES) return 1;

    pci_device_t *d = &g_pci_devs[g_pci_count++];
    d->bus       = bus;
    d->dev       = dev;
    d->fn        = fn;
    d->vendor_id = (uint16_t)(vid_did & 0xFFFF);
    d->device_id = (uint16_t)(vid_did >> 16);

    uint32_t class_rev = pci_read32(bus, dev, fn, 0x08);
    d->revision  = (uint8_t)(class_rev);
    d->prog_if   = (uint8_t)(class_rev >> 8);
    d->subclass  = (uint8_t)(class_rev >> 16);
    d->class     = (uint8_t)(class_rev >> 24);
    d->header_type = pci_read8(bus, dev, fn, 0x0E) & 0x7F;
    d->irq_line  = pci_read8(bus, dev, fn, 0x3C);

    if (d->header_type == 0) read_bars(d);

    KLOG_INFO("PCI %02x:%02x.%x  %04x:%04x  class=%02x/%02x\n",
              bus, dev, fn, d->vendor_id, d->device_id, d->class, d->subclass);
    return 1;
}

int pci_enumerate(pci_device_t *out, int max) {
    g_pci_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            if ((pci_read32(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF) continue;
            uint8_t hdr = pci_read8(bus, dev, 0, 0x0E);
            probe_fn(bus, dev, 0);
            if (hdr & 0x80) {   /* multi-function */
                for (int fn = 1; fn < 8; fn++) probe_fn(bus, dev, fn);
            }
        }
    }
    int n = g_pci_count < max ? g_pci_count : max;
    for (int i = 0; i < n; i++) out[i] = g_pci_devs[i];
    return n;
}

void pci_enable_device(const pci_device_t *d) {
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= (1 << 1) | (1 << 2);  /* Memory Space + Bus Master */
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}

pci_device_t *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_pci_count; i++) {
        if (g_pci_devs[i].vendor_id == vendor &&
            g_pci_devs[i].device_id == device)
            return &g_pci_devs[i];
    }
    return NULL;
}

uint8_t pci_find_cap(const pci_device_t *d, uint8_t cap_id) {
    uint16_t status = pci_read16(d->bus, d->dev, d->fn, 0x06);
    if (!(status & (1 << 4))) return 0;              /* no cap list */
    uint8_t ptr = pci_read8(d->bus, d->dev, d->fn, 0x34) & ~3u;
    for (int i = 0; i < 48 && ptr; i++) {
        uint8_t id = pci_read8(d->bus, d->dev, d->fn, ptr);
        if (id == cap_id) return ptr;
        ptr = pci_read8(d->bus, d->dev, d->fn, ptr + 1) & ~3u;
    }
    return 0;
}
