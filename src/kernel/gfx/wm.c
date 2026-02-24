/* gfx/wm.c — Abstracted window manager with drag/resize/hit-test */
#include "wm.h"
#include "compositor.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"

static gfx_window_t *g_head = NULL;  /* front of z-order (top window first) */
static uint32_t      g_next_id = 1;
static int           g_sw, g_sh;

/* Shadow extends 3px right+down; dirty rects must include it. */
#define WM_SHADOW 3
static inline void dirty_expanded(gfx_rect_t fr) {
    compositor_dirty((gfx_rect_t){
        fr.x - 1, fr.y - 1,
        fr.w + WM_SHADOW + 2, fr.h + WM_SHADOW + 2
    });
}

/* ── Drag state (global — one drag at a time) ────────────────────────────── */
static struct {
    gfx_window_t  *win;
    wm_drag_mode_t mode;
    wm_hit_zone_t  zone;       /* which resize edge/corner */
    int            anchor_x, anchor_y;   /* mouse pos at drag start */
    gfx_rect_t     orig_frame;           /* window frame at drag start */
} g_drag;

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

/* ── Init ─────────────────────────────────────────────────────────────────── */
void wm_init(int screen_w, int screen_h) {
    g_sw = screen_w;  g_sh = screen_h;
    g_drag.mode = WM_DRAG_NONE;
    KLOG_INFO("wm: init %dx%d\n", screen_w, screen_h);
}

/* ── Create ───────────────────────────────────────────────────────────────── */
gfx_window_t *wm_create(const char *title, int x, int y, int w, int h) {
    gfx_window_t *win = kzalloc(sizeof(*win));
    if (!win) return NULL;
    win->surface = gfx_surface_create(w, h);
    if (!win->surface) { kfree(win); return NULL; }
    gfx_fill_rect(win->surface, (gfx_rect_t){0,0,w,h}, GFX_TERM_BG);

    strncpy(win->title, title, sizeof(win->title) - 1);
    win->frame   = (gfx_rect_t){ x, y, w + 2*WM_BORDER,
                                       h + WM_TITLE_H + 2*WM_BORDER };
    win->id      = g_next_id++;
    win->flags   = WM_FLAG_DEFAULT;
    win->visible = true;
    win->focused = false;
    win->userdata = NULL;
    win->on_key   = NULL;
    win->on_resize = NULL;
    win->on_close  = NULL;
    win->on_mouse  = NULL;
    list_push_front(win);
    KLOG_INFO("wm: created window '%s' id=%u  %dx%d at (%d,%d)\n",
              title, win->id, w, h, x, y);
    dirty_expanded(win->frame);
    return win;
}

/* ── Destroy ──────────────────────────────────────────────────────────────── */
void wm_destroy(gfx_window_t *win) {
    if (!win) return;
    if (g_drag.win == win) g_drag.mode = WM_DRAG_NONE;
    dirty_expanded(win->frame);
    list_remove(win);
    gfx_surface_free(win->surface);
    kfree(win);
}

/* ── Move ─────────────────────────────────────────────────────────────────── */
void wm_move(gfx_window_t *win, int x, int y) {
    dirty_expanded(win->frame);
    win->frame.x = x;
    win->frame.y = y;
    dirty_expanded(win->frame);
}

/* ── Resize ───────────────────────────────────────────────────────────────── */
void wm_resize(gfx_window_t *win, int new_w, int new_h) {
    if (new_w < WM_MIN_W) new_w = WM_MIN_W;
    if (new_h < WM_MIN_H) new_h = WM_MIN_H;
    if (new_w == win->surface->w && new_h == win->surface->h) return;

    dirty_expanded(win->frame);

    gfx_surface_t *old = win->surface;
    gfx_surface_t *ns  = gfx_surface_create(new_w, new_h);
    if (!ns) return;
    gfx_fill_rect(ns, (gfx_rect_t){0,0,new_w,new_h}, GFX_TERM_BG);

    /* Copy existing content (clipped to new dimensions) */
    int cw = old->w < new_w ? old->w : new_w;
    int ch = old->h < new_h ? old->h : new_h;
    gfx_blit(ns, 0, 0, old, (gfx_rect_t){0, 0, cw, ch});

    win->surface = ns;
    win->frame.w = new_w + 2 * WM_BORDER;
    win->frame.h = new_h + WM_TITLE_H + 2 * WM_BORDER;
    gfx_surface_free(old);

    dirty_expanded(win->frame);

    if (win->on_resize)
        win->on_resize(win, new_w, new_h);
}

/* ── Raise / Focus ────────────────────────────────────────────────────────── */
void wm_raise(gfx_window_t *win) {
    /* Unfocus all */
    for (gfx_window_t *w = g_head; w; w = w->next)
        w->focused = false;
    list_remove(win);
    list_push_front(win);
    win->focused = true;
    dirty_expanded(win->frame);
}

void wm_set_focus(gfx_window_t *win) {
    for (gfx_window_t *w = g_head; w; w = w->next)
        w->focused = false;
    if (win) win->focused = true;
}

