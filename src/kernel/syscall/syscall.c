/* syscall/syscall.c — INT 0x80 + SYSCALL dispatcher
 *
 * Registers vector 0x80 with DPL=3 so user-space can call INT 0x80.
 * Also sets up SYSCALL/SYSRET MSRs for the fast path.
 * Syscall number in rax; args in rdi, rsi, rdx, r10, r8, r9.
 * Return value written back to regs->rax.
 */
#include "syscall.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/cpu.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/waitq.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "ipc/signal.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#include <stdint.h>

/* ── Forward declarations to file_syscalls.c ────────────────────────────── */
int64_t sys_read(int fd, void *buf, uint64_t count);
int64_t sys_write(int fd, const void *buf, uint64_t count);
int64_t sys_readv(int fd, const iovec_t *iov, int iovcnt);
int64_t sys_pread64(int fd, void *buf, uint64_t count, int64_t offset);
int64_t sys_pwrite64(int fd, const void *buf, uint64_t count, int64_t offset);
int64_t sys_open(const char *path, int flags, uint32_t mode);
int64_t sys_openat(int dirfd, const char *path, int flags, uint32_t mode);
int64_t sys_close(int fd);
int64_t sys_stat(const char *path, linux_stat_t *buf);
int64_t sys_fstat(int fd, linux_stat_t *buf);
int64_t sys_lstat(const char *path, linux_stat_t *buf);
int64_t sys_fstatat(int dirfd, const char *path, linux_stat_t *buf, int flags);
int64_t sys_lseek(int fd, int64_t off, int whence);
int64_t sys_mkdirat(int dirfd, const char *path, uint32_t mode);
int64_t sys_unlinkat(int dirfd, const char *path, int flags);
int64_t sys_readlinkat(int dirfd, const char *path, char *buf, uint64_t bufsiz);
int64_t sys_renameat(int olddirfd, const char *old_path, int newdirfd, const char *new_path);
int64_t sys_faccessat(int dirfd, const char *path, int mode, int flags);
int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
int64_t sys_dup(int old_fd);
int64_t sys_dup2(int old_fd, int new_fd);
int64_t sys_getcwd(char *buf, uint64_t size);
int64_t sys_chdir(const char *path);
int64_t sys_mkdir(const char *path, uint32_t mode);
int64_t sys_rmdir(const char *path);
int64_t sys_unlink(const char *path);
int64_t sys_rename(const char *old, const char *newp);
int64_t sys_getdents64(int fd, void *dirp, uint64_t count);
int64_t sys_brk(uint64_t addr);

/* ── Forward declarations to net_syscalls.c ─────────────────────────────── */
struct sockaddr;
struct pollfd;
typedef uint32_t socklen_t;
int64_t sys_socket(int domain, int type, int protocol);
int64_t sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int64_t sys_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int64_t sys_sendto(int fd, const void *buf, uint64_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen);
int64_t sys_recvfrom(int fd, void *buf, uint64_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);
int64_t sys_shutdown(int fd, int how);
int64_t sys_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int64_t sys_listen(int fd, int backlog);
int64_t sys_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int64_t sys_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);
int64_t sys_setsockopt(int fd, int level, int optname,
                       const void *optval, socklen_t optlen);
int64_t sys_getsockopt(int fd, int level, int optname,
                       void *optval, socklen_t *optlen);
int64_t sys_poll(struct pollfd *fds, uint64_t nfds, int timeout);
int64_t sys_ioctl(int fd, unsigned long cmd, unsigned long arg);

/* ── Forward declarations to proc_syscalls.c ────────────────────────────── */
int64_t sys_fork(cpu_regs_t *regs);
int64_t sys_execve(const char *path, char *const argv[], char *const envp[]);
int64_t sys_wait4(int pid, int *wstatus, int options, void *rusage);
int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                 int fd, int64_t offset);
int64_t sys_munmap(uint64_t addr, uint64_t len);
int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot);
int64_t sys_gettid(void);
int64_t sys_tgkill(int tgid, int tid, int sig);
int64_t sys_set_robust_list(uint64_t head, uint64_t len);
int64_t sys_get_robust_list(int pid, uint64_t *head_ptr, uint64_t *len_ptr);

