/* drivers/input/input.c — SPSC event ring implementation */
#include "input.h"
#include "evdev.h"           /* evdev_feed_key, evdev_feed_mouse */
#include "lib/klog.h"
#include "arch/x86_64/cpu.h"   /* cpu_mfence */
#include "ipc/signal.h"

/* Defined in syscall/net_syscalls.c */
extern void tty_signal_foreground(int sig);

input_ring_t g_kbd_ring;
input_ring_t g_mouse_ring;

/* ── Toggle-key state ────────────────────────────────────────────────────── */
static bool g_capslock = false;  /* toggled on HID 0x39 press               */
static bool g_numlock  = false;  /* toggled on HID 0x53 press               */

#define TTY_CHAR_RING_SIZE 1024u
static char g_tty_chars[TTY_CHAR_RING_SIZE];
static volatile uint32_t g_tty_head;
static volatile uint32_t g_tty_tail;

static inline void tty_char_push(char ch) {
    uint32_t h = g_tty_head;
    uint32_t next = (h + 1) & (TTY_CHAR_RING_SIZE - 1);
    if (next == g_tty_tail) return;
    g_tty_chars[h] = ch;
    cpu_mfence();
    g_tty_head = next;
}

static inline void tty_seq_push(const char *s) {
    if (!s) return;
    while (*s) tty_char_push(*s++);
}

/* modifier byte bits: ctrl={0,4}, shift={1,5}, alt={2,6} */
#define MOD_CTRL   ((1 << 0) | (1 << 4))
#define MOD_SHIFT  ((1 << 1) | (1 << 5))
#define MOD_ALT    ((1 << 2) | (1 << 6))

static bool input_keycode_to_esc_seq(uint8_t keycode, const char **seq) {
    if (!seq) return false;
    switch (keycode) {
        /* ── Arrow keys ───────────────────────────────────────────────── */
        case 0x4F: *seq = "\x1b[C"; return true; /* Right arrow */
        case 0x50: *seq = "\x1b[D"; return true; /* Left arrow  */
        case 0x51: *seq = "\x1b[B"; return true; /* Down arrow  */
        case 0x52: *seq = "\x1b[A"; return true; /* Up arrow    */
        /* ── Navigation cluster ───────────────────────────────────────── */
        case 0x4A: *seq = "\x1b[H"; return true;  /* Home        */
        case 0x4D: *seq = "\x1b[F"; return true;  /* End         */
        case 0x4C: *seq = "\x1b[3~"; return true; /* Delete      */
        case 0x49: *seq = "\x1b[2~"; return true; /* Insert      */
        case 0x4B: *seq = "\x1b[5~"; return true; /* Page Up     */
        case 0x4E: *seq = "\x1b[6~"; return true; /* Page Down   */
        /* ── Function keys (xterm SS3 / CSI ~ sequences) ──────────────── */
        case 0x3A: *seq = "\x1bOP";    return true; /* F1  */
        case 0x3B: *seq = "\x1bOQ";    return true; /* F2  */
        case 0x3C: *seq = "\x1bOR";    return true; /* F3  */
        case 0x3D: *seq = "\x1bOS";    return true; /* F4  */
        case 0x3E: *seq = "\x1b[15~"; return true; /* F5  */
        case 0x3F: *seq = "\x1b[17~"; return true; /* F6  */
        case 0x40: *seq = "\x1b[18~"; return true; /* F7  */
        case 0x41: *seq = "\x1b[19~"; return true; /* F8  */
        case 0x42: *seq = "\x1b[20~"; return true; /* F9  */
        case 0x43: *seq = "\x1b[21~"; return true; /* F10 */
        case 0x44: *seq = "\x1b[23~"; return true; /* F11 */
        case 0x45: *seq = "\x1b[24~"; return true; /* F12 */
        /* ── Numpad when numlock is OFF → navigation sequences ─────────── */
        case 0x59: if (!g_numlock) { *seq = "\x1b[F"; return true; } break; /* KP1=End   */
        case 0x5A: if (!g_numlock) { *seq = "\x1b[B"; return true; } break; /* KP2=Down  */
        case 0x5B: if (!g_numlock) { *seq = "\x1b[6~"; return true; } break; /* KP3=PgDn */
        case 0x5C: if (!g_numlock) { *seq = "\x1b[D"; return true; } break; /* KP4=Left  */
        case 0x5E: if (!g_numlock) { *seq = "\x1b[C"; return true; } break; /* KP6=Right */
        case 0x5F: if (!g_numlock) { *seq = "\x1b[H"; return true; } break; /* KP7=Home  */
        case 0x60: if (!g_numlock) { *seq = "\x1b[A"; return true; } break; /* KP8=Up    */
        case 0x61: if (!g_numlock) { *seq = "\x1b[5~"; return true; } break; /* KP9=PgUp */
        case 0x62: if (!g_numlock) { *seq = "\x1b[2~"; return true; } break; /* KP0=Ins  */
        case 0x63: if (!g_numlock) { *seq = "\x1b[3~"; return true; } break; /* KP.=Del  */
        default: break;
    }
    return false;
}

