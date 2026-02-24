/* gfx/desktop.h — Desktop manager: WM event routing, window creation
 *
 * Responsibilities:
 *   1. Software-rendered mouse cursor (fallback when HW cursor fails)
 *   2. Multi-window setup: terminal(s), sysinfo, task manager
 *   3. Mouse hit-testing + drag/resize routing through WM
 *   4. Keyboard routing to focused window's shell
 */
#pragma once

/* Kernel task entry point.
 * Creates all desktop windows, then polls input rings forever,
 * routing mouse/keyboard events through the window manager.
 * Spawn with sched_spawn("desktop", desktop_task, NULL)
 * after compositor_init() and wm_init().                          */
void desktop_task(void *arg);
