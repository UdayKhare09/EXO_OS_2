/* gfx/desktop.c — Desktop manager: WM event routing + window creation
 *
 * Responsibilities:
 *   1. Hardware cursor init (+ software fallback for compose)
 *   2. Create initial windows: 2 terminals, sysinfo, task manager
 *   3. Mouse hit-testing → WM drag/resize/raise/focus/close/min/max
 *   4. Keyboard routing to focused window's shell callback
 *   5. Global keyboard shortcuts (Alt+Tab, Alt+F4/F9/F10, Super+Space)
 *   6. Taskbar (always-visible strip at bottom)
 *   7. Start menu (pop-up launcher, keyboard driven)
 *   8. Periodic refresh of sysinfo + task manager content
 */
#include "desktop.h"
#include "cursor.h"
#include "wm.h"
#include "compositor.h"
#include "gfx.h"
#include "fbcon.h"
#include "sysinfo.h"
#include "taskmgr.h"
#include "taskbar.h"
#include "startmenu.h"
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
static gfx_window_t *g_sysinfo_win  = NULL;
static gfx_window_t *g_taskmgr_win = NULL;
static int           g_sw, g_sh;    /* screen dimensions */

/* ── Software cursor (drawn during compose if HW cursor not visible) ──────── */
static int  g_mouse_x, g_mouse_y;
static bool g_mouse_btn_left  = false;
static bool g_was_btn_left    = false;
static int  g_prev_cursor_x   = -1;
static int  g_prev_cursor_y   = -1;

/* ── USB HID keycodes for global shortcuts ────────────────────────────────── */
#define HID_TAB   0x2B
#define HID_F4    0x3D
#define HID_F9    0x42
#define HID_F10   0x43
#define HID_SPACE 0x2C
#define MOD_ALT   0x44   /* bits 2 (L_Alt) and 6 (R_Alt) */
#define MOD_SUPER 0x88   /* bits 3 (L_GUI) and 7 (R_GUI)  */

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

/* ── Sysinfo / TaskMgr resize callbacks ──────────────────────────────────── */
static void sysinfo_resize_cb(gfx_window_t *win, int nw, int nh) {
    (void)nw; (void)nh;
    sysinfo_refresh(win);
    wm_damage(win);
}
static void taskmgr_resize_cb(gfx_window_t *win, int nw, int nh) {
    (void)nw; (void)nh;
    taskmgr_refresh(win);
    wm_damage(win);
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
    KLOG_INFO("desktop: created terminal '%s'\n", title);
}

/* ── Start-menu launch callback ───────────────────────────────────────────── */
static void startmenu_launch(int item_idx) {
    switch (item_idx) {
    case 0: /* New Terminal */
    {
        /* Pick a slightly offset position each time */
        int off = g_term_count * 24;
        create_terminal("Terminal", 30 + off, 30 + off, g_sw * 2/5, g_sh * 2/3);
        compositor_compose();
        break;
    }
    case 2: /* System Info */
        if (g_sysinfo_win) {
            wm_restore(g_sysinfo_win);
            wm_raise(g_sysinfo_win);
        } else {
            g_sysinfo_win = sysinfo_create(g_sw - 360, 10);
        }
        compositor_compose();
        break;
    case 3: /* Task Manager */
        if (g_taskmgr_win) {
            wm_restore(g_taskmgr_win);
            wm_raise(g_taskmgr_win);
        } else {
            g_taskmgr_win = taskmgr_create(g_sw - 460, g_sh - 400);
        }
        compositor_compose();
        break;
    default:
        KLOG_INFO("desktop: start menu item %d (no action)\n", item_idx);
        break;
    }
}

