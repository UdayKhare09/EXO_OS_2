/* drivers/usb/xhci.c — xHCI host controller driver
 *
 * Non-blocking design:
 *   IRQ handler   → xhci_irq() → sched_unblock(g_usb_event_task)  (one line)
 *   USB event task→ xhci_process_events() → HID callbacks → input ring
 *
 * Memory model:
 *   All DMA buffers allocated from PMM (physically contiguous), mapped via
 *   vmm_phys_to_virt (HHDM).  Physical addresses passed directly to hardware.
 */
#include "xhci.h"
#include "arch/x86_64/pci.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "sched/task.h"
#include "sched/sched.h"

/* ─── Forward declarations (USB core callbacks) ─────────────────────────── */
extern void usb_on_port_connected   (uint8_t port, uint8_t speed);
extern void usb_on_transfer_complete(uint8_t slot, uint8_t ep_id,
                                     uint8_t cc, uint32_t residual);

/* ─── Global state ───────────────────────────────────────────────────────── */
xhci_t       g_xhci;
struct task *g_usb_event_task = NULL;

/* ─── MMIO helpers ───────────────────────────────────────────────────────── */
static inline uint32_t cap_read32(uint32_t off) {
    return *(volatile uint32_t *)(g_xhci.cap_base + off);
}
static inline uint32_t op_read32(uint32_t off) {
    return *(volatile uint32_t *)(g_xhci.op_base + off);
}
static inline void op_write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_xhci.op_base + off) = v;
}
static inline uint64_t op_read64(uint32_t off) {
    uint64_t lo = op_read32(off);
    uint64_t hi = op_read32(off + 4);
    return lo | (hi << 32);
}
static inline void op_write64(uint32_t off, uint64_t v) {
    op_write32(off,     (uint32_t)(v & 0xFFFFFFFFu));
    op_write32(off + 4, (uint32_t)(v >> 32));
}
static inline uint32_t rt_ir_read32(uint32_t off) {
    return *(volatile uint32_t *)(g_xhci.rt_base + XHCI_RT_IR_BASE + off);
}
static inline void rt_ir_write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_xhci.rt_base + XHCI_RT_IR_BASE + off) = v;
}
static inline void rt_ir_write64(uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(g_xhci.rt_base + XHCI_RT_IR_BASE + off    ) = (uint32_t)(v & 0xFFFFFFFF);
    *(volatile uint32_t *)(g_xhci.rt_base + XHCI_RT_IR_BASE + off + 4) = (uint32_t)(v >> 32);
}
static inline uint32_t portsc_read(uint8_t port) {
    return op_read32(XHCI_PORT_BASE + (uint32_t)(port - 1) * XHCI_PORT_STRIDE);
}
static inline void portsc_write(uint8_t port, uint32_t v) {
    op_write32(XHCI_PORT_BASE + (uint32_t)(port - 1) * XHCI_PORT_STRIDE, v);
}

/* ─── DMA page allocation ────────────────────────────────────────────────── */
/* Allocates `npages` physically contiguous pages, zeroed, returns virt+phys */
static void *alloc_dma(size_t npages, uintptr_t *phys_out) {
    uintptr_t phys = pmm_alloc_pages(npages);
    if (!phys) return NULL;
    uintptr_t virt = vmm_phys_to_virt(phys);
    memset((void *)virt, 0, npages * PAGE_SIZE);
    *phys_out = phys;
    return (void *)virt;
}

