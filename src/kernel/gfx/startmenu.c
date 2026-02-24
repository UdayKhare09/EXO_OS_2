/* gfx/startmenu.c — Pop-up start menu */
#include "startmenu.h"
#include "gfx.h"
#include "font.h"
#include "taskbar.h"
#include "compositor.h"
#include "lib/string.h"

/* ── HID keycodes needed ──────────────────────────────────────────────────── */
#define HID_ESC    0x29
#define HID_RETURN 0x28
#define HID_UP     0x52
#define HID_DOWN   0x51

/* ── Colors ───────────────────────────────────────────────────────────────── */
#define SM_BG         GFX_RGB(0x16, 0x16, 0x24)
#define SM_BG_HEADER  GFX_RGB(0x08, 0x20, 0x58)
#define SM_SEL_BG     GFX_RGB(0x1E, 0x48, 0xA8)
#define SM_BORDER     GFX_RGB(0x44, 0x60, 0xA8)
#define SM_SEP        GFX_RGB(0x48, 0x48, 0x70)   /* visible separator      */
#define SM_TEXT       GFX_RGB(0xEE, 0xEE, 0xF8)
#define SM_SUBTEXT    GFX_RGB(0x88, 0x88, 0xAA)
#define SM_HEADER_TXT GFX_RGB(0xFF, 0xFF, 0xFF)

/* ── Layout ───────────────────────────────────────────────────────────────── */
#define SM_W          320   /* menu panel width                              */
/* SM_ITEM_H must accommodate 2 text rows (cell_h each) + spacing + padding.
 * Font is rasterised at 26px → cell_h ≈ 26.  Two rows: 2*26+4+8pad = 64. */
#define SM_ITEM_H      64   /* row height — two lines at 26px font           */
#define SM_SEP_H       14   /* gap row containing the horizontal rule        */
#define SM_HEADER_H    64   /* branding header height                        */
#define SM_CORNER_R     8
#define SM_PADDING     12
#define SM_ICON_W      22   /* icon square size                              */
#define SM_ICON_X       9   /* icon left offset from box edge                */
#define SM_TEXT_X      40   /* label/subtitle left offset from box edge      */
/* Offset from HEADER_H where items begin (after the 2-px separator line) */
#define SM_ITEMS_YOFF  (SM_HEADER_H + 2)

/* ── Module state ─────────────────────────────────────────────────────────── */
static int g_sw, g_sh;
static bool g_open = false;
static int  g_selected = 0;
static startmenu_launch_fn_t g_launch_fn = NULL;

static struct {
    startmenu_item_kind_t kind;
    char label[48];
    char subtitle[48];
} g_items[STARTMENU_MAX_ITEMS];
static int g_item_count = 0;

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static int count_entries(void) {
    int n = 0;
    for (int i = 0; i < g_item_count; i++)
        if (g_items[i].kind == SM_ITEM_ENTRY) n++;
    return n;
}

/* Map logical "entry index" → flat item index (skip separators) */
static int entry_to_item(int entry) {
    int e = 0;
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].kind == SM_ITEM_ENTRY) {
            if (e == entry) return i;
            e++;
        }
    }
    return -1;
}

static int compute_height(void) {
    int h = SM_ITEMS_YOFF;   /* header + separator */
    for (int i = 0; i < g_item_count; i++)
        h += (g_items[i].kind == SM_ITEM_ENTRY) ? SM_ITEM_H : SM_SEP_H;
    return h + 6; /* bottom padding */
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
void startmenu_init(int screen_w, int screen_h, startmenu_launch_fn_t launch_fn) {
    g_sw = screen_w;
    g_sh = screen_h;
    g_launch_fn = launch_fn;
    g_open = false;
    g_selected = 0;
    g_item_count = 0;
}

void startmenu_add(startmenu_item_kind_t kind, const char *label,
                   const char *subtitle) {
    if (g_item_count >= STARTMENU_MAX_ITEMS) return;
    int i = g_item_count++;
    g_items[i].kind = kind;
    if (label)    strncpy(g_items[i].label,    label,    47);
    if (subtitle) strncpy(g_items[i].subtitle, subtitle, 47);
}

/* ── State ────────────────────────────────────────────────────────────────── */
bool startmenu_is_open(void) { return g_open; }
void startmenu_open(void)    {
    g_open = true;
    g_selected = 0;
    compositor_force_full();
}
void startmenu_close(void)   {
    g_open = false;
    compositor_force_full();
}
void startmenu_toggle(void)  { if (g_open) startmenu_close(); else startmenu_open(); }

/* ── Keyboard ─────────────────────────────────────────────────────────────── */
void startmenu_on_keycode(uint8_t keycode) {
    if (!g_open) return;
    int n = count_entries();

    switch (keycode) {
    case HID_ESC:
        startmenu_close();
        break;
    case HID_RETURN:
        if (g_launch_fn) {
            int item_idx = entry_to_item(g_selected);
            if (item_idx >= 0) {
                startmenu_close();
                g_launch_fn(item_idx);
            }
        }
        break;
    case HID_UP:
        if (g_selected > 0) g_selected--;
        break;
    case HID_DOWN:
        if (g_selected < n - 1) g_selected++;
        break;
    default:
        break;
    }
}

/* ── Click ────────────────────────────────────────────────────────────────── */
bool startmenu_on_click(int x, int y) {
    if (!g_open) return false;

    int menu_h = compute_height();
    int menu_y = g_sh - TASKBAR_H - menu_h;
    int menu_x = 0;

    /* Click outside the menu → close */
    if (x < menu_x || x >= menu_x + SM_W ||
        y < menu_y || y >= menu_y + menu_h) {
        startmenu_close();
        return false;
    }

    /* Find which item was clicked */
    int iy = menu_y + SM_HEADER_H;
    int entry_sel = -1;
    int entry_idx = 0;

    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].kind == SM_ITEM_ENTRY) {
            if (y >= iy && y < iy + SM_ITEM_H) {
                entry_sel = entry_idx;
                break;
            }
            iy += SM_ITEM_H;
            entry_idx++;
        } else {
            iy += SM_SEP_H;
        }
    }

    if (entry_sel >= 0 && g_launch_fn) {
        int item_idx = entry_to_item(entry_sel);
        startmenu_close();
        g_launch_fn(item_idx);
    }
    return true;
}

