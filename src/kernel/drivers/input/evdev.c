/* drivers/input/evdev.c — Linux evdev character device implementation.
 *
 * Translates the internal HID event rings into Linux struct input_event
 * records consumable from /dev/input/event0 (keyboard) and
 * /dev/input/event1 (mouse).
 *
 * The HID USB boot-protocol keycode range is 0x04–0x73.
 * Modifier bits in byte-0 of the HID report are synthesised here as
 * separate EV_KEY events for LEFT/RIGHTCTRL, LEFT/RIGHTSHIFT, etc.
 */
#include "evdev.h"
#include "input.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "arch/x86_64/cpu.h"   /* cpu_mfence */
#include "sched/sched.h"       /* sched_get_ticks, sched_sleep */
#include "net/socket_defs.h"   /* POLLIN, POLLOUT, POLLRDNORM, POLLWRNORM */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Global state ─────────────────────────────────────────────────────────── */
evdev_ring_t g_evdev_rings[EVDEV_COUNT];
uint8_t      g_evdev_key_state[64];   /* 512-bit bitmap: bit set = key held */

/* ── HID keycode (USB page 7) → Linux KEY_* translation table ─────────────
 * Index = HID usage code; value = Linux keycode (0 = not mapped). */
static const uint16_t g_hid_to_key[0x80] = {
    /* 0x00 */ 0,
    /* 0x01 */ KEY_ESC,         /* Keyboard ErrorRollOver / used as phantom */
    /* 0x02 */ 0,               /* POST Fail */
    /* 0x03 */ 0,               /* Undefined */
    /* 0x04 */ KEY_A,
    /* 0x05 */ KEY_B,
    /* 0x06 */ KEY_C,
    /* 0x07 */ KEY_D,
    /* 0x08 */ KEY_E,
    /* 0x09 */ KEY_F,
    /* 0x0A */ KEY_G,
    /* 0x0B */ KEY_H,
    /* 0x0C */ KEY_I,
    /* 0x0D */ KEY_J,
    /* 0x0E */ KEY_K,
    /* 0x0F */ KEY_L,
    /* 0x10 */ KEY_M,
    /* 0x11 */ KEY_N,
    /* 0x12 */ KEY_O,
    /* 0x13 */ KEY_P,
    /* 0x14 */ KEY_Q,
    /* 0x15 */ KEY_R,
    /* 0x16 */ KEY_S,
    /* 0x17 */ KEY_T,
    /* 0x18 */ KEY_U,
    /* 0x19 */ KEY_V,
    /* 0x1A */ KEY_W,
    /* 0x1B */ KEY_X,
    /* 0x1C */ KEY_Y,
    /* 0x1D */ KEY_Z,
    /* 0x1E */ KEY_1,
    /* 0x1F */ KEY_2,
    /* 0x20 */ KEY_3,
    /* 0x21 */ KEY_4,
    /* 0x22 */ KEY_5,
    /* 0x23 */ KEY_6,
    /* 0x24 */ KEY_7,
    /* 0x25 */ KEY_8,
    /* 0x26 */ KEY_9,
    /* 0x27 */ KEY_0,
    /* 0x28 */ KEY_ENTER,
    /* 0x29 */ KEY_ESC,
    /* 0x2A */ KEY_BACKSPACE,
    /* 0x2B */ KEY_TAB,
    /* 0x2C */ KEY_SPACE,
    /* 0x2D */ KEY_MINUS,
    /* 0x2E */ KEY_EQUAL,
    /* 0x2F */ KEY_LEFTBRACE,
    /* 0x30 */ KEY_RIGHTBRACE,
    /* 0x31 */ KEY_BACKSLASH,
    /* 0x32 */ KEY_BACKSLASH,   /* non-US backslash */
    /* 0x33 */ KEY_SEMICOLON,
    /* 0x34 */ KEY_APOSTROPHE,
    /* 0x35 */ KEY_GRAVE,
    /* 0x36 */ KEY_COMMA,
    /* 0x37 */ KEY_DOT,
    /* 0x38 */ KEY_SLASH,
    /* 0x39 */ KEY_CAPSLOCK,
    /* 0x3A */ KEY_F1,
    /* 0x3B */ KEY_F2,
    /* 0x3C */ KEY_F3,
    /* 0x3D */ KEY_F4,
    /* 0x3E */ KEY_F5,
    /* 0x3F */ KEY_F6,
    /* 0x40 */ KEY_F7,
    /* 0x41 */ KEY_F8,
    /* 0x42 */ KEY_F9,
    /* 0x43 */ KEY_F10,
    /* 0x44 */ KEY_F11,
    /* 0x45 */ KEY_F12,
    /* 0x46 */ KEY_SYSRQ,
    /* 0x47 */ KEY_SCROLLLOCK,
    /* 0x48 */ KEY_PAUSE,
    /* 0x49 */ KEY_INSERT,
    /* 0x4A */ KEY_HOME,
    /* 0x4B */ KEY_PAGEUP,
    /* 0x4C */ KEY_DELETE,
    /* 0x4D */ KEY_END,
    /* 0x4E */ KEY_PAGEDOWN,
    /* 0x4F */ KEY_RIGHT,
    /* 0x50 */ KEY_LEFT,
    /* 0x51 */ KEY_DOWN,
    /* 0x52 */ KEY_UP,
    /* 0x53 */ KEY_NUMLOCK,
    /* 0x54 */ KEY_KPSLASH,
    /* 0x55 */ KEY_KPASTERISK,
    /* 0x56 */ KEY_KPMINUS,
    /* 0x57 */ KEY_KPPLUS,
    /* 0x58 */ KEY_KPENTER,
    /* 0x59 */ KEY_KP1,
    /* 0x5A */ KEY_KP2,
    /* 0x5B */ KEY_KP3,
    /* 0x5C */ KEY_KP4,
    /* 0x5D */ KEY_KP5,
    /* 0x5E */ KEY_KP6,
    /* 0x5F */ KEY_KP7,
    /* 0x60 */ KEY_KP8,
    /* 0x61 */ KEY_KP9,
    /* 0x62 */ KEY_KP0,
    /* 0x63 */ KEY_KPDOT,
    /* 0x64 */ KEY_BACKSLASH,   /* non-US \| */
    /* 0x65 */ 0,               /* Application */
    /* 0x66 */ 0,               /* Power */
    /* 0x67 */ 0,               /* Keypad = */
    /* 0x68–0x6F: F13-F20 not supported */
    /* 0x68 */ 0, /* 0x69 */ 0, /* 0x6A */ 0, /* 0x6B */ 0,
    /* 0x6C */ 0, /* 0x6D */ 0, /* 0x6E */ 0, /* 0x6F */ 0,
    /* 0x70 */ 0, /* 0x71 */ 0, /* 0x72 */ 0, /* 0x73 */ 0,
    /* 0x74–0x7F: not used */
    /* 0x74 */ 0, /* 0x75 */ 0, /* 0x76 */ 0, /* 0x77 */ 0,
    /* 0x78 */ 0, /* 0x79 */ 0, /* 0x7A */ 0, /* 0x7B */ 0,
    /* 0x7C */ 0, /* 0x7D */ 0, /* 0x7E */ 0, /* 0x7F */ 0,
};

