/* gfx/compositor.c — Compositor: CPU-render into back buffer, GPU double-buffer flip.
 *
 * Hardware acceleration is at the presentation layer:
 *   • virtio_gpu_flush() does TRANSFER_TO_HOST_2D then virgl BLIT back→front
 *     (no CPU memcpy, tear-free presentation).
 *   • Hardware cursor via virtio_gpu_cursor_set/move().
 *   • virtio_gpu_virgl_fill/blit() are available for future per-window HW compositing.
 */
#include "compositor.h"
#include "wm.h"
#include "virtio_gpu.h"
#include "lib/string.h"

static gfx_surface_t  *g_screen   = NULL;
static gfx_flush_fn_t  g_flush_fn = NULL;
static gfx_color_t     g_bg_color = GFX_DESKTOP_BG;
static gfx_rect_t      g_dirty    = {0, 0, 0, 0};

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
    if (!g_screen || gfx_rect_empty(g_dirty)) return;

    gfx_rect_t dr = g_dirty;
    g_dirty = (gfx_rect_t){0, 0, 0, 0};

    /* 1. CPU: fill desktop background into back-buffer shadow */
    gfx_fill_rect(g_screen, dr, g_bg_color);

    /* 2. CPU: draw all visible windows into back-buffer shadow */
    wm_compose(g_screen, dr);

    /* 3. GPU: TRANSFER shadow → back resource, then virgl BLIT back→front
     *         (or SET_SCANOUT swap when virgl is absent). No tearing. */
    if (g_flush_fn) g_flush_fn(dr);
}