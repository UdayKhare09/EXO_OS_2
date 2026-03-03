/* fs/procfs.c — Linux-accurate /proc filesystem
 *
 * All output formats match Linux 5.15 byte-for-byte so that procps, iproute2,
 * mlibc and similar tools work without patching.
 */
#include "fs/vfs.h"
#include "fs/fd.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/cred.h"
#include "mm/pmm.h"
#include "arch/x86_64/smp.h"
#include "drivers/storage/blkdev.h"
#include "drivers/net/netdev.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ── Node types ──────────────────────────────────────────────────────────── */
#define PROC_ROOT            0
#define PROC_PID_DIR         1
#define PROC_STATUS          2
#define PROC_MAPS            3
#define PROC_SELF            4
#define PROC_NET_DIR         5
#define PROC_NET_DEV         6
#define PROC_MEMINFO         7
#define PROC_UPTIME          8
#define PROC_CPUINFO         9
#define PROC_VERSION        10
#define PROC_LOADAVG        11
#define PROC_STAT           12
#define PROC_MOUNTS         13
#define PROC_PARTS          14
#define PROC_CMDLINE        15
#define PROC_FILESYSTEMS    16
#define PROC_PID_STAT       17
#define PROC_PID_CMD        18
#define PROC_PID_EXE        19
#define PROC_PID_FD_DIR     20
#define PROC_PID_FD_ENTRY   21
#define PROC_NET_TCP        22
#define PROC_NET_UDP        23
#define PROC_NET_UNIX       24
#define PROC_NET_ARP        25
#define PROC_NET_ROUTE      26
#define PROC_SYS_DIR        27
#define PROC_SYS_KERNEL_DIR 28
#define PROC_SYS_FS_DIR     29
#define PROC_SYS_VM_DIR     30
#define PROC_SYS_NET_DIR    31
#define PROC_DISKSTATS      32
#define PROC_INTERRUPTS     33
#define PROC_PID_IO         34
#define PROC_PID_CGROUP     35
#define PROC_PID_OOM_ADJ    36
#define PROC_PID_FDINFO_DIR 37
#define PROC_PID_FDINFO_ENT 38
#define PROC_PID_WCHAN      39
#define PROC_PID_SMAPS      40
#define PROC_SWAPS          41
#define PROC_DEVICES        42
#define PROC_MISC           43
#define PROC_SYS_HOSTNAME   44
#define PROC_SYS_FILEMAX    45
#define PROC_SYS_OVERCOMMIT 46
#define PROC_SYS_IPV4_DIR   47
#define PROC_SYS_FORWARD    48
#define PROC_NET_TCP6       49
#define PROC_NET_UDP6       50
#define PROC_PID_ENVIRON    51
#define PROC_IOMEM          52
#define PROC_IOPORTS        53
#define PROC_BUDDYINFO      54
#define PROC_SLABINFO       55
#define PROC_ZONEINFO       56
#define PROC_CRYPTO         57
#define PROC_NET_FIB_TRIE   58
#define PROC_NET_FIB_STAT   59

/* ── Per-vnode data ──────────────────────────────────────────────────────── */
typedef struct {
    int      type;
    uint32_t pid;    /* relevant for PID_DIR, STATUS, MAPS, FD, etc. */
    int      fd;     /* for PROC_PID_FD_ENTRY / PROC_PID_FDINFO_ENT  */
} procfs_node_t;

/* Kernel hostname (settable via /proc/sys/kernel/hostname) */
static char g_hostname[64] = "exo";

/* ── Config ──────────────────────────────────────────────────────────────── */
#define PROCFS_BUF_SIZE 65536

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
static vnode_t *procfs_alloc_node(int type, uint32_t pid, int fd, uint32_t mode) {
    vnode_t *v = vfs_alloc_vnode();
    if (!v) return NULL;

    procfs_node_t *pn = kmalloc(sizeof(procfs_node_t));
    if (!pn) { kfree(v); return NULL; }
    pn->type = type;
    pn->pid  = pid;
    pn->fd   = fd;

    v->ino      = (uint64_t)((type << 20) | pid | ((uint64_t)(uint32_t)fd << 36));
    v->mode     = mode;
    v->ops      = procfs_root ? procfs_root->ops : NULL;
    v->fsi      = procfs_fsi_global;
    v->fs_data  = pn;
    v->refcount = 1;
    return v;
}

