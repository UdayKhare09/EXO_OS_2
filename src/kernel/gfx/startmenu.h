/* gfx/startmenu.h — Pop-up start menu above the taskbar
 *
 * The menu opens bottom-left above the taskbar when the user presses
 * the Super key or clicks the taskbar Start button.
 *
 * Keyboard:  Up/Down → move selection, Enter → launch, Escape → close.
 * Mouse:     Click an item to launch, click outside to close.
 */
#pragma once
#include "gfx/gfx.h"
#include <stdbool.h>

/* Maximum items in the menu */
#define STARTMENU_MAX_ITEMS  16

/* Item kinds */
typedef enum {
    SM_ITEM_ENTRY,      /* normal clickable entry */
    SM_ITEM_SEPARATOR,  /* horizontal rule        */
} startmenu_item_kind_t;

/* Called when user activates an item (index = item index in the list) */
typedef void (*startmenu_launch_fn_t)(int index);

/* Initialise.  Must be called after compositor_init.
 * launch_fn is called whenever the user picks an item.             */
void startmenu_init(int screen_w, int screen_h,
                    startmenu_launch_fn_t launch_fn);

/* Add an item to the menu (call before first draw). */
void startmenu_add(startmenu_item_kind_t kind, const char *label,
                   const char *subtitle);

/* Query / toggle open state */
bool startmenu_is_open(void);
void startmenu_open(void);
void startmenu_close(void);
void startmenu_toggle(void);

/* Input routing (called by desktop event loop) */
void startmenu_on_keycode(uint8_t keycode);  /* raw HID keycode */
bool startmenu_on_click(int x, int y);       /* returns true if consumed */

/* Draw the menu onto dst (after wm_compose, before cursor) */
void startmenu_draw(gfx_surface_t *dst);
