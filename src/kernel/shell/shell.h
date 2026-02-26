/* shell/shell.h — Interactive kernel shell with command registration
 *
 * Bound to the global fbcon text console.  Input comes from the USB HID
 * driver via the input_ring (see drivers/input/input.h).
 *
 * Subsystems register commands via shell_register_cmd() during init,
 * before the shell task starts its REPL loop.
 */
#pragma once
#include "gfx/fbcon.h"
#include "fs/vfs.h"

/* ── Shell line buffer ───────────────────────────────────────────────────── */
#define SHELL_LINE_MAX 256

struct shell {
    fbcon_t *con;
    char     line[SHELL_LINE_MAX];
    int      len;
    char     cwd[VFS_MOUNT_PATH_MAX];
};

typedef struct shell shell_t;

/* ── Command registration system ─────────────────────────────────────────── */
typedef void (*shell_cmd_fn_t)(shell_t *sh, const char *args);

typedef struct {
    const char      *name;
    const char      *help;
    shell_cmd_fn_t   fn;
} shell_cmd_t;

/* Register a shell command.  Thread-safe (spinlock protected).
 * Must be called before the shell REPL loop starts.
 * Duplicates are silently rejected. */
void shell_register_cmd(const char *name, const char *help, shell_cmd_fn_t fn);

/* Sort the command table lexicographically.  Called once after all
 * registrations are complete (before REPL loop). */
void shell_sort_commands(void);

/* Create a shell attached to an fbcon console */
shell_t *shell_create(fbcon_t *con);

/* Destroy shell */
void shell_destroy(shell_t *sh);

/* Feed one character (ASCII + \n + \b) from the keyboard */
void shell_on_char_inst(shell_t *sh, char c);

/* Get the fbcon attached to this shell */
fbcon_t *shell_get_fbcon(shell_t *sh);