/* ── hex formatting helpers ────────────────────────────────────────────── */
static void fmt_hex16(uint64_t v, char *out) {
    for (int i = 15; i >= 0; i--) {
        int d = (int)(v & 0xF);
        out[i] = (d < 10) ? '0' + d : 'a' + (d - 10);
        v >>= 4;
    }
    out[16] = '\0';
}
static void fmt_hex8(uint32_t v, char *out) {
    for (int i = 7; i >= 0; i--) {
        int d = (int)(v & 0xF);
        out[i] = (d < 10) ? '0' + d : 'a' + (d - 10);
        v >>= 4;
    }
    out[8] = '\0';
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

/* alias for brevity */
#define PA(buf,bsz,off,...) procfs_appendf(buf,bsz,off,__VA_ARGS__)

/* ── State name mapping ──────────────────────────────────────────────────── */
static const char *state_name(task_state_t st) {
    switch (st) {
        case TASK_RUNNABLE: case TASK_RUNNING: return "R (running)";
        case TASK_BLOCKED:  case TASK_SLEEPING: return "S (sleeping)";
        case TASK_DEAD:     return "X (dead)";
        case TASK_ZOMBIE:   return "Z (zombie)";
        default:            return "S (sleeping)";
    }
}

static char proc_state_letter(task_state_t st) {
    switch (st) {
        case TASK_RUNNING: case TASK_RUNNABLE: return 'R';
        case TASK_SLEEPING: case TASK_BLOCKED: return 'S';
        case TASK_ZOMBIE: return 'Z';
        default: return 'S';
    }
}

/* ── /proc/<pid>/status — full Linux 5.15 format ────────────────────────── */
static ssize_t gen_status(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;

    int fdcount = 0;
    for (int i = 0; i < FD_TABLE_SIZE; i++)
        if (t->fd_table[i]) fdcount++;

    /* Estimate VM from VMA list */
    uint64_t vm_size = 0, vm_stk = 0, vm_exe = 0;
    for (vma_t *vma = t->vma_list; vma; vma = vma->next) {
        uint64_t sz = vma->end - vma->start;
        vm_size += sz;
        if (vma->flags & VMA_STACK) vm_stk += sz;
        else if (vma->flags & VMA_EXEC) vm_exe += sz;
    }
    if (!vm_size) { vm_size = 8192ULL*1024; vm_stk = 132*1024; vm_exe = 4*1024; }
    uint64_t vm_rss = vm_size / 4;
    uint64_t vm_data = vm_size > vm_stk + vm_exe ? vm_size - vm_stk - vm_exe : 0;

    size_t off = 0;
    off = PA(buf, bufsz, off, "Name:\t%s\n", t->name);
    off = PA(buf, bufsz, off, "Umask:\t%04o\n", t->umask ? t->umask : 0022u);
    off = PA(buf, bufsz, off, "State:\t%s\n", state_name(t->state));
    off = PA(buf, bufsz, off, "Tgid:\t%u\n", (unsigned)t->pid);
    off = PA(buf, bufsz, off, "Ngid:\t0\n");
    off = PA(buf, bufsz, off, "Pid:\t%u\n",  (unsigned)t->pid);
    off = PA(buf, bufsz, off, "PPid:\t%u\n", (unsigned)t->ppid);
    off = PA(buf, bufsz, off, "TracerPid:\t0\n");
    off = PA(buf, bufsz, off, "Uid:\t%u\t%u\t%u\t%u\n",
             (unsigned)t->cred.uid, (unsigned)t->cred.euid,
             (unsigned)t->cred.suid, (unsigned)t->cred.fsuid);
    off = PA(buf, bufsz, off, "Gid:\t%u\t%u\t%u\t%u\n",
             (unsigned)t->cred.gid, (unsigned)t->cred.egid,
             (unsigned)t->cred.sgid, (unsigned)t->cred.fsgid);
    off = PA(buf, bufsz, off, "FDSize:\t%d\n", fdcount > 64 ? fdcount : 64);
    off = PA(buf, bufsz, off, "Groups:");
    uint32_t ng = t->cred.group_count < (uint32_t)TASK_MAX_GROUPS ? t->cred.group_count : (uint32_t)TASK_MAX_GROUPS;
    for (uint32_t gi = 0; gi < ng; gi++)
        off = PA(buf, bufsz, off, " %u", (unsigned)t->cred.groups[gi]);
    off = PA(buf, bufsz, off, "\n");
    off = PA(buf, bufsz, off, "NStgid:\t%u\n", (unsigned)t->pid);
    off = PA(buf, bufsz, off, "NSpid:\t%u\n",  (unsigned)t->pid);
    off = PA(buf, bufsz, off, "NSpgid:\t%u\n", (unsigned)t->pgid);
    off = PA(buf, bufsz, off, "NSsid:\t%u\n",  (unsigned)t->sid);
    off = PA(buf, bufsz, off, "VmPeak:\t%8llu kB\n", (unsigned long long)(vm_size/1024));
    off = PA(buf, bufsz, off, "VmSize:\t%8llu kB\n", (unsigned long long)(vm_size/1024));
    off = PA(buf, bufsz, off, "VmLck:\t       0 kB\n");
    off = PA(buf, bufsz, off, "VmPin:\t       0 kB\n");
    off = PA(buf, bufsz, off, "VmHWM:\t%8llu kB\n", (unsigned long long)(vm_rss/1024));
    off = PA(buf, bufsz, off, "VmRSS:\t%8llu kB\n", (unsigned long long)(vm_rss/1024));
    off = PA(buf, bufsz, off, "RssAnon:\t%8llu kB\n", (unsigned long long)(vm_rss/2/1024));
    off = PA(buf, bufsz, off, "RssFile:\t%8llu kB\n", (unsigned long long)(vm_rss/2/1024));
    off = PA(buf, bufsz, off, "RssShmem:\t       0 kB\n");
    off = PA(buf, bufsz, off, "VmData:\t%8llu kB\n", (unsigned long long)(vm_data/1024));
    off = PA(buf, bufsz, off, "VmStk:\t%8llu kB\n", (unsigned long long)(vm_stk/1024));
    off = PA(buf, bufsz, off, "VmExe:\t%8llu kB\n", (unsigned long long)(vm_exe/1024));
    off = PA(buf, bufsz, off, "VmLib:\t       0 kB\n");
    off = PA(buf, bufsz, off, "VmPTE:\t       0 kB\n");
    off = PA(buf, bufsz, off, "VmSwap:\t       0 kB\n");
    off = PA(buf, bufsz, off, "HugetlbPages:\t       0 kB\n");
    off = PA(buf, bufsz, off, "CoreDumping:\t0\n");
    off = PA(buf, bufsz, off, "THP_enabled:\t1\n");
    off = PA(buf, bufsz, off, "Threads:\t1\n");
    off = PA(buf, bufsz, off, "SigQ:\t0/31717\n");
    char hex[20];
    fmt_hex16((uint64_t)t->sig_pending, hex);
    off = PA(buf, bufsz, off, "SigPnd:\t%s\n", hex);
    off = PA(buf, bufsz, off, "ShdPnd:\t0000000000000000\n");
    fmt_hex16((uint64_t)t->sig_mask, hex);
    off = PA(buf, bufsz, off, "SigBlk:\t%s\n", hex);
    off = PA(buf, bufsz, off, "SigIgn:\t0000000000000000\n");
    off = PA(buf, bufsz, off, "SigCgt:\t0000000000000000\n");
    fmt_hex16((uint64_t)t->cred.cap_inheritable, hex);
    off = PA(buf, bufsz, off, "CapInh:\t%s\n", hex);
    fmt_hex16((uint64_t)t->cred.cap_permitted, hex);
    off = PA(buf, bufsz, off, "CapPrm:\t%s\n", hex);
    fmt_hex16((uint64_t)t->cred.cap_effective, hex);
    off = PA(buf, bufsz, off, "CapEff:\t%s\n", hex);
    fmt_hex16((uint64_t)t->cred.cap_bounding, hex);
    off = PA(buf, bufsz, off, "CapBnd:\t%s\n", hex);
    off = PA(buf, bufsz, off, "CapAmb:\t0000000000000000\n");
    off = PA(buf, bufsz, off, "NoNewPrivs:\t%u\n", (unsigned)(t->no_new_privs ? 1 : 0));
    off = PA(buf, bufsz, off, "Seccomp:\t%u\n", (unsigned)t->seccomp_mode);
    off = PA(buf, bufsz, off, "Seccomp_filters:\t0\n");
    off = PA(buf, bufsz, off, "Speculation_Store_Bypass:\tthread vulnerable\n");
    off = PA(buf, bufsz, off, "SpeculationIndirectBranch:\talways enabled\n");
    uint32_t ncpu = smp_cpu_count(); if (!ncpu) ncpu = 1;
    off = PA(buf, bufsz, off, "Cpus_allowed:\t%08x\n", (1u << ncpu) - 1u);
    off = PA(buf, bufsz, off, "Cpus_allowed_list:\t0-%u\n", ncpu - 1);
    off = PA(buf, bufsz, off, "Mems_allowed:\t00000001\n");
    off = PA(buf, bufsz, off, "Mems_allowed_list:\t0\n");
    off = PA(buf, bufsz, off, "voluntary_ctxt_switches:\t100\n");
    off = PA(buf, bufsz, off, "nonvoluntary_ctxt_switches:\t50\n");
    return (ssize_t)off;
}

/* ── /proc/<pid>/maps ─────────────────────────────────────────────────────── */
static ssize_t gen_maps(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;

    size_t off = 0;
    for (vma_t *vma = t->vma_list; vma; vma = vma->next) {
        char start_hex[20], end_hex[20];

        #define HEX(val, dst) do { \
            uint64_t v = (val); char *p = (dst) + 16; *p = '\0'; \
            for (int i = 0; i < 16; i++) { \
                int d = v & 0xF; *(--p) = (d < 10) ? '0'+d : 'a'+(d-10); v >>= 4; \
            } \
        } while(0)

        HEX(vma->start, start_hex);
        HEX(vma->end, end_hex);
        #undef HEX

        char perms[5];
        perms[0] = (vma->flags & VMA_READ)  ? 'r' : '-';
        perms[1] = (vma->flags & VMA_WRITE) ? 'w' : '-';
        perms[2] = (vma->flags & VMA_EXEC)  ? 'x' : '-';
        perms[3] = 'p'; perms[4] = '\0';

        const char *label = "";
        if (vma->flags & VMA_STACK) label = " [stack]";
        else if (vma->flags & VMA_HEAP) label = " [heap]";

        off = PA(buf, bufsz, off, "%s-%s %s 00000000 00:00 0     %s\n",
                 start_hex, end_hex, perms, label);
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

/* ── /proc/<pid>/stat ─────────────────────────────────────────────────────── */
static ssize_t gen_pid_stat(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;

    uint64_t ticks = sched_get_ticks();
    uint64_t utime = ticks / 2, stime = ticks / 2;
    uint64_t start = (ticks > 100) ? ticks - 100 : 1;
    uint64_t vsize = 0;
    for (vma_t *v = t->vma_list; v; v = v->next) vsize += v->end - v->start;
    if (!vsize) vsize = 16ULL*1024*1024;
    uint64_t rss = vsize / PAGE_SIZE / 4;

    int n = ksnprintf(buf, bufsz,
        "%u (%s) %c %u %u %u 0 -1 4194304 0 0 0 0 %llu %llu 0 0 20 0 1 0 %llu %llu %llu"
        " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
        t->pid, t->name, proc_state_letter(t->state),
        t->ppid, t->pgid, t->sid,
        (unsigned long long)utime, (unsigned long long)stime,
        (unsigned long long)(start/10),
        (unsigned long long)vsize, (unsigned long long)rss);
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/<pid>/cmdline ──────────────────────────────────────────────────── */
static ssize_t gen_pid_cmdline(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t) return -ESRCH;
    size_t nlen = strlen(t->name);
    if (nlen + 1 >= bufsz) nlen = bufsz - 2;
    memcpy(buf, t->name, nlen);
    buf[nlen] = '\0';
    return (ssize_t)(nlen + 1);
}

/* ── /proc/<pid>/io ───────────────────────────────────────────────────────── */
static ssize_t gen_pid_io(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid); if (!t) return -ESRCH; (void)t;
    size_t off = 0;
    off = PA(buf, bufsz, off, "rchar: 0\n");
    off = PA(buf, bufsz, off, "wchar: 0\n");
    off = PA(buf, bufsz, off, "syscr: 0\n");
    off = PA(buf, bufsz, off, "syscw: 0\n");
    off = PA(buf, bufsz, off, "read_bytes: 0\n");
    off = PA(buf, bufsz, off, "write_bytes: 0\n");
    off = PA(buf, bufsz, off, "cancelled_write_bytes: 0\n");
    return (ssize_t)off;
}

/* ── /proc/<pid>/cgroup ───────────────────────────────────────────────────── */
static ssize_t gen_pid_cgroup(uint32_t pid, char *buf, size_t bufsz) {
    (void)pid;
    int n = ksnprintf(buf, bufsz, "0::/\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/<pid>/oom_score_adj ────────────────────────────────────────────── */
static ssize_t gen_pid_oom_adj(uint32_t pid, char *buf, size_t bufsz) {
    (void)pid;
    int n = ksnprintf(buf, bufsz, "0\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/<pid>/wchan ────────────────────────────────────────────────────── */
static ssize_t gen_pid_wchan(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid); if (!t) return -ESRCH;
    const char *wc = (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
                     ? "poll_schedule_timeout" : "0";
    int n = ksnprintf(buf, bufsz, "%s\n", wc);
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/<pid>/smaps ────────────────────────────────────────────────────── */
static ssize_t gen_pid_smaps(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid); if (!t) return -ESRCH;
    size_t off = 0;
    for (vma_t *vma = t->vma_list; vma; vma = vma->next) {
        char sh[17], eh[17];
        uint64_t sv = vma->start, ev = vma->end;
        for (int i=15;i>=0;i--){ int d=sv&0xF; sh[i]=(d<10)?'0'+d:'a'+(d-10); sv>>=4; } sh[16]=0;
        for (int i=15;i>=0;i--){ int d=ev&0xF; eh[i]=(d<10)?'0'+d:'a'+(d-10); ev>>=4; } eh[16]=0;
        char perms[5];
        perms[0]=(vma->flags&VMA_READ)?'r':'-';
        perms[1]=(vma->flags&VMA_WRITE)?'w':'-';
        perms[2]=(vma->flags&VMA_EXEC)?'x':'-';
        perms[3]='p'; perms[4]=0;
        uint64_t sz=(vma->end-vma->start)/1024;
        off = PA(buf,bufsz,off,"%s-%s %s 00000000 00:00 0\n",sh,eh,perms);
        off = PA(buf,bufsz,off,"Size:           %8llu kB\n",(unsigned long long)sz);
        off = PA(buf,bufsz,off,"KernelPageSize:        4 kB\nMMUPageSize:           4 kB\n");
        off = PA(buf,bufsz,off,"Rss:            %8llu kB\n",(unsigned long long)(sz/4));
        off = PA(buf,bufsz,off,"Pss:            %8llu kB\n",(unsigned long long)(sz/4));
        off = PA(buf,bufsz,off,"Shared_Clean:          0 kB\nShared_Dirty:          0 kB\n");
        off = PA(buf,bufsz,off,"Private_Clean:  %8llu kB\n",(unsigned long long)(sz/8));
        off = PA(buf,bufsz,off,"Private_Dirty:  %8llu kB\n",(unsigned long long)(sz/8));
        off = PA(buf,bufsz,off,"Referenced:     %8llu kB\n",(unsigned long long)(sz/4));
        off = PA(buf,bufsz,off,"Anonymous:      %8llu kB\n",(unsigned long long)(sz/4));
        off = PA(buf,bufsz,off,"LazyFree:              0 kB\nAnonHugePages:         0 kB\n");
        off = PA(buf,bufsz,off,"ShmemPmdMapped:        0 kB\nFilePmdMapped:         0 kB\n");
        off = PA(buf,bufsz,off,"Shared_Hugetlb:        0 kB\nPrivate_Hugetlb:       0 kB\n");
        off = PA(buf,bufsz,off,"Swap:                  0 kB\nSwapPss:               0 kB\nLocked:                0 kB\n");
        off = PA(buf,bufsz,off,"THPeligible:    0\n");
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

/* ── /proc/net/dev ────────────────────────────────────────────────────────── */
static ssize_t gen_net_dev(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    for (int i = 0; i < netdev_count(); i++) {
        netdev_t *dev = netdev_get_nth(i);
        if (!dev) continue;
        off = PA(buf, bufsz, off,
            "%6s: %7llu %7llu    0    0    0     0          0         0"
            " %7llu %7llu    0    0    0     0       0          0\n",
            dev->name, 0ULL, 0ULL, 0ULL, 0ULL);
    }
    return (ssize_t)off;
}

/* ── /proc/net/tcp, udp, unix ─────────────────────────────────────────────── */
static ssize_t gen_net_tcp(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off,
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    return (ssize_t)off;
}
static ssize_t gen_net_udp(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off,
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    return (ssize_t)off;
}
static ssize_t gen_net_unix(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    return (ssize_t)off;
}

/* ── /proc/net/arp ────────────────────────────────────────────────────────── */
static ssize_t gen_net_arp(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off,
        "IP address       HW type     Flags       HW address            Mask     Device\n");
    for (int i = 0; i < netdev_count(); i++) {
        netdev_t *d = netdev_get_nth(i); if (!d || !d->ip_addr) continue;
        uint32_t ip = d->ip_addr;
        uint8_t *m = d->mac;
        unsigned a=(ip>>24)&0xff, b=(ip>>16)&0xff, c=(ip>>8)&0xff, dd=ip&0xff;
        off = PA(buf, bufsz, off,
            "%u.%u.%u.%u       0x1         0x2         %02x:%02x:%02x:%02x:%02x:%02x *        %s\n",
            a,b,c,dd, m[0],m[1],m[2],m[3],m[4],m[5], d->name);
    }
    return (ssize_t)off;
}

/* ── /proc/net/route ──────────────────────────────────────────────────────── */
static ssize_t gen_net_route(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off,
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    for (int i = 0; i < netdev_count(); i++) {
        netdev_t *d = netdev_get_nth(i); if (!d || !d->ip_addr) continue;
        uint32_t mask = d->netmask ? d->netmask : 0xFFFFFF00U;
        uint32_t dest = d->ip_addr & mask;
        char hd[9], hm[9], hgw[9];
        fmt_hex8(dest, hd); fmt_hex8(0u, hgw); fmt_hex8(mask, hm);
        off = PA(buf, bufsz, off, "%s\t%s\t%s\t0001\t0\t0\t0\t%s\t0\t0\t0\n",
                d->name, hd, hgw, hm);
        if (d->gateway) {
            char hgw2[9]; fmt_hex8(d->gateway, hgw2);
            off = PA(buf, bufsz, off, "%s\t00000000\t%s\t0003\t0\t0\t100\t00000000\t0\t0\t0\n",
                    d->name, hgw2);
        }
    }
    return (ssize_t)off;
}

/* ── /proc/meminfo ────────────────────────────────────────────────────────── */
static ssize_t gen_meminfo(char *buf, size_t bufsz) {
    uint64_t total_kb = (pmm_get_total_pages() * PAGE_SIZE) / 1024;
    uint64_t free_kb  = (pmm_get_free_pages()  * PAGE_SIZE) / 1024;
    uint64_t used_kb  = (total_kb > free_kb) ? total_kb - free_kb : 0;
    uint64_t cache_kb = used_kb / 4, buf_kb = used_kb / 8;
    uint64_t avail_kb = free_kb + cache_kb;

    size_t off = 0;
    off = PA(buf, bufsz, off, "MemTotal:       %8llu kB\n", (unsigned long long)total_kb);
    off = PA(buf, bufsz, off, "MemFree:        %8llu kB\n", (unsigned long long)free_kb);
    off = PA(buf, bufsz, off, "MemAvailable:   %8llu kB\n", (unsigned long long)avail_kb);
    off = PA(buf, bufsz, off, "Buffers:        %8llu kB\n", (unsigned long long)buf_kb);
    off = PA(buf, bufsz, off, "Cached:         %8llu kB\n", (unsigned long long)cache_kb);
    off = PA(buf, bufsz, off, "SwapCached:            0 kB\n");
    off = PA(buf, bufsz, off, "Active:         %8llu kB\n", (unsigned long long)(used_kb/2));
    off = PA(buf, bufsz, off, "Inactive:       %8llu kB\n", (unsigned long long)(used_kb/2));
    off = PA(buf, bufsz, off, "Active(anon):   %8llu kB\n", (unsigned long long)(used_kb/4));
    off = PA(buf, bufsz, off, "Inactive(anon): %8llu kB\n", (unsigned long long)(used_kb/4));
    off = PA(buf, bufsz, off, "Active(file):   %8llu kB\n", (unsigned long long)(used_kb/4));
    off = PA(buf, bufsz, off, "Inactive(file): %8llu kB\n", (unsigned long long)(used_kb/4));
    off = PA(buf, bufsz, off, "Unevictable:           0 kB\nMlocked:               0 kB\n");
    off = PA(buf, bufsz, off, "SwapTotal:             0 kB\nSwapFree:              0 kB\n");
    off = PA(buf, bufsz, off, "Dirty:                 0 kB\nWriteback:             0 kB\n");
    off = PA(buf, bufsz, off, "AnonPages:      %8llu kB\n", (unsigned long long)(used_kb/4));
    off = PA(buf, bufsz, off, "Mapped:         %8llu kB\n", (unsigned long long)(used_kb/8));
    off = PA(buf, bufsz, off, "Shmem:                 0 kB\n");
    off = PA(buf, bufsz, off, "KReclaimable:   %8llu kB\n", (unsigned long long)(used_kb/16));
    off = PA(buf, bufsz, off, "Slab:           %8llu kB\n", (unsigned long long)(used_kb/8));
    off = PA(buf, bufsz, off, "SReclaimable:   %8llu kB\n", (unsigned long long)(used_kb/16));
    off = PA(buf, bufsz, off, "SUnreclaim:     %8llu kB\n", (unsigned long long)(used_kb/16));
    off = PA(buf, bufsz, off, "KernelStack:        3072 kB\nPageTables:           64 kB\n");
    off = PA(buf, bufsz, off, "NFS_Unstable:          0 kB\nBounce:                0 kB\n");
    off = PA(buf, bufsz, off, "WritebackTmp:          0 kB\n");
    off = PA(buf, bufsz, off, "CommitLimit:    %8llu kB\n", (unsigned long long)(total_kb/2));
    off = PA(buf, bufsz, off, "Committed_AS:   %8llu kB\n", (unsigned long long)used_kb);
    off = PA(buf, bufsz, off, "VmallocTotal:   34359738367 kB\nVmallocUsed:           0 kB\n");
    off = PA(buf, bufsz, off, "VmallocChunk:          0 kB\nPercpu:               48 kB\n");
    off = PA(buf, bufsz, off, "HardwareCorrupted:     0 kB\nAnonHugePages:         0 kB\n");
    off = PA(buf, bufsz, off, "ShmemHugePages:        0 kB\nShmemPmdMapped:        0 kB\n");
    off = PA(buf, bufsz, off, "FileHugePages:         0 kB\nFilePmdMapped:         0 kB\n");
    off = PA(buf, bufsz, off, "HugePages_Total:       0\nHugePages_Free:        0\n");
    off = PA(buf, bufsz, off, "HugePages_Rsvd:        0\nHugePages_Surp:        0\n");
    off = PA(buf, bufsz, off, "Hugepagesize:       2048 kB\nHugetlb:               0 kB\n");
    off = PA(buf, bufsz, off, "DirectMap4k:       16384 kB\n");
    off = PA(buf, bufsz, off, "DirectMap2M:    %8llu kB\n", (unsigned long long)total_kb);
    off = PA(buf, bufsz, off, "DirectMap1G:           0 kB\n");
    return (ssize_t)off;
}

/* ── Uptime, cpuinfo, version, loadavg, stat ─────────────────────────────── */
static ssize_t gen_uptime(char *buf, size_t bufsz) {
    uint64_t ticks = sched_get_ticks();
    uint64_t sec = ticks / 1000, frac = (ticks % 1000) / 10;
    int n = ksnprintf(buf, bufsz, "%llu.%02llu %llu.%02llu\n",
                      (unsigned long long)sec, (unsigned long long)frac,
                      (unsigned long long)sec, (unsigned long long)frac);
    return (n < 0) ? -EIO : (ssize_t)n;
}

static ssize_t gen_cpuinfo(char *buf, size_t bufsz) {
    size_t off = 0;
    uint32_t ncpu = smp_cpu_count(); if (!ncpu) ncpu = 1;
    for (uint32_t i = 0; i < ncpu; i++) {
        off = PA(buf,bufsz,off,"processor\t: %u\n", i);
        off = PA(buf,bufsz,off,"vendor_id\t: GenuineIntel\n");
        off = PA(buf,bufsz,off,"cpu family\t: 6\n");
        off = PA(buf,bufsz,off,"model\t\t: 94\n");
        off = PA(buf,bufsz,off,"model name\t: EXO_OS Virtual CPU @ 1.000GHz\n");
        off = PA(buf,bufsz,off,"stepping\t: 3\n");
        off = PA(buf,bufsz,off,"microcode\t: 0x1\n");
        off = PA(buf,bufsz,off,"cpu MHz\t\t: 1000.000\n");
        off = PA(buf,bufsz,off,"cache size\t: 8192 KB\n");
        off = PA(buf,bufsz,off,"physical id\t: 0\nsiblings\t: %u\n", ncpu);
        off = PA(buf,bufsz,off,"core id\t\t: %u\ncpu cores\t: %u\n", i, ncpu);
        off = PA(buf,bufsz,off,"apicid\t\t: %u\ninitial apicid\t: %u\n", i, i);
        off = PA(buf,bufsz,off,"fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: 22\nwp\t\t: yes\n");
        off = PA(buf,bufsz,off,"flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx rdtscp lm constant_tsc nopl xtopology tsc_reliable nonstop_tsc cpu_pni pclmulqdq ssse3 fma cx16 sse4_1 sse4_2 x2apic movbe popcnt aes xsave avx f16c rdrand hypervisor lahf_lm abm 3dnowprefetch\n");
        off = PA(buf,bufsz,off,"bogomips\t: 2000.00\n");
        off = PA(buf,bufsz,off,"clflush size\t: 64\ncache_alignment\t: 64\n");
        off = PA(buf,bufsz,off,"address sizes\t: 48 bits physical, 48 bits virtual\n");
        off = PA(buf,bufsz,off,"power management:\n\n");
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

static ssize_t gen_version(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz,
        "Linux version 5.15.0-exo (root@exo) (clang 15.0.0) #1 SMP Mon Jan  1 00:00:00 UTC 2024\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

static ssize_t gen_loadavg(char *buf, size_t bufsz) {
    uint32_t run = 0, total = 0;
    for (uint32_t i = 0; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t || t->state == TASK_DEAD) continue;
        total++;
        if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) run++;
    }
    task_t *cur = sched_current();
    uint32_t last_pid = cur ? cur->pid : 1;
    int n = ksnprintf(buf, bufsz, "0.00 0.00 0.00 %u/%u %u\n", run, total, last_pid);
    return (n < 0) ? -EIO : (ssize_t)n;
}

static ssize_t gen_stat(char *buf, size_t bufsz) {
    uint32_t ncpu = smp_cpu_count(); if (!ncpu) ncpu = 1;
    uint64_t ticks = sched_get_ticks();
    uint64_t jiffies = ticks / 10; /* ms → jiffies @100Hz */
    size_t off = 0;
    off = PA(buf, bufsz, off, "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n",
             (unsigned long long)jiffies, (unsigned long long)(jiffies*3));
    for (uint32_t i = 0; i < ncpu; i++)
        off = PA(buf, bufsz, off, "cpu%u %llu 0 0 %llu 0 0 0 0 0 0\n",
                 i, (unsigned long long)(jiffies/ncpu), (unsigned long long)(jiffies*3/ncpu));
    off = PA(buf, bufsz, off, "intr 0\nctxt 0\nbtime 0\n");
    off = PA(buf, bufsz, off, "processes %u\n", TASK_TABLE_SIZE);
    uint32_t run = 0, blk = 0;
    for (uint32_t i = 0; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t || t->state == TASK_DEAD) continue;
        if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) run++;
        else if (t->state == TASK_BLOCKED) blk++;
    }
    off = PA(buf, bufsz, off, "procs_running %u\n", run ? run : 1);
    off = PA(buf, bufsz, off, "procs_blocked %u\n", blk);
    off = PA(buf, bufsz, off, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
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
            strncpy(dev_path, "none", sizeof(dev_path)-1); dev_path[sizeof(dev_path)-1]='\0';
        } else {
            ksnprintf(dev_path, sizeof(dev_path), "/dev/%s", dev);
        }
        const char *opts;
        if      (strcmp(mnts[i].fs_name,"ext2")==0)  opts="rw,relatime,errors=remount-ro";
        else if (strcmp(mnts[i].fs_name,"fat32")==0) opts="rw,relatime,fmask=0022,dmask=0022";
        else    opts = "rw,relatime";
        off = PA(buf, bufsz, off, "%s %s %s %s 0 0\n",
                 dev_path, mnts[i].path, mnts[i].fs_name, opts);
        if (off + 1 >= bufsz) break;
    }
    return (ssize_t)off;
}

static ssize_t gen_partitions(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off, "major minor  #blocks  name\n\n");
    int n = blkdev_count();
    for (int i = 0; i < n; i++) {
        blkdev_t *d = blkdev_get_nth(i); if (!d) continue;
        uint64_t bk = (d->block_count * d->block_size) / 1024;
        off = PA(buf, bufsz, off, " 254 %5d %8llu %s\n", i, (unsigned long long)bk, d->name);
    }
    return (ssize_t)off;
}

static ssize_t gen_cmdline(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz,
        "root=/dev/vda2 rw console=ttyS0,115200 console=tty0 quiet splash\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

static ssize_t gen_filesystems(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf, bufsz, off, "\text2\n\tvfat\n");
    off = PA(buf, bufsz, off, "nodev\ttmpfs\nnodev\tprocfs\nnodev\tsysfs\n");
    off = PA(buf, bufsz, off, "nodev\tdevfs\nnodev\tdevpts\nnodev\tpipefs\n");
    off = PA(buf, bufsz, off, "nodev\tsockfs\nnodev\tbdev\nnodev\tanon_inodefs\n");
    return (ssize_t)off;
}

/* ── /proc/diskstats ──────────────────────────────────────────────────────── */
static ssize_t gen_diskstats(char *buf, size_t bufsz) {
    size_t off = 0;
    int n = blkdev_count();
    for (int i = 0; i < n; i++) {
        blkdev_t *d = blkdev_get_nth(i); if (!d) continue;
        off = PA(buf, bufsz, off,
            " 254 %5d %s 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", i, d->name);
    }
    return (ssize_t)off;
}

/* ── /proc/interrupts ─────────────────────────────────────────────────────── */
static ssize_t gen_interrupts(char *buf, size_t bufsz) {
    uint32_t ncpu = smp_cpu_count(); if (!ncpu) ncpu = 1;
    size_t off = 0;
    off = PA(buf, bufsz, off, "           ");
    for (uint32_t i = 0; i < ncpu; i++) off = PA(buf, bufsz, off, "      CPU%u", i);
    off = PA(buf, bufsz, off, "\n");
    off = PA(buf, bufsz, off, "  0:");
    for (uint32_t i = 0; i < ncpu; i++) off = PA(buf, bufsz, off, "          0");
    off = PA(buf, bufsz, off, "   IO-APIC    2-edge      timer\n");
    off = PA(buf, bufsz, off, "  1:");
    for (uint32_t i = 0; i < ncpu; i++) off = PA(buf, bufsz, off, "          0");
    off = PA(buf, bufsz, off, "   IO-APIC    1-edge      i8042\n");
    off = PA(buf, bufsz, off, " 14:");
    for (uint32_t i = 0; i < ncpu; i++) off = PA(buf, bufsz, off, "          0");
    off = PA(buf, bufsz, off, "   IO-APIC   14-edge      ata_piix\n");
    off = PA(buf, bufsz, off, "NMI:");
    for (uint32_t i = 0; i < ncpu; i++) off = PA(buf, bufsz, off, "          0");
    off = PA(buf, bufsz, off, "   Non-maskable interrupts\n");
    off = PA(buf, bufsz, off, "LOC:");
    for (uint32_t i = 0; i < ncpu; i++) off = PA(buf, bufsz, off, "          0");
    off = PA(buf, bufsz, off, "   Local timer interrupts\n");
    off = PA(buf, bufsz, off, "ERR:          0\nMIS:          0\n");
    return (ssize_t)off;
}

/* ── /proc/swaps ──────────────────────────────────────────────────────────── */
static ssize_t gen_swaps(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz,
        "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/devices ────────────────────────────────────────────────────────── */
static ssize_t gen_devices(char *buf, size_t bufsz) {
    size_t off = 0;
    off = PA(buf,bufsz,off,"Character devices:\n  1 mem\n  4 tty\n  5 /dev/tty\n");
    off = PA(buf,bufsz,off,"  5 /dev/console\n  5 /dev/ptmx\n 10 misc\n136 pts\n");
    off = PA(buf,bufsz,off,"Block devices:\n254 virtblk\n");
    return (ssize_t)off;
}

/* ── /proc/misc ───────────────────────────────────────────────────────────── */
static ssize_t gen_misc(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "  1 psaux\n 58 network_throughput\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/sys/kernel/hostname ────────────────────────────────────────────── */
static ssize_t gen_sys_hostname(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "%s\n", g_hostname);
    return (n < 0) ? -EIO : (ssize_t)n;
}
/* ── /proc/sys/fs/file-max ────────────────────────────────────────────────── */
static ssize_t gen_sys_filemax(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "%u\n", (unsigned)(TASK_TABLE_SIZE * FD_TABLE_SIZE));
    return (n < 0) ? -EIO : (ssize_t)n;
}
/* ── /proc/sys/vm/overcommit_memory ──────────────────────────────────────── */
static ssize_t gen_sys_overcommit(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "0\n"); return (n < 0) ? -EIO : (ssize_t)n;
}
/* ── /proc/sys/net/ipv4/ip_forward ───────────────────────────────────────── */
static ssize_t gen_sys_forward(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz, "0\n"); return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/<pid>/environ ─────────────────────────────────────────────────── */
static ssize_t gen_pid_environ(uint32_t pid, char *buf, size_t bufsz) {
    task_t *t = task_lookup(pid);
    if (!t || !t->env_block || !t->env_block_size) return 0;
    size_t len = t->env_block_size < bufsz ? t->env_block_size : bufsz;
    memcpy(buf, t->env_block, len);
    return (ssize_t)len;
}

/* ── /proc/iomem ─────────────────────────────────────────────────────────── */
static ssize_t gen_iomem(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "00000000-00000fff : Reserved\n");
    off = PA(buf,bufsz,off, "00001000-0009ffff : System RAM\n");
    off = PA(buf,bufsz,off, "000a0000-000bffff : PCI Bus 0000:00\n");
    off = PA(buf,bufsz,off, "  000a0000-000bffff : Video RAM area\n");
    off = PA(buf,bufsz,off, "000c0000-000c7fff : Video ROM\n");
    off = PA(buf,bufsz,off, "000f0000-000fffff : System ROM\n");
    off = PA(buf,bufsz,off, "00100000-7fffffff : System RAM\n");
    off = PA(buf,bufsz,off, "fd000000-fdffffff : 0000:00:02.0\n");
    off = PA(buf,bufsz,off, "  fd000000-fdffffff : vga\n");
    off = PA(buf,bufsz,off, "fe000000-fe003fff : 0000:00:03.0\n");
    off = PA(buf,bufsz,off, "fec00000-fec00fff : Reserved\n");
    off = PA(buf,bufsz,off, "  fec00000-fec00fff : IOAPIC 0\n");
    off = PA(buf,bufsz,off, "fed00000-fed003ff : HPET 0\n");
    off = PA(buf,bufsz,off, "fee00000-fee00fff : Local APIC\n");
    off = PA(buf,bufsz,off, "fffc0000-ffffffff : Reserved\n");
    return (ssize_t)off;
}

/* ── /proc/ioports ───────────────────────────────────────────────────────── */
static ssize_t gen_ioports(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "0000-0cf7 : PCI Bus 0000:00\n");
    off = PA(buf,bufsz,off, "  0000-001f : dma1\n");
    off = PA(buf,bufsz,off, "  0020-0021 : pic1\n");
    off = PA(buf,bufsz,off, "  0040-0043 : timer0\n");
    off = PA(buf,bufsz,off, "  0060-0060 : keyboard\n");
    off = PA(buf,bufsz,off, "  0064-0064 : keyboard\n");
    off = PA(buf,bufsz,off, "  0070-0077 : rtc0\n");
    off = PA(buf,bufsz,off, "  0080-008f : dma page reg\n");
    off = PA(buf,bufsz,off, "  00a0-00a1 : pic2\n");
    off = PA(buf,bufsz,off, "  00c0-00df : dma2\n");
    off = PA(buf,bufsz,off, "  00f0-00ff : fpu\n");
    off = PA(buf,bufsz,off, "  03f8-03ff : serial\n");
    off = PA(buf,bufsz,off, "0cf8-0cff : PCI conf1\n");
    off = PA(buf,bufsz,off, "0d00-ffff : PCI Bus 0000:00\n");
    off = PA(buf,bufsz,off, "  afe0-afe3 : ACPI GPE0_BLK\n");
    off = PA(buf,bufsz,off, "  b000-b03f : virtio\n");
    return (ssize_t)off;
}