/* Modifier-bit → Linux keycode (bit index 0-7 of the USB modifier byte) */
static const uint16_t g_mod_keycodes[8] = {
    KEY_LEFTCTRL,   /* bit 0 */
    KEY_LEFTSHIFT,  /* bit 1 */
    KEY_LEFTALT,    /* bit 2 */
    KEY_LEFTMETA,   /* bit 3 */
    KEY_RIGHTCTRL,  /* bit 4 */
    KEY_RIGHTSHIFT, /* bit 5 */
    KEY_RIGHTALT,   /* bit 6 */
    KEY_RIGHTMETA,  /* bit 7 */
};

/* ── Low-level ring push ─────────────────────────────────────────────────── */
static void evdev_ring_push(evdev_ring_t *r, uint16_t type, uint16_t code,
                            int32_t value) {
    uint64_t ms = sched_get_ticks();
    uint32_t h    = r->head;
    uint32_t next = (h + 1) & (EVDEV_RING_SIZE - 1);
    if (next == r->tail) {
        /* Ring full: drop oldest event to make room */
        r->tail = (r->tail + 1) & (EVDEV_RING_SIZE - 1);
    }
    r->buf[h].tv_sec  = (int64_t)(ms / 1000ULL);
    r->buf[h].tv_usec = (int64_t)((ms % 1000ULL) * 1000ULL);
    r->buf[h].type    = type;
    r->buf[h].code    = code;
    r->buf[h].value   = value;
    cpu_mfence();
    r->head = next;
}