/* ── Syscall table ───────────────────────────────────────────────────────── */
typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t);

static int64_t sc_read(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_read((int)a,(void*)b,c); }
static int64_t sc_write(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_write((int)a,(const void*)b,c); }
static int64_t sc_pread64(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)e;(void)f; return sys_pread64((int)a,(void*)b,c,(int64_t)d); }
static int64_t sc_pwrite64(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)e;(void)f; return sys_pwrite64((int)a,(const void*)b,c,(int64_t)d); }
static int64_t sc_open(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_open((const char*)a,(int)b,(uint32_t)c); }
static int64_t sc_openat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)e;(void)f; return sys_openat((int)a,(const char*)b,(int)c,(uint32_t)d); }
static int64_t sc_close(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_close((int)a); }
static int64_t sc_stat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_stat((const char*)a,(linux_stat_t*)b); }
static int64_t sc_fstat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_fstat((int)a,(linux_stat_t*)b); }
static int64_t sc_lstat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_lstat((const char*)a,(linux_stat_t*)b); }
static int64_t sc_lseek(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_lseek((int)a,(int64_t)b,(int)c); }
static int64_t sc_brk(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_brk(a); }
static int64_t sc_dup(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_dup((int)a); }
static int64_t sc_dup2(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_dup2((int)a,(int)b); }

/* ── sys_exit: properly terminate the task ────────────────────────────────── */
static int64_t sc_exit(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)b;(void)c;(void)d;(void)e;(void)f;
    task_t *cur = sched_current();
    if (cur) {
        cur->exit_status = (int)(a & 0xFF) << 8;  /* encode like waitpid */
        KLOG_INFO("syscall: task '%s' tid=%u exit(%llu)\n",
                  cur->name, cur->tid, a);

        /* Wake parent if waiting */
        if (cur->parent) {
            signal_send(cur->parent, SIGCHLD);
            sched_unblock(cur->parent);
        }

        /* Write 0 to clear_child_tid and futex-wake (for threading) */
        if (cur->clear_child_tid) {
            *cur->clear_child_tid = 0;
            extern int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val,
                                     const kernel_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3);
            sys_futex((uint32_t *)cur->clear_child_tid, 1/*FUTEX_WAKE*/, 1, NULL, NULL, 0);
        }
    }
    sched_task_exit();
    /* unreachable */
    for(;;) __asm__ volatile("hlt");
    return 0;
}

static int64_t sc_getcwd(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_getcwd((char*)a,b); }
static int64_t sc_chdir(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_chdir((const char*)a); }
static int64_t sc_rename(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_rename((const char*)a,(const char*)b); }
static int64_t sc_renameat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)e;(void)f; return sys_renameat((int)a,(const char*)b,(int)c,(const char*)d); }
static int64_t sc_mkdir(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_mkdir((const char*)a,(uint32_t)b); }
static int64_t sc_mkdirat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_mkdirat((int)a,(const char*)b,(uint32_t)c); }
static int64_t sc_rmdir(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_rmdir((const char*)a); }
static int64_t sc_unlink(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_unlink((const char*)a); }
static int64_t sc_unlinkat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_unlinkat((int)a,(const char*)b,(int)c); }
static int64_t sc_symlinkat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_symlinkat((const char*)a,(int)b,(const char*)c); }
static int64_t sc_getdents64(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_getdents64((int)a,(void*)b,c); }

