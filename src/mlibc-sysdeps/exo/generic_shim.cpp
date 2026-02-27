#include <stddef.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <bits/ssize_t.h>
#include <bits/off_t.h>
#include <abi-bits/pid_t.h>
#include <abi-bits/uid_t.h>
#include <abi-bits/gid_t.h>
#include <mlibc/thread.hpp>
#include <mlibc/fsfd_target.hpp>
#include <sys/stat.h>
#include <termios.h>

struct timespec;

extern "C" {
int sys_open(const char *path, int flags, int mode, int *fd_out);
int sys_close(int fd);
int sys_read(int fd, void *buf, size_t count, long *bytes_read);
int sys_write(int fd, const void *buf, size_t count, long *bytes_written);
int sys_seek(int fd, long offset, int whence, long *new_pos);
int sys_dup(int fd, int flags, int *newfd);
int sys_dup2(int oldfd, int flags, int newfd);
int sys_pipe(int fds[2], int flags);
int sys_isatty(int fd);
int sys_fork(int *pid_out);
int sys_getpid(void);
int sys_getppid(void);
int sys_getpgid(int pid, int *pgid_out);
int sys_setpgid(int pid, int pgid);
int sys_setsid(int *sid_out);
int sys_getsid(int pid, int *sid_out);
int sys_getuid(void);
int sys_getgid(void);
int sys_geteuid(void);
int sys_getegid(void);
int sys_getcwd(char *buf, size_t size);
int sys_chdir(const char *path);
int sys_mkdir(const char *path, int mode);
int sys_mkdirat(int dirfd, const char *path, int mode);
int sys_rmdir(const char *path);
int sys_unlink(const char *path);
int sys_rename(const char *old, const char *new_path);
int sys_access(const char *path, int mode);
int sys_faccessat(int dirfd, const char *path, int mode, int flags);
int sys_execve(const char *path, char *const argv[], char *const envp[]);
int sys_kill(int pid, int sig);
int sys_gettid(void);
int sys_tgkill(int tgid, int tid, int sig);
int sys_sigaction(int sig, const void *act, void *oldact);
int sys_sigprocmask(int how, const void *set, void *oldset);
int sys_fcntl(int fd, int cmd, unsigned long arg, int *result);
int sys_ioctl(int fd, unsigned long request, void *arg, int *result);
int sys_readlink(const char *path, char *buf, size_t bufsz, long *len_out);
int sys_readlinkat(int dirfd, const char *path, char *buf, size_t bufsz, long *len_out);
int sys_umask(int mask, int *old);
int sys_getdents64(int fd, void *buf, size_t bufsz, long *bytes_read);
int sys_poll(struct pollfd *fds, unsigned long nfds, int timeout, int *num_events);
int sys_waitpid(int pid, int *status, int options, void *rusage, int *ret_pid);
int sys_clock_gettime(int clk, long *secs, long *nsecs);
int sys_futex_wait(int *pointer, int expected, const void *timeout);
int sys_futex_wake(int *pointer);
int sys_tcb_set(void *pointer);
void sys_exit(int status);
int sys_vm_map(void *hint, size_t size, int prot, int flags,
               int fd, long offset, void **out);
int sys_vm_unmap(void *addr, size_t size);
int sys_vm_protect(void *addr, size_t size, int prot);
int sys_anon_allocate(size_t size, void **out);
int sys_anon_free(void *addr, size_t size);

int sys_openat(int dirfd, const char *path, int flags, int mode, int *fd_out);
int sys_socket(int family, int type, int protocol, int *fd_out);
int sys_connect(int fd, const void *addr_ptr, unsigned addr_length);
int sys_bind(int fd, const void *addr_ptr, unsigned addr_length);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, int *newfd, void *addr_ptr, unsigned *addr_length, int flags);
int sys_sockname(int fd, void *addr_ptr, unsigned max_addr_length, unsigned *actual_length);
int sys_peername(int fd, void *addr_ptr, unsigned max_addr_length, unsigned *actual_length);
int sys_setsockopt(int fd, int layer, int number, const void *buffer, unsigned size);
int sys_getsockopt(int fd, int layer, int number, void *buffer, unsigned *size);
int sys_shutdown(int fd, int how);

