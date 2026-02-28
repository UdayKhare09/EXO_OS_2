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
#include "mm/vmm.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "drivers/storage/blkdev.h"
#include "gfx/fbcon.h"
#include "drivers/input/input.h"
#include "arch/x86_64/cpu.h"
#include "sched/sched.h"
#include "drivers/pty.h"
#include "ipc/signal.h"
#include <stdint.h>
#include <stddef.h>

#define TTY_IFLAG_ICRNL  0x00000100U
#define TTY_OFLAG_OPOST  0x00000001U
#define TTY_OFLAG_ONLCR  0x00000004U
#define TTY_LFLAG_ICANON 0x00000002U
#define TTY_LFLAG_ISIG   0x00000001U   /* generate signals on VINTR/VQUIT/VSUSP */

/* c_cc indices (POSIX) */
#define TTY_VINTR  0   /* ^C  → SIGINT  */
#define TTY_VQUIT  1   /* ^\  → SIGQUIT */
#define TTY_VSUSP 10   /* ^Z  → SIGTSTP */

/* Exposed by syscall/net_syscalls.c */
extern uint32_t tty_get_iflag(void);
extern uint32_t tty_get_oflag(void);
extern uint32_t tty_get_lflag(void);
extern uint8_t  tty_get_cc(int idx);
extern void     tty_signal_foreground(int sig);

static inline uint64_t devfs_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Linux makedev() ────────────────────────────────────────────────────── */
#define MKDEV(maj, min) \
    (((uint64_t)((maj) & 0xfff00ULL) << 32) | \
     ((uint64_t)((min) & 0xff000ULL) << 12) | \
     ((uint64_t)((maj) & 0x000ffULL) <<  8) | \
     ((uint64_t)((min) & 0x000ffULL)))

/* ── Device types ────────────────────────────────────────────────────────── */
#define DEV_NULL     0
#define DEV_ZERO     1
#define DEV_TTY      2
#define DEV_URANDOM  3
#define DEV_RANDOM   4
#define DEV_FULL     5
#define DEV_CONSOLE  6
#define DEV_PTMX     7
#define DEV_KMSG     8   /* /dev/kmsg  — kernel ring-buffer log              */
#define DEV_MEM      9   /* /dev/mem   — physical memory (HHDM-mapped)       */
#define DEV_VTTY    10   /* /dev/ttyN  — virtual console N (node->index)     */
#define DEV_LOOP    11   /* /dev/loopN — loop block device N (node->index)   */
#define DEV_SYM     12   /* symlink node: target in node->symlink_target     */

typedef struct {
    const char *name;
    int         dev_type;
    uint32_t    mode;
    uint32_t    dirent_type;
    uint64_t    rdev;         /* Linux makedev() */
} devfs_entry_t;

/* Static character device entries (no stdin/stdout/stderr — those are symlinks) */
static const devfs_entry_t g_dev_entries[] = {
    { "null",    DEV_NULL,    VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(1,3)  },
    { "zero",    DEV_ZERO,    VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(1,5)  },
    { "tty",     DEV_TTY,     VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(5,0)  },
    { "urandom", DEV_URANDOM, VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(1,9)  },
    { "random",  DEV_RANDOM,  VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(1,8)  },
    { "full",    DEV_FULL,    VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(1,7)  },
    { "console", DEV_CONSOLE, VFS_S_IFCHR | 0600, VFS_DT_CHR, MKDEV(5,1)  },
    { "ptmx",    DEV_PTMX,    VFS_S_IFCHR | 0666, VFS_DT_CHR, MKDEV(5,2)  },
    { "kmsg",    DEV_KMSG,    VFS_S_IFCHR | 0600, VFS_DT_CHR, MKDEV(1,11) },
    { "mem",     DEV_MEM,     VFS_S_IFCHR | 0640, VFS_DT_CHR, MKDEV(1,1)  },
};

#define DEV_MAX ((int)(sizeof(g_dev_entries) / sizeof(g_dev_entries[0])))

/* Static symlinks in /dev */
static const struct { const char *name; const char *target; } g_sym_entries[] = {
    { "stdin",  "/proc/self/fd/0" },
    { "stdout", "/proc/self/fd/1" },
    { "stderr", "/proc/self/fd/2" },
    { "fd",     "/proc/self/fd"   },
    { "core",   "/proc/kcore"     },
};
#define SYM_MAX ((int)(sizeof(g_sym_entries) / sizeof(g_sym_entries[0])))