/* ── /proc/buddyinfo ─────────────────────────────────────────────────────── */
static ssize_t gen_buddyinfo(char *buf, size_t bufsz) {
    int n = ksnprintf(buf, bufsz,
        "Node 0, zone   Normal   1   1   1   0   1   1   1   0   1   1   3\n");
    return (n < 0) ? -EIO : (ssize_t)n;
}

/* ── /proc/slabinfo ──────────────────────────────────────────────────────── */
static ssize_t gen_slabinfo(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "slabinfo - version: 2.1\n");
    off = PA(buf,bufsz,off, "# name            <active_objs> <num_objs> <objsize> "
             "<objperslab> <pagesperslab> : tunables <limit> <batchcount> "
             "<sharedfactor> : slabdata <active_slabs> <num_slabs> <sharedavail>\n");
    off = PA(buf,bufsz,off, "kmalloc-4096          32     32   4096    8    8 : tunables    0    0    0 : slabdata      4      4      0\n");
    off = PA(buf,bufsz,off, "kmalloc-2048          64     64   2048   16    8 : tunables    0    0    0 : slabdata      4      4      0\n");
    off = PA(buf,bufsz,off, "kmalloc-1024         128    128   1024   16    4 : tunables    0    0    0 : slabdata      8      8      0\n");
    off = PA(buf,bufsz,off, "kmalloc-512          256    256    512   16    2 : tunables    0    0    0 : slabdata     16     16      0\n");
    off = PA(buf,bufsz,off, "kmalloc-256          512    512    256   16    1 : tunables    0    0    0 : slabdata     32     32      0\n");
    off = PA(buf,bufsz,off, "kmalloc-128         1024   1024    128   32    1 : tunables    0    0    0 : slabdata     32     32      0\n");
    off = PA(buf,bufsz,off, "kmalloc-64          2048   2048     64   64    1 : tunables    0    0    0 : slabdata     32     32      0\n");
    return (ssize_t)off;
}

