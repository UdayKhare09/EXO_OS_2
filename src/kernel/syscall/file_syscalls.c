/* syscall/file_syscalls.c — POSIX file-I/O syscall implementations
 *
 * Called from syscall.c dispatcher with Linux x86-64 ABI arguments.
 * All syscalls return 0 or positive on success, or -errno on failure.
 *
 * Path resolution: if `path` is relative, join it with task->cwd first.
 */
#include "syscall.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

#include <stdint.h>
#include <stddef.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Resolve a possibly-relative user path to an absolute, normalised path.
 * Returns 0 on success; writes result into `out` (size >= VFS_MOUNT_PATH_MAX). */
static int resolve_path(const char *upath, char *out) {
    if (!upath || !upath[0]) return -EINVAL;
    if (upath[0] == '/') {
        return path_normalize(upath, out);
    }
    /* relative path — join with cwd */
    task_t *t = sched_current();
    if (!t) return -EINVAL;
    return path_join(t->cwd, upath, out);
}

static inline task_t *cur_task(void) { return sched_current(); }

/* ── sys_read ────────────────────────────────────────────────────────────── */
int64_t sys_read(int fd, void *buf, uint64_t count) {
    if (!buf || count == 0) return 0;
    task_t *t = cur_task();
    file_t *f = fd_get(t, fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;

    vnode_t *v = f->vnode;
    if (!v || !v->ops->read) return -EINVAL;

    /* Directories are not readable via read() */
    if (VFS_S_ISDIR(v->mode)) return -EISDIR;

    ssize_t n = v->ops->read(v, buf, (size_t)count, f->offset);
    if (n < 0) return n;
    f->offset += (uint64_t)n;
    return (int64_t)n;
}

/* ── sys_write ───────────────────────────────────────────────────────────── */
int64_t sys_write(int fd, const void *buf, uint64_t count) {
    if (!buf || count == 0) return 0;
    task_t *t = cur_task();
    file_t *f = fd_get(t, fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EACCES;

    vnode_t *v = f->vnode;
    if (!v || !v->ops->write) return -EINVAL;
    if (VFS_S_ISDIR(v->mode)) return -EISDIR;

    uint64_t off = (f->flags & O_APPEND) ? v->size : f->offset;
    ssize_t n = v->ops->write(v, buf, (size_t)count, off);
    if (n < 0) return n;
    f->offset = off + (uint64_t)n;
    return (int64_t)n;
}

/* ── sys_open ────────────────────────────────────────────────────────────── */
int64_t sys_open(const char *upath, int flags, uint32_t mode) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);

    if (!v) {
        /* If O_CREAT is set, create the file */
        if ((flags & O_CREAT) && err == -ENOENT) {
            /* Find parent directory */
            char parent_path[VFS_MOUNT_PATH_MAX];
            char name[VFS_NAME_MAX + 1];
            path_dirname(path, parent_path, sizeof(parent_path));
            path_basename(path, name, sizeof(name));

            vnode_t *parent = vfs_lookup(parent_path, true, &err);
            if (!parent) return err ? err : -ENOENT;

            if (!VFS_S_ISDIR(parent->mode)) {
                vfs_vnode_put(parent); return -ENOTDIR;
            }
            if (!parent->ops->create) {
                vfs_vnode_put(parent); return -EPERM;
            }
            v = parent->ops->create(parent, name, mode ? mode : 0644);
            vfs_vnode_put(parent);
            if (!v) return -EIO;
        } else {
            return err ? err : -ENOENT;
        }
    } else if (flags & O_EXCL) {
        /* File exists but O_EXCL demanded non-existence */
        vfs_vnode_put(v);
        return -EEXIST;
    }

    /* O_TRUNC: truncate regular file */
    if ((flags & O_TRUNC) && VFS_S_ISREG(v->mode) &&
        (flags & O_ACCMODE) != O_RDONLY) {
        if (v->ops->truncate) v->ops->truncate(v, 0);
    }

    /* Call fs open hook */
    if (v->ops->open) {
        r = v->ops->open(v, flags);
        if (r < 0) { vfs_vnode_put(v); return r; }
    }

    file_t *f = file_alloc(v, flags);
    vfs_vnode_put(v); /* file_alloc increments its own ref */
    if (!f) return -ENOMEM;

    if (flags & O_APPEND) f->offset = f->vnode->size;

    int fd = fd_alloc(cur_task(), f);
    file_put(f); /* fd table holds its own ref */
    if (fd < 0) return -EMFILE;
    return fd;
}

/* ── sys_close ───────────────────────────────────────────────────────────── */
int64_t sys_close(int fd) {
    return fd_close(cur_task(), fd) == 0 ? 0 : -EBADF;
}

/* ── vfs_stat → convert to linux_stat_t ────────────────────────────────── */
static void fill_linux_stat(linux_stat_t *lst, const vfs_stat_t *st) {
    __builtin_memset(lst, 0, sizeof(*lst));
    lst->st_ino    = st->ino;
    lst->st_mode   = st->mode;
    lst->st_nlink  = st->nlink;
    lst->st_uid    = st->uid;
    lst->st_gid    = st->gid;
    lst->st_size   = (int64_t)st->size;
    lst->st_blksize = 512;
    lst->st_blocks = (int64_t)((st->size + 511) / 512);
    lst->st_atime  = (uint64_t)st->atime;
    lst->st_mtime  = (uint64_t)st->mtime;
    lst->st_ctime  = (uint64_t)st->ctime;
}

/* ── sys_stat ────────────────────────────────────────────────────────────── */
int64_t sys_stat(const char *upath, linux_stat_t *buf) {
    if (!buf) return -EINVAL;
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;
    vfs_stat_t st;
    r = vfs_stat(path, &st);
    if (r < 0) return r;
    fill_linux_stat(buf, &st);
    return 0;
}

