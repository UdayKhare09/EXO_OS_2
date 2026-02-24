/* drivers/storage/ahci.c — AHCI/SATA host controller driver
 *
 * Supports LBA48 DMA read/write via Command List + FIS mechanism.
 * Single-command synchronous (polled) model, no NCQ.
 */
#include "ahci.h"
#include "blkdev.h"
#include "arch/x86_64/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/string.h"

#include <stdint.h>
#include <stdbool.h>

/* ── PCI class codes ─────────────────────────────────────────────────────── */
#define PCI_CLASS_STORAGE         0x01
#define PCI_SUBCLASS_SATA         0x06
#define PCI_PROGIF_AHCI           0x01   /* AHCI 1.0                        */

/* ── AHCI HBA generic registers (ABAR-relative) ─────────────────────────── */
#define AHCI_HBA_CAP    0x00
#define AHCI_HBA_GHC    0x04
#define AHCI_HBA_IS     0x08
#define AHCI_HBA_PI     0x0C
#define AHCI_HBA_VS     0x10

#define AHCI_GHC_AHCI_EN  (1U << 31)
#define AHCI_GHC_RESET    (1U << 0)
#define AHCI_GHC_IE       (1U << 1)

/* ── Port register offsets (from port base = ABAR+0x100+port*0x80) ───────── */
#define POFF_CLB    0x00   /* Command List Base (low 32)  */
#define POFF_CLBU   0x04   /* Command List Base (high 32) */
#define POFF_FB     0x08   /* FIS Base (low 32)           */
#define POFF_FBU    0x0C   /* FIS Base (high 32)          */
#define POFF_IS     0x10   /* Interrupt Status            */
#define POFF_IE     0x14   /* Interrupt Enable            */
#define POFF_CMD    0x18   /* Command and Status          */
#define POFF_TFD    0x20   /* Task File Data              */
#define POFF_SIG    0x24   /* Signature                   */
#define POFF_SSTS   0x28   /* SATA Status                 */
#define POFF_SCTL   0x2C   /* SATA Control                */
#define POFF_SERR   0x30   /* SATA Error                  */
#define POFF_SACT   0x34   /* SATA Active                 */
#define POFF_CI     0x38   /* Command Issue               */

#define PxCMD_ST    (1U << 0)
#define PxCMD_FRE   (1U << 4)
#define PxCMD_POD   (1U << 2)
#define PxCMD_SUD   (1U << 1)
#define PxCMD_FR    (1U << 14)
#define PxCMD_CR    (1U << 15)

/* ── SATA signatures ─────────────────────────────────────────────────────── */
#define SATA_SIG_ATA    0x00000101  /* SATA drive     */
#define SATA_SIG_ATAPI  0xEB140101  /* SATAPI drive   */

/* ── ATA commands ────────────────────────────────────────────────────────── */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_DEV_LBA            0x40

/* ── FIS types ───────────────────────────────────────────────────────────── */
#define FIS_TYPE_REG_H2D  0x27

/* ── AHCI structures ─────────────────────────────────────────────────────── */

/* H2D Register FIS (20 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  fis_type;    /* 0x27                                           */
    uint8_t  pmport_c;    /* bit7 = Command (1) or Control (0)              */
    uint8_t  command;
    uint8_t  featurel;

    uint8_t  lba0;        /* LBA 7:0                                        */
    uint8_t  lba1;        /* LBA 15:8                                       */
    uint8_t  lba2;        /* LBA 23:16                                      */
    uint8_t  device;      /* bit6=LBA mode, bit4=drive select               */

    uint8_t  lba3;        /* LBA 31:24 (ext)                                */
    uint8_t  lba4;        /* LBA 39:32 (ext)                                */
    uint8_t  lba5;        /* LBA 47:40 (ext)                                */
    uint8_t  featureh;

    uint8_t  countl;      /* sector count 7:0                               */
    uint8_t  counth;      /* sector count 15:8                              */
    uint8_t  icc;
    uint8_t  control;

    uint8_t  rsv[4];
} fis_reg_h2d_t;