/* ── Handle mouse button press ────────────────────────────────────────────── */
static void handle_mouse_press(int mx, int my) {
    /* 1. Let the start menu consume clicks first (when open) */
    if (startmenu_is_open()) {
        bool consumed = startmenu_on_click(mx, my);
        compositor_compose();
        if (consumed) return;
    }

    /* 2. Taskbar strip at the bottom */
    if (my >= g_sh - TASKBAR_H) {
        bool toggle_menu = false;
        taskbar_on_click(mx, my, &toggle_menu);
        if (toggle_menu) startmenu_toggle();
        compositor_compose();
        return;
    }

    /* 3. Normal window hit-test */
    wm_hit_zone_t zone;
    gfx_window_t *win = wm_hit_test(mx, my, &zone);
    if (!win) return;

    /* Always raise + focus on click */
    wm_raise(win);
    wm_set_focus(win);

    switch (zone) {
    case WM_HIT_CLOSE:
        if (win->on_close)
            win->on_close(win);
        else
            wm_destroy(win);
        break;

    case WM_HIT_MINIMIZE:
        wm_minimize(win);
        break;

    case WM_HIT_MAXIMIZE:
        if (win->maximized) wm_restore(win);
        else                wm_maximize(win);
        break;

    case WM_HIT_TITLE:
        if (!win->maximized && (win->flags & WM_FLAG_MOVABLE))
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
        if (win->on_mouse) {
            gfx_rect_t cr = wm_content_rect(win);
            win->on_mouse(win, mx - cr.x, my - cr.y, INPUT_BTN_LEFT);
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
        /* Dragging: need full window repaint */
        wm_update_drag(mx, my);
        compositor_compose();
    } else {
        /* Not dragging: only dirty the old and new cursor rectangles.
         * This avoids a full-screen redraw on every mouse wiggle.    */
        if (g_prev_cursor_x >= 0) {
            compositor_dirty((gfx_rect_t){
                g_prev_cursor_x - 2, g_prev_cursor_y - 2,
                CURSOR_W + 4, CURSOR_H + 4 });
        }
        compositor_dirty((gfx_rect_t){
            mx - 2, my - 2, CURSOR_W + 4, CURSOR_H + 4 });
        compositor_compose();
    }
    g_prev_cursor_x = mx;
    g_prev_cursor_y = my;
}

/* ── Desktop task entry ───────────────────────────────────────────────────── */
void desktop_task(void *arg) {
    (void)arg;

    gfx_surface_t *scr = compositor_screen();
    g_sw = scr ? scr->w : 1280;
    g_sh = scr ? scr->h : 720;

    /* Init hardware cursor */
    cursor_init();
    g_mouse_x = g_sw / 2;
    g_mouse_y = g_sh / 2;

    KLOG_INFO("desktop: creating windows (%dx%d)\n", g_sw, g_sh);

    /* ── Taskbar ─────────────────────────────────────────────────────────── */
    taskbar_init(g_sw, g_sh);
    compositor_set_bg_hook(taskbar_draw);    /* Taskbar is always visible and its clock changes every second:
     * register it as a persistent dirty rect so it re-renders each compose. */
    compositor_set_bg_rect((gfx_rect_t){ 0, g_sh - TASKBAR_H, g_sw, TASKBAR_H });
    /* ── Start menu ──────────────────────────────────────────────────────── */
    startmenu_init(g_sw, g_sh, startmenu_launch);
    startmenu_add(SM_ITEM_ENTRY,     "New Terminal",  "Open a shell window");
    startmenu_add(SM_ITEM_SEPARATOR, NULL,            NULL);
    startmenu_add(SM_ITEM_ENTRY,     "System Info",   "CPU, memory, uptime");
    startmenu_add(SM_ITEM_ENTRY,     "Task Manager",  "Running tasks");
    startmenu_add(SM_ITEM_SEPARATOR, NULL,            NULL);
    startmenu_add(SM_ITEM_ENTRY,     "Power Off",     "Halt the system");
    compositor_set_overlay_hook(startmenu_draw);

    /* ── Create initial windows ──────────────────────────────────────────── */

    /* Terminal 1: main terminal, left side */
    create_terminal("Terminal 1", 10, 10,
                    g_sw * 2/5, g_sh - TASKBAR_H - 60);

    /* Terminal 2: secondary terminal, offset */
    create_terminal("Terminal 2", g_sw * 2/5 + 30, 50,
                    g_sw * 2/5, g_sh - TASKBAR_H - 60);

    /* System Info: top-right */
    g_sysinfo_win = sysinfo_create(g_sw - 360, 10);
    if (g_sysinfo_win) g_sysinfo_win->on_resize = sysinfo_resize_cb;

    /* Task Manager: bottom-right (above taskbar) */
    g_taskmgr_win = taskmgr_create(g_sw - 460, g_sh - TASKBAR_H - 400);
    if (g_taskmgr_win) g_taskmgr_win->on_resize = taskmgr_resize_cb;

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
                if (g_mouse_x < 0)     g_mouse_x = 0;
                if (g_mouse_y < 0)     g_mouse_y = 0;
                if (g_mouse_x >= g_sw) g_mouse_x = g_sw - 1;
                if (g_mouse_y >= g_sh) g_mouse_y = g_sh - 1;

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
                    /* No button — still redraw software cursor */
                    handle_mouse_move(g_mouse_x, g_mouse_y);
                }

                g_was_btn_left = btn_now;

            } else if (ev.type == INPUT_EV_KEY &&
                       ev.state == INPUT_KEY_PRESS) {
                uint8_t kc  = ev.keycode;
                uint8_t mod = ev.modifiers;

                /* ── Global keyboard shortcuts ───────────────────────────── */

                /* Super+Space / Super+Tab  →  toggle start menu */
                if ((mod & MOD_SUPER) &&
                    (kc == HID_SPACE || kc == HID_TAB)) {
                    startmenu_toggle();
                    compositor_compose();
                    continue;
                }

                /* Route all keys to start menu when it is open */
                if (startmenu_is_open()) {
                    startmenu_on_keycode(kc);
                    compositor_compose();
                    continue;
                }

                /* Alt+Tab  →  cycle window focus forward */
                if ((mod & MOD_ALT) && kc == HID_TAB) {
                    wm_focus_next();
                    compositor_compose();
                    continue;
                }

                /* Alt+F4   →  close focused window */
                if ((mod & MOD_ALT) && kc == HID_F4) {
                    gfx_window_t *w = wm_get_focused();
                    if (w) {
                        if (w->on_close) w->on_close(w);
                        else             wm_destroy(w);
                        compositor_compose();
                    }
                    continue;
                }

                /* Alt+F9   →  minimize focused window */
                if ((mod & MOD_ALT) && kc == HID_F9) {
                    gfx_window_t *w = wm_get_focused();
                    if (w) { wm_minimize(w); compositor_compose(); }
                    continue;
                }

                /* Alt+F10  →  maximize / restore focused window */
                if ((mod & MOD_ALT) && kc == HID_F10) {
                    gfx_window_t *w = wm_get_focused();
                    if (w) {
                        if (w->maximized) wm_restore(w);
                        else              wm_maximize(w);
                        compositor_compose();
                    }
                    continue;
                }

                /* ── Route printable characters to focused shell ─────────── */
                {
                    char ch = input_keycode_to_ascii(kc, mod);
                    if (ch) {
                        gfx_window_t *focused = wm_get_focused();
                        if (focused && focused->on_key)
                            focused->on_key(focused, ch);
                    }
                }
            }
        }

        /* Periodic refresh (~1 second tick for sysinfo, clock) */
        uint64_t now = sched_get_ticks();
        if (now - last_refresh >= 1000) {
            last_refresh = now;
            if (g_sysinfo_win  && !g_sysinfo_win->minimized)
                sysinfo_refresh(g_sysinfo_win);
            if (g_taskmgr_win && !g_taskmgr_win->minimized)
                taskmgr_refresh(g_taskmgr_win);
            /* bg_rect covers the taskbar clock — already included in dirty
             * by compositor automatically; just compose to flush it. */
            compositor_compose();
        }

        if (!any) sched_sleep(1);   /* 1 ms idle sleep — better responsiveness */
    }
}
