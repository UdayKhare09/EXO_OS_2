/* drivers/input/input.h — Kernel input event subsystem
 *
 * Lock-free SPSC ring buffer for keyboard and mouse events.
 * Producer: HID driver (interrupt / USB event task context)
 * Consumer: any kernel task or future userspace via syscall
 *
 * Ring is power-of-2 sized → index masking is branchless.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Event types ─────────────────────────────────────────────────────────── */
#define INPUT_EV_KEY   1   /* keyboard key press / release                  */
#define INPUT_EV_MOUSE 2   /* mouse movement + buttons                      */

/* Key states */
#define INPUT_KEY_PRESS   1
#define INPUT_KEY_RELEASE 0

/* Mouse button bitmask (matches USB HID boot report byte 0) */
#define INPUT_BTN_LEFT   (1 << 0)
#define INPUT_BTN_RIGHT  (1 << 1)
#define INPUT_BTN_MIDDLE (1 << 2)

/* ── HID keycode → ASCII (partial, boot-protocol scancodes 0x04-0x38) ───── */
/* Consumer can call input_keycode_to_ascii() for printable characters.     */

typedef struct {
    uint8_t  type;          /* INPUT_EV_KEY or INPUT_EV_MOUSE               */
    uint8_t  state;         /* INPUT_KEY_PRESS / RELEASE (key events)       */

    /* Key event */
    uint8_t  keycode;       /* USB HID keycode (0x00-0xFF)                  */
    uint8_t  modifiers;     /* USB modifier byte (shift/ctrl/alt/…)         */

    /* Mouse event */
    int8_t   mouse_dx;
    int8_t   mouse_dy;
    int8_t   mouse_scroll;
    uint8_t  mouse_buttons; /* bitmask: LEFT|RIGHT|MIDDLE                   */
} input_event_t;

/* ── Ring buffer ─────────────────────────────────────────────────────────── */
#define INPUT_RING_SIZE  256u   /* must be power of 2                       */

typedef struct {
    input_event_t buf[INPUT_RING_SIZE];
    volatile uint32_t head;   /* producer writes here (mod SIZE)            */
    volatile uint32_t tail;   /* consumer reads here  (mod SIZE)            */
} input_ring_t;

/* Initialise the global input rings */
void input_init(void);

/* Push an event (called from USB event task; non-blocking, drops if full) */
void input_push_key  (uint8_t modifiers, uint8_t keycode, uint8_t state);
void input_push_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t scroll);

/* Pop an event (returns false if ring is empty — non-blocking)            */
bool input_poll(input_event_t *out);

/* Convert HID boot keycode to ASCII (0 if non-printable)                 */
char input_keycode_to_ascii(uint8_t keycode, uint8_t modifiers);

/* TTY character queue (fed from key-press events).                        */
bool input_tty_char_available(void);
int  input_tty_getchar_nonblock(char *out_ch);

/* Global rings (exported so WM / fbcon can read directly)                */
extern input_ring_t g_kbd_ring;
extern input_ring_t g_mouse_ring;
