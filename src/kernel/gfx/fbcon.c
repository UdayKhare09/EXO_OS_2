/* gfx/fbcon.c — Instance-based VT100 framebuffer console
 *
 * Each fbcon_t is a self-contained terminal with its own WM window,
 * cursor position, ANSI parser state, and color attributes.
 * Multiple instances can coexist for multi-terminal desktop.
 */
#include "fbcon.h"
#include "wm.h"
#include "compositor.h"
#include "font.h"
#include "gfx.h"
#include "lib/klog.h"
#include "mm/kmalloc.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "lib/string.h"

/* ── Padding inside the window content surface ────────────────────────────── */
#define FBCON_PAD_X   4
#define FBCON_PAD_Y   4

/* ── ANSI colour table (16 colours in ARGB) ─────────────────────────────── */
static const gfx_color_t g_ansi[16] = {
    GFX_RGB(  0,  0,  0),   /* 0  black   */
    GFX_RGB(170,  0,  0),   /* 1  red     */
    GFX_RGB(  0,170,  0),   /* 2  green   */
    GFX_RGB(170,170,  0),   /* 3  yellow  */
    GFX_RGB(  0,  0,170),   /* 4  blue    */
    GFX_RGB(170,  0,170),   /* 5  magenta */
    GFX_RGB(  0,170,170),   /* 6  cyan    */
    GFX_RGB(170,170,170),   /* 7  white   */
    /* bright */
    GFX_RGB( 85, 85, 85),
    GFX_RGB(255, 85, 85),
    GFX_RGB( 85,255, 85),
    GFX_RGB(255,255, 85),
    GFX_RGB( 85, 85,255),
    GFX_RGB(255, 85,255),
    GFX_RGB( 85,255,255),
    GFX_RGB(255,255,255),
};
#define DEFAULT_FG  7
#define DEFAULT_BG  0

/* ── Parser state ────────────────────────────────────────────────────────── */
typedef enum {
    PS_NORMAL,
    PS_ESC,
    PS_CSI,
    PS_CSI_PARAM,
} parse_state_t;

/* ── Console instance ────────────────────────────────────────────────────── */
struct fbcon {
    gfx_window_t *win;
    int           cols, rows;
    int           cw, ch;         /* cell width/height */
    int           cx, cy;         /* cursor col/row    */
    uint8_t       fg, bg;
    bool          bold;
    parse_state_t ps;
    int           csi_params[8];
    int           csi_n;
    int           csi_cur;
};

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static inline gfx_surface_t *surf(fbcon_t *c) {
    return c->win ? c->win->surface : NULL;
}

static void cell_clear(fbcon_t *c, int col, int row) {
    gfx_surface_t *s = surf(c); if (!s) return;
    int px = FBCON_PAD_X + col * c->cw;
    int py = FBCON_PAD_Y + row * c->ch;
    gfx_fill_rect(s, (gfx_rect_t){px, py, c->cw, c->ch}, g_ansi[c->bg]);
}

static void cell_put(fbcon_t *c, int col, int row, char ch) {
    gfx_surface_t *s = surf(c); if (!s) return;
    int px = FBCON_PAD_X + col * c->cw;
    int py = FBCON_PAD_Y + row * c->ch;
    gfx_fill_rect(s, (gfx_rect_t){px, py, c->cw, c->ch}, g_ansi[c->bg]);
    gfx_color_t fg = g_ansi[c->bold ? (c->fg | 8) : c->fg];
    gfx_draw_glyph(s, px, py, (uint8_t)ch, fg, GFX_TRANSPARENT);
}

static void dirty_row(fbcon_t *c, int row) {
    if (!c->win) return;
    gfx_rect_t r = {
        c->win->frame.x + WM_BORDER + FBCON_PAD_X,
        c->win->frame.y + WM_BORDER + WM_TITLE_H + FBCON_PAD_Y + row * c->ch,
        c->cols * c->cw,
        c->ch
    };
    compositor_dirty(r);
}

