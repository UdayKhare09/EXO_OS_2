/* fs/vfs.h — Virtual File System layer
 *
 * Linux-like VFS: every mounted filesystem provides a fs_ops_t vtable.
 * Callers interact with vnode_t objects through generic vfs_* functions.
 *
 * Ownership / lifecycle:
 *   - vfs_lookup() returns a vnode_t* with refcount bumped by 1.
 *   - Caller MUST call vfs_vnode_put() when done with the vnode.
 *   - vfs_open() additionally calls fs_ops.open(); vfs_close() calls fs_ops.close().
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "drivers/storage/blkdev.h"

/* ssize_t is POSIX; not provided by <stddef.h> in freestanding builds */
#ifndef __ssize_t_defined
typedef int64_t ssize_t;
#define __ssize_t_defined 1
#endif

/* ── Forward declarations ────────────────────────────────────────────────── */
typedef struct vnode    vnode_t;
typedef struct fs_ops   fs_ops_t;
typedef struct mount    mount_t;
typedef struct fs_inst  fs_inst_t;
typedef struct file     file_t;
typedef struct file_ops file_ops_t;  /* defined in fs/fd.h */

/* ── File type bits (stored in vnode.mode) ───────────────────────────────── */
#define VFS_S_IFMT    0xF000
#define VFS_S_IFREG   0x8000   /* regular file                              */
#define VFS_S_IFDIR   0x4000   /* directory                                 */
#define VFS_S_IFLNK   0xA000   /* symbolic link                             */
#define VFS_S_IFBLK   0x6000   /* block device                              */
#define VFS_S_IFCHR   0x2000   /* character device                          */
#define VFS_S_IFIFO   0x1000   /* FIFO / pipe                               */

#define VFS_S_ISREG(m)  (((m) & VFS_S_IFMT) == VFS_S_IFREG)
#define VFS_S_ISDIR(m)  (((m) & VFS_S_IFMT) == VFS_S_IFDIR)
#define VFS_S_ISLNK(m)  (((m) & VFS_S_IFMT) == VFS_S_IFLNK)
#define VFS_S_ISBLK(m)  (((m) & VFS_S_IFMT) == VFS_S_IFBLK)

/* Permission bits */
#define VFS_S_IRWXU 0700
#define VFS_S_IRUSR 0400
#define VFS_S_IWUSR 0200
#define VFS_S_IXUSR 0100
#define VFS_S_IRWXG 0070
#define VFS_S_IRGRP 0040
#define VFS_S_IWGRP 0020
#define VFS_S_IXGRP 0010
#define VFS_S_IRWXO 0007
#define VFS_S_IROTH 0004
#define VFS_S_IWOTH 0002
#define VFS_S_IXOTH 0001
#define VFS_S_ISUID 04000
#define VFS_S_ISGID 02000
#define VFS_S_ISVTX 01000

/* ── POSIX stat structure ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t dev;        /* device ID of containing fs (blkdev dev_id)       */
    uint64_t ino;        /* inode number                                      */
    uint32_t mode;       /* file type + permission bits                       */
    uint32_t nlink;      /* number of hard links                              */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;       /* total size in bytes (for regular files)           */
    uint64_t blocks;     /* number of 512-byte blocks allocated               */
    uint32_t blksize;    /* preferred I/O block size                          */
    uint64_t rdev;       /* device ID (for char/block device files)           */
    int64_t  atime;      /* last access time (seconds since epoch)            */
    int64_t  mtime;      /* last modification time                            */
    int64_t  ctime;      /* last status-change time                           */
} vfs_stat_t;

/* ── Directory entry (for readdir) ──────────────────────────────────────── */
#define VFS_NAME_MAX 255

typedef struct {
    uint64_t ino;
    char     name[VFS_NAME_MAX + 1];
    uint32_t type;  /* DT_REG=8, DT_DIR=4, DT_LNK=10, DT_UNKNOWN=0         */
} vfs_dirent_t;

#define VFS_DT_UNKNOWN 0
#define VFS_DT_FIFO    1
#define VFS_DT_CHR     2
#define VFS_DT_DIR     4
#define VFS_DT_BLK     6
#define VFS_DT_REG     8
#define VFS_DT_LNK     10

/* ── Filesystem operations vtable ───────────────────────────────────────── */
struct fs_ops {
    const char *name;   /* filesystem type name, e.g. "fat32", "ext2"        */

