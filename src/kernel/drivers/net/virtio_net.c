/* drivers/net/virtio_net.c — VirtIO network device driver (legacy PCI)
 *
 * Implements VirtIO 0.9 (legacy) network interface via I/O port BAR0.
 * Uses two virtqueues: RX (queue 0) and TX (queue 1).
 * RX is interrupt-driven; TX is polled.
 *
 * Register layout — same as virtio_blk (legacy VirtIO I/O BAR):
 *   +0x00  uint32  device_features (r)
 *   +0x04  uint32  guest_features  (w)
 *   +0x08  uint32  queue_pfn       (w)
 *   +0x0C  uint16  queue_size      (r)
 *   +0x0E  uint16  queue_select    (w)
 *   +0x10  uint16  queue_notify    (w)
 *   +0x12  uint8   device_status   (r/w)
 *   +0x13  uint8   isr_status      (r)
 *   +0x14  … device-specific config (MAC at +0x14..+0x19)
 */
#include "virtio_net.h"
#include "netdev.h"
#include "arch/x86_64/pci.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/ioapic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/string.h"

#include <stdint.h>

/* ── PCI IDs ─────────────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR            0x1AF4
#define VIRTIO_DEV_NET_LEGACY    0x1000   /* transitional: subsystem devid = 1 */
#define VIRTIO_DEV_NET_MODERN    0x1041

/* ── Legacy VirtIO I/O register offsets (same as virtio_blk) ─────────────── */
#define VIRT_REG_DEV_FEAT     0x00
#define VIRT_REG_DRV_FEAT     0x04
#define VIRT_REG_QUEUE_PFN    0x08
#define VIRT_REG_QUEUE_SIZE   0x0C
#define VIRT_REG_QUEUE_SEL    0x0E
#define VIRT_REG_QUEUE_NOTIFY 0x10
#define VIRT_REG_STATUS       0x12
#define VIRT_REG_ISR          0x13

/* Device-specific config starts at 0x14 for legacy (no MSI-X).
 * For net: MAC address at offset 0x14..0x19 */
#define VIRT_REG_MAC          0x14

/* ── Device status flags ─────────────────────────────────────────────────── */
#define VIRTIO_STATUS_ACK         0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FEAT_OK     0x08
#define VIRTIO_STATUS_FAILED      0x80

/* ── Virtqueue descriptor flags ─────────────────────────────────────────── */
#define VRING_DESC_F_NEXT         0x01
#define VRING_DESC_F_WRITE        0x02

/* ── VirtIO net feature bits ─────────────────────────────────────────────── */
#define VIRTIO_NET_F_MAC          (1u << 5)
#define VIRTIO_NET_F_STATUS       (1u << 16)
#define VIRTIO_NET_F_MRG_RXBUF   (1u << 15)

/* ── VirtIO net header (prepended to every packet) ───────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    /* uint16_t num_buffers; — only with VIRTIO_NET_F_MRG_RXBUF */
} virtio_net_hdr_t;

#define VIRTIO_NET_HDR_SIZE  sizeof(virtio_net_hdr_t)  /* 10 bytes */

/* ── Virtqueue ring sizes ────────────────────────────────────────────────── */
#define VQUEUE_MAX_SIZE 256
#define VQUEUE_ALIGN    4096

/* ── VirtIO ring structures ──────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQUEUE_MAX_SIZE];
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t          flags;
    uint16_t          idx;
    vring_used_elem_t ring[VQUEUE_MAX_SIZE];
} vring_used_t;

/* ── Per-virtqueue state ─────────────────────────────────────────────────── */
typedef struct {
    vring_desc_t  *desc;
    vring_avail_t *avail;
    vring_used_t  *used;
    uint16_t       queue_size;
    uint16_t       last_used_idx;
    uint16_t       free_head;       /* head of free descriptor list */
    uint16_t       num_free;        /* number of free descriptors   */
} virtqueue_t;

/* ── RX buffer layout ────────────────────────────────────────────────────── */
/* Each RX descriptor buffer: virtio_net_hdr + ETH_FRAME_MAX */
#define RX_BUF_SIZE   (VIRTIO_NET_HDR_SIZE + 14 + 1500 + 4)
#define RX_RING_SIZE  64   /* number of pre-posted RX buffers */
#define TX_RING_SIZE  64

/* ── Per-device private state ────────────────────────────────────────────── */
typedef struct {
    uint16_t      io_base;

    virtqueue_t   rx_vq;
    virtqueue_t   tx_vq;

    /* RX buffers: pre-allocated DMA-able memory */
    uintptr_t     rx_buf_phys[RX_RING_SIZE];
    uint8_t      *rx_buf_virt[RX_RING_SIZE];

    /* TX bounce buffer (one page, used synchronously) */
    uintptr_t     tx_buf_phys;
    uint8_t      *tx_buf_virt;

    uint8_t       irq_vec;    /* IDT vector for this device */
    netdev_t     *netdev;     /* back-pointer to netdev_t   */
} virtio_net_priv_t;

