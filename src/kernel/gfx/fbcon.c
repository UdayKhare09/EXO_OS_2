/* gfx/fbcon.c — Limine GOP framebuffer text console
 *
 * Renders text directly into the linear framebuffer Limine maps for us at
 * boot time.  No VirtIO GPU, no compositor, no window manager needed.
 *
 * Design notes
 * ─────────────
 *  • One global fbcon_t singleton (g_fbcon).  Shell / klog can share it.
 *  • Colour palette: 16 classic ANSI entries stored as 0xAARRGGBB.
 *  • ANSI sequences recognised (enough for the shell):
 *      ESC[0m  ESC[1m  ESC[2J  ESC[H
 *      ESC[<n>m  (30-37 fg, 40-47 bg, 90-97 bright fg, 100-107 bright bg)
 *  • Cursor is a single-cell filled block drawn/erased on demand.
 */

#include "fbcon.h"
#include "font.h"          /* g_font_atlas, font_get_glyph()  */
#include "lib/string.h"    /* memset, strlen                  */
#include "mm/kmalloc.h"    /* kzalloc                         */

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Colours ─────────────────────────────────────────────────────────────── */
static const uint32_t g_palette[16] = {
    /* normal (0-7) */
    0xFF1A1A2E,  /* 0  Black       */
    0xFFE06C75,  /* 1  Red         */
    0xFF98C379,  /* 2  Green       */
    0xFFE5C07B,  /* 3  Yellow      */
    0xFF61AFEF,  /* 4  Blue        */
    0xFFC678DD,  /* 5  Magenta     */
    0xFF56B6C2,  /* 6  Cyan        */
    0xFFABB2BF,  /* 7  White       */
    /* bright (8-15) */
    0xFF282C34,  /* 8  Bright Black  */
    0xFFBE5046,  /* 9  Bright Red    */
    0xFF7A9F60,  /* 10 Bright Green  */
    0xFFD19A66,  /* 11 Bright Yellow */
    0xFF528BFF,  /* 12 Bright Blue   */
    0xFFA9A1E1,  /* 13 Bright Magenta*/
    0xFF2BBAC5,  /* 14 Bright Cyan   */
    0xFFFFFFFF,  /* 15 Bright White  */
};
#define COL_BG_DEFAULT    g_palette[0]   /* dark navy background */
#define COL_FG_DEFAULT    g_palette[7]   /* soft white           */

/* ── ANSI parser state ───────────────────────────────────────────────────── */
typedef enum {
    ANSI_NORMAL,
    ANSI_ESC,
    ANSI_CSI,
} ansi_state_t;

/* ── Console struct ──────────────────────────────────────────────────────── */
#define MAX_CSI_PARAMS 8

struct fbcon {
    /* Framebuffer */
    uint32_t *fb;
    uint32_t  fb_w, fb_h, fb_pitch_u32;   /* pitch in uint32_t units */

    /* Font metrics */
    int cw, ch;        /* cell width / height in px */

    /* Grid dimensions */
    int cols, rows;

    /* Cursor position (in character cells) */
    int col, row;

    /* Current colours */
    uint32_t fg, bg;
    bool bold;

    /* Cursor visibility */
    bool cursor_visible;
    bool cursor_drawn;

    /* ANSI parser */
    ansi_state_t ansi;
    int  csi_params[MAX_CSI_PARAMS];
    int  csi_nparams;
};

static fbcon_t g_fbcon_storage;
static fbcon_t *g_fbcon = NULL;

/* ── Low-level pixel helpers ─────────────────────────────────────────────── */

static inline void put_pixel(fbcon_t *c, int x, int y, uint32_t colour) {
    if ((unsigned)x >= c->fb_w || (unsigned)y >= c->fb_h) return;
    c->fb[y * c->fb_pitch_u32 + x] = colour;
}

/* Fill a rect with a solid colour */
static void fill_rect(fbcon_t *c, int x, int y, int w, int h, uint32_t col) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            put_pixel(c, x + dx, y + dy, col);
}

