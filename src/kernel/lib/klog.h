#pragma once
#include <stdarg.h>
#include <stdint.h>

/* Initialize serial port for logging (COM1, 115200 baud) */
void klog_init(void);

/* Log levels */
void klog_info (const char *fmt, ...);
void klog_warn (const char *fmt, ...);
void klog_error(const char *fmt, ...);
void klog_debug(const char *fmt, ...);

/* Raw serial write */
void serial_putc(char c);
void serial_puts(const char *s);

#define KLOG_INFO(fmt,  ...) klog_info ("[I] " fmt, ##__VA_ARGS__)
#define KLOG_WARN(fmt,  ...) klog_warn ("[W] " fmt, ##__VA_ARGS__)
#define KLOG_ERR(fmt,   ...) klog_error("[E] " fmt, ##__VA_ARGS__)
#define KLOG_DEBUG(fmt, ...) klog_debug("[D] " fmt, ##__VA_ARGS__)

/* Optional secondary write callback (e.g., framebuffer console).
 * Set to NULL to disable. Called with the formatted string after serial. */
void klog_set_write_fn(void (*fn)(const char *s));

/* ── Ring buffer (dmesg) ─────────────────────────────────────────────────── */
/* Returns a pointer to the internal ring buffer and its size.
 * The caller must not free the pointer.
 * *out_total = total bytes written since boot (may exceed buf_size, meaning
 *              the oldest entries have been overwritten). */
const char *klog_get_ring(uint32_t *out_total, uint32_t *out_buf_size);