gfx_window_t *wm_get_focused(void) {
    for (gfx_window_t *w = g_head; w; w = w->next)
        if (w->focused) return w;
    return g_head; /* fallback: top window */
}

void wm_damage(gfx_window_t *win) {
    if (win) dirty_expanded(win->frame);
}

/* ── Hit testing ──────────────────────────────────────────────────────────── */
gfx_window_t *wm_hit_test(int x, int y, wm_hit_zone_t *zone_out) {
    for (gfx_window_t *w = g_head; w; w = w->next) {
        if (!w->visible) continue;
        gfx_rect_t fr = w->frame;

        /* Check if point is inside the frame (with resize grip margin) */
        int gx = (w->flags & WM_FLAG_RESIZABLE) ? WM_RESIZE_GRIP : 0;
        gfx_rect_t expanded = { fr.x - gx, fr.y - gx,
                                fr.w + 2*gx, fr.h + 2*gx };
        if (x < expanded.x || x >= expanded.x + expanded.w ||
            y < expanded.y || y >= expanded.y + expanded.h)
            continue;

        /* Check resize zones (edges and corners) */
        if (w->flags & WM_FLAG_RESIZABLE) {
            bool near_left   = (x < fr.x + WM_RESIZE_GRIP);
            bool near_right  = (x >= fr.x + fr.w - WM_RESIZE_GRIP);
            bool near_top    = (y < fr.y + WM_RESIZE_GRIP);
            bool near_bottom = (y >= fr.y + fr.h - WM_RESIZE_GRIP);

            if (near_top && near_left)     { *zone_out = WM_HIT_RESIZE_NW; return w; }
            if (near_top && near_right)    { *zone_out = WM_HIT_RESIZE_NE; return w; }
            if (near_bottom && near_left)  { *zone_out = WM_HIT_RESIZE_SW; return w; }
            if (near_bottom && near_right) { *zone_out = WM_HIT_RESIZE_SE; return w; }
            if (near_left)                 { *zone_out = WM_HIT_RESIZE_W;  return w; }
            if (near_right)                { *zone_out = WM_HIT_RESIZE_E;  return w; }
            if (near_top)                  { *zone_out = WM_HIT_RESIZE_N;  return w; }
            if (near_bottom)               { *zone_out = WM_HIT_RESIZE_S;  return w; }
        }

        /* Must be inside actual frame for title/content/close */
        if (x < fr.x || x >= fr.x + fr.w || y < fr.y || y >= fr.y + fr.h)
            continue;

        /* Close button: 14×14 circle at top-right of title */
        int bx = fr.x + fr.w - WM_BORDER - 16;
        int by = fr.y + WM_BORDER + (WM_TITLE_H - 14) / 2;
        if (x >= bx && x < bx + 14 && y >= by && y < by + 14) {
            *zone_out = WM_HIT_CLOSE;
            return w;
        }

        /* Title bar */
        if (y < fr.y + WM_BORDER + WM_TITLE_H) {
            *zone_out = WM_HIT_TITLE;
            return w;
        }

        /* Content area */
        *zone_out = WM_HIT_CONTENT;
        return w;
    }
    *zone_out = WM_HIT_NONE;
    return NULL;
}

/* ── Drag operations ──────────────────────────────────────────────────────── */
void wm_begin_drag(gfx_window_t *win, wm_drag_mode_t mode,
                   wm_hit_zone_t zone, int ax, int ay) {
    g_drag.win       = win;
    g_drag.mode      = mode;
    g_drag.zone      = zone;
    g_drag.anchor_x  = ax;
    g_drag.anchor_y  = ay;
    g_drag.orig_frame = win->frame;
}

void wm_update_drag(int mx, int my) {
    if (g_drag.mode == WM_DRAG_NONE || !g_drag.win) return;

    int dx = mx - g_drag.anchor_x;
    int dy = my - g_drag.anchor_y;
    gfx_rect_t of = g_drag.orig_frame;

    if (g_drag.mode == WM_DRAG_MOVE) {
        wm_move(g_drag.win, of.x + dx, of.y + dy);
        compositor_compose();
        return;
    }

    /* Resize: compute new frame based on which edge/corner */
    int new_x = of.x, new_y = of.y, new_w = of.w, new_h = of.h;

    switch (g_drag.zone) {
    case WM_HIT_RESIZE_E:  new_w = of.w + dx; break;
    case WM_HIT_RESIZE_S:  new_h = of.h + dy; break;
    case WM_HIT_RESIZE_SE: new_w = of.w + dx; new_h = of.h + dy; break;
    case WM_HIT_RESIZE_W:  new_x = of.x + dx; new_w = of.w - dx; break;
    case WM_HIT_RESIZE_N:  new_y = of.y + dy; new_h = of.h - dy; break;
    case WM_HIT_RESIZE_NW: new_x = of.x + dx; new_y = of.y + dy;
                            new_w = of.w - dx; new_h = of.h - dy; break;
    case WM_HIT_RESIZE_NE: new_y = of.y + dy;
                            new_w = of.w + dx; new_h = of.h - dy; break;
    case WM_HIT_RESIZE_SW: new_x = of.x + dx;
                            new_w = of.w - dx; new_h = of.h + dy; break;
    default: return;
    }

    /* Enforce minimum size (content dimensions) */
    int min_frame_w = WM_MIN_W + 2 * WM_BORDER;
    int min_frame_h = WM_MIN_H + WM_TITLE_H + 2 * WM_BORDER;
    if (new_w < min_frame_w) { new_w = min_frame_w; new_x = of.x + of.w - min_frame_w; }
    if (new_h < min_frame_h) { new_h = min_frame_h; new_y = of.y + of.h - min_frame_h; }

    int content_w = new_w - 2 * WM_BORDER;
    int content_h = new_h - WM_TITLE_H - 2 * WM_BORDER;

    dirty_expanded(g_drag.win->frame);
    g_drag.win->frame.x = new_x;
    g_drag.win->frame.y = new_y;
    wm_resize(g_drag.win, content_w, content_h);
    compositor_compose();
}