/* ── Network syscall wrappers ────────────────────────────────────────────── */
static int64_t sc_poll(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_poll((struct pollfd*)a,b,(int)c); }
static int64_t sc_ioctl(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_ioctl((int)a,b,c); }
static int64_t sc_socket(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_socket((int)a,(int)b,(int)c); }
static int64_t sc_connect(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_connect((int)a,(const struct sockaddr*)b,(socklen_t)c); }
static int64_t sc_accept(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_accept((int)a,(struct sockaddr*)b,(socklen_t*)c); }
static int64_t sc_sendto(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { return sys_sendto((int)a,(const void*)b,c,(int)d,(const struct sockaddr*)e,(socklen_t)f); }
static int64_t sc_recvfrom(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { return sys_recvfrom((int)a,(void*)b,c,(int)d,(struct sockaddr*)e,(socklen_t*)f); }
static int64_t sc_shutdown(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_shutdown((int)a,(int)b); }
static int64_t sc_bind(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_bind((int)a,(const struct sockaddr*)b,(socklen_t)c); }
static int64_t sc_listen(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_listen((int)a,(int)b); }
static int64_t sc_getsockname(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_getsockname((int)a,(struct sockaddr*)b,(socklen_t*)c); }
static int64_t sc_getpeername(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_getpeername((int)a,(struct sockaddr*)b,(socklen_t*)c); }
static int64_t sc_setsockopt(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)f; return sys_setsockopt((int)a,(int)b,(int)c,(const void*)d,(socklen_t)e); }
static int64_t sc_getsockopt(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)f; return sys_getsockopt((int)a,(int)b,(int)c,(void*)d,(socklen_t*)e); }

/* ── Process / memory syscall wrappers ────────────────────────────────────── */
static int64_t sc_getpid(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
      task_t *t = sched_current(); return t ? (int64_t)t->pid : 0; }
static int64_t sc_gettid(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
        { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return sys_gettid(); }
static int64_t sc_getppid(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
      task_t *t = sched_current(); return t ? (int64_t)t->ppid : 0; }
static int64_t sc_getuid(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 0; }
static int64_t sc_getgid(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 0; }

static int64_t sc_mmap(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { return sys_mmap(a,b,(int)c,(int)d,(int)e,(int64_t)f); }
static int64_t sc_munmap(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_munmap(a,b); }
static int64_t sc_mprotect(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_mprotect(a,b,(int)c); }

/* arch_prctl: set/get FS/GS base for TLS */
static int64_t sc_arch_prctl(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    task_t *cur = sched_current();
    switch ((int)a) {
        case ARCH_SET_FS:
            if (cur) cur->fs_base = b;
            wrmsr(MSR_FS_BASE, b);
            return 0;
        case ARCH_GET_FS:
            if (b) *(uint64_t *)b = cur ? cur->fs_base : rdmsr(MSR_FS_BASE);
            return 0;
        case ARCH_SET_GS:
            wrmsr(MSR_GS_BASE, b);
            return 0;
        case ARCH_GET_GS:
            if (b) *(uint64_t *)b = rdmsr(MSR_GS_BASE);
            return 0;
        default:
            return -EINVAL;
    }
}

static int64_t sc_set_tid_address(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)b;(void)c;(void)d;(void)e;(void)f;
    task_t *cur = sched_current();
    if (cur) cur->clear_child_tid = (uint64_t *)a;
    return cur ? (int64_t)cur->tid : 0;
}

static int64_t sc_writev(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    int fd = (int)a;
    const iovec_t *iov = (const iovec_t *)b;
    int iovcnt = (int)c;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t r = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break; /* short write */
    }
    return total;
}

static int64_t sc_readv(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    return sys_readv((int)a, (const iovec_t *)b, (int)c);
}

static int64_t sc_access(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    return sys_faccessat(AT_FDCWD, (const char *)a, (int)b, 0);
}

typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} kernel_timeval_t;

typedef struct {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
} kernel_tms_t;

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME 1

static int64_t sc_sched_yield(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    sched_tick();
    return 0;
}