/* ── Set/clear key-state bit ─────────────────────────────────────────────── */
static void key_state_set(uint16_t code, int pressed) {
    if (code >= 512) return;
    uint32_t byte = code / 8;
    uint8_t  bit  = (uint8_t)(1u << (code % 8));
    if (pressed)
        g_evdev_key_state[byte] |=  bit;
    else
        g_evdev_key_state[byte] &= ~bit;
}

/* ── Previous modifier state (to synthesise per-bit events) ─────────────── */
static uint8_t g_prev_modifiers = 0;

/* ── Public init ─────────────────────────────────────────────────────────── */
void evdev_init(void) {
    for (int i = 0; i < EVDEV_COUNT; i++) {
        g_evdev_rings[i].head = 0;
        g_evdev_rings[i].tail = 0;
    }
    memset(g_evdev_key_state, 0, sizeof(g_evdev_key_state));
    g_prev_modifiers = 0;
    KLOG_INFO("evdev: initialised (event0=kbd, event1=mouse)\n");
}

/* ── Feed a key event ────────────────────────────────────────────────────── */
void evdev_feed_key(uint8_t hid_keycode, uint8_t state, uint8_t modifiers) {
    evdev_ring_t *kbd = &g_evdev_rings[EVDEV_KBD_IDX];

    /* 1. Synthesise modifier key events for any changed modifier bits */
    uint8_t changed = g_prev_modifiers ^ modifiers;
    if (changed) {
        for (int b = 0; b < 8; b++) {
            if (!(changed & (1u << b))) continue;
            uint16_t mk = g_mod_keycodes[b];
            int pressed = (modifiers & (1u << b)) ? 1 : 0;
            key_state_set(mk, pressed);
            evdev_ring_push(kbd, EV_KEY, mk, pressed);
        }
        evdev_ring_push(kbd, EV_SYN, SYN_REPORT, 0);
        g_prev_modifiers = modifiers;
    }

    /* 2. Translate the HID keycode to a Linux keycode */
    if (hid_keycode == 0) return; /* no key */
    if (hid_keycode >= 0x80) return; /* out of table */
    uint16_t lk = g_hid_to_key[hid_keycode];
    if (lk == 0) return; /* unmapped key */

    /* 3. Push EV_KEY event */
    int pv = (state == INPUT_KEY_PRESS) ? 1 : 0;
    key_state_set(lk, pv);
    evdev_ring_push(kbd, EV_KEY, lk, pv);
    evdev_ring_push(kbd, EV_SYN, SYN_REPORT, 0);
}

