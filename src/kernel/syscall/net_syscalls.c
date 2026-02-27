/* syscall/net_syscalls.c — Socket system calls (Linux x86-64 ABI)
 *
 * These match the Linux syscall numbers:
 *   41 = socket,  42 = connect,  43 = accept,
 *   44 = sendto,  45 = recvfrom, 46 = sendmsg (stub),
 *   47 = recvmsg (stub), 48 = shutdown,
 *   49 = bind,    50 = listen,
 *   51 = getsockname, 52 = getpeername,
 *   53 = socketpair (stub), 54 = setsockopt, 55 = getsockopt
 */
#include "syscall.h"
#include "net/socket.h"
#include "net/socket_defs.h"
#include "drivers/input/input.h"
#include "sched/sched.h"
#include "lib/klog.h"
#include "lib/string.h"

/* ioctl requests (Linux ABI subset) */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define FIONBIO     0x5421

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

#define NCCS 19

typedef struct {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
} kernel_termios_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} kernel_winsize_t;

static kernel_termios_t g_tty_termios = {
    .c_iflag = 0x00000500U,
    .c_oflag = 0x00000005U,
    .c_cflag = 0x000000BFU,
    .c_lflag = 0x00008A3BU,
    .c_line = 0,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0 },
    .c_ispeed = 38400,
    .c_ospeed = 38400,
};

static inline bool is_tty_file(const file_t *f) {
    return f && f->path[0] && strcmp(f->path, "/dev/tty") == 0;
}

static void sync_socket_nonblock(file_t *f) {
    if (!f || f->f_ops != &g_socket_file_ops) return;
    socket_t *sk = (socket_t *)f->private_data;
    if (!sk) return;
    sk->nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;
}

static int tty_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    (void)f;
    if (!arg) return -EINVAL;

    if (cmd == TCGETS) {
        kernel_termios_t *user_t = (kernel_termios_t *)(uintptr_t)arg;
        *user_t = g_tty_termios;
        return 0;
    }
    if (cmd == TCSETS) {
        const kernel_termios_t *user_t = (const kernel_termios_t *)(uintptr_t)arg;
        g_tty_termios = *user_t;
        return 0;
    }
    if (cmd == TIOCGWINSZ) {
        kernel_winsize_t *ws = (kernel_winsize_t *)(uintptr_t)arg;
        ws->ws_row = 25;
        ws->ws_col = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    return -EINVAL;
}

/* ── socket(2) ───────────────────────────────────────────────────────────── */
int64_t sys_socket(int domain, int type, int protocol) {
    int r = socket_create(domain, type, protocol);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── connect(2) ──────────────────────────────────────────────────────────── */
int64_t sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->connect) return -EINVAL;
    int r = sk->proto_ops->connect(sk, addr, addrlen);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── accept(2) ───────────────────────────────────────────────────────────── */
int64_t sys_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->accept) return -EINVAL;
    int r = sk->proto_ops->accept(sk, addr, addrlen);
    if (r == -1) return -EAGAIN;
    return (int64_t)r;
}

/* ── sendto(2) ───────────────────────────────────────────────────────────── */
int64_t sys_sendto(int fd, const void *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->sendto) return -EINVAL;
    int r = (int)sk->proto_ops->sendto(sk, buf, len, flags,
                                       dest_addr, addrlen);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── recvfrom(2) ─────────────────────────────────────────────────────────── */
int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->recvfrom) return -EINVAL;
    int r = (int)sk->proto_ops->recvfrom(sk, buf, len, flags,
                                         src_addr, addrlen);
    if (r == -1) return -EAGAIN;
    return (int64_t)r;
}

/* ── shutdown(2) ─────────────────────────────────────────────────────────── */
int64_t sys_shutdown(int fd, int how) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->shutdown) return -EINVAL;
    int r = sk->proto_ops->shutdown(sk, how);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── bind(2) ─────────────────────────────────────────────────────────────── */
int64_t sys_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->bind) return -EINVAL;
    int r = sk->proto_ops->bind(sk, addr, addrlen);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── listen(2) ───────────────────────────────────────────────────────────── */
int64_t sys_listen(int fd, int backlog) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->listen) return -EINVAL;
    int r = sk->proto_ops->listen(sk, backlog);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── getsockname(2) ──────────────────────────────────────────────────────── */
int64_t sys_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    memcpy(addr, &sk->local_addr, sizeof(struct sockaddr_in));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

/* ── getpeername(2) ──────────────────────────────────────────────────────── */
int64_t sys_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->connected) return -EINVAL;
    if (!addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    memcpy(addr, &sk->remote_addr, sizeof(struct sockaddr_in));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

/* ── setsockopt(2) ───────────────────────────────────────────────────────── */
int64_t sys_setsockopt(int fd, int level, int optname,
                       const void *optval, socklen_t optlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->setsockopt) return -EINVAL;
    int r = sk->proto_ops->setsockopt(sk, level, optname, optval, optlen);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── getsockopt(2) ───────────────────────────────────────────────────────── */
int64_t sys_getsockopt(int fd, int level, int optname,
                       void *optval, socklen_t *optlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->getsockopt) return -EINVAL;
    int r = sk->proto_ops->getsockopt(sk, level, optname, optval, optlen);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── poll(2) ─────────────────────────────────────────────────────────────── */
int64_t sys_poll(struct pollfd *fds, uint64_t nfds, int timeout) {
    if (!fds || nfds == 0) return 0;

    task_t *cur = sched_current();
    int ready;
    uint64_t start_ticks = sched_get_ticks();
    uint64_t deadline = (timeout > 0) ? (start_ticks + (uint64_t)timeout) : 0;

    for (;;) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            file_t *f = fd_get(cur, fds[i].fd);
            if (!f) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            if (f->f_ops && f->f_ops->poll) {
                fds[i].revents = (int16_t)f->f_ops->poll(f, fds[i].events);
                if (fds[i].revents) ready++;
            } else if (is_tty_file(f)) {
                int mask = 0;
                if ((fds[i].events & (POLLIN | POLLRDNORM)) && input_tty_char_available())
                    mask |= (POLLIN | POLLRDNORM);
                if (fds[i].events & (POLLOUT | POLLWRNORM))
                    mask |= (POLLOUT | POLLWRNORM);
                fds[i].revents = (int16_t)mask;
                if (mask) ready++;
            }
        }
        if (ready > 0) break;
        if (timeout == 0) break;

        if (timeout > 0) {
            uint64_t now = sched_get_ticks();
            if (now >= deadline) break;
            uint64_t remain = deadline - now;
            sched_sleep((uint32_t)(remain > 10 ? 10 : remain));
        } else {
            sched_sleep(10);
        }
    }
    return (int64_t)ready;
}

/* ── ioctl(2) — basic network ioctl ──────────────────────────────────────── */
int64_t sys_ioctl(int fd, unsigned long cmd, unsigned long arg) {
    task_t *cur = sched_current();
    file_t *f = fd_get(cur, fd);
    if (!f) return -EBADF;

    if (cmd == FIONBIO) {
        if (!arg) return -EINVAL;
        int nb = *(int *)(uintptr_t)arg;
        if (nb)
            f->flags |= O_NONBLOCK;
        else
            f->flags &= ~O_NONBLOCK;
        sync_socket_nonblock(f);
        return 0;
    }

    if (f->f_ops && f->f_ops->ioctl)
        return (int64_t)f->f_ops->ioctl(f, cmd, arg);

    if (is_tty_file(f))
        return (int64_t)tty_ioctl(f, cmd, arg);

    return -EINVAL;
}
