/* gfx/gfx.c — Surface management and drawing primitives */
#include "gfx.h"
#include "font.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include <stddef.h>

/* ── Surface ──────────────────────────────────────────────────────────────── */
gfx_surface_t *gfx_surface_create(int w, int h) {
    gfx_surface_t *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->pixels = kmalloc((size_t)w * h * sizeof(gfx_color_t));
    if (!s->pixels) { kfree(s); return NULL; }
    s->w      = w;
    s->h      = h;
    s->stride = w;
    s->owned  = true;
    memset(s->pixels, 0, (size_t)w * h * sizeof(gfx_color_t));
    return s;
}

gfx_surface_t *gfx_surface_wrap(void *pixels, int w, int h, int stride_bytes) {
    gfx_surface_t *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->pixels = (gfx_color_t *)pixels;
    s->w      = w;
    s->h      = h;
    s->stride = stride_bytes / (int)sizeof(gfx_color_t);
    s->owned  = false;
    return s;
}

void gfx_surface_free(gfx_surface_t *s) {
    if (!s) return;
    if (s->owned && s->pixels) kfree(s->pixels);
    kfree(s);
}

/* ── Filled rect ──────────────────────────────────────────────────────────── */
void gfx_fill_rect(gfx_surface_t *s, gfx_rect_t r, gfx_color_t c) {
    gfx_rect_t bounds = {0, 0, s->w, s->h};
    r = gfx_rect_clip(r, bounds);
    if (gfx_rect_empty(r)) return;
    for (int y = r.y; y < r.y + r.h; y++) {
        gfx_color_t *row = s->pixels + y * s->stride + r.x;
        for (int x = 0; x < r.w; x++) row[x] = c;
    }
}

/* ── Border rect ──────────────────────────────────────────────────────────── */
void gfx_draw_rect(gfx_surface_t *s, gfx_rect_t r, gfx_color_t c, int bw) {
    gfx_fill_rect(s, (gfx_rect_t){r.x,         r.y,         r.w,  bw  }, c);
    gfx_fill_rect(s, (gfx_rect_t){r.x,         r.y+r.h-bw,  r.w,  bw  }, c);
    gfx_fill_rect(s, (gfx_rect_t){r.x,         r.y+bw,      bw,   r.h-2*bw}, c);
    gfx_fill_rect(s, (gfx_rect_t){r.x+r.w-bw,  r.y+bw,      bw,   r.h-2*bw}, c);
}

/* ── Blit (no alpha) ──────────────────────────────────────────────────────── */
void gfx_blit(gfx_surface_t *dst, int dx, int dy,
               const gfx_surface_t *src, gfx_rect_t sr) {
    /* clip src rect */
    if (sr.x < 0) { dx -= sr.x; sr.w += sr.x; sr.x = 0; }
    if (sr.y < 0) { dy -= sr.y; sr.h += sr.y; sr.y = 0; }
    if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
    if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
    /* clip dst */
    if (dx < 0) { sr.x -= dx; sr.w += dx; dx = 0; }
    if (dy < 0) { sr.y -= dy; sr.h += dy; dy = 0; }
    if (dx + sr.w > dst->w) sr.w = dst->w - dx;
    if (dy + sr.h > dst->h) sr.h = dst->h - dy;
    if (sr.w <= 0 || sr.h <= 0) return;

    for (int y = 0; y < sr.h; y++) {
        const gfx_color_t *s_row = src->pixels + (sr.y + y) * src->stride + sr.x;
        gfx_color_t       *d_row = dst->pixels + (dy  + y) * dst->stride + dx;
        memcpy(d_row, s_row, (size_t)sr.w * sizeof(gfx_color_t));
    }
}

/* ── Alpha blit ───────────────────────────────────────────────────────────── */
void gfx_blit_alpha(gfx_surface_t *dst, int dx, int dy,
                     const gfx_surface_t *src, gfx_rect_t sr) {
    if (sr.x < 0) { dx -= sr.x; sr.w += sr.x; sr.x = 0; }
    if (sr.y < 0) { dy -= sr.y; sr.h += sr.y; sr.y = 0; }
    if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
    if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
    if (dx < 0) { sr.x -= dx; sr.w += dx; dx = 0; }
    if (dy < 0) { sr.y -= dy; sr.h += dy; dy = 0; }
    if (dx + sr.w > dst->w) sr.w = dst->w - dx;
    if (dy + sr.h > dst->h) sr.h = dst->h - dy;
    if (sr.w <= 0 || sr.h <= 0) return;

    for (int y = 0; y < sr.h; y++) {
        const gfx_color_t *s_row = src->pixels + (sr.y + y) * src->stride + sr.x;
        gfx_color_t       *d_row = dst->pixels + (dy  + y) * dst->stride + dx;
        for (int x = 0; x < sr.w; x++)
            d_row[x] = gfx_blend(d_row[x], s_row[x]);
    }
}

