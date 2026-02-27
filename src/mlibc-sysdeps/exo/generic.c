/* generic.c — mlibc sysdep implementations for EXO_OS
 *
 * Each sys_* function is called by mlibc's POSIX wrappers.
 * They invoke __exo_syscall() with the appropriate Linux syscall number.
 *
 * mlibc sysdep convention:
 *   - Return 0 on success, or a positive errno on failure.
 *   - Output values are written through pointer parameters.
 *
 * The kernel uses Linux x86-64 syscall numbers and returns negative errno.
 */

#include <stdint.h>
#include <stddef.h>
#include <poll.h>

/* ── Syscall numbers (must match kernel syscall.h) ───────────────────────── */
#define SYS_READ              0
#define SYS_WRITE             1
#define SYS_OPEN              2
#define SYS_CLOSE             3
#define SYS_STAT              4
#define SYS_FSTAT             5
#define SYS_LSTAT             6
#define SYS_FSTATAT           262

/* errno values we need (cannot include <errno.h> in freestanding build) */
#ifndef EINVAL
#define EINVAL 22
#endif

/* ioctl requests for termios */
#define TCGETS_NR     0x5401
#define TCSETS_NR     0x5402
#define TCSETSW_NR    0x5403
#define TCSETSF_NR    0x5404
#define SYS_LSEEK             8
#define SYS_POLL              7
#define SYS_MMAP              9
#define SYS_MPROTECT          10
#define SYS_MUNMAP            11
#define SYS_BRK               12
#define SYS_RT_SIGACTION      13
#define SYS_RT_SIGPROCMASK    14
#define SYS_RT_SIGRETURN      15
#define SYS_IOCTL             16
#define SYS_WRITEV            20
#define SYS_ACCESS            21
#define SYS_PIPE              22
#define SYS_DUP               32
#define SYS_DUP2              33
#define SYS_GETPID            39
#define SYS_SOCKET            41
#define SYS_CONNECT           42
#define SYS_ACCEPT            43
#define SYS_SENDTO            44
#define SYS_RECVFROM          45
#define SYS_SHUTDOWN          48
#define SYS_BIND              49
#define SYS_LISTEN            50
#define SYS_GETSOCKNAME       51
#define SYS_GETPEERNAME       52
#define SYS_SETSOCKOPT        54
#define SYS_GETSOCKOPT        55
#define SYS_CLONE             56
#define SYS_FORK              57
#define SYS_EXECVE            59
#define SYS_EXIT              60
#define SYS_WAIT4             61
#define SYS_KILL              62
#define SYS_FCNTL             72
#define SYS_GETCWD            79
#define SYS_CHDIR             80
#define SYS_RENAME            82
#define SYS_MKDIR             83
#define SYS_RMDIR             84
#define SYS_UNLINK            87
#define SYS_READLINK          89
#define SYS_UMASK             95
#define SYS_GETUID            102
#define SYS_GETGID            104
#define SYS_GETEUID           107
#define SYS_GETEGID           108
#define SYS_SETPGID           109
#define SYS_GETPPID           110
#define SYS_SETSID            112
#define SYS_GETPGID           121
#define SYS_GETSID            124
#define SYS_ARCH_PRCTL        158
#define SYS_GETTID            186
#define SYS_FUTEX             202
#define SYS_GETDENTS64        217
#define SYS_SET_TID_ADDRESS   218
#define SYS_CLOCK_GETTIME     228
#define SYS_EXIT_GROUP        231
#define SYS_FSTATAT           262
#define SYS_UNLINKAT          263
#define SYS_OPENAT            257
#define SYS_MKDIRAT           258
#define SYS_READLINKAT        267
#define SYS_FACCESSAT         269
#define SYS_TGKILL            234
#define SYS_DUP3              292
#define SYS_PIPE2             293

#define ENOTTY                25
#define ENOSYS                38

#define TCGETS                0x5401

/* clone flags (Linux x86-64 subset used by mlibc pthread startup) */
#define CLONE_VM              0x00000100
#define CLONE_FS              0x00000200
#define CLONE_FILES           0x00000400
#define CLONE_SIGHAND         0x00000800
#define CLONE_THREAD          0x00010000

