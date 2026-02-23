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

/* ---- minimal vsnprintf ---- */
static void print_uint(uint64_t v, int base, int pad, char *buf, size_t *idx, size_t cap) {
    const char *digits = "0123456789abcdef";
    char tmp[64];
    int len = 0;
    if (v == 0) { tmp[len++] = '0'; }
    while (v) { tmp[len++] = digits[v % base]; v /= base; }
    /* zero-pad */
    while (len < pad) tmp[len++] = '0';
    /* reverse into buf */
    for (int i = len - 1; i >= 0; i--) {
        if (*idx < cap - 1) buf[(*idx)++] = tmp[i];
    }
}

static int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t i = 0;
#define PUT(c) do { if (i < cap - 1) buf[i++] = (c); } while(0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;
        int pad = 0;
        while (*fmt >= '0' && *fmt <= '9') pad = pad*10 + (*fmt++ - '0');
        /* Skip length modifiers (l, ll, h, hh, z) — we always pull 64-bit */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
        switch (*fmt++) {
            case 'c': PUT((char)va_arg(ap, int)); break;
            case 's': { const char *s = va_arg(ap, const char*);
                        if (!s) s = "(null)";
                        while (*s) PUT(*s++); break; }
            case 'd': { int64_t v = va_arg(ap, int64_t);
                        if (v < 0) { PUT('-'); v = -v; }
                        print_uint((uint64_t)v, 10, pad, buf, &i, cap); break; }
            case 'u': print_uint(va_arg(ap, uint64_t), 10, pad, buf, &i, cap); break;
            case 'x': print_uint(va_arg(ap, uint64_t), 16, pad, buf, &i, cap); break;
            case 'p': { PUT('0'); PUT('x');
                        print_uint((uint64_t)va_arg(ap, void*), 16, 16, buf, &i, cap); break; }
            case '%': PUT('%'); break;
            default: PUT('?'); break;
        }
    }
    buf[i] = '\0';
#undef PUT
    return (int)i;
}

static void klog_vprint(const char *fmt, va_list ap) {
    char buf[512];
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    serial_puts(buf);
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
