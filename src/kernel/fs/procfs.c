/* fs/procfs.c — Minimal /proc filesystem for EXO_OS
 *
 * Provides:
 *   /proc/<pid>/status   — process name, state, pid/ppid/uid/gid
 *   /proc/<pid>/maps     — VMA list (Linux format)
 *   /proc/self            → symlink to /proc/<current_pid>
 *
 * Implementation: a synthetic filesystem whose vnodes are created
 * on-the-fly during lookup and generate content lazily on read.
 */
#include "fs/vfs.h"
#include "fs/fd.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/pmm.h"
#include "arch/x86_64/smp.h"
#include "drivers/storage/blkdev.h"
#include "drivers/net/netdev.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ── Node types ──────────────────────────────────────────────────────────── */
#define PROC_ROOT      0   /* /proc directory itself */
#define PROC_PID_DIR   1   /* /proc/<pid> directory  */
#define PROC_STATUS    2   /* /proc/<pid>/status file */
#define PROC_MAPS      3   /* /proc/<pid>/maps file   */
#define PROC_SELF      4   /* /proc/self symlink       */
#define PROC_NET_DIR   5   /* /proc/net directory       */
#define PROC_NET_DEV   6   /* /proc/net/dev file        */
#define PROC_MEMINFO   7   /* /proc/meminfo             */
#define PROC_UPTIME    8   /* /proc/uptime              */
#define PROC_CPUINFO   9   /* /proc/cpuinfo             */
#define PROC_VERSION  10   /* /proc/version             */
#define PROC_LOADAVG  11   /* /proc/loadavg             */
#define PROC_STAT     12   /* /proc/stat                */
#define PROC_MOUNTS   13   /* /proc/mounts              */
#define PROC_PARTS    14   /* /proc/partitions          */
#define PROC_CMDLINE  15   /* /proc/cmdline             */
#define PROC_FILESYSTEMS 16 /* /proc/filesystems         */
#define PROC_PID_STAT 17   /* /proc/<pid>/stat          */
#define PROC_PID_CMD  18   /* /proc/<pid>/cmdline       */
#define PROC_PID_EXE  19   /* /proc/<pid>/exe symlink   */

/* ── Per-vnode data ──────────────────────────────────────────────────────── */
typedef struct {
    int      type;
    uint32_t pid;    /* relevant for PID_DIR, STATUS, MAPS */
} procfs_node_t;

/* ── Config ──────────────────────────────────────────────────────────────── */
#define PROCFS_BUF_SIZE 4096

/* ── Forward declarations ────────────────────────────────────────────────── */
static vnode_t *procfs_lookup(vnode_t *dir, const char *name);
static int      procfs_open(vnode_t *v, int flags);
static int      procfs_close(vnode_t *v);
static ssize_t  procfs_read(vnode_t *v, void *buf, size_t len, uint64_t off);
static ssize_t  procfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off);
static int      procfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out);
static int      procfs_stat(vnode_t *v, vfs_stat_t *st);
static int      procfs_readlink(vnode_t *v, char *buf, size_t bufsize);
static vnode_t *procfs_mount(fs_inst_t *fsi, blkdev_t *dev);
static void     procfs_unmount(fs_inst_t *fsi);
static void     procfs_evict(vnode_t *v);

/* Root vnode */
static vnode_t *procfs_root = NULL;
static fs_inst_t *procfs_fsi_global = NULL;

/* ── Helper: integer to decimal string ───────────────────────────────────── */
static int itoa_simple(uint64_t val, char *buf, size_t bufsz) {
    char tmp[24];
    int len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val && len < 23) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    if ((size_t)len >= bufsz) len = (int)bufsz - 1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

static uint32_t parse_pid(const char *name) {
    uint32_t pid = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return (uint32_t)-1;
        pid = pid * 10 + (name[i] - '0');
    }
    return pid;
}

/* ── Create a procfs vnode ────────────────────────────────────────────────── */
static vnode_t *procfs_alloc_node(int type, uint32_t pid, uint32_t mode) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;

    procfs_node_t *pn = kmalloc(sizeof(procfs_node_t));
    if (!pn) { kfree(v); return NULL; }
    pn->type = type;
    pn->pid  = pid;

    v->ino      = (uint64_t)((type << 20) | pid);
    v->mode     = mode;
    v->ops      = procfs_root ? procfs_root->ops : NULL; /* set after mount */
    v->fsi      = procfs_fsi_global;
    v->fs_data  = pn;
    v->refcount = 1;
    return v;
}

