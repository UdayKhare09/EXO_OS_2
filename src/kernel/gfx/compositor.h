/* gfx/compositor.h — Dirty-rect Software Compositor */
#pragma once
#include "gfx/gfx.h"

/* Flush callback type (virtio_gpu_flush or fb_backend_flush) */
typedef void (*gfx_flush_fn_t)(gfx_rect_t dirty);

/* Initialise compositor with the screen surface and a flush function.
 * screen must remain valid for the lifetime of the compositor.     */
void compositor_init(gfx_surface_t *screen, gfx_flush_fn_t flush_fn);

/* Mark a screen-space rectangle as dirty; compositor will redraw + flush it. */
void compositor_dirty(gfx_rect_t r);

/* Composite all dirty areas now (draws desktop, then all windows in z-order) */
void compositor_compose(void);

/* Set desktop background colour */
void compositor_set_bg(gfx_color_t c);

/* Return the screen surface */
gfx_surface_t *compositor_screen(void);

/* ── Optional draw hooks ──────────────────────────────────────────────────── */
/* Hook called AFTER desktop background, BEFORE windows (e.g. taskbar base).  */
typedef void (*gfx_draw_hook_t)(gfx_surface_t *dst);
void compositor_set_bg_hook(gfx_draw_hook_t fn);

/* Hook called AFTER windows, BEFORE cursor (e.g. start menu overlay).        */
void compositor_set_overlay_hook(gfx_draw_hook_t fn);

/* Register a rect that is always unioned into the dirty region each compose.
 * Use for areas that change independently of windows (e.g. the taskbar clock). */
void compositor_set_bg_rect(gfx_rect_t r);

/* Force the next compositor_compose() call to repaint the entire screen.
 * Call whenever a full-screen overlay appears or disappears (start menu).   */
void compositor_force_full(void);