static int64_t sc_nanosleep(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    const kernel_timespec_t *req = (const kernel_timespec_t *)a;
    kernel_timespec_t *rem = (kernel_timespec_t *)b;
    if (!req) return -EFAULT;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL)
        return -EINVAL;

    uint64_t ms = (uint64_t)req->tv_sec * 1000ULL + (uint64_t)(req->tv_nsec / 1000000LL);
    if (ms == 0 && req->tv_nsec > 0)
        ms = 1;
    if (ms > 0)
        sched_sleep((uint32_t)ms);

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static int64_t sc_clock_nanosleep(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    int clock_id = (int)a;
    int flags = (int)b;
    const kernel_timespec_t *req = (const kernel_timespec_t *)c;
    kernel_timespec_t *rem = (kernel_timespec_t *)d;

    if (!req) return -EFAULT;
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) return -EINVAL;
    if (flags & ~TIMER_ABSTIME) return -EINVAL;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL) return -EINVAL;

    uint64_t sleep_ms = 0;
    if (flags & TIMER_ABSTIME) {
        uint64_t now_ms = sched_get_ticks();
        uint64_t abs_ms = (uint64_t)req->tv_sec * 1000ULL + (uint64_t)(req->tv_nsec / 1000000ULL);
        if (abs_ms > now_ms)
            sleep_ms = abs_ms - now_ms;
    } else {
        sleep_ms = (uint64_t)req->tv_sec * 1000ULL + (uint64_t)(req->tv_nsec / 1000000ULL);
        if (sleep_ms == 0 && req->tv_nsec > 0)
            sleep_ms = 1;
    }

    if (sleep_ms > 0)
        sched_sleep((uint32_t)sleep_ms);

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static int64_t sc_times(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)b;(void)c;(void)d;(void)e;(void)f;
    kernel_tms_t *tbuf = (kernel_tms_t *)a;
    uint64_t ticks_ms = sched_get_ticks();
    int64_t ticks_100hz = (int64_t)(ticks_ms / 10ULL);

    if (tbuf) {
        tbuf->tms_utime = ticks_100hz;
        tbuf->tms_stime = 0;
        tbuf->tms_cutime = 0;
        tbuf->tms_cstime = 0;
    }
    return ticks_100hz;
}

static int64_t sc_gettimeofday(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    kernel_timeval_t *tv = (kernel_timeval_t *)a;
    if (!tv) return -EFAULT;

    uint64_t ticks = sched_get_ticks();
    tv->tv_sec = (int64_t)(ticks / 1000ULL);
    tv->tv_usec = (int64_t)((ticks % 1000ULL) * 1000ULL);

    (void)b;
    return 0;
}

typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} kernel_rlimit_t;

enum {
    RLIMIT_CPU = 0,
    RLIMIT_FSIZE = 1,
    RLIMIT_DATA = 2,
    RLIMIT_STACK = 3,
    RLIMIT_CORE = 4,
    RLIMIT_RSS = 5,
    RLIMIT_NPROC = 6,
    RLIMIT_NOFILE = 7,
    RLIMIT_MEMLOCK = 8,
    RLIMIT_AS = 9,
    RLIMIT_LOCKS = 10,
    RLIMIT_SIGPENDING = 11,
    RLIMIT_MSGQUEUE = 12,
    RLIMIT_NICE = 13,
    RLIMIT_RTPRIO = 14,
    RLIMIT_RTTIME = 15,
};

static int rlimit_default_for(int resource, kernel_rlimit_t *out) {
    if (!out) return -EFAULT;
    out->rlim_cur = ~0ULL;
    out->rlim_max = ~0ULL;

    switch (resource) {
        case RLIMIT_NOFILE:
            out->rlim_cur = TASK_FD_TABLE_SIZE;
            out->rlim_max = TASK_FD_TABLE_SIZE;
            return 0;
        case RLIMIT_STACK:
            out->rlim_cur = TASK_STACK_SIZE;
            out->rlim_max = TASK_STACK_SIZE;
            return 0;
        case RLIMIT_AS:
        case RLIMIT_DATA:
        case RLIMIT_CPU:
        case RLIMIT_FSIZE:
        case RLIMIT_CORE:
        case RLIMIT_RSS:
        case RLIMIT_NPROC:
        case RLIMIT_MEMLOCK:
        case RLIMIT_LOCKS:
        case RLIMIT_SIGPENDING:
        case RLIMIT_MSGQUEUE:
        case RLIMIT_NICE:
        case RLIMIT_RTPRIO:
        case RLIMIT_RTTIME:
            return 0;
        default:
            return -EINVAL;
    }
}

static int64_t sc_getrlimit(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    int resource = (int)a;
    kernel_rlimit_t *rlim = (kernel_rlimit_t *)b;
    if (!rlim) return -EFAULT;
    return rlimit_default_for(resource, rlim);
}

