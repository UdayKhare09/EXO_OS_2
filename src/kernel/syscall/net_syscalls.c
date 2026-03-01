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
#include "drivers/net/netdev.h"
#include "drivers/input/input.h"
#include "gfx/fbcon.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/cred.h"
#include "ipc/signal.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "net/netutil.h"

extern int64_t sys_pipe2(int pipefd[2], int flags);

/* ioctl requests (Linux ABI subset) */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCNOTTY   0x5422
#define FIONBIO     0x5421

/* socket interface ioctls used by ifconfig */
#define SIOCGIFCONF   0x8912
#define SIOCGIFFLAGS  0x8913
#define SIOCSIFFLAGS  0x8914
#define SIOCGIFADDR   0x8915
#define SIOCSIFADDR   0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFHWADDR  0x8927

#define IFF_UP         0x0001
#define IFF_BROADCAST  0x0002
#define IFF_RUNNING    0x0040

#define ARPHRD_ETHER   1

typedef struct {
    char sa_data[14];
    uint16_t sa_family;
} __attribute__((packed)) kernel_sockaddr_alt_t;

typedef struct {
    char ifr_name[16];
    union {
        struct sockaddr     ifru_addr;
        struct sockaddr     ifru_netmask;
        struct sockaddr     ifru_broadaddr;
        struct sockaddr     ifru_hwaddr;
        int16_t             ifru_flags;
    } ifr_ifru;
} kernel_ifreq_t;

typedef struct {
    int ifc_len;
    union {
        char          *ifc_buf;
        kernel_ifreq_t *ifc_req;
    } ifc_ifcu;
} kernel_ifconf_t;

#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags

static netdev_t *ioctl_pick_dev(const char ifname[16]) {
    if (ifname && ifname[0]) {
        netdev_t *by_name = netdev_get_by_name(ifname);
        if (by_name) return by_name;
    }
    return netdev_get_nth(0);
}

static void ioctl_fill_sockaddr_in(struct sockaddr *sa, uint32_t ip_nbo) {
    struct sockaddr_in *sin = (struct sockaddr_in *)sa;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = ip_nbo;
}

static uint32_t ioctl_ipv4_broadcast(uint32_t ip, uint32_t mask) {
    return (ip & mask) | ~mask;
}

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

static int g_tty_fg_pgid = 1;

static inline int current_has_cap(uint64_t cap) {
    task_t *cur = sched_current();
    return cur && capable(&cur->cred, cap);
}

uint32_t tty_get_iflag(void) {
    return g_tty_termios.c_iflag;
}

uint32_t tty_get_lflag(void) {
    return g_tty_termios.c_lflag;
}

uint32_t tty_get_oflag(void) {
    return g_tty_termios.c_oflag;
}

void tty_set_fg_pgid(int pgid) {
    g_tty_fg_pgid = pgid;
}

uint8_t tty_get_cc(int idx) {
    if (idx < 0 || idx >= 19) return 0;
    return g_tty_termios.c_cc[idx];
}

void tty_signal_foreground(int sig) {
    if (sig <= 0 || sig >= NSIGS) return;
    for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t) continue;
        if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE) continue;
        if ((int)t->pgid == g_tty_fg_pgid)
            signal_send(t, sig);
    }
}

static inline bool is_tty_path(const char *path) {
    if (!path || !path[0]) return false;
    return strcmp(path, "/dev/tty") == 0 ||
           strcmp(path, "/dev/console") == 0 ||
           strcmp(path, "/dev/stdin") == 0 ||
           strcmp(path, "/dev/stdout") == 0 ||
           strcmp(path, "/dev/stderr") == 0;
}

static inline bool is_tty_file(const file_t *f) {
    return f && is_tty_path(f->path);
}

static void sync_socket_nonblock(file_t *f) {
    if (!f || f->f_ops != &g_socket_file_ops) return;
    socket_t *sk = (socket_t *)f->private_data;
    if (!sk) return;
    sk->nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;
}

