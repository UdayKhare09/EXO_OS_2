/* gfx/compositor.c — Compositor: CPU-render into back buffer, GPU double-buffer flip.
 *
 * Hardware acceleration is at the presentation layer:
 *   • virtio_gpu_flush() does TRANSFER_TO_HOST_2D then virgl BLIT back→front
 *     (no CPU memcpy, tear-free presentation).
 *   • Software cursor drawn after all windows, before flush.
 */
#include "compositor.h"
#include "wm.h"
#include "cursor.h"
#include "virtio_gpu.h"
#include "lib/string.h"

static gfx_surface_t  *g_screen       = NULL;
static gfx_flush_fn_t  g_flush_fn     = NULL;
static gfx_color_t     g_bg_color     = GFX_DESKTOP_BG;
static gfx_rect_t      g_dirty        = {0, 0, 0, 0};
static gfx_draw_hook_t g_bg_hook      = NULL;
static gfx_draw_hook_t g_overlay_hook = NULL;
static gfx_rect_t      g_bg_rect      = {0, 0, 0, 0}; /* always-dirty region */
static bool            g_force_full   = false;         /* next frame = full   */

void compositor_set_bg_hook(gfx_draw_hook_t fn)      { g_bg_hook      = fn; }
void compositor_set_overlay_hook(gfx_draw_hook_t fn) { g_overlay_hook = fn; }
void compositor_set_bg_rect(gfx_rect_t r)            { g_bg_rect      = r;  }
void compositor_force_full(void)                     { g_force_full   = true; }

void compositor_init(gfx_surface_t *screen, gfx_flush_fn_t flush_fn) {
    g_screen   = screen;
    g_flush_fn = flush_fn;
    g_dirty = (gfx_rect_t){0, 0, screen->w, screen->h};
}

void compositor_set_bg(gfx_color_t c) { g_bg_color = c; }
gfx_surface_t *compositor_screen(void) { return g_screen; }

void compositor_dirty(gfx_rect_t r) {
    g_dirty = gfx_rect_union(g_dirty, r);
}

void compositor_compose(void) {
    if (!g_screen) return;

    gfx_rect_t dr = g_dirty;
    g_dirty = (gfx_rect_t){0, 0, 0, 0};

    /* Always include the persistent bg rect (taskbar strip for clock updates) */
    if (!gfx_rect_empty(g_bg_rect))
        dr = gfx_rect_union(dr, g_bg_rect);

    /* Full repaint requested (start menu open/close, etc.) */
    if (g_force_full) {
        dr = (gfx_rect_t){0, 0, g_screen->w, g_screen->h};
        g_force_full = false;
    }

    if (gfx_rect_empty(dr)) return;

    /* 1. CPU: fill desktop background into back-buffer shadow */
    gfx_fill_rect(g_screen, dr, g_bg_color);

    /* 2. Optional bg hook (taskbar base layer, wallpaper patterns, etc.) */
    if (g_bg_hook) g_bg_hook(g_screen);

    /* 3. CPU: draw all visible windows into back-buffer shadow */
    wm_compose(g_screen, dr);

    /* 4. Optional overlay hook (start menu, notifications, etc.) */
    if (g_overlay_hook) g_overlay_hook(g_screen);

    /* 5. CPU: draw software cursor on top of everything */
    cursor_draw(g_screen);

    /* 7. GPU: TRANSFER shadow → back resource, then virgl BLIT back→front
     *         (or SET_SCANOUT swap when virgl is absent). No tearing. */
    if (g_flush_fn) g_flush_fn(dr);
}