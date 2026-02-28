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
#include "drivers/storage/blkdev.h"
#include "gfx/fbcon.h"
#include "drivers/input/input.h"
#include "arch/x86_64/cpu.h"
#include "sched/sched.h"
#include <stdint.h>
#include <stddef.h>

#define TTY_IFLAG_ICRNL 0x00000100U
#define TTY_OFLAG_OPOST 0x00000001U
#define TTY_OFLAG_ONLCR 0x00000004U
#define TTY_LFLAG_ICANON 0x00000002U

/* Exposed by syscall/net_syscalls.c */
extern uint32_t tty_get_iflag(void);
extern uint32_t tty_get_oflag(void);
extern uint32_t tty_get_lflag(void);

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
#define DEV_RANDOM   4
#define DEV_FULL     5

typedef struct {
    const char *name;
    int dev_type;
    uint32_t mode;
    uint32_t dirent_type;
} devfs_entry_t;

static const devfs_entry_t g_dev_entries[] = {
    { "null",    DEV_NULL,    VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "zero",    DEV_ZERO,    VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "tty",     DEV_TTY,     VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "urandom", DEV_URANDOM, VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "random",  DEV_RANDOM,  VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "full",    DEV_FULL,    VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "stdin",   DEV_TTY,     VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "stdout",  DEV_TTY,     VFS_S_IFCHR | 0666, VFS_DT_CHR },
    { "stderr",  DEV_TTY,     VFS_S_IFCHR | 0666, VFS_DT_CHR },
};

#define DEV_MAX ((int)(sizeof(g_dev_entries) / sizeof(g_dev_entries[0])))

/* ── Per-vnode device info ───────────────────────────────────────────────── */
typedef struct {
    int dev_type;
    blkdev_t *blk;
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
static fs_ops_t devfs_ops;

/* devfs root directory vnode */
static vnode_t *devfs_root = NULL;

/* Device vnodes */
static vnode_t *dev_vnodes[DEV_MAX];

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

        case DEV_TTY: {
            if (len == 0) return 0;
            char *dst = (char *)buf;
            size_t done = 0;
            uint32_t iflag = tty_get_iflag();
            uint32_t lflag = tty_get_lflag();
            bool canonical = (lflag & TTY_LFLAG_ICANON) != 0;
            bool map_crnl = (iflag & TTY_IFLAG_ICRNL) != 0;

            while (done < len) {
                char ch = 0;
                if (input_tty_getchar_nonblock(&ch) == 0) {
                    if (map_crnl && ch == '\r') ch = '\n';
                    dst[done++] = ch;

                    if (canonical && ch == '\n') break;
                    continue;
                }

                if (done > 0) break;
                sched_sleep(1);
            }
            return (ssize_t)done;
        }

        case DEV_URANDOM:
        case DEV_RANDOM: {
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

        case DEV_FULL: {
            memset(buf, 0, len);
            return (ssize_t)len;
        }

        default:
            break;
    }

    if (node->blk) {
        blkdev_t *b = node->blk;
        uint32_t bsz = b->block_size ? b->block_size : 512;
        uint64_t dev_bytes = b->block_count * (uint64_t)bsz;
        if (off >= dev_bytes) return 0;
        if (off + len > dev_bytes) len = (size_t)(dev_bytes - off);

        uint8_t *tmp = kmalloc(bsz);
        if (!tmp) return -ENOMEM;

        size_t done = 0;
        while (done < len) {
            uint64_t pos = off + done;
            uint64_t lba = pos / bsz;
            uint32_t boff = (uint32_t)(pos % bsz);
            size_t chunk = len - done;
            if (chunk > bsz - boff) chunk = bsz - boff;

            if (blkdev_read(b, lba, 1, tmp) < 0) {
                kfree(tmp);
                return done ? (ssize_t)done : -EIO;
            }
            memcpy((uint8_t *)buf + done, tmp + boff, chunk);
            done += chunk;
        }
        kfree(tmp);
        return (ssize_t)done;
    }

    return -EIO;
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
                uint32_t oflag = tty_get_oflag();
                bool post = (oflag & TTY_OFLAG_OPOST) != 0;
                bool onlcr = (oflag & TTY_OFLAG_ONLCR) != 0;
                for (size_t i = 0; i < len; i++) {
                    char ch = s[i];
                    if (post && onlcr && ch == '\n')
                        fbcon_putchar_inst(con, '\r');
                    fbcon_putchar_inst(con, ch);
                }
            }
            return (ssize_t)len;
        }

        case DEV_URANDOM:
        case DEV_RANDOM:
            /* Seed the PRNG */
            if (len >= 8) {
                memcpy(&prng_state, buf, 8);
            }
            return (ssize_t)len;

        case DEV_FULL:
            return -ENOSPC;

        default:
            break;
    }

    if (node->blk) {
        blkdev_t *b = node->blk;
        uint32_t bsz = b->block_size ? b->block_size : 512;
        uint64_t dev_bytes = b->block_count * (uint64_t)bsz;
        if (off >= dev_bytes) return 0;
        if (off + len > dev_bytes) len = (size_t)(dev_bytes - off);

        uint8_t *tmp = kmalloc(bsz);
        if (!tmp) return -ENOMEM;

        size_t done = 0;
        while (done < len) {
            uint64_t pos = off + done;
            uint64_t lba = pos / bsz;
            uint32_t boff = (uint32_t)(pos % bsz);
            size_t chunk = len - done;
            if (chunk > bsz - boff) chunk = bsz - boff;

            if (boff != 0 || chunk != bsz) {
                if (blkdev_read(b, lba, 1, tmp) < 0) {
                    kfree(tmp);
                    return done ? (ssize_t)done : -EIO;
                }
                memcpy(tmp + boff, (const uint8_t *)buf + done, chunk);
                if (blkdev_write(b, lba, 1, tmp) < 0) {
                    kfree(tmp);
                    return done ? (ssize_t)done : -EIO;
                }
            } else {
                if (blkdev_write(b, lba, 1, (const uint8_t *)buf + done) < 0) {
                    kfree(tmp);
                    return done ? (ssize_t)done : -EIO;
                }
            }
            done += chunk;
        }
        kfree(tmp);
        return (ssize_t)done;
    }

    return -EIO;
}