static int tty_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    (void)f;
    if (cmd == TCGETS) {
        if (!arg) return -EINVAL;
        kernel_termios_t *user_t = (kernel_termios_t *)(uintptr_t)arg;
        *user_t = g_tty_termios;
        return 0;
    }
    if (cmd == TCSETS || cmd == TCSETSW || cmd == TCSETSF) {
        if (!arg) return -EINVAL;
        const kernel_termios_t *user_t = (const kernel_termios_t *)(uintptr_t)arg;
        g_tty_termios = *user_t;
        return 0;
    }
    if (cmd == TIOCGPGRP) {
        if (!arg) return -EINVAL;
        int *pgid = (int *)(uintptr_t)arg;
        *pgid = g_tty_fg_pgid;
        return 0;
    }
    if (cmd == TIOCSPGRP) {
        if (!arg) return -EINVAL;
        int new_pgid = *(int *)(uintptr_t)arg;
        if (new_pgid <= 0)
            return -EINVAL;
        g_tty_fg_pgid = new_pgid;
        return 0;
    }
    if (cmd == TIOCGWINSZ) {
        if (!arg) return -EINVAL;
        kernel_winsize_t *ws = (kernel_winsize_t *)(uintptr_t)arg;
        int rows = fbcon_text_rows();
        int cols = fbcon_text_cols();
        int xpx = fbcon_pixel_width();
        int ypx = fbcon_pixel_height();
        ws->ws_row = (uint16_t)((rows > 0) ? rows : 25);
        ws->ws_col = (uint16_t)((cols > 0) ? cols : 80);
        ws->ws_xpixel = (uint16_t)((xpx > 0) ? xpx : 640);
        ws->ws_ypixel = (uint16_t)((ypx > 0) ? ypx : 480);
        return 0;
    }
    if (cmd == TIOCSWINSZ) {
        if (!arg) return -EINVAL;
        /* update window size and deliver SIGWINCH to foreground process group */
        const kernel_winsize_t *ws = (const kernel_winsize_t *)(uintptr_t)arg;
        (void)ws; /* fbcon drives real dimensions; just signal SIGWINCH */
        tty_signal_foreground(28); /* SIGWINCH = 28 */
        return 0;
    }
    if (cmd == TIOCSCTTY) {
        /* make this terminal the controlling terminal of the calling session
         * Linux: sets tty->session = current->session, tty->pgrp = current->pgrp */
        task_t *cur = sched_current();
        if (cur) {
            g_tty_fg_pgid = (int)cur->pgid;
        }
        return 0;
    }
    if (cmd == TIOCNOTTY) {
        /* detach from controlling terminal — Linux sets current->signal->tty = NULL */
        /* We have a single global TTY so nothing to detach; just succeed */
        return 0;
    }
    return -EINVAL;
}

/* ── socket(2) ───────────────────────────────────────────────────────────── */
int64_t sys_socket(int domain, int type, int protocol) {
    int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (base_type == SOCK_RAW && !current_has_cap(CAP_NET_RAW))
        return -EPERM;

    int r = socket_create(domain, type, protocol);
    if (r == -1) return -EINVAL;
    return (int64_t)r;
}

/* ── socketpair(2) ───────────────────────────────────────────────────────── */
int64_t sys_socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!sv) return -EFAULT;
    int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_UNIX && domain != AF_LOCAL) return -EINVAL;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM &&
        base_type != SOCK_SEQPACKET) return -EINVAL;
    (void)protocol;
    extern int unix_socketpair(int type, int sv[2]);
    return (int64_t)unix_socketpair(type, sv);
}

