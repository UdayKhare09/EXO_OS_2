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
#include "mm/vmm.h"
#include "mm/pmm.h"

#include <stdint.h>
#include <stddef.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */
static void fill_linux_stat(linux_stat_t *lst, const vfs_stat_t *st);
int64_t sys_read(int fd, void *buf, uint64_t count);
int64_t sys_fstat(int fd, linux_stat_t *buf);
int64_t sys_fstatat(int dirfd, const char *upath, linux_stat_t *buf, int flags);
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

static bool task_in_group(const task_t *t, uint32_t gid) {
    if (!t) return false;
    if (t->gid == gid) return true;
    uint32_t limit = t->group_count;
    if (limit > TASK_MAX_GROUPS) limit = TASK_MAX_GROUPS;
    for (uint32_t i = 0; i < limit; i++) {
        if (t->groups[i] == gid)
            return true;
    }
    return false;
}

/* req_mode bits: 4=read, 2=write, 1=execute/search */
static int check_vnode_access(task_t *t, const vnode_t *v, int req_mode) {
    if (!t || !v) return -EINVAL;
    if (t->uid == 0) return 0;

    uint32_t class_perm;
    if (t->uid == v->uid)
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

static int apply_vnode_mode(vnode_t *v, uint32_t mode) {
    if (!v) return -EINVAL;
    if (v->ops && v->ops->chmod)
        return v->ops->chmod(v, mode);
    v->mode = (v->mode & VFS_S_IFMT) | (mode & 0777);
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
            apply_vnode_owner(v, (int)t->uid, (int)t->gid);
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

    /* AT_EMPTY_PATH: stat the fd itself (used for fstat via fstatat) */
    #define AT_EMPTY_PATH 0x1000
    if (flags & AT_EMPTY_PATH) {
        return sys_fstat(dirfd, buf);
    }

    if (flags & ~(AT_SYMLINK_NOFOLLOW)) return -EINVAL;

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
        apply_vnode_owner(v, (int)t->uid, (int)t->gid);
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

    if (t->uid == 0) return 0;

    uint32_t class_perm;
    if (t->uid == st.st_uid)
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
        if (acc < 0 && t->uid != f->vnode->uid) return -EACCES;

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
    if (acc < 0 && t->uid != v->uid) {
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

    if (t->uid != 0 && t->uid != v->uid) {
        vfs_vnode_put(v);
        return -EPERM;
    }

    int rc = apply_vnode_mode(v, mode);
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

    if (t->uid != 0) {
        vfs_vnode_put(v);
        return -EPERM;
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
    if (t->uid != 0 && t->uid != f->vnode->uid) return -EPERM;
    int rc = apply_vnode_mode(f->vnode, mode);
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
    if (t->uid != 0) return -EPERM;
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
    (void)data;

    task_t *t = cur_task();
    if (!t) return -ESRCH;
    if (t->uid != 0) return -EPERM;
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
