/* drivers/devfs.c — Simple device filesystem for EXO_OS
 *
 * Provides /dev/null, /dev/zero, /dev/tty, /dev/urandom as character
 * devices accessible through the VFS.
 *
 * Implementation: registers a "devfs" filesystem type, which is mounted
 * at /dev. Uses tmpfs-like in-memory vnodes with custom read/write ops
 * per device type.
 */
#include "fs/vfs.h"
#include "fs/fd.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "gfx/fbcon.h"
#include "arch/x86_64/cpu.h"
#include <stdint.h>
#include <stddef.h>

static inline uint64_t devfs_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Device types ────────────────────────────────────────────────────────── */
#define DEV_NULL     0
#define DEV_ZERO     1
#define DEV_TTY      2
#define DEV_URANDOM  3
#define DEV_MAX      4

/* ── Per-vnode device info ───────────────────────────────────────────────── */
typedef struct {
    int dev_type;
} devfs_node_t;

/* Forward declarations */
static vnode_t *devfs_lookup(vnode_t *dir, const char *name);
static int      devfs_open(vnode_t *v, int flags);
static int      devfs_close(vnode_t *v);
static ssize_t  devfs_read(vnode_t *v, void *buf, size_t len, uint64_t off);
static ssize_t  devfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off);
static int      devfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out);
static int      devfs_stat(vnode_t *v, vfs_stat_t *st);
static vnode_t *devfs_mount(fs_inst_t *fsi, blkdev_t *dev);
static void     devfs_unmount(fs_inst_t *fsi);

/* devfs root directory vnode */
static vnode_t *devfs_root = NULL;

/* Device vnodes */
static vnode_t *dev_vnodes[DEV_MAX];
static const char *dev_names[DEV_MAX] = { "null", "zero", "tty", "urandom" };

/* ── PRNG for /dev/urandom ───────────────────────────────────────────────── */
static uint64_t prng_state = 0x12345678DEADBEEFULL;

static uint64_t prng_next(void) {
    /* xorshift64* */
    uint64_t x = prng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    prng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ── Device operations ───────────────────────────────────────────────────── */

static ssize_t devfs_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    (void)off;
    if (!v || !v->fs_data) return -EIO;
    devfs_node_t *node = (devfs_node_t *)v->fs_data;

    switch (node->dev_type) {
        case DEV_NULL:
            return 0;  /* always EOF */

        case DEV_ZERO: {
            memset(buf, 0, len);
            return (ssize_t)len;
        }

        case DEV_TTY:
            /* TODO: read from input subsystem */
            return 0;

        case DEV_URANDOM: {
            uint8_t *dst = (uint8_t *)buf;
            size_t i = 0;
            while (i < len) {
                uint64_t r = prng_next();
                size_t chunk = len - i;
                if (chunk > 8) chunk = 8;
                memcpy(dst + i, &r, chunk);
                i += chunk;
            }
            return (ssize_t)len;
        }

        default:
            return -EIO;
    }
}

static ssize_t devfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    (void)off;
    if (!v || !v->fs_data) return -EIO;
    devfs_node_t *node = (devfs_node_t *)v->fs_data;

    switch (node->dev_type) {
        case DEV_NULL:
            return (ssize_t)len;  /* discard */

        case DEV_ZERO:
            return (ssize_t)len;  /* discard */

        case DEV_TTY: {
            /* Write to framebuffer console */
            fbcon_t *con = fbcon_get();
            if (con) {
                const char *s = (const char *)buf;
                for (size_t i = 0; i < len; i++)
                    fbcon_putchar_inst(con, s[i]);
            }
            return (ssize_t)len;
        }

        case DEV_URANDOM:
            /* Seed the PRNG */
            if (len >= 8) {
                memcpy(&prng_state, buf, 8);
            }
            return (ssize_t)len;

        default:
            return -EIO;
    }
}

static vnode_t *devfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !name) return NULL;
    for (int i = 0; i < DEV_MAX; i++) {
        if (strcmp(name, dev_names[i]) == 0) {
            if (dev_vnodes[i]) {
                vfs_vnode_get(dev_vnodes[i]);
                return dev_vnodes[i];
            }
        }
    }
    return NULL;
}

static int devfs_open(vnode_t *v, int flags) {
    (void)v; (void)flags;
    return 0;
}

static int devfs_close(vnode_t *v) {
    (void)v;
    return 0;
}

static int devfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    (void)dir;
    uint64_t idx = *cookie;
    if (idx >= DEV_MAX) return 0;

    out->ino  = idx + 100;
    out->type = VFS_DT_CHR;
    strncpy(out->name, dev_names[idx], VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    *cookie = idx + 1;
    return 1;
}

static int devfs_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->mode = v->mode;
    st->ino  = v->ino;
    st->size = 0;
    st->blksize = 4096;
    return 0;
}

/* ── devfs fs_ops vtable ─────────────────────────────────────────────────── */
static fs_ops_t devfs_ops = {
    .name    = "devfs",
    .lookup  = devfs_lookup,
    .open    = devfs_open,
    .close   = devfs_close,
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .create  = NULL,
    .mkdir   = NULL,
    .unlink  = NULL,
    .rmdir   = NULL,
    .rename  = NULL,
    .stat    = devfs_stat,
    .symlink = NULL,
    .readlink = NULL,
    .truncate = NULL,
    .sync    = NULL,
    .evict   = NULL,
    .mount   = devfs_mount,
    .unmount = devfs_unmount,
};

static vnode_t *devfs_mount(fs_inst_t *fsi, blkdev_t *dev) {
    (void)dev;

    /* Seed PRNG with TSC at setup */
    prng_state = devfs_rdtsc();

    /* Create root directory vnode */
    devfs_root = vfs_alloc_vnode();
    if (!devfs_root) return NULL;
    devfs_root->ino  = 1;
    devfs_root->mode = VFS_S_IFDIR | 0755;
    devfs_root->ops  = &devfs_ops;
    devfs_root->fsi  = fsi;
    devfs_root->refcount = 1;

    /* Create device vnodes */
    for (int i = 0; i < DEV_MAX; i++) {
        vnode_t *v = vfs_alloc_vnode();
        if (!v) continue;
        v->ino  = 100 + i;
        v->mode = VFS_S_IFCHR | 0666;
        v->ops  = &devfs_ops;
        v->fsi  = fsi;
        v->refcount = 1;

        devfs_node_t *node = kmalloc(sizeof(devfs_node_t));
        if (node) {
            node->dev_type = i;
            v->fs_data = node;
        }
        dev_vnodes[i] = v;
    }

    KLOG_INFO("devfs: mounted at /dev (%d device nodes)\n", DEV_MAX);
    return devfs_root;
}

static void devfs_unmount(fs_inst_t *fsi) {
    (void)fsi;
    for (int i = 0; i < DEV_MAX; i++) {
        if (dev_vnodes[i]) {
            kfree(dev_vnodes[i]->fs_data);
            kfree(dev_vnodes[i]);
            dev_vnodes[i] = NULL;
        }
    }
    kfree(devfs_root);
    devfs_root = NULL;
}

/* ── Public init function ────────────────────────────────────────────────── */
void devfs_init(void) {
    vfs_register_fs(&devfs_ops);
    if (vfs_mount("/dev", NULL, "devfs") == 0) {
        KLOG_INFO("devfs: /dev mounted successfully\n");
    } else {
        KLOG_WARN("devfs: failed to mount /dev\n");
    }
}