/* Draw one character cell at pixel (px, py) with the given glyph bitmap */
static void draw_glyph(fbcon_t *c, int px, int py, char ch, uint32_t fg, uint32_t bg) {
    const uint8_t *bmp = font_get_glyph((unsigned char)ch);

    /* Background fill */
    fill_rect(c, px, py, c->cw, c->ch, bg);

    if (!bmp) return;

    /* Blend each pixel using the alpha coverage byte from the atlas */
    for (int gy = 0; gy < c->ch; gy++) {
        for (int gx = 0; gx < c->cw; gx++) {
            uint8_t a = bmp[gy * c->cw + gx];
            if (a == 0) continue;
            if (a == 255) {
                put_pixel(c, px + gx, py + gy, fg);
            } else {
                /* Simple alpha blend: out = fg*a/255 + bg*(255-a)/255 */
                uint32_t fa = a, ba = 255 - a;
                uint8_t r = (uint8_t)(((fg >> 16 & 0xFF) * fa + (bg >> 16 & 0xFF) * ba) / 255);
                uint8_t g_ = (uint8_t)(((fg >>  8 & 0xFF) * fa + (bg >>  8 & 0xFF) * ba) / 255);
                uint8_t b_ = (uint8_t)(((fg       & 0xFF) * fa + (bg       & 0xFF) * ba) / 255);
                put_pixel(c, px + gx, py + gy, 0xFF000000 | (r << 16) | (g_ << 8) | b_);
            }
        }
    }
}

/* ── Cursor ──────────────────────────────────────────────────────────────── */

static void cursor_erase(fbcon_t *c) {
    if (!c->cursor_drawn) return;
    int px = c->col * c->cw;
    int py = c->row * c->ch;
    fill_rect(c, px, py, c->cw, c->ch, c->bg);
    c->cursor_drawn = false;
}

static void cursor_draw(fbcon_t *c) {
    if (c->cursor_drawn) return;
    if (!c->cursor_visible) return;
    int px = c->col * c->cw;
    int py = c->row * c->ch;
    /* Draw a filled block in fg colour */
    fill_rect(c, px, py, c->cw, c->ch, c->fg);
    c->cursor_drawn = true;
}

void fbcon_show_cursor_inst(fbcon_t *c) {
    if (!c) return;
    c->cursor_visible = true;
    cursor_draw(c);
}

void fbcon_hide_cursor_inst(fbcon_t *c) {
    if (!c) return;
    c->cursor_visible = false;
    cursor_erase(c);
}

void fbcon_tick(fbcon_t *c) {
    if (!c || !c->cursor_visible) return;
    if (c->cursor_drawn) cursor_erase(c);
    else                 cursor_draw(c);
}

/* ── Scrolling ───────────────────────────────────────────────────────────── */
static void scroll_up(fbcon_t *c) {
    /* Blit rows 1..rows-1 up by one */
    uint32_t row_pixels = (uint32_t)c->ch * c->fb_pitch_u32;
    uint32_t total      = (uint32_t)(c->rows - 1) * row_pixels;

    uint32_t *dst = c->fb;
    uint32_t *src = c->fb + row_pixels;

    for (uint32_t i = 0; i < total; i++)
        dst[i] = src[i];

    /* Clear the last row */
    fill_rect(c, 0, (c->rows - 1) * c->ch, c->fb_w, c->ch, c->bg);
}

/* ── Newline / carriage-return ───────────────────────────────────────────── */
static void newline(fbcon_t *c) {
    c->col = 0;
    c->row++;
    if (c->row >= c->rows) {
        scroll_up(c);
        c->row = c->rows - 1;
    }
}

/* ── ANSI CSI dispatch ───────────────────────────────────────────────────── */
static void apply_sgr(fbcon_t *c) {
    for (int i = 0; i < c->csi_nparams; i++) {
        int p = c->csi_params[i];
        if (p == 0) {
            c->fg = COL_FG_DEFAULT; c->bg = COL_BG_DEFAULT; c->bold = false;
        } else if (p == 1) {
            c->bold = true;
        } else if (p == 2) {
            c->bold = false;
        } else if (p >= 30 && p <= 37) {
            c->fg = g_palette[(p - 30) + (c->bold ? 8 : 0)];
        } else if (p >= 40 && p <= 47) {
            c->bg = g_palette[p - 40];
        } else if (p >= 90 && p <= 97) {
            c->fg = g_palette[(p - 90) + 8];
        } else if (p >= 100 && p <= 107) {
            c->bg = g_palette[(p - 100) + 8];
        }
    }
}

static void csi_dispatch(fbcon_t *c, char cmd) {
    switch (cmd) {
    case 'm':
        if (c->csi_nparams == 0) {
            c->csi_params[0] = 0; c->csi_nparams = 1;
        }
        apply_sgr(c);
        break;
    case 'J':
        /* ESC[2J: clear screen */
        fill_rect(c, 0, 0, (int)c->fb_w, (int)c->fb_h, c->bg);
        break;
    case 'H':
        /* ESC[H or ESC[row;colH: move cursor */
        c->row = (c->csi_nparams > 0 && c->csi_params[0] > 0) ? c->csi_params[0] - 1 : 0;
        c->col = (c->csi_nparams > 1 && c->csi_params[1] > 0) ? c->csi_params[1] - 1 : 0;
        if (c->row >= c->rows) c->row = c->rows - 1;
        if (c->col >= c->cols) c->col = c->cols - 1;
        break;
    default:
        break;
    }
}