/* ── State name mapping ──────────────────────────────────────────────────── */
static const char *state_name(task_state_t st) {
    switch (st) {
        case TASK_RUNNABLE: return "R (running)";
        case TASK_RUNNING:  return "R (running)";
        case TASK_BLOCKED:  return "S (sleeping)";
        case TASK_DEAD:     return "X (dead)";
        case TASK_SLEEPING: return "S (sleeping)";
        case TASK_ZOMBIE:   return "Z (zombie)";
        default:            return "? (unknown)";
    }
}

/* ── Generate /proc/<pid>/status content ─────────────────────────────────── */
static ssize_t gen_status(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;

    char tmp[24];
    size_t off = 0;

    #define STATUS_APPEND(s) do { \
        size_t l = strlen(s); \
        if (off + l < bufsz) { memcpy(buf + off, s, l); off += l; } \
    } while(0)

    STATUS_APPEND("Name:\t");
    STATUS_APPEND(t->name);
    STATUS_APPEND("\nState:\t");
    STATUS_APPEND(state_name(t->state));
    STATUS_APPEND("\nPid:\t");
    itoa_simple(t->pid, tmp, sizeof(tmp));
    STATUS_APPEND(tmp);
    STATUS_APPEND("\nPPid:\t");
    itoa_simple(t->ppid, tmp, sizeof(tmp));
    STATUS_APPEND(tmp);
    STATUS_APPEND("\nUid:\t");
    itoa_simple(t->uid, tmp, sizeof(tmp));
    STATUS_APPEND(tmp);
    STATUS_APPEND("\nGid:\t");
    itoa_simple(t->gid, tmp, sizeof(tmp));
    STATUS_APPEND(tmp);
    STATUS_APPEND("\nSigPnd:\t");
    itoa_simple(t->sig_pending, tmp, sizeof(tmp));
    STATUS_APPEND(tmp);
    STATUS_APPEND("\nSigBlk:\t");
    itoa_simple(t->sig_mask, tmp, sizeof(tmp));
    STATUS_APPEND(tmp);
    STATUS_APPEND("\n");
    #undef STATUS_APPEND

    return (ssize_t)off;
}

/* ── Generate /proc/<pid>/maps content ───────────────────────────────────── */
static ssize_t gen_maps(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;

    size_t off = 0;
    for (vma_t *vma = t->vma_list; vma; vma = vma->next) {
        /* Format: start-end perms offset dev inode pathname
         * e.g.:  00400000-00401000 r-xp 00000000 00:00 0  [text] */
        char line[128];
        char start_hex[20], end_hex[20];

        /* Simple hex formatter */
        #define HEX(val, dst) do { \
            uint64_t v = (val); char *p = (dst) + 16; *p = '\0'; \
            for (int i = 0; i < 16; i++) { \
                int d = v & 0xF; *(--p) = (d < 10) ? '0'+d : 'a'+(d-10); v >>= 4; \
            } \
        } while(0)

        HEX(vma->start, start_hex);
        HEX(vma->end, end_hex);

        char perms[5];
        perms[0] = (vma->flags & VMA_READ)  ? 'r' : '-';
        perms[1] = (vma->flags & VMA_WRITE) ? 'w' : '-';
        perms[2] = (vma->flags & VMA_EXEC)  ? 'x' : '-';
        perms[3] = 'p';  /* always private for now */
        perms[4] = '\0';

        const char *label = "";
        if (vma->flags & VMA_STACK) label = " [stack]";
        else if (vma->flags & VMA_HEAP) label = " [heap]";

        /* Build line manually */
        size_t loff = 0;
        #define APP(s) do { \
            const char *_s = (s); \
            while (*_s && loff < sizeof(line)-1) line[loff++] = *_s++; \
        } while(0)

        APP(start_hex); line[loff++] = '-';
        APP(end_hex); line[loff++] = ' ';
        APP(perms);
        APP(" 00000000 00:00 0");
        APP(label);
        line[loff++] = '\n';
        line[loff] = '\0';

        #undef APP
        #undef HEX

        if (off + loff < bufsz) {
            memcpy(buf + off, line, loff);
            off += loff;
        } else break;
    }

    return (ssize_t)off;
}

