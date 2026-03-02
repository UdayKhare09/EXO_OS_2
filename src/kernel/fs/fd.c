/* fs/fd.c — File descriptor table implementation */
#include "fd.h"
#include "vfs.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "sched/task.h"

#include <stddef.h>

/* ── file_t allocation ────────────────────────────────────────────────────── */
file_t *file_alloc(vnode_t *vnode, int flags) {
    file_t *f = kzalloc(sizeof(file_t));
    if (!f) return NULL;
    f->vnode        = vnode;
    f->flags        = flags;
    f->offset       = (flags & O_APPEND) ? vnode->size : 0;
    f->path[0]      = '\0';
    f->refcount     = 1;
    f->f_ops        = NULL;
    f->private_data = NULL;
    vfs_vnode_get(vnode);
    return f;
}

/* Allocate a generic (non-vnode) file_t — used for sockets, pipes, etc. */
file_t *file_alloc_generic(file_ops_t *ops, void *priv, int flags) {
    file_t *f = kzalloc(sizeof(file_t));
    if (!f) return NULL;
    f->vnode        = NULL;
    f->flags        = flags;
    f->offset       = 0;
    f->path[0]      = '\0';
    f->refcount     = 1;
    f->f_ops        = ops;
    f->private_data = priv;
    return f;
}

void file_get(file_t *f) {
    if (f) f->refcount++;
}

void file_put(file_t *f) {
    if (!f) return;
    if (f->refcount == 0) return;
    f->refcount--;
    if (f->refcount == 0) {
        /* Generic file ops path (sockets, pipes, etc.) */
        if (f->f_ops) {
            if (f->f_ops->close)
                f->f_ops->close(f);
        } else if (f->vnode) {
            /* VFS-backed file */
            if (f->vnode->ops && f->vnode->ops->close)
                f->vnode->ops->close(f->vnode);
            vfs_vnode_put(f->vnode);
        }
        kfree(f);
    }
}

/* ── fd table ─────────────────────────────────────────────────────────────── */
int fd_alloc(struct task *t, file_t *f) {
    if (!t || !f) return -1;
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (t->fd_table[i] == NULL) {
            t->fd_table[i] = f;
            t->fd_flags[i] = 0;
            file_get(f); /* bump: table now holds a reference */
            return i;
        }
    }
    return -1; /* EMFILE */
}

int fd_install(struct task *t, int fd, file_t *f) {
    if (!t || !f || fd < 0 || fd >= FD_TABLE_SIZE) return -1;
    if (t->fd_table[fd]) {
        file_put(t->fd_table[fd]);
    }
    t->fd_table[fd] = f;
    t->fd_flags[fd] = 0; /* dup()/dup2() clear FD_CLOEXEC on the new fd */
    file_get(f);
    return fd;
}

file_t *fd_get(struct task *t, int fd) {
    if (!t || fd < 0 || fd >= FD_TABLE_SIZE) return NULL;
    return t->fd_table[fd];
}

int fd_close(struct task *t, int fd) {
    if (!t || fd < 0 || fd >= FD_TABLE_SIZE) return -1;
    if (!t->fd_table[fd]) return -1; /* EBADF */
    file_put(t->fd_table[fd]);  /* one put for the table's reference */
    t->fd_table[fd] = NULL;
    t->fd_flags[fd] = 0;
    return 0;
}

int fd_dup(struct task *t, int old_fd) {
    file_t *f = fd_get(t, old_fd);
    if (!f) return -1;
    return fd_alloc(t, f);
}

int fd_dup2(struct task *t, int old_fd, int new_fd) {
    if (!t || new_fd < 0 || new_fd >= FD_TABLE_SIZE) return -1;
    if (old_fd == new_fd) {
        return fd_get(t, old_fd) ? new_fd : -1;
    }
    file_t *f = fd_get(t, old_fd);
    if (!f) return -1;
    return fd_install(t, new_fd, f);
}

void fd_close_cloexec(struct task *t) {
    if (!t) return;
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (t->fd_table[i] && (t->fd_flags[i] & FD_CLOEXEC))
            fd_close(t, i);
    }
}

void fd_close_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (t->fd_table[i]) fd_close(t, i);
    }
}

int fd_setup_stdio(struct task *t, vnode_t *console) {
    if (!t || !console) return -1;
    for (int fd = 0; fd <= 2; fd++) {
        int flags = (fd == 1 || fd == 2) ? O_WRONLY : O_RDONLY;
        /* Call the vnode's open handler so /dev/console (or /dev/tty) injects
         * its file_ops (line discipline with echo, ICANON, etc.) via
         * pending_f_ops — matching the Linux behaviour where PID 1's stdio
         * goes through the full TTY line discipline from the start.         */
        if (console->ops && console->ops->open)
            console->ops->open(console, flags);
        file_t *f = file_alloc(console, flags);
        if (!f) return -1;
        /* Pick up injected f_ops */
        if (console->pending_f_ops) {
            f->f_ops        = console->pending_f_ops;
            f->private_data = console->pending_priv;
            console->pending_f_ops = NULL;
            console->pending_priv  = NULL;
        }
        strncpy(f->path, "/dev/tty", sizeof(f->path) - 1);
        f->path[sizeof(f->path) - 1] = '\0';
        if (fd_install(t, fd, f) < 0) { file_put(f); return -1; }
        file_put(f); /* table now holds the last reference */
    }
    return 0;
}