void input_init(void) {
    g_kbd_ring.head   = 0;
    g_kbd_ring.tail   = 0;
    g_mouse_ring.head = 0;
    g_mouse_ring.tail = 0;
    g_tty_head = 0;
    g_tty_tail = 0;
    KLOG_INFO("input: event rings initialised (%u slots each)\n",
              INPUT_RING_SIZE);
}

/* ── Internal push / pop (generic) ──────────────────────────────────────── */
static inline bool ring_push(input_ring_t *r, const input_event_t *ev) {
    uint32_t h = r->head;
    uint32_t next = (h + 1) & (INPUT_RING_SIZE - 1);
    if (next == r->tail) return false;   /* full — drop */
    r->buf[h] = *ev;
    cpu_mfence();                        /* store event before advancing head */
    r->head = next;
    return true;
}

static inline bool ring_pop(input_ring_t *r, input_event_t *out) {
    uint32_t t = r->tail;
    if (t == r->head) return false;     /* empty */
    *out = r->buf[t];
    cpu_mfence();
    r->tail = (t + 1) & (INPUT_RING_SIZE - 1);
    return true;
}

void input_push_key(uint8_t modifiers, uint8_t keycode, uint8_t state) {
    /* ── Feed Linux evdev ring before any further processing ─────────────── */
    evdev_feed_key(keycode, state, modifiers);

    input_event_t ev = {
        .type      = INPUT_EV_KEY,
        .state     = state,
        .keycode   = keycode,
        .modifiers = modifiers,
    };
    if (!ring_push(&g_kbd_ring, &ev)) {
        input_event_t dropped;
        (void)ring_pop(&g_kbd_ring, &dropped);
        (void)ring_push(&g_kbd_ring, &ev);
    }

    if (state == INPUT_KEY_PRESS) {
        /* ── Toggle-key state updates ──────────────────────────────────── */
        if (keycode == 0x39) { g_capslock = !g_capslock; return; }
        if (keycode == 0x53) { g_numlock  = !g_numlock;  return; }
        if (modifiers & MOD_CTRL) {
            /* Ctrl+A..Ctrl+Z => 0x01..0x1A */
            if (keycode >= 0x04 && keycode <= 0x1D) {
                char ctrl = (char)(1 + (keycode - 0x04));
                if (ctrl == 0x03) {
                    /* Ctrl-C: interrupt foreground process group */
                    tty_signal_foreground(SIGINT);
                } else {
                    tty_char_push(ctrl);
                }
                return;
            }
        }

        const char *esc = 0;
        if (input_keycode_to_esc_seq(keycode, &esc)) {
            tty_seq_push(esc);
            return;
        }

        char ch = input_keycode_to_ascii(keycode, modifiers);
        if (ch) {
            if (modifiers & MOD_ALT) tty_char_push('\x1b');
            tty_char_push(ch);
        }
    }
}

/* Saved previous mouse button mask (for delta computation in evdev). */
static uint8_t g_prev_mouse_buttons = 0;

void input_push_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t scroll) {
    /* ── Feed Linux evdev ring first ─────────────────────────────────────── */
    evdev_feed_mouse(dx, dy, buttons, g_prev_mouse_buttons, scroll);
    g_prev_mouse_buttons = buttons;

    input_event_t ev = {
        .type         = INPUT_EV_MOUSE,
        .mouse_buttons= buttons,
        .mouse_dx     = dx,
        .mouse_dy     = dy,
        .mouse_scroll = scroll,
    };
    if (!ring_push(&g_mouse_ring, &ev))
        KLOG_WARN("input: mouse ring full, event dropped\n");
}

bool input_poll(input_event_t *out) {
    if (ring_pop(&g_kbd_ring, out))   return true;
    if (ring_pop(&g_mouse_ring, out)) return true;
    return false;
}

