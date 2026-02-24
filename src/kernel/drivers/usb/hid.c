/* drivers/usb/hid.c — USB HID boot-protocol driver (keyboard + mouse)
 *
 * Boot-protocol keyboard report  (8 bytes):
 *   [0] modifier bitmask  (LCtrl=0 LShift=1 LAlt=2 LGUI=3 RCtrl=4 RShift=5 RAlt=6 RGUI=7)
 *   [1] reserved (0x00)
 *   [2..7] up to 6 simultaneous keycodes (0x00 = no key)
 *
 * Boot-protocol mouse report (4 bytes):
 *   [0] button bitmask (bit0=left bit1=right bit2=middle)
 *   [1] X delta (signed)
 *   [2] Y delta (signed)
 *   [3] scroll wheel delta (signed, optional — present if mps >= 4)
 *
 * Non-blocking flow:
 *   xhci_irq (interrupt) → sched_unblock(g_usb_event_task)
 *   usb_event_task       → xhci_process_events()
 *                        → usb_on_transfer_complete()
 *                        → hid_transfer_done()       ← parse + push to ring
 *                        → xhci_queue_transfer()     ← re-arm the IN pipe
 */
#include "hid.h"
#include "xhci.h"
#include "usb_core.h"
#include "drivers/input/input.h"
#include "mm/kmalloc.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "arch/x86_64/cpu.h"   /* cpu_mfence */

/* ── HID bmRequestType for class-to-interface requests ──────────────────── */
/* Host-to-Device | Class | Interface = 0x21                                */
#define HID_RT_HOST_TO_IF  (USB_DIR_HOST_TO_DEV | USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define HID_RT_DEV_TO_IF   (USB_DIR_DEV_TO_HOST | USB_TYPE_CLASS | USB_RECIP_INTERFACE)

/* ── Convert USB EP address to xHCI DCI ──────────────────────────────────── */
/* DCI = EP_number * 2 + direction_bit  (dir = (addr >> 7) & 1)             */
static inline uint8_t ep_to_dci(uint8_t bEndpointAddress) {
    uint8_t num = bEndpointAddress & 0x0Fu;
    uint8_t dir = (bEndpointAddress >> 7) & 1u;
    return (uint8_t)(num * 2u + dir);
}

/* ── probe ───────────────────────────────────────────────────────────────── */
static bool hid_probe(uint8_t class, uint8_t subclass, uint8_t protocol) {
    return (class    == USB_CLASS_HID      &&
            subclass == HID_SUBCLASS_BOOT  &&
            (protocol == HID_PROTOCOL_KEYBOARD ||
             protocol == HID_PROTOCOL_MOUSE));
}

/* ── attach ──────────────────────────────────────────────────────────────── */
static bool hid_attach(usb_device_t *dev) {
    /* 1. Find the interrupt IN endpoint */
    usb_ep_desc_t *ep_in = NULL;
    for (uint8_t i = 0; i < dev->ep_count; i++) {
        usb_ep_desc_t *ep = &dev->eps[i];
        bool is_in  = (ep->bEndpointAddress >> 7) & 1u;
        bool is_int = (ep->bmAttributes & 0x03u) == USB_EP_INTERRUPT;
        if (is_in && is_int) { ep_in = ep; break; }
    }
    if (!ep_in) {
        KLOG_ERR("hid: no interrupt IN endpoint found\n");
        return false;
    }

    /* 2. Allocate per-device state */
    hid_state_t *state = kzalloc(sizeof(hid_state_t));
    if (!state) return false;

    state->protocol = dev->iface_protocol;
    state->ep_id    = ep_to_dci(ep_in->bEndpointAddress);
    state->mps      = ep_in->wMaxPacketSize;
    state->interval  = ep_in->bInterval;

    /* Clamp MPS to something sane (boot protocol max is 8 bytes kbd, 4 mouse) */
    if (state->mps == 0 || state->mps > 64) state->mps = 8;

    KLOG_INFO("hid: slot=%u proto=%u ep_id=%u mps=%u interval=%u\n",
              dev->slot_id, state->protocol,
              state->ep_id, state->mps, state->interval);

    /* 3. SET_PROTOCOL → Boot Protocol (wValue = 0)                          */
    xhci_control_transfer(dev->slot_id,
        HID_RT_HOST_TO_IF,
        HID_REQ_SET_PROTOCOL,
        HID_BOOT_PROTOCOL,
        dev->iface_num,
        0, 0);

    /* 4. SET_IDLE (duration = 0 → only report on change; Report ID = 0)     */
    /*    Only meaningful for keyboards but harmless for mice.               */
    xhci_control_transfer(dev->slot_id,
        HID_RT_HOST_TO_IF,
        HID_REQ_SET_IDLE,
        0x0000,              /* duration=0 | report_id=0 */
        dev->iface_num,
        0, 0);

    /* 5. Configure the interrupt IN endpoint on the xHCI controller         */
    if (!xhci_configure_endpoint(dev->slot_id,
                                 state->ep_id,
                                 EP_TYPE_INT_IN,
                                 state->mps,
                                 state->interval)) {
        KLOG_ERR("hid: configure_endpoint failed\n");
        kfree(state);
        return false;
    }

    /* 6. Allocate a permanent DMA buffer for incoming reports (1 page)      */
    state->buf_virt = usb_dma_alloc(&state->buf_phys);
    if (!state->buf_virt) {
        KLOG_ERR("hid: DMA buffer allocation failed\n");
        kfree(state);
        return false;
    }

    /* 7. Queue the first interrupt IN transfer (IOC = true → event on done) */
    xhci_queue_transfer(dev->slot_id, state->ep_id,
                        state->buf_phys, state->mps, true);

    dev->class_data = state;

    KLOG_INFO("hid: %s attached on slot %u\n",
              (state->protocol == HID_PROTOCOL_KEYBOARD) ? "keyboard" : "mouse",
              dev->slot_id);
    return true;
}