/* ── /proc/zoneinfo ──────────────────────────────────────────────────────── */
static ssize_t gen_zoneinfo(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "Node 0, zone   Normal\n");
    off = PA(buf,bufsz,off, "  per-node stats\n");
    off = PA(buf,bufsz,off, "      nr_inactive_anon 0\n");
    off = PA(buf,bufsz,off, "      nr_active_anon 0\n");
    off = PA(buf,bufsz,off, "      nr_inactive_file 0\n");
    off = PA(buf,bufsz,off, "      nr_active_file 0\n");
    off = PA(buf,bufsz,off, "  pages free     0\n");
    off = PA(buf,bufsz,off, "        min      0\n");
    off = PA(buf,bufsz,off, "        low      0\n");
    off = PA(buf,bufsz,off, "        high     0\n");
    off = PA(buf,bufsz,off, "        spanned  0\n");
    off = PA(buf,bufsz,off, "        present  0\n");
    off = PA(buf,bufsz,off, "        managed  0\n");
    return (ssize_t)off;
}

/* ── /proc/crypto ────────────────────────────────────────────────────────── */
static ssize_t gen_crypto(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "name         : sha256\ndriver       : sha256-generic\n"
             "module       : kernel\npriority     : 100\nrefcnt       : 1\n"
             "selftest     : passed\ninternal     : no\ntype         : shash\n"
             "blocksize    : 64\ndigestsize   : 32\n\n");
    off = PA(buf,bufsz,off, "name         : sha1\ndriver       : sha1-generic\n"
             "module       : kernel\npriority     : 100\nrefcnt       : 1\n"
             "selftest     : passed\ninternal     : no\ntype         : shash\n"
             "blocksize    : 64\ndigestsize   : 20\n\n");
    off = PA(buf,bufsz,off, "name         : md5\ndriver       : md5-generic\n"
             "module       : builtin\npriority     : 0\nrefcnt       : 1\n"
             "selftest     : passed\ninternal     : no\ntype         : shash\n"
             "blocksize    : 64\ndigestsize   : 16\n\n");
    off = PA(buf,bufsz,off, "name         : aes\ndriver       : aes-generic\n"
             "module       : kernel\npriority     : 100\nrefcnt       : 1\n"
             "selftest     : passed\ninternal     : no\ntype         : cipher\n"
             "blocksize    : 16\nmin keysize  : 16\nmax keysize  : 32\n\n");
    return (ssize_t)off;
}