/* ── connect(2) ──────────────────────────────────────────────────────────── */
int64_t sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    socket_t *sk = socket_from_fd(fd);
    if (!sk) return -EBADF;
    if (!sk->proto_ops || !sk->proto_ops->connect) return -EINVAL;
    int r = sk->proto_ops->connect(sk, addr, addrlen);
    if (r < 0) return (r == -1) ? -EINVAL : r;
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
    if (r < 0) return (r == -1) ? -EINVAL : r;
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
    if (r == -2) return -EINTR;
    if (r < 0) return (r == -1) ? -EAGAIN : r;
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

    if (addr) {
        uint16_t port = 0;
        if (addr->sa_family == AF_INET && addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
            port = ntohs(sin->sin_port);
        } else if (addr->sa_family == AF_INET6 && addrlen >= 4) {
            const uint8_t *raw = (const uint8_t *)addr;
            uint16_t p = 0;
            memcpy(&p, raw + 2, sizeof(p));
            port = ntohs(p);
        }

        if (port != 0 && port < 1024 && !current_has_cap(CAP_NET_BIND_SERVICE))
            return -EACCES;
    }

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
    if (r < 0) return (r == -1) ? -EINVAL : r;
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

    if (f->f_ops == &g_socket_file_ops) {
        if (cmd == SIOCGIFCONF) {
            kernel_ifconf_t *ifc = (kernel_ifconf_t *)(uintptr_t)arg;
            if (!ifc || !ifc->ifc_ifcu.ifc_req || ifc->ifc_len < (int)sizeof(kernel_ifreq_t))
                return -EINVAL;

            int total = netdev_count();
            int max = ifc->ifc_len / (int)sizeof(kernel_ifreq_t);
            int n = (total < max) ? total : max;

            for (int i = 0; i < n; i++) {
                netdev_t *dev = netdev_get_nth(i);
                if (!dev) continue;
                kernel_ifreq_t *ifr = &ifc->ifc_ifcu.ifc_req[i];
                memset(ifr, 0, sizeof(*ifr));
                strncpy(ifr->ifr_name, dev->name, sizeof(ifr->ifr_name) - 1);
                ioctl_fill_sockaddr_in(&ifr->ifr_addr, dev->ip_addr);
            }
            ifc->ifc_len = n * (int)sizeof(kernel_ifreq_t);
            return 0;
        }

        if (cmd == SIOCGIFFLAGS || cmd == SIOCSIFFLAGS ||
            cmd == SIOCGIFADDR || cmd == SIOCSIFADDR ||
            cmd == SIOCGIFNETMASK || cmd == SIOCSIFNETMASK ||
            cmd == SIOCGIFBRDADDR || cmd == SIOCGIFHWADDR) {
            kernel_ifreq_t *ifr = (kernel_ifreq_t *)(uintptr_t)arg;
            if (!ifr) return -EINVAL;

            netdev_t *dev = ioctl_pick_dev(ifr->ifr_name);
            if (!dev) return -ENOENT;

            if (cmd == SIOCGIFFLAGS) {
                int16_t flags = IFF_BROADCAST;
                if (dev->link) flags |= (IFF_UP | IFF_RUNNING);
                ifr->ifr_flags = flags;
                return 0;
            }

            if (cmd == SIOCSIFFLAGS) {
                bool up = (ifr->ifr_flags & IFF_UP) != 0;
                dev->link = up;
                return 0;
            }

            if (cmd == SIOCGIFADDR) {
                ioctl_fill_sockaddr_in(&ifr->ifr_addr, dev->ip_addr);
                return 0;
            }

            if (cmd == SIOCSIFADDR) {
                const struct sockaddr_in *sin = (const struct sockaddr_in *)&ifr->ifr_addr;
                dev->ip_addr = sin->sin_addr.s_addr;
                return 0;
            }

            if (cmd == SIOCGIFNETMASK) {
                ioctl_fill_sockaddr_in(&ifr->ifr_netmask, dev->netmask);
                return 0;
            }

            if (cmd == SIOCSIFNETMASK) {
                const struct sockaddr_in *sin = (const struct sockaddr_in *)&ifr->ifr_netmask;
                dev->netmask = sin->sin_addr.s_addr;
                return 0;
            }

            if (cmd == SIOCGIFBRDADDR) {
                ioctl_fill_sockaddr_in(&ifr->ifr_broadaddr,
                                       ioctl_ipv4_broadcast(dev->ip_addr, dev->netmask));
                return 0;
            }

            if (cmd == SIOCGIFHWADDR) {
                memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
                ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
                memcpy(ifr->ifr_hwaddr.sa_data, dev->mac, ETH_ALEN);
                return 0;
            }
        }
    }

    if (f->f_ops && f->f_ops->ioctl)
        return (int64_t)f->f_ops->ioctl(f, cmd, arg);

    if (is_tty_file(f))
        return (int64_t)tty_ioctl(f, cmd, arg);

    return -EINVAL;
}

