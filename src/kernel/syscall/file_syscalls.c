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
#include "fs/fat32/fat32.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/cred.h"
#include "net/socket_defs.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#include <stdint.h>
#include <stddef.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */
static void fill_linux_stat(linux_stat_t *lst, const vfs_stat_t *st);
int64_t sys_read(int fd, void *buf, uint64_t count);
int64_t sys_fstat(int fd, linux_stat_t *buf);
int64_t sys_fstatat(int dirfd, const char *upath, linux_stat_t *buf, int flags);
int64_t sys_fsync(int fd);
int64_t sys_fdatasync(int fd);
int64_t sys_sendfile(int out_fd, int in_fd, uint64_t *offset, uint64_t count);
int64_t sys_mkdirat(int dirfd, const char *upath, uint32_t mode);
int64_t sys_unlinkat(int dirfd, const char *upath, int flags);
int64_t sys_renameat(int olddirfd, const char *old_path, int newdirfd, const char *new_path);
int64_t sys_faccessat(int dirfd, const char *upath, int mode, int flags);
int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
int64_t sys_utimensat(int dirfd, const char *upath, const kernel_timespec_t times[2], int flags);
int64_t sys_fchmodat(int dirfd, const char *upath, uint32_t mode, int flags);
int64_t sys_fchownat(int dirfd, const char *upath, int owner, int group, int flags);

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

static int resolve_path_at(int dirfd, const char *upath, char *out) {
    if (!upath || !upath[0]) return -EINVAL;
    if (upath[0] == '/') {
        return path_normalize(upath, out);
    }

    task_t *t = sched_current();
    if (!t) return -EINVAL;

    if (dirfd == AT_FDCWD) {
        return path_join(t->cwd, upath, out);
    }

    file_t *dirf = fd_get(t, dirfd);
    if (!dirf) return -EBADF;
    if (!dirf->vnode || !VFS_S_ISDIR(dirf->vnode->mode)) return -ENOTDIR;
    if (!dirf->path[0]) return -EINVAL;
    return path_join(dirf->path, upath, out);
}

static inline task_t *cur_task(void) { return sched_current(); }

static inline int task_has_cap(const task_t *t, uint64_t cap) {
    return t && capable(&t->cred, cap);
}

static void vnode_clear_suid_on_write(vnode_t *v) {
    if (!v) return;
    if (!VFS_S_ISREG(v->mode)) return;
    if (v->mode & VFS_S_ISUID)
        v->mode &= ~VFS_S_ISUID;
}

static bool task_in_group(const task_t *t, uint32_t gid) {
    if (!t) return false;
    if (t->cred.gid == gid || t->cred.egid == gid || t->cred.fsgid == gid) return true;
    uint32_t limit = t->cred.group_count;
    if (limit > TASK_MAX_GROUPS) limit = TASK_MAX_GROUPS;
    for (uint32_t i = 0; i < limit; i++) {
        if (t->cred.groups[i] == gid)
            return true;
    }
    return false;
}

/* req_mode bits: 4=read, 2=write, 1=execute/search */
static int check_vnode_access(task_t *t, const vnode_t *v, int req_mode) {
    if (!t || !v) return -EINVAL;

    if ((req_mode & (4 | 1)) && task_has_cap(t, CAP_DAC_READ_SEARCH))
        return 0;

    if (task_has_cap(t, CAP_DAC_OVERRIDE)) {
        if (!(req_mode & 1))
            return 0;
        if (VFS_S_ISDIR(v->mode) || (v->mode & (VFS_S_IXUSR | VFS_S_IXGRP | VFS_S_IXOTH)))
            return 0;
    }

    uint32_t class_perm;
    if (t->cred.fsuid == v->uid)
        class_perm = (v->mode >> 6) & 0x7;
    else if (task_in_group(t, v->gid))
        class_perm = (v->mode >> 3) & 0x7;
    else
        class_perm = v->mode & 0x7;

    if ((req_mode & 4) && !(class_perm & 4)) return -EACCES;
    if ((req_mode & 2) && !(class_perm & 2)) return -EACCES;
    if ((req_mode & 1) && !(class_perm & 1)) return -EACCES;
    return 0;
}

static int check_path_access(const char *path, bool follow_last_link, int req_mode) {
    task_t *t = cur_task();
    if (!t) return -ESRCH;
    int err = 0;
    vnode_t *v = vfs_lookup(path, follow_last_link, &err);
    if (!v) return err ? err : -ENOENT;
    int rc = check_vnode_access(t, v, req_mode);
    vfs_vnode_put(v);
    return rc;
}

static int check_parent_dir_wx(const char *path) {
    char parent[VFS_MOUNT_PATH_MAX];
    char name[VFS_NAME_MAX + 1];
    path_dirname(path, parent, sizeof(parent));
    path_basename(path, name, sizeof(name));
    if (!name[0]) return -EINVAL;
    return check_path_access(parent, true, 3);
}

static int sticky_check_unlink_path(task_t *t, const char *path) {
    if (!t || !path) return -EINVAL;

    char parent_path[VFS_MOUNT_PATH_MAX];
    path_dirname(path, parent_path, sizeof(parent_path));

    int err = 0;
    vnode_t *parent = vfs_lookup(parent_path, true, &err);
    if (!parent) return err ? err : -ENOENT;

    if (!(parent->mode & VFS_S_ISVTX)) {
        vfs_vnode_put(parent);
        return 0;
    }

    if (task_has_cap(t, CAP_FOWNER)) {
        vfs_vnode_put(parent);
        return 0;
    }

    vnode_t *victim = vfs_lookup(path, false, &err);
    if (!victim) {
        vfs_vnode_put(parent);
        return err ? err : -ENOENT;
    }

    uint32_t euid = t->cred.euid;
    int allowed = (euid == victim->uid) || (euid == parent->uid);

    vfs_vnode_put(victim);
    vfs_vnode_put(parent);
    return allowed ? 0 : -EPERM;
}