    /* Look up a child of `dir` by `name`. Returns new vnode_t* (refcount+1)
     * or NULL if not found / error. */
    vnode_t *(*lookup)(vnode_t *dir, const char *name);

    /* Open a vnode (called after lookup on open path). Returns 0 or errno. */
    int (*open)(vnode_t *v, int flags);

    /* Close a vnode. Called when last file_t referencing it is closed. */
    int (*close)(vnode_t *v);

    /* Read up to `len` bytes from `v` at offset `off` into `buf`.
     * Returns bytes read, 0 for EOF, negative errno on error. */
    ssize_t (*read)(vnode_t *v, void *buf, size_t len, uint64_t off);

    /* Write up to `len` bytes from `buf` to `v` at offset `off`.
     * Returns bytes written or negative errno. */
    ssize_t (*write)(vnode_t *v, const void *buf, size_t len, uint64_t off);

    /* Iterate directory entries. `cookie` starts at 0; advance it per call.
     * Fills *out and returns 1; returns 0 when exhausted, -1 on error. */
    int (*readdir)(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out);

    /* Create a new regular file named `name` in dir `parent`. */
    vnode_t *(*create)(vnode_t *parent, const char *name, uint32_t mode);

    /* Create a directory named `name` inside `parent`. */
    vnode_t *(*mkdir)(vnode_t *parent, const char *name, uint32_t mode);

    /* Remove a file named `name` from `parent`. */
    int (*unlink)(vnode_t *parent, const char *name);

    /* Remove an empty directory named `name` from `parent`. */
    int (*rmdir)(vnode_t *parent, const char *name);

    /* Rename / move `old_name` in `old_dir` to `new_name` in `new_dir`. */
    int (*rename)(vnode_t *old_dir, const char *old_name,
                  vnode_t *new_dir, const char *new_name);

    /* Fill in *st for vnode `v`. */
    int (*stat)(vnode_t *v, vfs_stat_t *st);

    /* Create a symlink: new entry at `parent/name` pointing to `target`. */
    vnode_t *(*symlink)(vnode_t *parent, const char *name, const char *target);

    /* Read symlink target into buf (NUL-terminated). Returns 0 or errno. */
    int (*readlink)(vnode_t *v, char *buf, size_t bufsize);

    /* Truncate file to `size` bytes. */
    int (*truncate)(vnode_t *v, uint64_t size);

    /* Change permission bits (07777 mask) while preserving file type bits. */
    int (*chmod)(vnode_t *v, uint32_t mode);

    /* Change ownership; owner/group < 0 means unchanged. */
    int (*chown)(vnode_t *v, int owner, int group);

    /* Flush dirty state of `v` to underlying storage. */
    int (*sync)(vnode_t *v);

    /* Called by VFS when vnode refcount drops to 0 (may free fs-private data). */
    void (*evict)(vnode_t *v);

    /* Mount: given a block device (may be NULL for pseudo-fs), initialise the
     * filesystem and return the root vnode. Returns NULL on error. */
    vnode_t *(*mount)(fs_inst_t *fsi, blkdev_t *dev);

    /* Unmount: flush and free all resources. */
    void (*unmount)(fs_inst_t *fsi);
};

/* ── In-kernel vnode (inode equivalent) ─────────────────────────────────── */
struct vnode {
    uint64_t   ino;       /* filesystem-specific inode number                 */
    uint32_t   mode;      /* type + permissions                               */
    uint64_t   size;      /* file size in bytes                               */
    uint32_t   uid, gid;
    int64_t    atime, mtime, ctime; /* epoch seconds                         */
    uint32_t   nlink;

    fs_ops_t  *ops;       /* filesystem vtable                                */
    fs_inst_t *fsi;       /* filesystem instance this vnode belongs to        */
    mount_t   *mountpoint;/* if another FS is mounted here, non-NULL          */
    void      *fs_data;   /* filesystem-private data                          */

    uint32_t   refcount;  /* number of active references                      */

    /* ── Open-time file_ops injection (set by fs open() callback) ──────────
     * When a filesystem's open() sets these (e.g. /dev/ptmx allocates a PTY
     * master), vfs_open() transfers them into the returned file_t, exactly
     * like Linux's fops->open() filling filp->private_data.               */
    file_ops_t *pending_f_ops;
    void       *pending_priv;
};