/* ── Generate /proc/net/dev content ─────────────────────────────────────── */
static ssize_t gen_net_dev(char *buf, size_t bufsz) {
    size_t off = 0;

    #define NETDEV_APPEND(s) do { \
        const char *_s = (s); \
        while (*_s && off + 1 < bufsz) buf[off++] = *_s++; \
    } while (0)

    NETDEV_APPEND("Inter-|   Receive                                                |  Transmit\n");
    NETDEV_APPEND(" face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");

    for (int i = 0; i < netdev_count(); i++) {
        netdev_t *dev = netdev_get_nth(i);
        if (!dev) continue;

        NETDEV_APPEND(" ");
        NETDEV_APPEND(dev->name);
        NETDEV_APPEND(":");
        NETDEV_APPEND("        0       0    0    0    0     0          0         0");
        NETDEV_APPEND("        0       0    0    0    0     0       0          0\n");
    }

    #undef NETDEV_APPEND
    return (ssize_t)off;
}

static size_t procfs_appendf(char *buf, size_t bufsz, size_t off, const char *fmt, ...) {
    if (off >= bufsz) return off;
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf + off, bufsz - off, fmt, ap);
    va_end(ap);
    if (n < 0) return bufsz ? bufsz - 1 : 0;
    size_t next = off + (size_t)n;
    if (next >= bufsz) return bufsz ? bufsz - 1 : 0;
    return next;
}

static ssize_t gen_meminfo(char *buf, size_t bufsz) {
    uint64_t total_kb = (pmm_get_total_pages() * PAGE_SIZE) / 1024;
    uint64_t free_kb  = (pmm_get_free_pages() * PAGE_SIZE) / 1024;
    uint64_t used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0;

    size_t off = 0;
    off = procfs_appendf(buf, bufsz, off, "MemTotal:       %llu kB\n", (unsigned long long)total_kb);
    off = procfs_appendf(buf, bufsz, off, "MemFree:        %llu kB\n", (unsigned long long)free_kb);
    off = procfs_appendf(buf, bufsz, off, "MemAvailable:   %llu kB\n", (unsigned long long)free_kb);
    off = procfs_appendf(buf, bufsz, off, "MemUsed:        %llu kB\n", (unsigned long long)used_kb);
    return (ssize_t)off;
}

static ssize_t gen_uptime(char *buf, size_t bufsz) {
    uint64_t ticks = sched_get_ticks();
    uint64_t sec = ticks / 1000;
    uint64_t frac = (ticks % 1000) / 10;
    int n = ksnprintf(buf, bufsz, "%llu.%02llu %llu.%02llu\n",
                      (unsigned long long)sec, (unsigned long long)frac,
                      (unsigned long long)sec, (unsigned long long)frac);
    if (n < 0) return -EIO;
    return (ssize_t)n;
}

static ssize_t gen_cpuinfo(char *buf, size_t bufsz) {
    size_t off = 0;
    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;

    for (uint32_t i = 0; i < ncpu; i++) {
        off = procfs_appendf(buf, bufsz, off, "processor\t: %u\n", i);
        off = procfs_appendf(buf, bufsz, off, "vendor_id\t: EXO\n");
        off = procfs_appendf(buf, bufsz, off, "model name\t: EXO_OS Virtual CPU\n");
        off = procfs_appendf(buf, bufsz, off, "cpu MHz\t\t: 1000.000\n");
        off = procfs_appendf(buf, bufsz, off, "\n");
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

static ssize_t gen_version(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz,
                      "EXO_OS version 0.1 (exo@kernel) #1 SMP %u\n",
                      smp_cpu_count());
    if (n < 0) return -EIO;
    return (ssize_t)n;
}

static ssize_t gen_loadavg(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "0.00 0.00 0.00 1/1 %llu\n",
                      (unsigned long long)sched_get_ticks());
    if (n < 0) return -EIO;
    return (ssize_t)n;
}