/* ── HID Boot-protocol keycode → ASCII ──────────────────────────────────── */
/* Table index = USB HID keycode, value = ASCII (unshifted).
 * Only covers 0x04 (a) … 0x38 (/) — the rest return 0.                    */
static const char keycode_normal[0x40] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',
    [0x09]='f',[0x0A]='g',[0x0B]='h',[0x0C]='i',[0x0D]='j',
    [0x0E]='k',[0x0F]='l',[0x10]='m',[0x11]='n',[0x12]='o',
    [0x13]='p',[0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',
    [0x18]='u',[0x19]='v',[0x1A]='w',[0x1B]='x',[0x1C]='y',
    [0x1D]='z',
    [0x1E]='1',[0x1F]='2',[0x20]='3',[0x21]='4',[0x22]='5',
    [0x23]='6',[0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',
    [0x28]='\r',[0x29]=0x1B,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
    [0x2D]='-',[0x2E]='=',[0x2F]='[',[0x30]=']',[0x31]='\\',
    [0x33]=';',[0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',
    [0x38]='/',
};

static const char keycode_shifted[0x40] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',
    [0x09]='F',[0x0A]='G',[0x0B]='H',[0x0C]='I',[0x0D]='J',
    [0x0E]='K',[0x0F]='L',[0x10]='M',[0x11]='N',[0x12]='O',
    [0x13]='P',[0x14]='Q',[0x15]='R',[0x16]='S',[0x17]='T',
    [0x18]='U',[0x19]='V',[0x1A]='W',[0x1B]='X',[0x1C]='Y',
    [0x1D]='Z',
    [0x1E]='!',[0x1F]='@',[0x20]='#',[0x21]='$',[0x22]='%',
    [0x23]='^',[0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
    [0x28]='\r',[0x29]=0x1B,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
    [0x2D]='_',[0x2E]='+',[0x2F]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',
    [0x38]='?',
};

/* Numpad digit table (numlock ON): index = HID code - 0x59 */
static const char numpad_digits[12] = {
    /* 0x59=KP1 … 0x61=KP9 */ '1','2','3','4','5','6','7','8','9',
    /* 0x62=KP0 */             '0',
    /* 0x63=KP. */             '.',
    /* 0x5D=KP5, gaps: handle KP5 specially below */
};

char input_keycode_to_ascii(uint8_t keycode, uint8_t modifiers) {
    /* ── Numpad digits (numlock ON) ──────────────────────────────────────── */
    if (g_numlock) {
        if (keycode >= 0x59 && keycode <= 0x63) {
            /* KP5 (0x5D) is index 4 in the 0x59-based table → '5' */
            return numpad_digits[keycode - 0x59];
        }
        if (keycode == 0x58) return '\r'; /* KP Enter */
        if (keycode == 0x54) return '/';  /* KP / */
        if (keycode == 0x55) return '*';  /* KP * */
        if (keycode == 0x56) return '-';  /* KP - */
        if (keycode == 0x57) return '+';  /* KP + */
    }
    /* ── Regular keys (table only covers 0x04-0x3F) ──────────────────────── */
    if (keycode >= 0x40) return 0;
    bool shifted = (modifiers & MOD_SHIFT) != 0;
    /* Capslock inverts case only for letter keys (0x04-0x1D). */
    if (g_capslock && keycode >= 0x04 && keycode <= 0x1D)
        shifted = !shifted;
    if (shifted) return keycode_shifted[keycode];
    return keycode_normal[keycode];
}

bool input_tty_char_available(void) {
    return g_tty_tail != g_tty_head;
}

/* Return the number of characters waiting in the TTY char ring.
 * Used by FIONREAD / TIOCINQ ioctl. */
int input_tty_char_count(void) {
    uint32_t h = g_tty_head;
    uint32_t t = g_tty_tail;
    if (h >= t) return (int)(h - t);
    return (int)(TTY_CHAR_RING_SIZE - t + h);
}

int input_tty_getchar_nonblock(char *out_ch) {
    if (!out_ch) return 0;
    uint32_t t = g_tty_tail;
    if (t == g_tty_head) return -1;
    *out_ch = g_tty_chars[t];
    cpu_mfence();
    g_tty_tail = (t + 1) & (TTY_CHAR_RING_SIZE - 1);
    return 0;
}

/* input_tty_flush: discard all bytes in the TTY character ring (TCFLSH). */
void input_tty_flush(void) {
    cpu_mfence();
    g_tty_tail = g_tty_head;
}