/* Command List Entry (32 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t cfl_flags;   /* bits[4:0]=CFL, bit6=write, bit7=atapi         */
    uint16_t prdtl;       /* PRDT entry count                               */
    uint32_t prdbc;       /* PRD byte count transferred (filled by HW)      */
    uint32_t ctba;        /* command table base (low)                       */
    uint32_t ctbau;       /* command table base (high)                      */
    uint32_t rsv[4];
} ahci_cmd_hdr_t;

/* PRDT entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dba;         /* data base (low)                                */
    uint32_t dbau;        /* data base (high)                               */
    uint32_t rsv;
    uint32_t dbc_ioc;     /* bits[21:0]=byte_count-1, bit31=IOC             */
} ahci_prdt_t;

/* Command Table */
typedef struct __attribute__((packed)) {
    uint8_t   cfis[64];   /* command FIS                                    */
    uint8_t   acmd[16];   /* ATAPI command (unused for ATA)                 */
    uint8_t   rsv[48];
    ahci_prdt_t prdt[8];  /* up to 8 PRDT entries (4 KB each max here)      */
} ahci_cmd_table_t;

/* ── Per-port private data ───────────────────────────────────────────────── */
typedef struct {
    uintptr_t port_virt;   /* virtual base of port registers                */
    ahci_cmd_hdr_t   *clb; /* command list base (32 slots × 32 B)           */
    uint8_t          *fis; /* received FIS buffer (256 B)                    */
    ahci_cmd_table_t *ctbl;/* command table (1 used, slot 0)                */
    uint64_t capacity;     /* sectors                                        */
} ahci_port_priv_t;

/* ── MMIO accessors ──────────────────────────────────────────────────────── */
static inline uint32_t hba_r32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void hba_w32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}
static inline void port_w32(ahci_port_priv_t *p, uint32_t off, uint32_t val) {
    hba_w32(p->port_virt, off, val);
}
static inline uint32_t port_r32(ahci_port_priv_t *p, uint32_t off) {
    return hba_r32(p->port_virt, off);
}

/* ── Port start/stop DMA ─────────────────────────────────────────────────── */
static void port_stop(ahci_port_priv_t *p) {
    uint32_t cmd = port_r32(p, POFF_CMD);
    cmd &= ~(uint32_t)PxCMD_ST;
    port_w32(p, POFF_CMD, cmd);
    /* Wait for CR to clear */
    for (int i = 0; i < 500; i++) {
        if (!(port_r32(p, POFF_CMD) & PxCMD_CR)) break;
        for (volatile int j = 0; j < 10000; j++) {}
    }
    cmd = port_r32(p, POFF_CMD);
    cmd &= ~(uint32_t)PxCMD_FRE;
    port_w32(p, POFF_CMD, cmd);
    for (int i = 0; i < 500; i++) {
        if (!(port_r32(p, POFF_CMD) & PxCMD_FR)) break;
        for (volatile int j = 0; j < 10000; j++) {}
    }
}

static void port_start(ahci_port_priv_t *p) {
    /* Wait for CR clear */
    while (port_r32(p, POFF_CMD) & PxCMD_CR) {}
    uint32_t cmd = port_r32(p, POFF_CMD);
    cmd |= PxCMD_FRE | PxCMD_ST;
    port_w32(p, POFF_CMD, cmd);
}

/* ── Issue a command and poll for completion ─────────────────────────────── */
static int port_issue_cmd(ahci_port_priv_t *p) {
    /* Clear pending errors */
    port_w32(p, POFF_SERR, port_r32(p, POFF_SERR));
    port_w32(p, POFF_IS,   port_r32(p, POFF_IS));

    /* Issue slot 0 */
    port_w32(p, POFF_CI, 1);

    /* Poll until slot 0 clears */
    uint32_t timeout = 50000000;
    while (--timeout) {
        uint32_t tfd = port_r32(p, POFF_TFD);
        uint32_t ci  = port_r32(p, POFF_CI);
        if (!(ci & 1)) return 0;          /* done */
        if (tfd & 0x01) return -1;        /* BSY=0, ERR=1 → error */
        for (volatile int j = 0; j < 10; j++) {}
    }
    KLOG_WARN("ahci: command timeout\n");
    return -1;
}

