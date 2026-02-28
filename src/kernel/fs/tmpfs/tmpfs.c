/* fs/tmpfs/tmpfs.c — kmalloc-backed in-memory temporary filesystem
 *
 * Each inode keeps its own data/dirent/symlink storage in kmalloc'd memory.
 * tmpfs instances are independent (one per mount point).
 * Supports: files (read/write/truncate), directories (create/mkdir/unlink/readdir),
 *           symlinks, rename.  No persistence across reboots.
 */
#include "tmpfs.h"
#include "fs/vfs.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Private inode ───────────────────────────────────────────────────────── */
struct tmpfs_inode;

typedef struct tmpfs_dirent {
    char               name[VFS_NAME_MAX + 1];
    struct tmpfs_inode *inode;
    struct tmpfs_dirent *next;
} tmpfs_dirent_t;

typedef struct tmpfs_inode {
    uint32_t   ino;
    uint32_t   mode;
    uint32_t   uid, gid;
    uint32_t   atime, mtime, ctime;
    uint32_t   nlink;
    uint32_t   refcount;       /* number of vnodes + dir entries pointing here */
    /* File data */
    uint8_t   *data;
    size_t     size;
    size_t     capacity;
    /* Directory entries (only when IFDIR) */
    tmpfs_dirent_t *dir_head;
    /* Symlink target (only when IFLNK) */
    char      *link_target;
} tmpfs_inode_t;

/* ── Filesystem instance ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t next_ino;
} tmpfs_fs_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static tmpfs_inode_t *tmpfs_inode_alloc(tmpfs_fs_t *fs, uint32_t mode) {
    tmpfs_inode_t *ti = kzalloc(sizeof(tmpfs_inode_t));
    if (!ti) return NULL;
    ti->ino      = fs->next_ino++;
    ti->mode     = mode;
    ti->nlink    = 1;
    ti->refcount = 1;
    return ti;
}

static void tmpfs_inode_ref(tmpfs_inode_t *ti)  { ti->refcount++; }
static void tmpfs_inode_unref(tmpfs_inode_t *ti) {
    if (!ti) return;
    if (--ti->refcount == 0) {
        if (ti->data)        kfree(ti->data);
        if (ti->link_target) kfree(ti->link_target);
        /* Free all dir entries */
        tmpfs_dirent_t *d = ti->dir_head;
        while (d) {
            tmpfs_dirent_t *next = d->next;
            tmpfs_inode_unref(d->inode);
            kfree(d);
            d = next;
        }
        kfree(ti);
    }
}

static inline tmpfs_inode_t *vnode_ti(vnode_t *v) {
    return (tmpfs_inode_t *)v->fs_data;
}

/* ── Ensure file buffer can hold `need` bytes ─────────────────────────────── */
static int ensure_capacity(tmpfs_inode_t *ti, size_t need) {
    if (need <= ti->capacity) return 0;
    size_t newcap = ti->capacity ? ti->capacity * 2 : 256;
    while (newcap < need) newcap *= 2;
    uint8_t *nb = kzalloc(newcap);
    if (!nb) return -ENOMEM;
    if (ti->data) {
        memcpy(nb, ti->data, ti->size);
        kfree(ti->data);
    }
    ti->data     = nb;
    ti->capacity = newcap;
    return 0;
}

/* ── Build vnode from inode ──────────────────────────────────────────────── */
static vnode_t *make_vnode_from_ti(fs_inst_t *fsi, tmpfs_inode_t *ti) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;
    tmpfs_inode_ref(ti);
    v->ino      = ti->ino;
    v->mode     = ti->mode;
    v->size     = ti->size;
    v->uid      = ti->uid;
    v->gid      = ti->gid;
    v->nlink    = ti->nlink;
    v->atime    = ti->atime;
    v->mtime    = ti->mtime;
    v->ctime    = ti->ctime;
    v->ops      = &g_tmpfs_ops;
    v->fsi      = fsi;
    v->fs_data  = ti;
    v->refcount = 1;
    return v;
}

