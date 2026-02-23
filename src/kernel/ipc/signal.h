#pragma once
#include <stdint.h>

/* ── Signal numbers (POSIX-compatible subset) ────────────────────────────── */
#define NSIGS     32

#define SIGHUP     1   /* hangup                       */
#define SIGINT     2   /* interrupt (Ctrl-C)           */
#define SIGQUIT    3   /* quit                         */
#define SIGKILL    9   /* kill (uncatchable)           */
#define SIGUSR1   10   /* user-defined signal 1        */
#define SIGUSR2   12   /* user-defined signal 2        */
#define SIGPIPE   13   /* broken pipe                  */
#define SIGTERM   15   /* termination request          */
#define SIGCHLD   17   /* child stopped or exited      */

typedef void (*sig_handler_t)(int signo);

/* Special values for sig_handler_t */
#define SIG_DFL ((sig_handler_t)0)   /* default action (ignore in kernel) */
#define SIG_IGN ((sig_handler_t)1)   /* explicitly ignored                */

struct task;   /* forward declaration */

/* ── Lifecycle ───────────────────────────────────────────────────────────── */
sig_handler_t *signal_table_alloc(void);
void           signal_table_free(sig_handler_t *tbl);

/* ── Handler management ──────────────────────────────────────────────────── */
/* Register a handler for signal `sig` on task `t`. SIGKILL cannot be caught. */
void          signal_set(struct task *t, int sig, sig_handler_t handler);
sig_handler_t signal_get(struct task *t, int sig);

/* ── Delivery ────────────────────────────────────────────────────────────── */
/* Atomically raise signal `sig` on task `t`; unblocks it if blocked.
 * Safe to call from any CPU / any context. */
void signal_send(struct task *t, int sig);

/* Dispatch all pending signals for `t` in the CALLING task's context.
 * Must be called from task context, NOT from ISR context.
 * Called by ipc_recv() after being unblocked. */
void signal_dispatch(struct task *t);