int sys_stat(const char *path, void *statbuf);
int sys_fstat(int fd, void *statbuf);
int sys_lstat(const char *path, void *statbuf);
int sys_fstatat_raw(int dirfd, const char *path, void *statbuf, int flags);
int sys_tcgetattr(int fd, void *attr);
int sys_tcsetattr(int fd, int optional_action, const void *attr);

long __exo_spawn_thread(unsigned long flags, void *stack,
    int *parent_tid_ptr, void *tls);
}

namespace mlibc {

extern "C" void __mlibc_enter_thread(void *entry, void *user_arg) {
    auto tcb = mlibc::get_current_tcb();

    while(!__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED))
        ::sys_futex_wait(&tcb->tid, 0, nullptr);

    tcb->invokeThreadFunc(entry, user_arg);

    __atomic_store_n(&tcb->didExit, 1, __ATOMIC_RELEASE);
    ::sys_futex_wake(&tcb->didExit);

    ::sys_exit(0);
}

int sys_open(const char *path, int flags, unsigned int mode, int *fd) {
    return ::sys_open(path, flags, (int)mode, fd);
}

int sys_openat(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
    return ::sys_openat(dirfd, path, flags, (int)mode, fd);
}

int sys_close(int fd) {
    return ::sys_close(fd);
}

int sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read) {
    long out = 0;
    int e = ::sys_read(fd, buf, count, &out);
    if (!e && bytes_read)
        *bytes_read = out;
    return e;
}

int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
    long out = 0;
    int e = ::sys_write(fd, buf, count, &out);
    if (!e && bytes_written)
        *bytes_written = out;
    return e;
}

int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) {
    long out = 0;
    int e = ::sys_seek(fd, (long)offset, whence, &out);
    if (!e && new_offset)
        *new_offset = out;
    return e;
}

int sys_dup(int fd, int flags, int *newfd) {
    return ::sys_dup(fd, flags, newfd);
}

int sys_dup2(int oldfd, int flags, int newfd) {
    return ::sys_dup2(oldfd, flags, newfd);
}

int sys_pipe(int *fds, int flags) {
    return ::sys_pipe(fds, flags);
}

int sys_fork(int *child) {
    return ::sys_fork(child);
}

pid_t sys_getpid() {
    return (pid_t)::sys_getpid();
}

pid_t sys_getppid() {
    return (pid_t)::sys_getppid();
}

int sys_getpgid(pid_t pid, pid_t *pgid) {
    int tmp = 0;
    int e = ::sys_getpgid((int)pid, &tmp);
    if (!e && pgid) *pgid = (pid_t)tmp;
    return e;
}

int sys_setpgid(pid_t pid, pid_t pgid) {
    return ::sys_setpgid((int)pid, (int)pgid);
}

int sys_setsid(pid_t *sid) {
    int tmp = 0;
    int e = ::sys_setsid(&tmp);
    if (!e && sid) *sid = (pid_t)tmp;
    return e;
}

int sys_getsid(pid_t pid, pid_t *sid) {
    int tmp = 0;
    int e = ::sys_getsid((int)pid, &tmp);
    if (!e && sid) *sid = (pid_t)tmp;
    return e;
}

uid_t sys_getuid() {
    return (uid_t)::sys_getuid();
}

gid_t sys_getgid() {
    return (gid_t)::sys_getgid();
}

uid_t sys_geteuid() {
    return (uid_t)::sys_geteuid();
}

gid_t sys_getegid() {
    return (gid_t)::sys_getegid();
}

int sys_getcwd(char *buffer, size_t size) {
    return ::sys_getcwd(buffer, size);
}

int sys_chdir(const char *path) {
    return ::sys_chdir(path);
}

int sys_mkdir(const char *path, unsigned mode) {
    return ::sys_mkdir(path, (int)mode);
}

int sys_mkdirat(int dirfd, const char *path, mode_t mode) {
    return ::sys_mkdirat(dirfd, path, (int)mode);
}

int sys_rmdir(const char *path) {
    return ::sys_rmdir(path);
}

int sys_unlinkat(int fd, const char *path, int flags) {
    (void)fd;
    (void)flags;
    return ::sys_unlink(path);
}

int sys_access(const char *path, int mode) {
    return ::sys_access(path, mode);
}

int sys_faccessat(int dirfd, const char *pathname, int mode, int flags) {
    return ::sys_faccessat(dirfd, pathname, mode, flags);
}

int sys_execve(const char *path, char *const argv[], char *const envp[]) {
    return ::sys_execve(path, argv, envp);
}