/* ─── Ring management ────────────────────────────────────────────────────── */
static bool ring_alloc(xhci_ring_t *r, uint32_t size) {
    size_t pages = ((size_t)size * sizeof(xhci_trb_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    r->trbs    = alloc_dma(pages, &r->phys);
    if (!r->trbs) return false;
    r->enqueue = 0;
    r->dequeue = 0;
    r->cycle   = 1;
    r->size    = size;
    return true;
}

/* Write one TRB into a producer ring (command ring or transfer ring).
 * Automatically inserts a Link TRB at the end and toggles cycle state.
 * Returns physical address of the TRB written.                             */
static uintptr_t ring_enqueue(xhci_ring_t *r, const xhci_trb_t *trb) {
    /* Leave room for Link TRB at slot [size-1] */
    if (r->enqueue >= r->size - 1) {
        /* Write Link TRB pointing back to segment start */
        xhci_trb_t link = {0};
        link.parameter = r->phys;
        link.control   = TRB_TYPE(TTRB_LINK) | TRB_TC | (r->cycle ? TRB_C : 0);
        r->trbs[r->size - 1] = link;
        cpu_mfence();
        r->enqueue = 0;
        r->cycle  ^= 1;   /* toggle cycle bit after link */
    }

    uintptr_t phys = r->phys + r->enqueue * sizeof(xhci_trb_t);

    /* Write TRB with current cycle bit */
    xhci_trb_t t   = *trb;
    t.control      &= ~TRB_C;
    t.control      |= (r->cycle ? TRB_C : 0u);
    r->trbs[r->enqueue] = t;
    cpu_mfence();
    r->enqueue++;
    return phys;
}

/* Consume one TRB from the event ring; returns false when ring is empty.   */
static bool evt_dequeue(xhci_ring_t *r, xhci_trb_t *out) {
    xhci_trb_t *trb = &r->trbs[r->dequeue];
    cpu_mfence();
    /* Cycle bit of produced event must match consumer cycle state */
    if ((trb->control & TRB_C) != (uint32_t)r->cycle) return false;
    *out = *trb;
    r->dequeue++;
    if (r->dequeue >= r->size) {
        r->dequeue = 0;
        r->cycle  ^= 1;   /* toggle cycle bit when wrapping */
    }
    return true;
}

/* ─── PCI MSI configuration ──────────────────────────────────────────────── */
/* Enables MSI on the xHCI PCI device, routing to XHCI_IRQ_VECTOR on CPU 0 */
static void pci_enable_msi(pci_device_t *dev, uint8_t vector) {
    uint8_t cap = pci_find_cap(dev, 0x05);  /* MSI cap ID = 0x05 */
    if (!cap) {
        KLOG_WARN("xhci: no MSI capability, falling back to legacy INTx\n");
        return;
    }

    uint16_t ctrl = pci_read16(dev->bus, dev->dev, dev->fn, cap + 2);

    /* Message address: FSB delivery to LAPIC 0, edge, fixed */
    uint32_t msg_addr = 0xFEE00000u | ((uint32_t)cpu_lapic_id() << 12);
    uint16_t msg_data = (uint16_t)vector;  /* edge-triggered, fixed delivery */

    pci_write32(dev->bus, dev->dev, dev->fn, cap + 4, msg_addr);

    bool is64 = (ctrl >> 7) & 1;
    if (is64) {
        pci_write32(dev->bus, dev->dev, dev->fn, cap + 8, 0);  /* addr hi */
        pci_write16(dev->bus, dev->dev, dev->fn, cap + 12, msg_data);
    } else {
        pci_write16(dev->bus, dev->dev, dev->fn, cap + 8, msg_data);
    }

    /* Enable MSI, 1 message */
    ctrl &= ~(0x7 << 4);   /* requested vectors = 1 (bits 6:4 = 0b000) */
    ctrl |=  (1 << 0);     /* MSI enable bit */
    pci_write16(dev->bus, dev->dev, dev->fn, cap + 2, ctrl);

    KLOG_INFO("xhci: MSI configured → vector 0x%x\n", vector);
}

/* ─── IRQ handler (interrupt context — must be minimal) ─────────────────── */
/* Atomic "event pending" counter used to avoid a lost-wakeup race:
 * If the IRQ fires while usb_event_task is still TASK_RUNNING (not yet
 * TASK_BLOCKED), sched_unblock is a no-op.  The task reads and clears
 * this counter before deciding to call sched_block().  If non-zero, it
 * loops instead of blocking, picks up any events in the ring, then retries. */
volatile int g_xhci_ev_pending = 0;

static void xhci_irq(cpu_regs_t *regs) {
    (void)regs;

    /* Acknowledge interrupt pending in interrupter 0 IMAN */
    uint32_t iman = rt_ir_read32(IR_IMAN);
    rt_ir_write32(IR_IMAN, iman | IMAN_IP);

    /* Clear EINT in USBSTS */
    op_write32(XHCI_OP_USBSTS, USBSTS_EINT);

    /* Signal the event task.  Increment before sched_unblock so that if
     * the task is still RUNNING (unblock is a no-op), it will see the
     * non-zero counter and skip the sched_block() call.                   */
    __atomic_fetch_add(&g_xhci_ev_pending, 1, __ATOMIC_RELEASE);
    if (g_usb_event_task)
        sched_unblock(g_usb_event_task);

    apic_send_eoi();
}

/* ─── Controller reset + start ───────────────────────────────────────────── */
static bool hc_reset(void) {
    /* Stop the controller first */
    op_write32(XHCI_OP_USBCMD, op_read32(XHCI_OP_USBCMD) & ~USBCMD_RUN);

    /* Wait for HCH (halted) */
    for (int i = 0; i < 1000; i++) {
        if (op_read32(XHCI_OP_USBSTS) & USBSTS_HCH) goto halted;
        for (volatile int d = 0; d < 10000; d++) cpu_pause();
    }
    KLOG_ERR("xhci: timeout waiting for HC to halt\n");
    return false;
halted:
    /* Issue reset */
    op_write32(XHCI_OP_USBCMD, op_read32(XHCI_OP_USBCMD) | USBCMD_HCRST);

    /* Wait for reset to complete (HCRST clears itself, CNR clears too) */
    for (int i = 0; i < 2000; i++) {
        uint32_t cmd = op_read32(XHCI_OP_USBCMD);
        uint32_t sts = op_read32(XHCI_OP_USBSTS);
        if (!(cmd & USBCMD_HCRST) && !(sts & USBSTS_CNR)) return true;
        for (volatile int d = 0; d < 50000; d++) cpu_pause();
    }
    KLOG_ERR("xhci: timeout waiting for HC reset\n");
    return false;
}

/* ─── xhci_init ──────────────────────────────────────────────────────────── */
bool xhci_init(void) {
    /* Find xHCI controller from the already-scanned PCI device table */
    pci_device_t *devs;
    int n = pci_get_devices(&devs);
    pci_device_t *pdev = NULL;
    for (int i = 0; i < n; i++) {
        if (devs[i].class    == PCI_CLASS_SERIAL_BUS &&
            devs[i].subclass == PCI_SUBCLASS_USB     &&
            devs[i].prog_if  == PCI_PROGIF_XHCI) {
            pdev = &devs[i];
            break;
        }
    }
    if (!pdev) {
        KLOG_WARN("xhci: no xHCI controller found\n");
        return false;
    }
    KLOG_INFO("xhci: found controller %04x:%04x at %02x:%02x.%x\n",
              pdev->vendor_id, pdev->device_id,
              pdev->bus, pdev->dev, pdev->fn);

    /* Enable bus mastering + MMIO decode */
    pci_enable_device(pdev);

    /* Map MMIO BAR 0 */
    if (pdev->bars[0].type != PCI_BAR_MMIO || !pdev->bars[0].base) {
        KLOG_ERR("xhci: BAR0 not MMIO\n");
        return false;
    }
    size_t mmio_size = pdev->bars[0].size;
    if (mmio_size < 0x10000) mmio_size = 0x10000;  /* at least 64 KiB */
    KLOG_INFO("xhci: BAR0 phys=%p size=%lu\n",
              (void *)pdev->bars[0].base, (unsigned long)mmio_size);
    uintptr_t mmio_virt = vmm_mmio_map(pdev->bars[0].base, mmio_size);
    g_xhci.cap_base = (volatile uint8_t *)mmio_virt;

    /* Decode capability registers */
    uint8_t  caplength  = *(volatile uint8_t *)(mmio_virt + XHCI_CAP_CAPLENGTH);
    uint16_t version    = *(volatile uint16_t*)(mmio_virt + XHCI_CAP_HCIVERSION);
    uint32_t hcsparams1 = cap_read32(XHCI_CAP_HCSPARAMS1);
    uint32_t hccparams1 = cap_read32(XHCI_CAP_HCCPARAMS1);
    uint32_t dboff      = cap_read32(XHCI_CAP_DBOFF);
    uint32_t rtsoff     = cap_read32(XHCI_CAP_RTSOFF);

    g_xhci.op_base  = (volatile uint8_t *)(mmio_virt + caplength);
    g_xhci.rt_base  = (volatile uint8_t *)(mmio_virt + rtsoff);
    g_xhci.db_base  = (volatile uint32_t *)(mmio_virt + dboff);

    g_xhci.max_slots = (uint8_t)((hcsparams1 >> 0)  & 0xFF);
    g_xhci.max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    g_xhci.ctx_size  = (hccparams1 & HCCPARAMS1_CSZ) ? 64 : 32;

    KLOG_INFO("xhci: version=%04x slots=%u ports=%u ctx_size=%u\n",
              version, g_xhci.max_slots, g_xhci.max_ports, g_xhci.ctx_size);

    /* Reset host controller */
    if (!hc_reset()) return false;

    /* Clamp max_slots to our capability */
    if (g_xhci.max_slots > XHCI_MAX_SLOTS)
        g_xhci.max_slots = XHCI_MAX_SLOTS;
    op_write32(XHCI_OP_CONFIG, g_xhci.max_slots);

    /* ── DCBAA ──────────────────────────────────────────────────────────── */
    /* Needs (max_slots+1) * 8 bytes, 64-byte aligned (1 page is fine)     */
    g_xhci.dcbaa = alloc_dma(1, &g_xhci.dcbaa_phys);
    if (!g_xhci.dcbaa) return false;
    op_write64(XHCI_OP_DCBAAP_LO, g_xhci.dcbaa_phys);

    /* ── Command ring ────────────────────────────────────────────────────── */
    if (!ring_alloc(&g_xhci.cmd_ring, XHCI_RING_SIZE)) return false;
    uint64_t crcr = g_xhci.cmd_ring.phys | CRCR_RCS;
    op_write64(XHCI_OP_CRCR_LO, crcr);

    /* ── Event ring (interrupter 0) ──────────────────────────────────────── */
    if (!ring_alloc(&g_xhci.evt_ring, XHCI_EVT_RING_SIZE)) return false;

    /* Event Ring Segment Table — 1 entry */
    g_xhci.erst = alloc_dma(1, &g_xhci.erst_phys);
    if (!g_xhci.erst) return false;
    g_xhci.erst[0].base_addr = g_xhci.evt_ring.phys;
    g_xhci.erst[0].size      = XHCI_EVT_RING_SIZE;
    cpu_mfence();

    /* Set ERSTSZ = 1, ERSTBA, ERDP */
    rt_ir_write32(IR_ERSTSZ, 1);
    rt_ir_write64(IR_ERSTBA_LO, g_xhci.erst_phys);
    rt_ir_write64(IR_ERDP_LO,   g_xhci.evt_ring.phys);

    /* Enable interrupter 0 */
    rt_ir_write32(IR_IMOD, 0);            /* no interrupt moderation         */
    rt_ir_write32(IR_IMAN, IMAN_IE | IMAN_IP);

    /* ── Install IRQ handler + enable MSI ───────────────────────────────── */
    idt_register_handler(XHCI_IRQ_VECTOR, xhci_irq);
    pci_enable_msi(pdev, XHCI_IRQ_VECTOR);

    /* ── Start the controller ────────────────────────────────────────────── */
    uint32_t cmd = op_read32(XHCI_OP_USBCMD);
    cmd |= USBCMD_RUN | USBCMD_INTE | USBCMD_HSEE;
    op_write32(XHCI_OP_USBCMD, cmd);

    /* Wait until no longer halted */
    for (int i = 0; i < 1000; i++) {
        if (!(op_read32(XHCI_OP_USBSTS) & USBSTS_HCH)) break;
        for (volatile int d = 0; d < 10000; d++) cpu_pause();
    }
    if (op_read32(XHCI_OP_USBSTS) & USBSTS_HCH) {
        KLOG_ERR("xhci: controller failed to start\n");
        return false;
    }

    /* Give the HC a moment to settle, then scan ports for pre-connected
     * devices.  QEMU's nec-usb-xhci does NOT post PORT_STATUS_CHANGE events
     * for devices that were already attached when USBCMD_RUN is first set
     * (unlike real hardware).  We queue found ports to pending_ports[] so
     * usb_event_task picks them up on its first iteration.                */
    for (volatile int d = 0; d < 50000; d++) cpu_pause();
    for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
        uint32_t sc = portsc_read(p);
        if ((sc & PORTSC_CCS) && !g_xhci.port_active[p]) {
            uint8_t speed = (uint8_t)((sc & PORTSC_SPEED_MASK) >> 10);
            if (g_xhci.pending_count < XHCI_MAX_PENDING) {
                g_xhci.pending_ports [g_xhci.pending_count] = p;
                g_xhci.pending_speeds[g_xhci.pending_count] = speed;
                g_xhci.pending_count++;
                KLOG_INFO("xhci: port %u pre-connected (speed=%u)\n", p, speed);
            }
        }
    }
    KLOG_INFO("xhci: controller running\n");
    return true;
}

/* ─── Port reset ─────────────────────────────────────────────────────────── */
bool xhci_port_reset(uint8_t port) {
    uint32_t sc = portsc_read(port);

    /* For USB3 ports, use Warm Port Reset; for USB2 use PR bit */
    bool usb3 = ((sc & PORTSC_SPEED_MASK) >> 10) >= 4;
    uint32_t rst_bit = usb3 ? PORTSC_WPR : PORTSC_PR;

    /* Clear W1C bits while asserting reset */
    uint32_t wr = (sc & ~PORTSC_W1C_BITS) | rst_bit;
    portsc_write(port, wr);

    /* Wait for PRC (port reset change) */
    for (int i = 0; i < 5000; i++) {
        sc = portsc_read(port);
        if (sc & PORTSC_PRC) break;
        for (volatile int d = 0; d < 10000; d++) cpu_pause();
    }
    if (!(portsc_read(port) & PORTSC_PRC)) {
        KLOG_ERR("xhci: port %u reset timeout\n", port);
        return false;
    }
    /* Clear PRC */
    portsc_write(port, (portsc_read(port) & ~PORTSC_W1C_BITS) | PORTSC_PRC);

    /* Small additional settle delay */
    for (volatile int d = 0; d < 500000; d++) cpu_pause();

    sc = portsc_read(port);
    KLOG_INFO("xhci: port %u reset done, PED=%d CCS=%d\n",
              port, (int)!!(sc & PORTSC_PED), (int)!!(sc & PORTSC_CCS));
    return true;
}

/* ─── Doorbell ───────────────────────────────────────────────────────────── */
void xhci_ring_cmd_doorbell(void) {
    cpu_mfence();
    g_xhci.db_base[0] = 0;   /* doorbell slot 0, target 0 = host controller */
    cpu_mfence();
}

void xhci_ring_ep_doorbell(uint8_t slot, uint8_t ep_id) {
    cpu_mfence();
    g_xhci.db_base[slot] = ep_id;
    cpu_mfence();
}

/* ─── Command submission (task context) ──────────────────────────────────── */
uint8_t xhci_submit_cmd(xhci_trb_t *trb) {
    g_xhci.cmd_pending  = true;
    g_xhci.cmd_cc       = CC_INVALID;
    uintptr_t trb_phys  = ring_enqueue(&g_xhci.cmd_ring, trb);
    g_xhci.cmd_trb_phys = trb_phys;
    xhci_ring_cmd_doorbell();

    /* Spin-poll the event ring directly so this works regardless of whether
     * the usb-evt task exists or is the caller.  Port-status events that
     * arrive during this loop are queued and handled later by the caller.  */
    for (int i = 0; i < 500000; i++) {
        if (!g_xhci.cmd_pending) break;
        xhci_process_events();
        for (volatile int d = 0; d < 200; d++) cpu_pause();
    }
    if (g_xhci.cmd_pending) {
        KLOG_ERR("xhci: command timeout\n");
        g_xhci.cmd_pending = false;
        return CC_INVALID;
    }
    return g_xhci.cmd_cc;
}

/* Allocate and initialise per-slot device contexts                         */
static bool slot_alloc_contexts(uint8_t slot_id, uint8_t port, uint8_t speed) {
    xhci_slot_t *s = &g_xhci.slots[slot_id];
    s->valid    = true;
    s->slot_id  = slot_id;
    s->port     = port;
    s->speed    = speed;
    s->address  = 0;

    size_t ctx_pages = 1;   /* one page covers 32 endpoint contexts each ≤64B */

    s->input_ctx_virt  = alloc_dma(ctx_pages, &s->input_ctx_phys);
    s->output_ctx_virt = alloc_dma(ctx_pages, &s->output_ctx_phys);
    if (!s->input_ctx_virt || !s->output_ctx_virt) return false;

    /* Allocate EP0 transfer ring */
    if (!ring_alloc(&s->xfer_rings[1], XHCI_RING_SIZE)) return false;

    /* Register output context in DCBAA */
    g_xhci.dcbaa[slot_id] = s->output_ctx_phys;
    cpu_mfence();
    return true;
}

/* Fill input context for ADDRESS DEVICE command (EP0 only) */
static void slot_prepare_address_ctx(uint8_t slot_id, uint16_t mps) {
    xhci_slot_t *s    = &g_xhci.slots[slot_id];
    size_t        csz = g_xhci.ctx_size;
    uint8_t      *ib  = (uint8_t *)s->input_ctx_virt;

    /* Input Control Context */
    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)ib;
    icc->drop_flags = 0;
    icc->add_flags  = (1u << 0) | (1u << 1);   /* add slot + EP0 */

    /* Slot Context at offset csz (after ICC) */
    xhci_slot_ctx_t *sc = (xhci_slot_ctx_t *)(ib + csz);
    sc->dw0 = SLOT_CTX_ENTRIES(1) | SLOT_CTX_SPEED(s->speed);
    /* Route string = 0, root hub port number in DW1 bits [23:16] */
    sc->dw1 = (uint32_t)(s->port) << 16;

    /* EP0 Context at offset csz*2 */
    xhci_ep_ctx_t *ep0 = (xhci_ep_ctx_t *)(ib + csz * 2);
    ep0->dw0         = 0;   /* error count = 3 (bits 2:1) set below */
    ep0->dw1         = ((uint32_t)3 << 1)          /* error count = 3      */
                     | ((uint32_t)EP_TYPE_CONTROL << 3)
                     | ((uint32_t)mps << 16);       /* max packet size      */
    ep0->dequeue_ptr = s->xfer_rings[1].phys | 1;  /* DCS = 1 (cycle=1)   */
    ep0->dw4         = mps;                         /* average TRB length   */
    cpu_mfence();
}