#define VTTY_COUNT  7   /* /dev/tty0 .. /dev/tty6  (major 4) */
#define LOOP_COUNT  8   /* /dev/loop0 .. /dev/loop7 (major 7) */

/* readdir cookie layout:
 *  [0,          DEV_MAX)                 static char devices
 *  [DEV_MAX,    DEV_MAX+SYM_MAX)         static symlinks
 *  [+SYM_MAX,   +VTTY_COUNT)             /dev/ttyN
 *  [+VTTY_COUNT,+LOOP_COUNT)             /dev/loopN
 *  beyond                                block devices (blkdev_get_nth)      */
#define RDDIR_SYM_BASE  (DEV_MAX)
#define RDDIR_VTTY_BASE (DEV_MAX + SYM_MAX)
#define RDDIR_LOOP_BASE (DEV_MAX + SYM_MAX + VTTY_COUNT)
#define RDDIR_BLK_BASE  (DEV_MAX + SYM_MAX + VTTY_COUNT + LOOP_COUNT)

/* ── Per-vnode device info ───────────────────────────────────────────────── */
typedef struct {
    int      dev_type;
    blkdev_t *blk;
    uint64_t  rdev;              /* Linux makedev() for stat                 */
    int       index;             /* DEV_VTTY (0-6) or DEV_LOOP (0-7)         */
    char      symlink_target[256]; /* DEV_SYM: symlink destination            */
} devfs_node_t;