/* ── I/O helpers ─────────────────────────────────────────────────────────── */
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

/* ── Virtqueue setup ─────────────────────────────────────────────────────── */
static int vq_setup(uint16_t io_base, uint16_t queue_idx, virtqueue_t *vq) {
    outw(io_base + VIRT_REG_QUEUE_SEL, queue_idx);
    uint16_t qsz = inw(io_base + VIRT_REG_QUEUE_SIZE);
    if (qsz == 0) return -1;

    size_t desc_bytes  = 16 * qsz;
    size_t avail_bytes = 6 + 2 * qsz;
    size_t avail_end   = desc_bytes + avail_bytes;
    size_t pad         = (VQUEUE_ALIGN - (avail_end % VQUEUE_ALIGN)) % VQUEUE_ALIGN;
    size_t used_off    = avail_end + pad;
    size_t used_bytes  = 6 + 8 * qsz;
    size_t total_bytes = used_off + used_bytes;
    size_t total_pages = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uintptr_t q_phys = pmm_alloc_pages(total_pages);
    if (!q_phys) return -1;
    uintptr_t q_virt = vmm_phys_to_virt(q_phys);
    memset((void *)q_virt, 0, total_pages * PAGE_SIZE);

    vq->desc           = (vring_desc_t *)(q_virt);
    vq->avail          = (vring_avail_t *)(q_virt + desc_bytes);
    vq->used           = (vring_used_t  *)(q_virt + used_off);
    vq->queue_size     = qsz;
    vq->last_used_idx  = 0;
    vq->free_head      = 0;
    vq->num_free       = qsz;

    /* Chain free descriptors */
    for (uint16_t i = 0; i < qsz; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = 0;
    }

    outl(io_base + VIRT_REG_QUEUE_PFN, (uint32_t)(q_phys / VQUEUE_ALIGN));
    return 0;
}

/* ── Post an RX buffer to the RX virtqueue ───────────────────────────────── */
static void rx_post_buffer(virtio_net_priv_t *p, int buf_idx) {
    virtqueue_t *vq = &p->rx_vq;
    if (vq->num_free == 0) return;

    uint16_t di = vq->free_head;
    vq->free_head = vq->desc[di].next;
    vq->num_free--;

    vq->desc[di].addr  = p->rx_buf_phys[buf_idx];
    vq->desc[di].len   = RX_BUF_SIZE;
    vq->desc[di].flags = VRING_DESC_F_WRITE;  /* device writes into this */
    vq->desc[di].next  = 0;

    uint16_t avail_slot = vq->avail->idx % vq->queue_size;
    vq->avail->ring[avail_slot] = di;
    mb();
    vq->avail->idx++;
    mb();
}

/* ── Interrupt handler ───────────────────────────────────────────────────── */
/* We store a per-device pointer indexed by IRQ vector */
static virtio_net_priv_t *g_irq_to_priv[256] = {0};

static void virtio_net_isr(cpu_regs_t *regs) {
    (void)regs;

    /* Scan all registered devices sharing legacy IRQ */
    for (int i = 0; i < 256; i++) {
        virtio_net_priv_t *p = g_irq_to_priv[i];
        if (!p) continue;

        /* Read ISR status to acknowledge interrupt */
        uint8_t isr = inb(p->io_base + VIRT_REG_ISR);
        if (!(isr & 0x01)) continue;  /* not for us */

        /* Process received packets */
        virtqueue_t *vq = &p->rx_vq;
        while (vq->last_used_idx != vq->used->idx) {
            uint16_t ui = vq->last_used_idx % vq->queue_size;
            vring_used_elem_t *ue = &vq->used->ring[ui];
            uint16_t desc_idx = (uint16_t)ue->id;
            uint32_t total_len = ue->len;

            /* Find which buffer this descriptor corresponds to */
            uintptr_t buf_phys = vq->desc[desc_idx].addr;
            uint8_t *buf_virt = NULL;
            int buf_idx = -1;
            for (int j = 0; j < RX_RING_SIZE; j++) {
                if (p->rx_buf_phys[j] == buf_phys) {
                    buf_virt = p->rx_buf_virt[j];
                    buf_idx = j;
                    break;
                }
            }

            if (buf_virt && total_len > VIRTIO_NET_HDR_SIZE) {
                /* Skip virtio_net_hdr, dispatch the ethernet frame */
                uint8_t *frame = buf_virt + VIRTIO_NET_HDR_SIZE;
                size_t frame_len = total_len - VIRTIO_NET_HDR_SIZE;
                netdev_rx_dispatch(p->netdev, frame, frame_len);
            }

            /* Return descriptor to free list */
            vq->desc[desc_idx].flags = 0;
            vq->desc[desc_idx].next  = vq->free_head;
            vq->free_head = desc_idx;
            vq->num_free++;

            vq->last_used_idx++;

            /* Re-post the buffer */
            if (buf_idx >= 0)
                rx_post_buffer(p, buf_idx);
        }

        /* Notify device we've consumed RX buffers */
        outw(p->io_base + VIRT_REG_QUEUE_NOTIFY, 0);
    }

    apic_send_eoi();
}

