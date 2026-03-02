/* drivers/storage/virtio_blk.c — VirtIO block device driver (legacy PCI)
 *
 * Implements the VirtIO 0.9 (legacy) block interface via I/O port BAR0.
 * Queue depth: 1 (synchronous, polled completion).
 *
 * Register layout (legacy VirtIO I/O BAR):
 *   +0x00  uint32  device_features (r)
 *   +0x04  uint32  guest_features  (w)
 *   +0x08  uint32  queue_pfn       (w)
 *   +0x0C  uint16  queue_size      (r)
 *   +0x0E  uint16  queue_select    (w)
 *   +0x10  uint16  queue_notify    (w)
 *   +0x12  uint8   device_status   (r/w)
 *   +0x13  uint8   isr_status      (r)
 *   +0x14  … device-specific config
 *
 * Device-specific config (block):
 *   +0x14  uint64  capacity (in 512-byte sectors)
 *   +0x1C  uint32  size_max
 *   +0x20  uint32  seg_max
 *   +0x24  …
 *   +0x34  uint32  blk_size
 */
#include "virtio_blk.h"
#include "blkdev.h"
#include "arch/x86_64/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/spinlock.h"
#include "lib/string.h"

#include <stdint.h>

/* ── PCI IDs ─────────────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_DEV_BLK_LEGACY 0x1001
#define VIRTIO_DEV_BLK_MODERN 0x1042

/* ── Legacy VirtIO I/O register offsets ─────────────────────────────────── */
#define VIRT_REG_DEV_FEAT     0x00
#define VIRT_REG_DRV_FEAT     0x04
#define VIRT_REG_QUEUE_PFN    0x08
#define VIRT_REG_QUEUE_SIZE   0x0C
#define VIRT_REG_QUEUE_SEL    0x0E
#define VIRT_REG_QUEUE_NOTIFY 0x10
#define VIRT_REG_STATUS       0x12
#define VIRT_REG_ISR          0x13
#define VIRT_REG_CAPACITY     0x14   /* 64-bit in two 32-bit reads           */

/* ── Device status flags ─────────────────────────────────────────────────── */
#define VIRTIO_STATUS_ACK         0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FEAT_OK     0x08
#define VIRTIO_STATUS_FAILED      0x80

/* ── Virtqueue descriptor flags ─────────────────────────────────────────── */
#define VRING_DESC_F_NEXT         0x01
#define VRING_DESC_F_WRITE        0x02   /* device-writable (from guest view) */

/* ── VirtIO block request types ─────────────────────────────────────────── */
#define VIRTIO_BLK_T_IN           0      /* read from device                 */
#define VIRTIO_BLK_T_OUT          1      /* write to device                  */
#define VIRTIO_BLK_T_FLUSH        4      /* flush                            */

/* ── Virtqueue ring sizes (power of 2) ──────────────────────────────────── */
#define VQUEUE_MAX_SIZE 256        /* max descriptor count for struct arrays */
#define VQUEUE_ALIGN   4096        /* VirtIO page size for queue alignment    */

/* ── VirtIO structures ───────────────────────────────────────────────────── */
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

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_hdr_t;

/* ── DMA bounce buffer layout (one PMM page, all offsets known) ─────────── */
/* Offset   0: virtio_blk_req_hdr_t (16 bytes) — request header             */
/* Offset  16: uint8_t status        (1 byte)  — completion status           */
/* Offset 512: uint8_t data[512]               — sector data bounce buffer   */
#define DMA_OFF_HDR    0
#define DMA_OFF_STATUS 16
#define DMA_OFF_DATA   512

/* ── Per-device private state ────────────────────────────────────────────── */
typedef struct {
    uint16_t io_base;            /* I/O port BAR0 base                       */

    /* Virtqueue memory (physically contiguous, aligned to VQUEUE_ALIGN) */
    vring_desc_t  *desc;         /* descriptor table [queue_size]            */
    vring_avail_t *avail;        /* available ring                           */
    vring_used_t  *used;         /* used ring                                */

    uint16_t queue_size;         /* actual device queue size (power of 2)    */
    uint16_t last_used_idx;      /* last consumed used ring entry            */

    uint64_t capacity;           /* device capacity in 512-byte sectors      */

    /* DMA bounce buffer — allocated via PMM so physical address is HHDM */
    uintptr_t dma_phys;          /* physical address of bounce page          */
    uint8_t  *dma_virt;          /* virtual address  of bounce page          */

    /* Queue + bounce buffer are single-instance; all requests must serialize. */
    spinlock_t io_lock;
} virtio_blk_priv_t;

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
static inline void io_wait(void) {
    __asm__ volatile ("pause" ::: "memory");
}
static inline void mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