static void scroll_up(fbcon_t *c) {
    gfx_surface_t *s = surf(c); if (!s) return;
    int row_bytes = c->cols * c->cw;
    int total_h   = c->rows * c->ch;
    for (int py = FBCON_PAD_Y; py < FBCON_PAD_Y + total_h - c->ch; py++) {
        gfx_color_t *dst_row = s->pixels + py            * s->stride + FBCON_PAD_X;
        gfx_color_t *src_row = s->pixels + (py + c->ch) * s->stride + FBCON_PAD_X;
        memcpy(dst_row, src_row, (size_t)(row_bytes * sizeof(gfx_color_t)));
    }
    for (int py = FBCON_PAD_Y + total_h - c->ch; py < FBCON_PAD_Y + total_h; py++) {
        gfx_color_t *row = s->pixels + py * s->stride + FBCON_PAD_X;
        for (int i = 0; i < row_bytes; i++) row[i] = g_ansi[DEFAULT_BG];
    }
    compositor_dirty(c->win->frame);
}

static void newline(fbcon_t *c) {
    c->cx = 0;
    c->cy++;
    if (c->cy >= c->rows) {
        scroll_up(c);
        c->cy = c->rows - 1;
    }
}

static void clear_screen(fbcon_t *c) {
    gfx_surface_t *s = surf(c); if (!s) return;
    gfx_fill_rect(s, (gfx_rect_t){0, 0, s->w, s->h}, g_ansi[DEFAULT_BG]);
    c->cx = c->cy = 0;
    compositor_dirty(c->win->frame);
}

static void clear_eol(fbcon_t *c) {
    for (int col = c->cx; col < c->cols; col++) cell_clear(c, col, c->cy);
    dirty_row(c, c->cy);
}

/* ── CSI dispatch ─────────────────────────────────────────────────────────── */
static void csi_dispatch(fbcon_t *c, char cmd) {
    int p0 = (c->csi_n > 0) ? c->csi_params[0] : 0;
    int p1 = (c->csi_n > 1) ? c->csi_params[1] : 0;
    switch (cmd) {
    case 'A': c->cy -= (p0 ? p0 : 1); if (c->cy < 0) c->cy = 0; break;
    case 'B': c->cy += (p0 ? p0 : 1); if (c->cy >= c->rows) c->cy = c->rows-1; break;
    case 'C': c->cx += (p0 ? p0 : 1); if (c->cx >= c->cols) c->cx = c->cols-1; break;
    case 'D': c->cx -= (p0 ? p0 : 1); if (c->cx < 0) c->cx = 0; break;
    case 'H': case 'f':
        c->cy = (p0 ? p0 - 1 : 0); if (c->cy >= c->rows) c->cy = c->rows-1;
        c->cx = (p1 ? p1 - 1 : 0); if (c->cx >= c->cols) c->cx = c->cols-1;
        break;
    case 'J':
        if (p0 == 2 || p0 == 0) clear_screen(c);
        break;
    case 'K':
        clear_eol(c);
        break;
    case 'm':
        for (int i = 0; i <= c->csi_n; i++) {
            int v = (i < c->csi_n) ? c->csi_params[i] : c->csi_cur;
            if (v == 0)  { c->fg = DEFAULT_FG; c->bg = DEFAULT_BG; c->bold = false; }
            else if (v == 1) c->bold = true;
            else if (v == 22) c->bold = false;
            else if (v >= 30 && v <= 37) c->fg = (uint8_t)(v - 30);
            else if (v == 39)            c->fg = DEFAULT_FG;
            else if (v >= 40 && v <= 47) c->bg = (uint8_t)(v - 40);
            else if (v == 49)            c->bg = DEFAULT_BG;
            else if (v >= 90 && v <= 97) c->fg = (uint8_t)(v - 90 + 8);
            else if (v >= 100&&v <=107)  c->bg = (uint8_t)(v - 100 + 8);
        }
        break;
    default: break;
    }
}

