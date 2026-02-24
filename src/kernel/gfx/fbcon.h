/* gfx/fbcon.h — Framebuffer console (VT100 terminal window) */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void fbcon_init(void);            /* create WM window, set as kernel console */
void fbcon_putchar(char c);
void fbcon_puts(const char *s);
void fbcon_printf(const char *fmt, ...);

/* Redirect kernel log to fbcon (call after fbcon_init) */
void fbcon_takeover_klog(void);
