/* gfx/sysinfo.c — System Information window
 *
 * Displays real-time system data:
 *   • OS version, architecture
 *   • CPU count (from SMP)
 *   • Memory stats (total/used/free from PMM)
 *   • Uptime (from scheduler jiffies)
 *   • USB device summary
 */
#include "sysinfo.h"
#include "gfx.h"
#include "compositor.h"
#include "font.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "arch/x86_64/smp.h"
#include "lib/string.h"

#define SYSINFO_W   340
#define SYSINFO_H   320
#define PAD         8
#define LINE_H      22

/* Helper: format a uint64 to decimal string */
static void u64_to_str(uint64_t v, char *buf, int buflen) {
    char tmp[24];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = '0' + (int)(v % 10); v /= 10; } }
    int j = 0;
    while (n > 0 && j < buflen - 1) buf[j++] = tmp[--n];
    buf[j] = '\0';
}

/* Helper: copy str into buf then append " MB" */
static void fmt_mb(char *out, int outsz, uint64_t mb) {
    char tmp[24];
    u64_to_str(mb, tmp, sizeof(tmp));
    int i = 0;
    for (int k = 0; tmp[k] && i < outsz - 4; k++) out[i++] = tmp[k];
    out[i++] = ' '; out[i++] = 'M'; out[i++] = 'B'; out[i] = '\0';
}

static void draw_label_value(gfx_surface_t *s, int x, int *y,
                             const char *label, const char *value) {
    gfx_color_t label_col = GFX_RGB(0x88, 0x88, 0xAA);
    gfx_color_t value_col = GFX_RGB(0xEE, 0xEE, 0xF0);
    int nx = gfx_draw_string(s, x, *y, label, label_col, GFX_TRANSPARENT);
    gfx_draw_string(s, nx, *y, value, value_col, GFX_TRANSPARENT);
    *y += LINE_H;
}

static void draw_separator(gfx_surface_t *s, int x, int *y, int w) {
    gfx_fill_rect(s, (gfx_rect_t){x, *y + 4, w - 2*PAD, 1},
                  GFX_RGB(0x40, 0x40, 0x55));
    *y += 10;
}

static void draw_section_header(gfx_surface_t *s, int x, int *y,
                                const char *title) {
    gfx_draw_string(s, x, *y, title,
                    GFX_RGB(0x60, 0xA0, 0xFF), GFX_TRANSPARENT);
    *y += LINE_H + 2;
}

void sysinfo_refresh(gfx_window_t *win) {
    if (!win || !win->surface) return;
    gfx_surface_t *s = win->surface;

    /* Clear content */
    gfx_fill_rect(s, (gfx_rect_t){0, 0, s->w, s->h}, GFX_RGB(0x12, 0x12, 0x20));

    int x = PAD;
    int y = PAD;
    char buf[64];

    /* ── System ──────────────────────────── */
    draw_section_header(s, x, &y, "System");
    draw_label_value(s, x, &y, "OS:     ", "EXO_OS 0.1.0");
    draw_label_value(s, x, &y, "Arch:   ", "x86_64 (64-bit)");
    draw_label_value(s, x, &y, "Boot:   ", "Limine 8");

    draw_separator(s, x, &y, s->w);

    /* ── CPU ─────────────────────────────── */
    draw_section_header(s, x, &y, "Processor");
    u64_to_str((uint64_t)smp_cpu_count(), buf, sizeof(buf));
    draw_label_value(s, x, &y, "CPUs:   ", buf);

    /* Uptime */
    uint64_t ticks = sched_get_ticks();
    uint64_t secs  = ticks / 1000;
    uint64_t mins  = secs / 60;
    uint64_t hours = mins / 60;
    /* Format: HHh MMm SSs */
    char uptime_buf[32];
    int ui = 0;
    if (hours > 0) {
        u64_to_str(hours, buf, sizeof(buf));
        for (int k = 0; buf[k]; k++) uptime_buf[ui++] = buf[k];
        uptime_buf[ui++] = 'h'; uptime_buf[ui++] = ' ';
    }
    u64_to_str(mins % 60, buf, sizeof(buf));
    for (int k = 0; buf[k]; k++) uptime_buf[ui++] = buf[k];
    uptime_buf[ui++] = 'm'; uptime_buf[ui++] = ' ';
    u64_to_str(secs % 60, buf, sizeof(buf));
    for (int k = 0; buf[k]; k++) uptime_buf[ui++] = buf[k];
    uptime_buf[ui++] = 's'; uptime_buf[ui] = '\0';
    draw_label_value(s, x, &y, "Uptime: ", uptime_buf);

    draw_separator(s, x, &y, s->w);

    /* ── Memory ──────────────────────────── */
    draw_section_header(s, x, &y, "Memory");

    uint64_t total_pages = pmm_get_total_pages();
    uint64_t free_pg     = pmm_get_free_pages();
    uint64_t used_pg     = total_pages - free_pg;
    uint64_t total_mb    = total_pages * PAGE_SIZE / (1024*1024);
    uint64_t free_mb     = free_pg * PAGE_SIZE / (1024*1024);
    uint64_t used_mb     = used_pg * PAGE_SIZE / (1024*1024);

    char mem_buf[32];
    fmt_mb(mem_buf, sizeof(mem_buf), total_mb);
    draw_label_value(s, x, &y, "Total:  ", mem_buf);

    fmt_mb(mem_buf, sizeof(mem_buf), used_mb);
    draw_label_value(s, x, &y, "Used:   ", mem_buf);

    fmt_mb(mem_buf, sizeof(mem_buf), free_mb);
    draw_label_value(s, x, &y, "Free:   ", mem_buf);

    /* Memory usage bar */
    y += 2;
    int bar_w = s->w - 2 * PAD;
    int bar_h = 12;
    gfx_fill_rounded(s, (gfx_rect_t){x, y, bar_w, bar_h},
                     GFX_RGB(0x25, 0x25, 0x35), 4);
    if (total_pages > 0) {
        int fill_w = (int)((uint64_t)bar_w * used_pg / total_pages);
        if (fill_w > bar_w) fill_w = bar_w;
        if (fill_w > 0)
            gfx_fill_rounded(s, (gfx_rect_t){x, y, fill_w, bar_h},
                             GFX_RGB(0x40, 0x90, 0xFF), 4);
    }

    wm_damage(win);
    compositor_compose();
}

gfx_window_t *sysinfo_create(int x, int y) {
    gfx_window_t *win = wm_create("System Info", x, y, SYSINFO_W, SYSINFO_H);
    if (!win) return NULL;
    win->flags = WM_FLAG_MOVABLE | WM_FLAG_CLOSABLE;  /* not resizable */
    sysinfo_refresh(win);
    return win;
}
