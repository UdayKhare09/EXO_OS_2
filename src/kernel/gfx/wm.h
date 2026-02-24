/* gfx/wm.h — Abstracted floating window manager with drag/resize support
 *
 * Features:
 *   • Z-ordered window stack (front-to-back linked list)
 *   • Title bar with close button
 *   • Mouse-driven move (drag title bar) and resize (drag edges/corners)
 *   • Hit-testing API for desktop event routing
 *   • Per-window keyboard focus
 *   • Callback-based content rendering
 */
#pragma once
#include "gfx/gfx.h"
#include "gfx/font.h"
#include <stdbool.h>

/* ── Layout constants ─────────────────────────────────────────────────────── */
#define WM_TITLE_H   26   /* title bar height in pixels                     */
#define WM_BORDER     2   /* window border thickness                        */
#define WM_RESIZE_GRIP 6  /* resize handle grab zone (pixels from edge)     */
#define WM_MAX_WIN   32   /* max simultaneous windows                       */
#define WM_MIN_W    100   /* minimum window content width                   */
#define WM_MIN_H     60   /* minimum window content height                  */

/* ── Window flags ─────────────────────────────────────────────────────────── */
#define WM_FLAG_MOVABLE    (1 << 0)
#define WM_FLAG_RESIZABLE  (1 << 1)
#define WM_FLAG_CLOSABLE   (1 << 2)
#define WM_FLAG_DEFAULT    (WM_FLAG_MOVABLE | WM_FLAG_RESIZABLE | WM_FLAG_CLOSABLE)
#define WM_FLAG_FIXED      (WM_FLAG_MOVABLE | WM_FLAG_CLOSABLE) /* not resizable */

/* ── Hit-test zones ───────────────────────────────────────────────────────── */
typedef enum {
    WM_HIT_NONE = 0,
    WM_HIT_TITLE,
    WM_HIT_CLOSE,
    WM_HIT_CONTENT,
    WM_HIT_RESIZE_N,
    WM_HIT_RESIZE_S,
    WM_HIT_RESIZE_E,
    WM_HIT_RESIZE_W,
    WM_HIT_RESIZE_NW,
    WM_HIT_RESIZE_NE,
    WM_HIT_RESIZE_SW,
    WM_HIT_RESIZE_SE,
} wm_hit_zone_t;

/* ── Drag state ───────────────────────────────────────────────────────────── */
typedef enum {
    WM_DRAG_NONE = 0,
    WM_DRAG_MOVE,
    WM_DRAG_RESIZE,
} wm_drag_mode_t;

/* ── Window callbacks ─────────────────────────────────────────────────────── */
struct gfx_window;
typedef void (*wm_key_cb_t)(struct gfx_window *win, char ch);
typedef void (*wm_resize_cb_t)(struct gfx_window *win, int new_w, int new_h);
typedef void (*wm_close_cb_t)(struct gfx_window *win);
typedef void (*wm_mouse_cb_t)(struct gfx_window *win, int x, int y, uint8_t buttons);

/* ── Window structure ─────────────────────────────────────────────────────── */
typedef struct gfx_window {
    gfx_surface_t  *surface;        /* client content pixels (excl. decorations) */
    gfx_rect_t      frame;          /* full window rect on screen                */
    char            title[64];
    uint32_t        id;
    uint32_t        flags;          /* WM_FLAG_* bitmask */
    bool            visible;
    bool            focused;
    void           *userdata;       /* opaque pointer for callbacks */

    /* Callbacks */
    wm_key_cb_t     on_key;
    wm_resize_cb_t  on_resize;
    wm_close_cb_t   on_close;
    wm_mouse_cb_t   on_mouse;

    struct gfx_window *next;        /* linked list (front-to-back z-order) */
} gfx_window_t;

/* ── Core API ─────────────────────────────────────────────────────────────── */
void            wm_init(int screen_w, int screen_h);
gfx_window_t   *wm_create(const char *title, int x, int y, int w, int h);
void            wm_destroy(gfx_window_t *win);
void            wm_move(gfx_window_t *win, int x, int y);
void            wm_resize(gfx_window_t *win, int new_w, int new_h);
void            wm_raise(gfx_window_t *win);
void            wm_damage(gfx_window_t *win);

/* ── Hit testing + Mouse interaction ──────────────────────────────────────── */
gfx_window_t   *wm_hit_test(int x, int y, wm_hit_zone_t *zone_out);
void            wm_begin_drag(gfx_window_t *win, wm_drag_mode_t mode,
                              wm_hit_zone_t zone, int anchor_x, int anchor_y);
void            wm_update_drag(int mouse_x, int mouse_y);
void            wm_end_drag(void);
bool            wm_is_dragging(void);

/* ── Focus management ─────────────────────────────────────────────────────── */
gfx_window_t   *wm_get_focused(void);
void            wm_set_focus(gfx_window_t *win);

/* ── Composition ──────────────────────────────────────────────────────────── */
void            wm_compose(gfx_surface_t *dst, gfx_rect_t dirty);
gfx_window_t   *wm_get_window_list(void);

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static inline gfx_rect_t wm_content_rect(const gfx_window_t *w) {
    return (gfx_rect_t){
        w->frame.x + WM_BORDER,
        w->frame.y + WM_BORDER + WM_TITLE_H,
        w->frame.w - 2 * WM_BORDER,
        w->frame.h - WM_TITLE_H - 2 * WM_BORDER
    };
}
static inline gfx_surface_t *wm_surface(gfx_window_t *w) { return w->surface; }