/* Forward declarations */
static vnode_t *devfs_lookup(vnode_t *dir, const char *name);
static int      devfs_open(vnode_t *v, int flags);
static int      devfs_close(vnode_t *v);
static ssize_t  devfs_read(vnode_t *v, void *buf, size_t len, uint64_t off);
static ssize_t  devfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off);
static int      devfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out);
static int      devfs_stat(vnode_t *v, vfs_stat_t *st);
static vnode_t *devfs_symlink(vnode_t *parent, const char *name, const char *tgt);
static int      devfs_readlink(vnode_t *v, char *buf, size_t sz);
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

        case DEV_TTY:
        case DEV_CONSOLE: {
            if (len == 0) return 0;
            char *dst = (char *)buf;
            size_t done = 0;
            uint32_t iflag = tty_get_iflag();
            uint32_t lflag = tty_get_lflag();
            bool canonical = (lflag & TTY_LFLAG_ICANON) != 0;
            bool isig      = (lflag & TTY_LFLAG_ISIG)   != 0;
            bool map_crnl  = (iflag & TTY_IFLAG_ICRNL)  != 0;

            /* Pre-load signal chars from termios (defaults: ^C, ^\, ^Z) */
            char vintr = isig ? (char)tty_get_cc(TTY_VINTR) : 0;
            char vquit = isig ? (char)tty_get_cc(TTY_VQUIT) : 0;
            char vsusp = isig ? (char)tty_get_cc(TTY_VSUSP) : 0;

            while (done < len) {
                char ch = 0;
                if (input_tty_getchar_nonblock(&ch) == 0) {
                    if (map_crnl && ch == '\r') ch = '\n';

                    /* Signal-generating characters (ISIG): consume but don't buffer */
                    if (isig && vintr && ch == vintr) {
                        tty_signal_foreground(SIGINT);
                        continue;
                    }
                    if (isig && vquit && ch == vquit) {
                        tty_signal_foreground(SIGQUIT);
                        continue;
                    }
                    if (isig && vsusp && ch == vsusp) {
                        tty_signal_foreground(SIGTSTP);
                        continue;
                    }

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

        case DEV_KMSG: {
            uint32_t total, bufsz;
            const char *ring = klog_get_ring(&total, &bufsz);
            if (!ring || !bufsz || total == 0) return 0;
            uint64_t avail = (total < bufsz) ? (uint64_t)total : (uint64_t)bufsz;
            if (off >= avail) return 0;
            if (len > (size_t)(avail - off)) len = (size_t)(avail - off);
            /* Ring may wrap; copy in two chunks */
            uint64_t start = (total > bufsz) ? ((uint64_t)total % bufsz) : 0;
            uint64_t rpos  = (start + off) % bufsz;
            size_t copied  = 0;
            while (copied < len) {
                size_t chunk = len - copied;
                if (rpos + chunk > bufsz) chunk = bufsz - (size_t)rpos;
                memcpy((uint8_t *)buf + copied, ring + rpos, chunk);
                copied += chunk;
                rpos = (rpos + chunk) % bufsz;
            }
            return (ssize_t)copied;
        }

        case DEV_MEM: {
            const uint64_t MEM_LIMIT = 0x100000000ULL; /* 4 GiB */
            if (off >= MEM_LIMIT) return 0;
            if (off + (uint64_t)len > MEM_LIMIT) len = (size_t)(MEM_LIMIT - off);
            const void *virt = (const void *)vmm_phys_to_virt((uintptr_t)off);
            memcpy(buf, virt, len);
            return (ssize_t)len;
        }

        case DEV_VTTY: {
            /* All virtual consoles map to the single physical console */
            if (len == 0) return 0;
            char *dst = (char *)buf;
            size_t done = 0;
            uint32_t iflag = tty_get_iflag();
            uint32_t lflag = tty_get_lflag();
            bool canonical = (lflag & TTY_LFLAG_ICANON) != 0;
            bool isig      = (lflag & TTY_LFLAG_ISIG)   != 0;
            bool map_crnl  = (iflag & TTY_IFLAG_ICRNL)  != 0;
            char vintr = isig ? (char)tty_get_cc(TTY_VINTR) : 0;
            char vquit = isig ? (char)tty_get_cc(TTY_VQUIT) : 0;
            char vsusp = isig ? (char)tty_get_cc(TTY_VSUSP) : 0;
            while (done < len) {
                char ch = 0;
                if (input_tty_getchar_nonblock(&ch) == 0) {
                    if (map_crnl && ch == '\r') ch = '\n';
                    if (isig && vintr && ch == vintr) { tty_signal_foreground(SIGINT);  continue; }
                    if (isig && vquit && ch == vquit) { tty_signal_foreground(SIGQUIT); continue; }
                    if (isig && vsusp && ch == vsusp) { tty_signal_foreground(SIGTSTP); continue; }
                    dst[done++] = ch;
                    if (canonical && ch == '\n') break;
                    continue;
                }
                if (done > 0) break;
                sched_sleep(1);
            }
            return (ssize_t)done;
        }

        case DEV_LOOP:
        case DEV_SYM:
            return -ENXIO;

        default:
            break;
    }

    /* Block device fallthrough (sda, vda, etc.) */
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

        case DEV_TTY:
        case DEV_CONSOLE: {
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

        case DEV_KMSG:
            klog_info("%.*s", (int)len, (const char *)buf);
            return (ssize_t)len;

        case DEV_MEM: {
            const uint64_t MEM_LIMIT = 0x100000000ULL;
            if (off >= MEM_LIMIT) return 0;
            if (off + (uint64_t)len > MEM_LIMIT) len = (size_t)(MEM_LIMIT - off);
            void *virt = (void *)vmm_phys_to_virt((uintptr_t)off);
            memcpy(virt, buf, len);
            return (ssize_t)len;
        }

        case DEV_VTTY: {
            fbcon_t *con = fbcon_get();
            if (con) {
                const char *s = (const char *)buf;
                uint32_t oflag = tty_get_oflag();
                bool post  = (oflag & TTY_OFLAG_OPOST) != 0;
                bool onlcr = (oflag & TTY_OFLAG_ONLCR)  != 0;
                for (size_t i = 0; i < len; i++) {
                    char ch = s[i];
                    if (post && onlcr && ch == '\n')
                        fbcon_putchar_inst(con, '\r');
                    fbcon_putchar_inst(con, ch);
                }
            }
            return (ssize_t)len;
        }

        case DEV_LOOP:
        case DEV_SYM:
            return -ENXIO;

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

/* ── Helpers: allocate dynamic vnodes on-demand ─────────────────────────── */
static vnode_t *devfs_make_sym(vnode_t *dir, const char *target, uint64_t ino) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;
    devfs_node_t *node = kmalloc(sizeof(devfs_node_t));
    if (!node) { kfree(v); return NULL; }
    memset(node, 0, sizeof(*node));
    node->dev_type = DEV_SYM;
    strncpy(node->symlink_target, target, 255);
    v->ino      = ino;
    v->mode     = VFS_S_IFLNK | 0777;
    v->ops      = &devfs_ops;
    v->fsi      = dir->fsi;
    v->fs_data  = node;
    v->refcount = 1;
    return v;
}

static vnode_t *devfs_make_dynamic(vnode_t *dir, int type, int idx_,
                                   uint64_t rdev_, uint64_t ino, uint32_t mode) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;
    devfs_node_t *node = kmalloc(sizeof(devfs_node_t));
    if (!node) { kfree(v); return NULL; }
    memset(node, 0, sizeof(*node));
    node->dev_type = type;
    node->index    = idx_;
    node->rdev     = rdev_;
    v->ino      = ino;
    v->mode     = mode;
    v->ops      = &devfs_ops;
    v->fsi      = dir->fsi;
    v->fs_data  = node;
    v->refcount = 1;
    return v;
}

static vnode_t *devfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !name) return NULL;

    /* Static char device nodes (cached in dev_vnodes[]) */
    for (int i = 0; i < DEV_MAX; i++) {
        if (strcmp(name, g_dev_entries[i].name) == 0) {
            if (dev_vnodes[i]) {
                vfs_vnode_get(dev_vnodes[i]);
                return dev_vnodes[i];
            }
        }
    }

    /* Static symlinks */
    for (int i = 0; i < SYM_MAX; i++) {
        if (strcmp(name, g_sym_entries[i].name) == 0)
            return devfs_make_sym(dir, g_sym_entries[i].target, (uint64_t)(500 + i));
    }

    /* /dev/ttyN  (N = 0 .. VTTY_COUNT-1) */
    if (name[0]=='t' && name[1]=='t' && name[2]=='y' &&
        name[3]>='0' && name[3]<='9' && name[4]=='\0') {
        int n = name[3] - '0';
        if (n < VTTY_COUNT)
            return devfs_make_dynamic(dir, DEV_VTTY, n, MKDEV(4,(uint32_t)n),
                                      (uint64_t)(600+n), VFS_S_IFCHR | 0620);
    }

    /* /dev/loopN (N = 0 .. LOOP_COUNT-1) */
    if (name[0]=='l' && name[1]=='o' && name[2]=='o' && name[3]=='p' &&
        name[4]>='0' && name[4]<='9' && name[5]=='\0') {
        int n = name[4] - '0';
        if (n < LOOP_COUNT)
            return devfs_make_dynamic(dir, DEV_LOOP, n, MKDEV(7,(uint32_t)n),
                                      (uint64_t)(700+n), VFS_S_IFBLK | 0660);
    }

    /* Block devices registered with blkdev */
    blkdev_t *blk = blkdev_find_by_name(name);
    if (blk) {
        vnode_t *v = vfs_alloc_vnode();
        if (!v) return NULL;
        devfs_node_t *node = kmalloc(sizeof(devfs_node_t));
        if (!node) { kfree(v); return NULL; }
        memset(node, 0, sizeof(*node));
        node->dev_type = -1;
        node->blk      = blk;
        uint32_t maj   = (blk->name[0] == 'v') ? 252U : 8U;
        node->rdev     = MKDEV(maj, (uint32_t)blk->dev_id);
        v->ino      = 1000 + (uint64_t)blk->dev_id;
        v->mode     = VFS_S_IFBLK | 0660;
        v->ops      = &devfs_ops;
        v->fsi      = dir->fsi;
        v->fs_data  = node;
        v->refcount = 1;
        v->size     = blk->block_count *
                      (uint64_t)(blk->block_size ? blk->block_size : 512);
        return v;
    }

    return NULL;
}

