/* drivers/usb/hid.h — USB HID boot-protocol class driver
 *
 * Supports:
 *   USB HID keyboard  (boot protocol, 8-byte report)
 *   USB HID mouse     (boot protocol, 3–4-byte report)
 *
 * Non-blocking design:
 *   • attach()        : configure EP, allocate DMA buf, queue first IN transfer
 *   • transfer_done() : parse report → input ring → re-queue next IN transfer
 *   • No polling loop, no sleep — all activity is interrupt-driven via xHCI
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "usb_core.h"

/* ── Per-device HID state (stored in usb_device_t::class_data) ──────────── */
typedef struct {
    uint8_t   ep_id;          /* xHCI endpoint context index (DCI)          */
    uint8_t   protocol;       /* HID_PROTOCOL_KEYBOARD or HID_PROTOCOL_MOUSE */
    uint16_t  mps;            /* max packet size of the interrupt IN EP      */
    uint8_t   interval;       /* bInterval from endpoint descriptor          */

    /* DMA receive buffer — xHCI writes directly here via physical address   */
    void     *buf_virt;
    uintptr_t buf_phys;

    /* Keyboard: previous report state for delta detection                   */
    uint8_t   last_mods;
    uint8_t   last_keys[6];
} hid_state_t;

/* ── Class driver table entry (register this with usb_register_class_driver) */
extern const usb_class_driver_t g_hid_driver;

/* Register the HID driver — call once before usb_init()                    */
void hid_init(void);
