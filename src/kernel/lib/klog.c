#include "klog.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

/* COM1 base port */
#define SERIAL_PORT  0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void klog_init(void) {
    outb(SERIAL_PORT + 1, 0x00); /* Disable all interrupts     */
    outb(SERIAL_PORT + 3, 0x80); /* Enable DLAB                */
    outb(SERIAL_PORT + 0, 0x01); /* Divisor low  (115200 baud) */
    outb(SERIAL_PORT + 1, 0x00); /* Divisor high               */
    outb(SERIAL_PORT + 3, 0x03); /* 8 bits, no parity, one stop*/
    outb(SERIAL_PORT + 2, 0xC7); /* Enable FIFO, clear, 14-byte*/
    outb(SERIAL_PORT + 4, 0x0B); /* RTS/DSR set                */
}

void serial_putc(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20));
    if (c == '\n') {
        outb(SERIAL_PORT, '\r');
        while (!(inb(SERIAL_PORT + 5) & 0x20));
    }
    outb(SERIAL_PORT, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

/* kvsnprintf is provided by lib/string.c (declared in lib/string.h) */

/* Spinlock: serialize output so lines from different CPUs don't interleave */
static volatile int klog_lock = 0;
static inline void klog_acquire(void) {
    while (__atomic_test_and_set(&klog_lock, __ATOMIC_ACQUIRE));
}
static inline void klog_release(void) {
    __atomic_clear(&klog_lock, __ATOMIC_RELEASE);
}

/* Secondary write callback (e.g. framebuffer console) */
static void (*g_write_fn)(const char *) = NULL;
void klog_set_write_fn(void (*fn)(const char *s)) { g_write_fn = fn; }

/* ── Ring buffer for dmesg ───────────────────────────────────────────────── */
#define KLOG_RING_SIZE  (16 * 1024)   /* 16 KiB ring buffer */
static char     g_ring[KLOG_RING_SIZE];
static uint32_t g_ring_write = 0;     /* total bytes written (monotonic) */

static void ring_write(const char *s) {
    while (*s) {
        g_ring[g_ring_write % KLOG_RING_SIZE] = *s++;
        g_ring_write++;
    }
}

const char *klog_get_ring(uint32_t *out_total, uint32_t *out_buf_size) {
    if (out_total)    *out_total    = g_ring_write;
    if (out_buf_size) *out_buf_size = KLOG_RING_SIZE;
    return g_ring;
}

static void klog_vprint(const char *fmt, va_list ap) {
    char buf[512];
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    klog_acquire();
    serial_puts(buf);
    ring_write(buf);
    if (g_write_fn) g_write_fn(buf);
    klog_release();
}

void klog_info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); klog_vprint(fmt, ap); va_end(ap);
}
void klog_warn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); klog_vprint(fmt, ap); va_end(ap);
}
void klog_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); klog_vprint(fmt, ap); va_end(ap);
}
void klog_debug(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); klog_vprint(fmt, ap); va_end(ap);
}