static ssize_t gen_stat(char *buf, size_t bufsz) {
    size_t off = 0;
    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    uint64_t ticks = sched_get_ticks();

    off = procfs_appendf(buf, bufsz, off, "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n",
                         (unsigned long long)ticks,
                         (unsigned long long)(ticks / 4));
    for (uint32_t i = 0; i < ncpu; i++) {
        off = procfs_appendf(buf, bufsz, off, "cpu%u %llu 0 0 %llu 0 0 0 0 0 0\n",
                             i,
                             (unsigned long long)(ticks / ncpu),
                             (unsigned long long)(ticks / 4 / ncpu));
    }
    off = procfs_appendf(buf, bufsz, off, "btime 0\n");
    off = procfs_appendf(buf, bufsz, off, "processes %u\n", TASK_TABLE_SIZE);
    return (ssize_t)off;
}

static ssize_t gen_mounts(char *buf, size_t bufsz) {
    vfs_mount_info_t mnts[VFS_MAX_MOUNTS];
    int n = vfs_snapshot_mounts(mnts, VFS_MAX_MOUNTS);
    size_t off = 0;

    for (int i = 0; i < n; i++) {
        const char *dev = mnts[i].dev_name;
        char dev_path[48];
        if (strcmp(dev, "none") == 0) {
            strncpy(dev_path, "none", sizeof(dev_path) - 1);
            dev_path[sizeof(dev_path) - 1] = '\0';
        } else {
            ksnprintf(dev_path, sizeof(dev_path), "/dev/%s", dev);
        }
        off = procfs_appendf(buf, bufsz, off, "%s %s %s rw 0 0\n",
                             dev_path, mnts[i].path, mnts[i].fs_name);
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

static ssize_t gen_partitions(char *buf, size_t bufsz) {
    size_t off = 0;
    off = procfs_appendf(buf, bufsz, off, "major minor  #blocks  name\n\n");

    int n = blkdev_count();
    for (int i = 0; i < n; i++) {
        blkdev_t *d = blkdev_get_nth(i);
        if (!d) continue;
        uint64_t blocks_k = (d->block_count * d->block_size) / 1024;
        off = procfs_appendf(buf, bufsz, off, " 254 %5d %8llu %s\n",
                             i,
                             (unsigned long long)blocks_k,
                             d->name);
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

static ssize_t gen_cmdline(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "root=/dev/vda2 console=tty\n");
    if (n < 0) return -EIO;
    return (ssize_t)n;
}

static ssize_t gen_filesystems(char *buf, size_t bufsz) {
    size_t off = 0;
    off = procfs_appendf(buf, bufsz, off, "\text2\n");
    off = procfs_appendf(buf, bufsz, off, "\tfat32\n");
    off = procfs_appendf(buf, bufsz, off, "nodev\ttmpfs\n");
    off = procfs_appendf(buf, bufsz, off, "nodev\tprocfs\n");
    off = procfs_appendf(buf, bufsz, off, "nodev\tsysfs\n");
    off = procfs_appendf(buf, bufsz, off, "nodev\tdevfs\n");
    return (ssize_t)off;
}

static char proc_state_letter(task_state_t st) {
    switch (st) {
        case TASK_RUNNING:
        case TASK_RUNNABLE: return 'R';
        case TASK_SLEEPING:
        case TASK_BLOCKED: return 'S';
        case TASK_ZOMBIE: return 'Z';
        default: return 'S';
    }
}

static ssize_t gen_pid_stat(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;

    uint64_t now = sched_get_ticks();
    uint64_t utime = now / 2;
    uint64_t stime = now / 2;
    uint64_t start_time = (now > 100) ? (now - 100) : 1;
    uint64_t vsize = 16ULL * 1024ULL * 1024ULL;
    uint64_t rss_pages = 1024;

    int n = ksnprintf(buf, bufsz,
        "%u (%s) %c %u %u %u 0 0 0 0 0 0 0 %llu %llu 0 0 20 0 1 0 %llu %llu %llu 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        t->pid,
        t->name,
        proc_state_letter(t->state),
        t->ppid,
        t->pgid,
        t->sid,
        (unsigned long long)utime,
        (unsigned long long)stime,
        (unsigned long long)start_time,
        (unsigned long long)vsize,
        (unsigned long long)rss_pages);
    if (n < 0) return -EIO;
    return (ssize_t)n;
}

static ssize_t gen_pid_cmdline(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;
    size_t nlen = strlen(t->name);
    if (nlen + 1 >= bufsz) nlen = bufsz - 2;
    memcpy(buf, t->name, nlen);
    buf[nlen] = '\0';
    return (ssize_t)(nlen + 1);
}

/* ── VFS operations ──────────────────────────────────────────────────────── */

static vnode_t *procfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !dir->fs_data || !name) return NULL;
    procfs_node_t *pn = (procfs_node_t *)dir->fs_data;

    if (pn->type == PROC_ROOT) {
        if (strcmp(name, "net") == 0) {
            return procfs_alloc_node(PROC_NET_DIR, 0, VFS_S_IFDIR | 0555);
        }
        if (strcmp(name, "meminfo") == 0) {
            return procfs_alloc_node(PROC_MEMINFO, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "uptime") == 0) {
            return procfs_alloc_node(PROC_UPTIME, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "cpuinfo") == 0) {
            return procfs_alloc_node(PROC_CPUINFO, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "version") == 0) {
            return procfs_alloc_node(PROC_VERSION, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "loadavg") == 0) {
            return procfs_alloc_node(PROC_LOADAVG, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "stat") == 0) {
            return procfs_alloc_node(PROC_STAT, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "mounts") == 0) {
            return procfs_alloc_node(PROC_MOUNTS, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "partitions") == 0) {
            return procfs_alloc_node(PROC_PARTS, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "cmdline") == 0) {
            return procfs_alloc_node(PROC_CMDLINE, 0, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "filesystems") == 0) {
            return procfs_alloc_node(PROC_FILESYSTEMS, 0, VFS_S_IFREG | 0444);
        }
        /* /proc/self — symlink */
        if (strcmp(name, "self") == 0) {
            return procfs_alloc_node(PROC_SELF, 0, VFS_S_IFLNK | 0777);
        }
        /* /proc/<pid> — directory */
        uint32_t pid = parse_pid(name);
        if (pid != (uint32_t)-1) {
            task_t *t = task_lookup(pid);
            if (t) {
                return procfs_alloc_node(PROC_PID_DIR, pid, VFS_S_IFDIR | 0555);
            }
        }
        return NULL;
    }

    if (pn->type == PROC_PID_DIR) {
        if (strcmp(name, "status") == 0) {
            return procfs_alloc_node(PROC_STATUS, pn->pid, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "maps") == 0) {
            return procfs_alloc_node(PROC_MAPS, pn->pid, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "stat") == 0) {
            return procfs_alloc_node(PROC_PID_STAT, pn->pid, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "cmdline") == 0) {
            return procfs_alloc_node(PROC_PID_CMD, pn->pid, VFS_S_IFREG | 0444);
        }
        if (strcmp(name, "exe") == 0) {
            return procfs_alloc_node(PROC_PID_EXE, pn->pid, VFS_S_IFLNK | 0777);
        }
        return NULL;
    }

    if (pn->type == PROC_NET_DIR) {
        if (strcmp(name, "dev") == 0) {
            return procfs_alloc_node(PROC_NET_DEV, 0, VFS_S_IFREG | 0444);
        }
        return NULL;
    }

    return NULL;
}

static int procfs_open(vnode_t *v, int flags) {
    (void)v; (void)flags;
    return 0;
}

static int procfs_close(vnode_t *v) {
    (void)v;
    return 0;
}

static ssize_t procfs_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    if (!v || !v->fs_data) return -EIO;
    procfs_node_t *pn = (procfs_node_t *)v->fs_data;

    /* Generate content into temp buffer, then copy the requested region */
    char *tmp = kmalloc(PROCFS_BUF_SIZE);
    if (!tmp) return -ENOMEM;

    ssize_t total = 0;
    switch (pn->type) {
        case PROC_STATUS: total = gen_status(pn->pid, tmp, PROCFS_BUF_SIZE); break;
        case PROC_MAPS:   total = gen_maps(pn->pid, tmp, PROCFS_BUF_SIZE); break;
        case PROC_NET_DEV: total = gen_net_dev(tmp, PROCFS_BUF_SIZE); break;
        case PROC_MEMINFO: total = gen_meminfo(tmp, PROCFS_BUF_SIZE); break;
        case PROC_UPTIME: total = gen_uptime(tmp, PROCFS_BUF_SIZE); break;
        case PROC_CPUINFO: total = gen_cpuinfo(tmp, PROCFS_BUF_SIZE); break;
        case PROC_VERSION: total = gen_version(tmp, PROCFS_BUF_SIZE); break;
        case PROC_LOADAVG: total = gen_loadavg(tmp, PROCFS_BUF_SIZE); break;
        case PROC_STAT: total = gen_stat(tmp, PROCFS_BUF_SIZE); break;
        case PROC_MOUNTS: total = gen_mounts(tmp, PROCFS_BUF_SIZE); break;
        case PROC_PARTS: total = gen_partitions(tmp, PROCFS_BUF_SIZE); break;
        case PROC_CMDLINE: total = gen_cmdline(tmp, PROCFS_BUF_SIZE); break;
        case PROC_FILESYSTEMS: total = gen_filesystems(tmp, PROCFS_BUF_SIZE); break;
        case PROC_PID_STAT: total = gen_pid_stat(pn->pid, tmp, PROCFS_BUF_SIZE); break;
        case PROC_PID_CMD: total = gen_pid_cmdline(pn->pid, tmp, PROCFS_BUF_SIZE); break;
        default: kfree(tmp); return -EIO;
    }

    if (total < 0) { kfree(tmp); return total; }
    if ((uint64_t)off >= (uint64_t)total) { kfree(tmp); return 0; }

    ssize_t avail = total - (ssize_t)off;
    if ((ssize_t)len < avail) avail = (ssize_t)len;
    memcpy(buf, tmp + off, avail);
    kfree(tmp);
    return avail;
}