/* ─── Enable Slot command ────────────────────────────────────────────────── */
/* Returns slot_id (1-based) or 0 on failure                                 */
uint8_t xhci_enable_slot(void) {
    xhci_trb_t trb = {0};
    trb.control = TRB_TYPE(CTRB_ENABLE_SLOT);
    uint8_t cc  = xhci_submit_cmd(&trb);
    if (cc != CC_SUCCESS) {
        KLOG_ERR("xhci: Enable Slot failed cc=%u\n", cc);
        return 0;
    }
    return g_xhci.cmd_slot;
}

/* ─── Address Device command ─────────────────────────────────────────────── */
/* BSR=0 → actually assign address (USBAddress); BSR=1 → block Set Address  */
uint8_t xhci_address_device(uint8_t slot_id, bool bsr) {
    xhci_slot_t *s = &g_xhci.slots[slot_id];

    /* Default max packet size for EP0 by speed:
     * LowSpeed=8, FullSpeed=8 (8..64 via GET_DESCRIPTOR), High=64, Super=512 */
    uint16_t mps = 64;
    if      (s->speed <= 2) mps = 8;
    else if (s->speed == 4) mps = 512;

    slot_prepare_address_ctx(slot_id, mps);

    xhci_trb_t trb = {0};
    trb.parameter = s->input_ctx_phys;
    trb.control   = TRB_TYPE(CTRB_ADDRESS_DEVICE)
                  | ((uint32_t)slot_id << 24)
                  | (bsr ? (1u << 9) : 0u);
    uint8_t cc = xhci_submit_cmd(&trb);
    if (cc != CC_SUCCESS) {
        KLOG_ERR("xhci: Address Device slot=%u failed cc=%u\n", slot_id, cc);
        return 0;
    }

    /* Read assigned address from output slot context */
    uint8_t *ob  = (uint8_t *)s->output_ctx_virt;
    xhci_slot_ctx_t *osc = (xhci_slot_ctx_t *)(ob + g_xhci.ctx_size);
    s->address = (uint8_t)(osc->dw3 & 0xFF);
    KLOG_INFO("xhci: slot %u assigned USB address %u\n", slot_id, s->address);
    return s->address;
}

