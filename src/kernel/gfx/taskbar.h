/* gfx/taskbar.h — Bottom taskbar: Start button + window buttons + clock
 *
 * The taskbar is a fixed 36-px strip at the bottom of the screen.
 * It is drawn by the compositor after the desktop background and before
 * all windows, so it always appears as a floor layer.
 *
 * Clicking the Start button raises or lowers the start menu.
 * Clicking a window button raises/restores that window.
 */
#pragma once
#include "gfx/gfx.h"
#include <stdbool.h>

#define TASKBAR_H  36   /* height in pixels */

/* Initialise (must be called once after compositor_init).  */
void taskbar_init(int screen_w, int screen_h);

/* Draw the taskbar onto dst (called by compositor each compose cycle). */
void taskbar_draw(gfx_surface_t *dst);

/* Handle a mouse click at (x, y).
 * Returns true if the event was consumed by the taskbar.
 * start_menu_toggle_out is set true if the Start button was pressed. */
bool taskbar_on_click(int x, int y, bool *start_menu_toggle_out);

/* Expose geometry so desktop can clamp window positions */
static inline int taskbar_height(void) { return TASKBAR_H; }