/* ── Glyph drawing (atlas lookup + alpha composite) ──────────────────────── *
 * Optimised path: direct row-pointer access, skip transparent pixels,
 * batch fill of background cell, and branchless alpha blend.                 */
void gfx_draw_glyph(gfx_surface_t *s, int x, int y, int codepoint,
                    gfx_color_t fg, gfx_color_t bg) {
    const gfx_font_atlas_t *a = &g_font_atlas;
    const uint8_t *bmp = font_get_glyph(codepoint);

    int cw = a->cell_w;
    int ch = a->cell_h;

    /* Fill background cell first */
    if (GFX_A(bg) > 0)
        gfx_fill_rect(s, (gfx_rect_t){x, y, cw, ch}, bg);
    if (!bmp) return;

    /* Clip to surface bounds */
    int x0 = x < 0 ? -x : 0;
    int y0 = y < 0 ? -y : 0;
    int x1 = (x + cw > s->w) ? s->w - x : cw;
    int y1 = (y + ch > s->h) ? s->h - y : ch;
    if (x0 >= x1 || y0 >= y1) return;

    /* Pre-extract fg components */
    uint32_t fg_r = GFX_R(fg);
    uint32_t fg_g = GFX_G(fg);
    uint32_t fg_b = GFX_B(fg);

    for (int row = y0; row < y1; row++) {
        gfx_color_t *dst_row = s->pixels + (y + row) * s->stride + x;
        const uint8_t *src_row = bmp + row * cw;
        for (int col = x0; col < x1; col++) {
            uint32_t alpha = src_row[col];
            if (alpha == 0) continue;
            if (alpha == 255) {
                dst_row[col] = fg;
            } else {
                /* Alpha blend: fg_component * alpha + dst * (255-alpha) */
                gfx_color_t d = dst_row[col];
                uint32_t ia = 255 - alpha;
                uint32_t r = (fg_r * alpha + GFX_R(d) * ia + 128) / 255;
                uint32_t g = (fg_g * alpha + GFX_G(d) * ia + 128) / 255;
                uint32_t b = (fg_b * alpha + GFX_B(d) * ia + 128) / 255;
                dst_row[col] = GFX_RGB(r, g, b);
            }
        }
    }
}

int gfx_draw_string(gfx_surface_t *s, int x, int y, const char *str,
                    gfx_color_t fg, gfx_color_t bg) {
    const gfx_font_atlas_t *a = &g_font_atlas;
    int cx = x;
    for (; *str; str++) {
        gfx_draw_glyph(s, cx, y, (unsigned char)*str, fg, bg);
        cx += a->cell_w;
    }
    return cx;
}

/* ── Horizontal gradient ──────────────────────────────────────────────────── */
void gfx_fill_gradient_h(gfx_surface_t *s, gfx_rect_t r,
                          gfx_color_t left, gfx_color_t right) {
    gfx_rect_t bounds = {0, 0, s->w, s->h};
    r = gfx_rect_clip(r, bounds);
    if (gfx_rect_empty(r)) return;
    for (int x = 0; x < r.w; x++) {
        uint32_t t = (uint32_t)(x * 255) / (uint32_t)(r.w > 1 ? r.w - 1 : 1);
        uint32_t it = 255 - t;
        gfx_color_t c = GFX_RGB(
            (GFX_R(left) * it + GFX_R(right) * t) >> 8,
            (GFX_G(left) * it + GFX_G(right) * t) >> 8,
            (GFX_B(left) * it + GFX_B(right) * t) >> 8
        );
        for (int y = r.y; y < r.y + r.h; y++)
            gfx_putpixel(s, r.x + x, y, c);
    }
}

/* ── Rounded rect ─────────────────────────────────────────────────────────── */
void gfx_fill_rounded(gfx_surface_t *s, gfx_rect_t r,
                       gfx_color_t c, int rad) {
    if (rad <= 0) { gfx_fill_rect(s, r, c); return; }
    /* fill center cross */
    gfx_fill_rect(s, (gfx_rect_t){r.x+rad, r.y,     r.w-2*rad, r.h      }, c);
    gfx_fill_rect(s, (gfx_rect_t){r.x,     r.y+rad, r.w,       r.h-2*rad}, c);
    /* four corners using midpoint circle */
    for (int cy = 0; cy < rad; cy++) {
        for (int cx2 = 0; cx2 < rad; cx2++) {
            int dx = rad - 1 - cx2, dy = rad - 1 - cy;
            if (dx*dx + dy*dy <= rad*rad) {
                gfx_putpixel(s, r.x + cx2,           r.y + cy,           c);
                gfx_putpixel(s, r.x + r.w - 1 - cx2, r.y + cy,           c);
                gfx_putpixel(s, r.x + cx2,           r.y + r.h - 1 - cy, c);
                gfx_putpixel(s, r.x + r.w - 1 - cx2, r.y + r.h - 1 - cy, c);
            }
        }
    }
}