static int apply_vnode_mode(vnode_t *v, uint32_t mode) {
    if (!v) return -EINVAL;
    if (v->ops && v->ops->chmod)
        return v->ops->chmod(v, mode);
    v->mode = (v->mode & VFS_S_IFMT) | (mode & 07777);
    return 0;
}

static int apply_vnode_owner(vnode_t *v, int owner, int group) {
    if (!v) return -EINVAL;
    if (v->ops && v->ops->chown)
        return v->ops->chown(v, owner, group);
    if (owner >= 0) v->uid = (uint32_t)owner;
    if (group >= 0) v->gid = (uint32_t)group;
    return 0;
}

int64_t sys_pread64(int fd, void *buf, uint64_t count, int64_t offset) {
    if (!buf || count == 0) return 0;
    if (offset < 0) return -EINVAL;

    task_t *t = cur_task();
    file_t *f = fd_get(t, fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;
    if (!f->vnode) return -ESPIPE;

    vnode_t *v = f->vnode;
    if (!v || !v->ops || !v->ops->read) return -EINVAL;
    if (VFS_S_ISDIR(v->mode)) return -EISDIR;

    int acc = check_vnode_access(t, v, 4);
    if (acc < 0) return acc;

    ssize_t n = v->ops->read(v, buf, (size_t)count, (uint64_t)offset);
    if (n < 0) return n;
    return (int64_t)n;
}

int64_t sys_pwrite64(int fd, const void *buf, uint64_t count, int64_t offset) {
    if (!buf || count == 0) return 0;
    if (offset < 0) return -EINVAL;

    task_t *t = cur_task();
    file_t *f = fd_get(t, fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EACCES;
    if (!f->vnode) return -ESPIPE;

    vnode_t *v = f->vnode;
    if (!v || !v->ops || !v->ops->write) return -EINVAL;
    if (VFS_S_ISDIR(v->mode)) return -EISDIR;

    int acc = check_vnode_access(t, v, 2);
    if (acc < 0) return acc;

    ssize_t n = v->ops->write(v, buf, (size_t)count, (uint64_t)offset);
    if (n < 0) return n;
    if (n > 0) vnode_clear_suid_on_write(v);
    return (int64_t)n;
}

int64_t sys_readv(int fd, const iovec_t *iov, int iovcnt) {
    if (!iov || iovcnt < 0) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t r = sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;
    }
    return total;
}

/* ── sys_read ────────────────────────────────────────────────────────────── */
int64_t sys_read(int fd, void *buf, uint64_t count) {
    if (!buf || count == 0) return 0;
    task_t *t = cur_task();
    file_t *f = fd_get(t, fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;
    /* Generic file ops path (sockets, pipes, etc.) */
    if (f->f_ops) {
        if (!f->f_ops->read) return -EINVAL;
        return (int64_t)f->f_ops->read(f, buf, (size_t)count);
    }
    vnode_t *v = f->vnode;
    if (!v || !v->ops->read) return -EINVAL;

    /* Directories are not readable via read() */
    if (VFS_S_ISDIR(v->mode)) return -EISDIR;

    int acc = check_vnode_access(t, v, 4);
    if (acc < 0) return acc;

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
    /* Generic file ops path (sockets, pipes, etc.) */
    if (f->f_ops) {
        if (!f->f_ops->write) return -EINVAL;
        return (int64_t)f->f_ops->write(f, buf, (size_t)count);
    }
    vnode_t *v = f->vnode;
    if (!v || !v->ops->write) return -EINVAL;
    if (VFS_S_ISDIR(v->mode)) return -EISDIR;

    int acc = check_vnode_access(t, v, 2);
    if (acc < 0) return acc;

    uint64_t off = (f->flags & O_APPEND) ? v->size : f->offset;
    ssize_t n = v->ops->write(v, buf, (size_t)count, off);
    if (n < 0) return n;
    f->offset = off + (uint64_t)n;
    if (n > 0) vnode_clear_suid_on_write(v);
    return (int64_t)n;
}

/* ── sys_sendfile ────────────────────────────────────────────────────────── */
int64_t sys_sendfile(int out_fd, int in_fd, uint64_t *offset, uint64_t count) {
    if (count == 0) return 0;

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    file_t *in_f = fd_get(t, in_fd);
    file_t *out_f = fd_get(t, out_fd);
    if (!in_f || !out_f) return -EBADF;
    if ((in_f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;
    if ((out_f->flags & O_ACCMODE) == O_RDONLY) return -EACCES;
    if (offset && !in_f->vnode) return -ESPIPE;

    uint64_t pos = 0;
    if (offset) pos = *offset;

    size_t buf_cap = (count < 65536ULL) ? (size_t)count : 65536U;
    if (buf_cap == 0) return 0;
    uint8_t *buf = kmalloc(buf_cap);
    if (!buf) return -ENOMEM;

    uint64_t total = 0;
    while (total < count) {
        size_t want = (size_t)(count - total);
        if (want > buf_cap) want = buf_cap;

        int64_t rd = offset
            ? sys_pread64(in_fd, buf, want, (int64_t)pos)
            : sys_read(in_fd, buf, want);
        if (rd == 0) break;
        if (rd < 0) {
            kfree(buf);
            if (offset) *offset = pos;
            return total ? (int64_t)total : rd;
        }

        size_t done = 0;
        while (done < (size_t)rd) {
            int64_t wr = sys_write(out_fd, buf + done, (uint64_t)((size_t)rd - done));
            if (wr <= 0) {
                kfree(buf);
                if (offset) *offset = pos;
                if (wr < 0) return total ? (int64_t)total : wr;
                return (int64_t)total;
            }
            done += (size_t)wr;
            total += (uint64_t)wr;
            if (offset) pos += (uint64_t)wr;
        }

        if ((size_t)rd < want) break;
    }

    kfree(buf);
    if (offset) *offset = pos;
    return (int64_t)total;
}

/* ── sys_open ────────────────────────────────────────────────────────────── */
static int64_t do_open_abs(const char *path, int flags, uint32_t mode) {
    int r;
    task_t *t = cur_task();
    if (!t) return -ESRCH;

    uint32_t create_mode = (mode ? mode : 0644) & ~t->umask;

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

            int acc = check_vnode_access(t, parent, 3);
            if (acc < 0) {
                vfs_vnode_put(parent);
                return acc;
            }

            if (!VFS_S_ISDIR(parent->mode)) {
                vfs_vnode_put(parent); return -ENOTDIR;
            }
            if (!parent->ops->create) {
                vfs_vnode_put(parent); return -EPERM;
            }
            v = parent->ops->create(parent, name, create_mode);
            vfs_vnode_put(parent);
            if (!v) return -EIO;
            apply_vnode_owner(v, (int)t->cred.fsuid, (int)t->cred.fsgid);
            apply_vnode_mode(v, create_mode);
        } else {
            return err ? err : -ENOENT;
        }
    } else if (flags & O_EXCL) {
        /* File exists but O_EXCL demanded non-existence */
        vfs_vnode_put(v);
        return -EEXIST;
    }

    int need = 0;
    switch (flags & O_ACCMODE) {
        case O_RDONLY: need = 4; break;
        case O_WRONLY: need = 2; break;
        case O_RDWR:   need = 6; break;
        default:       need = 4; break;
    }
    int acc = check_vnode_access(t, v, need);
    if (acc < 0) {
        vfs_vnode_put(v);
        return acc;
    }

    if (VFS_S_ISDIR(v->mode) && (need & 2)) {
        vfs_vnode_put(v);
        return -EISDIR;
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

    int file_flags = flags & ~O_CLOEXEC;
    file_t *f = file_alloc(v, file_flags);
    vfs_vnode_put(v); /* file_alloc increments its own ref */
    if (!f) return -ENOMEM;

    strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';

    if (flags & O_APPEND) f->offset = f->vnode->size;

    int fd = fd_alloc(cur_task(), f);
    file_put(f); /* fd table holds its own ref */
    if (fd < 0) return -EMFILE;
    if (flags & O_CLOEXEC) t->fd_flags[fd] |= FD_CLOEXEC;
    return fd;
}

int64_t sys_open(const char *upath, int flags, uint32_t mode) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path(upath, path);
    if (r < 0) return r;
    return do_open_abs(path, flags, mode);
}

int64_t sys_openat(int dirfd, const char *upath, int flags, uint32_t mode) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;
    return do_open_abs(path, flags, mode);
}

int64_t sys_fstatat(int dirfd, const char *upath, linux_stat_t *buf, int flags) {
    if (!buf) return -EINVAL;

    const int supported = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH;
    if (flags & ~supported) return -EINVAL;

    /* AT_EMPTY_PATH: stat the fd itself (used by glibc for fstat-like probes) */
    if ((flags & AT_EMPTY_PATH) && (!upath || !upath[0])) {
        return sys_fstat(dirfd, buf);
    }

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    vfs_stat_t st;
    if (flags & AT_SYMLINK_NOFOLLOW)
        r = vfs_lstat(path, &st);
    else
        r = vfs_stat(path, &st);
    if (r < 0) return r;
    fill_linux_stat(buf, &st);
    return 0;
}

int64_t sys_mkdirat(int dirfd, const char *upath, uint32_t mode) {
    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    r = check_parent_dir_wx(path);
    if (r < 0) return r;

    uint32_t create_mode = (mode ? mode : 0755) & ~t->umask;
    r = vfs_mkdir(path, create_mode);
    if (r < 0) return r;

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (v) {
        apply_vnode_owner(v, (int)t->cred.fsuid, (int)t->cred.fsgid);
        apply_vnode_mode(v, create_mode);
        vfs_vnode_put(v);
    }
    return 0;
}

int64_t sys_unlinkat(int dirfd, const char *upath, int flags) {
    if (flags & ~AT_REMOVEDIR) return -EINVAL;

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    r = check_parent_dir_wx(path);
    if (r < 0) return r;

    r = sticky_check_unlink_path(cur_task(), path);
    if (r < 0) return r;

    if (flags & AT_REMOVEDIR)
        return vfs_rmdir(path);
    return vfs_unlink(path);
}

int64_t sys_readlinkat(int dirfd, const char *upath, char *buf, uint64_t bufsiz) {
    if (!buf || bufsiz == 0) return -EINVAL;

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    r = vfs_readlink(path, buf, (size_t)bufsiz);
    return r < 0 ? r : (int64_t)strlen(buf);
}

int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    if (!target || !target[0]) return -EINVAL;

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(newdirfd, linkpath, path);
    if (r < 0) return r;
    return vfs_symlink(target, path);
}