int sys_kill(int pid, int sig) {
    return ::sys_kill(pid, sig);
}

pid_t sys_gettid() {
    return (pid_t)::sys_gettid();
}

int sys_tgkill(int tgid, int tid, int sig) {
    return ::sys_tgkill(tgid, tid, sig);
}

int sys_sigaction(int sn, const struct sigaction *sa, struct sigaction *old) {
    if (sn == SIGCANCEL)
        return ENOSYS;
    return ::sys_sigaction(sn, sa, old);
}

int sys_sigprocmask(int how, const sigset_t *set, sigset_t *retrieve) {
    return ::sys_sigprocmask(how, set, retrieve);
}

int sys_thread_sigmask(int how, const sigset_t *set, sigset_t *retrieve) {
    return ::sys_sigprocmask(how, set, retrieve);
}

int sys_fcntl(int fd, int request, va_list args, int *result) {
    unsigned long arg = 0;
    switch (request) {
        case F_GETFD:
        case F_GETFL:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
        case F_GETPIPE_SZ:
            break;
        default:
            arg = va_arg(args, unsigned long);
            break;
    }
    return ::sys_fcntl(fd, request, arg, result);
}

int sys_ioctl(int fd, unsigned long request, void *arg, int *result) {
    return ::sys_ioctl(fd, request, arg, result);
}

int sys_readlink(const char *path, void *buffer, size_t max_size, ssize_t *length) {
    long out = 0;
    int e = ::sys_readlink(path, (char *)buffer, max_size, &out);
    if (!e && length)
        *length = out;
    return e;
}

int sys_readlinkat(int dirfd, const char *path, void *buffer, size_t max_size, ssize_t *length) {
    long out = 0;
    int e = ::sys_readlinkat(dirfd, path, (char *)buffer, max_size, &out);
    if (!e && length)
        *length = out;
    return e;
}

int sys_rename(const char *path, const char *new_path) {
    return ::sys_rename(path, new_path);
}

int sys_umask(mode_t mode, mode_t *old) {
    int out = 0;
    int e = ::sys_umask((int)mode, &out);
    if (!e && old)
        *old = (mode_t)out;
    return e;
}

int sys_socket(int family, int type, int protocol, int *fd) {
    return ::sys_socket(family, type, protocol, fd);
}

int sys_connect(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
    return ::sys_connect(fd, addr_ptr, addr_length);
}

int sys_bind(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
    return ::sys_bind(fd, addr_ptr, addr_length);
}

int sys_listen(int fd, int backlog) {
    return ::sys_listen(fd, backlog);
}

int sys_accept(int fd, int *newfd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
    return ::sys_accept(fd, newfd, addr_ptr, addr_length, flags);
}

int sys_sockname(int fd, struct sockaddr *addr_ptr, socklen_t max_addr_length, socklen_t *actual_length) {
    return ::sys_sockname(fd, addr_ptr, max_addr_length, actual_length);
}

int sys_peername(int fd, struct sockaddr *addr_ptr, socklen_t max_addr_length, socklen_t *actual_length) {
    return ::sys_peername(fd, addr_ptr, max_addr_length, actual_length);
}

int sys_setsockopt(int fd, int layer, int number, const void *buffer, socklen_t size) {
    return ::sys_setsockopt(fd, layer, number, buffer, size);
}

int sys_getsockopt(int fd, int layer, int number, void *buffer, socklen_t *size) {
    return ::sys_getsockopt(fd, layer, number, buffer, size);
}

int sys_shutdown(int fd, int how) {
    return ::sys_shutdown(fd, how);
}

int sys_open_dir(const char *path, int *handle) {
    int fd = -1;
    int e = ::sys_open(path, O_RDONLY, 0, &fd);
    if (!e && handle)
        *handle = fd;
    return e;
}

int sys_read_entries(int handle, void *buffer, size_t max_size, size_t *bytes_read) {
    long out = 0;
    int e = ::sys_getdents64(handle, buffer, max_size, &out);
    if (!e && bytes_read)
        *bytes_read = (size_t)out;
    return e;
}

int sys_poll(struct pollfd *fds, nfds_t count, int timeout, int *num_events) {
    return ::sys_poll(fds, (unsigned long)count, timeout, num_events);
}

