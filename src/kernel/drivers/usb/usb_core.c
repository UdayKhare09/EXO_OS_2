/* drivers/usb/usb_core.c — USB protocol / enumeration layer */
#include "usb_core.h"
#include "xhci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "arch/x86_64/cpu.h"

/* ─── Device table ───────────────────────────────────────────────────────── */
#define USB_MAX_DEVICES   16

static usb_device_t g_devices[USB_MAX_DEVICES];
static int          g_dev_count = 0;

/* ─── Class driver registry ──────────────────────────────────────────────── */
#define USB_MAX_CLASS_DRIVERS  8

static const usb_class_driver_t *g_class_drivers[USB_MAX_CLASS_DRIVERS];
static int                       g_class_driver_count = 0;

void usb_register_class_driver(const usb_class_driver_t *drv) {
    if (g_class_driver_count >= USB_MAX_CLASS_DRIVERS) return;
    g_class_drivers[g_class_driver_count++] = drv;
    KLOG_INFO("usb: registered class driver \"%s\"\n", drv->name);
}

/* Find a device by slot_id */
static usb_device_t *dev_by_slot(uint8_t slot_id) {
    for (int i = 0; i < g_dev_count; i++)
        if (g_devices[i].slot_id == slot_id) return &g_devices[i];
    return NULL;
}

/* Find the class driver for a device */
static const usb_class_driver_t *find_driver(usb_device_t *dev) {
    for (int i = 0; i < g_class_driver_count; i++) {
        const usb_class_driver_t *d = g_class_drivers[i];
        if (d->probe(dev->iface_class, dev->iface_subclass, dev->iface_protocol))
            return d;
    }
    return NULL;
}

/* ─── DMA buffer helpers ─────────────────────────────────────────────────── */
/* Allocate one page for a descriptor buffer; returns virt+phys.            */
static void *desc_buf_alloc(uintptr_t *phys) {
    uintptr_t p = pmm_alloc_pages(1);
    if (!p) return NULL;
    uintptr_t v = vmm_phys_to_virt(p);
    memset((void *)v, 0, PAGE_SIZE);
    *phys = p;
    return (void *)v;
}

/* ─── Control transfer wrappers ──────────────────────────────────────────── */
bool usb_get_descriptor(usb_device_t *dev, uint8_t dt, uint8_t idx,
                         uint16_t lang, void *buf, uint16_t len) {
    /* We need a physically contiguous DMA buffer */
    uintptr_t phys;
    void *virt = desc_buf_alloc(&phys);
    if (!virt) return false;

    xhci_control_transfer(dev->slot_id,
        USB_DIR_DEV_TO_HOST | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR,
        (uint16_t)((dt << 8) | idx),
        lang,
        len,
        phys);

    /* Copy from DMA buffer to caller's buffer */
    memcpy(buf, virt, len);
    pmm_free_pages(phys, 1);
    return true;   /* caller checks descriptor bLength to detect errors */
}

bool usb_set_configuration(usb_device_t *dev, uint8_t value) {
    xhci_control_transfer(dev->slot_id,
        USB_DIR_HOST_TO_DEV | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION,
        value,
        0, 0, 0);
    return true;
}

/* GET_DESCRIPTOR for class-specific descriptors (e.g., HID report)         */
bool usb_get_class_descriptor(usb_device_t *dev, uint8_t iface,
                               uint8_t dt, uint8_t idx,
                               void *buf, uint16_t len) {
    uintptr_t phys;
    void *virt = desc_buf_alloc(&phys);
    if (!virt) return false;

    xhci_control_transfer(dev->slot_id,
        USB_DIR_DEV_TO_HOST | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
        USB_REQ_GET_DESCRIPTOR,
        (uint16_t)((dt << 8) | idx),
        iface,
        len,
        phys);

    memcpy(buf, virt, len);
    pmm_free_pages(phys, 1);
    return true;
}