/* ── Core character emit ─────────────────────────────────────────────────── */
static void emit_char(fbcon_t *c, char ch) {
    switch (c->ansi) {

    case ANSI_NORMAL:
        if (ch == '\033') { c->ansi = ANSI_ESC; return; }
        if (ch == '\r') { c->col = 0; return; }
        if (ch == '\n') { newline(c); return; }
        if (ch == '\b') {
            if (c->col > 0) {
                c->col--;
                int px = c->col * c->cw, py = c->row * c->ch;
                fill_rect(c, px, py, c->cw, c->ch, c->bg);
            }
            return;
        }
        if (ch == '\t') {
            int next = (c->col + 8) & ~7;
            if (next >= c->cols) next = c->cols - 1;
            while (c->col < next) {
                int px = c->col * c->cw, py = c->row * c->ch;
                fill_rect(c, px, py, c->cw, c->ch, c->bg);
                c->col++;
            }
            return;
        }
        /* Printable */
        {
            int px = c->col * c->cw, py = c->row * c->ch;
            draw_glyph(c, px, py, ch, c->fg, c->bg);
            c->col++;
            if (c->col >= c->cols) newline(c);
        }
        return;

    case ANSI_ESC:
        if (ch == '[') {
            c->ansi = ANSI_CSI;
            for (int i = 0; i < MAX_CSI_PARAMS; i++) c->csi_params[i] = 0;
            c->csi_nparams = 0;
        } else {
            c->ansi = ANSI_NORMAL;
        }
        return;

    case ANSI_CSI:
        if (ch >= '0' && ch <= '9') {
            if (c->csi_nparams == 0) c->csi_nparams = 1;
            c->csi_params[c->csi_nparams - 1] =
                c->csi_params[c->csi_nparams - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (c->csi_nparams < MAX_CSI_PARAMS) {
                c->csi_nparams++;
                c->csi_params[c->csi_nparams - 1] = 0;
            }
        } else {
            csi_dispatch(c, ch);
            c->ansi = ANSI_NORMAL;
        }
        return;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void fbcon_init(const fbcon_fb_t *fb) {
    fbcon_t *c = &g_fbcon_storage;
    c->fb          = fb->fb;
    c->fb_w        = fb->width;
    c->fb_h        = fb->height;
    c->fb_pitch_u32 = fb->pitch / 4;

    c->cw = g_font_atlas.cell_w;
    c->ch = g_font_atlas.cell_h;

    c->cols = (int)(fb->width  / (uint32_t)c->cw);
    c->rows = (int)(fb->height / (uint32_t)c->ch);

    c->col = 0; c->row = 0;
    c->fg  = COL_FG_DEFAULT;
    c->bg  = COL_BG_DEFAULT;
    c->bold = false;

    c->cursor_visible = false;
    c->cursor_drawn   = false;
    c->ansi           = ANSI_NORMAL;

    /* Clear screen */
    fill_rect(c, 0, 0, (int)c->fb_w, (int)c->fb_h, c->bg);

    g_fbcon = c;
}

fbcon_t *fbcon_get(void) { return g_fbcon; }

void fbcon_putchar_inst(fbcon_t *c, char ch) {
    if (!c) return;
    bool was_drawn = c->cursor_drawn;
    if (was_drawn) cursor_erase(c);
    emit_char(c, ch);
    if (was_drawn) cursor_draw(c);
}

void fbcon_puts_inst(fbcon_t *c, const char *s) {
    if (!c || !s) return;
    bool was_drawn = c->cursor_drawn;
    if (was_drawn) cursor_erase(c);
    while (*s) emit_char(c, *s++);
    if (was_drawn) cursor_draw(c);
}

/* ── Minimal printf ──────────────────────────────────────────────────────── */

void fbcon_printf_inst(fbcon_t *c, const char *fmt, ...) {
    if (!c || !fmt) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    bool was_drawn = c->cursor_drawn;
    if (was_drawn) cursor_erase(c);
    for (const char *p = buf; *p; p++) emit_char(c, *p);
    if (was_drawn) cursor_draw(c);
}