/* ── sys_close ───────────────────────────────────────────────────────────── */
int64_t sys_close(int fd) {
    return fd_close(cur_task(), fd) == 0 ? 0 : -EBADF;
}

int64_t sys_fsync(int fd) {
    task_t *t = cur_task();
    file_t *f = fd_get(t, fd);
    if (!f) return -EBADF;

    /* Non-VFS descriptors (pipes/sockets/eventfd/etc.) have no inode to flush. */
    if (!f->vnode) return -EINVAL;

    if (f->vnode->ops && f->vnode->ops->sync) {
        int r = f->vnode->ops->sync(f->vnode);
        if (r < 0) return r;
    }
    return 0;
}

int64_t sys_fdatasync(int fd) {
    /* No separate metadata/data distinction yet; treat as fsync(). */
    return sys_fsync(fd);
}

/* ── vfs_stat → convert to linux_stat_t ────────────────────────────────── */
static void fill_linux_stat(linux_stat_t *lst, const vfs_stat_t *st) {
    __builtin_memset(lst, 0, sizeof(*lst));
    lst->st_dev    = st->dev;
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
    lst->st_rdev   = st->rdev;
    lst->st_ctime  = (uint64_t)st->ctime;
}

/* ── sys_stat ────────────────────────────────────────────────────────────── */
int64_t sys_stat(const char *upath, linux_stat_t *buf) {
    return sys_fstatat(AT_FDCWD, upath, buf, 0);
}