static int64_t sc_prlimit64(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    int pid = (int)a;
    int resource = (int)b;
    const kernel_rlimit_t *new_limit = (const kernel_rlimit_t *)c;
    kernel_rlimit_t *old_limit = (kernel_rlimit_t *)d;

    task_t *cur = sched_current();
    if (!cur) return -ESRCH;
    if (pid != 0 && pid != (int)cur->pid) return -ESRCH;

    kernel_rlimit_t def;
    int rr = rlimit_default_for(resource, &def);
    if (rr < 0) return rr;

    if (old_limit)
        *old_limit = def;

    if (new_limit) {
        if (new_limit->rlim_cur > new_limit->rlim_max)
            return -EINVAL;
        if (new_limit->rlim_cur > def.rlim_max || new_limit->rlim_max > def.rlim_max)
            return -EPERM;
    }

    return 0;
}

static int64_t sc_faccessat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    return sys_faccessat((int)a, (const char *)b, (int)c, (int)d);
}

static int64_t sc_exit_group(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    return sc_exit(a,b,c,d,e,f);  /* same as exit for now (no threads) */
}

static int64_t sc_clock_gettime(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    kernel_timespec_t *ts = (kernel_timespec_t *)b;
    if (!ts) return -EFAULT;
    uint64_t ticks = sched_get_ticks();
    ts->tv_sec  = (int64_t)(ticks / 1000);
    ts->tv_nsec = (int64_t)((ticks % 1000) * 1000000);
    (void)a; /* clock_id ignored for now; both REALTIME and MONOTONIC use ticks */
    return 0;
}

static int64_t sc_umask(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)b;(void)c;(void)d;(void)e;(void)f;
    (void)a;
    return 022; /* always return default umask */
}

static int64_t sc_dup3(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    task_t *cur = sched_current();
    if (!cur) return -ENOSYS;
    int r = fd_dup2(cur, (int)a, (int)b);
    if (r >= 0 && (c & 0x80000)) { /* O_CLOEXEC = 0x80000 */
        file_t *fp = fd_get(cur, r);
        if (fp) fp->fd_flags |= FD_CLOEXEC;
    }
    return r;
}

static int64_t sc_kill(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    task_t *target = task_lookup((uint32_t)a);
    if (!target) return -ESRCH;
    signal_send(target, (int)b);
    return 0;
}

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} kernel_utsname_t;

static int64_t sc_uname(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)b;(void)c;(void)d;(void)e;(void)f;
    kernel_utsname_t *u = (kernel_utsname_t *)a;
    if (!u) return -EFAULT;

    memset(u, 0, sizeof(*u));
    strncpy(u->sysname, "Linux", sizeof(u->sysname) - 1);
    strncpy(u->nodename, "exo", sizeof(u->nodename) - 1);
    strncpy(u->release, "0.1.0", sizeof(u->release) - 1);
    strncpy(u->version, "EXO_OS", sizeof(u->version) - 1);
    strncpy(u->machine, "x86_64", sizeof(u->machine) - 1);
    strncpy(u->domainname, "localdomain", sizeof(u->domainname) - 1);
    return 0;
}

static uint64_t g_getrandom_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t gr_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t getrandom_next(void) {
    uint64_t x = g_getrandom_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_getrandom_state = x + gr_rdtsc();
    return x * 0x2545F4914F6CDD1DULL;
}