/* ── Keyboard report parser ──────────────────────────────────────────────── */
static void parse_keyboard(hid_state_t *state, const uint8_t *buf) {
    uint8_t mods      = buf[0];
    /* buf[1] = reserved */
    const uint8_t *keys = buf + 2;   /* 6 keycode slots */

    /* Detect modifier changes (treat each bit as an individual "key") */
    uint8_t mod_changed = mods ^ state->last_mods;
    for (int b = 0; b < 8; b++) {
        if (mod_changed & (1u << b)) {
            uint8_t pressed = (mods >> b) & 1u;
            /* Modifier keycodes: 0xE0..0xE7 match USB HID modifier positions */
            input_push_key(mods, (uint8_t)(0xE0 + b),
                           pressed ? INPUT_KEY_PRESS : INPUT_KEY_RELEASE);
        }
    }
    state->last_mods = mods;

    /* Key releases: keys in last[] not in current buf */
    for (int i = 0; i < 6; i++) {
        uint8_t kc = state->last_keys[i];
        if (!kc) continue;
        bool still_held = false;
        for (int j = 0; j < 6; j++) {
            if (keys[j] == kc) { still_held = true; break; }
        }
        if (!still_held)
            input_push_key(mods, kc, INPUT_KEY_RELEASE);
    }

    /* Key presses: keys in current buf not in last[] */
    for (int i = 0; i < 6; i++) {
        uint8_t kc = keys[i];
        if (!kc || kc == 0x01 /* ErrorRollOver */) continue;
        bool was_held = false;
        for (int j = 0; j < 6; j++) {
            if (state->last_keys[j] == kc) { was_held = true; break; }
        }
        if (!was_held)
            input_push_key(mods, kc, INPUT_KEY_PRESS);
    }

    memcpy(state->last_keys, keys, 6);
}

/* ── Mouse report parser ─────────────────────────────────────────────────── */
static void parse_mouse(hid_state_t *state, const uint8_t *buf, uint32_t len) {
    (void)state;
    uint8_t  buttons = buf[0];
    int8_t   dx      = (len >= 2) ? (int8_t)buf[1] : 0;
    int8_t   dy      = (len >= 3) ? (int8_t)buf[2] : 0;
    int8_t   scroll  = (len >= 4) ? (int8_t)buf[3] : 0;
    input_push_mouse(buttons, dx, dy, scroll);
}

/* ── transfer_done ───────────────────────────────────────────────────────── */
static void hid_transfer_done(usb_device_t *dev,
                               uint8_t       ep_id,
                               uint8_t       cc,
                               uint32_t      residual,
                               void         *buf_ignored,
                               uint32_t      buf_len_ignored) {
    (void)buf_ignored; (void)buf_len_ignored;

    hid_state_t *state = (hid_state_t *)dev->class_data;
    if (!state || ep_id != state->ep_id) return;

    /* Short-packet (CC_SHORT_PACKET) and SUCCESS both carry valid data.     */
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        KLOG_WARN("hid: transfer ep=%u cc=%u — skip parse\n", ep_id, cc);
        goto requeue;
    }

    /* Actual received byte count = mps - residual                           */
    uint32_t received = (uint32_t)state->mps;
    if (residual <= received) received -= residual;

    /* Memory barrier before reading DMA buffer the xHCI just filled         */
    cpu_mfence();
    const uint8_t *data = (const uint8_t *)state->buf_virt;

    if (state->protocol == HID_PROTOCOL_KEYBOARD && received >= 2)
        parse_keyboard(state, data);
    else if (state->protocol == HID_PROTOCOL_MOUSE && received >= 3)
        parse_mouse(state, data, received);

    /* Zero buffer so stale data is not re-parsed if HW drops the next TRB  */
    memset(state->buf_virt, 0, state->mps);
    cpu_mfence();

requeue:
    /* Re-arm the IN pipe — always, even on error (device may recover)       */
    xhci_queue_transfer(dev->slot_id, state->ep_id,
                        state->buf_phys, state->mps, true);
}

/* ── detach ──────────────────────────────────────────────────────────────── */
static void hid_detach(usb_device_t *dev) {
    hid_state_t *state = (hid_state_t *)dev->class_data;
    if (!state) return;

    /* Free the DMA buffer */
    if (state->buf_phys)
        usb_dma_free(state->buf_phys);

    kfree(state);
    dev->class_data = NULL;
    KLOG_INFO("hid: device detached (slot %u)\n", dev->slot_id);
}

/* ── Driver table entry ──────────────────────────────────────────────────── */
const usb_class_driver_t g_hid_driver = {
    .name          = "hid-boot",
    .probe         = hid_probe,
    .attach        = hid_attach,
    .transfer_done = hid_transfer_done,
    .detach        = hid_detach,
};

void hid_init(void) {
    usb_register_class_driver(&g_hid_driver);
    KLOG_INFO("hid: boot-protocol driver registered\n");
}
