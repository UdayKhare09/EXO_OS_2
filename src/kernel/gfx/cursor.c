/* gfx/cursor.c — Software mouse cursor
 *
 * A 20×24 NW-pointing arrow stored as an alpha bitmap and
 * alpha-blended onto the compositor back-buffer each frame.
 *
 * The bitmap uses three layers:
 *   1. Black outline       (alpha 0xFF)
 *   2. White fill          (alpha 0xFF)
 *   3. Anti-alias fringe   (alpha ~0x60) along the outline edges
 */
#include "cursor.h"
#include "gfx.h"
#include "compositor.h"

/* ── Arrow shape (20 wide × 24 tall) ─────────────────────────────────────── *
 * '#' = black outline      '.' = white fill
 * '+' = AA fringe (semi-transparent outline)
 * ' ' = fully transparent                                                    */
static const char *const g_arrow[CURSOR_H] = {
    "#+                  ",
    "#.#+                ",
    "#..#+               ",
    "#...#+              ",
    "#....#+             ",
    "#.....#+            ",
    "#......#+           ",
    "#.......#+          ",
    "#........#+         ",
    "#.........#+        ",
    "#..........#+       ",
    "#...........#+      ",
    "#............#+     ",
    "#.............#+    ",
    "#..............#+   ",
    "#.......########+   ",
    "#.....#.#+          ",
    "#....#+#.#+         ",
    "#...#+ +#.#+        ",
    "#..#+   +#.#+       ",
    "#+#      +#.#+      ",
    " +        +#.#+     ",
    "           +##+     ",
    "            ++      ",
};

/* Current on-screen position */
static int g_cx, g_cy;

static gfx_color_t pixel_for(char ch) {
    switch (ch) {
    case '#': return GFX_ARGB(0xFF, 0x00, 0x00, 0x00);   /* black outline */
    case '.': return GFX_ARGB(0xFF, 0xFF, 0xFF, 0xFF);   /* white fill    */
    case '+': return GFX_ARGB(0x60, 0x00, 0x00, 0x00);   /* AA fringe     */
    default:  return 0x00000000u;                          /* transparent   */
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */
void cursor_init(void) {
    gfx_surface_t *scr = compositor_screen();
    g_cx = scr ? scr->w / 2 : 640;
    g_cy = scr ? scr->h / 2 : 360;
}

void cursor_update_delta(int dx, int dy) {
    gfx_surface_t *scr = compositor_screen();
    int sw = scr ? scr->w : 1280;
    int sh = scr ? scr->h : 720;

    /* Mark old cursor rect dirty */
    compositor_dirty((gfx_rect_t){ g_cx, g_cy, CURSOR_W, CURSOR_H });

    g_cx += dx;
    g_cy += dy;
    if (g_cx < 0)   g_cx = 0;
    if (g_cy < 0)   g_cy = 0;
    if (g_cx >= sw)  g_cx = sw - 1;
    if (g_cy >= sh)  g_cy = sh - 1;

    /* Mark new cursor rect dirty */
    compositor_dirty((gfx_rect_t){ g_cx, g_cy, CURSOR_W, CURSOR_H });
}

int cursor_x(void) { return g_cx; }
int cursor_y(void) { return g_cy; }

void cursor_draw(gfx_surface_t *dst) {
    if (!dst) return;
    for (int row = 0; row < CURSOR_H; row++) {
        int dy = g_cy + row;
        if (dy < 0 || dy >= dst->h) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int dx = g_cx + col;
            if (dx < 0 || dx >= dst->w) continue;
            gfx_color_t src = pixel_for(g_arrow[row][col]);
            if (GFX_A(src) == 0) continue;
            gfx_color_t bg = dst->pixels[dy * dst->stride + dx];
            dst->pixels[dy * dst->stride + dx] = gfx_blend(bg, src);
        }
    }
}
