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
#include "sched/sched.h"
#include "lib/klog.h"
#include "lib/string.h"

/* ── socket(2) ───────────────────────────────────────────────────────────── */
int64_t sys_socket(int domain, int type, int protocol) {
    return (int64_t)socket_create(domain, type, protocol);
}

/* ── connect(2) ──────────────────────────────────────────────────────────── */
int64_t sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->connect) return -1;
    return (int64_t)sk->proto_ops->connect(sk, addr, addrlen);
}

/* ── accept(2) ───────────────────────────────────────────────────────────── */
int64_t sys_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->accept) return -1;
    return (int64_t)sk->proto_ops->accept(sk, addr, addrlen);
}

/* ── sendto(2) ───────────────────────────────────────────────────────────── */
int64_t sys_sendto(int fd, const void *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->sendto) return -1;
    return (int64_t)sk->proto_ops->sendto(sk, buf, len, flags,
                                          dest_addr, addrlen);
}

/* ── recvfrom(2) ─────────────────────────────────────────────────────────── */
int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->recvfrom) return -1;
    return (int64_t)sk->proto_ops->recvfrom(sk, buf, len, flags,
                                            src_addr, addrlen);
}

/* ── shutdown(2) ─────────────────────────────────────────────────────────── */
int64_t sys_shutdown(int fd, int how) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->shutdown) return -1;
    return (int64_t)sk->proto_ops->shutdown(sk, how);
}

/* ── bind(2) ─────────────────────────────────────────────────────────────── */
int64_t sys_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->bind) return -1;
    return (int64_t)sk->proto_ops->bind(sk, addr, addrlen);
}

/* ── listen(2) ───────────────────────────────────────────────────────────── */
int64_t sys_listen(int fd, int backlog) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->listen) return -1;
    return (int64_t)sk->proto_ops->listen(sk, backlog);
}

/* ── getsockname(2) ──────────────────────────────────────────────────────── */
int64_t sys_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -1;
    memcpy(addr, &sk->local_addr, sizeof(struct sockaddr_in));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

/* ── getpeername(2) ──────────────────────────────────────────────────────── */
int64_t sys_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk || !sk->connected) return -1;
    if (!addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -1;
    memcpy(addr, &sk->remote_addr, sizeof(struct sockaddr_in));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

/* ── setsockopt(2) ───────────────────────────────────────────────────────── */
int64_t sys_setsockopt(int fd, int level, int optname,
                       const void *optval, socklen_t optlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->setsockopt) return -1;
    return (int64_t)sk->proto_ops->setsockopt(sk, level, optname,
                                               optval, optlen);
}

/* ── getsockopt(2) ───────────────────────────────────────────────────────── */
int64_t sys_getsockopt(int fd, int level, int optname,
                       void *optval, socklen_t *optlen)
{
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -1;
    if (!sk->proto_ops || !sk->proto_ops->getsockopt) return -1;
    return (int64_t)sk->proto_ops->getsockopt(sk, level, optname,
                                               optval, optlen);
}

/* ── poll(2) ─────────────────────────────────────────────────────────────── */
int64_t sys_poll(struct pollfd *fds, uint64_t nfds, int timeout) {
    if (!fds || nfds == 0) return 0;

    task_t *cur = sched_current();
    int ready = 0;

    /* simple poll implementation: check once, optionally block */
    for (uint64_t attempt = 0; ; attempt++) {
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
            }
        }
        if (ready > 0 || timeout == 0) break;
        /* block briefly and retry */
        if (timeout > 0 && attempt > 0) break; /* simplified: try twice */
        sched_sleep(timeout > 0 ? (uint32_t)timeout : 10);
    }
    return (int64_t)ready;
}

/* ── ioctl(2) — basic network ioctl ──────────────────────────────────────── */
int64_t sys_ioctl(int fd, unsigned long cmd, unsigned long arg) {
    task_t *cur = sched_current();
    file_t *f = fd_get(cur, fd);
    if (!f) return -1;
    if (f->f_ops && f->f_ops->ioctl)
        return (int64_t)f->f_ops->ioctl(f, cmd, arg);
    return -1;
}