/* ── Directory helpers ───────────────────────────────────────────────────── */
static tmpfs_inode_t *dir_lookup(tmpfs_inode_t *dir, const char *name) {
    for (tmpfs_dirent_t *d = dir->dir_head; d; d = d->next) {
        if (strcmp(d->name, name) == 0) return d->inode;
    }
    return NULL;
}

static int dir_add(tmpfs_inode_t *dir, const char *name, tmpfs_inode_t *child) {
    tmpfs_dirent_t *de = kzalloc(sizeof(tmpfs_dirent_t));
    if (!de) return -ENOMEM;
    strncpy(de->name, name, VFS_NAME_MAX);
    de->inode = child;
    tmpfs_inode_ref(child);
    de->next = dir->dir_head;
    dir->dir_head = de;
    return 0;
}

static int dir_remove(tmpfs_inode_t *dir, const char *name) {
    tmpfs_dirent_t **pp = &dir->dir_head;
    while (*pp) {
        tmpfs_dirent_t *d = *pp;
        if (strcmp(d->name, name) == 0) {
            *pp = d->next;
            tmpfs_inode_unref(d->inode);
            kfree(d);
            return 0;
        }
        pp = &d->next;
    }
    return -ENOENT;
}

/* ── fs_ops implementations ──────────────────────────────────────────────── */
static vnode_t *tmpfs_lookup(vnode_t *dir, const char *name) {
    tmpfs_inode_t *dti = vnode_ti(dir);
    tmpfs_inode_t *child = dir_lookup(dti, name);
    if (!child) return NULL;
    return make_vnode_from_ti(dir->fsi, child);
}

static int tmpfs_open(vnode_t *v, int flags)  { (void)v; (void)flags; return 0; }
static int tmpfs_close(vnode_t *v)            { (void)v; return 0; }

static ssize_t tmpfs_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    tmpfs_inode_t *ti = vnode_ti(v);
    if (off >= ti->size) return 0;
    if (off + len > ti->size) len = (size_t)(ti->size - off);
    memcpy(buf, ti->data + off, len);
    return (ssize_t)len;
}

static ssize_t tmpfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    if (len == 0) return 0;
    tmpfs_inode_t *ti = vnode_ti(v);
    size_t end = (size_t)(off + len);
    if (ensure_capacity(ti, end) < 0) return -ENOMEM;
    /* Zero any gap between old size and write start */
    if (off > ti->size) memset(ti->data + ti->size, 0, (size_t)off - ti->size);
    memcpy(ti->data + off, buf, len);
    if (end > ti->size) ti->size = end;
    v->size = ti->size;
    return (ssize_t)len;
}

static int tmpfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    tmpfs_inode_t *dti = vnode_ti(dir);
    /* cookie = 0          → start from dir_head
     * cookie = ~0ULL      → end-of-list sentinel (prevents NULL-pointer restart)
     * cookie = <ptr>      → resume from this tmpfs_dirent_t *
     */
#define TMPFS_COOKIE_EOF (~(uint64_t)0)
    tmpfs_dirent_t *cur;
    if (*cookie == 0) {
        cur = dti->dir_head;
    } else if (*cookie == TMPFS_COOKIE_EOF) {
        return 0;
    } else {
        cur = (tmpfs_dirent_t *)(uintptr_t)*cookie;
    }
    if (!cur) return 0;
    out->ino = cur->inode->ino;
    strncpy(out->name, cur->name, VFS_NAME_MAX);
    uint32_t ifmt = cur->inode->mode & VFS_S_IFMT;
    out->type = (ifmt == VFS_S_IFDIR) ? VFS_DT_DIR :
                (ifmt == VFS_S_IFLNK) ? VFS_DT_LNK : VFS_DT_REG;
    *cookie = cur->next ? (uint64_t)(uintptr_t)cur->next : TMPFS_COOKIE_EOF;
    return 1;
}

