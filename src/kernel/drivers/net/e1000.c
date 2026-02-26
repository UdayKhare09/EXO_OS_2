/* drivers/net/e1000.c — Intel 82540EM (e1000) Gigabit Ethernet driver
 *
 * Implements the Intel 8254x family NIC driver for QEMU's "-device e1000".
 * PCI device: vendor 0x8086, device 0x100E.
 * MMIO via BAR0.  Descriptor-ring RX + TX.  Interrupt-driven receive.
 *
 * Reference: Intel 8254x Software Developer's Manual (SDM)
 */
#include "e1000.h"
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
#include "net/netutil.h"

#include <stdint.h>

/* ── PCI IDs ─────────────────────────────────────────────────────────────── */
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E   /* 82540EM — QEMU default */
#define E1000_DEVICE_ID_I   0x100F   /* 82545EM (copper) */
#define E1000_DEVICE_ID_2   0x10D3   /* 82574L */
#define E1000_DEVICE_ID_3   0x153A   /* I217-LM */

/* ── Register offsets (MMIO from BAR0) ───────────────────────────────────── */
#define E1000_CTRL      0x0000   /* Device Control                           */
#define E1000_STATUS    0x0008   /* Device Status                            */
#define E1000_EERD      0x0014   /* EEPROM Read                              */
#define E1000_ICR       0x00C0   /* Interrupt Cause Read                     */
#define E1000_ITR       0x00C4   /* Interrupt Throttling Rate                */
#define E1000_ICS       0x00C8   /* Interrupt Cause Set                      */
#define E1000_IMS       0x00D0   /* Interrupt Mask Set/Read                  */
#define E1000_IMC       0x00D8   /* Interrupt Mask Clear                     */

/* Receive */
#define E1000_RCTL      0x0100   /* Receive Control                          */
#define E1000_RDBAL     0x2800   /* RX Descriptor Base Address Low           */
#define E1000_RDBAH     0x2804   /* RX Descriptor Base Address High          */
#define E1000_RDLEN     0x2808   /* RX Descriptor Length                     */
#define E1000_RDH       0x2810   /* RX Descriptor Head                       */
#define E1000_RDT       0x2818   /* RX Descriptor Tail                       */

/* Transmit */
#define E1000_TCTL      0x0400   /* Transmit Control                         */
#define E1000_TIPG      0x0410   /* TX Inter-Packet Gap                      */
#define E1000_TDBAL     0x3800   /* TX Descriptor Base Address Low           */
#define E1000_TDBAH     0x3804   /* TX Descriptor Base Address High          */
#define E1000_TDLEN     0x3808   /* TX Descriptor Length                     */
#define E1000_TDH       0x3810   /* TX Descriptor Head                       */
#define E1000_TDT       0x3818   /* TX Descriptor Tail                       */

/* Receive Address */
#define E1000_RAL0      0x5400   /* Receive Address Low  (MAC low 4 bytes)   */
#define E1000_RAH0      0x5404   /* Receive Address High (MAC high 2 + AV)   */

/* Multicast Table Array */
#define E1000_MTA_BASE  0x5200   /* 128 × 32-bit words                       */

/* ── CTRL register bits ──────────────────────────────────────────────────── */
#define CTRL_FD         (1 << 0)    /* Full Duplex                            */
#define CTRL_ASDE       (1 << 5)    /* Auto-Speed Detection Enable            */
#define CTRL_SLU        (1 << 6)    /* Set Link Up                            */
#define CTRL_ILOS       (1 << 7)    /* Invert Loss-of-Signal                  */
#define CTRL_RST        (1 << 26)   /* Device Reset                           */
#define CTRL_VME        (1 << 30)   /* VLAN Mode Enable                       */
#define CTRL_PHY_RST    (1 << 31)   /* PHY Reset                              */

/* ── RCTL register bits ──────────────────────────────────────────────────── */
#define RCTL_EN         (1 << 1)    /* Receiver Enable                        */
#define RCTL_SBP        (1 << 2)    /* Store Bad Packets                      */
#define RCTL_UPE        (1 << 3)    /* Unicast Promisc Enable                 */
#define RCTL_MPE        (1 << 4)    /* Multicast Promisc Enable               */
#define RCTL_LBM_NONE   (0 << 6)   /* No Loopback                            */
#define RCTL_RDMTS_HALF (0 << 8)   /* RX Desc Min Threshold = ½              */
#define RCTL_BAM        (1 << 15)   /* Broadcast Accept Mode                  */
#define RCTL_BSIZE_2048 (0 << 16)   /* Buffer Size = 2048                     */
#define RCTL_BSIZE_4096 ((3 << 16) | (1 << 25))
#define RCTL_SECRC      (1 << 26)   /* Strip Ethernet CRC                     */