static int devfs_open(vnode_t *v, int flags) {
    (void)flags;
    if (!v || !v->fs_data) return 0;
    devfs_node_t *node = (devfs_node_t *)v->fs_data;

    if (node->dev_type == DEV_TTY) {
        /* /dev/tty → the process's controlling terminal.
         * If the caller has a PTY as its ctty, route the file through
         * the slave side of that PTY exactly like Linux does. */
        task_t *cur = sched_current();
        if (cur && cur->ctty_pty) {
            v->pending_f_ops = &g_pty_slave_fops;
            v->pending_priv  = cur->ctty_pty;
        }
        /* else: fall through to the physical console (framebuffer/keyboard) */
        return 0;
    }

    if (node->dev_type == DEV_PTMX) {
        /* Each open of /dev/ptmx allocates a new PTY master (Linux: ptmx_open) */
        int idx = pty_alloc();
        if (idx < 0) return -ENOMEM;
        pty_pair_t *pair = pty_get(idx);
        if (!pair) return -EIO;
        /* Inject into the vnode so vfs_open() transfers it to the file_t */
        v->pending_f_ops = &g_pty_master_fops;
        v->pending_priv  = pair;
    }
    return 0;
}

static int devfs_close(vnode_t *v) {
    (void)v;
    return 0;
}