static vnode_t *tmpfs_create(vnode_t *parent, const char *name, uint32_t mode) {
    tmpfs_fs_t    *fs  = (tmpfs_fs_t *)parent->fsi->priv;
    tmpfs_inode_t *dti = vnode_ti(parent);
    if (dir_lookup(dti, name)) return NULL; /* already exists */

    tmpfs_inode_t *ti = tmpfs_inode_alloc(fs, VFS_S_IFREG | (mode & 0xFFF));
    if (!ti) return NULL;

    if (dir_add(dti, name, ti) < 0) { tmpfs_inode_unref(ti); return NULL; }
    vnode_t *v = make_vnode_from_ti(parent->fsi, ti);
    tmpfs_inode_unref(ti); /* dir_add refs it; alloc gave us one extra */
    return v;
}

static vnode_t *tmpfs_mkdir(vnode_t *parent, const char *name, uint32_t mode) {
    tmpfs_fs_t    *fs  = (tmpfs_fs_t *)parent->fsi->priv;
    tmpfs_inode_t *dti = vnode_ti(parent);
    if (dir_lookup(dti, name)) return NULL;

    tmpfs_inode_t *ti = tmpfs_inode_alloc(fs, VFS_S_IFDIR | (mode & 0xFFF));
    if (!ti) return NULL;
    ti->nlink = 2; /* "." + parent link */

    /* Synthesize "." and ".." */
    dir_add(ti,  ".",  ti);
    dir_add(ti,  "..", vnode_ti(parent));

    if (dir_add(dti, name, ti) < 0) { tmpfs_inode_unref(ti); return NULL; }
    vnode_t *v = make_vnode_from_ti(parent->fsi, ti);
    tmpfs_inode_unref(ti);
    return v;
}

static int tmpfs_unlink(vnode_t *parent, const char *name) {
    return dir_remove(vnode_ti(parent), name);
}

static int tmpfs_rmdir(vnode_t *parent, const char *name) {
    tmpfs_inode_t *dti = vnode_ti(parent);
    tmpfs_inode_t *child = dir_lookup(dti, name);
    if (!child) return -ENOENT;
    if (!VFS_S_ISDIR(child->mode)) return -ENOTDIR;
    /* Check empty: only "." and ".." allowed */
    int count = 0;
    for (tmpfs_dirent_t *d = child->dir_head; d; d = d->next) count++;
    if (count > 2) return -ENOTEMPTY;
    return dir_remove(dti, name);
}

static int tmpfs_rename(vnode_t *old_parent, const char *old_name,
                         vnode_t *new_parent, const char *new_name) {
    tmpfs_inode_t *old_dti = vnode_ti(old_parent);
    tmpfs_inode_t *new_dti = vnode_ti(new_parent);
    tmpfs_inode_t *child   = dir_lookup(old_dti, old_name);
    if (!child) return -ENOENT;

    /* Remove existing destination if present */
    dir_remove(new_dti, new_name);

    if (dir_add(new_dti, new_name, child) < 0) return -ENOSPC;
    return dir_remove(old_dti, old_name);
}

static int tmpfs_stat(vnode_t *v, vfs_stat_t *st) {
    tmpfs_inode_t *ti = vnode_ti(v);
    memset(st, 0, sizeof(*st));
    st->ino   = ti->ino;
    st->mode  = ti->mode;
    st->size  = ti->size;
    st->nlink = ti->nlink;
    st->uid   = ti->uid;
    st->gid   = ti->gid;
    return 0;
}

static vnode_t *tmpfs_symlink(vnode_t *parent, const char *name, const char *target) {
    tmpfs_fs_t    *fs  = (tmpfs_fs_t *)parent->fsi->priv;
    tmpfs_inode_t *dti = vnode_ti(parent);
    if (dir_lookup(dti, name)) return NULL;

    tmpfs_inode_t *ti = tmpfs_inode_alloc(fs, VFS_S_IFLNK | 0777);
    if (!ti) return NULL;

    size_t tlen = strlen(target);
    ti->link_target = kzalloc(tlen + 1);
    if (!ti->link_target) { tmpfs_inode_unref(ti); return NULL; }
    memcpy(ti->link_target, target, tlen + 1);
    ti->size = tlen;

    if (dir_add(dti, name, ti) < 0) { tmpfs_inode_unref(ti); return NULL; }
    vnode_t *v = make_vnode_from_ti(parent->fsi, ti);
    tmpfs_inode_unref(ti);
    return v;
}

