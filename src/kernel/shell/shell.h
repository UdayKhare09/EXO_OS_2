/* shell/shell.h — Simple interactive kernel shell
 *
 * Bound to the global fbcon text console.  Input comes from the USB HID
 * driver via the input_ring (see drivers/input/input.h).
 */
#pragma once
#include "gfx/fbcon.h"

typedef struct shell shell_t;

/* Create a shell attached to an fbcon console */
shell_t *shell_create(fbcon_t *con);

/* Destroy shell */
void shell_destroy(shell_t *sh);

/* Feed one character (ASCII + \n + \b) from the keyboard */
void shell_on_char_inst(shell_t *sh, char c);

/* Get the fbcon attached to this shell */
fbcon_t *shell_get_fbcon(shell_t *sh);
