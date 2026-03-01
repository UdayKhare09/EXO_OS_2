/* drivers/devpts.c — devpts: the /dev/pts filesystem
 *
 * Linux model: a synthetic filesystem mounted at /dev/pts.
 *   - Each open /dev/ptmx call produces a pair; the slave lives here as
 *     /dev/pts/0, /dev/pts/1, … (matching the PTY index).
 *   - readdir() on /dev/pts lists only currently-allocated PTY numbers.
 *   - open() on /dev/pts/N injects g_pty_slave_fops into the returned file_t
 *     via vnode->pending_f_ops (the same mechanism used by /dev/ptmx).
 *   - ptrace/ptsname use /dev/pts/N so the slave path is "/dev/pts/<N>".
 */
#include "fs/vfs.h"
#include "fs/fd.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "drivers/pty.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int  type;   /* 0 = root dir, 1 = slave node */
    int  index;  /* PTY index for slave nodes */
} devpts_node_t;

static vnode_t    *devpts_root = NULL;
static fs_inst_t  *devpts_fsi  = NULL;
static fs_ops_t    devpts_ops;

/* ── Alloc helpers ───────────────────────────────────────────────────────── */
static vnode_t *devpts_alloc_node(int type, int index, uint32_t mode) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;
    devpts_node_t *dn = kmalloc(sizeof(devpts_node_t));
    if (!dn) { kfree(v); return NULL; }
    dn->type  = type;
    dn->index = index;

    v->ino      = (type == 0) ? 1 : (uint64_t)(index + 2);
    v->mode     = mode;
    v->ops      = &devpts_ops;
    v->fsi      = devpts_fsi;
    v->fs_data  = dn;
    v->refcount = 1;
    return v;
}

/* ── Integer parse helper ────────────────────────────────────────────────── */
static int parse_pty_index(const char *name) {
    int n = 0;
    if (!name || !name[0]) return -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
        n = n * 10 + (name[i] - '0');
    }
    if (n < 0 || n >= PTY_MAX) return -1;
    return n;
}

/* ── VFS operations ──────────────────────────────────────────────────────── */

static vnode_t *devpts_lookup(vnode_t *dir, const char *name) {
    if (!dir || !dir->fs_data || !name) return NULL;
    devpts_node_t *dn = (devpts_node_t *)dir->fs_data;
    if (dn->type != 0) return NULL; /* not a dir */

    int idx = parse_pty_index(name);
    if (idx < 0) return NULL;

    pty_pair_t *p = pty_get(idx);
    if (!p) return NULL; /* no such PTY */

    /* Linux: /dev/pts/N is char device crw--w---- root tty */
    vnode_t *v = devpts_alloc_node(1, idx, VFS_S_IFCHR | 0620);
    if (!v) return NULL;
    v->uid = p->owner_uid;
    v->gid = p->owner_gid;
    return v;
}

static int devpts_open(vnode_t *v, int flags) {
    (void)flags;
    if (!v || !v->fs_data) return 0;
    devpts_node_t *dn = (devpts_node_t *)v->fs_data;
    if (dn->type != 1) return 0; /* root dir open is fine */

    /* slave open: attach f_ops via pending mechanism */
    pty_pair_t *p = pty_get(dn->index);
    if (!p) return -ENXIO;

    p->slave_open++;
    v->pending_f_ops = &g_pty_slave_fops;
    v->pending_priv  = p;
    return 0;
}

static int devpts_close(vnode_t *v) {
    (void)v;
    return 0;
}

static ssize_t devpts_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    (void)v; (void)buf; (void)len; (void)off;
    return -EIO;
}

static ssize_t devpts_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    (void)v; (void)buf; (void)len; (void)off;
    return -EIO;
}

static int devpts_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    if (!dir || !dir->fs_data) return -1;
    devpts_node_t *dn = (devpts_node_t *)dir->fs_data;
    if (dn->type != 0) return -1;

    /* Iterate allocated PTY pairs */
    uint64_t found = 0;
    for (int i = 0; i < PTY_MAX; i++) {
        pty_pair_t *p = pty_get(i);
        if (!p) continue;
        if (found == *cookie) {
            out->ino  = (uint64_t)(i + 2);
            out->type = VFS_DT_CHR;
            /* write decimal index as name */
            int n = i;
            char tmp[12];
            int tlen = 0;
            if (n == 0) { tmp[tlen++] = '0'; }
            else {
                char rev[10]; int rlen = 0;
                while (n > 0) { rev[rlen++] = '0' + (n % 10); n /= 10; }
                for (int j = rlen - 1; j >= 0; j--) tmp[tlen++] = rev[j];
            }
            tmp[tlen] = '\0';
            strncpy(out->name, tmp, VFS_NAME_MAX);
            out->name[VFS_NAME_MAX] = '\0';
            *cookie = found + 1;
            return 1;
        }
        found++;
    }
    return 0;
}

static int devpts_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->mode    = v->mode;
    st->ino     = v->ino;
    st->uid     = v->uid;
    st->gid     = v->gid;
    st->blksize = 4096;
    return 0;
}

static void devpts_evict(vnode_t *v) {
    if (v && v->fs_data) {
        kfree(v->fs_data);
        v->fs_data = NULL;
    }
}

static vnode_t *devpts_mount(fs_inst_t *fsi, blkdev_t *dev) {
    (void)dev;
    devpts_fsi  = fsi;
    devpts_root = devpts_alloc_node(0, -1, VFS_S_IFDIR | 0755);
    if (!devpts_root) return NULL;
    KLOG_INFO("devpts: mounted at /dev/pts\n");
    return devpts_root;
}

static void devpts_unmount(fs_inst_t *fsi) {
    (void)fsi;
    if (devpts_root) {
        if (devpts_root->fs_data) kfree(devpts_root->fs_data);
        kfree(devpts_root);
        devpts_root = NULL;
    }
}

static fs_ops_t devpts_ops = {
    .name     = "devpts",
    .lookup   = devpts_lookup,
    .open     = devpts_open,
    .close    = devpts_close,
    .read     = devpts_read,
    .write    = devpts_write,
    .readdir  = devpts_readdir,
    .create   = NULL,
    .mkdir    = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .stat     = devpts_stat,
    .symlink  = NULL,
    .readlink = NULL,
    .truncate = NULL,
    .sync     = NULL,
    .evict    = devpts_evict,
    .mount    = devpts_mount,
    .unmount  = devpts_unmount,
};

void devpts_init(void) {
    vfs_register_fs(&devpts_ops);
    /* Directory /dev/pts is created in main.c before this mount */
    if (vfs_mount("/dev/pts", NULL, "devpts") == 0) {
        KLOG_INFO("devpts: /dev/pts mounted successfully\n");
    } else {
        KLOG_WARN("devpts: failed to mount /dev/pts\n");
    }
}