/* ── blkdev I/O ──────────────────────────────────────────────────────────── */
static int ahci_blk_rw(blkdev_t *dev, uint64_t lba, uint32_t count,
                        void *buf, bool write) {
    ahci_port_priv_t *p = (ahci_port_priv_t *)dev->priv;
    uint8_t *data = (uint8_t *)buf;

    /* Process in 8-sector (4 KB) chunks to fit our 8-PRDT table */
    for (uint32_t done = 0; done < count; ) {
        uint32_t batch = count - done;
        if (batch > 8) batch = 8;   /* 8 sectors × 512 B = 4 KB per PRDT   */

        /* Build command table */
        ahci_cmd_table_t *ct = p->ctbl;
        memset(ct, 0, sizeof(*ct));

        /* Fill H2D FIS */
        fis_reg_h2d_t *fis = (fis_reg_h2d_t *)ct->cfis;
        fis->fis_type  = FIS_TYPE_REG_H2D;
        fis->pmport_c  = 0x80;   /* command */
        fis->command   = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        fis->device    = ATA_DEV_LBA;

        uint64_t cur_lba = lba + done;
        fis->lba0 = (uint8_t)(cur_lba >>  0);
        fis->lba1 = (uint8_t)(cur_lba >>  8);
        fis->lba2 = (uint8_t)(cur_lba >> 16);
        fis->lba3 = (uint8_t)(cur_lba >> 24);
        fis->lba4 = (uint8_t)(cur_lba >> 32);
        fis->lba5 = (uint8_t)(cur_lba >> 40);
        fis->countl = (uint8_t)(batch & 0xFF);
        fis->counth = (uint8_t)((batch >> 8) & 0xFF);

        /* One PRDT per sector (512 B each, max 4 B alignment) */
        for (uint32_t i = 0; i < batch; i++) {
            uintptr_t dphys = vmm_virt_to_phys(
                (uintptr_t)(data + (done + i) * 512));
            ct->prdt[i].dba     = (uint32_t)(dphys & 0xFFFFFFFF);
            ct->prdt[i].dbau    = (uint32_t)(dphys >> 32);
            ct->prdt[i].rsv     = 0;
            ct->prdt[i].dbc_ioc = (512 - 1);   /* 511 = 512 B */
        }

        /* Command list entry (slot 0) */
        ahci_cmd_hdr_t *hdr = &p->clb[0];
        memset(hdr, 0, sizeof(*hdr));
        uint16_t cfl = sizeof(fis_reg_h2d_t) / 4;   /* FIS length in DWORDs */
        hdr->cfl_flags = cfl | (write ? (1 << 6) : 0);
        hdr->prdtl     = (uint16_t)batch;

        uintptr_t ct_phys = vmm_virt_to_phys((uintptr_t)ct);
        hdr->ctba  = (uint32_t)(ct_phys & 0xFFFFFFFF);
        hdr->ctbau = (uint32_t)(ct_phys >> 32);

        if (port_issue_cmd(p) < 0) return -1;
        done += batch;
    }
    return 0;
}

static int ahci_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    return ahci_blk_rw(dev, lba, count, buf, false);
}
static int ahci_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                       const void *buf) {
    return ahci_blk_rw(dev, lba, count, (void *)buf, true);
}
static uint64_t ahci_block_count(blkdev_t *dev) {
    return ((ahci_port_priv_t *)dev->priv)->capacity;
}
static uint32_t ahci_block_size(blkdev_t *dev) { (void)dev; return 512; }
static int      ahci_flush(blkdev_t *dev)       { (void)dev; return 0; }

static blkdev_ops_t g_ahci_ops = {
    .read_blocks     = ahci_read,
    .write_blocks    = ahci_write,
    .get_block_count = ahci_block_count,
    .get_block_size  = ahci_block_size,
    .flush           = ahci_flush,
};

/* ── Port initialisation ─────────────────────────────────────────────────── */
static int ahci_port_letter = 0; /* 0='a' → "sda" */