static ssize_t procfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    (void)v; (void)buf; (void)len; (void)off;
    return -EACCES;
}

static int procfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    if (!dir || !dir->fs_data) return -1;
    procfs_node_t *pn = (procfs_node_t *)dir->fs_data;

    if (pn->type == PROC_ROOT) {
        uint64_t idx = *cookie;

        /* Entry 0: "self" symlink */
        if (idx == 0) {
            out->ino  = 0xFFFF;
            out->type = VFS_DT_LNK;
            strncpy(out->name, "self", VFS_NAME_MAX);
            out->name[VFS_NAME_MAX] = '\0';
            *cookie = 1;
            return 1;
        }

        /* Entry 1: "net" directory */
        if (idx == 1) {
            out->ino  = 0xFFFE;
            out->type = VFS_DT_DIR;
            strncpy(out->name, "net", VFS_NAME_MAX);
            out->name[VFS_NAME_MAX] = '\0';
            *cookie = 2;
            return 1;
        }

        static const char *root_files[] = {
            "meminfo", "uptime", "cpuinfo", "version",
            "loadavg", "stat", "mounts", "partitions", "cmdline", "filesystems"
        };
        uint64_t root_file_base = 2;
        uint64_t root_file_count = sizeof(root_files) / sizeof(root_files[0]);
        if (idx >= root_file_base && idx < root_file_base + root_file_count) {
            uint64_t file_i = idx - root_file_base;
            out->ino  = 0xFFF0 - (uint64_t)file_i;
            out->type = VFS_DT_REG;
            strncpy(out->name, root_files[file_i], VFS_NAME_MAX);
            out->name[VFS_NAME_MAX] = '\0';
            *cookie = idx + 1;
            return 1;
        }

        /* Remaining entries: iterate task table for live tasks */
        uint32_t skip = (uint32_t)(idx - (root_file_base + root_file_count));
        uint32_t found = 0;
        for (uint32_t i = 0; i < TASK_TABLE_SIZE; i++) {
            task_t *t = task_get_from_table(i);
            if (!t || t->state == TASK_DEAD) continue;
            if (found == skip) {
                out->ino  = t->pid;
                out->type = VFS_DT_DIR;
                itoa_simple(t->pid, out->name, VFS_NAME_MAX);
                *cookie = idx + 1;
                return 1;
            }
            found++;
        }
        return 0;  /* exhausted */
    }

    if (pn->type == PROC_NET_DIR) {
        if (*cookie > 0) return 0;
        out->ino  = 0xFFFD;
        out->type = VFS_DT_REG;
        strncpy(out->name, "dev", VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = 1;
        return 1;
    }

    if (pn->type == PROC_PID_DIR) {
        static const char *entries[] = { "status", "maps", "stat", "cmdline", "exe" };
        uint64_t idx = *cookie;
        if (idx >= 5) return 0;
        out->ino  = (pn->pid << 4) | (idx + 1);
        out->type = (idx == 4) ? VFS_DT_LNK : VFS_DT_REG;
        strncpy(out->name, entries[idx], VFS_NAME_MAX);
        out->name[VFS_NAME_MAX] = '\0';
        *cookie = idx + 1;
        return 1;
    }

    return 0;
}

