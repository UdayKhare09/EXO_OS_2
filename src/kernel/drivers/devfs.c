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
#include "lib/spinlock.h"
#include "drivers/storage/blkdev.h"
#include "gfx/fbcon.h"
#include "drivers/input/input.h"
#include "drivers/input/evdev.h"    /* g_evdev_kbd_fops, g_evdev_mouse_fops */
#include "drivers/fb.h"             /* g_fb_fops */
#include "arch/x86_64/cpu.h"
#include "net/socket_defs.h"         /* POLLIN, POLLOUT, POLLRDNORM */
#include "sched/sched.h"
#include "drivers/pty.h"
#include "ipc/signal.h"
#include <stdint.h>
#include <stddef.h>

#define TTY_IFLAG_ICRNL  0x00000100U
#define TTY_OFLAG_OPOST  0x00000001U
#define TTY_OFLAG_ONLCR  0x00000004U
/* lflag bits */
#define TTY_LFLAG_ISIG   0x00000001U   /* generate signals on VINTR/VQUIT/VSUSP */
#define TTY_LFLAG_ICANON 0x00000002U
#define TTY_LFLAG_ECHO   0x00000008U   /* echo input characters                 */
#define TTY_LFLAG_ECHOE  0x00000010U   /* echo ERASE as BS-SP-BS                */
#define TTY_LFLAG_ECHOK  0x00000020U   /* echo newline after KILL               */
#define TTY_LFLAG_ECHONL 0x00000040U   /* echo NL even if ECHO is off           */

/* c_cc indices (Linux/POSIX) */
#define TTY_VINTR   0   /* ^C  → SIGINT  */
#define TTY_VQUIT   1   /* ^\  → SIGQUIT */
#define TTY_VERASE  2   /* DEL/^H → erase previous char */
#define TTY_VKILL   3   /* ^U → erase line              */
#define TTY_VEOF    4   /* ^D → end of file             */
#define TTY_VSUSP  10   /* ^Z  → SIGTSTP */
#define TTY_VWERASE 14  /* ^W → erase previous word     */

/* Exposed by syscall/net_syscalls.c */
extern uint32_t tty_get_iflag(void);
extern uint32_t tty_get_oflag(void);
extern uint32_t tty_get_lflag(void);
extern uint8_t  tty_get_cc(int idx);
extern void     tty_signal_foreground(int sig);
/* tty_console_ioctl: the session-aware TTY ioctl handler in net_syscalls.c
 * (renamed from the old static tty_ioctl; handles TCGETS/TCSETS/TIOCGWINSZ…) */
extern int tty_console_ioctl(struct file *f, unsigned long cmd, unsigned long arg);

/* ── Physical console file_ops (for /dev/ttyN and /dev/tty without PTY) ─────
 * These wrap the keyboard input ring + fbcon output.                       */