static int devfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    (void)dir;
    uint64_t idx = *cookie;

    /* Static char device nodes */
    if (idx < (uint64_t)RDDIR_SYM_BASE) {
        out->ino  = idx + 100;
        out->type = g_dev_entries[idx].dirent_type;
        strncpy(out->name, g_dev_entries[idx].name, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    /* Symlinks */
    if (idx < (uint64_t)RDDIR_VTTY_BASE) {
        uint64_t si = idx - RDDIR_SYM_BASE;
        out->ino  = 500 + si;
        out->type = VFS_DT_LNK;
        strncpy(out->name, g_sym_entries[si].name, VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    /* Virtual consoles tty0..tty(VTTY_COUNT-1) */
    if (idx < (uint64_t)RDDIR_LOOP_BASE) {
        uint64_t ti = idx - RDDIR_VTTY_BASE;
        out->ino  = 600 + ti;
        out->type = VFS_DT_CHR;
        out->name[0]='t'; out->name[1]='t'; out->name[2]='y';
        out->name[3]=(char)('0'+ti); out->name[4]='\0';
        *cookie = idx + 1;
        return 1;
    }

    /* Loop devices loop0..loop(LOOP_COUNT-1) */
    if (idx < (uint64_t)RDDIR_BLK_BASE) {
        uint64_t li = idx - RDDIR_LOOP_BASE;
        out->ino  = 700 + li;
        out->type = VFS_DT_BLK;
        out->name[0]='l'; out->name[1]='o'; out->name[2]='o';
        out->name[3]='p'; out->name[4]=(char)('0'+li); out->name[5]='\0';
        *cookie = idx + 1;
        return 1;
    }

    /* Block devices */
    uint64_t bidx = idx - RDDIR_BLK_BASE;
    blkdev_t *blk = blkdev_get_nth((int)bidx);
    if (!blk) return 0;
    out->ino  = 1000 + blk->dev_id;
    out->type = VFS_DT_BLK;
    strncpy(out->name, blk->name, VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    *cookie = idx + 1;
    return 1;
}

static int devfs_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->mode    = v->mode;
    st->ino     = v->ino;
    st->size    = v->size;
    st->blksize = 4096;
    st->nlink   = 1;
    if (v->fs_data) {
        const devfs_node_t *node = (const devfs_node_t *)v->fs_data;
        st->rdev = node->rdev;
        if (node->blk) {
            uint32_t maj = (node->blk->name[0] == 'v') ? 252U : 8U;
            st->rdev = MKDEV(maj, (uint32_t)node->blk->dev_id);
        }
    }
    return 0;
}

static void devfs_evict(vnode_t *v) {
    if (!v || !v->fs_data) return;
    devfs_node_t *node = (devfs_node_t *)v->fs_data;
    /* Free nodes that were dynamically allocated during lookup */
    if (node->blk || node->dev_type == DEV_SYM ||
        node->dev_type == DEV_VTTY || node->dev_type == DEV_LOOP) {
        kfree(node);
        v->fs_data = NULL;
    }
}

/* ── Symlink support for /dev/stdin, /dev/stdout, /dev/stderr, /dev/fd ────── */
static vnode_t *devfs_symlink(vnode_t *parent, const char *name,
                              const char *target) {
    (void)name; /* in devfs we don't persist the name — caller does via VFS */
    return devfs_make_sym(parent, target, 0);
}

static int devfs_readlink(vnode_t *v, char *buf, size_t sz) {
    if (!v || !v->fs_data || sz == 0) return -EINVAL;
    const devfs_node_t *node = (const devfs_node_t *)v->fs_data;
    if (node->dev_type != DEV_SYM) return -EINVAL;
    size_t tlen = strlen(node->symlink_target);
    if (sz > tlen + 1) sz = tlen + 1;
    memcpy(buf, node->symlink_target, sz - 1);
    buf[sz - 1] = '\0';
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
    .symlink  = devfs_symlink,
    .readlink = devfs_readlink,
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
            memset(node, 0, sizeof(*node));
            node->dev_type = i;
            node->blk      = NULL;
            node->rdev     = g_dev_entries[i].rdev;
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
