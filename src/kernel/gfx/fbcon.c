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
 *  • Cursor is drawn by inverting the active cell (Linux-console-like).
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
    int saved_col, saved_row;

    /* Current colours */
    uint32_t fg, bg;
    bool bold;
    bool inverse;
    bool autowrap;
    bool newline_mode;
    bool wrap_pending;

    /* Cursor visibility */
    bool cursor_visible;
    bool cursor_drawn;
    int cursor_saved_col;
    int cursor_saved_row;
    bool cursor_saved_valid;
    uint32_t *cursor_backup;

    int scroll_top;
    int scroll_bottom;

    /* ANSI parser */
    ansi_state_t ansi;
    int  csi_params[MAX_CSI_PARAMS];
    int  csi_nparams;
    bool csi_private;
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
    if (c->cursor_saved_valid && c->cursor_backup) {
        int px = c->cursor_saved_col * c->cw;
        int py = c->cursor_saved_row * c->ch;
        for (int y = 0; y < c->ch; y++) {
            int fb_y = py + y;
            if ((unsigned)fb_y >= c->fb_h) continue;
            uint32_t *dst = &c->fb[fb_y * c->fb_pitch_u32 + px];
            uint32_t *src = &c->cursor_backup[y * c->cw];
            for (int x = 0; x < c->cw; x++) {
                int fb_x = px + x;
                if ((unsigned)fb_x >= c->fb_w) continue;
                dst[x] = src[x];
            }
        }
    }
    c->cursor_saved_valid = false;
    c->cursor_drawn = false;
}