static vnode_t *devfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !name) return NULL;
    for (int i = 0; i < DEV_MAX; i++) {
        if (strcmp(name, g_dev_entries[i].name) == 0) {
            if (dev_vnodes[i]) {
                vfs_vnode_get(dev_vnodes[i]);
                return dev_vnodes[i];
            }
        }
    }

    blkdev_t *blk = blkdev_find_by_name(name);
    if (blk) {
        vnode_t *v = vfs_alloc_vnode();
        if (!v) return NULL;
        devfs_node_t *node = kmalloc(sizeof(devfs_node_t));
        if (!node) {
            kfree(v);
            return NULL;
        }
        node->dev_type = -1;
        node->blk = blk;

        v->ino = 1000 + (uint64_t)blk->dev_id;
        v->mode = VFS_S_IFBLK | 0660;
        v->ops = &devfs_ops;
        v->fsi = dir->fsi;
        v->fs_data = node;
        v->refcount = 1;
        v->size = blk->block_count * (uint64_t)(blk->block_size ? blk->block_size : 512);
        return v;
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
    if (idx < DEV_MAX) {
        out->ino  = idx + 100;
        out->type = g_dev_entries[idx].dirent_type;
        strncpy(out->name, g_dev_entries[idx].name, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    uint64_t bidx = idx - DEV_MAX;
    blkdev_t *blk = blkdev_get_nth((int)bidx);
    if (!blk) return 0;
    out->ino = 1000 + blk->dev_id;
    out->type = VFS_DT_BLK;
    strncpy(out->name, blk->name, VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    *cookie = idx + 1;
    return 1;
}

static int devfs_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->mode = v->mode;
    st->ino  = v->ino;
    st->size = v->size;
    st->blksize = 4096;
    return 0;
}

static void devfs_evict(vnode_t *v) {
    if (!v || !v->fs_data) return;
    devfs_node_t *node = (devfs_node_t *)v->fs_data;
    if (node->blk) {
        kfree(node);
        v->fs_data = NULL;
    }
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
    .evict   = devfs_evict,
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
        v->mode = g_dev_entries[i].mode;
        v->ops  = &devfs_ops;
        v->fsi  = fsi;
        v->refcount = 1;

        devfs_node_t *node = kmalloc(sizeof(devfs_node_t));
        if (node) {
            node->dev_type = i;
            node->blk = NULL;
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
            devfs_node_t *n = (devfs_node_t *)dev_vnodes[i]->fs_data;
            if (n && !n->blk) kfree(dev_vnodes[i]->fs_data);
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