/* ── Public: create instance ──────────────────────────────────────────────── */
fbcon_t *fbcon_create(const char *title, int x, int y, int w, int h) {
    const gfx_font_atlas_t *a = &g_font_atlas;
    fbcon_t *c = kzalloc(sizeof(*c));
    if (!c) return NULL;

    c->cw = a->cell_w;
    c->ch = a->cell_h;

    if (w <= 0 || h <= 0) {
        gfx_surface_t *screen = compositor_screen();
        int sw = screen ? screen->w : 1280;
        int sh = screen ? screen->h : 720;
        w = sw * 3 / 5;
        h = sh * 2 / 3;
    }

    /* Calculate cols/rows from content area minus padding */
    c->cols = (w - 2 * FBCON_PAD_X) / c->cw;
    c->rows = (h - 2 * FBCON_PAD_Y) / c->ch;
    if (c->cols < 20) c->cols = 20;
    if (c->rows < 5) c->rows = 5;

    int win_w = c->cols * c->cw + 2 * FBCON_PAD_X;
    int win_h = c->rows * c->ch + 2 * FBCON_PAD_Y;

    c->win = wm_create(title, x, y, win_w, win_h);
    if (!c->win) {
        kfree(c);
        return NULL;
    }
    c->win->userdata = c;
    gfx_fill_rect(c->win->surface,
                  (gfx_rect_t){0, 0, c->win->surface->w, c->win->surface->h},
                  g_ansi[DEFAULT_BG]);

    c->fg   = DEFAULT_FG;
    c->bg   = DEFAULT_BG;
    c->bold = false;
    c->ps   = PS_NORMAL;

    KLOG_INFO("fbcon: '%s' %dx%d cols/rows, cell %dx%d\n",
              title, c->cols, c->rows, c->cw, c->ch);
    return c;
}

void fbcon_destroy(fbcon_t *c) {
    if (!c) return;
    if (c->win) wm_destroy(c->win);
    kfree(c);
}

struct gfx_window *fbcon_get_window(fbcon_t *c) { return c ? c->win : NULL; }
int fbcon_get_cols(fbcon_t *c) { return c ? c->cols : 0; }
int fbcon_get_rows(fbcon_t *c) { return c ? c->rows : 0; }

/* ── Resize callback ──────────────────────────────────────────────────────── */
void fbcon_on_resize(fbcon_t *c, int new_w, int new_h) {
    if (!c) return;
    c->cols = (new_w - 2 * FBCON_PAD_X) / c->cw;
    c->rows = (new_h - 2 * FBCON_PAD_Y) / c->ch;
    if (c->cols < 10) c->cols = 10;
    if (c->rows < 3)  c->rows = 3;
    if (c->cx >= c->cols) c->cx = c->cols - 1;
    if (c->cy >= c->rows) c->cy = c->rows - 1;
}

/* ── Character output ─────────────────────────────────────────────────────── */
void fbcon_putchar_inst(fbcon_t *c, char ch) {
    if (!c || !c->win) return;

    switch (c->ps) {
    case PS_NORMAL:
        if (ch == 0x1B) { c->ps = PS_ESC; return; }
        if (ch == '\r') { c->cx = 0; return; }
        if (ch == '\n') { newline(c); dirty_row(c, c->cy); compositor_compose(); return; }
        if (ch == '\t') {
            int next = (c->cx + 8) & ~7;
            while (c->cx < next && c->cx < c->cols) { cell_put(c, c->cx, c->cy, ' '); c->cx++; }
            dirty_row(c, c->cy); return;
        }
        if (ch == '\b') {
            if (c->cx > 0) { c->cx--; cell_clear(c, c->cx, c->cy); dirty_row(c, c->cy); }
            return;
        }
        if ((uint8_t)ch < 0x20) return;
        cell_put(c, c->cx, c->cy, ch);
        dirty_row(c, c->cy);
        c->cx++;
        if (c->cx >= c->cols) newline(c);
        compositor_compose();
        return;

    case PS_ESC:
        if (ch == '[') { c->ps = PS_CSI; c->csi_n = 0; c->csi_cur = 0; return; }
        c->ps = PS_NORMAL;
        return;

    case PS_CSI:
    case PS_CSI_PARAM:
        if (ch >= '0' && ch <= '9') {
            c->csi_cur = c->csi_cur * 10 + (ch - '0');
            c->ps = PS_CSI_PARAM;
            return;
        }
        if (ch == ';') {
            if (c->csi_n < 8) c->csi_params[c->csi_n++] = c->csi_cur;
            c->csi_cur = 0;
            return;
        }
        if (c->csi_n < 8) c->csi_params[c->csi_n] = c->csi_cur;
        csi_dispatch(c, ch);
        c->ps = PS_NORMAL;
        compositor_compose();
        return;
    }
}