static int64_t sc_getrandom(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    void *buf = (void *)a;
    uint64_t len = b;
    unsigned int flags = (unsigned int)c;

    if (!buf && len > 0) return -EFAULT;
    if (flags & ~0x3U) return -EINVAL;
    if (len == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t r = getrandom_next();
        uint64_t chunk = len - done;
        if (chunk > 8) chunk = 8;
        memcpy(out + done, &r, (size_t)chunk);
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t sc_tgkill(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    return sys_tgkill((int)a, (int)b, (int)c);
}

static int64_t sc_set_robust_list(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    return sys_set_robust_list(a, b);
}

static int64_t sc_get_robust_list(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    return sys_get_robust_list((int)a, (uint64_t *)b, (uint64_t *)c);
}

static int64_t sc_wait4(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)e;(void)f; return sys_wait4((int)a,(int*)b,(int)c,(void*)d); }

/* Fork needs the register frame to set up child's return */
static cpu_regs_t *g_fork_regs = NULL;  /* set before dispatching fork */

static int64_t sc_fork(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    return sys_fork(g_fork_regs);
}

static int64_t sc_execve(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    return sys_execve((const char *)a, (char *const *)b, (char *const *)c);
}

/* Signal syscalls */
static int64_t sc_rt_sigaction(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    return sys_rt_sigaction((int)a, (const kernel_sigaction_t *)b,
                            (kernel_sigaction_t *)c, d);
}
static int64_t sc_rt_sigprocmask(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    return sys_rt_sigprocmask((int)a, (const uint64_t *)b, (uint64_t *)c, d);
}
static cpu_regs_t *g_sigreturn_regs = NULL;
static int64_t sc_rt_sigreturn(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    return sys_rt_sigreturn(g_sigreturn_regs);
}

/* Pipe syscalls — forward declarations */
extern int64_t sys_pipe2(int pipefd[2], int flags);
static int64_t sc_pipe(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)b;(void)c;(void)d;(void)e;(void)f;
    return sys_pipe2((int *)a, 0);
}
static int64_t sc_pipe2(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)c;(void)d;(void)e;(void)f;
    return sys_pipe2((int *)a, (int)b);
}

/* Clone syscall — forward declaration */
extern int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid,
                         uint64_t child_tid, uint64_t tls, cpu_regs_t *regs);
static int64_t sc_clone(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)f;
    return sys_clone(a, b, c, d, e, g_fork_regs);
}

/* Futex syscall — forward declaration */
extern int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val,
                         const kernel_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3);
static int64_t sc_futex(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    return sys_futex((uint32_t *)a, (int)b, (uint32_t)c,
                     (const kernel_timespec_t *)d, (uint32_t *)e, (uint32_t)f);
}

/* Readlink syscall */
static int64_t sc_readlink(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    return sys_readlinkat(AT_FDCWD, (const char *)a, (char *)b, c);
}
static int64_t sc_readlinkat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    return sys_readlinkat((int)a, (const char *)b, (char *)c, d);
}

/* Fcntl syscall */
static int64_t sc_fcntl(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)d;(void)e;(void)f;
    task_t *cur = sched_current();
    if (!cur) return -ENOSYS;
    int fd_num = (int)a;
    int cmd = (int)b;
    file_t *fp = fd_get(cur, fd_num);
    if (!fp) return -EBADF;
    switch (cmd) {
        case 1: /* F_GETFD */ return fp->fd_flags;
        case 2: /* F_SETFD */ fp->fd_flags = (int)c; return 0;
        case 3: /* F_GETFL */ return fp->flags;
        case 4: /* F_SETFL */ fp->flags = (int)c; return 0;
        case 0: /* F_DUPFD */ return fd_dup(cur, fd_num);
        default: return -EINVAL;
    }
}

/* Fstatat syscall */
static int64_t sc_fstatat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)e;(void)f;
    return sys_fstatat((int)a, (const char *)b, (linux_stat_t *)c, (int)d);
}