/* ── sys_fstat ───────────────────────────────────────────────────────────── */
int64_t sys_fstat(int fd, linux_stat_t *buf) {
    if (!buf) return -EINVAL;
    file_t *f = fd_get(cur_task(), fd);
    if (!f) return -EBADF;    /* Non-vnode fds (sockets etc.) return a minimal stat */
    if (!f->vnode) {
        __builtin_memset(buf, 0, sizeof(*buf));
        buf->st_mode = 0140666;  /* S_IFSOCK | rw-rw-rw- */
        return 0;
    }    vfs_stat_t st;
    int r = f->vnode->ops->stat(f->vnode, &st);
    if (r < 0) return r;
    fill_linux_stat(buf, &st);
    return 0;
}

/* ── sys_lstat ───────────────────────────────────────────────────────────── */
int64_t sys_lstat(const char *upath, linux_stat_t *buf) {
    return sys_fstatat(AT_FDCWD, upath, buf, AT_SYMLINK_NOFOLLOW);
}

/* ── sys_lseek ───────────────────────────────────────────────────────────── */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int64_t sys_lseek(int fd, int64_t off, int whence) {
    file_t *f = fd_get(cur_task(), fd);
    if (!f) return -EBADF;
    if (!f->vnode) return -ESPIPE;  /* pipes/sockets are not seekable */
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
    return (int64_t)(len + 1);  /* Linux ABI: return byte count, not pointer */
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
    r = check_vnode_access(cur_task(), v, 1);
    if (r < 0) { vfs_vnode_put(v); return r; }
    vfs_vnode_put(v);

    task_t *t = cur_task();
    strncpy(t->cwd, path, TASK_CWD_MAX - 1);
    t->cwd[TASK_CWD_MAX - 1] = '\0';
    return 0;
}

/* ── sys_mkdir ───────────────────────────────────────────────────────────── */
int64_t sys_mkdir(const char *upath, uint32_t mode) {
    return sys_mkdirat(AT_FDCWD, upath, mode);
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
    return sys_unlinkat(AT_FDCWD, upath, 0);
}

/* ── sys_rename ──────────────────────────────────────────────────────────── */
int64_t sys_rename(const char *old_path, const char *new_path) {
    return sys_renameat(AT_FDCWD, old_path, AT_FDCWD, new_path);
}

int64_t sys_renameat(int olddirfd, const char *old_path, int newdirfd, const char *new_path) {
    char old_abs[VFS_MOUNT_PATH_MAX], new_abs[VFS_MOUNT_PATH_MAX];
    int r;
    r = resolve_path_at(olddirfd, old_path, old_abs); if (r < 0) return r;
    r = resolve_path_at(newdirfd, new_path, new_abs); if (r < 0) return r;

    r = check_parent_dir_wx(old_abs); if (r < 0) return r;
    r = check_parent_dir_wx(new_abs); if (r < 0) return r;

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    r = sticky_check_unlink_path(t, old_abs);
    if (r < 0) return r;

    int new_err = 0;
    vnode_t *new_target = vfs_lookup(new_abs, false, &new_err);
    if (new_target) {
        vfs_vnode_put(new_target);
        r = sticky_check_unlink_path(t, new_abs);
        if (r < 0) return r;
    }

    return vfs_rename(old_abs, new_abs);
}