static ssize_t tty_console_read(file_t *f, void *buf, size_t len) {
    (void)f;
    if (len == 0) return 0;
    char       *dst     = (char *)buf;
    uint32_t    iflag   = tty_get_iflag();
    uint32_t    lflag   = tty_get_lflag();
    bool canonical= (lflag & TTY_LFLAG_ICANON)  != 0;
    bool isig     = (lflag & TTY_LFLAG_ISIG)    != 0;
    bool do_echo  = (lflag & TTY_LFLAG_ECHO)    != 0;
    bool echoe    = (lflag & TTY_LFLAG_ECHOE)   != 0;
    bool echok    = (lflag & TTY_LFLAG_ECHOK)   != 0;
    bool echonl   = (lflag & TTY_LFLAG_ECHONL)  != 0;
    bool map_crnl = (iflag & TTY_IFLAG_ICRNL)   != 0;

    char vintr  = isig     ? (char)tty_get_cc(TTY_VINTR)  : 0;
    char vquit  = isig     ? (char)tty_get_cc(TTY_VQUIT)  : 0;
    char vsusp  = isig     ? (char)tty_get_cc(TTY_VSUSP)  : 0;
    char verase = canonical? (char)tty_get_cc(TTY_VERASE) : 0;
    char vkill  = canonical? (char)tty_get_cc(TTY_VKILL)  : 0;
    char veof   = canonical? (char)tty_get_cc(TTY_VEOF)   : 0;
    char vwerase= canonical? (char)tty_get_cc(TTY_VWERASE): 0;

    fbcon_t *con = fbcon_get();

    /* ── Non-canonical / raw mode: VMIN/VTIME ─────────────────────────── */
    if (!canonical) {
        uint8_t vmin = tty_get_cc(6); /* VMIN */
        if (vmin == 0) vmin = 1;
        size_t done = 0;
        while (done < len) {
            char ch = 0;
            if (input_tty_getchar_nonblock(&ch) == 0) {
                if (map_crnl && ch == '\r') ch = '\n';
                if (isig && vintr && ch == vintr) { tty_signal_foreground(SIGINT);  continue; }
                if (isig && vquit && ch == vquit) { tty_signal_foreground(SIGQUIT); continue; }
                if (isig && vsusp && ch == vsusp) { tty_signal_foreground(SIGTSTP); continue; }
                if (do_echo && con) fbcon_putchar_inst(con, ch);
                dst[done++] = ch;
                if (done >= (size_t)vmin) break;
            } else {
                if (done > 0) break;
                if (f->flags & O_NONBLOCK) return -EAGAIN;
                sched_sleep(1);
            }
        }
        return (ssize_t)done;
    }

    /* ── Canonical mode: full Linux n_tty line discipline ──────────────
     * Buffer characters locally, process erase/kill/signal/EOF,
     * echo to screen, and deliver a complete line to the caller.
     * Protected by a spinlock so concurrent readers (e.g. forked glibc
     * processes) don't corrupt the shared line buffer.                  */
#define LBUF_MAX 4096
    static spinlock_t lbuf_lock;
    static char   lbuf[LBUF_MAX];
    static size_t llen   = 0;
    static bool   lready = false;
    static bool   lbuf_lock_inited = false;
    if (!lbuf_lock_inited) { spinlock_init(&lbuf_lock); lbuf_lock_inited = true; }
    spinlock_acquire(&lbuf_lock);

    /* Deliver leftover data from a previous partial read */
    if (lready && llen > 0) {
        size_t take = (llen < len) ? llen : len;
        memcpy(dst, lbuf, take);
        llen -= take;
        if (llen > 0) memmove(lbuf, lbuf + take, llen);
        else          lready = false;
        spinlock_release(&lbuf_lock);
        return (ssize_t)take;
    }
    lready = false;

    for (;;) {
        char ch = 0;
        if (input_tty_getchar_nonblock(&ch) != 0) {
            if (f->flags & O_NONBLOCK) {
                if (llen > 0) goto deliver;
                spinlock_release(&lbuf_lock);
                return -EAGAIN;
            }
            sched_sleep(1);
            continue;
        }

        /* ICRNL */
        if (map_crnl && ch == '\r') ch = '\n';

        /* ISIG: signal-generating characters */
        if (isig) {
            if (vintr && ch == vintr) {
                if (do_echo && con) {
                    fbcon_putchar_inst(con, '^');
                    fbcon_putchar_inst(con, 'C');
                    fbcon_putchar_inst(con, '\n');
                }
                llen = 0;
                tty_signal_foreground(SIGINT);
                continue;
            }
            if (vquit && ch == vquit) {
                llen = 0;
                tty_signal_foreground(SIGQUIT);
                continue;
            }
            if (vsusp && ch == vsusp) {
                if (do_echo && con) {
                    fbcon_putchar_inst(con, '^');
                    fbcon_putchar_inst(con, 'Z');
                    fbcon_putchar_inst(con, '\n');
                }
                llen = 0;
                tty_signal_foreground(SIGTSTP);
                continue;
            }
        }

        /* VERASE: erase previous character (^H / DEL) */
        if (verase && ch == verase) {
            if (llen > 0) {
                llen--;
                if (echoe && con) {
                    fbcon_putchar_inst(con, '\b');
                    fbcon_putchar_inst(con, ' ');
                    fbcon_putchar_inst(con, '\b');
                }
            }
            continue;
        }

        /* VWERASE: erase previous word (^W) */
        if (vwerase && ch == vwerase) {
            while (llen > 0 && lbuf[llen-1] == ' ') {
                llen--;
                if (echoe && con) {
                    fbcon_putchar_inst(con, '\b');
                    fbcon_putchar_inst(con, ' ');
                    fbcon_putchar_inst(con, '\b');
                }
            }
            while (llen > 0 && lbuf[llen-1] != ' ') {
                llen--;
                if (echoe && con) {
                    fbcon_putchar_inst(con, '\b');
                    fbcon_putchar_inst(con, ' ');
                    fbcon_putchar_inst(con, '\b');
                }
            }
            continue;
        }

        /* VKILL: erase entire line (^U) */
        if (vkill && ch == vkill) {
            if (echoe && con) {
                for (size_t i = 0; i < llen; i++) {
                    fbcon_putchar_inst(con, '\b');
                    fbcon_putchar_inst(con, ' ');
                    fbcon_putchar_inst(con, '\b');
                }
            } else if (echok && con) {
                fbcon_putchar_inst(con, '\n');
            }
            llen = 0;
            continue;
        }

        /* VEOF: ^D — deliver line without newline; empty buffer means EOF */
        if (veof && ch == veof) {
            if (llen == 0) {
                return 0;  /* EOF */
            }
            goto deliver;
        }

        /* Append to line buffer */
        if (llen < LBUF_MAX - 1)
            lbuf[llen++] = ch;

        /* Echo */
        if (con) {
            if (ch == '\n') {
                if (do_echo || echonl) fbcon_putchar_inst(con, '\n');
            } else if ((unsigned char)ch < 0x20 && ch != '\t') {
                /* control character: show as ^X */
                if (do_echo) {
                    fbcon_putchar_inst(con, '^');
                    fbcon_putchar_inst(con, (char)(ch + 0x40));
                }
            } else {
                if (do_echo) fbcon_putchar_inst(con, ch);
            }
        }

        /* Newline completes the line */
        if (ch == '\n') goto deliver;
    }

deliver:;
    size_t take = (llen < len) ? llen : len;
    memcpy(dst, lbuf, take);
    llen -= take;
    if (llen > 0) { memmove(lbuf, lbuf + take, llen); lready = true; }
    spinlock_release(&lbuf_lock);
    return (ssize_t)take;
}

