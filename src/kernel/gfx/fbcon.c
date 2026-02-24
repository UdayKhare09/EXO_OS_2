/* gfx/fbcon.c — VT100 framebuffer console on a WM window */
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

/* ── Dimensions ──────────────────────────────────────────────────────────── */
#define FBCON_WIN_X   8
#define FBCON_WIN_Y   8
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
    PS_ESC,          /* got ESC */
    PS_CSI,          /* got ESC[ */
    PS_CSI_PARAM,    /* accumulating parameter digits */
} parse_state_t;

/* ── Console state ───────────────────────────────────────────────────────── */
static gfx_window_t *g_win   = NULL;
static int           g_cols  = 0;
static int           g_rows  = 0;
static int           g_cw    = 0;   /* cell width  */
static int           g_ch    = 0;   /* cell height */
static int           g_cx    = 0;   /* cursor col  */
static int           g_cy    = 0;   /* cursor row  */
static uint8_t       g_fg    = DEFAULT_FG;
static uint8_t       g_bg    = DEFAULT_BG;
static bool          g_bold  = false;
static parse_state_t g_ps    = PS_NORMAL;
static int           g_csi_params[8];
static int           g_csi_n  = 0;
static int           g_csi_cur = 0; /* digits for current param */

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static inline gfx_surface_t *surf(void) { return g_win ? g_win->surface : NULL; }

static void cell_clear(int col, int row) {
    gfx_surface_t *s = surf(); if (!s) return;
    int px = FBCON_PAD_X + col * g_cw;
    int py = FBCON_PAD_Y + row * g_ch;
    gfx_fill_rect(s, (gfx_rect_t){px, py, g_cw, g_ch}, g_ansi[g_bg]);
}

static void cell_put(int col, int row, char c) {
    gfx_surface_t *s = surf(); if (!s) return;
    int px = FBCON_PAD_X + col * g_cw;
    int py = FBCON_PAD_Y + row * g_ch;
    gfx_fill_rect(s, (gfx_rect_t){px, py, g_cw, g_ch}, g_ansi[g_bg]);
    gfx_color_t fg = g_ansi[g_bold ? (g_fg | 8) : g_fg];
    gfx_draw_glyph(s, px, py, (uint8_t)c, fg, GFX_TRANSPARENT);
}

static void dirty_row(int row) {
    /* Mark the corresponding rect on screen dirty via compositor */
    if (!g_win) return;
    gfx_rect_t r = {
        g_win->frame.x + WM_BORDER + FBCON_PAD_X,
        g_win->frame.y + WM_BORDER + WM_TITLE_H + FBCON_PAD_Y + row * g_ch,
        g_cols * g_cw,
        g_ch
    };
    compositor_dirty(r);
}

static void scroll_up(void) {
    gfx_surface_t *s = surf(); if (!s) return;
    int row_bytes = g_cols * g_cw;
    int total_h   = g_rows * g_ch;
    /* Shift pixel rows up by g_ch */
    for (int py = FBCON_PAD_Y; py < FBCON_PAD_Y + total_h - g_ch; py++) {
        gfx_color_t *dst_row = s->pixels + py              * s->stride + FBCON_PAD_X;
        gfx_color_t *src_row = s->pixels + (py + g_ch)    * s->stride + FBCON_PAD_X;
        memcpy(dst_row, src_row, (size_t)(row_bytes * sizeof(gfx_color_t)));
    }
    /* Clear last row */
    for (int py = FBCON_PAD_Y + total_h - g_ch; py < FBCON_PAD_Y + total_h; py++) {
        gfx_color_t *row = s->pixels + py * s->stride + FBCON_PAD_X;
        for (int i = 0; i < row_bytes; i++) row[i] = g_ansi[DEFAULT_BG];
    }
    /* Dirty the whole window content */
    compositor_dirty(g_win->frame);
}

static void newline(void) {
    g_cx = 0;
    g_cy++;
    if (g_cy >= g_rows) {
        scroll_up();
        g_cy = g_rows - 1;
    }
}

static void clear_screen(void) {
    gfx_surface_t *s = surf(); if (!s) return;
    gfx_fill_rect(s, (gfx_rect_t){0, 0, s->w, s->h}, g_ansi[DEFAULT_BG]);
    g_cx = g_cy = 0;
    compositor_dirty(g_win->frame);
}

static void clear_eol(void) {
    for (int c = g_cx; c < g_cols; c++) cell_clear(c, g_cy);
    dirty_row(g_cy);
}

