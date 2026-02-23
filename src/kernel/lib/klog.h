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