/* ── Feed mouse events ───────────────────────────────────────────────────── */
void evdev_feed_mouse(int8_t dx, int8_t dy, uint8_t buttons,
                      uint8_t prev_buttons, int8_t scroll) {
    evdev_ring_t *mouse = &g_evdev_rings[EVDEV_MOUSE_IDX];
    bool any = false;

    /* Relative movement */
    if (dx) { evdev_ring_push(mouse, EV_REL, REL_X, dx); any = true; }
    if (dy) { evdev_ring_push(mouse, EV_REL, REL_Y, dy); any = true; }
    if (scroll) { evdev_ring_push(mouse, EV_REL, REL_WHEEL, -scroll); any = true; }

    /* Button state changes */
    uint8_t btn_changed = buttons ^ prev_buttons;
    if (btn_changed & INPUT_BTN_LEFT) {
        int v = (buttons & INPUT_BTN_LEFT) ? 1 : 0;
        key_state_set(BTN_LEFT, v);
        evdev_ring_push(mouse, EV_KEY, BTN_LEFT, v);
        any = true;
    }
    if (btn_changed & INPUT_BTN_RIGHT) {
        int v = (buttons & INPUT_BTN_RIGHT) ? 1 : 0;
        key_state_set(BTN_RIGHT, v);
        evdev_ring_push(mouse, EV_KEY, BTN_RIGHT, v);
        any = true;
    }
    if (btn_changed & INPUT_BTN_MIDDLE) {
        int v = (buttons & INPUT_BTN_MIDDLE) ? 1 : 0;
        key_state_set(BTN_MIDDLE, v);
        evdev_ring_push(mouse, EV_KEY, BTN_MIDDLE, v);
        any = true;
    }

    if (any) evdev_ring_push(mouse, EV_SYN, SYN_REPORT, 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * file_ops for /dev/input/eventN
 * The private_data field carries the ring index as (void*)(uintptr_t)idx.
 * ══════════════════════════════════════════════════════════════════════════ */

static ssize_t evdev_read(file_t *f, void *buf, size_t count) {
    int idx = (int)(uintptr_t)f->private_data;
    if (idx < 0 || idx >= EVDEV_COUNT) return -EIO;
    evdev_ring_t *r = &g_evdev_rings[idx];

    const size_t esz = sizeof(linux_input_event_t);
    if (count < esz) return -EINVAL;

    linux_input_event_t *dst = (linux_input_event_t *)buf;
    size_t done = 0;

    while (done + esz <= count) {
        uint32_t t = r->tail;
        if (t == r->head) {
            if (done > 0) break;
            if (f->flags & O_NONBLOCK) return -EAGAIN;
            sched_sleep(1);
            continue;
        }
        dst[done / esz] = r->buf[t];
        cpu_mfence();
        r->tail = (t + 1) & (EVDEV_RING_SIZE - 1);
        done += esz;
    }
    return (ssize_t)done;
}

static ssize_t evdev_write(file_t *f, const void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return (ssize_t)count; /* writes to event nodes are ignored */
}

static int evdev_poll(file_t *f, int events) {
    int idx = (int)(uintptr_t)f->private_data;
    evdev_ring_t *r = &g_evdev_rings[idx];
    int ready = 0;
    if ((events & POLLIN) && r->tail != r->head)
        ready |= POLLIN | POLLRDNORM;
    if (events & (POLLOUT | POLLWRNORM))
        ready |= POLLOUT | POLLWRNORM;
    return ready;
}

/* Build the EV_KEY capability bitmask from our translation table */
static void evdev_fill_key_bits(uint8_t *bits, size_t len) {
    if (!bits || len == 0) return;
    memset(bits, 0, len);
    /* Keys from HID table */
    for (int i = 0; i < 0x80; i++) {
        uint16_t lk = g_hid_to_key[i];
        if (lk && lk < (uint16_t)(len * 8)) {
            bits[lk / 8] |= (uint8_t)(1u << (lk % 8));
        }
    }
    /* Modifier synthetic keys */
    for (int b = 0; b < 8; b++) {
        uint16_t mk = g_mod_keycodes[b];
        if (mk < (uint16_t)(len * 8))
            bits[mk / 8] |= (uint8_t)(1u << (mk % 8));
    }
    /* Mouse buttons */
    if (BTN_LEFT    < (uint16_t)(len * 8)) bits[BTN_LEFT    / 8] |= (uint8_t)(1u << (BTN_LEFT    % 8));
    if (BTN_RIGHT   < (uint16_t)(len * 8)) bits[BTN_RIGHT   / 8] |= (uint8_t)(1u << (BTN_RIGHT   % 8));
    if (BTN_MIDDLE  < (uint16_t)(len * 8)) bits[BTN_MIDDLE  / 8] |= (uint8_t)(1u << (BTN_MIDDLE  % 8));
}

static int evdev_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    int idx = (int)(uintptr_t)f->private_data;
    if (idx < 0 || idx >= EVDEV_COUNT) return -EIO;

    /* Fixed-size requests */
    if (cmd == EVIOCGVERSION) {
        if (!arg) return -EINVAL;
        *(uint32_t *)(uintptr_t)arg = 0x010001U;
        return 0;
    }
    if (cmd == EVIOCGID) {
        if (!arg) return -EINVAL;
        evdev_input_id_t *id = (evdev_input_id_t *)(uintptr_t)arg;
        id->bustype = BUS_USB;
        id->vendor  = 0;
        id->product = (uint16_t)idx;
        id->version = 1;
        return 0;
    }

    /* Variable-size requests: strip length field for comparison */
    unsigned long cmd_base = EVIO_CMD_BASE(cmd);
    size_t ioc_len = EVIO_SIZE(cmd);
    void *ubuf = (void *)(uintptr_t)arg;

    if (cmd_base == EVIOCGNAME_BASE) {
        /* Device name string */
        const char *name = (idx == EVDEV_KBD_IDX) ? "EXO Keyboard" : "EXO Mouse";
        size_t nlen = strlen(name);
        if (!ubuf || ioc_len == 0) return -EINVAL;
        size_t copy = (nlen + 1 < ioc_len) ? nlen + 1 : ioc_len;
        memcpy(ubuf, name, copy);
        return (int)(nlen + 1);
    }

    if (cmd_base == EVIOCGKEY_BASE) {
        /* Current key state bitmap */
        if (!ubuf) return -EINVAL;
        if (idx != EVDEV_KBD_IDX) {
            /* For mouse: return zero bitmap (no keys held) */
            memset(ubuf, 0, ioc_len);
            return 0;
        }
        size_t copy = (ioc_len < 64) ? ioc_len : 64;
        memcpy(ubuf, g_evdev_key_state, copy);
        if (ioc_len > 64) memset((uint8_t *)ubuf + 64, 0, ioc_len - 64);
        return 0;
    }

    /* EVIOCGBIT(ev_type, len): event-type bitmask or key-capability bitmask */
    /* The nr byte encodes: 0x20 = EV_SYN bitmask, 0x21 = EV_KEY, 0x22 = EV_REL */
    if (cmd_base == EVIOCGBIT_SYN_BASE) {
        /* Which event types does this device generate? */
        if (!ubuf) return -EINVAL;
        memset(ubuf, 0, ioc_len);
        if (ioc_len >= 1) {
            if (idx == EVDEV_KBD_IDX)
                ((uint8_t *)ubuf)[0] = (1u << EV_SYN) | (1u << EV_KEY);
            else
                ((uint8_t *)ubuf)[0] = (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_REL);
        }
        return 0;
    }

    if (cmd_base == EVIOCGBIT_KEY_BASE) {
        /* Which EV_KEY codes does this device generate? */
        if (!ubuf) return -EINVAL;
        evdev_fill_key_bits((uint8_t *)ubuf, ioc_len);
        return 0;
    }

    if (cmd_base == EVIOCGBIT_REL_BASE) {
        /* EV_REL codes */
        if (!ubuf) return -EINVAL;
        memset(ubuf, 0, ioc_len);
        if (idx == EVDEV_MOUSE_IDX && ioc_len >= 2) {
            ((uint8_t *)ubuf)[0] |= (1u << REL_X) | (1u << REL_Y);
            /* REL_WHEEL = 8: byte 1, bit 0 */
            ((uint8_t *)ubuf)[1] |= (1u << (REL_WHEEL - 8));
        }
        return 0;
    }

    /* EVIOCGABS — not applicable to relative devices */
    return -EINVAL;
}

static int evdev_close(file_t *f) {
    (void)f;
    return 0;
}

/* ── Exported file_ops ───────────────────────────────────────────────────── */
file_ops_t g_evdev_kbd_fops = {
    .read  = evdev_read,
    .write = evdev_write,
    .close = evdev_close,
    .poll  = evdev_poll,
    .ioctl = evdev_ioctl,
};

file_ops_t g_evdev_mouse_fops = {
    .read  = evdev_read,
    .write = evdev_write,
    .close = evdev_close,
    .poll  = evdev_poll,
    .ioctl = evdev_ioctl,
};