static int procfs_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->mode    = v->mode;
    st->ino     = v->ino;
    st->size    = 0;
    st->blksize = 4096;
    return 0;
}

static int procfs_readlink(vnode_t *v, char *buf, size_t bufsize) {
    if (!v || !v->fs_data) return -EIO;
    procfs_node_t *pn = (procfs_node_t *)v->fs_data;

    if (pn->type == PROC_SELF) {
        task_t *cur = sched_current();
        if (!cur) return -ESRCH;
        char pidstr[16];
        itoa_simple(cur->pid, pidstr, sizeof(pidstr));
        /* Build "/proc/<pid>" */
        size_t plen = strlen(pidstr) + 6; /* "/proc/" */
        if (plen >= bufsize) return -ERANGE;
        memcpy(buf, "/proc/", 6);
        memcpy(buf + 6, pidstr, strlen(pidstr) + 1);
        return 0;
    }
    if (pn->type == PROC_PID_EXE) {
        task_t *t = task_lookup(pn->pid);
        if (!t) return -ESRCH;
        const char *target = "/bin/sh";
        if (strcmp(t->name, "init") == 0) target = "/bin/sh";
        size_t tlen = strlen(target);
        if (tlen + 1 > bufsize) return -ERANGE;
        memcpy(buf, target, tlen + 1);
        return 0;
    }
    return -EINVAL;
}