static int tmpfs_readlink(vnode_t *v, char *buf, size_t sz) {
    tmpfs_inode_t *ti = vnode_ti(v);
    if (!ti->link_target) return -EINVAL;
    size_t len = strlen(ti->link_target);
    if (len >= sz) len = sz - 1;
    memcpy(buf, ti->link_target, len);
    buf[len] = '\0';
    return (int)len;
}

static int tmpfs_truncate(vnode_t *v, uint64_t size) {
    tmpfs_inode_t *ti = vnode_ti(v);
    if (size > ti->size) {
        if (ensure_capacity(ti, (size_t)size) < 0) return -ENOMEM;
        memset(ti->data + ti->size, 0, (size_t)size - ti->size);
    }
    ti->size = (size_t)size;
    v->size  = ti->size;
    return 0;
}

static int tmpfs_chmod(vnode_t *v, uint32_t mode) {
    tmpfs_inode_t *ti = vnode_ti(v);
    ti->mode = (ti->mode & VFS_S_IFMT) | (mode & 0777);
    v->mode  = ti->mode;
    return 0;
}

static int tmpfs_chown(vnode_t *v, int owner, int group) {
    tmpfs_inode_t *ti = vnode_ti(v);
    if (owner >= 0) {
        ti->uid = (uint32_t)owner;
        v->uid = (uint32_t)owner;
    }
    if (group >= 0) {
        ti->gid = (uint32_t)group;
        v->gid = (uint32_t)group;
    }
    return 0;
}

static int tmpfs_sync_v(vnode_t *v) { (void)v; return 0; }

static void tmpfs_evict(vnode_t *v) {
    tmpfs_inode_unref(vnode_ti(v));
    v->fs_data = NULL;
}

/* ── Mount ───────────────────────────────────────────────────────────────── */
static vnode_t *tmpfs_mount(fs_inst_t *fsi, blkdev_t *dev) {
    (void)dev; /* tmpfs needs no block device */

    tmpfs_fs_t *fs = kzalloc(sizeof(tmpfs_fs_t));
    if (!fs) return NULL;
    fs->next_ino = 1;
    fsi->priv = fs;

    tmpfs_inode_t *root = tmpfs_inode_alloc(fs, VFS_S_IFDIR | 0755);
    if (!root) { kfree(fs); return NULL; }
    root->nlink = 2;
    /* "." and ".." both point to root */
    dir_add(root, ".", root);
    dir_add(root, "..", root);

    vnode_t *v = vfs_alloc_vnode();
    if (!v) { tmpfs_inode_unref(root); kfree(fs); return NULL; }
    v->ino      = root->ino;
    v->mode     = root->mode;
    v->size     = 0;
    v->nlink    = 2;
    v->ops      = &g_tmpfs_ops;
    v->fsi      = fsi;
    v->fs_data  = root; /* already ref'd by alloc */
    v->refcount = 1;

    KLOG_INFO("tmpfs: mounted new instance at fsi=%p\n", fsi);
    return v;
}

static void tmpfs_unmount(fs_inst_t *fsi) {
    kfree(fsi->priv);
    fsi->priv = NULL;
}

/* ── fs_ops vtable ───────────────────────────────────────────────────────── */
fs_ops_t g_tmpfs_ops = {
    .name     = "tmpfs",
    .lookup   = tmpfs_lookup,
    .open     = tmpfs_open,
    .close    = tmpfs_close,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .create   = tmpfs_create,
    .mkdir    = tmpfs_mkdir,
    .unlink   = tmpfs_unlink,
    .rmdir    = tmpfs_rmdir,
    .rename   = tmpfs_rename,
    .stat     = tmpfs_stat,
    .symlink  = tmpfs_symlink,
    .readlink = tmpfs_readlink,
    .truncate = tmpfs_truncate,
    .chmod    = tmpfs_chmod,
    .chown    = tmpfs_chown,
    .sync     = tmpfs_sync_v,
    .evict    = tmpfs_evict,
    .mount    = tmpfs_mount,
    .unmount  = tmpfs_unmount,
};

void tmpfs_register(void) {
    vfs_register_fs(&g_tmpfs_ops);
}
