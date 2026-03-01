/* drivers/input/evdev.h — Linux evdev-compatible character device interface.
 *
 * Exposes /dev/input/event0 (keyboard, EV_KEY) and
 *          /dev/input/event1 (mouse,    EV_REL + EV_KEY).
 *
 * Each node delivers struct input_event records in the exact Linux ABI format
 * (24 bytes on x86-64). Fully compatible with libinput, SDL2-evdev, etc.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fs/fd.h"   /* file_ops_t, file_t */

/* ── Linux event types (EV_*) ────────────────────────────────────────────── */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04

/* ── EV_SYN codes ────────────────────────────────────────────────────────── */
#define SYN_REPORT      0x00

/* ── EV_REL codes ────────────────────────────────────────────────────────── */
#define REL_X           0x00
#define REL_Y           0x01
#define REL_WHEEL       0x08

/* ── Mouse button codes (EV_KEY) ─────────────────────────────────────────── */
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112

/* ── Linux KEY codes (EV_KEY) — subset used by our HID table ─────────────── */
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_NUMLOCK     69
#define KEY_SCROLLLOCK  70
#define KEY_KP7         71
#define KEY_KP8         72
#define KEY_KP9         73
#define KEY_KPMINUS     74
#define KEY_KP4         75
#define KEY_KP5         76
#define KEY_KP6         77
#define KEY_KPPLUS      78
#define KEY_KP1         79
#define KEY_KP2         80
#define KEY_KP3         81
#define KEY_KP0         82
#define KEY_KPDOT       83
#define KEY_F11         87
#define KEY_F12         88
#define KEY_KPENTER     96
#define KEY_RIGHTCTRL   97
#define KEY_KPSLASH     98
#define KEY_SYSRQ       99
#define KEY_RIGHTALT    100
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_INSERT      110
#define KEY_DELETE      111
#define KEY_PAUSE       119
#define KEY_LEFTMETA    125
#define KEY_RIGHTMETA   126
#define KEY_MAX         0x2FF

/* ── HID bus type ────────────────────────────────────────────────────────── */
#define BUS_USB         0x0003

/* ── Linux struct input_id ───────────────────────────────────────────────── */
typedef struct {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} evdev_input_id_t;

/* ── Linux struct input_event (24 bytes on x86-64) ──────────────────────── */
typedef struct {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed)) linux_input_event_t;

/* ── SPSC ring for evdev events ─────────────────────────────────────────── */
#define EVDEV_RING_SIZE  512u   /* power of 2 */

typedef struct {
    linux_input_event_t    buf[EVDEV_RING_SIZE];
    volatile uint32_t      head;   /* producer */
    volatile uint32_t      tail;   /* consumer */
} evdev_ring_t;

/* ── Node indices ────────────────────────────────────────────────────────── */
#define EVDEV_KBD_IDX   0
#define EVDEV_MOUSE_IDX 1
#define EVDEV_COUNT     2

/* ── evdev ioctls (Linux _IOC encoding) ──────────────────────────────────── */
/* _IOC(dir,type,nr,size): dir=2(R), type='E'=0x45, nr, size */
#define EVIOCGVERSION       0x80044501UL    /* _IOR('E',0x01,int)        */
#define EVIOCGID            0x80084502UL    /* _IOR('E',0x02,input_id)   */
/* Variable-size: strip bits [16..29] for comparison */
#define EVIO_CMD_BASE(cmd)  ((cmd) & ~0x3FFF0000UL)
#define EVIO_SIZE(cmd)      (((cmd) >> 16) & 0x3FFF)
#define EVIOCGNAME_BASE     0x80004506UL    /* EVIOCGNAME(len)  */
#define EVIOCGKEY_BASE      0x80004518UL    /* EVIOCGKEY(len)   */
#define EVIOCGBIT_SYN_BASE  0x80004520UL    /* EVIOCGBIT(EV_SYN,len) */
#define EVIOCGBIT_KEY_BASE  0x80004521UL    /* EVIOCGBIT(EV_KEY,len) */
#define EVIOCGBIT_REL_BASE  0x80004522UL    /* EVIOCGBIT(EV_REL,len) */

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Initialise evdev rings and key-state table. Call once at boot. */
void evdev_init(void);

/* Feed a key event from the HID driver.
 *   hid_keycode : USB HID boot-protocol key code (0x04–0x73)
 *   state       : INPUT_KEY_PRESS (1) or INPUT_KEY_RELEASE (0)
 *   modifiers   : current USB modifier byte (bit-field: LCtrl/LShift/…)
 * Internally translates HID→Linux keycode and pushes EV_KEY + EV_SYN.
 * Must be called AFTER the TTY char path so capslock is already tracked. */
void evdev_feed_key(uint8_t hid_keycode, uint8_t state, uint8_t modifiers);

/* Feed mouse motion and button state change from the HID driver.
 *   dx, dy          : signed pixel delta
 *   buttons         : current button bitmask (INPUT_BTN_LEFT etc.)
 *   prev_buttons    : previous button bitmask (to compute deltas) */
void evdev_feed_mouse(int8_t dx, int8_t dy, uint8_t buttons,
                      uint8_t prev_buttons, int8_t scroll);

/* file_ops for /dev/input/eventN (set as pending_f_ops in devfs_open) */
extern file_ops_t g_evdev_kbd_fops;
extern file_ops_t g_evdev_mouse_fops;

/* Current held-key bitmask (512 bits = 64 bytes), updated per key event */
extern uint8_t g_evdev_key_state[64];

/* Per-node event rings — exported so devfs can check availability for poll */
extern evdev_ring_t g_evdev_rings[EVDEV_COUNT];