int64_t sys_faccessat(int dirfd, const char *upath, int mode, int flags) {
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
    if (mode & ~7) return -EINVAL;

    linux_stat_t st;
    int stat_flags = (flags & AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0;
    int r = sys_fstatat(dirfd, upath, &st, stat_flags);
    if (r < 0) return r;

    if (mode == 0) return 0; /* F_OK */

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    if (task_has_cap(t, CAP_DAC_OVERRIDE)) return 0;

    uint32_t class_perm;
    if (t->cred.fsuid == st.st_uid)
        class_perm = (st.st_mode >> 6) & 0x7;
    else if (task_in_group(t, st.st_gid))
        class_perm = (st.st_mode >> 3) & 0x7;
    else
        class_perm = st.st_mode & 0x7;

    if ((mode & 4) && !(class_perm & 4)) return -EACCES;
    if ((mode & 2) && !(class_perm & 2)) return -EACCES;
    if ((mode & 1) && !(class_perm & 1)) return -EACCES;
    return 0;
}

int64_t sys_utimensat(int dirfd, const char *upath, const kernel_timespec_t times[2], int flags) {
    (void)times; /* Timestamp precision/control not yet implemented. */

    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    /* futimens(fd, ...) path via utimensat(fd, NULL, ...) */
    if (!upath) {
        file_t *f = fd_get(t, dirfd);
        if (!f) return -EBADF;
        if (!f->vnode) return -EINVAL;

        int acc = check_vnode_access(t, f->vnode, 2);
        if (acc < 0 && t->cred.fsuid != f->vnode->uid) return -EACCES;

        uint64_t now = sched_get_ticks() / 1000;
        f->vnode->atime = (int64_t)now;
        f->vnode->mtime = (int64_t)now;
        f->vnode->ctime = (int64_t)now;
        return 0;
    }

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    int err = 0;
    bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0;
    vnode_t *v = vfs_lookup(path, follow, &err);
    if (!v) return err ? err : -ENOENT;

    int acc = check_vnode_access(t, v, 2);
    if (acc < 0 && t->cred.fsuid != v->uid) {
        vfs_vnode_put(v);
        return -EACCES;
    }

    uint64_t now = sched_get_ticks() / 1000;
    v->atime = (int64_t)now;
    v->mtime = (int64_t)now;
    v->ctime = (int64_t)now;
    vfs_vnode_put(v);
    return 0;
}

int64_t sys_fchmodat(int dirfd, const char *upath, uint32_t mode, int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    int err = 0;
    bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0;
    vnode_t *v = vfs_lookup(path, follow, &err);
    if (!v) return err ? err : -ENOENT;

    int owner = (t->cred.fsuid == v->uid);
    if (!owner && !task_has_cap(t, CAP_FOWNER)) {
        vfs_vnode_put(v);
        return -EPERM;
    }

    uint32_t desired_mode = mode & 07777;
    if (!task_has_cap(t, CAP_FSETID))
        desired_mode &= ~(VFS_S_ISUID | VFS_S_ISGID);

    int rc = apply_vnode_mode(v, desired_mode);
    if (rc < 0) {
        vfs_vnode_put(v);
        return rc;
    }
    v->ctime = (int64_t)(sched_get_ticks() / 1000);
    vfs_vnode_put(v);
    return 0;
}

int64_t sys_fchownat(int dirfd, const char *upath, int owner, int group, int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;

    char path[VFS_MOUNT_PATH_MAX];
    int r = resolve_path_at(dirfd, upath, path);
    if (r < 0) return r;

    task_t *t = cur_task();
    if (!t) return -ESRCH;

    int err = 0;
    bool follow = (flags & AT_SYMLINK_NOFOLLOW) == 0;
    vnode_t *v = vfs_lookup(path, follow, &err);
    if (!v) return err ? err : -ENOENT;

    int owner_match = (t->cred.fsuid == v->uid);
    bool change_owner = (owner >= 0 && (uint32_t)owner != v->uid);
    bool change_group = (group >= 0 && (uint32_t)group != v->gid);

    if (change_owner && !task_has_cap(t, CAP_CHOWN)) {
        vfs_vnode_put(v);
        return -EPERM;
    }

    if (change_group && !task_has_cap(t, CAP_CHOWN)) {
        if (!owner_match || !task_in_group(t, (uint32_t)group)) {
            vfs_vnode_put(v);
            return -EPERM;
        }
    }

    int rc = apply_vnode_owner(v, owner, group);
    if (rc < 0) {
        vfs_vnode_put(v);
        return rc;
    }
    v->ctime = (int64_t)(sched_get_ticks() / 1000);
    vfs_vnode_put(v);
    return 0;
}

int64_t sys_chmod(const char *upath, uint32_t mode) {
    return sys_fchmodat(AT_FDCWD, upath, mode, 0);
}

int64_t sys_fchmod(int fd, uint32_t mode) {
    task_t *t = cur_task();
    if (!t) return -ESRCH;
    file_t *f = fd_get(t, fd);
    if (!f || !f->vnode) return -EBADF;
    int owner = (t->cred.fsuid == f->vnode->uid);
    if (!owner && !task_has_cap(t, CAP_FOWNER)) return -EPERM;
    uint32_t desired_mode = mode & 07777;
    if (!task_has_cap(t, CAP_FSETID))
        desired_mode &= ~(VFS_S_ISUID | VFS_S_ISGID);
    int rc = apply_vnode_mode(f->vnode, desired_mode);
    if (rc < 0) return rc;
    f->vnode->ctime = (int64_t)(sched_get_ticks() / 1000);
    return 0;
}

int64_t sys_chown(const char *upath, int owner, int group) {
    return sys_fchownat(AT_FDCWD, upath, owner, group, 0);
}

int64_t sys_lchown(const char *upath, int owner, int group) {
    return sys_fchownat(AT_FDCWD, upath, owner, group, AT_SYMLINK_NOFOLLOW);
}

int64_t sys_fchown(int fd, int owner, int group) {
    task_t *t = cur_task();
    if (!t) return -ESRCH;
    file_t *f = fd_get(t, fd);
    if (!f || !f->vnode) return -EBADF;
    int owner_match = (t->cred.fsuid == f->vnode->uid);
    bool change_owner = (owner >= 0 && (uint32_t)owner != f->vnode->uid);
    bool change_group = (group >= 0 && (uint32_t)group != f->vnode->gid);

    if (change_owner && !task_has_cap(t, CAP_CHOWN)) return -EPERM;
    if (change_group && !task_has_cap(t, CAP_CHOWN)) {
        if (!owner_match || !task_in_group(t, (uint32_t)group))
            return -EPERM;
    }
    int rc = apply_vnode_owner(f->vnode, owner, group);
    if (rc < 0) return rc;
    f->vnode->ctime = (int64_t)(sched_get_ticks() / 1000);
    return 0;
}

/* ── sys_getdents64 ──────────────────────────────────────────────────────── */
int64_t sys_getdents64(int fd, void *dirp, uint64_t count) {
    if (!dirp || count < sizeof(linux_dirent64_t) + 2) return -EINVAL;

    file_t *f = fd_get(cur_task(), fd);
    if (!f) return -EBADF;
    vnode_t *v = f->vnode;
    if (!VFS_S_ISDIR(v->mode)) return -ENOTDIR;
    if (!v->ops->readdir) return -EINVAL;

    int acc = check_vnode_access(cur_task(), v, 4);
    if (acc < 0) return acc;

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

/* ── sys_mount ───────────────────────────────────────────────────────────── */
int64_t sys_mount(const char *source, const char *target, const char *fstype,
                  uint64_t flags, const void *data) {
    (void)flags;

    task_t *t = cur_task();
    if (!t) return -ESRCH;
    if (t->cred.uid != 0) return -EPERM;
    if (!target || !target[0]) return -EINVAL;

    KLOG_INFO("mount: src='%s' tgt='%s' type='%s' flags=0x%llx\n",
              source ? source : "", target,
              (fstype && fstype[0]) ? fstype : "(auto)",
              (unsigned long long)flags);

    char target_path[VFS_MOUNT_PATH_MAX];
    int rc = resolve_path(target, target_path);
    if (rc < 0) {
        KLOG_WARN("mount: resolve target '%s' failed rc=%d\n", target, rc);
        return rc;
    }

    int lerr = 0;
    vnode_t *mnt = vfs_lookup(target_path, true, &lerr);
    if (!mnt) {
        int err = lerr ? lerr : -ENOENT;
        KLOG_WARN("mount: target lookup '%s' failed rc=%d\n", target_path, err);
        return err;
    }
    if (!VFS_S_ISDIR(mnt->mode)) {
        vfs_vnode_put(mnt);
        KLOG_WARN("mount: target '%s' is not a directory\n", target_path);
        return -ENOTDIR;
    }
    vfs_vnode_put(mnt);

    blkdev_t *dev = NULL;
    if (source && source[0]) {
        char src_path[VFS_MOUNT_PATH_MAX];
        const char *src_use = source;

        /* Normalize source path so mount accepts both absolute and relative
         * device paths consistently (e.g. "/dev/vda3", "./dev/vda3"). */
        if (source[0] == '/' || source[0] == '.') {
            int src_rc = resolve_path(source, src_path);
            if (src_rc < 0) {
                KLOG_WARN("mount: resolve source '%s' failed rc=%d\n", source, src_rc);
                return src_rc;
            }
            src_use = src_path;
        }

        const char *name = src_use;
        if (strncmp(name, "/dev/", 5) == 0) {
            int derr = 0;
            vnode_t *dv = vfs_lookup(src_use, true, &derr);
            if (!dv) {
                int err = derr ? derr : -ENOENT;
                KLOG_WARN("mount: source lookup '%s' failed rc=%d\n", src_use, err);
                return err;
            }
            if (!VFS_S_ISBLK(dv->mode)) {
                vfs_vnode_put(dv);
                KLOG_WARN("mount: source '%s' is not a block device\n", src_use);
                return -EINVAL;
            }
            vfs_vnode_put(dv);
            name += 5;
        }

        while (*name == '/') name++;
        if (!name[0]) return -EINVAL;

        char dev_name[32];
        size_t ni = 0;
        while (name[ni] && name[ni] != '/' && ni < sizeof(dev_name) - 1) {
            dev_name[ni] = name[ni];
            ni++;
        }
        dev_name[ni] = '\0';
        if (!dev_name[0]) return -EINVAL;

        dev = blkdev_find_by_name(dev_name);
        if (!dev) {
            KLOG_WARN("mount: block device '%s' not found\n", dev_name);
            return -ENOENT;
        }
    }

    bool auto_fs = (!fstype || !fstype[0] || strcmp(fstype, "auto") == 0);

    if (data) {
        if (!auto_fs && strcmp(fstype, "fat32") == 0)
            fat32_set_mount_opts((const char *)data);
        else if (auto_fs)
            fat32_set_mount_opts((const char *)data);
    }

    if (!auto_fs) {
        rc = vfs_mount(target_path, dev, fstype);
        if (rc < 0)
            KLOG_WARN("mount: vfs_mount(%s,%s,%s) failed rc=%d\n",
                      target_path, source ? source : "", fstype, rc);
        return rc;
    }

    if (dev) {
        static const char *const fs_try[] = {"ext2", "fat32"};
        for (size_t i = 0; i < sizeof(fs_try) / sizeof(fs_try[0]); i++) {
            rc = vfs_mount(target_path, dev, fs_try[i]);
            if (rc == 0) {
                KLOG_INFO("mount: auto-detected fs '%s' for '%s'\n", fs_try[i], source);
                return 0;
            }
        }
        KLOG_WARN("mount: auto-detect failed for '%s'\n", source ? source : "");
        return -EINVAL;
    }

    rc = vfs_mount(target_path, NULL, "tmpfs");
    if (rc < 0)
        KLOG_WARN("mount: tmpfs mount on '%s' failed rc=%d\n", target_path, rc);
    return rc;
}

/* ── sys_brk ─────────────────────────────────────────────────────────────── */
int64_t sys_brk(uint64_t addr) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    /* brk(0) → return current brk */
    if (addr == 0)
        return (int64_t)cur->brk_current;

    /* Page-align the new brk */
    uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t old_brk = cur->brk_current;

    /* Don't allow shrinking below base */
    if (new_brk < cur->brk_base)
        return (int64_t)old_brk;

    /* Sanity limit: 256 MiB heap max */
    if (new_brk - cur->brk_base > (256ULL * 1024 * 1024))
        return (int64_t)old_brk;

    if (new_brk > old_brk) {
        /* Expand: map new pages */
        uint64_t old_page = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint64_t pg = old_page; pg < new_brk; pg += PAGE_SIZE) {
            uintptr_t phys = pmm_alloc_pages(1);
            if (!phys) return (int64_t)old_brk;  /* OOM */
            memset((void *)vmm_phys_to_virt(phys), 0, PAGE_SIZE);
            vmm_map_page_in(cur->cr3, pg, phys, VMM_USER_RW);
        }
    } else if (new_brk < old_brk) {
        /* Shrink: unmap pages */
        uint64_t new_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint64_t pg = new_page; pg < old_brk; pg += PAGE_SIZE) {
            uintptr_t phys = vmm_unmap_page_in(cur->cr3, pg);
            if (phys) pmm_page_unref(phys);
        }
    }

    cur->brk_current = new_brk;

    /* Update or create heap VMA */
    vma_t *v = cur->vma_list;
    while (v) {
        if (v->flags & VMA_HEAP) { v->end = new_brk; break; }
        v = v->next;
    }
    if (!v && new_brk > cur->brk_base) {
        v = kmalloc(sizeof(vma_t));
        if (v) {
            v->start = cur->brk_base;
            v->end   = new_brk;
            v->flags = VMA_READ | VMA_WRITE | VMA_USER | VMA_HEAP;
            v->file = NULL;
            v->file_offset = 0;
            v->file_size = 0;
            v->mmap_flags = 0;
            v->next  = cur->vma_list;
            cur->vma_list = v; /* simple prepend, not sorted */
        }
    }

    return (int64_t)new_brk;
}