/* ── /proc/net/fib_trie ──────────────────────────────────────────────────── */
static ssize_t gen_net_fib_trie(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "Main:\n");
    off = PA(buf,bufsz,off, "  +-- 0.0.0.0/0 2 0 2\n");
    off = PA(buf,bufsz,off, "     /0 universe UNICAST\n");
    off = PA(buf,bufsz,off, "Local:\n");
    off = PA(buf,bufsz,off, "  +-- 0.0.0.0/0 2 0 2\n");
    off = PA(buf,bufsz,off, "     /32 host LOCAL\n");
    return (ssize_t)off;
}

/* ── /proc/net/fib_triestat ──────────────────────────────────────────────── */
static ssize_t gen_net_fib_triestat(char *buf, size_t bufsz) {
    int off = 0;
    off = PA(buf,bufsz,off, "Basic info: size of leaf: 48 bytes, size of tnode: 40 bytes.\n");
    off = PA(buf,bufsz,off, "Main:\n\tAver depth:     2.00\n\tMax depth:\t3\n");
    off = PA(buf,bufsz,off, "\tLeaves:\t\t4\n\tPrefixes:\t6\n\tInternal nodes:\t2\n");
    off = PA(buf,bufsz,off, "Local:\n\tAver depth:     1.40\n\tMax depth:\t2\n");
    off = PA(buf,bufsz,off, "\tLeaves:\t\t5\n\tPrefixes:\t8\n\tInternal nodes:\t1\n");
    return (ssize_t)off;
}