/* ─── Configure Endpoint command ─────────────────────────────────────────── */
/* Adds a new endpoint (e.g., HID interrupt IN) to an already-addressed slot */
bool xhci_configure_endpoint(uint8_t slot_id,
                              uint8_t ep_id,     /* 1-31, xHCI dword index */
                              uint8_t ep_type,   /* EP_TYPE_INT_IN etc.    */
                              uint16_t mps,
                              uint8_t interval) {
    xhci_slot_t *s   = &g_xhci.slots[slot_id];
    size_t        csz = g_xhci.ctx_size;
    uint8_t      *ib  = (uint8_t *)s->input_ctx_virt;

    /* Allocate transfer ring for this endpoint if not already done */
    if (!s->xfer_rings[ep_id].trbs) {
        if (!ring_alloc(&s->xfer_rings[ep_id], XHCI_RING_SIZE))
            return false;
    }

    /* Input Control Context: drop nothing, add Slot + this EP */
    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)ib;
    icc->drop_flags = 0;
    icc->add_flags  = (1u << 0) | (1u << ep_id);

    /* Update Slot Context: bump context entries */
    xhci_slot_ctx_t *sc = (xhci_slot_ctx_t *)(ib + csz);
    uint32_t cur_entries = (sc->dw0 >> 27) & 0x1F;
    if (ep_id > cur_entries) {
        sc->dw0 = (sc->dw0 & ~(0x1Fu << 27)) | SLOT_CTX_ENTRIES(ep_id);
    }

    /* EP Context at offset csz * (1 + ep_id) past ICC */
    xhci_ep_ctx_t *epc = (xhci_ep_ctx_t *)(ib + csz * (1u + ep_id));
    memset(epc, 0, sizeof(*epc));
    epc->dw0 = (uint32_t)interval << 16;  /* interval in DW0[23:16] */
    epc->dw1 = ((uint32_t)3 << 1)         /* error count            */
             | ((uint32_t)ep_type << 3)
             | ((uint32_t)mps << 16);
    epc->dequeue_ptr = s->xfer_rings[ep_id].phys | 1;
    epc->dw4 = mps;
    cpu_mfence();

    xhci_trb_t trb = {0};
    trb.parameter = s->input_ctx_phys;
    trb.control   = TRB_TYPE(CTRB_CONFIG_ENDPOINT) | ((uint32_t)slot_id << 24);
    uint8_t cc = xhci_submit_cmd(&trb);
    if (cc != CC_SUCCESS) {
        KLOG_ERR("xhci: Configure Endpoint slot=%u ep=%u failed cc=%u\n",
                 slot_id, ep_id, cc);
        return false;
    }
    KLOG_INFO("xhci: slot %u ep %u configured (type=%u mps=%u interval=%u)\n",
              slot_id, ep_id, ep_type, mps, interval);
    return true;
}