/* arch_prctl sub-commands */
#define ARCH_SET_FS     0x1002

/* Raw syscall */
extern long __exo_syscall(long nr, long a1, long a2, long a3,
                          long a4, long a5, long a6);

/* Helper: convert kernel return (negative errno) to sysdep return (positive errno) */
static inline int sc_error(long ret) {
    if (ret < 0) return (int)(-ret);
    return 0;
}

/* ── Process lifecycle ────────────────────────────────────────────────────── */

__attribute__((noreturn))
void sys_exit(int status) {
    __exo_syscall(SYS_EXIT, status, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

__attribute__((noreturn))
void sys_exit_group(int status) {
    __exo_syscall(SYS_EXIT_GROUP, status, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

int sys_fork(int *pid_out) {
    long ret = __exo_syscall(SYS_FORK, 0, 0, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (pid_out) *pid_out = (int)ret;
    return 0;
}

int sys_execve(const char *path, char *const argv[], char *const envp[]) {
    long ret = __exo_syscall(SYS_EXECVE, (long)path, (long)argv, (long)envp, 0, 0, 0);
    return sc_error(ret);
}

int sys_waitpid(int pid, int *status, int options, void *rusage, int *ret_pid) {
    long ret = __exo_syscall(SYS_WAIT4, pid, (long)status, options, (long)rusage, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (ret_pid) *ret_pid = (int)ret;
    return 0;
}

int sys_kill(int pid, int sig) {
    return sc_error(__exo_syscall(SYS_KILL, pid, sig, 0, 0, 0, 0));
}

int sys_getpid(void) {
    return (int)__exo_syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

int sys_getppid(void) {
    return (int)__exo_syscall(SYS_GETPPID, 0, 0, 0, 0, 0, 0);
}

int sys_gettid(void) {
    return (int)__exo_syscall(SYS_GETTID, 0, 0, 0, 0, 0, 0);
}

int sys_tgkill(int tgid, int tid, int sig) {
    return sc_error(__exo_syscall(SYS_TGKILL, tgid, tid, sig, 0, 0, 0));
}

int sys_getuid(void) {
    return (int)__exo_syscall(SYS_GETUID, 0, 0, 0, 0, 0, 0);
}

int sys_getgid(void) {
    return (int)__exo_syscall(SYS_GETGID, 0, 0, 0, 0, 0, 0);
}

int sys_geteuid(void) {
    return (int)__exo_syscall(SYS_GETEUID, 0, 0, 0, 0, 0, 0);
}

int sys_getegid(void) {
    return (int)__exo_syscall(SYS_GETEGID, 0, 0, 0, 0, 0, 0);
}

int sys_getpgid(int pid, int *pgid_out) {
    long ret = __exo_syscall(SYS_GETPGID, pid, 0, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (pgid_out) *pgid_out = (int)ret;
    return 0;
}

int sys_setpgid(int pid, int pgid) {
    return sc_error(__exo_syscall(SYS_SETPGID, pid, pgid, 0, 0, 0, 0));
}

int sys_setsid(int *sid_out) {
    long ret = __exo_syscall(SYS_SETSID, 0, 0, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (sid_out) *sid_out = (int)ret;
    return 0;
}

int sys_getsid(int pid, int *sid_out) {
    long ret = __exo_syscall(SYS_GETSID, pid, 0, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (sid_out) *sid_out = (int)ret;
    return 0;
}

/* ── Memory management ────────────────────────────────────────────────────── */

int sys_vm_map(void *hint, size_t size, int prot, int flags,
               int fd, long offset, void **out) {
    long ret = __exo_syscall(SYS_MMAP, (long)hint, (long)size,
                             prot, flags, fd, offset);
    if (ret < 0) return (int)(-ret);
    if (out) *out = (void *)ret;
    return 0;
}

int sys_vm_unmap(void *addr, size_t size) {
    return sc_error(__exo_syscall(SYS_MUNMAP, (long)addr, (long)size, 0, 0, 0, 0));
}

int sys_vm_protect(void *addr, size_t size, int prot) {
    return sc_error(__exo_syscall(SYS_MPROTECT, (long)addr, (long)size, prot, 0, 0, 0));
}

int sys_anon_allocate(size_t size, void **out) {
    return sys_vm_map((void *)0, size, 3 /* PROT_READ|PROT_WRITE */,
                      0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0, out);
}

int sys_anon_free(void *addr, size_t size) {
    return sys_vm_unmap(addr, size);
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

int sys_open(const char *path, int flags, int mode, int *fd_out) {
    long ret = __exo_syscall(SYS_OPEN, (long)path, flags, mode, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (fd_out) *fd_out = (int)ret;
    return 0;
}

int sys_openat(int dirfd, const char *path, int flags, int mode, int *fd_out) {
    long ret = __exo_syscall(SYS_OPENAT, dirfd, (long)path, flags, mode, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (fd_out) *fd_out = (int)ret;
    return 0;
}

int sys_close(int fd) {
    return sc_error(__exo_syscall(SYS_CLOSE, fd, 0, 0, 0, 0, 0));
}

int sys_read(int fd, void *buf, size_t count, long *bytes_read) {
    long ret = __exo_syscall(SYS_READ, fd, (long)buf, (long)count, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (bytes_read) *bytes_read = ret;
    return 0;
}

int sys_write(int fd, const void *buf, size_t count, long *bytes_written) {
    long ret = __exo_syscall(SYS_WRITE, fd, (long)buf, (long)count, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (bytes_written) *bytes_written = ret;
    return 0;
}

int sys_seek(int fd, long offset, int whence, long *new_pos) {
    long ret = __exo_syscall(SYS_LSEEK, fd, offset, whence, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (new_pos) *new_pos = ret;
    return 0;
}

int sys_dup(int fd, int flags, int *newfd) {
    long ret = __exo_syscall(SYS_DUP, fd, 0, 0, 0, 0, 0);
    (void)flags;
    if (ret < 0) return (int)(-ret);
    if (newfd) *newfd = (int)ret;
    return 0;
}

int sys_dup2(int oldfd, int flags, int newfd) {
    (void)flags;
    long ret = __exo_syscall(SYS_DUP2, oldfd, newfd, 0, 0, 0, 0);
    return sc_error(ret);
}

int sys_stat(const char *path, void *statbuf) {
    return sc_error(__exo_syscall(SYS_STAT, (long)path, (long)statbuf, 0, 0, 0, 0));
}

int sys_fstat(int fd, void *statbuf) {
    return sc_error(__exo_syscall(SYS_FSTAT, fd, (long)statbuf, 0, 0, 0, 0));
}

int sys_lstat(const char *path, void *statbuf) {
    return sc_error(__exo_syscall(SYS_LSTAT, (long)path, (long)statbuf, 0, 0, 0, 0));
}

int sys_fstatat_raw(int dirfd, const char *path, void *statbuf, int flags) {
    return sc_error(__exo_syscall(SYS_FSTATAT, dirfd, (long)path, (long)statbuf, flags, 0, 0));
}

int sys_tcgetattr(int fd, void *attr) {
    long ret = __exo_syscall(SYS_IOCTL, fd, TCGETS_NR, (long)attr, 0, 0, 0);
    return sc_error(ret);
}

int sys_tcsetattr(int fd, int optional_action, const void *attr) {
    int req;
    switch (optional_action) {
        case 0: req = TCSETS_NR;  break;  /* TCSANOW  */
        case 1: req = TCSETSW_NR; break;  /* TCSADRAIN */
        case 2: req = TCSETSF_NR; break;  /* TCSAFLUSH */
        default: return EINVAL;
    }
    long ret = __exo_syscall(SYS_IOCTL, fd, req, (long)attr, 0, 0, 0);
    return sc_error(ret);
}

int sys_fcntl(int fd, int cmd, unsigned long arg, int *result) {
    long ret = __exo_syscall(SYS_FCNTL, fd, cmd, (long)arg, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (result) *result = (int)ret;
    return 0;
}

int sys_ioctl(int fd, unsigned long request, void *arg, int *result) {
    long ret = __exo_syscall(SYS_IOCTL, fd, (long)request, (long)arg, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (result) *result = (int)ret;
    return 0;
}

int sys_access(const char *path, int mode) {
    return sc_error(__exo_syscall(SYS_ACCESS, (long)path, mode, 0, 0, 0, 0));
}

int sys_faccessat(int dirfd, const char *path, int mode, int flags) {
    return sc_error(__exo_syscall(SYS_FACCESSAT, dirfd, (long)path, mode, flags, 0, 0));
}

int sys_pipe(int fds[2], int flags) {
    if (flags)
        return sc_error(__exo_syscall(SYS_PIPE2, (long)fds, flags, 0, 0, 0, 0));
    return sc_error(__exo_syscall(SYS_PIPE, (long)fds, 0, 0, 0, 0, 0));
}

int sys_poll(struct pollfd *fds, unsigned long nfds, int timeout, int *num_events) {
    long ret = __exo_syscall(SYS_POLL, (long)fds, (long)nfds, timeout, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (num_events) *num_events = (int)ret;
    return 0;
}

int sys_isatty(int fd) {
    unsigned char termios_buf[128];
    long ret = __exo_syscall(SYS_IOCTL, fd, TCGETS, (long)termios_buf, 0, 0, 0);
    if (ret == 0)
        return 0;

    int e = sc_error(ret);
    if (e == ENOSYS)
        return ENOTTY;
    return e;
}

int sys_readlink(const char *path, char *buf, size_t bufsz, long *len_out) {
    long ret = __exo_syscall(SYS_READLINK, (long)path, (long)buf, (long)bufsz, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (len_out) *len_out = ret;
    return 0;
}

int sys_readlinkat(int dirfd, const char *path, char *buf, size_t bufsz, long *len_out) {
    long ret = __exo_syscall(SYS_READLINKAT, dirfd, (long)path, (long)buf, (long)bufsz, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (len_out) *len_out = ret;
    return 0;
}

/* ── Directory operations ─────────────────────────────────────────────────── */

int sys_getcwd(char *buf, size_t size) {
    long ret = __exo_syscall(SYS_GETCWD, (long)buf, (long)size, 0, 0, 0, 0);
    return sc_error(ret);
}

int sys_chdir(const char *path) {
    return sc_error(__exo_syscall(SYS_CHDIR, (long)path, 0, 0, 0, 0, 0));
}

int sys_mkdir(const char *path, int mode) {
    return sc_error(__exo_syscall(SYS_MKDIR, (long)path, mode, 0, 0, 0, 0));
}

int sys_mkdirat(int dirfd, const char *path, int mode) {
    return sc_error(__exo_syscall(SYS_MKDIRAT, dirfd, (long)path, mode, 0, 0, 0));
}

int sys_rmdir(const char *path) {
    return sc_error(__exo_syscall(SYS_RMDIR, (long)path, 0, 0, 0, 0, 0));
}

int sys_unlink(const char *path) {
    return sc_error(__exo_syscall(SYS_UNLINK, (long)path, 0, 0, 0, 0, 0));
}

int sys_rename(const char *old, const char *new_path) {
    return sc_error(__exo_syscall(SYS_RENAME, (long)old, (long)new_path, 0, 0, 0, 0));
}

/* ── Sockets ─────────────────────────────────────────────────────────────── */

int sys_socket(int family, int type, int protocol, int *fd_out) {
    long ret = __exo_syscall(SYS_SOCKET, family, type, protocol, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (fd_out) *fd_out = (int)ret;
    return 0;
}

int sys_connect(int fd, const void *addr_ptr, unsigned addr_length) {
    return sc_error(__exo_syscall(SYS_CONNECT, fd, (long)addr_ptr, (long)addr_length, 0, 0, 0));
}

int sys_bind(int fd, const void *addr_ptr, unsigned addr_length) {
    return sc_error(__exo_syscall(SYS_BIND, fd, (long)addr_ptr, (long)addr_length, 0, 0, 0));
}

int sys_listen(int fd, int backlog) {
    return sc_error(__exo_syscall(SYS_LISTEN, fd, backlog, 0, 0, 0, 0));
}

int sys_accept(int fd, int *newfd, void *addr_ptr, unsigned *addr_length, int flags) {
    (void)flags;
    long ret = __exo_syscall(SYS_ACCEPT, fd, (long)addr_ptr, (long)addr_length, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (newfd) *newfd = (int)ret;
    return 0;
}

int sys_sockname(int fd, void *addr_ptr, unsigned max_addr_length, unsigned *actual_length) {
    (void)max_addr_length;
    return sc_error(__exo_syscall(SYS_GETSOCKNAME, fd, (long)addr_ptr, (long)actual_length, 0, 0, 0));
}

int sys_peername(int fd, void *addr_ptr, unsigned max_addr_length, unsigned *actual_length) {
    (void)max_addr_length;
    return sc_error(__exo_syscall(SYS_GETPEERNAME, fd, (long)addr_ptr, (long)actual_length, 0, 0, 0));
}

int sys_setsockopt(int fd, int layer, int number, const void *buffer, unsigned size) {
    return sc_error(__exo_syscall(SYS_SETSOCKOPT, fd, layer, number, (long)buffer, (long)size, 0));
}

int sys_getsockopt(int fd, int layer, int number, void *buffer, unsigned *size) {
    return sc_error(__exo_syscall(SYS_GETSOCKOPT, fd, layer, number, (long)buffer, (long)size, 0));
}

int sys_shutdown(int fd, int how) {
    return sc_error(__exo_syscall(SYS_SHUTDOWN, fd, how, 0, 0, 0, 0));
}

/* ── Signals ──────────────────────────────────────────────────────────────── */

int sys_sigaction(int sig, const void *act, void *oldact) {
    return sc_error(__exo_syscall(SYS_RT_SIGACTION, sig, (long)act,
                                  (long)oldact, 8, 0, 0));
}

int sys_sigprocmask(int how, const void *set, void *oldset) {
    return sc_error(__exo_syscall(SYS_RT_SIGPROCMASK, how, (long)set,
                                  (long)oldset, 8, 0, 0));
}

/* ── Clock / time ─────────────────────────────────────────────────────────── */

int sys_clock_gettime(int clk, long *secs, long *nsecs) {
    struct { long sec; long nsec; } ts;
    long ret = __exo_syscall(SYS_CLOCK_GETTIME, clk, (long)&ts, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (secs)  *secs  = ts.sec;
    if (nsecs) *nsecs = ts.nsec;
    return 0;
}

/* ── Thread / futex support ───────────────────────────────────────────────── */

int sys_clone(void *tcb, int *tid_out) {
    /* Minimal thread clone wrapper for early pthread bring-up.
     * A full mlibc port can later pass dedicated stack/TLS/child_tid values. */
    (void)tcb;
    long ret = __exo_syscall(SYS_CLONE,
                             CLONE_VM | CLONE_FS | CLONE_FILES |
                                 CLONE_SIGHAND | CLONE_THREAD,
                             0, (long)tid_out, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (tid_out) *tid_out = (int)ret;
    return 0;
}

int sys_futex_wait(int *pointer, int expected, const void *timeout) {
    return sc_error(__exo_syscall(SYS_FUTEX, (long)pointer, 0 /* FUTEX_WAIT */,
                                  expected, (long)timeout, 0, 0));
}

int sys_futex_wake(int *pointer) {
    return sc_error(__exo_syscall(SYS_FUTEX, (long)pointer, 1 /* FUTEX_WAKE */,
                                  0x7FFFFFFF, 0, 0, 0));
}

/* TLS: set FS base via arch_prctl */
int sys_tcb_set(void *pointer) {
    return sc_error(__exo_syscall(SYS_ARCH_PRCTL, ARCH_SET_FS, (long)pointer,
                                  0, 0, 0, 0));
}

int sys_set_tid_address(int *tidptr, int *old_tid) {
    long ret = __exo_syscall(SYS_SET_TID_ADDRESS, (long)tidptr, 0, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (old_tid) *old_tid = (int)ret;
    return 0;
}

/* ── Misc ─────────────────────────────────────────────────────────────────── */

int sys_umask(int mask, int *old) {
    long ret = __exo_syscall(SYS_UMASK, mask, 0, 0, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (old) *old = (int)ret;
    return 0;
}

int sys_getdents64(int fd, void *buf, size_t bufsz, long *bytes_read) {
    long ret = __exo_syscall(SYS_GETDENTS64, fd, (long)buf, (long)bufsz, 0, 0, 0);
    if (ret < 0) return (int)(-ret);
    if (bytes_read) *bytes_read = ret;
    return 0;
}
