/* gfx/desktop.c — Desktop manager: WM event routing + window creation
 *
 * Responsibilities:
 *   1. Hardware cursor init (+ software fallback for compose)
 *   2. Create initial windows: 2 terminals, sysinfo, task manager
 *   3. Mouse hit-testing → WM drag/resize/raise/focus/close
 *   4. Keyboard routing to focused window's shell callback
 *   5. Periodic refresh of sysinfo + task manager content
 */
#include "desktop.h"
#include "cursor.h"
#include "wm.h"
#include "compositor.h"
#include "gfx.h"
#include "fbcon.h"
#include "sysinfo.h"
#include "taskmgr.h"
#include "shell/shell.h"
#include "drivers/input/input.h"
#include "sched/sched.h"
#include "lib/klog.h"

/* ── Terminal + shell pairs ───────────────────────────────────────────────── */
#define MAX_TERMS 4
static struct {
    fbcon_t  *con;
    shell_t  *sh;
} g_terms[MAX_TERMS];
static int g_term_count = 0;

/* ── Special windows ──────────────────────────────────────────────────────── */
static gfx_window_t *g_sysinfo_win = NULL;
static gfx_window_t *g_taskmgr_win = NULL;

/* ── Software cursor (drawn during compose if HW cursor not visible) ──────── */
static int  g_mouse_x, g_mouse_y;
static bool g_mouse_btn_left  = false;
static bool g_was_btn_left    = false;

/* ── Terminal-window key callback ─────────────────────────────────────────── */
static void term_key_cb(gfx_window_t *win, char ch) {
    /* Find which shell corresponds to this window */
    for (int i = 0; i < g_term_count; i++) {
        if (fbcon_get_window(g_terms[i].con) == win) {
            shell_on_char_inst(g_terms[i].sh, ch);
            return;
        }
    }
}

/* ── Terminal-window resize callback ──────────────────────────────────────── */
static void term_resize_cb(gfx_window_t *win, int new_w, int new_h) {
    for (int i = 0; i < g_term_count; i++) {
        if (fbcon_get_window(g_terms[i].con) == win) {
            fbcon_on_resize(g_terms[i].con, new_w, new_h);
            return;
        }
    }
}

/* ── Create a terminal window ─────────────────────────────────────────────── */
static void create_terminal(const char *title, int x, int y, int w, int h) {
    if (g_term_count >= MAX_TERMS) return;
    fbcon_t *con = fbcon_create(title, x, y, w, h);
    if (!con) return;
    shell_t *sh = shell_create(con);
    if (!sh) { fbcon_destroy(con); return; }

    gfx_window_t *win = fbcon_get_window(con);
    if (win) {
        win->on_key    = term_key_cb;
        win->on_resize = term_resize_cb;
    }

    g_terms[g_term_count].con = con;
    g_terms[g_term_count].sh  = sh;
    g_term_count++;
}

/* ── Handle mouse button press ────────────────────────────────────────────── */
static void handle_mouse_press(int mx, int my) {
    wm_hit_zone_t zone;
    gfx_window_t *win = wm_hit_test(mx, my, &zone);
    if (!win) return;

    /* Always raise + focus on click */
    wm_raise(win);
    wm_set_focus(win);

    switch (zone) {
    case WM_HIT_CLOSE:
        /* For now, just hide the window (don't destroy — keep it simple) */
        /* Could call win->on_close if set */
        break;

    case WM_HIT_TITLE:
        if (win->flags & WM_FLAG_MOVABLE)
            wm_begin_drag(win, WM_DRAG_MOVE, zone, mx, my);
        break;

    case WM_HIT_RESIZE_N:
    case WM_HIT_RESIZE_S:
    case WM_HIT_RESIZE_E:
    case WM_HIT_RESIZE_W:
    case WM_HIT_RESIZE_NW:
    case WM_HIT_RESIZE_NE:
    case WM_HIT_RESIZE_SW:
    case WM_HIT_RESIZE_SE:
        if (win->flags & WM_FLAG_RESIZABLE)
            wm_begin_drag(win, WM_DRAG_RESIZE, zone, mx, my);
        break;

    case WM_HIT_CONTENT:
        /* Route mouse click to window if it has a mouse callback */
        if (win->on_mouse) {
            gfx_rect_t cr = wm_content_rect(win);
            win->on_mouse(win, mx - cr.x, my - cr.y,
                         INPUT_BTN_LEFT);
        }
        break;

    default:
        break;
    }

    compositor_compose();
}

