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
#include <stdint.h>
#include <stddef.h>

/* ── Node types ──────────────────────────────────────────────────────────── */
#define PROC_ROOT      0   /* /proc directory itself */
#define PROC_PID_DIR   1   /* /proc/<pid> directory  */
#define PROC_STATUS    2   /* /proc/<pid>/status file */
#define PROC_MAPS      3   /* /proc/<pid>/maps file   */
#define PROC_SELF      4   /* /proc/self symlink       */

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

/* ── VFS operations ──────────────────────────────────────────────────────── */

static vnode_t *procfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !dir->fs_data || !name) return NULL;
    procfs_node_t *pn = (procfs_node_t *)dir->fs_data;

    if (pn->type == PROC_ROOT) {
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

        /* Entries 1..N: iterate task table for live tasks */
        uint32_t skip = (uint32_t)(idx - 1);
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

    if (pn->type == PROC_PID_DIR) {
        static const char *entries[] = { "status", "maps" };
        uint64_t idx = *cookie;
        if (idx >= 2) return 0;
        out->ino  = (pn->pid << 4) | (idx + 1);
        out->type = VFS_DT_REG;
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