/* ── sendmsg(2) ──────────────────────────────────────────────────────────── */
int64_t sys_sendmsg(int fd, const msghdr_t *msg, int flags) {
    if (!msg) return -EFAULT;

    /* SCM_RIGHTS: if control message present, pass fds to peer's unix_sock */
    if (msg->msg_control && msg->msg_controllen >= 12) {
        /* struct cmsghdr { uint32_t len; int level; int type; } + payload */
        const uint8_t *ctrl = (const uint8_t *)msg->msg_control;
        uint32_t cmsg_len   = *(const uint32_t *)(ctrl + 0);
        int      cmsg_level = *(const int *)(ctrl + 4);
        int      cmsg_type  = *(const int *)(ctrl + 8);
        if (cmsg_level == 1 /* SOL_SOCKET */ && cmsg_type == 1 /* SCM_RIGHTS */) {
            uint32_t data_len = cmsg_len > 12 ? cmsg_len - 12 : 0;
            int nfds = (int)(data_len / sizeof(int));
            const int *fds_in = (const int *)(ctrl + 12);
            socket_t *sk = socket_from_fd(fd);
            if (sk && sk->domain == AF_UNIX) {
                extern void *unix_sock_from_socket(socket_t *sk);
                extern void *unix_sock_get_peer(void *us);
                extern void  unix_push_files(void *, file_t **, int);
                void *us   = unix_sock_from_socket(sk);
                void *peer = unix_sock_get_peer(us);
                if (peer) {
                    task_t *t = sched_current();
                    file_t *fptrs[16];
                    int n = nfds > 16 ? 16 : nfds;
                    for (int i = 0; i < n; i++) fptrs[i] = fd_get(t, fds_in[i]);
                    unix_push_files(peer, fptrs, n);
                }
            }
        }
    }

    ssize_t total = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t r = sys_sendto(fd, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len,
                               flags, (const struct sockaddr *)msg->msg_name, msg->msg_namelen);
        if (r < 0) return (total > 0) ? total : r;
        total += r;
    }
    return total;
}

/* ── recvmsg(2) ──────────────────────────────────────────────────────────── */
int64_t sys_recvmsg(int fd, msghdr_t *msg, int flags) {
    if (!msg) return -EFAULT;
    ssize_t total = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        socklen_t fromlen = msg->msg_namelen;
        ssize_t r = sys_recvfrom(fd, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len,
                                 flags, (struct sockaddr *)msg->msg_name, &fromlen);
        if (r < 0) return (total > 0) ? total : r;
        if (r == 0) break;
        total += r;
        msg->msg_namelen = fromlen;
    }
    msg->msg_flags = 0;

    /* Fill ancillary data: SCM_CREDENTIALS + SCM_RIGHTS */
    if (msg->msg_control && msg->msg_controllen >= 24) {
        uint8_t *ctrl = (uint8_t *)msg->msg_control;
        uint64_t cap  = msg->msg_controllen;
        uint64_t pos  = 0;

        /* SCM_CREDENTIALS (24 bytes: 12-byte cmsghdr + 12-byte ucred) */
        if (cap - pos >= 24) {
            task_t *cur = sched_current();
            uint32_t *c = (uint32_t *)(ctrl + pos);
            c[0] = 24;                     /* cmsg_len */
            c[1] = 1;                      /* SOL_SOCKET */
            c[2] = 2;                      /* SCM_CREDENTIALS */
            c[3] = cur ? cur->pid : 0;     /* pid */
            c[4] = cur ? cur->cred.euid : 0; /* uid */
            c[5] = cur ? cur->cred.egid : 0; /* gid */
            pos += 24;
        }

        /* SCM_RIGHTS: dequeue any pending fds */
        socket_t *sk = socket_from_fd(fd);
        if (sk && sk->domain == AF_UNIX && cap - pos >= 12) {
            extern void *unix_sock_from_socket(socket_t *);
            extern int   unix_pop_files(void *, task_t *, int *, int);
            void *us = unix_sock_from_socket(sk);
            if (us) {
                int out_fds[16];
                int maxfds = (int)((cap - pos - 12) / sizeof(int));
                if (maxfds > 16) maxfds = 16;
                task_t *cur = sched_current();
                int n = unix_pop_files(us, cur, out_fds, maxfds);
                if (n > 0) {
                    uint32_t rights_len = 12 + (uint32_t)(n * sizeof(int));
                    uint32_t *r = (uint32_t *)(ctrl + pos);
                    r[0] = rights_len;  /* cmsg_len */
                    r[1] = 1;           /* SOL_SOCKET */
                    r[2] = 1;           /* SCM_RIGHTS */
                    memcpy(ctrl + pos + 12, out_fds, (size_t)(n * sizeof(int)));
                    pos += rights_len;
                }
            }
        }
        msg->msg_controllen = pos;
    } else {
        msg->msg_controllen = 0;
    }
    return total;
}

/* ── accept4(2) ──────────────────────────────────────────────────────────── */
int64_t sys_accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    int64_t nfd = sys_accept(fd, addr, addrlen);
    if (nfd < 0) return nfd;
    if (flags) {
        task_t *t = sched_current();
        file_t *f = fd_get(t, (int)nfd);
        if (f) {
            if (flags & SOCK_NONBLOCK) f->flags |= O_NONBLOCK;
            if (flags & SOCK_CLOEXEC)  t->fd_flags[(int)nfd] |= FD_CLOEXEC;
        }
    }
    return nfd;
}