/* ─── Queue a Normal TRB onto an endpoint transfer ring ─────────────────── */
void xhci_queue_transfer(uint8_t slot_id, uint8_t ep_id,
                          uintptr_t phys, uint32_t length, bool ioc) {
    xhci_slot_t *s = &g_xhci.slots[slot_id];
    xhci_trb_t trb = {0};
    trb.parameter = phys;
    trb.status    = length;
    trb.control   = TRB_TYPE(TTRB_NORMAL) | TRB_ISP | (ioc ? TRB_IOC : 0u);
    ring_enqueue(&s->xfer_rings[ep_id], &trb);
    xhci_ring_ep_doorbell(slot_id, ep_id);
}

/* ─── Control transfer helpers (Setup + Data + Status) ──────────────────── */
/* Submits a full control transfer synchronously; `data_phys` may be 0.     */
/* Returns bytes transferred (or 0 on error).                               */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_pkt_t;

static volatile uint8_t ctrl_cc;   /* completion code from last control transfer     */

void xhci_control_transfer(uint8_t   slot_id,
                            uint8_t   bmRequestType,
                            uint8_t   bRequest,
                            uint16_t  wValue,
                            uint16_t  wIndex,
                            uint16_t  wLength,
                            uintptr_t data_phys) {
    xhci_slot_t *s   = &g_xhci.slots[slot_id];
    xhci_ring_t *r   = &s->xfer_rings[1];  /* EP0 */

    /* Pack setup packet into TRB parameter field (8 bytes, immediate) */
    usb_setup_pkt_t setup = {
        .bmRequestType = bmRequestType,
        .bRequest      = bRequest,
        .wValue        = wValue,
        .wIndex        = wIndex,
        .wLength       = wLength,
    };
    uint64_t setup_param;
    memcpy(&setup_param, &setup, 8);

    /* Determine transfer direction for Status stage */
    /* Direction bit in bmRequestType: 1 = device-to-host (IN)              */
    bool dir_in = (bmRequestType >> 7) & 1;
    /* TRT field in Setup Stage TRB control[4:3]: 2=OUT data, 3=IN data     */
    uint32_t trt = wLength ? (dir_in ? (3u << 3) : (2u << 3)) : 0u;

    /* Setup Stage TRB */
    {
        xhci_trb_t trb = {0};
        trb.parameter  = setup_param;
        trb.status     = 8;  /* setup packet always 8 bytes */
        trb.control    = TRB_TYPE(TTRB_SETUP_STAGE) | TRB_IDT | trt;
        ring_enqueue(r, &trb);
    }

    /* Data Stage TRB (if wLength > 0) */
    if (wLength && data_phys) {
        xhci_trb_t trb = {0};
        trb.parameter  = data_phys;
        trb.status     = wLength;
        trb.control    = TRB_TYPE(TTRB_DATA_STAGE) | TRB_ISP
                       | (dir_in ? (1u << 16) : 0u);  /* DIR bit */
        ring_enqueue(r, &trb);
    }

    /* Status Stage TRB (direction opposite to Data, or IN if no data)      */
    {
        xhci_trb_t trb = {0};
        trb.status     = 0;
        /* DIR in status stage: opposite of data (for data transfer)        */
        uint32_t status_dir = (wLength && dir_in) ? 0u : (1u << 16);
        trb.control    = TRB_TYPE(TTRB_STATUS_STAGE) | TRB_IOC | status_dir;
        ring_enqueue(r, &trb);
    }

    cpu_mfence();
    xhci_ring_ep_doorbell(slot_id, 1);

    /* Spin-poll the event ring directly (same rationale as xhci_submit_cmd) */
    ctrl_cc = CC_INVALID;
    for (int i = 0; i < 500000; i++) {
        if (ctrl_cc != CC_INVALID) break;
        xhci_process_events();
        for (volatile int d = 0; d < 200; d++) cpu_pause();
    }
    if (ctrl_cc == CC_INVALID)
        KLOG_ERR("xhci: control transfer timeout slot=%u\n", slot_id);
}

