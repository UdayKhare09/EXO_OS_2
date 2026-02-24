/* drivers/usb/xhci.h — xHCI (USB 3.x) host controller driver
 *
 * Supports USB 3.2 Gen 1/2 (SuperSpeed/SuperSpeedPlus), USB 2.0 (HighSpeed),
 * USB 1.1 (Full/LowSpeed) via a single xHCI controller.
 *
 * Design:
 *   - Non-blocking: IRQ handler wakes a dedicated kernel task via sched_unblock
 *   - Command ring   : synchronous-style with completion polling (task context)
 *   - Transfer rings : one per active endpoint, async INT-IN for HID
 *   - Event ring     : single interrupter, processed by usb_event_task
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── PCI class codes for xHCI ────────────────────────────────────────────── */
#define PCI_CLASS_SERIAL_BUS   0x0C
#define PCI_SUBCLASS_USB       0x03
#define PCI_PROGIF_XHCI        0x30

/* ── Interrupt vector allocated to xHCI MSI ─────────────────────────────── */
#define XHCI_IRQ_VECTOR        0x40

/* ── xHCI Capability Register offsets (from MMIO base) ──────────────────── */
#define XHCI_CAP_CAPLENGTH     0x00   /* 1 byte: cap reg length              */
#define XHCI_CAP_HCIVERSION    0x02   /* 2 bytes: BCD version                */
#define XHCI_CAP_HCSPARAMS1    0x04   /* max slots[23:16], max intrs[18:8],  */
                                      /*          max ports[31:24]           */
#define XHCI_CAP_HCSPARAMS2    0x08   /* IST, ERST max, SPR, max scratchpad  */
#define XHCI_CAP_HCSPARAMS3    0x0C
#define XHCI_CAP_HCCPARAMS1    0x10   /* AC64, BNC, CSZ, PPC, PIND, LHRC,   */
                                      /* LTC, NSS, PAE, SPC, SEC, CFC,       */
                                      /* MaxPSASize, xECP                    */
#define XHCI_CAP_DBOFF         0x14   /* doorbell array offset (DWORD)       */
#define XHCI_CAP_RTSOFF        0x18   /* runtime regs offset (DWORD, 32-aln) */
#define XHCI_CAP_HCCPARAMS2    0x1C

/* HCCPARAMS1 bit */
#define HCCPARAMS1_CSZ         (1 << 2)  /* 1 = 64-byte contexts, 0 = 32-byte */
#define HCCPARAMS1_AC64        (1 << 0)  /* 64-bit addressing capable          */

/* ── Operational Register offsets (from op_base = MMIO + CAPLENGTH) ──────── */
#define XHCI_OP_USBCMD         0x00
#define XHCI_OP_USBSTS         0x04
#define XHCI_OP_PAGESIZE       0x08
#define XHCI_OP_DNCTRL         0x14
#define XHCI_OP_CRCR_LO        0x18   /* command ring control (low 32 bits)  */
#define XHCI_OP_CRCR_HI        0x1C
#define XHCI_OP_DCBAAP_LO      0x30   /* device context base address array   */
#define XHCI_OP_DCBAAP_HI      0x34
#define XHCI_OP_CONFIG         0x38

/* USBCMD bits */
#define USBCMD_RUN             (1 << 0)
#define USBCMD_HCRST           (1 << 1)  /* host controller reset             */
#define USBCMD_INTE            (1 << 2)  /* interrupter enable                */
#define USBCMD_HSEE            (1 << 3)  /* host system error enable          */

/* USBSTS bits */
#define USBSTS_HCH             (1 << 0)  /* host controller halted            */
#define USBSTS_HSE             (1 << 2)  /* host system error                 */
#define USBSTS_EINT            (1 << 3)  /* event interrupt                   */
#define USBSTS_PCD             (1 << 4)  /* port change detected              */
#define USBSTS_CNR             (1 << 11) /* controller not ready              */

/* CRCR bits */
#define CRCR_RCS               (1 << 0)  /* ring cycle state                  */
#define CRCR_CS                (1 << 1)  /* command stop                      */
#define CRCR_CA                (1 << 2)  /* command abort                     */
#define CRCR_CRR               (1 << 3)  /* command ring running              */

