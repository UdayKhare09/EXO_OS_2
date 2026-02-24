/* gfx/taskmgr.h — Task Manager window */
#pragma once
#include "gfx/wm.h"

/* Create a task manager window at (x,y). Returns the WM window. */
gfx_window_t *taskmgr_create(int x, int y);

/* Refresh the task list display */
void taskmgr_refresh(gfx_window_t *win);