/* Sparse dispatch table indexed by syscall number */
#define SYSCALL_TABLE_SIZE 512
static syscall_fn_t g_syscall_table[SYSCALL_TABLE_SIZE] = {
    [SYS_READ]      = sc_read,
    [SYS_WRITE]     = sc_write,
    [SYS_PREAD64]   = sc_pread64,
    [SYS_PWRITE64]  = sc_pwrite64,
    [SYS_READV]     = sc_readv,
    [SYS_OPEN]      = sc_open,
    [SYS_OPENAT]    = sc_openat,
    [SYS_CLOSE]     = sc_close,
    [SYS_STAT]      = sc_stat,
    [SYS_FSTAT]     = sc_fstat,
    [SYS_LSTAT]     = sc_lstat,
    [SYS_LSEEK]     = sc_lseek,
    [SYS_MMAP]      = sc_mmap,
    [SYS_MPROTECT]  = sc_mprotect,
    [SYS_MUNMAP]    = sc_munmap,
    [SYS_BRK]       = sc_brk,
    [SYS_GETTIMEOFDAY] = sc_gettimeofday,
    [SYS_WRITEV]    = sc_writev,
    [SYS_ACCESS]    = sc_access,
    [SYS_SCHED_YIELD] = sc_sched_yield,
    [SYS_DUP]       = sc_dup,
    [SYS_DUP2]      = sc_dup2,
    [SYS_GETPID]    = sc_getpid,
    [SYS_NANOSLEEP] = sc_nanosleep,
    [SYS_GETTID]    = sc_gettid,
    [SYS_FORK]      = sc_fork,
    [SYS_EXECVE]    = sc_execve,
    [SYS_EXIT]      = sc_exit,
    [SYS_WAIT4]     = sc_wait4,
    [SYS_KILL]      = sc_kill,
    [SYS_UNAME]     = sc_uname,
    [SYS_TGKILL]    = sc_tgkill,
    [SYS_GETCWD]    = sc_getcwd,
    [SYS_CHDIR]     = sc_chdir,
    [SYS_RENAME]    = sc_rename,
    [SYS_RENAMEAT]  = sc_renameat,
    [SYS_MKDIR]     = sc_mkdir,
    [SYS_MKDIRAT]   = sc_mkdirat,
    [SYS_RMDIR]     = sc_rmdir,
    [SYS_UNLINK]    = sc_unlink,
    [SYS_UNLINKAT]  = sc_unlinkat,
    [SYS_SYMLINKAT] = sc_symlinkat,
    [SYS_UMASK]     = sc_umask,
    [SYS_GETRLIMIT] = sc_getrlimit,
    [SYS_TIMES]     = sc_times,
    [SYS_GETUID]    = sc_getuid,
    [SYS_GETGID]    = sc_getgid,
    [SYS_GETEUID]   = sc_getuid,
    [SYS_GETEGID]   = sc_getgid,
    [SYS_GETPPID]   = sc_getppid,
    [SYS_ARCH_PRCTL]= sc_arch_prctl,
    [SYS_GETDENTS64]= sc_getdents64,
    [SYS_SET_TID_ADDRESS] = sc_set_tid_address,
    [SYS_SET_ROBUST_LIST] = sc_set_robust_list,
    [SYS_GET_ROBUST_LIST] = sc_get_robust_list,
    [SYS_CLOCK_GETTIME]   = sc_clock_gettime,
    [SYS_CLOCK_NANOSLEEP] = sc_clock_nanosleep,
    [SYS_EXIT_GROUP]= sc_exit_group,
    [SYS_DUP3]      = sc_dup3,
    [SYS_PRLIMIT64] = sc_prlimit64,
    [SYS_GETRANDOM] = sc_getrandom,
    [SYS_POLL]      = sc_poll,
    [SYS_IOCTL]     = sc_ioctl,
    [SYS_SOCKET]    = sc_socket,
    [SYS_CONNECT]   = sc_connect,
    [SYS_ACCEPT]    = sc_accept,
    [SYS_SENDTO]    = sc_sendto,
    [SYS_RECVFROM]  = sc_recvfrom,
    [SYS_SHUTDOWN]  = sc_shutdown,
    [SYS_BIND]      = sc_bind,
    [SYS_LISTEN]    = sc_listen,
    [SYS_GETSOCKNAME] = sc_getsockname,
    [SYS_GETPEERNAME]  = sc_getpeername,
    [SYS_SETSOCKOPT]   = sc_setsockopt,
    [SYS_GETSOCKOPT]   = sc_getsockopt,
    [SYS_RT_SIGACTION]   = sc_rt_sigaction,
    [SYS_RT_SIGPROCMASK] = sc_rt_sigprocmask,
    [SYS_RT_SIGRETURN]   = sc_rt_sigreturn,
    [SYS_PIPE]           = sc_pipe,
    [SYS_PIPE2]          = sc_pipe2,
    [SYS_CLONE]          = sc_clone,
    [SYS_FUTEX]          = sc_futex,
    [SYS_READLINK]       = sc_readlink,
    [SYS_READLINKAT]     = sc_readlinkat,
    [SYS_FCNTL]          = sc_fcntl,
    [SYS_FSTATAT]        = sc_fstatat,
    [SYS_FACCESSAT]      = sc_faccessat,
};