/* ── truncate(2) / ftruncate(2) ───────────────────────────────────────────── */
int64_t sys_truncate(const char *path, int64_t length) {
    if (!path) return -EFAULT;
    if (length < 0) return -EINVAL;
    char resolved[VFS_MOUNT_PATH_MAX];
    if (resolve_path(path, resolved) < 0) return -ENOENT;
    int r = vfs_truncate(resolved, (uint64_t)length);
    return r < 0 ? r : 0;
}

int64_t sys_ftruncate(int fd, int64_t length) {
    if (length < 0) return -EINVAL;
    task_t *t = sched_current(); if (!t) return -ESRCH;
    if (fd < 0 || fd >= FD_TABLE_SIZE || !t->fd_table[fd]) return -EBADF;
    file_t *f = t->fd_table[fd];
    if (!f->vnode || !f->vnode->ops || !f->vnode->ops->truncate) return -EINVAL;
    f->vnode->ops->truncate(f->vnode, (uint64_t)length);
    return 0;
}

/* ── link(2) ──────────────────────────────────────────────────────────────── */
int64_t sys_link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -EPERM;  /* hard links not implemented */
}

/* ── mknod(2) ─────────────────────────────────────────────────────────────── */
int64_t sys_mknod(const char *path, uint32_t mode, uint64_t dev) {
    (void)dev;
    if (!path) return -EFAULT;
    /* Only support creation of regular files via mknod */
    if ((mode & 0170000) != 0100000) return -EPERM;
    int fd = (int)sys_open(path, SYS_O_CREAT | SYS_O_TRUNC | SYS_O_WRONLY, mode & 07777);
    if (fd < 0) return fd;
    return sys_close(fd);
}