/* ── netdev_ops ─────────────────────────────────────────────────────────── */
static int vnet_send(netdev_t *dev, const void *data, size_t len) {
    virtio_net_priv_t *p = (virtio_net_priv_t *)dev->priv;
    virtqueue_t *vq = &p->tx_vq;

    if (len > ETH_FRAME_MAX) return -1;

    /* Prepare TX bounce buffer: virtio_net_hdr + frame */
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)p->tx_buf_virt;
    memset(hdr, 0, VIRTIO_NET_HDR_SIZE);
    memcpy(p->tx_buf_virt + VIRTIO_NET_HDR_SIZE, data, len);

    size_t total = VIRTIO_NET_HDR_SIZE + len;

    /* Use a single descriptor for the entire packet */
    if (vq->num_free == 0) return -1;
    uint16_t di = vq->free_head;
    vq->free_head = vq->desc[di].next;
    vq->num_free--;

    vq->desc[di].addr  = p->tx_buf_phys;
    vq->desc[di].len   = (uint32_t)total;
    vq->desc[di].flags = 0;  /* device-readable */
    vq->desc[di].next  = 0;

    uint16_t avail_slot = vq->avail->idx % vq->queue_size;
    vq->avail->ring[avail_slot] = di;
    mb();
    vq->avail->idx++;
    mb();

    /* Kick TX queue (queue 1) */
    outw(p->io_base + VIRT_REG_QUEUE_NOTIFY, 1);

    /* Poll for TX completion */
    uint32_t timeout = 10000000;
    while (vq->used->idx == vq->last_used_idx && --timeout)
        __asm__ volatile("pause" ::: "memory");

    if (timeout == 0) {
        KLOG_WARN("virtio-net: TX timeout\n");
        return -1;
    }

    vq->last_used_idx++;

    /* Return descriptor to free list */
    vq->desc[di].next = vq->free_head;
    vq->free_head = di;
    vq->num_free++;

    return 0;
}

static void vnet_get_mac(netdev_t *dev, uint8_t mac[6]) {
    virtio_net_priv_t *p = (virtio_net_priv_t *)dev->priv;
    for (int i = 0; i < 6; i++)
        mac[i] = inb(p->io_base + VIRT_REG_MAC + i);
}

static bool vnet_link_up(netdev_t *dev) {
    (void)dev;
    return true;  /* assume link is always up for now */
}

static netdev_ops_t g_vnet_ops = {
    .send_packet = vnet_send,
    .get_mac     = vnet_get_mac,
    .link_up     = vnet_link_up,
};

/* ── Probe a single PCI device ──────────────────────────────────────────── */
static int vnet_letter = 0;

