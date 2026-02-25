/* shell/cputest.h — CPU / SMP / scheduler / multithreading test suite
 *
 * Exposed as the shell command "cputest".
 * Runs entirely in task context; no real-mode or ring-3 involvement.
 */
#pragma once
#include "gfx/fbcon.h"

/* Run the full test suite and print results to the fbcon console.
 * Blocks until all sub-tests complete (may take a few seconds).          */
void cputest_run(fbcon_t *con);