static ssize_t tty_console_write(file_t *f, const void *buf, size_t len) {
    (void)f;
    fbcon_t *con = fbcon_get();
    if (!con) return (ssize_t)len;   /* discard if no console */
    const char *s = (const char *)buf;
    uint32_t oflag = tty_get_oflag();
    bool post  = (oflag & TTY_OFLAG_OPOST)  != 0;
    bool onlcr = (oflag & TTY_OFLAG_ONLCR) != 0;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (post && onlcr && ch == '\n')
            fbcon_putchar_inst(con, '\r');
        fbcon_putchar_inst(con, ch);
    }
    return (ssize_t)len;
}

static int tty_console_poll(file_t *f, int events) {
    (void)f;
    int ready = 0;
    if ((events & (POLLIN | POLLRDNORM)) && input_tty_char_available())
        ready |= POLLIN | POLLRDNORM;
    if (events & (POLLOUT | POLLWRNORM))
        ready |= POLLOUT | POLLWRNORM;
    return ready;
}

static int tty_console_close(file_t *f) { (void)f; return 0; }

static file_ops_t g_tty_console_fops = {
    .read  = tty_console_read,
    .write = tty_console_write,
    .close = tty_console_close,
    .poll  = tty_console_poll,
    .ioctl = tty_console_ioctl,
};

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
#define DEV_INPUT_DIR   13   /* /dev/input (sub-directory)                   */
#define DEV_INPUT_EVENT 14   /* /dev/input/eventN — Linux evdev char device   */
#define DEV_FB          15   /* /dev/fb0  — Linux fbdev framebuffer           */

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
/* Extra synthetic entries (fb0 and input/) appear before block devices     */
#define RDDIR_EXTRA_BASE (DEV_MAX + SYM_MAX + VTTY_COUNT + LOOP_COUNT)
#define RDDIR_BLK_BASE   (RDDIR_EXTRA_BASE + 2)  /* +2: fb0 + input dir    */

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
static int      devfs_chmod(vnode_t *v, uint32_t mode);
static int      devfs_chown(vnode_t *v, int owner, int group);
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

    /* If the parent is the /dev/input sub-directory, look up eventN nodes */
    if (dir->fs_data) {
        devfs_node_t *pn = (devfs_node_t *)dir->fs_data;
        if (pn->dev_type == DEV_INPUT_DIR) {
            if (strcmp(name, "event0") == 0)
                return devfs_make_dynamic(dir, DEV_INPUT_EVENT, 0,
                                          MKDEV(13, 64), 900,
                                          VFS_S_IFCHR | 0660);
            if (strcmp(name, "event1") == 0)
                return devfs_make_dynamic(dir, DEV_INPUT_EVENT, 1,
                                          MKDEV(13, 65), 901,
                                          VFS_S_IFCHR | 0660);
            return NULL;
        }
    }

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

    /* /dev/input  — sub-directory containing evdev nodes */
    if (strcmp(name, "input") == 0)
        return devfs_make_dynamic(dir, DEV_INPUT_DIR, 0, 0,
                                   800, VFS_S_IFDIR | 0755);

    /* /dev/fb0  — Linux framebuffer character device (major 29, minor 0) */
    if (strcmp(name, "fb0") == 0)
        return devfs_make_dynamic(dir, DEV_FB, 0, MKDEV(29, 0),
                                   850, VFS_S_IFCHR | 0660);

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
         * If the caller has a PTY as its ctty, route through the PTY slave.
         * Otherwise fall through to the physical console.                   */
        task_t *cur = sched_current();
        if (cur && cur->ctty_pty) {
            v->pending_f_ops = &g_pty_slave_fops;
            v->pending_priv  = cur->ctty_pty;
        } else {
            v->pending_f_ops = &g_tty_console_fops;
            v->pending_priv  = NULL;
        }
        return 0;
    }

    if (node->dev_type == DEV_CONSOLE) {
        /* /dev/console → always the physical console line discipline.
         * On Linux, /dev/console is a full TTY with echo, ICANON, etc.
         * Route through the same file_ops as /dev/tty console path.     */
        v->pending_f_ops = &g_tty_console_fops;
        v->pending_priv  = NULL;
        return 0;
    }

    if (node->dev_type == DEV_VTTY) {
        /* /dev/ttyN always routes to the physical console.                  */
        v->pending_f_ops = &g_tty_console_fops;
        v->pending_priv  = NULL;
        return 0;
    }

    if (node->dev_type == DEV_PTMX) {
        /* Each open of /dev/ptmx allocates a new PTY master */
        int idx = pty_alloc();
        if (idx < 0) return -ENOMEM;
        pty_pair_t *pair = pty_get(idx);
        if (!pair) return -EIO;
        v->pending_f_ops = &g_pty_master_fops;
        v->pending_priv  = pair;
        return 0;
    }

    if (node->dev_type == DEV_INPUT_EVENT) {
        /* event0 = keyboard, event1 = mouse */
        v->pending_f_ops = (node->index == 0) ? &g_evdev_kbd_fops
                                              : &g_evdev_mouse_fops;
        v->pending_priv  = (void *)(uintptr_t)(unsigned int)node->index;
        return 0;
    }

    if (node->dev_type == DEV_FB) {
        v->pending_f_ops = &g_fb_fops;
        v->pending_priv  = NULL;
        return 0;
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
    if (idx < (uint64_t)RDDIR_EXTRA_BASE) {
        uint64_t li = idx - RDDIR_LOOP_BASE;
        out->ino  = 700 + li;
        out->type = VFS_DT_BLK;
        out->name[0]='l'; out->name[1]='o'; out->name[2]='o';
        out->name[3]='p'; out->name[4]=(char)('0'+li); out->name[5]='\0';
        *cookie = idx + 1;
        return 1;
    }

    /* Synthetic: /dev/fb0 (extra index 0) and /dev/input (extra index 1) */
    if (idx < (uint64_t)RDDIR_BLK_BASE) {
        uint64_t ei = idx - RDDIR_EXTRA_BASE;
        if (ei == 0) {
            out->ino  = 850;
            out->type = VFS_DT_CHR;
            out->name[0]='f'; out->name[1]='b'; out->name[2]='0'; out->name[3]='\0';
        } else {
            out->ino  = 800;
            out->type = VFS_DT_DIR;
            out->name[0]='i'; out->name[1]='n'; out->name[2]='p';
            out->name[3]='u'; out->name[4]='t'; out->name[5]='\0';
        }
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
    st->dev     = v->fsi ? v->fsi->dev_id : 0;
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

static int devfs_chmod(vnode_t *v, uint32_t mode) {
    if (!v) return -EINVAL;
    v->mode = (v->mode & VFS_S_IFMT) | (mode & 07777);
    v->ctime = (int64_t)(sched_get_ticks() / 1000);
    return 0;
}

static int devfs_chown(vnode_t *v, int owner, int group) {
    if (!v) return -EINVAL;
    if (owner >= 0) v->uid = (uint32_t)owner;
    if (group >= 0) v->gid = (uint32_t)group;
    v->ctime = (int64_t)(sched_get_ticks() / 1000);
    return 0;
}

static void devfs_evict(vnode_t *v) {
    if (!v || !v->fs_data) return;
    devfs_node_t *node = (devfs_node_t *)v->fs_data;
    /* Free nodes that were dynamically allocated during lookup */
    if (node->blk || node->dev_type == DEV_SYM ||
        node->dev_type == DEV_VTTY  || node->dev_type == DEV_LOOP ||
        node->dev_type == DEV_INPUT_DIR || node->dev_type == DEV_INPUT_EVENT ||
        node->dev_type == DEV_FB) {
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
    .chmod    = devfs_chmod,
    .chown    = devfs_chown,
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