/* ── Draw ─────────────────────────────────────────────────────────────────── */
void startmenu_draw(gfx_surface_t *dst) {
    if (!g_open) return;

    int menu_h = compute_height();
    int menu_y = g_sh - TASKBAR_H - menu_h;
    int menu_x = 0;

    gfx_rect_t box = { menu_x, menu_y, SM_W, menu_h };

    /* Outer border */
    gfx_fill_rounded(dst, box, SM_BORDER, SM_CORNER_R);

    /* Inner background */
    gfx_rect_t inner = { box.x + 1, box.y + 1, box.w - 2, box.h - 2 };
    gfx_fill_rounded(dst, inner, SM_BG, SM_CORNER_R - 1);

    /* Header (OS branding) */
    gfx_rect_t hdr = { box.x + 1, box.y + 1, box.w - 2, SM_HEADER_H - 1 };
    gfx_fill_gradient_h(dst, hdr, SM_BG_HEADER, GFX_RGB(0x06, 0x14, 0x38));

    const gfx_font_atlas_t *a = &g_font_atlas;
    /* Two rows: title + subtitle, vertically centred in header */
    int two_lines_h = a->cell_h * 2 + 4;
    int hy = box.y + (SM_HEADER_H - two_lines_h) / 2;
    gfx_draw_string(dst, box.x + SM_PADDING, hy,
                    "EXO OS", SM_HEADER_TXT, GFX_TRANSPARENT);
    gfx_draw_string(dst, box.x + SM_PADDING, hy + a->cell_h + 4,
                    "kernel desktop environment",
                    SM_SUBTEXT, GFX_TRANSPARENT);

    /* Separator below header — 2 px thick, clearly visible */
    gfx_fill_rect(dst,
        (gfx_rect_t){ box.x + 1, box.y + SM_HEADER_H, box.w - 2, 2 },
        SM_SEP);

    /* Items — start at SM_ITEMS_YOFF (= SM_HEADER_H + 2) */
    int iy      = menu_y + SM_ITEMS_YOFF;
    (void)iy;
    iy          = menu_y + SM_ITEMS_YOFF;

    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].kind == SM_ITEM_SEPARATOR) {
            /* Visible 1-px rule in the middle of the separator row */
            int sy = iy + SM_SEP_H / 2;
            gfx_fill_rect(dst,
                (gfx_rect_t){ box.x + SM_TEXT_X, sy,
                              box.w - SM_TEXT_X - SM_PADDING, 1 },
                SM_SEP);
            iy += SM_SEP_H;
            continue;
        }

        bool is_sel = (i == entry_to_item(g_selected));

        /* Selection highlight */
        if (is_sel) {
            gfx_fill_rounded(dst,
                (gfx_rect_t){ box.x + 3, iy + 2, box.w - 6, SM_ITEM_H - 4 },
                SM_SEL_BG, 5);
        }

        /* Icon: vertically centred in row */
        gfx_color_t icon_col = is_sel
            ? GFX_RGB(0x70, 0xB0, 0xFF) : GFX_RGB(0x30, 0x50, 0x90);
        gfx_fill_rounded(dst,
            (gfx_rect_t){
                box.x + SM_ICON_X,
                iy + (SM_ITEM_H - SM_ICON_W) / 2,
                SM_ICON_W, SM_ICON_W },
            icon_col, 5);

        /* Label + subtitle block, vertically centred in the row */
        bool has_sub = (g_items[i].subtitle[0] != '\0');
        int  gap     = 3;                                /* inter-line gap      */
        int  block_h = has_sub ? (a->cell_h * 2 + gap) : a->cell_h;
        int  ty      = iy + (SM_ITEM_H - block_h) / 2;
        if (ty < iy) ty = iy;                           /* clamp              */

        gfx_color_t label_col = is_sel ? SM_HEADER_TXT : SM_TEXT;
        gfx_draw_string(dst, box.x + SM_TEXT_X, ty,
                        g_items[i].label, label_col, GFX_TRANSPARENT);

        if (has_sub) {
            gfx_draw_string(dst, box.x + SM_TEXT_X, ty + a->cell_h + gap,
                            g_items[i].subtitle, SM_SUBTEXT, GFX_TRANSPARENT);
        }

        iy += SM_ITEM_H;
    }
}