/* ── TCTL register bits ──────────────────────────────────────────────────── */
#define TCTL_EN         (1 << 1)    /* Transmitter Enable                     */
#define TCTL_PSP        (1 << 3)    /* Pad Short Packets                      */
#define TCTL_CT_SHIFT   4           /* Collision Threshold                    */
#define TCTL_COLD_SHIFT 12          /* Collision Distance                     */
#define TCTL_RTLC       (1 << 24)   /* Re-transmit on Late Collision          */

/* ── TIPG recommended values (IEEE 802.3) ────────────────────────────────── */
#define TIPG_IPGT       10          /* IPG Transmit Time                      */
#define TIPG_IPGR1      4           /* IPG Receive Time 1 (2/3 of IPGR2)     */
#define TIPG_IPGR2      6           /* IPG Receive Time 2                     */

/* ── Interrupt bits (ICR / IMS / IMC) ────────────────────────────────────── */
#define ICR_TXDW        (1 << 0)    /* TX Descriptor Written Back             */
#define ICR_TXQE        (1 << 1)    /* TX Queue Empty                         */
#define ICR_LSC         (1 << 2)    /* Link Status Change                     */
#define ICR_RXSEQ       (1 << 3)    /* RX Sequence Error                      */
#define ICR_RXDMT0      (1 << 4)    /* RX Desc Minimum Threshold Reached      */
#define ICR_RXO         (1 << 6)    /* RX Overrun                             */
#define ICR_RXT0        (1 << 7)    /* RX Timer Interrupt                     */

/* ── Descriptor ring sizes ───────────────────────────────────────────────── */
#define E1000_NUM_RX_DESC  64
#define E1000_NUM_TX_DESC  64
#define E1000_RX_BUF_SIZE  2048

/* ── RX descriptor (legacy) ──────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;          /* physical address of RX buffer                */
    uint16_t length;        /* length of received data                      */
    uint16_t checksum;      /* packet checksum                              */
    uint8_t  status;        /* status bits                                  */
    uint8_t  errors;        /* error bits                                   */
    uint16_t special;       /* special / VLAN tag                           */
} e1000_rx_desc_t;

#define RXD_STAT_DD     (1 << 0)    /* Descriptor Done                */
#define RXD_STAT_EOP    (1 << 1)    /* End of Packet                  */

/* ── TX descriptor (legacy) ──────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;          /* physical address of TX buffer                */
    uint16_t length;        /* length of data to send                       */
    uint8_t  cso;           /* checksum offset                              */
    uint8_t  cmd;           /* command bits                                 */
    uint8_t  status;        /* status bits (DD)                             */
    uint8_t  css;           /* checksum start                               */
    uint16_t special;       /* special / VLAN tag                           */
} e1000_tx_desc_t;

#define TXD_CMD_EOP     (1 << 0)    /* End of Packet                  */
#define TXD_CMD_IFCS    (1 << 1)    /* Insert FCS/CRC                 */
#define TXD_CMD_RS      (1 << 3)    /* Report Status                  */
#define TXD_STAT_DD     (1 << 0)    /* Descriptor Done                */

/* ── Max e1000 NICs we support simultaneously ────────────────────────────── */
#define E1000_MAX_NICS  4

/* ── Per-device private data ─────────────────────────────────────────────── */
typedef struct {
    volatile uint8_t *mmio;         /* mapped MMIO base                     */
    pci_device_t     *pci;          /* PCI device descriptor                */

    /* RX ring */
    e1000_rx_desc_t  *rx_descs;     /* RX descriptor ring (physically contiguous) */
    uint64_t          rx_descs_phys;
    uint8_t          *rx_bufs[E1000_NUM_RX_DESC]; /* RX buffers (virtual)   */
    uint64_t          rx_bufs_phys[E1000_NUM_RX_DESC]; /* RX buffers (phys) */
    uint16_t          rx_cur;       /* next descriptor to check              */

    /* TX ring */
    e1000_tx_desc_t  *tx_descs;     /* TX descriptor ring (physically contiguous) */
    uint64_t          tx_descs_phys;
    uint8_t          *tx_bufs[E1000_NUM_TX_DESC]; /* TX buffers (virtual)   */
    uint64_t          tx_bufs_phys[E1000_NUM_TX_DESC]; /* TX buffers (phys) */
    uint16_t          tx_cur;       /* next descriptor to use for TX         */

    /* Netdev reference (back-pointer) */
    netdev_t          ndev;         /* embedded netdev                       */

    /* IRQ vector assigned */
    uint8_t           irq_vec;
} e1000_priv_t;