/* ─── Descriptor parser ──────────────────────────────────────────────────── */
/* Parse a raw configuration descriptor blob, filling dev->eps[].           */
static void parse_config(usb_device_t *dev, uint8_t *blob, uint16_t total) {
    dev->ep_count       = 0;
    dev->iface_class    = 0;
    dev->iface_subclass = 0;
    dev->iface_protocol = 0;

    bool first_iface = true;
    uint16_t pos = 0;
    while (pos + 2 <= total) {
        uint8_t bLen  = blob[pos];
        uint8_t bType = blob[pos + 1];
        if (!bLen) break;

        if (bType == USB_DT_INTERFACE && first_iface) {
            usb_iface_desc_t *id = (usb_iface_desc_t *)(blob + pos);
            dev->iface_num      = id->bInterfaceNumber;
            dev->iface_class    = id->bInterfaceClass;
            dev->iface_subclass = id->bInterfaceSubClass;
            dev->iface_protocol = id->bInterfaceProtocol;
            first_iface = false;
            KLOG_INFO("usb: interface class=%02x sub=%02x proto=%02x\n",
                      dev->iface_class, dev->iface_subclass, dev->iface_protocol);
        } else if (bType == USB_DT_ENDPOINT && dev->ep_count < USB_MAX_ENDPOINTS) {
            usb_ep_desc_t *ed = (usb_ep_desc_t *)(blob + pos);
            dev->eps[dev->ep_count++] = *ed;
            KLOG_INFO("usb: endpoint addr=%02x attr=%02x mps=%u interval=%u\n",
                      ed->bEndpointAddress, ed->bmAttributes,
                      ed->wMaxPacketSize, ed->bInterval);
        }
        pos += bLen;
    }
}

/* ─── Convert USB EP address to xHCI ep_id ──────────────────────────────── */
/* xHCI ep_id = 2 * EP_number + (direction: 1=IN, 0=OUT)                    */
/* For Ctrl EP0: ep_id = 1                                                   */
static uint8_t ep_addr_to_dci(uint8_t bEndpointAddress) {
    uint8_t num = bEndpointAddress & 0x0F;
    uint8_t dir = (bEndpointAddress >> 7) & 1;
    return (uint8_t)(num * 2 + dir);
}

/* ─── Enumerate a newly connected device ────────────────────────────────── */
static void enumerate_device(uint8_t port, uint8_t speed) {
    if (g_dev_count >= USB_MAX_DEVICES) {
        KLOG_WARN("usb: device table full\n");
        return;
    }
    usb_device_t *dev = &g_devices[g_dev_count];
    memset(dev, 0, sizeof(*dev));
    dev->port  = port;
    dev->speed = speed;

    /* ── 1. Enable slot ────────────────────────────────────────────────── */
    xhci_trb_t trb = {0};
    trb.control = TRB_TYPE(CTRB_ENABLE_SLOT);
    uint8_t cc  = xhci_submit_cmd(&trb);
    if (cc != CC_SUCCESS) {
        KLOG_ERR("usb: Enable Slot failed (port %u)\n", port);
        return;
    }
    uint8_t slot_id = g_xhci.cmd_slot;
    KLOG_INFO("usb: port %u → slot %u\n", port, slot_id);

    /* ── 2. Allocate device contexts + EP0 ring ─────────────────────── */
    /* Declared in xhci.h; defined in xhci.c (needs static ring_alloc)  */
    if (!xhci_slot_alloc_ctx(slot_id, port, speed)) {
        KLOG_ERR("usb: slot %u: context allocation failed\n", slot_id);
        return;
    }
    dev->slot_id = slot_id;

    /* ── 3. Address Device (BSR=1: no SET_ADDRESS, just update context) */
    uint8_t addr = xhci_address_device(slot_id, true);
    (void)addr;   /* with BSR=1 address stays 0 until we send SET_ADDRESS */
    dev->addr = 0;

    /* ── 4. GET_DESCRIPTOR(Device, 8 bytes) — get bMaxPacketSize0 ─── */
    uint8_t partial[8];
    usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, partial, 8);
    uint8_t mps0 = partial[7];
    if (!mps0) mps0 = 8;
    KLOG_INFO("usb: slot %u bMaxPacketSize0=%u\n", slot_id, mps0);

    /* Update EP0 context with correct MPS if needed (Evaluate Context) */
    /* For simplicity we re-address with BSR=0 which commits the slot   */
    addr = xhci_address_device(slot_id, false);
    dev->addr = addr;

    /* ── 5. GET_DESCRIPTOR(Device, full 18 bytes) ────────────────────── */
    usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->dev_desc, 18);
    KLOG_INFO("usb: slot %u VID=%04x PID=%04x class=%02x\n",
              slot_id, dev->dev_desc.idVendor, dev->dev_desc.idProduct,
              dev->dev_desc.bDeviceClass);

    /* ── 6. GET_DESCRIPTOR(Config) — two passes: total length first ──── */
    uint8_t cfg_hdr[9];
    usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, cfg_hdr, 9);
    uint16_t total_len = (uint16_t)(cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8));
    if (!total_len || total_len > 512) total_len = 256;

    uintptr_t cfg_phys;
    void *cfg_buf = desc_buf_alloc(&cfg_phys);
    if (!cfg_buf) return;
    usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, cfg_buf, total_len);
    parse_config(dev, cfg_buf, total_len);
    pmm_free_pages(cfg_phys, 1);

    /* ── 7. Set Configuration ─────────────────────────────────────────── */
    uint8_t cfg_val = cfg_hdr[5];   /* bConfigurationValue */
    usb_set_configuration(dev, cfg_val);
    KLOG_INFO("usb: slot %u SET_CONFIGURATION=%u\n", slot_id, cfg_val);

    /* ── 8. Find and bind class driver ───────────────────────────────── */
    const usb_class_driver_t *drv = find_driver(dev);
    if (!drv) {
        KLOG_WARN("usb: no driver for class=%02x sub=%02x proto=%02x\n",
                  dev->iface_class, dev->iface_subclass, dev->iface_protocol);
        return;
    }
    KLOG_INFO("usb: binding driver \"%s\"\n", drv->name);
    if (!drv->attach(dev)) {
        KLOG_ERR("usb: driver \"%s\" attach failed\n", drv->name);
        return;
    }

    g_dev_count++;
}