/* ── CSI dispatch ─────────────────────────────────────────────────────────── */
static void csi_dispatch(char cmd) {
    int p0 = (g_csi_n > 0) ? g_csi_params[0] : 0;
    int p1 = (g_csi_n > 1) ? g_csi_params[1] : 0;
    switch (cmd) {
    case 'A': g_cy -= (p0 ? p0 : 1); if (g_cy < 0) g_cy = 0; break;
    case 'B': g_cy += (p0 ? p0 : 1); if (g_cy >= g_rows) g_cy = g_rows-1; break;
    case 'C': g_cx += (p0 ? p0 : 1); if (g_cx >= g_cols) g_cx = g_cols-1; break;
    case 'D': g_cx -= (p0 ? p0 : 1); if (g_cx < 0) g_cx = 0; break;
    case 'H': case 'f':
        g_cy = (p0 ? p0 - 1 : 0); if (g_cy >= g_rows) g_cy = g_rows-1;
        g_cx = (p1 ? p1 - 1 : 0); if (g_cx >= g_cols) g_cx = g_cols-1;
        break;
    case 'J':
        if (p0 == 2 || p0 == 0) clear_screen();
        break;
    case 'K':
        clear_eol();
        break;
    case 'm':
        /* SGR */
        for (int i = 0; i <= g_csi_n; i++) {
            int v = (i < g_csi_n) ? g_csi_params[i] : g_csi_cur;
            if (v == 0)  { g_fg = DEFAULT_FG; g_bg = DEFAULT_BG; g_bold = false; }
            else if (v == 1) g_bold = true;
            else if (v == 22) g_bold = false;
            else if (v >= 30 && v <= 37) g_fg = (uint8_t)(v - 30);
            else if (v == 39)            g_fg = DEFAULT_FG;
            else if (v >= 40 && v <= 47) g_bg = (uint8_t)(v - 40);
            else if (v == 49)            g_bg = DEFAULT_BG;
            else if (v >= 90 && v <= 97) g_fg = (uint8_t)(v - 90 + 8);
            else if (v >= 100&&v <=107)  g_bg = (uint8_t)(v - 100 + 8);
        }
        break;
    default: break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */
void fbcon_init(void) {
    const gfx_font_atlas_t *a = &g_font_atlas;
    g_cw = a->cell_w;
    g_ch = a->cell_h;

    /* Use ~80% of screen width, 90% of height */
    gfx_surface_t *screen = compositor_screen();
    int sw = screen ? screen->w : 1280;
    int sh = screen ? screen->h : 800;

    int usable_w = sw - 2 * FBCON_WIN_X - 2 * WM_BORDER - 2 * FBCON_PAD_X;
    int usable_h = sh - 2 * FBCON_WIN_Y - WM_TITLE_H - WM_BORDER - 2 * FBCON_PAD_Y;

    g_cols = usable_w / g_cw;
    g_rows = usable_h / g_ch;

    int win_w = g_cols * g_cw + 2 * FBCON_PAD_X;
    int win_h = g_rows * g_ch + 2 * FBCON_PAD_Y;

    g_win = wm_create("Terminal", FBCON_WIN_X, FBCON_WIN_Y, win_w, win_h);
    if (!g_win) {
        KLOG_ERR("fbcon: failed to create terminal window\n");
        return;
    }
    gfx_fill_rect(g_win->surface,
                  (gfx_rect_t){0, 0, g_win->surface->w, g_win->surface->h},
                  g_ansi[DEFAULT_BG]);
    compositor_compose();
    KLOG_INFO("fbcon: %dx%d cols/rows, cell %dx%d\n", g_cols, g_rows, g_cw, g_ch);
}

void fbcon_putchar(char c) {
    if (!g_win) return;

    switch (g_ps) {
    /* ── Normal text ──────────────────────────────────────────────────── */
    case PS_NORMAL:
        if (c == 0x1B) { g_ps = PS_ESC; return; }
        if (c == '\r') { g_cx = 0; return; }
        if (c == '\n') { newline(); dirty_row(g_cy); compositor_compose(); return; }
        if (c == '\t') {
            int next = (g_cx + 8) & ~7;
            while (g_cx < next && g_cx < g_cols) { cell_put(g_cx, g_cy, ' '); g_cx++; }
            dirty_row(g_cy); return;
        }
        if (c == '\b') {
            if (g_cx > 0) { g_cx--; cell_clear(g_cx, g_cy); dirty_row(g_cy); }
            return;
        }
        if ((uint8_t)c < 0x20) return;  /* ignore other controls */
        cell_put(g_cx, g_cy, c);
        dirty_row(g_cy);
        g_cx++;
        if (g_cx >= g_cols) newline();
        /* Flush every character for responsiveness */
        compositor_compose();
        return;

    /* ── ESC received ────────────────────────────────────────────────── */
    case PS_ESC:
        if (c == '[') { g_ps = PS_CSI; g_csi_n = 0; g_csi_cur = 0; return; }
        g_ps = PS_NORMAL;
        return;

    /* ── ESC[ received — collect params ─────────────────────────────── */
    case PS_CSI:
    case PS_CSI_PARAM:
        if (c >= '0' && c <= '9') {
            g_csi_cur = g_csi_cur * 10 + (c - '0');
            g_ps = PS_CSI_PARAM;
            return;
        }
        if (c == ';') {
            if (g_csi_n < 8) g_csi_params[g_csi_n++] = g_csi_cur;
            g_csi_cur = 0;
            return;
        }
        /* Final byte */
        if (g_csi_n < 8) g_csi_params[g_csi_n] = g_csi_cur;
        csi_dispatch(c);
        g_ps = PS_NORMAL;
        compositor_compose();
        return;
    }
}

void fbcon_puts(const char *s) {
    while (*s) fbcon_putchar(*s++);
}

/* Minimal vsnprintf for fbcon_printf (no FPU, no libc) */
static int fbcon_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t i = 0;
#define FPUT(c) do { if (i < cap - 1) buf[i++] = (c); } while(0)
    while (*fmt) {
        if (*fmt != '%') { FPUT(*fmt++); continue; }
        fmt++;
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

/* Simple kernel printf into fbcon */
static char g_fmtbuf[1024];
void fbcon_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fbcon_vsnprintf(g_fmtbuf, sizeof(g_fmtbuf), fmt, ap);
    va_end(ap);
    fbcon_puts(g_fmtbuf);
}

/* ── klog redirect ────────────────────────────────────────────────────────── */
static void fbcon_klog_write(const char *msg) {
    fbcon_puts(msg);
}

void fbcon_takeover_klog(void) {
    klog_set_write_fn(fbcon_klog_write);
}