/* ── Static device pool ──────────────────────────────────────────────────── */
static e1000_priv_t g_e1000_devs[E1000_MAX_NICS];
static int          g_e1000_count = 0;

/* ── MMIO register access helpers ────────────────────────────────────────── */
static inline uint32_t e1000_read(e1000_priv_t *e, uint32_t reg)
{
    return *(volatile uint32_t *)(e->mmio + reg);
}

static inline void e1000_write(e1000_priv_t *e, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(e->mmio + reg) = val;
}

/* ── EEPROM read (poll-based) ────────────────────────────────────────────── */
static uint16_t e1000_eeprom_read(e1000_priv_t *e, uint8_t addr)
{
    /* Start read: set address and start bit */
    e1000_write(e, E1000_EERD, ((uint32_t)addr << 8) | 1);

    /* Poll for done (bit 4) */
    uint32_t val;
    for (int i = 0; i < 10000; i++) {
        val = e1000_read(e, E1000_EERD);
        if (val & (1 << 4))
            return (uint16_t)(val >> 16);
    }

    KLOG_WARN("e1000: EEPROM read timeout for addr %u\n", addr);
    return 0;
}

/* ── Read MAC address from EEPROM or RAL/RAH ────────────────────────────── */
static void e1000_read_mac(e1000_priv_t *e, uint8_t mac[6])
{
    /* Try EEPROM first */
    uint16_t w0 = e1000_eeprom_read(e, 0);
    uint16_t w1 = e1000_eeprom_read(e, 1);
    uint16_t w2 = e1000_eeprom_read(e, 2);

    if (w0 || w1 || w2) {
        mac[0] = (uint8_t)(w0 & 0xFF);
        mac[1] = (uint8_t)(w0 >> 8);
        mac[2] = (uint8_t)(w1 & 0xFF);
        mac[3] = (uint8_t)(w1 >> 8);
        mac[4] = (uint8_t)(w2 & 0xFF);
        mac[5] = (uint8_t)(w2 >> 8);
    } else {
        /* Fallback: read from RAL/RAH registers */
        uint32_t ral = e1000_read(e, E1000_RAL0);
        uint32_t rah = e1000_read(e, E1000_RAH0);
        mac[0] = (uint8_t)(ral);
        mac[1] = (uint8_t)(ral >> 8);
        mac[2] = (uint8_t)(ral >> 16);
        mac[3] = (uint8_t)(ral >> 24);
        mac[4] = (uint8_t)(rah);
        mac[5] = (uint8_t)(rah >> 8);
    }

    KLOG_INFO("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── Initialise RX descriptor ring ───────────────────────────────────────── */
static void e1000_init_rx(e1000_priv_t *e)
{
    /* Allocate descriptor ring — must be 128-byte aligned, physically contiguous */
    size_t desc_sz = sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC;
    uint64_t desc_phys = pmm_alloc_pages((desc_sz + 4095) / 4096);
    e->rx_descs = (e1000_rx_desc_t *)vmm_phys_to_virt(desc_phys);
    e->rx_descs_phys = desc_phys;
    memset(e->rx_descs, 0, desc_sz);

    /* Allocate receive buffers */
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t buf_phys = pmm_alloc_pages(1);
        e->rx_bufs[i] = (uint8_t *)vmm_phys_to_virt(buf_phys);
        e->rx_bufs_phys[i] = buf_phys;
        memset(e->rx_bufs[i], 0, E1000_RX_BUF_SIZE);

        e->rx_descs[i].addr   = buf_phys;
        e->rx_descs[i].status = 0;
    }

    /* Program RX descriptor base and length */
    e1000_write(e, E1000_RDBAL, (uint32_t)(desc_phys & 0xFFFFFFFF));
    e1000_write(e, E1000_RDBAH, (uint32_t)(desc_phys >> 32));
    e1000_write(e, E1000_RDLEN, (uint32_t)desc_sz);

    /* Head = 0, Tail = NUM-1 (all descriptors available to HW) */
    e1000_write(e, E1000_RDH, 0);
    e1000_write(e, E1000_RDT, E1000_NUM_RX_DESC - 1);
    e->rx_cur = 0;

    /* Enable receiver: unicast, broadcast, strip CRC, 2048-byte bufs */
    e1000_write(e, E1000_RCTL,
                RCTL_EN | RCTL_BAM | RCTL_SECRC |
                RCTL_BSIZE_2048 | RCTL_RDMTS_HALF | RCTL_LBM_NONE);

    KLOG_INFO("e1000: RX ring: %d descriptors @ phys 0x%lx\n",
              E1000_NUM_RX_DESC, desc_phys);
}

/* ── Initialise TX descriptor ring ───────────────────────────────────────── */
static void e1000_init_tx(e1000_priv_t *e)
{
    /* Allocate descriptor ring — 128-byte aligned */
    size_t desc_sz = sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC;
    uint64_t desc_phys = pmm_alloc_pages((desc_sz + 4095) / 4096);
    e->tx_descs = (e1000_tx_desc_t *)vmm_phys_to_virt(desc_phys);
    e->tx_descs_phys = desc_phys;
    memset(e->tx_descs, 0, desc_sz);

    /* Allocate transmit buffers */
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t buf_phys = pmm_alloc_pages(1);
        e->tx_bufs[i] = (uint8_t *)vmm_phys_to_virt(buf_phys);
        e->tx_bufs_phys[i] = buf_phys;
        memset(e->tx_bufs[i], 0, E1000_RX_BUF_SIZE);

        e->tx_descs[i].addr   = buf_phys;
        e->tx_descs[i].status = TXD_STAT_DD; /* mark as free */
    }

    /* Program TX descriptor base and length */
    e1000_write(e, E1000_TDBAL, (uint32_t)(desc_phys & 0xFFFFFFFF));
    e1000_write(e, E1000_TDBAH, (uint32_t)(desc_phys >> 32));
    e1000_write(e, E1000_TDLEN, (uint32_t)desc_sz);

    /* Head = 0, Tail = 0 (no packets queued) */
    e1000_write(e, E1000_TDH, 0);
    e1000_write(e, E1000_TDT, 0);
    e->tx_cur = 0;

    /* Enable transmitter: pad short packets, collision settings */
    e1000_write(e, E1000_TCTL,
                TCTL_EN | TCTL_PSP |
                (15 << TCTL_CT_SHIFT) |
                (64 << TCTL_COLD_SHIFT) |
                TCTL_RTLC);

    /* Set IPG values per Intel recommendation for 802.3 */
    e1000_write(e, E1000_TIPG,
                TIPG_IPGT | (TIPG_IPGR1 << 10) | (TIPG_IPGR2 << 20));

    KLOG_INFO("e1000: TX ring: %d descriptors @ phys 0x%lx\n",
              E1000_NUM_TX_DESC, desc_phys);
}

