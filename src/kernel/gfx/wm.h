/* gfx/wm.h — Simple tiling/floating window manager */
#pragma once
#include "gfx/gfx.h"
#include "gfx/font.h"
#include <stdbool.h>

#define WM_TITLE_H  24   /* title bar height in pixels */
#define WM_BORDER   2    /* window border thickness    */
#define WM_MAX_WIN  32   /* max simultaneous windows   */

typedef struct gfx_window {
    gfx_surface_t  *surface;        /* client content pixels (excl. title)   */
    gfx_rect_t      frame;         /* full window rect on screen (incl title)*/
    char            title[64];
    uint32_t        id;
    bool            visible;
    bool            focused;
    struct gfx_window *next;        /* linked list (front-to-back z-order)    */
} gfx_window_t;

/* Initialise window manager */
void wm_init(int screen_w, int screen_h);

/* Create a floating window; returns NULL on OOM.
 * x, y, w, h describe the CONTENT area (title bar extra). */
gfx_window_t *wm_create(const char *title, int x, int y, int w, int h);

/* Destroy a window and free resources */
void           wm_destroy(gfx_window_t *win);

/* Move a window (content origin) */
void           wm_move(gfx_window_t *win, int x, int y);

/* Raise window to top (focused) */
void           wm_raise(gfx_window_t *win);

/* Mark window content dirty → triggers compositor repaint */
void           wm_damage(gfx_window_t *win);

/* Compose all windows into dst surface within the dirty rect.
 * Called by compositor_compose(). */
void           wm_compose(gfx_surface_t *dst, gfx_rect_t dirty);

/* Return content surface of a window */
static inline gfx_surface_t *wm_surface(gfx_window_t *w) { return w->surface; }