int sys_waitpid(int pid, int *status, int flags, struct rusage *ru, int *ret_pid) {
    int out = 0;
    int e = ::sys_waitpid(pid, status, flags, (void *)ru, &out);
    if (!e && ret_pid)
        *ret_pid = out;
    return e;
}

int sys_isatty(int fd) {
    return ::sys_isatty(fd);
}

int sys_stat(fsfd_target fsfdt, int fd, const char *path, int flags, struct stat *statbuf) {
    switch (fsfdt) {
        case fsfd_target::path:
            /* stat() / lstat() — use fstatat with AT_FDCWD */
            return ::sys_fstatat_raw(-100, path, statbuf, flags);
        case fsfd_target::fd:
            /* fstat() — use fstatat with AT_EMPTY_PATH */
            return ::sys_fstatat_raw(fd, "", statbuf, flags | 0x1000);
        case fsfd_target::fd_path:
            /* fstatat() */
            return ::sys_fstatat_raw(fd, path, statbuf, flags);
        default:
            return EINVAL;
    }
}

int sys_tcgetattr(int fd, struct termios *attr) {
    return ::sys_tcgetattr(fd, (void *)attr);
}

int sys_tcsetattr(int fd, int optional_action, const struct termios *attr) {
    return ::sys_tcsetattr(fd, optional_action, (const void *)attr);
}

int sys_clock_get(int clock, long *secs, long *nanos) {
    long s = 0, ns = 0;
    int e = ::sys_clock_gettime(clock, &s, &ns);
    if (!e) {
        if (secs) *secs = s;
        if (nanos) *nanos = ns;
    }
    return e;
}

int sys_futex_wait(int *pointer, int expected, const struct timespec *time) {
    return ::sys_futex_wait(pointer, expected, time);
}

int sys_futex_wake(int *pointer, bool) {
    return ::sys_futex_wake(pointer);
}

int sys_tcb_set(void *pointer) {
    return ::sys_tcb_set(pointer);
}

int sys_vm_map(void *hint, size_t size, int prot, int flags,
        int fd, off_t offset, void **out) {
    return ::sys_vm_map(hint, size, prot, flags, fd, (long)offset, out);
}

int sys_vm_unmap(void *address, size_t size) {
    return ::sys_vm_unmap(address, size);
}

int sys_anon_allocate(size_t size, void **pointer) {
    return ::sys_anon_allocate(size, pointer);
}

int sys_anon_free(void *pointer, size_t size) {
    return ::sys_anon_free(pointer, size);
}

int sys_prepare_stack(void **stack, void *entry, void *user_arg, void *tcb,
        size_t *stack_size, size_t *guard_size, void **stack_base) {
    (void)tcb;

    if (!*stack_size)
        *stack_size = 0x200000;

    uintptr_t map;
    if (*stack) {
        map = (uintptr_t)*stack;
        *guard_size = 0;
    } else {
        void *mapped = nullptr;
        int e = ::sys_vm_map(nullptr, *stack_size + *guard_size,
                0, 0x22, -1, 0, &mapped);
        if (e)
            return e;
        map = (uintptr_t)mapped;

        e = ::sys_vm_protect((void *)(map + *guard_size), *stack_size, 3);
        if (e)
            return e;
    }

    *stack_base = (void *)map;

    auto sp = (uintptr_t *)(map + *guard_size + *stack_size);
    *--sp = (uintptr_t)user_arg;
    *--sp = (uintptr_t)entry;
    *stack = (void *)sp;

    return 0;
}

int sys_clone(void *tcb, int *pid_out, void *stack) {
    unsigned long flags = 0x00000100UL | 0x00000200UL | 0x00000400UL |
            0x00000800UL | 0x00010000UL | 0x00080000UL |
            0x00100000UL;

    long ret = __exo_spawn_thread(flags, stack, (int *)pid_out, tcb);
    if (ret < 0)
        return (int)(-ret);

    return 0;
}

[[noreturn]] void sys_thread_exit() {
    ::sys_exit(0);
    __builtin_unreachable();
}

void sys_exit(int status) {
    ::sys_exit(status);
}

void sys_libc_log(const char *msg) {
    long ignored = 0;
    if (!msg)
        return;
    const char *p = msg;
    size_t len = 0;
    while (*p++)
        len++;
    (void)::sys_write(2, msg, len, &ignored);
}

[[noreturn]] void sys_libc_panic() {
    ::sys_exit(127);
    __builtin_unreachable();
}

}