/* ── Handle received packets ─────────────────────────────────────────────── */
static void e1000_handle_rx(e1000_priv_t *e)
{
    while (1) {
        uint16_t cur = e->rx_cur;
        e1000_rx_desc_t *desc = &e->rx_descs[cur];

        /* Check if hardware has written this descriptor */
        if (!(desc->status & RXD_STAT_DD))
            break;

        /* Only process complete packets (EOP set, no errors) */
        if ((desc->status & RXD_STAT_EOP) && desc->errors == 0) {
            uint16_t len = desc->length;
            if (len > 0 && len <= E1000_RX_BUF_SIZE) {
                netdev_rx_dispatch(&e->ndev, e->rx_bufs[cur], len);
            }
        }

        /* Reset descriptor for reuse */
        desc->status = 0;
        desc->errors = 0;
        desc->length = 0;

        /* Advance tail to give this descriptor back to hardware */
        e1000_write(e, E1000_RDT, cur);

        /* Move to next descriptor */
        e->rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
    }
}

/* ── ISR: interrupt handler ──────────────────────────────────────────────── */
static void e1000_isr_dev(e1000_priv_t *e)
{
    /* Read and acknowledge interrupts */
    uint32_t icr = e1000_read(e, E1000_ICR);

    if (icr & ICR_RXT0) {
        /* Receive timer — process received packets */
        e1000_handle_rx(e);
    }

    if (icr & (ICR_RXDMT0 | ICR_RXO)) {
        /* Minimum threshold / overrun — also process RX */
        e1000_handle_rx(e);
    }

    if (icr & ICR_LSC) {
        /* Link status change */
        uint32_t status = e1000_read(e, E1000_STATUS);
        e->ndev.link = !!(status & (1 << 1));  /* LU bit */
        KLOG_INFO("e1000: %s link %s\n",
                  e->ndev.name, e->ndev.link ? "up" : "down");
    }
}

