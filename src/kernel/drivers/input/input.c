/* drivers/input/input.c — SPSC event ring implementation */
#include "input.h"
#include "lib/klog.h"
#include "arch/x86_64/cpu.h"   /* cpu_mfence */

input_ring_t g_kbd_ring;
input_ring_t g_mouse_ring;

void input_init(void) {
    g_kbd_ring.head   = 0;
    g_kbd_ring.tail   = 0;
    g_mouse_ring.head = 0;
    g_mouse_ring.tail = 0;
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
    input_event_t ev = {
        .type      = INPUT_EV_KEY,
        .state     = state,
        .keycode   = keycode,
        .modifiers = modifiers,
    };
    if (!ring_push(&g_kbd_ring, &ev))
        KLOG_WARN("input: keyboard ring full, event dropped\n");
}

void input_push_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t scroll) {
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
    [0x28]='\n',[0x29]=0x1B,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
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
    [0x28]='\n',[0x29]=0x1B,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
    [0x2D]='_',[0x2E]='+',[0x2F]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',
    [0x38]='?',
};

/* modifier byte bit 1 = Left Shift, bit 5 = Right Shift */
#define MOD_SHIFT  ((1 << 1) | (1 << 5))

char input_keycode_to_ascii(uint8_t keycode, uint8_t modifiers) {
    if (keycode >= 0x40) return 0;
    if (modifiers & MOD_SHIFT) return keycode_shifted[keycode];
    return keycode_normal[keycode];
}
