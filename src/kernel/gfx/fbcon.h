/* gfx/fbcon.h — Instance-based framebuffer console (VT100 terminal window)
 *
 * Each fbcon_t owns its own WM window, cursor state, and ANSI parser.
 * Multiple terminals can coexist simultaneously.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Opaque console instance */
typedef struct fbcon fbcon_t;

/* Create a new terminal window at (x,y) with given content width/height.
 * Pass 0,0 for w,h to auto-size based on screen. */
fbcon_t *fbcon_create(const char *title, int x, int y, int w, int h);

/* Destroy a terminal instance */
void fbcon_destroy(fbcon_t *con);

/* Character / string output */
void fbcon_putchar_inst(fbcon_t *con, char c);
void fbcon_puts_inst(fbcon_t *con, const char *s);
void fbcon_printf_inst(fbcon_t *con, const char *fmt, ...);

/* Text cursor (2-px underline) */
void fbcon_show_cursor_inst(fbcon_t *con);
void fbcon_hide_cursor_inst(fbcon_t *con);

/* Get the underlying WM window */
struct gfx_window;
struct gfx_window *fbcon_get_window(fbcon_t *con);

/* Get columns/rows */
int fbcon_get_cols(fbcon_t *con);
int fbcon_get_rows(fbcon_t *con);

/* Resize callback — called by WM when window is resized */
void fbcon_on_resize(fbcon_t *con, int new_w, int new_h);

/* ── Legacy single-instance API (used by main.c init) ────────────────────── */
void fbcon_init(void);
void fbcon_putchar(char c);
void fbcon_puts(const char *s);
void fbcon_printf(const char *fmt, ...);
void fbcon_show_cursor(void);
void fbcon_hide_cursor(void);
void fbcon_takeover_klog(void);