/* Global ISR dispatchers — one per possible device (up to 4) */
static void e1000_isr_0(cpu_regs_t *r) { (void)r; if (g_e1000_count > 0) e1000_isr_dev(&g_e1000_devs[0]); apic_send_eoi(); }
static void e1000_isr_1(cpu_regs_t *r) { (void)r; if (g_e1000_count > 1) e1000_isr_dev(&g_e1000_devs[1]); apic_send_eoi(); }
static void e1000_isr_2(cpu_regs_t *r) { (void)r; if (g_e1000_count > 2) e1000_isr_dev(&g_e1000_devs[2]); apic_send_eoi(); }
static void e1000_isr_3(cpu_regs_t *r) { (void)r; if (g_e1000_count > 3) e1000_isr_dev(&g_e1000_devs[3]); apic_send_eoi(); }

static isr_handler_t g_e1000_isrs[] = { e1000_isr_0, e1000_isr_1, e1000_isr_2, e1000_isr_3 };

/* ── Netdev ops ──────────────────────────────────────────────────────────── */

static int e1000_send_packet(netdev_t *dev, const void *data, size_t len)
{
    e1000_priv_t *e = (e1000_priv_t *)dev->priv;
    if (len > ETH_MTU + ETH_HLEN)
        return -1;

    uint16_t cur = e->tx_cur;
    e1000_tx_desc_t *desc = &e->tx_descs[cur];

    /* Wait for previous TX on this slot to complete */
    int timeout = 100000;
    while (!(desc->status & TXD_STAT_DD) && --timeout > 0)
        __asm__ volatile("pause");

    if (timeout <= 0) {
        KLOG_WARN("e1000: TX timeout on desc %u\n", cur);
        return -1;
    }

    /* Copy data to TX buffer */
    memcpy(e->tx_bufs[cur], data, len);

    /* Set up descriptor */
    desc->addr   = e->tx_bufs_phys[cur];
    desc->length = (uint16_t)len;
    desc->cso    = 0;
    desc->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    desc->status = 0;
    desc->css    = 0;
    desc->special = 0;

    /* Advance tail to submit the descriptor */
    e->tx_cur = (cur + 1) % E1000_NUM_TX_DESC;
    e1000_write(e, E1000_TDT, e->tx_cur);

    return 0;
}

static void e1000_get_mac(netdev_t *dev, uint8_t mac_out[6])
{
    memcpy(mac_out, dev->mac, 6);
}

static bool e1000_link_up(netdev_t *dev)
{
    e1000_priv_t *e = (e1000_priv_t *)dev->priv;
    uint32_t status = e1000_read(e, E1000_STATUS);
    return !!(status & (1 << 1));  /* LU (Link Up) bit */
}

static netdev_ops_t g_e1000_ops = {
    .send_packet = e1000_send_packet,
    .get_mac     = e1000_get_mac,
    .link_up     = e1000_link_up,
};

/* ── Reset the device ────────────────────────────────────────────────────── */
static void e1000_reset(e1000_priv_t *e)
{
    /* Disable interrupts first */
    e1000_write(e, E1000_IMC, 0xFFFFFFFF);

    /* Global device reset */
    uint32_t ctrl = e1000_read(e, E1000_CTRL);
    ctrl |= CTRL_RST;
    e1000_write(e, E1000_CTRL, ctrl);

    /* Wait for reset to complete (typically < 1us) */
    for (volatile int i = 0; i < 100000; i++)
        ;

    /* Disable interrupts again after reset */
    e1000_write(e, E1000_IMC, 0xFFFFFFFF);
    e1000_read(e, E1000_ICR);  /* clear pending */
}

/* ── Clear the Multicast Table Array ─────────────────────────────────────── */
static void e1000_clear_mta(e1000_priv_t *e)
{
    for (int i = 0; i < 128; i++)
        e1000_write(e, E1000_MTA_BASE + (i * 4), 0);
}

