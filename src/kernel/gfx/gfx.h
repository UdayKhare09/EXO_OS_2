/* gfx/gfx.h — Core graphics abstraction (surfaces, colors, drawing) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Color (ARGB8888 packed) ──────────────────────────────────────────────── */
typedef uint32_t gfx_color_t;

#define GFX_ARGB(a,r,g,b) \
    (((gfx_color_t)(a) << 24) | ((gfx_color_t)(r) << 16) | \
     ((gfx_color_t)(g) <<  8) |  (gfx_color_t)(b))
#define GFX_RGB(r,g,b)   GFX_ARGB(0xFF, r, g, b)
#define GFX_A(c)   (((c) >> 24) & 0xFF)
#define GFX_R(c)   (((c) >> 16) & 0xFF)
#define GFX_G(c)   (((c) >>  8) & 0xFF)
#define GFX_B(c)   (((c)      ) & 0xFF)

/* Common colours */
#define GFX_BLACK       GFX_RGB(0x0C, 0x0C, 0x0C)
#define GFX_WHITE       GFX_RGB(0xFF, 0xFF, 0xFF)
#define GFX_GREY        GFX_RGB(0x40, 0x40, 0x40)
#define GFX_LIGHT_GREY  GFX_RGB(0xC0, 0xC0, 0xC0)
#define GFX_ACCENT      GFX_RGB(0x00, 0x7A, 0xFF)
#define GFX_TITLE_BG    GFX_RGB(0x1E, 0x1E, 0x2E)
#define GFX_TITLE_FG    GFX_WHITE
#define GFX_DESKTOP_BG  GFX_RGB(0x18, 0x18, 0x2A)
#define GFX_TERM_BG     GFX_RGB(0x0D, 0x0D, 0x1A)
#define GFX_TERM_FG     GFX_RGB(0xCC, 0xCC, 0xCC)
#define GFX_TRANSPARENT GFX_ARGB(0,0,0,0)

/* ── Rectangle ────────────────────────────────────────────────────────────── */
typedef struct { int x, y, w, h; } gfx_rect_t;

static inline bool gfx_rect_empty(gfx_rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline gfx_rect_t gfx_rect_union(gfx_rect_t a, gfx_rect_t b) {
    if (gfx_rect_empty(a)) return b;
    if (gfx_rect_empty(b)) return a;
    int x1 = a.x < b.x ? a.x : b.x;
    int y1 = a.y < b.y ? a.y : b.y;
    int x2 = (a.x+a.w) > (b.x+b.w) ? (a.x+a.w) : (b.x+b.w);
    int y2 = (a.y+a.h) > (b.y+b.h) ? (a.y+a.h) : (b.y+b.h);
    return (gfx_rect_t){ x1, y1, x2-x1, y2-y1 };
}
static inline gfx_rect_t gfx_rect_clip(gfx_rect_t r, gfx_rect_t bounds) {
    int x1 = r.x < bounds.x ? bounds.x : r.x;
    int y1 = r.y < bounds.y ? bounds.y : r.y;
    int x2 = (r.x+r.w) < (bounds.x+bounds.w) ? (r.x+r.w) : (bounds.x+bounds.w);
    int y2 = (r.y+r.h) < (bounds.y+bounds.h) ? (r.y+r.h) : (bounds.y+bounds.h);
    if (x2 <= x1 || y2 <= y1) return (gfx_rect_t){0,0,0,0};
    return (gfx_rect_t){ x1, y1, x2-x1, y2-y1 };
}

/* ── Surface (pixel buffer) ───────────────────────────────────────────────── */
typedef struct {
    gfx_color_t *pixels;   /* ARGB8888 pixel array              */
    int          w;        /* width in pixels                   */
    int          h;        /* height in pixels                  */
    int          stride;   /* pixels per row (may be > w)       */
    bool         owned;    /* if true, pixels was kmalloc-ed    */
} gfx_surface_t;

/* Allocate a new ARGB surface */
gfx_surface_t *gfx_surface_create(int w, int h);

/* Wrap an existing pixel buffer (e.g., the Limine framebuffer) */
gfx_surface_t *gfx_surface_wrap(void *pixels, int w, int h, int stride_bytes);

void           gfx_surface_free(gfx_surface_t *s);

/* ── Drawing primitives ───────────────────────────────────────────────────── */
static inline void gfx_putpixel(gfx_surface_t *s, int x, int y, gfx_color_t c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h)
        s->pixels[y * s->stride + x] = c;
}
static inline gfx_color_t gfx_getpixel(const gfx_surface_t *s, int x, int y) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h)
        return s->pixels[y * s->stride + x];
    return 0;
}

/* Alpha-blend src onto dst (src over dst) */
static inline gfx_color_t gfx_blend(gfx_color_t dst, gfx_color_t src) {
    uint32_t a = GFX_A(src);
    if (a == 0xFF) return src;
    if (a == 0x00) return dst;
    uint32_t ia = 255 - a;
    uint32_t r = (GFX_R(src) * a + GFX_R(dst) * ia) >> 8;
    uint32_t g = (GFX_G(src) * a + GFX_G(dst) * ia) >> 8;
    uint32_t b = (GFX_B(src) * a + GFX_B(dst) * ia) >> 8;
    return GFX_RGB(r, g, b);
}

void gfx_fill_rect(gfx_surface_t *s, gfx_rect_t r, gfx_color_t c);
void gfx_draw_rect(gfx_surface_t *s, gfx_rect_t r, gfx_color_t c, int border);
void gfx_blit(gfx_surface_t *dst, int dx, int dy,
               const gfx_surface_t *src, gfx_rect_t sr);
void gfx_blit_alpha(gfx_surface_t *dst, int dx, int dy,
                     const gfx_surface_t *src, gfx_rect_t sr);

/* Draw a single glyph using pre-rasterized atlas.
 * (fg_a = per-glyph alpha weight from atlas, 0-255) */
void gfx_draw_glyph(gfx_surface_t *s, int x, int y, int codepoint,
                    gfx_color_t fg, gfx_color_t bg);

/* Draw a null-terminated string; returns new x position */
int  gfx_draw_string(gfx_surface_t *s, int x, int y, const char *str,
                     gfx_color_t fg, gfx_color_t bg);

/* Horizontal / vertical gradient fill */
void gfx_fill_gradient_h(gfx_surface_t *s, gfx_rect_t r,
                          gfx_color_t left, gfx_color_t right);

/* Round-cornered rect (r = corner radius) */
void gfx_fill_rounded(gfx_surface_t *s, gfx_rect_t rect,
                       gfx_color_t c, int radius);