static vnode_t *procfs_lookup(vnode_t *dir, const char *name) {
    if (!dir || !dir->fs_data || !name) return NULL;
    procfs_node_t *pn = (procfs_node_t *)dir->fs_data;

    if (pn->type == PROC_ROOT) {
        struct { const char *n; int t; uint32_t m; } root_map[] = {
            { "net",         PROC_NET_DIR,    VFS_S_IFDIR | 0555 },
            { "sys",         PROC_SYS_DIR,    VFS_S_IFDIR | 0555 },
            { "self",        PROC_SELF,       VFS_S_IFLNK | 0777 },
            { "thread-self", PROC_SELF,       VFS_S_IFLNK | 0777 },
            { "meminfo",     PROC_MEMINFO,    VFS_S_IFREG | 0444 },
            { "uptime",      PROC_UPTIME,     VFS_S_IFREG | 0444 },
            { "cpuinfo",     PROC_CPUINFO,    VFS_S_IFREG | 0444 },
            { "version",     PROC_VERSION,    VFS_S_IFREG | 0444 },
            { "loadavg",     PROC_LOADAVG,    VFS_S_IFREG | 0444 },
            { "stat",        PROC_STAT,       VFS_S_IFREG | 0444 },
            { "mounts",      PROC_MOUNTS,     VFS_S_IFREG | 0444 },
            { "partitions",  PROC_PARTS,      VFS_S_IFREG | 0444 },
            { "cmdline",     PROC_CMDLINE,    VFS_S_IFREG | 0444 },
            { "filesystems", PROC_FILESYSTEMS,VFS_S_IFREG | 0444 },
            { "diskstats",   PROC_DISKSTATS,   VFS_S_IFREG | 0444 },
            { "interrupts",  PROC_INTERRUPTS,  VFS_S_IFREG | 0444 },
            { "swaps",       PROC_SWAPS,       VFS_S_IFREG | 0444 },
            { "devices",     PROC_DEVICES,     VFS_S_IFREG | 0444 },
            { "misc",        PROC_MISC,        VFS_S_IFREG | 0444 },
            { "iomem",       PROC_IOMEM,       VFS_S_IFREG | 0444 },
            { "ioports",     PROC_IOPORTS,     VFS_S_IFREG | 0444 },
            { "buddyinfo",   PROC_BUDDYINFO,   VFS_S_IFREG | 0444 },
            { "slabinfo",    PROC_SLABINFO,    VFS_S_IFREG | 0444 },
            { "zoneinfo",    PROC_ZONEINFO,    VFS_S_IFREG | 0444 },
            { "crypto",      PROC_CRYPTO,      VFS_S_IFREG | 0444 },
        };
        for (size_t i = 0; i < sizeof(root_map)/sizeof(root_map[0]); i++) {
            if (strcmp(name, root_map[i].n) == 0)
                return procfs_alloc_node(root_map[i].t, 0, 0, root_map[i].m);
        }
        uint32_t pid = parse_pid(name);
        if (pid != (uint32_t)-1 && task_lookup(pid))
            return procfs_alloc_node(PROC_PID_DIR, pid, 0, VFS_S_IFDIR | 0555);
        return NULL;
    }

    if (pn->type == PROC_PID_DIR) {
        struct { const char *n; int t; uint32_t m; } pid_map[] = {
            { "status",        PROC_STATUS,        VFS_S_IFREG | 0444 },
            { "maps",          PROC_MAPS,          VFS_S_IFREG | 0444 },
            { "stat",          PROC_PID_STAT,      VFS_S_IFREG | 0444 },
            { "cmdline",       PROC_PID_CMD,       VFS_S_IFREG | 0444 },
            { "exe",           PROC_PID_EXE,       VFS_S_IFLNK | 0777 },
            { "fd",            PROC_PID_FD_DIR,    VFS_S_IFDIR | 0500 },
            { "fdinfo",        PROC_PID_FDINFO_DIR,VFS_S_IFDIR | 0500 },
            { "io",            PROC_PID_IO,        VFS_S_IFREG | 0400 },
            { "cgroup",        PROC_PID_CGROUP,    VFS_S_IFREG | 0444 },
            { "oom_score_adj", PROC_PID_OOM_ADJ,    VFS_S_IFREG | 0644 },
            { "wchan",         PROC_PID_WCHAN,      VFS_S_IFREG | 0444 },
            { "smaps",         PROC_PID_SMAPS,      VFS_S_IFREG | 0444 },
            { "smaps_rollup",  PROC_PID_SMAPS,      VFS_S_IFREG | 0444 },
            { "environ",       PROC_PID_ENVIRON,    VFS_S_IFREG | 0400 },
        };
        for (size_t i = 0; i < sizeof(pid_map)/sizeof(pid_map[0]); i++) {
            if (strcmp(name, pid_map[i].n) == 0)
                return procfs_alloc_node(pid_map[i].t, pn->pid, 0, pid_map[i].m);
        }
        return NULL;
    }

    if (pn->type == PROC_PID_FD_DIR) {
        int fd_num = 0;
        for (int i = 0; name[i]; i++) {
            if (name[i] < '0' || name[i] > '9') return NULL;
            fd_num = fd_num * 10 + (name[i] - '0');
        }
        task_t *t = task_lookup(pn->pid);
        if (!t || fd_num < 0 || fd_num >= FD_TABLE_SIZE || !t->fd_table[fd_num])
            return NULL;
        return procfs_alloc_node(PROC_PID_FD_ENTRY, pn->pid, fd_num, VFS_S_IFLNK | 0777);
    }

    if (pn->type == PROC_PID_FDINFO_DIR) {
        int fd_num = 0;
        for (int i = 0; name[i]; i++) {
            if (name[i] < '0' || name[i] > '9') return NULL;
            fd_num = fd_num * 10 + (name[i] - '0');
        }
        task_t *t = task_lookup(pn->pid);
        if (!t || fd_num < 0 || fd_num >= FD_TABLE_SIZE || !t->fd_table[fd_num])
            return NULL;
        return procfs_alloc_node(PROC_PID_FDINFO_ENT, pn->pid, fd_num, VFS_S_IFREG | 0400);
    }

    if (pn->type == PROC_NET_DIR) {
        struct { const char *n; int t; } nm[] = {
            {"dev",PROC_NET_DEV}, {"tcp",PROC_NET_TCP}, {"tcp6",PROC_NET_TCP6},
            {"udp",PROC_NET_UDP}, {"udp6",PROC_NET_UDP6}, {"unix",PROC_NET_UNIX},
            {"arp",PROC_NET_ARP}, {"route",PROC_NET_ROUTE},
            {"fib_trie",PROC_NET_FIB_TRIE}, {"fib_triestat",PROC_NET_FIB_STAT},
        };
        for (size_t i = 0; i < sizeof(nm)/sizeof(nm[0]); i++)
            if (strcmp(name, nm[i].n) == 0)
                return procfs_alloc_node(nm[i].t, 0, 0, VFS_S_IFREG | 0444);
        return NULL;
    }

    if (pn->type == PROC_SYS_DIR) {
        if (strcmp(name,"kernel")==0) return procfs_alloc_node(PROC_SYS_KERNEL_DIR,0,0,VFS_S_IFDIR|0555);
        if (strcmp(name,"fs")    ==0) return procfs_alloc_node(PROC_SYS_FS_DIR,    0,0,VFS_S_IFDIR|0555);
        if (strcmp(name,"vm")    ==0) return procfs_alloc_node(PROC_SYS_VM_DIR,    0,0,VFS_S_IFDIR|0555);
        if (strcmp(name,"net")   ==0) return procfs_alloc_node(PROC_SYS_NET_DIR,   0,0,VFS_S_IFDIR|0555);
        return NULL;
    }

    if (pn->type == PROC_SYS_KERNEL_DIR) {
        if (strcmp(name,"hostname")==0)  return procfs_alloc_node(PROC_SYS_HOSTNAME,  0,0,VFS_S_IFREG|0644);
        if (strcmp(name,"ostype")==0)    return procfs_alloc_node(PROC_VERSION,        0,0,VFS_S_IFREG|0444);
        if (strcmp(name,"osrelease")==0) return procfs_alloc_node(PROC_VERSION,        0,0,VFS_S_IFREG|0444);
        if (strcmp(name,"pid_max")==0)   return procfs_alloc_node(PROC_SYS_FILEMAX,   0,0,VFS_S_IFREG|0644);
        if (strcmp(name,"printk")==0)    return procfs_alloc_node(PROC_SYS_OVERCOMMIT,0,0,VFS_S_IFREG|0644);
        if (strcmp(name,"panic")==0)     return procfs_alloc_node(PROC_SYS_OVERCOMMIT,0,0,VFS_S_IFREG|0644);
        return NULL;
    }

    if (pn->type == PROC_SYS_FS_DIR) {
        if (strcmp(name,"file-max")==0)   return procfs_alloc_node(PROC_SYS_FILEMAX,   0,0,VFS_S_IFREG|0644);
        if (strcmp(name,"file-nr")==0)    return procfs_alloc_node(PROC_SYS_FILEMAX,   0,0,VFS_S_IFREG|0444);
        if (strcmp(name,"inode-max")==0)  return procfs_alloc_node(PROC_SYS_FILEMAX,   0,0,VFS_S_IFREG|0644);
        if (strcmp(name,"inode-nr")==0)   return procfs_alloc_node(PROC_SYS_FILEMAX,   0,0,VFS_S_IFREG|0444);
        return NULL;
    }

    if (pn->type == PROC_SYS_VM_DIR) {
        /* Accept any vm/ knob, return overcommit stub */
        return procfs_alloc_node(PROC_SYS_OVERCOMMIT, 0, 0, VFS_S_IFREG|0644);
    }

    if (pn->type == PROC_SYS_NET_DIR) {
        if (strcmp(name,"ipv4")==0) return procfs_alloc_node(PROC_SYS_IPV4_DIR,0,0,VFS_S_IFDIR|0555);
        return procfs_alloc_node(PROC_SYS_OVERCOMMIT, 0, 0, VFS_S_IFREG|0644);
    }

    if (pn->type == PROC_SYS_IPV4_DIR) {
        if (strcmp(name,"ip_forward")==0)
            return procfs_alloc_node(PROC_SYS_FORWARD, 0, 0, VFS_S_IFREG|0644);
        if (strcmp(name,"conf")==0)
            return procfs_alloc_node(PROC_SYS_IPV4_DIR,0,0,VFS_S_IFDIR|0555);
        /* Stub for any other ipv4 knob */
        return procfs_alloc_node(PROC_SYS_OVERCOMMIT, 0, 0, VFS_S_IFREG|0644);
    }

    return NULL;
}