static void cursor_draw(fbcon_t *c) {
    if (c->cursor_drawn) return;
    if (!c->cursor_visible) return;
    if (!c->cursor_backup) return;

    int px = c->col * c->cw;
    int py = c->row * c->ch;

    for (int y = 0; y < c->ch; y++) {
        int fb_y = py + y;
        if ((unsigned)fb_y >= c->fb_h) continue;
        uint32_t *row = &c->fb[fb_y * c->fb_pitch_u32 + px];
        uint32_t *bak = &c->cursor_backup[y * c->cw];
        for (int x = 0; x < c->cw; x++) {
            int fb_x = px + x;
            if ((unsigned)fb_x >= c->fb_w) continue;
            bak[x] = row[x];
        }
    }

    for (int y = 0; y < c->ch; y++) {
        int fb_y = py + y;
        if ((unsigned)fb_y >= c->fb_h) continue;
        for (int x = 0; x < c->cw; x++) {
            int fb_x = px + x;
            if ((unsigned)fb_x >= c->fb_w) continue;
            uint32_t p = c->fb[fb_y * c->fb_pitch_u32 + fb_x];
            c->fb[fb_y * c->fb_pitch_u32 + fb_x] = 0xFF000000u | (~p & 0x00FFFFFFu);
        }
    }

    c->cursor_saved_col = c->col;
    c->cursor_saved_row = c->row;
    c->cursor_saved_valid = true;
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
static void scroll_up_region(fbcon_t *c, int top_row, int bottom_row) {
    if (top_row < 0) top_row = 0;
    if (bottom_row >= c->rows) bottom_row = c->rows - 1;
    if (top_row >= bottom_row) return;

    int top_px = top_row * c->ch;
    int bot_px = (bottom_row + 1) * c->ch;
    int move_h = bot_px - top_px - c->ch;

    if (move_h > 0) {
        uint32_t *dst = c->fb + (uint32_t)top_px * c->fb_pitch_u32;
        uint32_t *src = c->fb + (uint32_t)(top_px + c->ch) * c->fb_pitch_u32;
        uint32_t total = (uint32_t)move_h * c->fb_pitch_u32;
        for (uint32_t i = 0; i < total; i++) dst[i] = src[i];
    }

    fill_rect(c, 0, bottom_row * c->ch, c->fb_w, c->ch, c->bg);
}

static void scroll_down_region(fbcon_t *c, int top_row, int bottom_row) {
    if (top_row < 0) top_row = 0;
    if (bottom_row >= c->rows) bottom_row = c->rows - 1;
    if (top_row >= bottom_row) return;

    int top_px = top_row * c->ch;
    int bot_px = (bottom_row + 1) * c->ch;
    int move_h = bot_px - top_px - c->ch;

    if (move_h > 0) {
        uint32_t *dst = c->fb + (uint32_t)(top_px + c->ch) * c->fb_pitch_u32;
        uint32_t *src = c->fb + (uint32_t)top_px * c->fb_pitch_u32;
        uint32_t total = (uint32_t)move_h * c->fb_pitch_u32;
        for (uint32_t i = total; i > 0; i--) dst[i - 1] = src[i - 1];
    }

    fill_rect(c, 0, top_row * c->ch, c->fb_w, c->ch, c->bg);
}

static void linefeed(fbcon_t *c) {
    if (c->row < c->scroll_top || c->row > c->scroll_bottom) {
        c->row++;
        if (c->row >= c->rows) {
            scroll_up_region(c, 0, c->rows - 1);
            c->row = c->rows - 1;
        }
        return;
    }

    if (c->row == c->scroll_bottom) {
        scroll_up_region(c, c->scroll_top, c->scroll_bottom);
    } else {
        c->row++;
    }
}

static void insert_chars(fbcon_t *c, int n) {
    if (n <= 0) n = 1;
    if (c->col >= c->cols) return;
    if (n > c->cols - c->col) n = c->cols - c->col;

    int y0 = c->row * c->ch;
    int src_px = c->col * c->cw;
    int dst_px = (c->col + n) * c->cw;
    int move_px = (c->cols - c->col - n) * c->cw;
    if (move_px > 0) {
        for (int y = 0; y < c->ch; y++) {
            uint32_t *row = &c->fb[(y0 + y) * c->fb_pitch_u32];
            for (int x = move_px - 1; x >= 0; x--) {
                row[dst_px + x] = row[src_px + x];
            }
        }
    }
    fill_rect(c, src_px, y0, n * c->cw, c->ch, c->bg);
}

static void delete_chars(fbcon_t *c, int n) {
    if (n <= 0) n = 1;
    if (c->col >= c->cols) return;
    if (n > c->cols - c->col) n = c->cols - c->col;

    int y0 = c->row * c->ch;
    int dst_px = c->col * c->cw;
    int src_px = (c->col + n) * c->cw;
    int move_px = (c->cols - c->col - n) * c->cw;
    if (move_px > 0) {
        for (int y = 0; y < c->ch; y++) {
            uint32_t *row = &c->fb[(y0 + y) * c->fb_pitch_u32];
            for (int x = 0; x < move_px; x++) {
                row[dst_px + x] = row[src_px + x];
            }
        }
    }
    fill_rect(c, dst_px + move_px, y0, n * c->cw, c->ch, c->bg);
}

static void insert_lines(fbcon_t *c, int n) {
    if (n <= 0) n = 1;
    if (c->row < c->scroll_top || c->row > c->scroll_bottom) return;
    int avail = c->scroll_bottom - c->row + 1;
    if (n > avail) n = avail;

    for (int i = 0; i < n; i++) {
        scroll_down_region(c, c->row, c->scroll_bottom);
    }
}

static void delete_lines(fbcon_t *c, int n) {
    if (n <= 0) n = 1;
    if (c->row < c->scroll_top || c->row > c->scroll_bottom) return;
    int avail = c->scroll_bottom - c->row + 1;
    if (n > avail) n = avail;

    for (int i = 0; i < n; i++) {
        scroll_up_region(c, c->row, c->scroll_bottom);
    }
}

/* ── ANSI CSI dispatch ───────────────────────────────────────────────────── */
static void apply_sgr(fbcon_t *c) {
    for (int i = 0; i < c->csi_nparams; i++) {
        int p = c->csi_params[i];
        if (p == 0) {
            c->fg = COL_FG_DEFAULT; c->bg = COL_BG_DEFAULT; c->bold = false; c->inverse = false;
        } else if (p == 1) {
            c->bold = true;
        } else if (p == 2) {
            c->bold = false;
        } else if (p == 7) {
            c->inverse = true;
        } else if (p == 27) {
            c->inverse = false;
        } else if (p == 39) {
            c->fg = g_palette[c->bold ? 15 : 7];
        } else if (p == 49) {
            c->bg = COL_BG_DEFAULT;
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

static void clamp_cursor(fbcon_t *c) {
    if (c->row < 0) c->row = 0;
    if (c->col < 0) c->col = 0;
    if (c->row >= c->rows) c->row = c->rows - 1;
    if (c->col >= c->cols) c->col = c->cols - 1;
}

static void csi_dispatch(fbcon_t *c, char cmd) {
    int p0 = (c->csi_nparams > 0 && c->csi_params[0] > 0) ? c->csi_params[0] : 1;
    switch (cmd) {
    case 'm':
        if (c->csi_nparams == 0) {
            c->csi_params[0] = 0; c->csi_nparams = 1;
        }
        apply_sgr(c);
        break;
    case 'A':
        c->row -= p0;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'B':
        c->row += p0;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'C':
        c->col += p0;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'D':
        c->col -= p0;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'G':
        c->col = p0 - 1;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case '@':
        insert_chars(c, p0);
        c->wrap_pending = false;
        break;
    case 'P':
        delete_chars(c, p0);
        c->wrap_pending = false;
        break;
    case 'L':
        insert_lines(c, p0);
        c->wrap_pending = false;
        break;
    case 'M':
        delete_lines(c, p0);
        c->wrap_pending = false;
        break;
    case 'J':
        /* ED — Erase in Display:
         *   ESC[J / ESC[0J : cursor -> end of screen
         *   ESC[1J          : start of screen -> cursor
         *   ESC[2J          : whole screen */
        {
            int mode = (c->csi_nparams > 0) ? c->csi_params[0] : 0;
            int cx = c->col * c->cw;
            int cy = c->row * c->ch;
            if (mode == 0) {
                fill_rect(c, cx, cy, (int)c->fb_w - cx, c->ch, c->bg);
                if (c->row + 1 < c->rows) {
                    fill_rect(c, 0, (c->row + 1) * c->ch,
                              (int)c->fb_w, (c->rows - (c->row + 1)) * c->ch, c->bg);
                }
            } else if (mode == 1) {
                if (c->row > 0) {
                    fill_rect(c, 0, 0, (int)c->fb_w, c->row * c->ch, c->bg);
                }
                fill_rect(c, 0, cy, cx + c->cw, c->ch, c->bg);
            } else if (mode == 2) {
                fill_rect(c, 0, 0, (int)c->fb_w, (int)c->fb_h, c->bg);
            }
        }
        break;
    case 'K':
        /* EL — Erase in Line:
         *   ESC[K / ESC[0K : cursor -> end of line
         *   ESC[1K          : start of line -> cursor
         *   ESC[2K          : whole line */
        {
            int mode = (c->csi_nparams > 0) ? c->csi_params[0] : 0;
            int cx = c->col * c->cw;
            int cy = c->row * c->ch;
            if (mode == 0) {
                fill_rect(c, cx, cy, (int)c->fb_w - cx, c->ch, c->bg);
            } else if (mode == 1) {
                fill_rect(c, 0, cy, cx + c->cw, c->ch, c->bg);
            } else if (mode == 2) {
                fill_rect(c, 0, cy, (int)c->fb_w, c->ch, c->bg);
            }
        }
        break;
    case 'H':
    case 'f':
        /* ESC[H or ESC[row;colH: move cursor */
        c->row = (c->csi_nparams > 0 && c->csi_params[0] > 0) ? c->csi_params[0] - 1 : 0;
        c->col = (c->csi_nparams > 1 && c->csi_params[1] > 0) ? c->csi_params[1] - 1 : 0;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'd':
        c->row = p0 - 1;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'r': {
        int top = (c->csi_nparams > 0 && c->csi_params[0] > 0) ? c->csi_params[0] - 1 : 0;
        int bot = (c->csi_nparams > 1 && c->csi_params[1] > 0) ? c->csi_params[1] - 1 : (c->rows - 1);
        if (top >= 0 && bot < c->rows && top < bot) {
            c->scroll_top = top;
            c->scroll_bottom = bot;
        } else {
            c->scroll_top = 0;
            c->scroll_bottom = c->rows - 1;
        }
        c->row = c->scroll_top;
        c->col = 0;
        break;
    }
    case 's':
        c->saved_row = c->row;
        c->saved_col = c->col;
        break;
    case 'u':
        c->row = c->saved_row;
        c->col = c->saved_col;
        c->wrap_pending = false;
        clamp_cursor(c);
        break;
    case 'h':
        if (c->csi_private) {
            int n = (c->csi_nparams > 0) ? c->csi_nparams : 1;
            for (int i = 0; i < n; i++) {
                int p = (c->csi_nparams > 0) ? c->csi_params[i] : 0;
                if (p == 25) {
                    c->cursor_visible = true;
                    cursor_draw(c);
                } else if (p == 7) {
                    c->autowrap = true;
                }
            }
        } else {
            int n = (c->csi_nparams > 0) ? c->csi_nparams : 1;
            for (int i = 0; i < n; i++) {
                int p = (c->csi_nparams > 0) ? c->csi_params[i] : 0;
                if (p == 20)
                    c->newline_mode = true;
            }
        }
        break;
    case 'l':
        if (c->csi_private) {
            int n = (c->csi_nparams > 0) ? c->csi_nparams : 1;
            for (int i = 0; i < n; i++) {
                int p = (c->csi_nparams > 0) ? c->csi_params[i] : 0;
                if (p == 25) {
                    c->cursor_visible = false;
                    cursor_erase(c);
                } else if (p == 7) {
                    c->autowrap = false;
                }
            }
        } else {
            int n = (c->csi_nparams > 0) ? c->csi_nparams : 1;
            for (int i = 0; i < n; i++) {
                int p = (c->csi_nparams > 0) ? c->csi_params[i] : 0;
                if (p == 20)
                    c->newline_mode = false;
            }
        }
        break;
    default:
        break;
    }
}

/* ── Core character emit ─────────────────────────────────────────────────── */
static void emit_char(fbcon_t *c, char ch) {
    switch (c->ansi) {

    case ANSI_NORMAL:
        if (ch == '\033') { c->ansi = ANSI_ESC; c->wrap_pending = false; return; }
        if (ch == '\r') { c->col = 0; c->wrap_pending = false; return; }
        if (ch == '\n') {
            if (c->newline_mode) c->col = 0;
            c->wrap_pending = false;
            linefeed(c);
            return;
        }
        if (ch == '\b') {
            c->wrap_pending = false;
            if (c->col > 0) {
                c->col--;
            }
            return;
        }
        if ((unsigned char)ch < 0x20 || (unsigned char)ch == 0x7F) {
            /* Ignore remaining C0 controls (e.g. BEL) and DEL for now. */
            return;
        }
        if (ch == '\t') {
            if (c->wrap_pending) {
                c->col = 0;
                linefeed(c);
                c->wrap_pending = false;
            }
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
            if (c->wrap_pending) {
                c->col = 0;
                linefeed(c);
                c->wrap_pending = false;
            }

            int px = c->col * c->cw, py = c->row * c->ch;
            uint32_t fg = c->inverse ? c->bg : c->fg;
            uint32_t bg = c->inverse ? c->fg : c->bg;
            draw_glyph(c, px, py, ch, fg, bg);
            if (c->col >= c->cols - 1) {
                c->wrap_pending = c->autowrap;
            } else {
                c->col++;
            }
        }
        return;

    case ANSI_ESC:
        if (ch == '[') {
            c->ansi = ANSI_CSI;
            for (int i = 0; i < MAX_CSI_PARAMS; i++) c->csi_params[i] = 0;
            c->csi_nparams = 0;
            c->csi_private = false;
        } else if (ch == '7') {
            c->saved_row = c->row;
            c->saved_col = c->col;
            c->wrap_pending = false;
            c->ansi = ANSI_NORMAL;
        } else if (ch == '8') {
            c->row = c->saved_row;
            c->col = c->saved_col;
            c->wrap_pending = false;
            clamp_cursor(c);
            c->ansi = ANSI_NORMAL;
        } else {
            c->ansi = ANSI_NORMAL;
        }
        return;

    case ANSI_CSI:
        if (ch == '?') {
            c->csi_private = true;
            return;
        }
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
    c->saved_col = 0; c->saved_row = 0;
    c->fg  = COL_FG_DEFAULT;
    c->bg  = COL_BG_DEFAULT;
    c->bold = false;
    c->inverse = false;
    c->autowrap = true;
    c->newline_mode = false;
    c->wrap_pending = false;

    c->cursor_visible = true;
    c->cursor_drawn   = false;
    c->cursor_saved_col = 0;
    c->cursor_saved_row = 0;
    c->cursor_saved_valid = false;
    c->cursor_backup = kmalloc((size_t)c->cw * (size_t)c->ch * sizeof(uint32_t));
    c->ansi           = ANSI_NORMAL;
    c->csi_private    = false;
    c->scroll_top = 0;
    c->scroll_bottom = c->rows - 1;

    /* Clear screen */
    fill_rect(c, 0, 0, (int)c->fb_w, (int)c->fb_h, c->bg);

    g_fbcon = c;
    cursor_draw(c);
}

fbcon_t *fbcon_get(void) { return g_fbcon; }

int fbcon_text_cols(void) {
    return g_fbcon ? g_fbcon->cols : 0;
}

int fbcon_text_rows(void) {
    return g_fbcon ? g_fbcon->rows : 0;
}

int fbcon_pixel_width(void) {
    return g_fbcon ? (int)g_fbcon->fb_w : 0;
}

int fbcon_pixel_height(void) {
    return g_fbcon ? (int)g_fbcon->fb_h : 0;
}

void fbcon_putchar_inst(fbcon_t *c, char ch) {
    if (!c) return;
    if (c->cursor_drawn) cursor_erase(c);
    emit_char(c, ch);
    if (c->cursor_visible) cursor_draw(c);
}

void fbcon_puts_inst(fbcon_t *c, const char *s) {
    if (!c || !s) return;
    if (c->cursor_drawn) cursor_erase(c);
    while (*s) emit_char(c, *s++);
    if (c->cursor_visible) cursor_draw(c);
}

/* ── Minimal printf ──────────────────────────────────────────────────────── */

void fbcon_printf_inst(fbcon_t *c, const char *fmt, ...) {
    if (!c || !fmt) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (c->cursor_drawn) cursor_erase(c);
    for (const char *p = buf; *p; p++) emit_char(c, *p);
    if (c->cursor_visible) cursor_draw(c);
}
