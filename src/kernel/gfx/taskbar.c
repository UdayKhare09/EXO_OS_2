/* gfx/taskbar.c — Bottom taskbar: Start button + live window list + clock */
#include "taskbar.h"
#include "wm.h"
#include "gfx.h"
#include "font.h"
#include "sched/sched.h"
#include "lib/string.h"

/* ── Module state ─────────────────────────────────────────────────────────── */
static int g_sw, g_sh;

/* ── Colors ───────────────────────────────────────────────────────────────── */
#define TB_BG_L     GFX_RGB(0x18, 0x18, 0x28)
#define TB_BG_R     GFX_RGB(0x10, 0x10, 0x1C)
#define TB_SEP      GFX_RGB(0x30, 0x30, 0x48)
#define TB_START_L  GFX_RGB(0x1C, 0x4E, 0xA8)
#define TB_START_R  GFX_RGB(0x10, 0x2E, 0x70)
#define TB_WIN_ACT  GFX_RGB(0x20, 0x44, 0x88)
#define TB_WIN_HOV  GFX_RGB(0x28, 0x38, 0x58)
#define TB_WIN_INA  GFX_RGB(0x1A, 0x1A, 0x28)
#define TB_WIN_MIN  GFX_RGB(0x28, 0x28, 0x3A)
#define TB_TEXT     GFX_RGB(0xDD, 0xDD, 0xEE)
#define TB_TEXT_DIM GFX_RGB(0x88, 0x88, 0xA0)

/* ── Start button geometry ────────────────────────────────────────────────── */
#define START_W  88
#define START_H  TASKBAR_H

/* ── Window button width ──────────────────────────────────────────────────── */
#define WIN_BTN_W  140
#define WIN_BTN_PAD  4

/* ── Simple integer-only clock (ticks → seconds:minutes if tick=1ms) ──────── */
static void format_clock(char *buf, int bufsz) {
    uint64_t ticks = sched_get_ticks();   /* assumed milliseconds */
    uint64_t secs  = ticks / 1000;
    int      ss    = (int)(secs % 60);
    int      mm    = (int)((secs / 60) % 60);
    int      hh    = (int)((secs / 3600) % 24);
    /* format HH:MM:SS */
    if (bufsz < 9) return;
    buf[0] = '0' + hh / 10; buf[1] = '0' + hh % 10; buf[2] = ':';
    buf[3] = '0' + mm / 10; buf[4] = '0' + mm % 10; buf[5] = ':';
    buf[6] = '0' + ss / 10; buf[7] = '0' + ss % 10; buf[8] = '\0';
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
void taskbar_init(int screen_w, int screen_h) {
    g_sw = screen_w;
    g_sh = screen_h;
}

/* ── Draw ─────────────────────────────────────────────────────────────────── */
void taskbar_draw(gfx_surface_t *dst) {
    gfx_rect_t bar = { 0, g_sh - TASKBAR_H, g_sw, TASKBAR_H };

    /* Background gradient */
    gfx_fill_gradient_h(dst, bar, TB_BG_L, TB_BG_R);

    /* Top separator line */
    for (int x = 0; x < g_sw; x++)
        gfx_putpixel(dst, x, bar.y, TB_SEP);

    /* ── Start button ──────────────────────────────────────────────── */
    gfx_rect_t sbr = { 0, bar.y, START_W, TASKBAR_H };
    gfx_fill_gradient_h(dst, sbr, TB_START_L, TB_START_R);
    /* Right edge highlight */
    for (int py = bar.y; py < bar.y + TASKBAR_H; py++)
        gfx_putpixel(dst, START_W - 1, py, GFX_RGB(0x30, 0x68, 0xC8));

    {
        const gfx_font_atlas_t *a = &g_font_atlas;
        int ty = bar.y + (TASKBAR_H - a->cell_h) / 2;
        gfx_draw_string(dst, 12, ty, "EXO OS", GFX_WHITE, GFX_TRANSPARENT);
    }

    /* ── Window buttons ────────────────────────────────────────────── */
    {
        const gfx_font_atlas_t *a = &g_font_atlas;
        int bx = START_W + 4;
        int by = bar.y + (TASKBAR_H - (a->cell_h + 8)) / 2;
        int bh = a->cell_h + 8;

        for (gfx_window_t *w = wm_get_window_list(); w; w = w->next) {
            if (!w->visible) continue;
            if (bx + WIN_BTN_W > g_sw - 80) break; /* no room (leave clock space) */

            gfx_rect_t br = { bx, by, WIN_BTN_W - WIN_BTN_PAD, bh };
            gfx_color_t bg = w->minimized ? TB_WIN_MIN
                           : w->focused   ? TB_WIN_ACT
                                          : TB_WIN_INA;
            gfx_fill_rounded(dst, br, bg, 4);

            /* Left accent bar for focused window */
            if (w->focused && !w->minimized) {
                for (int py = by + 2; py < by + bh - 2; py++)
                    gfx_putpixel(dst, bx + 2, py, GFX_RGB(0x50, 0xA0, 0xFF));
            }

            /* Title text — clipped */
            int max_chars = (WIN_BTN_W - 12) / a->cell_w;
            char clipped[48];
            int i;
            for (i = 0; i < max_chars - 1 && w->title[i]; i++)
                clipped[i] = w->title[i];
            if (w->title[i] && i > 2) {
                clipped[i-1] = '.'; clipped[i] = '.'; i++;
            }
            clipped[i] = '\0';

            int ty = by + (bh - a->cell_h) / 2;
            gfx_color_t tcol = w->minimized ? TB_TEXT_DIM : TB_TEXT;
            gfx_draw_string(dst, bx + 8, ty, clipped, tcol, GFX_TRANSPARENT);

            bx += WIN_BTN_W;
        }
    }

    /* ── Clock (right side) ────────────────────────────────────────── */
    {
        char clk[12];
        format_clock(clk, sizeof(clk));
        const gfx_font_atlas_t *a = &g_font_atlas;
        int text_w = 8 * a->cell_w;  /* HH:MM:SS */
        int cx = g_sw - text_w - 8;
        int cy = bar.y + (TASKBAR_H - a->cell_h) / 2;
        gfx_draw_string(dst, cx, cy, clk, TB_TEXT, GFX_TRANSPARENT);
    }
}

/* ── Click handling ───────────────────────────────────────────────────────── */
bool taskbar_on_click(int x, int y, bool *start_menu_toggle_out) {
    *start_menu_toggle_out = false;
    int bar_y = g_sh - TASKBAR_H;
    if (y < bar_y) return false;

    /* Start button */
    if (x < START_W) {
        *start_menu_toggle_out = true;
        return true;
    }

    /* Window buttons */
    {
        const gfx_font_atlas_t *a = &g_font_atlas;
        int bh = a->cell_h + 8;
        int by = bar_y + (TASKBAR_H - bh) / 2;
        int bx = START_W + 4;

        for (gfx_window_t *w = wm_get_window_list(); w; w = w->next) {
            if (!w->visible) continue;
            if (bx + WIN_BTN_W > g_sw - 80) break;

            gfx_rect_t br = { bx, by, WIN_BTN_W - WIN_BTN_PAD, bh };
            if (x >= br.x && x < br.x + br.w && y >= br.y && y < br.y + br.h) {
                if (w->minimized)
                    wm_restore(w);
                else if (w->focused)
                    wm_minimize(w);
                else
                    wm_raise(w);
                return true;
            }
            bx += WIN_BTN_W;
        }
    }

    return true; /* consumed by taskbar even if nothing matched */
}