/* ── Handle mouse release ─────────────────────────────────────────────────── */
static void handle_mouse_release(void) {
    if (wm_is_dragging()) {
        wm_end_drag();
        compositor_compose();
    }
}

/* ── Handle mouse movement ────────────────────────────────────────────────── */
static void handle_mouse_move(int mx, int my) {
    if (wm_is_dragging()) {
        wm_update_drag(mx, my);
    }
    /* Always recompose for software cursor update */
    compositor_compose();
}

/* ── Desktop task entry ───────────────────────────────────────────────────── */
void desktop_task(void *arg) {
    (void)arg;

    gfx_surface_t *scr = compositor_screen();
    int sw = scr ? scr->w : 1280;
    int sh = scr ? scr->h : 720;

    /* Init hardware cursor */
    cursor_init();
    g_mouse_x = sw / 2;
    g_mouse_y = sh / 2;

    KLOG_INFO("desktop: creating windows (%dx%d)\n", sw, sh);

    /* ── Create windows ──────────────────────────────────────────────────── */

    /* Terminal 1: main terminal, left side */
    create_terminal("Terminal 1", 10, 10, sw * 2/5, sh * 2/3);

    /* Terminal 2: secondary terminal, offset */
    create_terminal("Terminal 2", sw * 2/5 + 30, 50, sw * 2/5, sh * 2/3);

    /* System Info: top-right */
    g_sysinfo_win = sysinfo_create(sw - 360, 10);

    /* Task Manager: bottom-right */
    g_taskmgr_win = taskmgr_create(sw - 460, sh - 400);

    /* Initial compose */
    compositor_compose();

    KLOG_INFO("desktop: entering event loop\n");

    /* ── Main event loop ─────────────────────────────────────────────────── */
    uint64_t last_refresh = 0;

    for (;;) {
        bool any = false;
        input_event_t ev;

        while (input_poll(&ev)) {
            any = true;

            if (ev.type == INPUT_EV_MOUSE) {
                /* Update cursor position */
                g_mouse_x += (int)ev.mouse_dx;
                g_mouse_y += (int)ev.mouse_dy;
                if (g_mouse_x < 0) g_mouse_x = 0;
                if (g_mouse_y < 0) g_mouse_y = 0;
                if (g_mouse_x >= sw) g_mouse_x = sw - 1;
                if (g_mouse_y >= sh) g_mouse_y = sh - 1;

                /* Move hardware cursor */
                cursor_update_delta((int)ev.mouse_dx, (int)ev.mouse_dy);

                /* Track button state */
                bool btn_now = (ev.mouse_buttons & INPUT_BTN_LEFT) != 0;

                if (btn_now && !g_was_btn_left) {
                    g_mouse_btn_left = true;
                    handle_mouse_press(g_mouse_x, g_mouse_y);
                } else if (!btn_now && g_was_btn_left) {
                    g_mouse_btn_left = false;
                    handle_mouse_release();
                } else if (btn_now) {
                    handle_mouse_move(g_mouse_x, g_mouse_y);
                } else {
                    /* No button — still need to redraw software cursor */
                    handle_mouse_move(g_mouse_x, g_mouse_y);
                }

                g_was_btn_left = btn_now;

            } else if (ev.type == INPUT_EV_KEY &&
                       ev.state == INPUT_KEY_PRESS) {
                char ch = input_keycode_to_ascii(ev.keycode, ev.modifiers);
                if (ch) {
                    /* Route to focused window's key callback */
                    gfx_window_t *focused = wm_get_focused();
                    if (focused && focused->on_key)
                        focused->on_key(focused, ch);
                }
            }
        }

        /* Periodic refresh of info windows (~1 second) */
        uint64_t now = sched_get_ticks();
        if (now - last_refresh >= 1000) {
            last_refresh = now;
            if (g_sysinfo_win) sysinfo_refresh(g_sysinfo_win);
            if (g_taskmgr_win) taskmgr_refresh(g_taskmgr_win);
        }

        if (!any) sched_sleep(5);
    }
}