/* ── statfs(2) / fstatfs(2) ───────────────────────────────────────────────── */
static void fill_statfs(linux_statfs_t *st, uint64_t total_kb, uint64_t free_kb,
                        long fstype) {
    st->f_type    = fstype;
    st->f_bsize   = 4096;
    st->f_blocks  = total_kb / 4;
    st->f_bfree   = free_kb / 4;
    st->f_bavail  = free_kb / 4;
    st->f_files   = 65536;
    st->f_ffree   = 32768;
    st->f_fsid[0] = st->f_fsid[1] = 0;
    st->f_namelen = 255;
    st->f_frsize  = 4096;
    st->f_flags   = 4096; /* ST_RELATIME */
}

int64_t sys_statfs(const char *path, linux_statfs_t *buf) {
    if (!path || !buf) return -EFAULT;
    extern uint64_t pmm_get_total_pages(void);
    extern uint64_t pmm_get_free_pages(void);
    uint64_t total_kb = (pmm_get_total_pages() * 4096) / 1024;
    uint64_t free_kb  = (pmm_get_free_pages()  * 4096) / 1024;
    fill_statfs(buf, total_kb, free_kb, 0xEF53L /* EXT2_SUPER_MAGIC */);
    return 0;
}

int64_t sys_fstatfs(int fd, linux_statfs_t *buf) {
    (void)fd;
    if (!buf) return -EFAULT;
    extern uint64_t pmm_get_total_pages(void);
    extern uint64_t pmm_get_free_pages(void);
    uint64_t total_kb = (pmm_get_total_pages() * 4096) / 1024;
    uint64_t free_kb  = (pmm_get_free_pages()  * 4096) / 1024;
    fill_statfs(buf, total_kb, free_kb, 0xEF53L);
    return 0;
}

/* ── flock(2) ─────────────────────────────────────────────────────────────── */
int64_t sys_flock(int fd, int operation) {
    (void)fd; (void)operation;
    return 0;  /* advisory locks: always succeed */
}

/* ── fallocate(2) ─────────────────────────────────────────────────────────── */
int64_t sys_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    (void)mode; (void)offset; (void)len;
    task_t *t = sched_current(); if (!t) return -ESRCH;
    if (fd < 0 || fd >= FD_TABLE_SIZE || !t->fd_table[fd]) return -EBADF;
    return 0;
}