/* ── blkdev ops ─────────────────────────────────────────────────────────── */
static int virtio_blk_rw(blkdev_t *dev, uint64_t lba, uint32_t count,
                          void *buf, bool write) {
    virtio_blk_priv_t *p = (virtio_blk_priv_t *)dev->priv;
    uint8_t *data = (uint8_t *)buf;

    spinlock_acquire(&p->io_lock);

    /* DMA bounce buffer pointers at known physical addresses */
    virtio_blk_req_hdr_t *hdr    = (virtio_blk_req_hdr_t *)(p->dma_virt + DMA_OFF_HDR);
    uint8_t              *status = p->dma_virt + DMA_OFF_STATUS;
    uint8_t              *dma_data = p->dma_virt + DMA_OFF_DATA;

    uintptr_t hdr_phys    = p->dma_phys + DMA_OFF_HDR;
    uintptr_t status_phys = p->dma_phys + DMA_OFF_STATUS;
    uintptr_t data_phys   = p->dma_phys + DMA_OFF_DATA;

    for (uint32_t s = 0; s < count; s++) {
        /* Descriptor indices: fixed 3-descriptor chain in slots 0, 1, 2 */
        uint16_t d0 = 0, d1 = 1, d2 = 2;

        /* Fill request header in bounce buffer */
        hdr->type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
        hdr->reserved = 0;
        hdr->sector   = lba + s;
        *status       = 0xFF; /* sentinel — device sets 0=OK */

        /* For writes, copy caller data into bounce buffer */
        if (write)
            memcpy(dma_data, data + s * 512, 512);

        /* Descriptor 0: request header (device-readable) */
        p->desc[d0].addr  = hdr_phys;
        p->desc[d0].len   = sizeof(virtio_blk_req_hdr_t);
        p->desc[d0].flags = VRING_DESC_F_NEXT;
        p->desc[d0].next  = d1;

        /* Descriptor 1: data buffer (device-writable for reads) */
        p->desc[d1].addr  = data_phys;
        p->desc[d1].len   = 512;
        p->desc[d1].flags = VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE);
        p->desc[d1].next  = d2;

        /* Descriptor 2: status byte (always device-writable) */
        p->desc[d2].addr  = status_phys;
        p->desc[d2].len   = 1;
        p->desc[d2].flags = VRING_DESC_F_WRITE;
        p->desc[d2].next  = 0;

        /* Post descriptor chain head to available ring */
        uint16_t avail_slot = p->avail->idx % p->queue_size;
        mb();
        p->avail->ring[avail_slot] = d0;
        mb();
        p->avail->idx++;
        mb();

        /* Kick device (queue 0) */
        outw(p->io_base + VIRT_REG_QUEUE_NOTIFY, 0);

        /* Poll used ring for completion */
        uint32_t timeout = 10000000;
        while (p->used->idx == p->last_used_idx && --timeout)
            io_wait();

        int ret = 0;
        if (timeout == 0) {
            KLOG_WARN("virtio-blk: I/O timeout lba=%llu\n",
                      (unsigned long long)(lba + s));
            ret = -1;
        } else {
            p->last_used_idx++;
            if (*status != 0) {
                KLOG_WARN("virtio-blk: I/O error lba=%llu status=%u\n",
                          (unsigned long long)(lba + s), *status);
                ret = -1;
            } else if (!write) {
                /* Copy bounce buffer data into caller's buffer */
                memcpy(data + s * 512, dma_data, 512);
            }
        }

        if (ret < 0) {
            spinlock_release(&p->io_lock);
            return -1;
        }
    }

    spinlock_release(&p->io_lock);
    return 0;
}

static int vblk_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    return virtio_blk_rw(dev, lba, count, buf, false);
}