/* ── Port Status/Control (at op_base + 0x400 + port*0x10) ───────────────── */
#define XHCI_PORT_BASE         0x400
#define XHCI_PORT_STRIDE       0x10

#define PORTSC_CCS             (1 <<  0)  /* current connect status           */
#define PORTSC_PED             (1 <<  1)  /* port enabled/disabled            */
#define PORTSC_OCA             (1 <<  3)  /* over-current active              */
#define PORTSC_PR              (1 <<  4)  /* port reset                       */
#define PORTSC_PLS_MASK        (0xF<< 5)  /* port link state [8:5]            */
#define PORTSC_PLS_U0          (0  << 5)  /* active                           */
#define PORTSC_PP              (1 <<  9)  /* port power                       */
#define PORTSC_SPEED_MASK      (0xF<<10)  /* port speed [13:10]               */
#define PORTSC_SPEED_FULL      (1  <<10)  /* USB 1.1 full speed               */
#define PORTSC_SPEED_LOW       (2  <<10)  /* USB 1.0 low  speed               */
#define PORTSC_SPEED_HIGH      (3  <<10)  /* USB 2.0 high speed               */
#define PORTSC_SPEED_SUPER     (4  <<10)  /* USB 3.x super speed              */
#define PORTSC_CSC             (1 <<17)   /* connect status change            */
#define PORTSC_PEC             (1 <<18)   /* port enable change               */
#define PORTSC_PRC             (1 <<21)   /* port reset change                */
#define PORTSC_PLC             (1 <<22)   /* port link state change           */
#define PORTSC_WPR             (1 <<31)   /* warm port reset (USB3)           */

/* Write-1-to-clear (W1C) bits that must be preserved / cleared on writes   */
#define PORTSC_W1C_BITS  (PORTSC_CSC|PORTSC_PEC|PORTSC_PRC|PORTSC_PLC| \
                          (1<<23)|(1<<24)|(1<<25)|(1<<26)|(1<<27)|(1<<28))

/* ── Runtime Register offsets (from rt_base = MMIO + RTSOFF) ────────────── */
#define XHCI_RT_MFINDEX        0x00
#define XHCI_RT_IR_BASE        0x20   /* interrupter 0 register set          */
#define XHCI_RT_IR_STRIDE      0x20

/* Interrupter register offsets (relative to XHCI_RT_IR_BASE) */
#define IR_IMAN                0x00   /* interrupt management                */
#define IR_IMOD                0x04   /* interrupt moderation                */
#define IR_ERSTSZ              0x08   /* event ring segment table size       */
#define IR_ERSTBA_LO           0x10   /* ERST base address low               */
#define IR_ERSTBA_HI           0x14
#define IR_ERDP_LO             0x18   /* event ring dequeue pointer low      */
#define IR_ERDP_HI             0x1C

#define IMAN_IE                (1 << 1)  /* interrupt enable                 */
#define IMAN_IP                (1 << 0)  /* interrupt pending (W1C)          */
#define ERDP_EHB               (1 << 3)  /* event handler busy (W1C)         */

