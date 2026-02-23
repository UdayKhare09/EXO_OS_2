#include "panic.h"
#include "klog.h"
#include <stdarg.h>

__attribute__((noreturn))
void kpanic_at(const char *file, int line, const char *msg) {
    klog_error("\n\n*** KERNEL PANIC ***\n");
    klog_error("  %s\n", msg);
    klog_error("  at %s:%d\n", file, line);
    klog_error("  System halted.\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

__attribute__((noreturn))
void kpanic(const char *fmt, ...) {
    va_list ap;
    klog_error("\n\n*** KERNEL PANIC ***\n  ");
    va_start(ap, fmt);
    klog_info(fmt, ap);   /* note: this re-delegates to klog which calls vprint */
    va_end(ap);
    klog_error("\n  System halted.\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}