void wm_end_drag(void) {
    g_drag.mode = WM_DRAG_NONE;
    g_drag.win  = NULL;
}

bool wm_is_dragging(void) {
    return g_drag.mode != WM_DRAG_NONE;
}

/* ── Window list ──────────────────────────────────────────────────────────── */
gfx_window_t *wm_get_window_list(void) { return g_head; }

/* ── Draw a single window onto dst ────────────────────────────────────────── */
static void draw_window(gfx_surface_t *dst, gfx_window_t *win) {
    if (!win->visible) return;
    gfx_rect_t fr = win->frame;

    /* Drop shadow (translucent black offset 3px) */
    gfx_rect_t shadow = { fr.x + 3, fr.y + 3, fr.w, fr.h };
    gfx_color_t shadow_col = GFX_ARGB(0x50, 0, 0, 0);
    for (int sy = shadow.y; sy < shadow.y + shadow.h && sy < dst->h; sy++) {
        if (sy < 0) continue;
        for (int sx = shadow.x; sx < shadow.x + shadow.w && sx < dst->w; sx++) {
            if (sx < 0) continue;
            gfx_color_t old = gfx_getpixel(dst, sx, sy);
            gfx_putpixel(dst, sx, sy, gfx_blend(old, shadow_col));
        }
    }

    /* Window border (rounded) */
    gfx_color_t border_col = win->focused ? GFX_RGB(0x50,0x50,0x60)
                                          : GFX_RGB(0x35,0x35,0x40);
    gfx_fill_rounded(dst, fr, border_col, 5);

    /* Title bar background */
    gfx_rect_t tb = { fr.x + WM_BORDER, fr.y + WM_BORDER,
                       fr.w - 2*WM_BORDER, WM_TITLE_H };
    gfx_color_t tb_left  = win->focused ? GFX_RGB(0x20,0x60,0xD0)
                                        : GFX_RGB(0x3A,0x3A,0x48);
    gfx_color_t tb_right = win->focused ? GFX_RGB(0x10,0x40,0x90)
                                        : GFX_RGB(0x30,0x30,0x3C);
    gfx_fill_gradient_h(dst, tb, tb_left, tb_right);

    /* Title text */
    const gfx_font_atlas_t *a = &g_font_atlas;
    int tx = tb.x + 10;
    int ty = tb.y + (WM_TITLE_H - a->cell_h) / 2;
    gfx_draw_string(dst, tx, ty, win->title,
                    GFX_RGB(0xEE,0xEE,0xF0), GFX_TRANSPARENT);

    /* Close button (red circle, right side of title bar) */
    if (win->flags & WM_FLAG_CLOSABLE) {
        int bx = fr.x + fr.w - WM_BORDER - 18;
        int by = fr.y + WM_BORDER + (WM_TITLE_H - 14) / 2;
        gfx_fill_rounded(dst, (gfx_rect_t){bx, by, 14, 14},
                          GFX_RGB(0xFF, 0x5F, 0x57), 7);
    }

    /* Content area blit */
    int cx = fr.x + WM_BORDER;
    int cy = fr.y + WM_BORDER + WM_TITLE_H;
    gfx_blit(dst, cx, cy, win->surface,
             (gfx_rect_t){0, 0, win->surface->w, win->surface->h});
}

/* ── Compose all windows ──────────────────────────────────────────────────── */
void wm_compose(gfx_surface_t *dst, gfx_rect_t dirty) {
    /* Collect into stack array, render back-to-front */
    gfx_window_t *stack[WM_MAX_WIN];
    int n = 0;
    for (gfx_window_t *w = g_head; w && n < WM_MAX_WIN; w = w->next)
        stack[n++] = w;
    (void)dirty;
    for (int i = n - 1; i >= 0; i--)
        draw_window(dst, stack[i]);
}
