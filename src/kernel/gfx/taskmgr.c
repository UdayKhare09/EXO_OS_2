/* gfx/taskmgr.c — Task Manager window
 *
 * Displays all running/sleeping/blocked tasks with:
 *   • TID, Name, CPU, Priority, State
 * Refreshes periodically from the desktop event loop.
 */
#include "taskmgr.h"
#include "gfx.h"
#include "compositor.h"
#include "font.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "lib/string.h"

#define TASKMGR_W   440
#define TASKMGR_H   360
#define PAD         6
#define HDR_H       24
#define ROW_H       20
#define MAX_DISPLAY 32

/* Column positions (pixels from left) */
#define COL_TID   PAD
#define COL_NAME  (PAD + 40)
#define COL_CPU   (PAD + 200)
#define COL_PRI   (PAD + 250)
#define COL_STATE (PAD + 310)

static const char *state_name(task_state_t s) {
    switch (s) {
    case TASK_RUNNABLE: return "READY";
    case TASK_RUNNING:  return "RUN";
    case TASK_BLOCKED:  return "BLOCK";
    case TASK_DEAD:     return "DEAD";
    case TASK_SLEEPING: return "SLEEP";
    default:            return "?";
    }
}

static gfx_color_t state_color(task_state_t s) {
    switch (s) {
    case TASK_RUNNING:  return GFX_RGB(0x50, 0xFF, 0x50);
    case TASK_RUNNABLE: return GFX_RGB(0x80, 0xCC, 0x80);
    case TASK_SLEEPING: return GFX_RGB(0x80, 0x80, 0xFF);
    case TASK_BLOCKED:  return GFX_RGB(0xFF, 0xAA, 0x40);
    default:            return GFX_RGB(0x88, 0x88, 0x88);
    }
}

static void u32_to_str(uint32_t v, char *buf, int buflen) {
    char tmp[12];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = '0' + (int)(v % 10); v /= 10; } }
    int j = 0;
    while (n > 0 && j < buflen - 1) buf[j++] = tmp[--n];
    buf[j] = '\0';
}

void taskmgr_refresh(gfx_window_t *win) {
    if (!win || !win->surface) return;
    gfx_surface_t *s = win->surface;

    /* Clear */
    gfx_fill_rect(s, (gfx_rect_t){0, 0, s->w, s->h}, GFX_RGB(0x12, 0x12, 0x20));

    /* Header background */
    gfx_fill_rect(s, (gfx_rect_t){0, 0, s->w, HDR_H}, GFX_RGB(0x1A, 0x1A, 0x30));

    int y = (HDR_H - g_font_atlas.cell_h) / 2;
    gfx_color_t hdr_col = GFX_RGB(0x70, 0xA0, 0xDD);
    gfx_draw_string(s, COL_TID,   y, "TID",   hdr_col, GFX_TRANSPARENT);
    gfx_draw_string(s, COL_NAME,  y, "Name",  hdr_col, GFX_TRANSPARENT);
    gfx_draw_string(s, COL_CPU,   y, "CPU",   hdr_col, GFX_TRANSPARENT);
    gfx_draw_string(s, COL_PRI,   y, "Pri",   hdr_col, GFX_TRANSPARENT);
    gfx_draw_string(s, COL_STATE, y, "State", hdr_col, GFX_TRANSPARENT);

    /* Separator line */
    gfx_fill_rect(s, (gfx_rect_t){0, HDR_H, s->w, 1}, GFX_RGB(0x40, 0x40, 0x55));

    /* Snapshot tasks */
    sched_task_info_t tasks[MAX_DISPLAY];
    int count = sched_snapshot_tasks(tasks, MAX_DISPLAY);

    y = HDR_H + 3;
    gfx_color_t text_col = GFX_RGB(0xCC, 0xCC, 0xDD);
    char buf[16];

    for (int i = 0; i < count && y + ROW_H < s->h; i++) {
        /* Alternating row background */
        if (i & 1)
            gfx_fill_rect(s, (gfx_rect_t){0, y, s->w, ROW_H},
                          GFX_RGB(0x16, 0x16, 0x28));

        int ty = y + (ROW_H - g_font_atlas.cell_h) / 2;

        u32_to_str(tasks[i].tid, buf, sizeof(buf));
        gfx_draw_string(s, COL_TID, ty, buf, text_col, GFX_TRANSPARENT);

        /* Truncate name to fit */
        char name_buf[16];
        strncpy(name_buf, tasks[i].name, 15);
        name_buf[15] = '\0';
        gfx_draw_string(s, COL_NAME, ty, name_buf, text_col, GFX_TRANSPARENT);

        u32_to_str(tasks[i].cpu_id, buf, sizeof(buf));
        gfx_draw_string(s, COL_CPU, ty, buf, text_col, GFX_TRANSPARENT);

        u32_to_str(tasks[i].priority, buf, sizeof(buf));
        gfx_draw_string(s, COL_PRI, ty, buf, text_col, GFX_TRANSPARENT);

        gfx_draw_string(s, COL_STATE, ty, state_name(tasks[i].state),
                        state_color(tasks[i].state), GFX_TRANSPARENT);

        y += ROW_H;
    }

    /* Task count summary at bottom */
    y = s->h - g_font_atlas.cell_h - 4;
    gfx_fill_rect(s, (gfx_rect_t){0, y - 2, s->w, g_font_atlas.cell_h + 6},
                  GFX_RGB(0x1A, 0x1A, 0x30));
    u32_to_str((uint32_t)count, buf, sizeof(buf));
    char summary[32] = "Tasks: ";
    /* Append count to summary */
    int slen = (int)strlen(summary);
    int blen = (int)strlen(buf);
    for (int i = 0; i < blen && slen < 31; i++)
        summary[slen++] = buf[i];
    summary[slen] = '\0';
    gfx_draw_string(s, PAD, y, summary,
                    GFX_RGB(0x88, 0x88, 0xAA), GFX_TRANSPARENT);

    wm_damage(win);
    compositor_compose();
}

gfx_window_t *taskmgr_create(int x, int y) {
    gfx_window_t *win = wm_create("Task Manager", x, y, TASKMGR_W, TASKMGR_H);
    if (!win) return NULL;
    win->flags = WM_FLAG_MOVABLE | WM_FLAG_CLOSABLE;
    taskmgr_refresh(win);
    return win;
}