static int probe_one(pci_device_t *pci) {
    /* Verify this is actually a network device (class 0x02) */
    if (pci->class != 0x02) {
        /* Legacy virtio 0x1000 could be net or blk — check subsystem */
        if (pci->device_id == VIRTIO_DEV_NET_LEGACY) {
            /* Read subsystem device ID to confirm it's a net device */
            uint16_t subsys = pci_read16(pci->bus, pci->dev, pci->fn, 0x2E);
            if (subsys != 1) return -1;  /* subsystem device ID 1 = net */
        } else {
            return -1;
        }
    }

    pci_enable_device(pci);

    /* BAR0 is I/O port for legacy VirtIO */
    if (pci->bars[0].type != PCI_BAR_IO || pci->bars[0].base == 0) {
        KLOG_WARN("virtio-net: BAR0 not I/O — skipping\n");
        return -1;
    }
    uint16_t io = (uint16_t)pci->bars[0].base;

    /* ─── VirtIO device initialisation sequence (legacy) ─── */
    outb(io + VIRT_REG_STATUS, 0);                          /* reset         */
    outb(io + VIRT_REG_STATUS, VIRTIO_STATUS_ACK);
    outb(io + VIRT_REG_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Negotiate features */
    uint32_t dev_feats = inl(io + VIRT_REG_DEV_FEAT);
    uint32_t drv_feats = dev_feats & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
    outl(io + VIRT_REG_DRV_FEAT, drv_feats);

    /* Allocate private state */
    virtio_net_priv_t *priv = kzalloc(sizeof(virtio_net_priv_t));
    if (!priv) return -1;
    priv->io_base = io;

    /* ─── Set up RX virtqueue (queue 0) ─── */
    if (vq_setup(io, 0, &priv->rx_vq) < 0) {
        KLOG_ERR("virtio-net: failed to set up RX queue\n");
        kfree(priv);
        return -1;
    }

    /* ─── Set up TX virtqueue (queue 1) ─── */
    if (vq_setup(io, 1, &priv->tx_vq) < 0) {
        KLOG_ERR("virtio-net: failed to set up TX queue\n");
        kfree(priv);
        return -1;
    }

    /* ─── Allocate RX buffers ─── */
    for (int i = 0; i < RX_RING_SIZE; i++) {
        uintptr_t phys = pmm_alloc_pages(1);
        if (!phys) {
            KLOG_ERR("virtio-net: OOM for RX buffer %d\n", i);
            kfree(priv);
            return -1;
        }
        priv->rx_buf_phys[i] = phys;
        priv->rx_buf_virt[i] = (uint8_t *)vmm_phys_to_virt(phys);
        memset(priv->rx_buf_virt[i], 0, PAGE_SIZE);
    }

    /* ─── Allocate TX bounce buffer ─── */
    priv->tx_buf_phys = pmm_alloc_pages(1);
    if (!priv->tx_buf_phys) {
        KLOG_ERR("virtio-net: OOM for TX buffer\n");
        kfree(priv);
        return -1;
    }
    priv->tx_buf_virt = (uint8_t *)vmm_phys_to_virt(priv->tx_buf_phys);
    memset(priv->tx_buf_virt, 0, PAGE_SIZE);

    /* ─── Register IRQ handler ─── */
    /* Use the PCI IRQ line (legacy interrupt) — typically IRQ 9-11 on q35 */
    uint8_t irq = pci->irq_line;
    uint8_t vec = 0x30 + irq;  /* map to IDT vectors 0x30+ */
    priv->irq_vec = vec;

    g_irq_to_priv[vec] = priv;
    idt_set_handler(vec, isr_stub_table[vec], IDT_INT_GATE, 0);
    idt_register_handler(vec, virtio_net_isr);

    /* Route the PCI IRQ through the I/O APIC to our vector.
     * PCI uses level-triggered, active-low interrupts. */
    if (ioapic_is_init()) {
        ioapic_set_irq(irq, vec, apic_get_id(), IOAPIC_FLAG_PCI);
    }

    KLOG_INFO("virtio-net: IRQ %u → vector 0x%02x\n", irq, vec);

    /* Signal DRIVER_OK */
    outb(io + VIRT_REG_STATUS, VIRTIO_STATUS_ACK
                              | VIRTIO_STATUS_DRIVER
                              | VIRTIO_STATUS_DRIVER_OK);

    /* ─── Pre-post RX buffers ─── */
    for (int i = 0; i < RX_RING_SIZE; i++)
        rx_post_buffer(priv, i);

    /* Kick RX queue to tell device buffers are available */
    outw(io + VIRT_REG_QUEUE_NOTIFY, 0);

    /* ─── Register netdev ─── */
    netdev_t *dev = kzalloc(sizeof(netdev_t));
    if (!dev) { kfree(priv); return -1; }

    ksnprintf(dev->name, NETDEV_NAME_MAX, "vnet%d", vnet_letter++);
    dev->ops  = &g_vnet_ops;
    dev->priv = priv;
    priv->netdev = dev;

    dev->link = true;   /* link is up once DRIVER_OK is set */
    if (netdev_register(dev) < 0) {
        kfree(dev); kfree(priv);
        return -1;
    }

    KLOG_INFO("virtio-net: '%s' ready\n", dev->name);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
int virtio_net_init(void) {
    int found = 0;

    /* Scan all PCI devices for VirtIO net */
    pci_device_t *pci_buf;
    int pci_count = pci_get_devices(&pci_buf);

    for (int i = 0; i < pci_count; i++) {
        pci_device_t *d = &pci_buf[i];
        if (d->vendor_id != VIRTIO_VENDOR) continue;

        /* Legacy transitional device — could be net (subsys 1) or blk (subsys 2) */
        if (d->device_id == VIRTIO_DEV_NET_LEGACY ||
            d->device_id == VIRTIO_DEV_NET_MODERN) {
            if (probe_one(d) == 0) found++;
        }
    }

    if (found == 0)
        KLOG_INFO("virtio-net: no devices found\n");

    return found;
}
