/* gfx/wm.c — Window manager */
#include "wm.h"
#include "compositor.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"

static gfx_window_t *g_head = NULL;  /* front of z-order (top window first) */
static uint32_t      g_next_id = 1;
static int           g_sw, g_sh;

/* ── Linked list helpers ──────────────────────────────────────────────────── */
static void list_push_front(gfx_window_t *w) {
    w->next = g_head;
    g_head  = w;
}
static void list_remove(gfx_window_t *w) {
    gfx_window_t **pp = &g_head;
    while (*pp) {
        if (*pp == w) { *pp = w->next; w->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

void wm_init(int screen_w, int screen_h) {
    g_sw = screen_w;  g_sh = screen_h;
    KLOG_INFO("wm: init %dx%d\n", screen_w, screen_h);
}

gfx_window_t *wm_create(const char *title, int x, int y, int w, int h) {
    gfx_window_t *win = kzalloc(sizeof(*win));
    if (!win) return NULL;
    win->surface = gfx_surface_create(w, h);
    if (!win->surface) { kfree(win); return NULL; }
    /* Fill content with a neutral dark colour by default */
    gfx_fill_rect(win->surface, (gfx_rect_t){0,0,w,h}, GFX_TERM_BG);

    strncpy(win->title, title, sizeof(win->title) - 1);
    win->frame   = (gfx_rect_t){ x, y, w + 2*WM_BORDER,
                                      h + WM_TITLE_H + WM_BORDER };
    win->id      = g_next_id++;
    win->visible = true;
    win->focused = false;
    list_push_front(win);
    KLOG_INFO("wm: created window '%s' id=%u  %dx%d at (%d,%d)\n",
              title, win->id, w, h, x, y);
    compositor_dirty(win->frame);
    return win;
}

void wm_destroy(gfx_window_t *win) {
    if (!win) return;
    compositor_dirty(win->frame);
    list_remove(win);
    gfx_surface_free(win->surface);
    kfree(win);
}

void wm_move(gfx_window_t *win, int x, int y) {
    compositor_dirty(win->frame);  /* dirty old position */
    win->frame.x = x;
    win->frame.y = y;
    compositor_dirty(win->frame);  /* dirty new position */
}

void wm_raise(gfx_window_t *win) {
    list_remove(win);
    list_push_front(win);
    win->focused = true;
    compositor_dirty(win->frame);
}

void wm_damage(gfx_window_t *win) {
    if (win) compositor_dirty(win->frame);
}

/* ── Draw a single window's title bar + shadow onto dst ──────────────────── */
static void draw_window(gfx_surface_t *dst, gfx_window_t *win) {
    if (!win->visible) return;
    gfx_rect_t fr = win->frame;

    /* Drop shadow (translucent black offset 2px) */
    gfx_rect_t shadow = { fr.x + 3, fr.y + 3, fr.w, fr.h };
    gfx_color_t shadow_col = GFX_ARGB(0x60, 0, 0, 0);
    /* simple: draw dark rect */
    for (int sy = shadow.y; sy < shadow.y + shadow.h; sy++) {
        for (int sx = shadow.x; sx < shadow.x + shadow.w; sx++) {
            gfx_color_t old = gfx_getpixel(dst, sx, sy);
            gfx_putpixel(dst, sx, sy, gfx_blend(old, shadow_col));
        }
    }

    /* Window border */
    gfx_fill_rounded(dst, fr, GFX_GREY, 4);

    /* Title bar gradient */
    gfx_rect_t tb = { fr.x + WM_BORDER, fr.y + WM_BORDER,
                       fr.w - 2*WM_BORDER, WM_TITLE_H };
    gfx_color_t tb_left  = win->focused ? GFX_ACCENT        : GFX_GREY;
    gfx_color_t tb_right = win->focused ? GFX_RGB(0,0x5A,0xC0): GFX_LIGHT_GREY;
    gfx_fill_gradient_h(dst, tb, tb_left, tb_right);

    /* Title text */
    const gfx_font_atlas_t *a = &g_font_atlas;
    int tx = tb.x + 8;
    int ty = tb.y + (WM_TITLE_H - a->cell_h) / 2;
    gfx_draw_string(dst, tx, ty, win->title, GFX_WHITE, GFX_TRANSPARENT);

    /* Window close button (red circle) */
    int bx = fr.x + fr.w - WM_BORDER - 16;
    int by = fr.y + WM_BORDER + (WM_TITLE_H - 14) / 2;
    gfx_fill_rounded(dst, (gfx_rect_t){bx, by, 14, 14},
                      GFX_RGB(0xFF, 0x5F, 0x57), 7);

    /* Content area */
    int cx = fr.x + WM_BORDER;
    int cy = fr.y + WM_BORDER + WM_TITLE_H;
    gfx_blit(dst, cx, cy, win->surface,
             (gfx_rect_t){0, 0, win->surface->w, win->surface->h});
}

void wm_compose(gfx_surface_t *dst, gfx_rect_t dirty) {
    /* Iterate back-to-front (reverse linked list = bottom window first) */
    /* Collect pointers into a small stack array, then render in reverse */
    gfx_window_t *stack[WM_MAX_WIN];
    int n = 0;
    for (gfx_window_t *w = g_head; w && n < WM_MAX_WIN; w = w->next)
        stack[n++] = w;
    (void)dirty;  /* future: skip windows that don't intersect */
    for (int i = n - 1; i >= 0; i--)
        draw_window(dst, stack[i]);
}
