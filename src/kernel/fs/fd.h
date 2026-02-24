/* fs/fd.h — Per-task file descriptor table
 *
 * Each task has an fd_table of FD_TABLE_SIZE slots.
 * File descriptors are indices into that table.
 * A `file_t` wraps a vnode_t + offset + flags and is reference-counted
 * so that dup()/fork() share the same file position.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/vfs.h"

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define FD_TABLE_SIZE  256   /* max open files per task                      */
#define FD_MAX         (FD_TABLE_SIZE - 1)

/* ── Open file object ────────────────────────────────────────────────────── */
typedef struct file {
    vnode_t  *vnode;          /* underlying vnode (refcount managed by file) */
    uint64_t  offset;         /* current file position                       */
    int       flags;          /* O_RDONLY / O_WRONLY / O_RDWR etc.           */
    int       fd_flags;       /* FD_CLOEXEC, etc.                            */
    uint32_t  refcount;       /* number of fds pointing at this file         */
} file_t;

#define FD_CLOEXEC 1

/* forward declare task_t to avoid circular includes */
struct task;

/* ── Allocate / free a file_t ────────────────────────────────────────────── */
file_t *file_alloc(vnode_t *vnode, int flags);
void    file_get(file_t *f);     /* increment refcount */
void    file_put(file_t *f);     /* decrement refcount; frees when 0 */

/* ── fd table operations ─────────────────────────────────────────────────── */

/* Install `f` into the lowest free slot of task `t`. Returns fd ≥ 0, or -1. */
int fd_alloc(struct task *t, file_t *f);

/* Install `f` into a specific slot `fd` (close existing if present).
 * Returns `fd` on success, -1 on error. */
int fd_install(struct task *t, int fd, file_t *f);

/* Look up fd in task's table. Returns file_t* (NOT refcount-bumped) or NULL. */
file_t *fd_get(struct task *t, int fd);

/* Close fd slot: decrements file refcount, clears slot. Returns 0 or -1. */
int fd_close(struct task *t, int fd);

/* Duplicate fd (like dup()): returns new fd or -1. */
int fd_dup(struct task *t, int old_fd);

/* Duplicate fd into a specific slot (like dup2()): returns new_fd or -1. */
int fd_dup2(struct task *t, int old_fd, int new_fd);

/* Close all fds with FD_CLOEXEC set (called on exec). */
void fd_close_cloexec(struct task *t);

/* Close all open fds (called on task exit). */
void fd_close_all(struct task *t);

/* ── stdio helpers: install stdin/stdout/stderr from a vnode ─────────────── */
int fd_setup_stdio(struct task *t, vnode_t *console);