/* ─── xHCI callbacks ─────────────────────────────────────────────────────── */
void usb_on_port_connected(uint8_t port, uint8_t speed) {
    KLOG_INFO("usb: port %u connected (speed=%u)\n", port, speed);
    enumerate_device(port, speed);
}

void usb_on_transfer_complete(uint8_t slot_id, uint8_t ep_id,
                               uint8_t cc, uint32_t residual) {
    usb_device_t *dev = dev_by_slot(slot_id);
    if (!dev || !dev->class_data) return;

    const usb_class_driver_t *drv = find_driver(dev);
    if (!drv || !drv->transfer_done) return;

    /* Find the transfer buffer for this endpoint.
     * Class driver stores it in class_data; we pass NULL here and let
     * the class driver retrieve it from its own state.                     */
    drv->transfer_done(dev, ep_id, cc, residual, NULL, 0);
}

/* xhci_slot_alloc_ctx is defined in xhci.c (needs access to static ring_alloc) */

/* ─── usb_core_init ──────────────────────────────────────────────────────── */
void usb_core_init(void) {
    g_dev_count = 0;
    /* Do NOT reset g_class_driver_count — class drivers (e.g. hid_init)
     * register themselves before usb_core_init() is called.              */
    memset(g_devices, 0, sizeof(g_devices));
    KLOG_INFO("usb: core initialised\n");
}

/* ─── USB event task (kernel task, runs forever) ─────────────────────────── */
static void usb_event_task(void *arg) {
    (void)arg;
    /* g_usb_event_task is set by usb_init() before spawning,
     * so IRQ handlers can unblock us from the first interrupt.             */
    KLOG_INFO("usb: event task started\n");

    for (;;) {
        /* Atomically consume the IRQ-set wakeup token BEFORE processing.
         * This ordering prevents the lost-wakeup race:
         *   1. Clear token (any subsequent IRQ will set it again)
         *   2. Process event ring (drains all current events)
         *   3. If token is still 0 and nothing pending → safe to block;
         *      if an IRQ fired between steps 1 and 3 the token is now 1
         *      so we loop instead of blocking.                            */
        __atomic_store_n(&g_xhci_ev_pending, 0, __ATOMIC_RELAXED);

        int events = xhci_process_events();
        xhci_handle_pending_connects();

        /* Re-check token: non-zero means an IRQ arrived during processing */
        int tok = __atomic_load_n(&g_xhci_ev_pending, __ATOMIC_ACQUIRE);
        if (!events && !g_xhci.pending_count && !tok) {
            sched_block();
        }
    }
}

/* ─── Top-level init ─────────────────────────────────────────────────────── */
bool usb_init(void) {
    usb_core_init();

    /* Spawn the event task FIRST and record the task pointer so the
     * IRQ handler (xhci_irq) can unblock it even before the task body
     * runs.  This also means xhci_submit_cmd's sched_sleep path works
     * once interrupts are enabled.                                          */
    g_usb_event_task = sched_spawn("usb-evt", usb_event_task, NULL);

    if (!xhci_init()) {
        KLOG_WARN("usb: xHCI init failed — USB not available\n");
        return false;
    }

    KLOG_INFO("usb: stack ready\n");
    return true;
}

/* ─—— Public DMA helpers for class drivers ─————————————————————————─— */
void *alloc_dma_usb(size_t pages, uintptr_t *phys_out) {
    uintptr_t phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;
    uintptr_t virt = vmm_phys_to_virt(phys);
    memset((void *)virt, 0, pages * PAGE_SIZE);
    *phys_out = phys;
    return (void *)virt;
}

void *usb_dma_alloc(uintptr_t *phys_out) {
    return alloc_dma_usb(1, phys_out);
}

void usb_dma_free(uintptr_t phys) {
    pmm_free_pages(phys, 1);
}