/* ── Filesystem instance (one per mounted volume) ───────────────────────── */
struct fs_inst {
    blkdev_t  *dev;       /* underlying block device (NULL for pseudo-fs)     */
    vnode_t   *root;      /* root vnode                                       */
    fs_ops_t  *ops;       /* filesystem vtable                                */
    void      *priv;      /* fs-private superblock data                        */
    mount_t   *mount;     /* back-pointer to mount entry                      */
    uint64_t   dev_id;    /* unique device ID exposed via stat(2) st_dev      */
};

/* ── Mount table entry ───────────────────────────────────────────────────── */
#define VFS_MOUNT_PATH_MAX 512
#define VFS_MAX_MOUNTS     32

struct mount {
    char       path[VFS_MOUNT_PATH_MAX];  /* absolute mount point            */
    vnode_t   *root;                      /* root vnode of mounted FS        */
    vnode_t   *covered;                   /* vnode in parent FS that this covers */
    fs_inst_t *fsi;
    bool       active;
};

typedef struct {
    char path[VFS_MOUNT_PATH_MAX];
    char fs_name[16];
    char dev_name[32];
} vfs_mount_info_t;

/* ── Registered filesystem types ─────────────────────────────────────────── */
#define VFS_MAX_FS_TYPES  8

/* ── Open flags (mirroring Linux, passed to sys_open / vfs_open) ─────────── */
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_ACCMODE    3
#define O_CREAT    (1 << 6)
#define O_EXCL     (1 << 7)
#define O_TRUNC    (1 << 9)
#define O_APPEND   (1 << 10)
#define O_NONBLOCK (1 << 11)
#define O_DIRECTORY (1 << 16)
#define O_NOFOLLOW  (1 << 17)
#define O_CLOEXEC   (1 << 19)

/* ── Error codes (Linux-compatible, matching kernel UAPI) ────────────────── */
#define EPERM     1   /* Operation not permitted                              */
#define ENOENT    2   /* No such file or directory                            */
#define ESRCH     3   /* No such process                                      */
#define EINTR     4   /* Interrupted system call                              */
#define EIO       5   /* I/O error                                            */
#define ENXIO     6   /* No such device or address                            */
#define E2BIG     7   /* Argument list too long                               */
#define ENOEXEC   8   /* Exec format error                                    */
#define EBADF     9   /* Bad file number                                      */
#define ECHILD   10   /* No child processes                                   */
#define EAGAIN   11   /* Try again / EWOULDBLOCK                              */
#define ENOMEM   12   /* Out of memory                                        */
#define EACCES   13   /* Permission denied                                    */
#define EFAULT   14   /* Bad address                                          */
#define EBUSY    16   /* Device or resource busy                              */
#define EEXIST   17   /* File exists                                          */
#define EXDEV    18   /* Cross-device link                                    */
#define ENODEV   19   /* No such device                                       */
#define ENOTDIR  20   /* Not a directory                                      */
#define EISDIR   21   /* Is a directory                                       */
#define EINVAL   22   /* Invalid argument                                     */
#define ENFILE   23   /* File table overflow                                  */
#define EMFILE   24   /* Too many open files                                  */
#define EFBIG    27   /* File too large                                       */
#define ENOSPC   28   /* No space left on device                              */
#define ESPIPE   29   /* Illegal seek                                         */
#define EROFS    30   /* Read-only file system                                */
#define EPIPE    32   /* Broken pipe                                          */
#define ERANGE   34   /* Math result not representable                        */
#define EDEADLK  35   /* Resource deadlock would occur                        */
#define ENAMETOOLONG 36 /* File name too long                                 */
#define ENOSYS   38   /* Function not implemented                             */
#define ENOTEMPTY 39  /* Directory not empty                                  */
#define ELOOP    40   /* Too many symbolic links encountered                  */
#define EWOULDBLOCK EAGAIN /* Operation would block                           */
#define ENOMSG   42   /* No message of desired type                           */
#define ENODATA  61   /* No data available                                    */
#define EOVERFLOW 75  /* Value too large for defined data type                */
#define ENOTSOCK  88  /* Socket operation on non-socket                       */
#define EDESTADDRREQ 89 /* Destination address required                       */
#define EMSGSIZE 90   /* Message too long                                     */
#define EPROTOTYPE 91 /* Protocol wrong type for socket                       */
#define ENOPROTOOPT 92 /* Protocol not available                              */
#define EPROTONOSUPPORT 93 /* Protocol not supported                          */
#define EOPNOTSUPP 95 /* Operation not supported on transport endpoint        */
#define EAFNOSUPPORT 97 /* Address family not supported by protocol           */
#define EADDRINUSE 98 /* Address already in use                               */
#define EADDRNOTAVAIL 99 /* Cannot assign requested address                   */
#define ENETDOWN  100 /* Network is down                                      */
#define ENETUNREACH 101 /* Network is unreachable                             */
#define ECONNABORTED 103 /* Software caused connection abort                  */
#define ECONNRESET 104 /* Connection reset by peer                            */
#define ENOBUFS  105  /* No buffer space available                            */
#define EISCONN  106  /* Transport endpoint is already connected              */
#define ENOTCONN 107  /* Transport endpoint is not connected                  */
#define ETIMEDOUT 110 /* Connection timed out                                 */
#define ECONNREFUSED 111 /* Connection refused                                */
#define EINPROGRESS 115 /* Operation now in progress                          */
#define EEALREADY 37  /* alias for EALREADY */
#define EALREADY  37  /* Operation already in progress                        */

