/* drivers/usb/usb_core.h — USB protocol layer
 *
 * Responsible for:
 *   - Standard USB descriptors (device, configuration, interface, endpoint)
 *   - Control transfer wrappers (GET_DESCRIPTOR, SET_CONFIGURATION, etc.)
 *   - Enumeration: new port → discover class → bind class driver
 *   - Transfer completion routing from xHCI to class drivers
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── bmRequestType direction / type / recipient ─────────────────────────── */
#define USB_DIR_HOST_TO_DEV     0x00
#define USB_DIR_DEV_TO_HOST     0x80
#define USB_TYPE_STANDARD       0x00
#define USB_TYPE_CLASS          0x20
#define USB_TYPE_VENDOR         0x40
#define USB_RECIP_DEVICE        0x00
#define USB_RECIP_INTERFACE     0x01
#define USB_RECIP_ENDPOINT      0x02

/* ── Standard bRequest codes ────────────────────────────────────────────── */
#define USB_REQ_GET_STATUS       0x00
#define USB_REQ_CLEAR_FEATURE    0x01
#define USB_REQ_SET_FEATURE      0x03
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_DESCRIPTOR   0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE    0x0A
#define USB_REQ_SET_INTERFACE    0x0B

/* ── Descriptor types ───────────────────────────────────────────────────── */
#define USB_DT_DEVICE            0x01
#define USB_DT_CONFIG            0x02
#define USB_DT_STRING            0x03
#define USB_DT_INTERFACE         0x04
#define USB_DT_ENDPOINT          0x05
#define USB_DT_HID               0x21
#define USB_DT_HID_REPORT        0x22

/* ── Device class codes ─────────────────────────────────────────────────── */
#define USB_CLASS_HID            0x03
#define USB_CLASS_MASS_STORAGE   0x08
#define USB_CLASS_HUB            0x09

/* ── HID subclass / protocol ────────────────────────────────────────────── */
#define HID_SUBCLASS_BOOT        0x01
#define HID_PROTOCOL_KEYBOARD    0x01
#define HID_PROTOCOL_MOUSE       0x02

/* ── HID class request codes (bmRequestType = 0x21) ────────────────────── */
#define HID_REQ_SET_IDLE         0x0A
#define HID_REQ_SET_PROTOCOL     0x0B
#define HID_BOOT_PROTOCOL        0x00   /* wValue for SET_PROTOCOL boot     */

/* ── USB descriptor structures ──────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;   /* 18 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;   /* 9 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_iface_desc_t;    /* 9 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;  /* bit 7: direction (1=IN), bits[3:0]: EP num */
    uint8_t  bmAttributes;      /* bits[1:0]: transfer type                   */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;         /* polling interval (frames / microframes)    */
} usb_ep_desc_t;       /* 7 bytes */

/* bmAttributes transfer type */
#define USB_EP_CONTROL         0x00
#define USB_EP_ISOCHRONOUS     0x01
#define USB_EP_BULK            0x02
#define USB_EP_INTERRUPT       0x03

/* ── USB device (our internal view after enumeration) ───────────────────── */
#define USB_MAX_ENDPOINTS      8

typedef struct {
    uint8_t  addr;              /* USB address assigned by addressing         */
    uint8_t  slot_id;           /* xHCI slot ID                               */
    uint8_t  port;
    uint8_t  speed;

    usb_device_desc_t dev_desc;

    /* First configuration, first interface */
    uint8_t  iface_num;         /* bInterfaceNumber (wIndex for class reqs) */
    uint8_t  iface_class;
    uint8_t  iface_subclass;
    uint8_t  iface_protocol;

    /* Discovered endpoints */
    usb_ep_desc_t eps[USB_MAX_ENDPOINTS];
    uint8_t       ep_count;

    /* Class-driver private data (e.g., HID state) */
    void *class_data;
} usb_device_t;

/* ── USB class driver interface ─────────────────────────────────────────── */
typedef struct {
    const char *name;
    /* Returns true if this driver handles the given interface class/subclass */
    bool (*probe)(uint8_t class, uint8_t subclass, uint8_t protocol);
    /* Called after enumeration to set up class-specific state               */
    bool (*attach)(usb_device_t *dev);
    /* Called when a transfer on any of the device's endpoints completes     */
    void (*transfer_done)(usb_device_t *dev, uint8_t ep_id,
                          uint8_t cc, uint32_t residual,
                          void *buf, uint32_t buf_len);
    /* Called when device is detached (cleanup)                              */
    void (*detach)(usb_device_t *dev);
} usb_class_driver_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Initialise USB core, register built-in class drivers                     */
void usb_core_init(void);

/* Register a class driver (call before usb_init)                           */
void usb_register_class_driver(const usb_class_driver_t *drv);

/* Called by xHCI on port connect event                                     */
void usb_on_port_connected(uint8_t port, uint8_t speed);

/* Called by xHCI when a transfer completes                                 */
void usb_on_transfer_complete(uint8_t slot_id, uint8_t ep_id,
                               uint8_t cc, uint32_t residual);

/* Control transfer wrappers (task context only) */
bool usb_get_descriptor(usb_device_t *dev, uint8_t dt, uint8_t idx,
                         uint16_t lang, void *buf, uint16_t len);
bool usb_set_configuration(usb_device_t *dev, uint8_t value);
bool usb_get_class_descriptor(usb_device_t *dev, uint8_t iface,
                               uint8_t dt, uint8_t idx,
                               void *buf, uint16_t len);

/* DMA page allocator (1 page, zeroed) — usable by class drivers            */
void *usb_dma_alloc(uintptr_t *phys_out);
void  usb_dma_free (uintptr_t phys);

/* Top-level: find xHCI, enumerate devices, bind class drivers              */
bool usb_init(void);

/* ── Enumerated device query (for lsusb) ────────────────────────────────── */
/* Return the n-th enumerated USB device (0-based).  Returns NULL if oob. */
const usb_device_t *usb_get_device(int n);

/* Return total number of enumerated USB devices. */
int usb_device_count(void);