static int vblk_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                       const void *buf) {
    return virtio_blk_rw(dev, lba, count, (void *)buf, true);
}

static uint64_t vblk_block_count(blkdev_t *dev) {
    return ((virtio_blk_priv_t *)dev->priv)->capacity;
}

static uint32_t vblk_block_size(blkdev_t *dev) {
    (void)dev;
    return 512;
}

static int vblk_flush(blkdev_t *dev) {
    virtio_blk_priv_t *p = (virtio_blk_priv_t *)dev->priv;
    spinlock_acquire(&p->io_lock);

    /* FLUSH uses only 2 descriptors: header + status (no data) */
    uint16_t d0 = 0, d1 = 1;

    virtio_blk_req_hdr_t *hdr = (virtio_blk_req_hdr_t *)(p->dma_virt + DMA_OFF_HDR);
    uint8_t *status           = p->dma_virt + DMA_OFF_STATUS;
    hdr->type     = VIRTIO_BLK_T_FLUSH;
    hdr->reserved = 0;
    hdr->sector   = 0;
    *status       = 0xFF;

    p->desc[d0].addr  = p->dma_phys + DMA_OFF_HDR;
    p->desc[d0].len   = sizeof(virtio_blk_req_hdr_t);
    p->desc[d0].flags = VRING_DESC_F_NEXT;
    p->desc[d0].next  = d1;

    p->desc[d1].addr  = p->dma_phys + DMA_OFF_STATUS;
    p->desc[d1].len   = 1;
    p->desc[d1].flags = VRING_DESC_F_WRITE;
    p->desc[d1].next  = 0;

    mb();
    p->avail->ring[p->avail->idx % p->queue_size] = d0;
    mb();
    p->avail->idx++;
    mb();
    outw(p->io_base + VIRT_REG_QUEUE_NOTIFY, 0);

    uint32_t timeout = 10000000;
    while (p->used->idx == p->last_used_idx && --timeout) io_wait();
    if (timeout) p->last_used_idx++;
    spinlock_release(&p->io_lock);
    return (timeout ? 0 : -1);
}

static blkdev_ops_t g_vblk_ops = {
    .read_blocks     = vblk_read,
    .write_blocks    = vblk_write,
    .get_block_count = vblk_block_count,
    .get_block_size  = vblk_block_size,
    .flush           = vblk_flush,
};

/* ── Probe a single PCI device ──────────────────────────────────────────── */
static int vblk_letter = 0; /* 0='a', 1='b', … */