/* ── Probe and set up a single e1000 NIC ─────────────────────────────────── */
static int e1000_probe_one(pci_device_t *pci_dev, int idx)
{
    if (idx >= E1000_MAX_NICS) {
        KLOG_WARN("e1000: max NICs reached (%d)\n", E1000_MAX_NICS);
        return -1;
    }

    e1000_priv_t *e = &g_e1000_devs[idx];
    memset(e, 0, sizeof(*e));
    e->pci = pci_dev;

    /* Enable bus-mastering and memory space */
    pci_enable_device(pci_dev);

    /* Map MMIO BAR0 */
    if (pci_dev->bars[0].type != PCI_BAR_MMIO || pci_dev->bars[0].base == 0) {
        KLOG_ERR("e1000: BAR0 is not MMIO or not present\n");
        return -1;
    }

    uint64_t bar_phys = pci_dev->bars[0].base;
    uint64_t bar_size = pci_dev->bars[0].size;
    if (bar_size == 0) bar_size = 0x20000;  /* 128KB default for e1000 */

    e->mmio = (volatile uint8_t *)vmm_phys_to_virt(bar_phys);
    KLOG_INFO("e1000: MMIO BAR0 at phys 0x%lx, size 0x%lx\n",
              bar_phys, bar_size);

    /* Reset the device */
    e1000_reset(e);

    /* Set up link: full duplex, set link up */
    uint32_t ctrl = e1000_read(e, E1000_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE | CTRL_FD;
    ctrl &= ~(CTRL_ILOS | CTRL_PHY_RST);
    e1000_write(e, E1000_CTRL, ctrl);

    /* Clear multicast table */
    e1000_clear_mta(e);

    /* Read MAC address */
    uint8_t mac[6];
    e1000_read_mac(e, mac);

    /* Set up RX and TX */
    e1000_init_rx(e);
    e1000_init_tx(e);

    /* Set up IRQ */
    uint8_t irq_line = pci_dev->irq_line;
    uint8_t irq_vec = 0x30 + irq_line;  /* map to IDT vector */
    e->irq_vec = irq_vec;
    extern void *isr_stub_table[];
    idt_set_handler(irq_vec, isr_stub_table[irq_vec], 0x8E, 0);
    idt_register_handler(irq_vec, g_e1000_isrs[idx]);
    if (ioapic_is_init())
        ioapic_set_irq(irq_line, irq_vec, apic_get_id(), 0);
    KLOG_INFO("e1000: IRQ line %u -> vector 0x%02x\n", irq_line, irq_vec);

    /* Enable interrupts: RX timer, link status change, RX min threshold */
    e1000_write(e, E1000_IMS,
                ICR_RXT0 | ICR_LSC | ICR_RXDMT0 | ICR_RXO);

    /* Set up netdev */
    netdev_t *ndev = &e->ndev;
    ksnprintf(ndev->name, NETDEV_NAME_MAX, "eth%d", idx);
    ndev->ops  = &g_e1000_ops;
    ndev->priv = e;
    ndev->mtu  = ETH_MTU;
    memcpy(ndev->mac, mac, 6);

    /* Check link status */
    uint32_t status = e1000_read(e, E1000_STATUS);
    ndev->link = !!(status & (1 << 1));

    /* Register with netdev subsystem */
    if (netdev_register(ndev) < 0) {
        KLOG_ERR("e1000: failed to register netdev %s\n", ndev->name);
        return -1;
    }

    KLOG_INFO("e1000: %s Intel 82540EM registered (link %s)\n",
              ndev->name, ndev->link ? "up" : "down");

    return 0;
}

/* ── Public init: probe all e1000 devices ────────────────────────────────── */
int e1000_init(void)
{
    KLOG_INFO("e1000: scanning PCI for Intel e1000 NICs...\n");

    /* Use the already-scanned PCI device list */
    pci_device_t *pci_devs;
    int pci_count = pci_get_devices(&pci_devs);

    int found = 0;
    for (int i = 0; i < pci_count && found < E1000_MAX_NICS; i++) {
        pci_device_t *d = &pci_devs[i];
        if (d->vendor_id == E1000_VENDOR_ID &&
            (d->device_id == E1000_DEVICE_ID ||
             d->device_id == E1000_DEVICE_ID_I)) {

            KLOG_INFO("e1000: found at PCI %02x:%02x.%x\n",
                      d->bus, d->dev, d->fn);

            if (e1000_probe_one(d, found) == 0)
                found++;
        }
    }

    g_e1000_count = found;

    if (found == 0)
        KLOG_INFO("e1000: no Intel e1000 NICs found\n");
    else
        KLOG_INFO("e1000: %d NIC(s) initialised\n", found);

    return found;
}