/* ─── Event ring processing ──────────────────────────────────────────────── */
int xhci_process_events(void) {
    int count = 0;
    xhci_trb_t ev;

    while (evt_dequeue(&g_xhci.evt_ring, &ev)) {
        uint32_t type    = TRB_GET_TYPE(ev.control);
        uint8_t  cc      = TRB_CC(ev.status);
        uint8_t  slot_id = TRB_SLOT(ev.control);

        switch (type) {
        case ETRB_CMD_COMPLETION: {
            uintptr_t cmd_phys = (uintptr_t)(ev.parameter & ~0xFull);
            if (g_xhci.cmd_pending && cmd_phys == g_xhci.cmd_trb_phys) {
                g_xhci.cmd_cc      = cc;
                g_xhci.cmd_slot    = slot_id;
                g_xhci.cmd_pending = false;
            }
            if (cc != CC_SUCCESS)
                KLOG_WARN("xhci: cmd completion cc=%u slot=%u\n", cc, slot_id);
            break;
        }
        case ETRB_TRANSFER: {
            uint8_t  ep_id   = TRB_EP_ID(ev.control);
            uint32_t residual= ev.status & 0x00FFFFFFu;

            /* Control transfer on EP0: drive ctrl_cc */
            if (ep_id == 1) {
                ctrl_cc = cc;
            } else {
                /* HID interrupt transfer or other */
                usb_on_transfer_complete(slot_id, ep_id, cc, residual);
            }
            break;
        }
        case ETRB_PORT_STATUS: {
            uint8_t port = (uint8_t)((ev.parameter >> 24) & 0xFF);
            uint32_t sc  = portsc_read(port);
            KLOG_INFO("xhci: port %u status change (PORTSC=%08x)\n", port, sc);

            /* Acknowledge all W1C change bits */
            portsc_write(port, (sc & ~PORTSC_W1C_BITS) | PORTSC_W1C_BITS);

            if ((sc & PORTSC_CSC) && (sc & PORTSC_CCS) &&
                !g_xhci.port_active[port]) {
                /* Genuine connect event on idle port — queue for deferred enum */
                uint8_t speed = (uint8_t)((sc & PORTSC_SPEED_MASK) >> 10);
                if (g_xhci.pending_count < XHCI_MAX_PENDING) {
                    g_xhci.pending_ports [g_xhci.pending_count] = port;
                    g_xhci.pending_speeds[g_xhci.pending_count] = speed;
                    g_xhci.pending_count++;
                }
            } else if (!(sc & PORTSC_CCS) && g_xhci.port_active[port]) {
                /* Disconnect */
                g_xhci.port_active[port] = false;
                KLOG_INFO("xhci: port %u disconnected\n", port);
            }
            break;
        }
        default:
            break;
        }

        /* Advance ERDP after consuming each event */
        uintptr_t erdp = g_xhci.evt_ring.phys
                       + g_xhci.evt_ring.dequeue * sizeof(xhci_trb_t);
        rt_ir_write64(IR_ERDP_LO, erdp | ERDP_EHB);
        count++;
    }
    return count;
}

