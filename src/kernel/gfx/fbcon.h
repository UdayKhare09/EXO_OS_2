/* gfx/fbcon.h — Limine GOP framebuffer text console
 *
 * A simple, self-contained terminal that renders directly into the Limine
 * linear framebuffer that the bootloader hands us.  No VirtIO GPU, no
 * compositor — just pixels.
 *
 * Features
 *   • Pre-rasterised font via the existing g_font_atlas (gfx/font.h)
 *   • Scrolling, wrap, backspace, \n / \r
 *   • ANSI colour escape sequences (foreground, background, bold, reset)
 *   • Blinking text cursor (toggled by fbcon_tick)
 *   • printf-style fbcon_printf_inst
 *
 * Only one instance is ever created (g_fbcon), initialised early in kmain.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Framebuffer descriptor passed in from Limine ───────────────────────── */
typedef struct {
    uint32_t *fb;       /* HHDM-mapped linear framebuffer                   */
    uint32_t  width;    /* horizontal pixels                                 */
    uint32_t  height;   /* vertical pixels                                   */
    uint32_t  pitch;    /* bytes per row  (may be > width*4 due to padding)  */
} fbcon_fb_t;

/* ── Console instance ───────────────────────────────────────────────────── */
typedef struct fbcon fbcon_t;

/* Initialise the global console from a Limine framebuffer.
 * Must be called once, before any fbcon_puts_inst / fbcon_printf_inst.  */
void fbcon_init(const fbcon_fb_t *fb);

/* Return the global singleton (NULL before fbcon_init) */
fbcon_t *fbcon_get(void);

/* ── Output primitives ──────────────────────────────────────────────────── */
void fbcon_putchar_inst (fbcon_t *c, char ch);
void fbcon_puts_inst    (fbcon_t *c, const char *s);
void fbcon_printf_inst  (fbcon_t *c, const char *fmt, ...);

/* ── Cursor control ─────────────────────────────────────────────────────── */
void fbcon_show_cursor_inst(fbcon_t *c);
void fbcon_hide_cursor_inst(fbcon_t *c);

/* Blink tick — call from a periodic timer / scheduler task at ~2 Hz */
void fbcon_tick(fbcon_t *c);

/* Return physical base address and geometry of the framebuffer.
 * Needed by /dev/fb0 mmap so user-space can map the real framebuffer pixels.
 * phys_base is the physical address of the first pixel; pitch_bytes is the
 * number of bytes per row (may be larger than width*4 due to alignment).    */
void fbcon_get_fb_info(uint64_t *phys_base, uint32_t *width,
                       uint32_t *height, uint32_t *pitch_bytes);

/* Report current text grid size (columns/rows). Returns 0 if unavailable. */
int fbcon_text_cols(void);
int fbcon_text_rows(void);

/* Report current framebuffer pixel size. Returns 0 if unavailable. */
int fbcon_pixel_width(void);
int fbcon_pixel_height(void);
