/* gfx/cursor.h — Software mouse cursor (drawn by compositor)
 *
 * The cursor bitmap is alpha-blended onto the back-buffer after all
 * windows have been composited, then flushed together in one
 * TRANSFER_TO_HOST_2D.  This avoids any dependence on virtio-gpu
 * hardware-cursor support (which silently fails in many QEMU configs).
 */
#pragma once
#include "gfx/gfx.h"

/* Build arrow bitmap, centre position on screen.
 * Call after compositor_init(). */
void cursor_init(void);

/* Apply relative mouse delta, clamp to screen bounds, mark dirty. */
void cursor_update_delta(int dx, int dy);

/* Current cursor hotspot position (after clamping) */
int cursor_x(void);
int cursor_y(void);

/* Draw the cursor onto `dst` at its current position (alpha-blended).
 * Called by the compositor *after* wm_compose().                    */
void cursor_draw(gfx_surface_t *dst);

/* Cursor dimensions (for dirty-rect calculation) */
#define CURSOR_W  20
#define CURSOR_H  24