/* ── TRB (Transfer Request Block) — 16 bytes ─────────────────────────────── */
typedef struct __attribute__((packed, aligned(16))) {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/* TRB type field (control bits [15:10]) */
#define TRB_TYPE(t)            ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(ctrl)     (((ctrl) >> 10) & 0x3F)

/* TRB control bit 0 = Cycle bit */
#define TRB_C                  (1u << 0)
/* TRB control bit 1 = Toggle Cycle (Link TRB) or Evaluate Next TRB        */
#define TRB_TC                 (1u << 1)
/* TRB control bit 5 = Interrupt On Completion                              */
#define TRB_IOC                (1u << 5)
/* TRB control bit 4 = Chain bit (multi-TRB TD)                            */
#define TRB_CH                 (1u << 4)
/* TRB control bit 2 = Interrupt on Short Packet                            */
#define TRB_ISP                (1u << 2)
/* TRB control bit 6 = Immediate Data (Setup-stage TRB)                    */
#define TRB_IDT                (1u << 6)

/* Transfer TRB types */
#define TTRB_NORMAL            1u
#define TTRB_SETUP_STAGE       2u
#define TTRB_DATA_STAGE        3u
#define TTRB_STATUS_STAGE      4u
#define TTRB_LINK              6u
#define TTRB_EVENT_DATA        7u
#define TTRB_NOOP              8u

/* Command TRB types */
#define CTRB_ENABLE_SLOT       9u
#define CTRB_DISABLE_SLOT      10u
#define CTRB_ADDRESS_DEVICE    11u
#define CTRB_CONFIG_ENDPOINT   12u
#define CTRB_EVAL_CONTEXT      13u
#define CTRB_RESET_ENDPOINT    14u
#define CTRB_STOP_ENDPOINT     15u
#define CTRB_SET_TR_DEQUEUE    16u
#define CTRB_RESET_DEVICE      17u
#define CTRB_NOOP              23u

/* Event TRB types */
#define ETRB_TRANSFER          32u
#define ETRB_CMD_COMPLETION    33u
#define ETRB_PORT_STATUS       34u
#define ETRB_MFWRAP            39u

/* Completion codes (event TRB status[31:24]) */
#define CC_INVALID             0u
#define CC_SUCCESS             1u
#define CC_DATA_BUFFER_ERR     2u
#define CC_BABBLE_ERR          3u
#define CC_USB_TRANSACTION_ERR 4u
#define CC_TRB_ERR             5u
#define CC_STALL_ERR           6u
#define CC_RESOURCE_ERR        7u
#define CC_BANDWIDTH_ERR       8u
#define CC_NO_SLOTS_ERR        9u
#define CC_SHORT_PACKET        13u
#define CC_RING_UNDERRUN       14u
#define CC_RING_OVERRUN        15u
#define CC_STOPPED             26u

/* Extract completion code from event TRB status */
#define TRB_CC(status)         (((status) >> 24) & 0xFFu)
/* Extract slot ID from event TRB control */
#define TRB_SLOT(ctrl)         (((ctrl)   >> 24) & 0xFFu)
/* Extract endpoint ID from transfer event control */
#define TRB_EP_ID(ctrl)        (((ctrl)   >> 16) & 0x1Fu)

/* ── Slot/Endpoint context structures (32-byte variant) ──────────────────── */
/* CSZ bit in HCCPARAMS1 selects 32-byte (0) or 64-byte (1) contexts.       */
/* We support both — context_size member in xhci_t controls stride.         */

typedef struct __attribute__((packed)) {
    uint32_t dw0;   /* route string, speed, MTT, hub, ctx entries           */
    uint32_t dw1;   /* max exit latency, root hub port number, port count   */
    uint32_t dw2;   /* interrupter target, TT hub slot, TT port, TTT, IR   */
    uint32_t dw3;   /* USB device address, slot state                       */
    uint32_t rsvd[4];
} xhci_slot_ctx_t;   /* 32 bytes */

/* Slot context DW0 fields */
#define SLOT_CTX_ENTRIES(n)    ((n) << 27)    /* context entries (must ≥ EP count) */
#define SLOT_CTX_SPEED(s)      ((s) << 20)    /* port speed (same encoding as PORTSC) */
#define SLOT_CTX_ROOT_PORT(p)  ((p) << 16)    /* bits 23:16 in DW1 */
#define SLOT_CTX_STATE(s)      ((s) << 27)    /* slot state in DW3 */

typedef struct __attribute__((packed)) {
    uint32_t dw0;   /* EP state, mult, max primary streams, interval, HID  */
    uint32_t dw1;   /* max packet size, max burst size, EP type, error count */
    uint64_t dequeue_ptr;  /* TR dequeue pointer + DCS (cycle state)        */
    uint32_t dw4;   /* average TRB length, max ESIT payload                 */
    uint32_t rsvd[3];
} xhci_ep_ctx_t;   /* 32 bytes */

/* EP type encoding for ep_ctx dw1 bits [5:3] */
#define EP_TYPE_INVALID        0u
#define EP_TYPE_ISOCH_OUT      1u
#define EP_TYPE_BULK_OUT       2u
#define EP_TYPE_INT_OUT        3u
#define EP_TYPE_CONTROL        4u
#define EP_TYPE_ISOCH_IN       5u
#define EP_TYPE_BULK_IN        6u
#define EP_TYPE_INT_IN         7u

/* EP context DW0 interval for interrupt endpoints = 2^(interval-1) * 125us */
/* DW1[5:3] = EP type, DW1[31:16] = max packet size                        */
/* DCS (dequeue cycle state) = bit 0 of dequeue_ptr                        */

/* ── Input Context (for commands) ───────────────────────────────────────── */
/* Input Control Context + Slot Context + up to 31 EP Contexts              */
/* Physical layout: [ICC 32B][Slot 32B][EP0 32B][EP1 32B]…                  */

typedef struct __attribute__((packed)) {
    uint32_t drop_flags;    /* bits 1-31: drop EP N context (bit 1 = EP0) */
    uint32_t add_flags;     /* bits 1-31: add  EP N context                */
    uint32_t rsvd[6];
} xhci_input_ctrl_ctx_t;   /* 32 bytes */

/* ── Ring abstraction ────────────────────────────────────────────────────── */
#define XHCI_RING_SIZE         64u    /* TRBs per ring segment (power of 2) */
#define XHCI_EVT_RING_SIZE     256u   /* event ring size (power of 2)       */
#define XHCI_MAX_PENDING       8u     /* deferred port-connect queue depth  */
#define XHCI_MAX_PORTS         255u   /* compile-time max ports (spec limit) */

/* Atomic wakeup-pending counter: bumped by xhci_irq, read+cleared by
 * usb_event_task before sched_block() to avoid the lost-wakeup race.     */
extern volatile int g_xhci_ev_pending;

typedef struct {
    xhci_trb_t  *trbs;       /* virtual address of TRB array (page-aligned) */
    uintptr_t    phys;        /* physical address                            */
    uint32_t     enqueue;     /* next TRB slot to write                      */
    uint32_t     dequeue;     /* next TRB slot to read (event ring only)     */
    uint8_t      cycle;       /* producer cycle bit                          */
    uint32_t     size;        /* number of TRBs (including Link TRB)         */
} xhci_ring_t;

/* Event Ring Segment Table entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t base_addr;
    uint16_t size;
    uint16_t rsvd[3];
} xhci_erst_entry_t;

/* ── Per-device state ────────────────────────────────────────────────────── */
#define XHCI_MAX_SLOTS         32u    /* we support up to 32 slots safely    */
#define XHCI_MAX_EP            16u    /* endpoints per device                */

typedef struct {
    bool      valid;
    uint8_t   slot_id;
    uint8_t   port;           /* 1-based port number                         */
    uint8_t   speed;          /* PORTSC speed encoding                       */
    uint8_t   address;        /* USB device address                          */
    xhci_ring_t xfer_rings[XHCI_MAX_EP];  /* one per endpoint context index  */

    /* Contexts (contiguous: [IOC][Slot][EP0][EP1]…) physical + virtual */
    void     *input_ctx_virt;
    uintptr_t input_ctx_phys;
    void     *output_ctx_virt;
    uintptr_t output_ctx_phys;
} xhci_slot_t;

/* ── Main xHCI controller state ─────────────────────────────────────────── */
typedef struct {
    /* MMIO regions */
    volatile uint8_t  *cap_base;   /* capability registers                  */
    volatile uint8_t  *op_base;    /* operational registers                 */
    volatile uint8_t  *rt_base;    /* runtime registers                     */
    volatile uint32_t *db_base;    /* doorbell array                        */

    /* Capabilities decoded at init */
    uint8_t   ctx_size;            /* 32 or 64 bytes per context entry      */
    uint8_t   max_ports;
    uint8_t   max_slots;

    /* Command ring */
    xhci_ring_t cmd_ring;

    /* Event ring (interrupter 0) */
    xhci_ring_t  evt_ring;
    xhci_erst_entry_t *erst;       /* event ring segment table (virtual)    */
    uintptr_t          erst_phys;

    /* Device Context Base Address Array (DCBAA) */
    uint64_t  *dcbaa;              /* virtual address (page-aligned)         */
    uintptr_t  dcbaa_phys;

    /* Per-slot state */
    xhci_slot_t slots[XHCI_MAX_SLOTS + 1];  /* index 0 unused (spec)        */

    /* Pending command completion (polled from usb_event_task) */
    volatile bool      cmd_pending;
    volatile uint8_t   cmd_slot;    /* slot id from last completion          */
    volatile uint8_t   cmd_cc;      /* completion code from last completion  */
    volatile uintptr_t cmd_trb_phys;/* physical address of pending cmd TRB  */

    /* Deferred port-connect queue (populated by event ring, drained outside) */
    uint8_t  pending_ports [XHCI_MAX_PENDING];
    uint8_t  pending_speeds[XHCI_MAX_PENDING];
    uint8_t  pending_count;

    /* Per-port active flag — set when enumeration starts, cleared on disconnect */
    bool     port_active[XHCI_MAX_PORTS + 1];
} xhci_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Find and initialise the first xHCI controller on the PCI bus.
 * Returns true on success.  Call once after PCI enumeration.               */
bool xhci_init(void);

/* Process all pending events from the event ring.
 * Called by the USB event task.  Drives port hotplug, command completions,
 * and transfer completions.
 * Returns the number of events processed (0 = ring was empty).             */
int xhci_process_events(void);

/* Handle any ports queued by xhci_process_events() for enumeration.
 * Must be called OUTSIDE xhci_process_events() — does port reset + enum.  */
void xhci_handle_pending_connects(void);

/* Kick the command ring doorbell (slot 0) */
void xhci_ring_cmd_doorbell(void);

/* Kick an endpoint doorbell for a slot */
void xhci_ring_ep_doorbell(uint8_t slot, uint8_t ep_id);

/* Submit a command TRB and wait for completion (task context only).
 * Returns completion code (CC_SUCCESS = ok).                               */
uint8_t xhci_submit_cmd(xhci_trb_t *trb);

/* Reset a port, wait for it to come out of reset */
bool xhci_port_reset(uint8_t port);

/* Enable a new device slot; returns 1-based slot_id (0 on failure)        */
uint8_t xhci_do_enable_slot(void);

/* Address Device command; bsr=true skips SET_ADDRESS (init pass only).
 * Returns USB device address (0 on failure).                               */
uint8_t xhci_do_address_device(uint8_t slot_id, bool bsr);

/* Direct (non-wrapper) address device — same as do_address_device          */
uint8_t xhci_address_device(uint8_t slot_id, bool bsr);

/* Add an endpoint context to a slot and issue Configure Endpoint command.  */
bool xhci_configure_endpoint(uint8_t slot_id,
                              uint8_t ep_id,    /* xHCI DCI: num*2+dir     */
                              uint8_t ep_type,  /* EP_TYPE_INT_IN etc.     */
                              uint16_t mps,
                              uint8_t  interval);

/* Queue a Normal TRB on an endpoint's transfer ring and ring the doorbell. */
void xhci_queue_transfer(uint8_t slot_id, uint8_t ep_id,
                          uintptr_t phys, uint32_t length, bool ioc);

/* Issue a full USB control transfer on EP0 (task context only).            */
void xhci_control_transfer(uint8_t   slot_id,
                            uint8_t   bmRequestType,
                            uint8_t   bRequest,
                            uint16_t  wValue,
                            uint16_t  wIndex,
                            uint16_t  wLength,
                            uintptr_t data_phys);

/* Get internal slot state (used by usb_core / class drivers)               */
xhci_slot_t *xhci_get_slot(uint8_t slot_id);

/* Allocate device contexts + EP0 transfer ring for a slot.                 */
/* Must be called after Enable Slot, before Address Device.                 */
bool xhci_slot_alloc_ctx(uint8_t slot_id, uint8_t port, uint8_t speed);

/* The running task pointer that handles USB events (set by usb_init) */
extern struct task *g_usb_event_task;

/* Global controller instance (single-controller system) */
extern xhci_t g_xhci;