/* ── madvise(2) ───────────────────────────────────────────────────────────── */
int64_t sys_madvise(void *addr, uint64_t length, int advice) {
    (void)addr; (void)length; (void)advice;
    return 0;
}

/* ── memfd_create(2) ──────────────────────────────────────────────────────── */
int64_t sys_memfd_create(const char *name, unsigned int flags) {
    (void)name; (void)flags;
    /* Create an anonymous tmpfs file */
    task_t *t = sched_current(); if (!t) return -ESRCH;
    return sys_open("/tmp/.memfd", 0100 | 02 | 01000, 0600); /* O_CREAT|O_RDWR|O_TRUNC */
}

/* ── inotify_init1(2) — stub ─────────────────────────────────────────────── */
int64_t sys_inotify_init1(int flags) {
    (void)flags;
    return -ENOSYS;
}

/* ── select(2) / pselect6(2) ────────────────────────────────────────────────
 * Convert fd_set bitmasks → struct pollfd[] → call sys_poll() → write back.
 * fd_set is 128 bytes (1024 bits) on Linux x86-64.                         */

int64_t sys_poll(struct pollfd *fds, uint64_t nfds, int timeout); /* forward */

typedef struct { uint64_t fds_bits[16]; } linux_fd_set_t; /* 1024 bits */

#define FD_ISSET_L(fd, setp) \
    (((setp)->fds_bits[(fd) >> 6] >> ((fd) & 63)) & 1)
#define FD_SET_L(fd, setp) \
    ((setp)->fds_bits[(fd) >> 6] |= (uint64_t)1 << ((fd) & 63))

int64_t sys_select(int nfds, linux_fd_set_t *readfds, linux_fd_set_t *writefds,
                   linux_fd_set_t *exceptfds, void *timeout) {
    if (nfds < 0 || nfds > 1024) return -EINVAL;

    /* Build a temporary pollfd array on the stack for small nfds,
     * or allocate from heap for larger sets.                       */
    struct pollfd *pfds = NULL;
    int npfds = 0;
    struct pollfd stack_pfds[64];
    bool heap = (nfds > 64);
    if (heap) {
        extern void *kmalloc(uint64_t size);
        pfds = (struct pollfd *)kmalloc((uint64_t)nfds * sizeof(struct pollfd));
        if (!pfds) return -ENOMEM;
    } else {
        pfds = stack_pfds;
    }

    /* Fill pollfd[] from the three fd_sets */
    for (int fd = 0; fd < nfds; fd++) {
        int events = 0;
        if (readfds   && FD_ISSET_L(fd, readfds))   events |= 1 | 0x40; /* POLLIN|POLLRDNORM */
        if (writefds  && FD_ISSET_L(fd, writefds))  events |= 4 | 0x100;/* POLLOUT|POLLWRNORM */
        if (exceptfds && FD_ISSET_L(fd, exceptfds)) events |= 2;        /* POLLPRI */
        if (!events) continue;
        pfds[npfds].fd      = fd;
        pfds[npfds].events  = (short)events;
        pfds[npfds].revents = 0;
        npfds++;
    }

    /* Compute timeout_ms: NULL → infinite (-1), {0,0} → 0 (poll) */
    int timeout_ms = -1;
    if (timeout) {
        /* struct timeval: 8 bytes tv_sec + 8 bytes tv_usec on LP64 */
        int64_t *tv = (int64_t *)timeout;
        int64_t ms  = tv[0] * 1000 + tv[1] / 1000;
        timeout_ms  = (ms > 0x7fffffff) ? 0x7fffffff : (int)ms;
    }

    int64_t ret = sys_poll(pfds, (uint64_t)npfds, timeout_ms);

    if (ret >= 0) {
        /* Zero the output fd_sets before writing results back */
        if (readfds)   for (int i = 0; i < 16; i++) readfds->fds_bits[i]   = 0;
        if (writefds)  for (int i = 0; i < 16; i++) writefds->fds_bits[i]  = 0;
        if (exceptfds) for (int i = 0; i < 16; i++) exceptfds->fds_bits[i] = 0;

        int ready = 0;
        for (int i = 0; i < npfds; i++) {
            int fd = pfds[i].fd;
            int rev = pfds[i].revents;
            if (!rev) continue;
            if (readfds   && (rev & (1 | 0x40)))  { FD_SET_L(fd, readfds);   ready++; }
            if (writefds  && (rev & (4 | 0x100))) { FD_SET_L(fd, writefds);  ready++; }
            if (exceptfds && (rev & 2))            { FD_SET_L(fd, exceptfds); ready++; }
        }
        ret = ready;
    }

    if (heap) {
        extern void kfree(void *ptr);
        kfree(pfds);
    }
    return ret;
}

/* pselect6: like select but with nanosecond timespec and optional sigmask */
int64_t sys_pselect6(int nfds, linux_fd_set_t *readfds, linux_fd_set_t *writefds,
                     linux_fd_set_t *exceptfds, void *timeout_ts, void *sigmask_arg) {
    (void)sigmask_arg; /* signal mask handling not fully supported yet */

    /* Convert timespec → timeval-compatible buffer for sys_select */
    int64_t tv[2] = {0, 0};
    void *tv_ptr = NULL;
    if (timeout_ts) {
        /* struct timespec: 8-byte tv_sec + 8-byte tv_nsec */
        int64_t *ts = (int64_t *)timeout_ts;
        tv[0] = ts[0];           /* seconds */
        tv[1] = ts[1] / 1000;   /* nanoseconds → microseconds */
        tv_ptr = tv;
    }
    return sys_select(nfds, readfds, writefds, exceptfds, tv_ptr);
}
