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