/* ── VFS global API ──────────────────────────────────────────────────────── */

/* Initialise the VFS (call once after kmalloc_init). */
void vfs_init(void);

/* Register a filesystem type so it can be used with vfs_mount(). */
int vfs_register_fs(fs_ops_t *ops);

/* Mount `fstype` filesystem on `blkdev` at `path`.
 * `path` must be an absolute path (e.g. "/", "/mnt/usb").
 * For pseudo-fs (tmpfs), pass dev=NULL.
 * Returns 0 on success, negative errno on failure. */
int vfs_mount(const char *path, blkdev_t *dev, const char *fstype);

/* Unmount the filesystem mounted at `path`. Returns 0 or errno. */
int vfs_unmount(const char *path);

/* Get the root vnode of the root filesystem (refcount NOT incremented). */
vnode_t *vfs_get_root(void);

/* Resolve an absolute path to a vnode (refcount+1).
 * If `follow_last_link` is true, symlinks at the final component are followed.
 * Returns NULL and sets *err_out (if non-NULL) to a negative errno. */
vnode_t *vfs_lookup(const char *path, bool follow_last_link, int *err_out);

/* Increment vnode refcount. */
void vfs_vnode_get(vnode_t *v);

/* Decrement vnode refcount; calls ops->evict() if it hits 0. */
void vfs_vnode_put(vnode_t *v);

/* Stat a path. Returns 0 or negative errno. */
int vfs_stat(const char *path, vfs_stat_t *st);

/* Stat without following symlinks at final component. */
int vfs_lstat(const char *path, vfs_stat_t *st);

/* Create a directory at `path` with given `mode`. Returns 0 or errno. */
int vfs_mkdir(const char *path, uint32_t mode);

/* Remove a file at `path`. Returns 0 or errno. */
int vfs_unlink(const char *path);

/* Remove an empty directory at `path`. Returns 0 or errno. */
int vfs_rmdir(const char *path);

/* Rename/move. Returns 0 or errno. */
int vfs_rename(const char *old_path, const char *new_path);

/* Create a symlink at `link_path` pointing to `target`. */
int vfs_symlink(const char *target, const char *link_path);

/* Read a symlink. Returns 0 or errno. */
int vfs_readlink(const char *path, char *buf, size_t len);

/* Truncate a file at `path` to `size`. Returns 0 or errno. */
int vfs_truncate(const char *path, uint64_t size);

/* Sync all mounted filesystems. */
void vfs_sync_all(void);

/* Enumerate active mounts into buf. Returns number of mounts written.
 * If max_count is 0, returns the total number of active mounts. */
int vfs_snapshot_mounts(vfs_mount_info_t *buf, int max_count);

/* Allocate a new vnode (zero-initialised). Returns NULL on OOM. */
vnode_t *vfs_alloc_vnode(void);

/* Open a path and create a file_t wrapper around the resulting vnode.
 * Returns NULL on failure.
 * If O_CREAT is set and the path does not exist, creates the file using `mode`.
 */
file_t *vfs_open(const char *path, int flags, uint32_t mode);

/* ── Path utilities (implemented in Rust path.rs) ────────────────────────── */

/* Normalise absolute path (collapse ".", "..", "//"). Returns 0 or errno.
 * `out` must be at least VFS_MOUNT_PATH_MAX bytes. */
int  path_normalize(const char *in, char *out);

/* Join `base` (absolute) and `rel` into normalised absolute path. */
int  path_join(const char *base, const char *rel, char *out);

/* Extract directory component into `out`. */
void path_dirname(const char *path, char *out, size_t out_sz);

/* Extract basename (last component) into `out`. */
void path_basename(const char *path, char *out, size_t out_sz);