/* ── INT 0x80 handler ─────────────────────────────────────────────────────── */
static void syscall_dispatch(cpu_regs_t *regs) {
    uint64_t nr = regs->rax;

    /* sys_fork/clone/sigreturn need access to the full register frame */
    if (nr == SYS_FORK || nr == SYS_CLONE) g_fork_regs = regs;
    if (nr == SYS_RT_SIGRETURN) g_sigreturn_regs = regs;

    if (nr < SYSCALL_TABLE_SIZE && g_syscall_table[nr]) {
        regs->rax = (uint64_t)g_syscall_table[nr](
            regs->rdi, regs->rsi, regs->rdx,
            regs->r10, regs->r8,  regs->r9);
    } else {
        KLOG_WARN("syscall: unknown nr=%llu\n", nr);
        regs->rax = (uint64_t)-ENOSYS;
    }

    /* Deliver pending signals on return to user-space */
    task_t *cur = sched_current();
    if (cur && cur->is_user)
        signal_deliver_user(cur, regs);
}

/* ── SYSCALL fast-path handler (called from syscall_entry asm) ────────────── */
void syscall_dispatch_fast(cpu_regs_t *regs) {
    /* Identical dispatch logic — the asm stub already built a cpu_regs_t frame */
    uint64_t nr = regs->rax;


    if (nr == SYS_FORK || nr == SYS_CLONE) g_fork_regs = regs;
    if (nr == SYS_RT_SIGRETURN) g_sigreturn_regs = regs;

    if (nr < SYSCALL_TABLE_SIZE && g_syscall_table[nr]) {
        regs->rax = (uint64_t)g_syscall_table[nr](
            regs->rdi, regs->rsi, regs->rdx,
            regs->r10, regs->r8,  regs->r9);
    } else {
        KLOG_WARN("syscall: unknown nr=%llu (fast)\n", nr);
        regs->rax = (uint64_t)-ENOSYS;
    }


    /* Deliver pending signals on return to user-space */
    task_t *cur = sched_current();
    if (cur && cur->is_user)
        signal_deliver_user(cur, regs);
}

/* ── SYSCALL/SYSRET MSR setup ─────────────────────────────────────────────── */
extern void syscall_entry(void);   /* defined in context_switch.asm */

void syscall_init_fast(void) {
    /* MSR_STAR: kernel CS/SS in bits [47:32], user CS/SS base in bits [63:48].
     *
     * For SYSRET (64-bit):
     *   CS = STAR[63:48] + 16  |  RPL=3  →  0x10 + 16 = 0x20 | 3 = 0x23 ✓
     *   SS = STAR[63:48] + 8   |  RPL=3  →  0x10 +  8 = 0x18 | 3 = 0x1B ✓
     *
     * For SYSCALL:
     *   CS = STAR[47:32]       →  0x08  (kernel code)  ✓
     *   SS = STAR[47:32] + 8   →  0x10  (kernel data)  ✓
     */
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    /* MSR_LSTAR: entry point for SYSCALL instruction */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* MSR_FMASK: RFLAGS bits to clear on SYSCALL entry.
     * Clear IF (bit 9) to disable interrupts during prologue.
     * Clear DF (bit 10) for ABI safety. */
    wrmsr(MSR_FMASK, (1ULL << 9) | (1ULL << 10));

    /* Enable SCE (syscall enable) bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    KLOG_INFO("syscall: SYSCALL/SYSRET fast path enabled (LSTAR=%p)\n",
              (void *)syscall_entry);
}

/* ── Init ────────────────────────────────────────────────────────────────── */
void syscall_init(void) {
    /* Register INT 0x80 with USER_GATE (DPL=3) so ring-3 can invoke it */
    idt_set_handler(0x80, isr_stub_table[0x80], IDT_USER_GATE, 0);
    idt_register_handler(0x80, syscall_dispatch);
    KLOG_INFO("syscall: INT 0x80 gate installed\n");

    /* Set up SYSCALL/SYSRET fast path */
    syscall_init_fast();
}
