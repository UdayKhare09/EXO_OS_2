/* gfx/sysinfo.h — System Information window */
#pragma once
#include "gfx/wm.h"

/* Create a system info window at (x,y). Returns the WM window. */
gfx_window_t *sysinfo_create(int x, int y);

/* Refresh the displayed information */
void sysinfo_refresh(gfx_window_t *win);