static int init_port(uintptr_t abar_virt, int port_idx) {
    uintptr_t port_base = abar_virt + 0x100 + (uint32_t)port_idx * 0x80;

    /* Check SATA device present */
    uint32_t ssts = hba_r32(port_base, POFF_SSTS);
    uint8_t det = ssts & 0x0F;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    if (det != 3 || ipm != 1) return -1;  /* no device / not active */

    uint32_t sig = hba_r32(port_base, POFF_SIG);
    if (sig != SATA_SIG_ATA) return -1;   /* not a plain ATA disk */

    /* Allocate memory for CLB (1 KB), FIS (256 B), command table (1 KB) */
    uintptr_t clb_phys  = pmm_alloc_pages(1);
    uintptr_t fis_phys  = pmm_alloc_pages(1);
    uintptr_t ctbl_phys = pmm_alloc_pages(1);
    if (!clb_phys || !fis_phys || !ctbl_phys) return -1;

    memset((void *)vmm_phys_to_virt(clb_phys),  0, PAGE_SIZE);
    memset((void *)vmm_phys_to_virt(fis_phys),  0, PAGE_SIZE);
    memset((void *)vmm_phys_to_virt(ctbl_phys), 0, PAGE_SIZE);

    ahci_port_priv_t *priv = kzalloc(sizeof(ahci_port_priv_t));
    if (!priv) return -1;

    priv->port_virt = port_base;
    priv->clb  = (ahci_cmd_hdr_t  *)vmm_phys_to_virt(clb_phys);
    priv->fis  = (uint8_t         *)vmm_phys_to_virt(fis_phys);
    priv->ctbl = (ahci_cmd_table_t*)vmm_phys_to_virt(ctbl_phys);

    /* Point CLB slot 0 command table to ctbl_phys */
    priv->clb[0].ctba  = (uint32_t)(ctbl_phys & 0xFFFFFFFF);
    priv->clb[0].ctbau = (uint32_t)(ctbl_phys >> 32);

    /* Stop port, set CLB + FB, restart */
    port_stop(priv);

    hba_w32(port_base, POFF_CLB,  (uint32_t)(clb_phys & 0xFFFFFFFF));
    hba_w32(port_base, POFF_CLBU, (uint32_t)(clb_phys >> 32));
    hba_w32(port_base, POFF_FB,   (uint32_t)(fis_phys & 0xFFFFFFFF));
    hba_w32(port_base, POFF_FBU,  (uint32_t)(fis_phys >> 32));

    /* Clear pending errors & interrupts */
    hba_w32(port_base, POFF_SERR, 0xFFFFFFFF);
    hba_w32(port_base, POFF_IS,   0xFFFFFFFF);

    port_start(priv);

    /* ── ATA IDENTIFY ── */
    uint16_t id_buf[256] = {0};
    uintptr_t id_phys = vmm_virt_to_phys((uintptr_t)id_buf);

    memset(priv->ctbl, 0, sizeof(ahci_cmd_table_t));
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)priv->ctbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;
    fis->countl   = 1;

    priv->ctbl->prdt[0].dba     = (uint32_t)(id_phys & 0xFFFFFFFF);
    priv->ctbl->prdt[0].dbau    = (uint32_t)(id_phys >> 32);
    priv->ctbl->prdt[0].dbc_ioc = 511;  /* 512 - 1 */

    priv->clb[0].cfl_flags = sizeof(fis_reg_h2d_t) / 4;
    priv->clb[0].prdtl     = 1;
    priv->clb[0].ctba  = (uint32_t)(ctbl_phys & 0xFFFFFFFF);
    priv->clb[0].ctbau = (uint32_t)(ctbl_phys >> 32);

    if (port_issue_cmd(priv) < 0) {
        kfree(priv);
        return -1;
    }

    /* IDENTIFY data word 100-103 = LBA48 sector count */
    uint64_t cap =
        (uint64_t)id_buf[100]        |
        ((uint64_t)id_buf[101] << 16)|
        ((uint64_t)id_buf[102] << 32)|
        ((uint64_t)id_buf[103] << 48);
    if (cap == 0) {
        /* Fall back to words 60-61 (LBA28) */
        cap = (uint64_t)id_buf[60] | ((uint64_t)id_buf[61] << 16);
    }
    priv->capacity = cap;

    /* Model string: words 27-46, big-endian byte-swapped */
    char model[42] = {0};
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id_buf[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id_buf[27 + i] & 0xFF);
    }
    /* trim trailing spaces */
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';

    /* Register blkdev */
    blkdev_t *dev = kzalloc(sizeof(blkdev_t));
    if (!dev) { kfree(priv); return -1; }

    dev->name[0] = 's';
    dev->name[1] = 'd';
    dev->name[2] = (char)('a' + ahci_port_letter++);
    dev->name[3] = '\0';
    dev->ops  = &g_ahci_ops;
    dev->priv = priv;

    if (blkdev_register(dev) < 0) { kfree(dev); kfree(priv); return -1; }

    KLOG_INFO("ahci: port%d '%s' '%s' cap=%llu MiB\n",
              port_idx, dev->name, model,
              (unsigned long long)(cap / 2048));
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
int ahci_init(void) {
    int registered = 0;

    /* Scan all PCI devices for AHCI controllers */
    int n = 0;
    blkdev_t *tmp; (void)tmp; /* just to avoid unused-var */

    /* We need to enumerate PCI devices manually here as pci_find only returns
     * one device. Walk the PCI table via known API. */
    /* Use pci_find with class-based scan: loop by trying to find AHCI HBA */
    /* EXO_OS pci_enumerate fills a static table; we iterate all 64 slots. */
    extern int pci_enumerate(pci_device_t *out, int max);
    #define AHCI_MAX_HBA 4
    pci_device_t ahci_devs[AHCI_MAX_HBA];
    int found = 0;

    /* We can't easily re-enumerate, so use a small re-scan */
    pci_device_t scan_buf[64];
    int cnt = pci_enumerate(scan_buf, 64);
    for (int i = 0; i < cnt; i++) {
        pci_device_t *d = &scan_buf[i];
        if (d->class == PCI_CLASS_STORAGE && d->subclass == PCI_SUBCLASS_SATA) {
            if (found < AHCI_MAX_HBA)
                ahci_devs[found++] = *d;
        }
    }

    if (found == 0) {
        KLOG_INFO("ahci: no AHCI controller found\n");
        return 0;
    }

    for (int h = 0; h < found; h++) {
        pci_device_t *hba_pci = &ahci_devs[h];
        pci_enable_device(hba_pci);

        /* AHCI BAR5 = ABAR */
        if (hba_pci->bars[5].type != PCI_BAR_MMIO || hba_pci->bars[5].base == 0)
            continue;

        uintptr_t abar_phys = hba_pci->bars[5].base;
        size_t    abar_size = hba_pci->bars[5].size;
        if (abar_size == 0) abar_size = 0x1100;

        uintptr_t abar = vmm_mmio_map(abar_phys, abar_size);
        if (!abar) continue;

        /* Enable AHCI global mode + GHC.AE */
        uint32_t ghc = hba_r32(abar, AHCI_HBA_GHC);
        ghc |= AHCI_GHC_AHCI_EN;
        hba_w32(abar, AHCI_HBA_GHC, ghc);

        /* Reset HBA */
        ghc |= AHCI_GHC_RESET;
        hba_w32(abar, AHCI_HBA_GHC, ghc);
        uint32_t timeout = 1000000;
        while (--timeout && (hba_r32(abar, AHCI_HBA_GHC) & AHCI_GHC_RESET)) {}

        /* Re-enable AHCI */
        ghc = hba_r32(abar, AHCI_HBA_GHC);
        ghc |= AHCI_GHC_AHCI_EN;
        hba_w32(abar, AHCI_HBA_GHC, ghc);

        /* Probe implemented ports */
        uint32_t pi = hba_r32(abar, AHCI_HBA_PI);
        for (int p = 0; p < 32; p++) {
            if (pi & (1U << p)) {
                if (init_port(abar, p) == 0)
                    registered++;
            }
        }
    }

    if (registered == 0)
        KLOG_INFO("ahci: controller present but no drives found\n");

    return registered;
}