static int probe_one(pci_device_t *pci) {
    pci_enable_device(pci);

    /* BAR0 is I/O port for legacy VirtIO */
    if (pci->bars[0].type != PCI_BAR_IO || pci->bars[0].base == 0) {
        KLOG_WARN("virtio-blk: BAR0 not I/O — skipping\n");
        return -1;
    }
    uint16_t io = (uint16_t)pci->bars[0].base;

    /* ─── VirtIO device initialisation sequence (legacy) ─── */
    outb(io + VIRT_REG_STATUS, 0);                          /* reset         */
    outb(io + VIRT_REG_STATUS, VIRTIO_STATUS_ACK);          /* acknowledge   */
    outb(io + VIRT_REG_STATUS, VIRTIO_STATUS_ACK
                              | VIRTIO_STATUS_DRIVER);       /* have driver   */

    /* Negotiate features: accept basic subset (no indirect desc, no event idx) */
    uint32_t dev_feats = inl(io + VIRT_REG_DEV_FEAT);
    outl(io + VIRT_REG_DRV_FEAT, dev_feats & 0x00FFFFFF);  /* basic subset  */

    /* Read capacity BEFORE setting DRIVER_OK (legacy: cap is always readable) */
    uint32_t cap_lo = inl(io + VIRT_REG_CAPACITY);
    uint32_t cap_hi = inl(io + VIRT_REG_CAPACITY + 4);
    uint64_t capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    /* ─── Set up virtqueue 0 ─── */
    outw(io + VIRT_REG_QUEUE_SEL, 0);
    uint16_t qsz = inw(io + VIRT_REG_QUEUE_SIZE);
    if (qsz == 0) {
        KLOG_ERR("virtio-blk: queue 0 size is 0\n");
        return -1;
    }
    if (qsz > VQUEUE_MAX_SIZE) {
        KLOG_ERR("virtio-blk: queue size %u exceeds driver max %u\n",
                 qsz, (unsigned)VQUEUE_MAX_SIZE);
        return -1;
    }

    /* Virtqueue layout (page-aligned):
     *   [desc table] [avail ring] [<pad to page>] [used ring]
     */
    size_t desc_bytes  = 16 * qsz;
    size_t avail_bytes = 6 + 2 * qsz;
    size_t avail_end   = desc_bytes + avail_bytes;
    size_t pad         = (VQUEUE_ALIGN - (avail_end % VQUEUE_ALIGN)) % VQUEUE_ALIGN;
    size_t used_off    = avail_end + pad;
    size_t used_bytes  = 6 + 8 * qsz;
    size_t total_bytes = used_off + used_bytes;

    size_t total_pages = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t q_phys   = pmm_alloc_pages(total_pages);
    if (!q_phys) {
        KLOG_ERR("virtio-blk: OOM for virtqueue\n");
        return -1;
    }
    uintptr_t q_virt = vmm_phys_to_virt(q_phys);
    memset((void *)q_virt, 0, total_pages * PAGE_SIZE);

    virtio_blk_priv_t *priv = kzalloc(sizeof(virtio_blk_priv_t));
    if (!priv) { pmm_free_pages(q_phys, total_pages); return -1; }

    /* Allocate DMA bounce buffer (1 page via PMM → HHDM virtual address) */
    uintptr_t dma_phys = pmm_alloc_pages(1);
    if (!dma_phys) {
        KLOG_ERR("virtio-blk: OOM for DMA bounce buffer\n");
        kfree(priv);
        pmm_free_pages(q_phys, total_pages);
        return -1;
    }
    uint8_t *dma_virt = (uint8_t *)vmm_phys_to_virt(dma_phys);
    memset(dma_virt, 0, PAGE_SIZE);

    priv->io_base        = io;
    priv->desc           = (vring_desc_t *)(q_virt);
    priv->avail          = (vring_avail_t *)(q_virt + desc_bytes);
    priv->used           = (vring_used_t  *)(q_virt + used_off);
    priv->queue_size     = qsz;
    priv->last_used_idx  = 0;
    priv->capacity       = capacity;
    priv->dma_phys       = dma_phys;
    priv->dma_virt       = dma_virt;
    spinlock_init(&priv->io_lock);

    /* Write queue PFN (in VirtIO pages = 4096 B) */
    outl(io + VIRT_REG_QUEUE_PFN, (uint32_t)(q_phys / VQUEUE_ALIGN));

    /* NOW signal DRIVER_OK — queue is configured, device may process requests */
    outb(io + VIRT_REG_STATUS, VIRTIO_STATUS_ACK
                              | VIRTIO_STATUS_DRIVER
                              | VIRTIO_STATUS_DRIVER_OK);

    /* ─── Register blkdev ─── */
    blkdev_t *dev = kzalloc(sizeof(blkdev_t));
    if (!dev) { kfree(priv); pmm_free_pages(q_phys, total_pages); return -1; }

    dev->name[0] = 'v';
    dev->name[1] = 'd';
    dev->name[2] = (char)('a' + vblk_letter++);
    dev->name[3] = '\0';
    dev->ops     = &g_vblk_ops;
    dev->priv    = priv;

    if (blkdev_register(dev) < 0) {
        kfree(dev); kfree(priv);
        pmm_free_pages(q_phys, total_pages);
        return -1;
    }

    KLOG_INFO("virtio-blk: '%s' capacity=%llu MiB\n",
              dev->name,
              (unsigned long long)(capacity / 2048));
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
int virtio_blk_init(void) {
    int found = 0;

    pci_device_t *d;
    d = pci_find(VIRTIO_VENDOR, VIRTIO_DEV_BLK_LEGACY);
    if (d && probe_one(d) == 0) found++;

    d = pci_find(VIRTIO_VENDOR, VIRTIO_DEV_BLK_MODERN);
    if (d && probe_one(d) == 0) found++;

    if (found == 0)
        KLOG_INFO("virtio-blk: no devices found\n");

    return found;
}