static void procfs_evict(vnode_t *v) {
    if (v && v->fs_data) {
        kfree(v->fs_data);
        v->fs_data = NULL;
    }
}

/* ── fs_ops vtable ───────────────────────────────────────────────────────── */
static fs_ops_t procfs_ops = {
    .name     = "procfs",
    .lookup   = procfs_lookup,
    .open     = procfs_open,
    .close    = procfs_close,
    .read     = procfs_read,
    .write    = procfs_write,
    .readdir  = procfs_readdir,
    .create   = NULL,
    .mkdir    = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .stat     = procfs_stat,
    .symlink  = NULL,
    .readlink = procfs_readlink,
    .truncate = NULL,
    .sync     = NULL,
    .evict    = procfs_evict,
    .mount    = procfs_mount,
    .unmount  = procfs_unmount,
};

static vnode_t *procfs_mount(fs_inst_t *fsi, blkdev_t *dev) {
    (void)dev;
    procfs_fsi_global = fsi;

    procfs_root = procfs_alloc_node(PROC_ROOT, 0, VFS_S_IFDIR | 0555);
    if (!procfs_root) return NULL;
    procfs_root->ops = &procfs_ops;

    /* Fix ops for root (procfs_alloc_node set it to NULL first time)  */
    KLOG_INFO("procfs: mounted at /proc\n");
    return procfs_root;
}

static void procfs_unmount(fs_inst_t *fsi) {
    (void)fsi;
    if (procfs_root) {
        kfree(procfs_root->fs_data);
        kfree(procfs_root);
        procfs_root = NULL;
    }
}

/* ── Public init ─────────────────────────────────────────────────────────── */
void procfs_init(void) {
    vfs_register_fs(&procfs_ops);
    if (vfs_mount("/proc", NULL, "procfs") == 0) {
        KLOG_INFO("procfs: /proc mounted successfully\n");
    } else {
        KLOG_WARN("procfs: failed to mount /proc\n");
    }
}