/* ─── Deferred port enumeration (called by usb_event_task, outside evt loop) */
void xhci_handle_pending_connects(void) {
    while (g_xhci.pending_count > 0) {
        uint8_t port  = g_xhci.pending_ports[0];
        uint8_t speed = g_xhci.pending_speeds[0];

        /* Shift queue */
        for (uint8_t i = 0; i < g_xhci.pending_count - 1u; i++) {
            g_xhci.pending_ports [i] = g_xhci.pending_ports [i + 1];
            g_xhci.pending_speeds[i] = g_xhci.pending_speeds[i + 1];
        }
        g_xhci.pending_count--;

        /* Mark active BEFORE port reset so that CSC events generated by
         * the reset itself are ignored in xhci_process_events.             */
        g_xhci.port_active[port] = true;

        if (xhci_port_reset(port)) {
            usb_on_port_connected(port, speed);
        } else {
            /* Reset failed — release port for a future connect event       */
            g_xhci.port_active[port] = false;
        }
    }
}

/* ─── Slot context accessors (used by usb_core) ──────────────────────────── */
xhci_slot_t *xhci_get_slot(uint8_t slot_id) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) return NULL;
    return &g_xhci.slots[slot_id];
}
/* Public wrapper so usb_core.c can allocate slot contexts without needing   */
/* access to the static ring_alloc / alloc_dma functions inside this file.   */
bool xhci_slot_alloc_ctx(uint8_t slot_id, uint8_t port, uint8_t speed) {
    return slot_alloc_contexts(slot_id, port, speed);
}
uint8_t xhci_do_enable_slot(void) { return xhci_enable_slot(); }
uint8_t xhci_do_address_device(uint8_t slot, bool bsr) {
    return xhci_address_device(slot, bsr);
}
