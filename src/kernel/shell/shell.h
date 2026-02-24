/* shell/shell.h — Instance-based interactive kernel shell
 *
 * Each shell_t is bound to a specific fbcon_t terminal.
 * Multiple shells can run in multiple terminal windows.
 */
#pragma once
#include "gfx/fbcon.h"

typedef struct shell shell_t;

/* Create a new shell instance attached to an fbcon terminal */
shell_t *shell_create(fbcon_t *con);

/* Destroy a shell instance */
void shell_destroy(shell_t *sh);

/* Feed one character (ASCII, '\n', '\b') to this shell.
 * Called from the desktop input task on each KEY_PRESS event. */
void shell_on_char_inst(shell_t *sh, char c);

/* Get the fbcon attached to this shell */
fbcon_t *shell_get_fbcon(shell_t *sh);

/* ── Legacy single-instance API ───────────────────────────────────────────── */
void shell_init(void);
void shell_on_char(char c);