void fbcon_puts_inst(fbcon_t *c, const char *s) {
    while (*s) fbcon_putchar_inst(c, *s++);
}

/* Minimal vsnprintf for fbcon_printf (no FPU, no libc) */
static int fbcon_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t i = 0;
#define FPUT(ch) do { if (i < cap - 1) buf[i++] = (ch); } while(0)
    while (*fmt) {
        if (*fmt != '%') { FPUT(*fmt++); continue; }
        fmt++;
        /* skip length modifiers */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
        switch (*fmt++) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s="(null)"; while (*s) FPUT(*s++); break; }
        case 'c': FPUT((char)va_arg(ap, int)); break;
        case 'd': {
            int64_t v = va_arg(ap, int64_t);
            char tmp[24]; int n = 0;
            if (v < 0) { FPUT('-'); v = -v; }
            if (v == 0) { FPUT('0'); break; }
            while (v) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
            while (n > 0) FPUT(tmp[--n]); break; }
        case 'u': {
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[24]; int n = 0;
            if (v == 0) { FPUT('0'); break; }
            while (v) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
            while (n > 0) FPUT(tmp[--n]); break; }
        case 'x': {
            uint64_t v = va_arg(ap, uint64_t);
            char tmp[18]; int n = 0;
            const char *h = "0123456789abcdef";
            if (v == 0) { FPUT('0'); break; }
            while (v) { tmp[n++] = h[v & 0xF]; v >>= 4; }
            while (n > 0) FPUT(tmp[--n]); break; }
        case '%': FPUT('%'); break;
        default: FPUT('?'); break;
        }
    }
    buf[i] = '\0';
#undef FPUT
    return (int)i;
}

static char g_fmtbuf[1024];
void fbcon_printf_inst(fbcon_t *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fbcon_vsnprintf(g_fmtbuf, sizeof(g_fmtbuf), fmt, ap);
    va_end(ap);
    fbcon_puts_inst(c, g_fmtbuf);
}

/* ── Text cursor ──────────────────────────────────────────────────────────── */
void fbcon_show_cursor_inst(fbcon_t *c) {
    gfx_surface_t *s = surf(c);
    if (!s || !c->win) return;
    int px = FBCON_PAD_X + c->cx * c->cw;
    int py = FBCON_PAD_Y + c->cy * c->ch + c->ch - 3;
    gfx_fill_rect(s, (gfx_rect_t){px, py, c->cw, 2}, GFX_WHITE);
    dirty_row(c, c->cy);
    compositor_compose();
}

void fbcon_hide_cursor_inst(fbcon_t *c) {
    cell_clear(c, c->cx, c->cy);
    dirty_row(c, c->cy);
}

/* ── Legacy single-instance wrapper ───────────────────────────────────────── */
static fbcon_t *g_legacy = NULL;

void fbcon_init(void) {
    gfx_surface_t *screen = compositor_screen();
    int sw = screen ? screen->w : 1280;
    int sh = screen ? screen->h : 720;
    int w  = sw * 3 / 5;
    int h  = sh * 2 / 3;
    g_legacy = fbcon_create("Terminal", 10, 10, w, h);
    if (g_legacy)
        compositor_compose();
}

void fbcon_putchar(char c)                    { fbcon_putchar_inst(g_legacy, c); }
void fbcon_puts(const char *s)                { fbcon_puts_inst(g_legacy, s); }
void fbcon_printf(const char *fmt, ...)       {
    va_list ap;
    va_start(ap, fmt);
    fbcon_vsnprintf(g_fmtbuf, sizeof(g_fmtbuf), fmt, ap);
    va_end(ap);
    fbcon_puts_inst(g_legacy, g_fmtbuf);
}
void fbcon_show_cursor(void)                  { fbcon_show_cursor_inst(g_legacy); }
void fbcon_hide_cursor(void)                  { fbcon_hide_cursor_inst(g_legacy); }

static void fbcon_klog_write(const char *msg) { fbcon_puts_inst(g_legacy, msg); }
void fbcon_takeover_klog(void)                { klog_set_write_fn(fbcon_klog_write); }