static int procfs_open(vnode_t *v, int flags) { (void)v; (void)flags; return 0; }
static int procfs_close(vnode_t *v) { (void)v; return 0; }

static ssize_t procfs_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    if (!v || !v->fs_data) return -EIO;
    procfs_node_t *pn = (procfs_node_t *)v->fs_data;

    /* fdinfo: small fixed-size, no temp alloc needed */
    if (pn->type == PROC_PID_FDINFO_ENT) {
        task_t *t = task_lookup(pn->pid); if (!t) return -ESRCH;
        file_t *f = (pn->fd >= 0 && pn->fd < FD_TABLE_SIZE) ? t->fd_table[pn->fd] : NULL;
        if (!f) return -ENOENT;
        char tmp[256];
        int n = ksnprintf(tmp, sizeof(tmp),
            "pos:\t%llu\nflags:\t%07o\nmnt_id:\t23\n",
            (unsigned long long)f->offset, (unsigned)f->flags);
        if (n < 0) return -EIO;
        if ((uint64_t)off >= (uint64_t)n) return 0;
        ssize_t avail = n - (ssize_t)off;
        if ((ssize_t)len < avail) avail = (ssize_t)len;
        memcpy(buf, tmp + off, (size_t)avail);
        return avail;
    }

    char *tmp = kmalloc(PROCFS_BUF_SIZE);
    if (!tmp) return -ENOMEM;

    ssize_t total = 0;
    switch (pn->type) {
        case PROC_STATUS:        total = gen_status(pn->pid,   tmp, PROCFS_BUF_SIZE); break;
        case PROC_MAPS:          total = gen_maps(pn->pid,     tmp, PROCFS_BUF_SIZE); break;
        case PROC_PID_STAT:      total = gen_pid_stat(pn->pid, tmp, PROCFS_BUF_SIZE); break;
        case PROC_PID_CMD:       total = gen_pid_cmdline(pn->pid,tmp,PROCFS_BUF_SIZE);break;
        case PROC_PID_IO:        total = gen_pid_io(pn->pid,   tmp, PROCFS_BUF_SIZE); break;
        case PROC_PID_CGROUP:    total = gen_pid_cgroup(pn->pid,tmp,PROCFS_BUF_SIZE); break;
        case PROC_PID_OOM_ADJ:   total = gen_pid_oom_adj(pn->pid,tmp,PROCFS_BUF_SIZE);break;
        case PROC_PID_WCHAN:     total = gen_pid_wchan(pn->pid,tmp,PROCFS_BUF_SIZE);  break;
        case PROC_PID_SMAPS:     total = gen_pid_smaps(pn->pid,tmp,PROCFS_BUF_SIZE);  break;
        case PROC_NET_DEV:       total = gen_net_dev(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_NET_TCP:
        case PROC_NET_TCP6:      total = gen_net_tcp(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_NET_UDP:
        case PROC_NET_UDP6:      total = gen_net_udp(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_NET_UNIX:      total = gen_net_unix(tmp,     PROCFS_BUF_SIZE); break;
        case PROC_NET_ARP:       total = gen_net_arp(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_NET_ROUTE:     total = gen_net_route(tmp,    PROCFS_BUF_SIZE); break;
        case PROC_MEMINFO:       total = gen_meminfo(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_UPTIME:        total = gen_uptime(tmp,       PROCFS_BUF_SIZE); break;
        case PROC_CPUINFO:       total = gen_cpuinfo(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_VERSION:       total = gen_version(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_LOADAVG:       total = gen_loadavg(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_STAT:          total = gen_stat(tmp,         PROCFS_BUF_SIZE); break;
        case PROC_MOUNTS:        total = gen_mounts(tmp,       PROCFS_BUF_SIZE); break;
        case PROC_PARTS:         total = gen_partitions(tmp,   PROCFS_BUF_SIZE); break;
        case PROC_CMDLINE:       total = gen_cmdline(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_FILESYSTEMS:   total = gen_filesystems(tmp,  PROCFS_BUF_SIZE); break;
        case PROC_DISKSTATS:     total = gen_diskstats(tmp,    PROCFS_BUF_SIZE); break;
        case PROC_INTERRUPTS:    total = gen_interrupts(tmp,   PROCFS_BUF_SIZE); break;
        case PROC_SWAPS:         total = gen_swaps(tmp,        PROCFS_BUF_SIZE); break;
        case PROC_DEVICES:       total = gen_devices(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_MISC:          total = gen_misc(tmp,         PROCFS_BUF_SIZE); break;
        case PROC_SYS_HOSTNAME:  total = gen_sys_hostname(tmp, PROCFS_BUF_SIZE); break;
        case PROC_SYS_FILEMAX:   total = gen_sys_filemax(tmp,  PROCFS_BUF_SIZE); break;
        case PROC_SYS_OVERCOMMIT:total = gen_sys_overcommit(tmp,PROCFS_BUF_SIZE);break;
        case PROC_SYS_FORWARD:   total = gen_sys_forward(tmp,  PROCFS_BUF_SIZE); break;
        case PROC_PID_ENVIRON:   total = gen_pid_environ(pn->pid,tmp,PROCFS_BUF_SIZE);break;
        case PROC_IOMEM:         total = gen_iomem(tmp,         PROCFS_BUF_SIZE); break;
        case PROC_IOPORTS:       total = gen_ioports(tmp,       PROCFS_BUF_SIZE); break;
        case PROC_BUDDYINFO:     total = gen_buddyinfo(tmp,     PROCFS_BUF_SIZE); break;
        case PROC_SLABINFO:      total = gen_slabinfo(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_ZONEINFO:      total = gen_zoneinfo(tmp,      PROCFS_BUF_SIZE); break;
        case PROC_CRYPTO:        total = gen_crypto(tmp,        PROCFS_BUF_SIZE); break;
        case PROC_NET_FIB_TRIE:  total = gen_net_fib_trie(tmp,  PROCFS_BUF_SIZE); break;
        case PROC_NET_FIB_STAT:  total = gen_net_fib_triestat(tmp,PROCFS_BUF_SIZE);break;
        default: kfree(tmp); return -EIO;
    }

    if (total < 0) { kfree(tmp); return total; }
    if ((uint64_t)off >= (uint64_t)total) { kfree(tmp); return 0; }
    ssize_t avail = total - (ssize_t)off;
    if ((ssize_t)len < avail) avail = (ssize_t)len;
    memcpy(buf, tmp + off, (size_t)avail);
    kfree(tmp);
    return avail;
}

static ssize_t procfs_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    if (!v || !v->fs_data) return -EIO;
    procfs_node_t *pn = (procfs_node_t *)v->fs_data;
    (void)off;

    if (pn->type == PROC_SYS_HOSTNAME) {
        size_t n = len < sizeof(g_hostname)-1 ? len : sizeof(g_hostname)-1;
        memcpy(g_hostname, buf, n);
        while (n > 0 && (g_hostname[n-1]=='\n' || g_hostname[n-1]=='\r')) n--;
        g_hostname[n] = '\0';
        return (ssize_t)len;
    }
    /* Any /proc/sys writable node: silently accept */
    if (pn->type == PROC_SYS_OVERCOMMIT || pn->type == PROC_SYS_FILEMAX ||
        pn->type == PROC_SYS_FORWARD    || pn->type == PROC_PID_OOM_ADJ)
        return (ssize_t)len;
    return -EACCES;
}

static int procfs_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    if (!dir || !dir->fs_data) return -1;
    procfs_node_t *pn = (procfs_node_t *)dir->fs_data;
    uint64_t idx = *cookie;

    if (pn->type == PROC_ROOT) {
        static const struct { const char *n; uint32_t t; } root_ents[] = {
            {"self",VFS_DT_LNK},{"thread-self",VFS_DT_LNK},
            {"net",VFS_DT_DIR},{"sys",VFS_DT_DIR},
            {"meminfo",VFS_DT_REG},{"uptime",VFS_DT_REG},
            {"cpuinfo",VFS_DT_REG},{"version",VFS_DT_REG},
            {"loadavg",VFS_DT_REG},{"stat",VFS_DT_REG},
            {"mounts",VFS_DT_REG},{"partitions",VFS_DT_REG},
            {"cmdline",VFS_DT_REG},{"filesystems",VFS_DT_REG},
            {"diskstats",VFS_DT_REG},{"interrupts",VFS_DT_REG},
            {"swaps",VFS_DT_REG},{"devices",VFS_DT_REG},{"misc",VFS_DT_REG},
            {"iomem",VFS_DT_REG},{"ioports",VFS_DT_REG},{"buddyinfo",VFS_DT_REG},
            {"slabinfo",VFS_DT_REG},{"zoneinfo",VFS_DT_REG},{"crypto",VFS_DT_REG},
        };
        uint64_t nstatic = sizeof(root_ents)/sizeof(root_ents[0]);
        if (idx < nstatic) {
            out->ino = 0xFFF0 - idx; out->type = root_ents[idx].t;
            strncpy(out->name, root_ents[idx].n, VFS_NAME_MAX);
            out->name[VFS_NAME_MAX] = '\0';
            *cookie = idx + 1; return 1;
        }
        uint64_t skip = idx - nstatic, found = 0;
        for (uint32_t i = 0; i < TASK_TABLE_SIZE; i++) {
            task_t *t = task_get_from_table(i);
            if (!t || t->state == TASK_DEAD) continue;
            if (found == skip) {
                out->ino = t->pid; out->type = VFS_DT_DIR;
                itoa_simple(t->pid, out->name, VFS_NAME_MAX);
                *cookie = idx + 1; return 1;
            }
            found++;
        }
        return 0;
    }

    if (pn->type == PROC_PID_DIR) {
        static const struct { const char *n; uint32_t t; } pid_ents[] = {
            {"status",VFS_DT_REG},{"maps",VFS_DT_REG},{"stat",VFS_DT_REG},
            {"cmdline",VFS_DT_REG},{"exe",VFS_DT_LNK},{"fd",VFS_DT_DIR},
            {"fdinfo",VFS_DT_DIR},{"io",VFS_DT_REG},{"cgroup",VFS_DT_REG},
            {"oom_score_adj",VFS_DT_REG},{"wchan",VFS_DT_REG},{"smaps",VFS_DT_REG},
            {"environ",VFS_DT_REG},
        };
        uint64_t n = sizeof(pid_ents)/sizeof(pid_ents[0]);
        if (idx >= n) return 0;
        out->ino = (pn->pid<<4)|(idx+1); out->type = pid_ents[idx].t;
        strncpy(out->name, pid_ents[idx].n, VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx + 1; return 1;
    }

    if (pn->type == PROC_PID_FD_DIR) {
        task_t *t = task_lookup(pn->pid); if (!t) return -1;
        uint64_t found = 0;
        for (int i = 0; i < FD_TABLE_SIZE; i++) {
            if (!t->fd_table[i]) continue;
            if (found == idx) {
                out->ino = (uint64_t)((pn->pid<<8)|i)+0x100000;
                out->type = VFS_DT_LNK;
                itoa_simple((uint64_t)i, out->name, VFS_NAME_MAX);
                *cookie = idx + 1; return 1;
            }
            found++;
        }
        return 0;
    }

    if (pn->type == PROC_PID_FDINFO_DIR) {
        task_t *t = task_lookup(pn->pid); if (!t) return -1;
        uint64_t found = 0;
        for (int i = 0; i < FD_TABLE_SIZE; i++) {
            if (!t->fd_table[i]) continue;
            if (found == idx) {
                out->ino = (uint64_t)((pn->pid<<8)|i)+0x200000;
                out->type = VFS_DT_REG;
                itoa_simple((uint64_t)i, out->name, VFS_NAME_MAX);
                *cookie = idx + 1; return 1;
            }
            found++;
        }
        return 0;
    }

    if (pn->type == PROC_NET_DIR) {
        static const char *ne[] = {"dev","tcp","tcp6","udp","udp6","unix","arp","route",
                                   "fib_trie","fib_triestat"};
        uint64_t n = sizeof(ne)/sizeof(ne[0]);
        if (idx >= n) return 0;
        out->ino = 0xFFFD-idx; out->type = VFS_DT_REG;
        strncpy(out->name, ne[idx], VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx+1; return 1;
    }

    if (pn->type == PROC_SYS_DIR) {
        static const char *se[] = {"kernel","fs","vm","net"};
        uint64_t n = sizeof(se)/sizeof(se[0]);
        if (idx >= n) return 0;
        out->ino = 0xFFF8-idx; out->type = VFS_DT_DIR;
        strncpy(out->name, se[idx], VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx+1; return 1;
    }

    if (pn->type == PROC_SYS_KERNEL_DIR) {
        static const char *ke[] = {"hostname","ostype","osrelease","pid_max","printk","panic"};
        uint64_t n = sizeof(ke)/sizeof(ke[0]);
        if (idx >= n) return 0;
        out->ino = 0xFFF0-idx; out->type = VFS_DT_REG;
        strncpy(out->name, ke[idx], VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx+1; return 1;
    }

    if (pn->type == PROC_SYS_FS_DIR) {
        static const char *fe[] = {"file-max","file-nr","inode-max","inode-nr"};
        uint64_t n = sizeof(fe)/sizeof(fe[0]);
        if (idx >= n) return 0;
        out->ino = 0xFFEF-idx; out->type = VFS_DT_REG;
        strncpy(out->name, fe[idx], VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx+1; return 1;
    }

    if (pn->type == PROC_SYS_VM_DIR) {
        static const char *ve[] = {"overcommit_memory","overcommit_ratio","swappiness","dirty_ratio"};
        uint64_t n = sizeof(ve)/sizeof(ve[0]);
        if (idx >= n) return 0;
        out->ino = 0xFFEEu - idx; out->type = VFS_DT_REG;
        strncpy(out->name, ve[idx], VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx+1; return 1;
    }

    if (pn->type == PROC_SYS_NET_DIR) {
        if (idx > 0) return 0;
        out->ino = 0xFFED; out->type = VFS_DT_DIR;
        strncpy(out->name, "ipv4", VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = 1; return 1;
    }

    if (pn->type == PROC_SYS_IPV4_DIR) {
        static const char *ie[] = {"ip_forward","conf","tcp_rmem","tcp_wmem","ip_local_port_range"};
        uint64_t n = sizeof(ie)/sizeof(ie[0]);
        if (idx >= n) return 0;
        out->ino = 0xFFEC-idx;
        out->type = (idx==1) ? VFS_DT_DIR : VFS_DT_REG;
        strncpy(out->name, ie[idx], VFS_NAME_MAX); out->name[VFS_NAME_MAX]='\0';
        *cookie = idx+1; return 1;
    }

    return 0;
}

static int procfs_stat(vnode_t *v, vfs_stat_t *st) {
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->dev     = v->fsi ? v->fsi->dev_id : 0;
    st->mode    = v->mode;
    st->ino     = v->ino;
    st->blksize = 4096;
    if (VFS_S_ISREG(v->mode)) st->size = 4096;

    if (v->fs_data) {
        procfs_node_t *pn = (procfs_node_t *)v->fs_data;
        if (pn->pid != 0) {
            task_t *owner = task_lookup(pn->pid);
            if (owner) {
                st->uid = owner->cred.euid;
                st->gid = owner->cred.egid;
            }
        }
    }
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
        if (!t->exe_path[0]) return -ENOENT;
        size_t tlen = strlen(t->exe_path);
        if (tlen + 1 > bufsize) return -ERANGE;
        memcpy(buf, t->exe_path, tlen + 1);
        return 0;
    }
    if (pn->type == PROC_PID_FD_ENTRY) {
        task_t *t = task_lookup(pn->pid);
        if (!t) return -ESRCH;
        int fd = pn->fd;
        if (fd < 0 || fd >= FD_TABLE_SIZE || !t->fd_table[fd]) return -ENOENT;
        file_t *f = t->fd_table[fd];
        if (f->path[0]) {
            size_t tlen = strlen(f->path);
            if (tlen + 1 > bufsize) return -ERANGE;
            memcpy(buf, f->path, tlen + 1);
            return 0;
        }
        int n = ksnprintf(buf, bufsize, "socket:[%u]", (unsigned)fd);
        return (n < 0 || (size_t)n >= bufsize) ? -ERANGE : 0;
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

    procfs_root = procfs_alloc_node(PROC_ROOT, 0, 0, VFS_S_IFDIR | 0555);
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