/* ── sys_fstat ───────────────────────────────────────────────────────────── */
int64_t sys_fstat(int fd, linux_stat_t *buf) {
    if (!buf) return -EINVAL;
    file_t *f = fd_get(cur_task(), fd);
    if (!f) return -EBADF;
    vfs_stat_t st;
    int r = f->vnode->ops->stat(f->vnode, &st);
    if (r < 0) return r;
    fill_linux_stat(buf, &st);
    return 0;
}

/* ── sys_lstat ───────────────────────────────────────────────────────────── */
int64_t sys_lstat(const char *upath, linux_stat_t *buf) {
    if (!buf) return -EINVAL;
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;
    vfs_stat_t st;
    r = vfs_lstat(path, &st);
    if (r < 0) return r;
    fill_linux_stat(buf, &st);
    return 0;
}

/* ── sys_lseek ───────────────────────────────────────────────────────────── */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int64_t sys_lseek(int fd, int64_t off, int whence) {
    file_t *f = fd_get(cur_task(), fd);
    if (!f) return -EBADF;
    if (VFS_S_ISDIR(f->vnode->mode)) return -EISDIR;

    int64_t new_off;
    switch (whence) {
    case SEEK_SET: new_off = off; break;
    case SEEK_CUR: new_off = (int64_t)f->offset + off; break;
    case SEEK_END: new_off = (int64_t)f->vnode->size + off; break;
    default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}

/* ── sys_dup / sys_dup2 ──────────────────────────────────────────────────── */
int64_t sys_dup(int old_fd) {
    int r = fd_dup(cur_task(), old_fd);
    return r < 0 ? -EBADF : r;
}

int64_t sys_dup2(int old_fd, int new_fd) {
    int r = fd_dup2(cur_task(), old_fd, new_fd);
    return r < 0 ? -EBADF : r;
}

/* ── sys_getcwd ──────────────────────────────────────────────────────────── */
int64_t sys_getcwd(char *buf, uint64_t size) {
    if (!buf || size == 0) return -EINVAL;
    task_t *t = cur_task();
    size_t len = strlen(t->cwd);
    if (len + 1 > size) return -ERANGE;
    memcpy(buf, t->cwd, len + 1);
    return (int64_t)(uintptr_t)buf;
}

/* ── sys_chdir ───────────────────────────────────────────────────────────── */
int64_t sys_chdir(const char *upath) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (!v) return err ? err : -ENOENT;
    if (!VFS_S_ISDIR(v->mode)) { vfs_vnode_put(v); return -ENOTDIR; }
    vfs_vnode_put(v);

    task_t *t = cur_task();
    strncpy(t->cwd, path, TASK_CWD_MAX - 1);
    t->cwd[TASK_CWD_MAX - 1] = '\0';
    return 0;
}

/* ── sys_mkdir ───────────────────────────────────────────────────────────── */
int64_t sys_mkdir(const char *upath, uint32_t mode) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;
    return vfs_mkdir(path, mode ? mode : 0755);
}

/* ── sys_rmdir ───────────────────────────────────────────────────────────── */
int64_t sys_rmdir(const char *upath) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;
    return vfs_rmdir(path);
}

/* ── sys_unlink ──────────────────────────────────────────────────────────── */
int64_t sys_unlink(const char *upath) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;
    return vfs_unlink(path);
}

/* ── sys_rename ──────────────────────────────────────────────────────────── */
int64_t sys_rename(const char *old_path, const char *new_path) {
    char old_abs[VFS_MOUNT_PATH_MAX], new_abs[VFS_MOUNT_PATH_MAX];
    int r;
    r = resolve_path(old_path, old_abs); if (r < 0) return r;
    r = resolve_path(new_path, new_abs); if (r < 0) return r;
    return vfs_rename(old_abs, new_abs);
}

/* ── sys_getdents64 ──────────────────────────────────────────────────────── */
int64_t sys_getdents64(int fd, void *dirp, uint64_t count) {
    if (!dirp || count < sizeof(linux_dirent64_t) + 2) return -EINVAL;

    file_t *f = fd_get(cur_task(), fd);
    if (!f) return -EBADF;
    vnode_t *v = f->vnode;
    if (!VFS_S_ISDIR(v->mode)) return -ENOTDIR;
    if (!v->ops->readdir) return -EINVAL;

    uint8_t     *buf     = (uint8_t *)dirp;
    uint64_t     written = 0;
    vfs_dirent_t de;

    while (written + sizeof(linux_dirent64_t) + 2 <= count) {
        int r = v->ops->readdir(v, &f->offset, &de);
        if (r == 0) break;   /* EOF */
        if (r < 0) return r; /* error */

        uint16_t name_len = (uint16_t)strlen(de.name);
        uint16_t reclen   = (uint16_t)((offsetof(linux_dirent64_t, d_name) +
                                        name_len + 1 + 7) & ~7u);
        if (written + reclen > count) break;

        linux_dirent64_t *lde = (linux_dirent64_t *)(buf + written);
        lde->d_ino    = de.ino;
        lde->d_off    = (int64_t)f->offset;
        lde->d_reclen = reclen;
        lde->d_type   = (uint8_t)de.type;
        memcpy(lde->d_name, de.name, name_len + 1);

        written += reclen;
    }
    return (int64_t)written;
}

/* ── sys_brk ─────────────────────────────────────────────────────────────── */
int64_t sys_brk(uint64_t addr) {
    /* Stub — not implemented yet */
    (void)addr;
    return -ENOSYS;
}
