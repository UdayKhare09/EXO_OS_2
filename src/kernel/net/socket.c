/* net/socket.c — Socket layer: bridges fd ↔ TCP/UDP
 *
 * Each socket_create() allocates a socket_t, wraps it in a file_t via
 * file_alloc_generic(&g_socket_file_ops, socket), and installs the fd
 * into the current task.
 */
#include "net/socket.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "net/ipv4.h"
#include "net/netutil.h"
#include "fs/fd.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* forward declare protocol ops tables */
static socket_proto_ops_t tcp_proto_ops;
static socket_proto_ops_t udp_proto_ops;
static socket_proto_ops_t raw_icmp_proto_ops;

static inline int socket_has_unmasked_signal_pending(void) {
    task_t *cur = sched_current();
    if (!cur) return 0;
    uint32_t pending = __atomic_load_n(&cur->sig_pending, __ATOMIC_RELAXED);
    return (pending & ~cur->sig_mask) != 0;
}

#define RAW_ICMP_MAX_SOCKS 32
static socket_t *g_raw_icmp_socks[RAW_ICMP_MAX_SOCKS];

static int raw_icmp_register(socket_t *sk) {
    for (int i = 0; i < RAW_ICMP_MAX_SOCKS; i++) {
        if (!g_raw_icmp_socks[i]) {
            g_raw_icmp_socks[i] = sk;
            return 0;
        }
    }
    return -1;
}

static void raw_icmp_unregister(socket_t *sk) {
    for (int i = 0; i < RAW_ICMP_MAX_SOCKS; i++) {
        if (g_raw_icmp_socks[i] == sk) {
            g_raw_icmp_socks[i] = NULL;
            return;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  file_ops_t callbacks (dispatched from sys_read/sys_write/sys_close etc.)
 * ══════════════════════════════════════════════════════════════════════════ */

static ssize_t sock_file_read(file_t *f, void *buf, size_t count) {
    socket_t *sk = (socket_t *)f->private_data;
    if (!sk || !sk->proto_ops || !sk->proto_ops->recvfrom) return -1;
    return sk->proto_ops->recvfrom(sk, buf, count, 0, NULL, NULL);
}

static ssize_t sock_file_write(file_t *f, const void *buf, size_t count) {
    socket_t *sk = (socket_t *)f->private_data;
    if (!sk || !sk->proto_ops || !sk->proto_ops->sendto) return -1;
    return sk->proto_ops->sendto(sk, buf, count, 0, NULL, 0);
}

static int sock_file_close(file_t *f) {
    socket_t *sk = (socket_t *)f->private_data;
    if (!sk) return 0;
    if (sk->proto_ops && sk->proto_ops->close)
        sk->proto_ops->close(sk);
    skb_queue_purge(&sk->rx_queue);
    kfree(sk);
    f->private_data = NULL;
    return 0;
}

static int sock_file_poll(file_t *f, int events) {
    socket_t *sk = (socket_t *)f->private_data;
    if (!sk || !sk->proto_ops || !sk->proto_ops->poll) return 0;
    return sk->proto_ops->poll(sk, events);
}

static int sock_file_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    (void)f; (void)cmd; (void)arg;
    return -1;  /* TODO: SIOCG* ioctls */
}

file_ops_t g_socket_file_ops = {
    .read  = sock_file_read,
    .write = sock_file_write,
    .close = sock_file_close,
    .poll  = sock_file_poll,
    .ioctl = sock_file_ioctl,
};

/* ══════════════════════════════════════════════════════════════════════════
 *  Socket creation / lookup
 * ══════════════════════════════════════════════════════════════════════════ */

void socket_init(void) {
    KLOG_INFO("socket: socket layer initialised\n");
}

int socket_create(int domain, int type, int protocol) {
    if (domain != AF_INET) return -1;  /* only IPv4 for now */

    /* determine protocol */
    int real_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (protocol == 0) {
        if (real_type == SOCK_STREAM) protocol = IPPROTO_TCP;
        else if (real_type == SOCK_DGRAM) protocol = IPPROTO_UDP;
        else if (real_type == SOCK_RAW) protocol = IPPROTO_ICMP;
        else return -1;
    }

    if (real_type != SOCK_STREAM && real_type != SOCK_DGRAM && real_type != SOCK_RAW) return -1;

    socket_t *sk = kzalloc(sizeof(socket_t));
    if (!sk) return -1;

    sk->domain   = domain;
    sk->type     = real_type;
    sk->protocol = protocol;
    sk->nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;

    skb_queue_init(&sk->rx_queue);
    waitq_init(&sk->wq_rx);
    waitq_init(&sk->wq_tx);

    if (protocol == IPPROTO_TCP)
        sk->proto_ops = &tcp_proto_ops;
    else if (protocol == IPPROTO_UDP)
        sk->proto_ops = &udp_proto_ops;
    else if (real_type == SOCK_RAW && protocol == IPPROTO_ICMP)
        sk->proto_ops = &raw_icmp_proto_ops;
    else {
        kfree(sk);
        return -1;
    }

    if (sk->proto_ops == &raw_icmp_proto_ops) {
        if (raw_icmp_register(sk) < 0) {
            kfree(sk);
            return -1;
        }
    }

    /* allocate file_t and fd */
    int flags = 0; /* O_RDWR */
    file_t *f = file_alloc_generic(&g_socket_file_ops, sk, flags);
    if (!f) { kfree(sk); return -1; }

    if (type & SOCK_CLOEXEC)
        f->fd_flags |= FD_CLOEXEC;

    task_t *cur = sched_current();
    int fd = fd_alloc(cur, f);
    if (fd < 0) {
        file_put(f);
        return -1;
    }

    KLOG_DEBUG("socket: created %s socket fd=%d\n",
         protocol == IPPROTO_TCP ? "TCP" :
         (protocol == IPPROTO_UDP ? "UDP" : "RAW-ICMP"), fd);
    return fd;
}

void socket_deliver_icmp_rx(skbuff_t *skb) {
    if (!skb || skb->len == 0) return;

    for (int i = 0; i < RAW_ICMP_MAX_SOCKS; i++) {
        socket_t *sk = g_raw_icmp_socks[i];
        if (!sk) continue;

        skbuff_t *copy = skb_alloc(skb->len + 64);
        if (!copy) continue;
        skb_reserve(copy, 64);
        memcpy(skb_put(copy, skb->len), skb->data, skb->len);
        copy->src_ip = skb->src_ip;
        copy->dst_ip = skb->dst_ip;
        copy->protocol = IPPROTO_ICMP;

        skb_queue_push(&sk->rx_queue, copy);
        waitq_wake_one(&sk->wq_rx);
    }
}

socket_t *socket_from_fd(int fd) {
    task_t *cur = sched_current();
    file_t *f = fd_get(cur, fd);
    if (!f) return NULL;
    if (f->f_ops != &g_socket_file_ops) return NULL;
    return (socket_t *)f->private_data;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TCP protocol operations
 * ══════════════════════════════════════════════════════════════════════════ */

static int tcp_sock_connect(socket_t *sk, const struct sockaddr *addr,
                            socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    tcp_tcb_t *tcb = tcp_tcb_alloc();
    if (!tcb) return -1;

    sk->remote_addr = *sin;
    sk->proto_data  = tcb;

    /* find outgoing device */
    uint32_t dst_ip = sin->sin_addr.s_addr;  /* already NBO */
    uint32_t next_hop;
    netdev_t *dev = ip_route(dst_ip, &next_hop);
    if (!dev) { tcp_tcb_free(tcb); sk->proto_data = NULL; return -1; }

    tcb->dev         = dev;
    tcb->local_ip    = dev->ip_addr;
    tcb->remote_ip   = dst_ip;
    tcb->remote_port = ntohs(sin->sin_port);
    tcb->socket      = sk;

    /* assign ephemeral port if not bound */
    if (!sk->bound) {
        tcb->local_port = tcp_alloc_ephemeral();
        sk->local_addr.sin_family = AF_INET;
        sk->local_addr.sin_port   = htons(tcb->local_port);
        sk->local_addr.sin_addr.s_addr = tcb->local_ip;  /* both NBO */
    } else {
        tcb->local_port = ntohs(sk->local_addr.sin_port);
    }

    /* send SYN */
    tcb->snd_nxt = tcb->iss;
    tcb->snd_una = tcb->iss;
    tcb->state   = TCP_SYN_SENT;
    tcp_send_segment(tcb, TCP_SYN, NULL, 0);
    tcp_rexmit_timer_reset(tcb);

    /* block until ESTABLISHED or error */
    while (tcb->state == TCP_SYN_SENT && tcb->so_error == 0) {
        spinlock_release(&tcb->lock);
        waitq_wait(&tcb->wq_connect);
        spinlock_acquire(&tcb->lock);
    }

    if (tcb->state == TCP_ESTABLISHED) {
        sk->connected = 1;
        spinlock_release(&tcb->lock);
        return 0;
    }

    int err = tcb->so_error;
    spinlock_release(&tcb->lock);
    tcp_tcb_free(tcb);
    sk->proto_data = NULL;
    return err ? err : -ETIMEDOUT;
}

static int tcp_sock_bind(socket_t *sk, const struct sockaddr *addr,
                         socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    sk->local_addr = *sin;
    sk->bound = 1;
    return 0;
}

static int tcp_sock_listen(socket_t *sk, int backlog) {
    if (!sk->bound) return -1;

    tcp_tcb_t *tcb = tcp_tcb_alloc();
    if (!tcb) return -1;

    tcb->local_ip   = sk->local_addr.sin_addr.s_addr;  /* both NBO */
    tcb->local_port = ntohs(sk->local_addr.sin_port);
    tcb->state      = TCP_LISTEN;
    tcb->backlog    = (backlog > 0 && backlog <= SOMAXCONN) ? backlog : SOMAXCONN;
    tcb->socket     = sk;

    /* find device */
    if (tcb->local_ip == 0) {
        /* INADDR_ANY — pick first device */
        netdev_t *dev = netdev_get_nth(0);
        if (dev) tcb->dev = dev;
    } else {
        uint32_t nh;
        tcb->dev = ip_route(tcb->local_ip, &nh);
    }

    sk->proto_data = tcb;
    KLOG_INFO("socket: TCP LISTEN on port %u (backlog %d)\n",
         tcb->local_port, tcb->backlog);
    return 0;
}

static int tcp_sock_accept(socket_t *sk, struct sockaddr *addr,
                           socklen_t *addrlen)
{
    tcp_tcb_t *listener = (tcp_tcb_t *)sk->proto_data;
    if (!listener || listener->state != TCP_LISTEN) return -1;

    /* wait for connection in accept queue */
    while (listener->accept_head == listener->accept_tail) {
        if (sk->nonblock) return -1; /* EAGAIN */
        waitq_wait(&listener->wq_accept);
    }

    tcp_tcb_t *child = listener->accept_queue[listener->accept_tail];
    listener->accept_tail = (listener->accept_tail + 1) % SOMAXCONN;

    /* create new socket + file + fd for the accepted connection */
    socket_t *new_sk = kzalloc(sizeof(socket_t));
    if (!new_sk) { tcp_tcb_free(child); return -1; }

    new_sk->domain    = AF_INET;
    new_sk->type      = SOCK_STREAM;
    new_sk->protocol  = IPPROTO_TCP;
    new_sk->proto_ops = &tcp_proto_ops;
    new_sk->proto_data = child;
    new_sk->connected = 1;
    child->socket     = new_sk;

    new_sk->local_addr.sin_family      = AF_INET;
    new_sk->local_addr.sin_port        = htons(child->local_port);
    new_sk->local_addr.sin_addr.s_addr = child->local_ip;  /* both NBO */

    new_sk->remote_addr.sin_family      = AF_INET;
    new_sk->remote_addr.sin_port        = htons(child->remote_port);
    new_sk->remote_addr.sin_addr.s_addr = child->remote_ip;  /* both NBO */

    skb_queue_init(&new_sk->rx_queue);
    waitq_init(&new_sk->wq_rx);
    waitq_init(&new_sk->wq_tx);

    /* fill caller's addr */
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        memcpy(addr, &new_sk->remote_addr, sizeof(struct sockaddr_in));
        *addrlen = sizeof(struct sockaddr_in);
    }

    file_t *f = file_alloc_generic(&g_socket_file_ops, new_sk, 0);
    if (!f) { kfree(new_sk); tcp_tcb_free(child); return -1; }

    task_t *cur = sched_current();
    int fd = fd_alloc(cur, f);
    if (fd < 0) { file_put(f); return -1; }

    KLOG_DEBUG("socket: accepted fd=%d from %d.%d.%d.%d:%u\n",
         fd, IP4_A(child->remote_ip), IP4_B(child->remote_ip),
         IP4_C(child->remote_ip), IP4_D(child->remote_ip),
         child->remote_port);
    return fd;
}

static ssize_t tcp_sock_sendto(socket_t *sk, const void *buf, size_t len,
                               int flags, const struct sockaddr *dest_addr,
                               socklen_t addrlen)
{
    (void)flags; (void)dest_addr; (void)addrlen;
    tcp_tcb_t *tcb = (tcp_tcb_t *)sk->proto_data;
    if (!tcb) return -1;

    spinlock_acquire(&tcb->lock);
    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT) {
        spinlock_release(&tcb->lock);
        return -1;
    }

    /* copy data into TX ring buffer */
    size_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (written < len) {
        uint32_t next = (tcb->tx_head + 1) % tcb->tx_size;
        if (next == tcb->tx_tail) {
            /* TX buffer full — flush and wait */
            tcp_output(tcb);
            if ((tcb->tx_head + 1) % tcb->tx_size == tcb->tx_tail) {
                if (sk->nonblock) break;
                spinlock_release(&tcb->lock);
                waitq_wait(&tcb->wq_send);
                spinlock_acquire(&tcb->lock);
                if (tcb->state != TCP_ESTABLISHED &&
                    tcb->state != TCP_CLOSE_WAIT) {
                    spinlock_release(&tcb->lock);
                    return written > 0 ? (ssize_t)written : -1;
                }
                continue;
            }
        }
        tcb->tx_buf[tcb->tx_head] = src[written];
        tcb->tx_head = next;
        written++;
    }

    /* flush whatever is buffered */
    tcp_output(tcb);
    spinlock_release(&tcb->lock);
    return (ssize_t)written;
}

static ssize_t tcp_sock_recvfrom(socket_t *sk, void *buf, size_t len,
                                 int flags, struct sockaddr *src_addr,
                                 socklen_t *addrlen)
{
    (void)src_addr; (void)addrlen;
    tcp_tcb_t *tcb = (tcp_tcb_t *)sk->proto_data;
    if (!tcb) return -1;

    spinlock_acquire(&tcb->lock);

    /* wait for data */
    while (tcp_rx_available(tcb) == 0) {
        if (tcb->fin_received || tcb->state == TCP_CLOSE_WAIT ||
            tcb->state == TCP_CLOSED) {
            spinlock_release(&tcb->lock);
            return 0;    /* EOF */
        }
        if (sk->nonblock) {
            spinlock_release(&tcb->lock);
            return -1;   /* EAGAIN */
        }
        spinlock_release(&tcb->lock);
        waitq_wait(&tcb->wq_recv);
        spinlock_acquire(&tcb->lock);
    }

    /* copy from RX ring buffer */
    uint32_t avail = tcp_rx_available(tcb);
    size_t   to_copy = (len < avail) ? len : avail;
    uint8_t *dst = (uint8_t *)buf;

    for (size_t i = 0; i < to_copy; i++) {
        dst[i] = tcb->rx_buf[tcb->rx_tail];
        tcb->rx_tail = (tcb->rx_tail + 1) % tcb->rx_size;
    }

    /* update receive window */
    tcb->rcv_wnd = tcp_rx_free(tcb);

    int peek = (flags & MSG_PEEK);
    if (peek) {
        /* roll back — put data back (simplified: just don't advance tail) */
        /* Actually we already advanced; for proper PEEK we'd need to not
         * advance. For simplicity we skip MSG_PEEK support on TCP for now */
    }

    spinlock_release(&tcb->lock);
    return (ssize_t)to_copy;
}

static int tcp_sock_shutdown(socket_t *sk, int how) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)sk->proto_data;
    if (!tcb) return -1;

    spinlock_acquire(&tcb->lock);
    if ((how == SHUT_WR || how == SHUT_RDWR) &&
        !tcb->fin_sent &&
        (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT)) {
        tcb->fin_sent = 1;
        tcb->user_closed = 1;
        tcp_send_segment(tcb, TCP_FIN | TCP_ACK, NULL, 0);
        if (tcb->state == TCP_ESTABLISHED)
            tcb->state = TCP_FIN_WAIT_1;
        else if (tcb->state == TCP_CLOSE_WAIT)
            tcb->state = TCP_LAST_ACK;
    }
    spinlock_release(&tcb->lock);
    return 0;
}

static int tcp_sock_close(socket_t *sk) {
    tcp_sock_shutdown(sk, SHUT_RDWR);
    /* TCB will be freed after TIME_WAIT or RST */
    sk->proto_data = NULL;
    return 0;
}

static int tcp_sock_poll(socket_t *sk, int events) {
    tcp_tcb_t *tcb = (tcp_tcb_t *)sk->proto_data;
    int ready = 0;
    if (!tcb) return POLLERR;

    spinlock_acquire(&tcb->lock);
    if (events & POLLIN) {
        if (tcp_rx_available(tcb) > 0 || tcb->fin_received)
            ready |= POLLIN;
    }
    if (events & POLLOUT) {
        if (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) {
            uint32_t free_tx = tcb->tx_size - 1 -
                ((tcb->tx_head - tcb->tx_tail) % tcb->tx_size);
            if (free_tx > 0) ready |= POLLOUT;
        }
    }
    if (tcb->state == TCP_CLOSED || tcb->so_error)
        ready |= POLLERR;
    if (tcb->fin_received)
        ready |= POLLHUP;
    spinlock_release(&tcb->lock);
    return ready;
}

static int tcp_sock_setsockopt(socket_t *sk, int level, int optname,
                               const void *optval, socklen_t optlen) {
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0; /* silently succeed for now */
}

static int tcp_sock_getsockopt(socket_t *sk, int level, int optname,
                               void *optval, socklen_t *optlen) {
    if (!optval || !optlen || *optlen < sizeof(int)) return -1;

    int value = 0;
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_TYPE:
                value = sk->type;
                break;
            case SO_ERROR: {
                tcp_tcb_t *tcb = (tcp_tcb_t *)sk->proto_data;
                if (!tcb) {
                    value = ENOTCONN;
                } else {
                    spinlock_acquire(&tcb->lock);
                    value = (tcb->so_error < 0) ? -tcb->so_error : tcb->so_error;
                    tcb->so_error = 0;
                    spinlock_release(&tcb->lock);
                }
                break;
            }
            case SO_ACCEPTCONN:
                value = (sk->proto_data &&
                         ((tcp_tcb_t *)sk->proto_data)->state == TCP_LISTEN) ? 1 : 0;
                break;
            default:
                return -1;
        }
    } else {
        return -1;
    }

    *(int *)optval = value;
    *optlen = sizeof(int);
    return 0;
}

static socket_proto_ops_t tcp_proto_ops = {
    .connect    = tcp_sock_connect,
    .bind       = tcp_sock_bind,
    .listen     = tcp_sock_listen,
    .accept     = tcp_sock_accept,
    .sendto     = tcp_sock_sendto,
    .recvfrom   = tcp_sock_recvfrom,
    .shutdown   = tcp_sock_shutdown,
    .close      = tcp_sock_close,
    .poll       = tcp_sock_poll,
    .setsockopt = tcp_sock_setsockopt,
    .getsockopt = tcp_sock_getsockopt,
};

/* ══════════════════════════════════════════════════════════════════════════
 *  UDP protocol operations
 * ══════════════════════════════════════════════════════════════════════════ */

/* UDP RX callback — queues incoming datagram to socket's rx_queue */
static void udp_sock_rx_callback(skbuff_t *skb, uint32_t src_ip,
                                 uint16_t src_port, uint32_t dst_ip,
                                 uint16_t dst_port,
                                 const void *payload, size_t payload_len,
                                 void *ctx)
{
    (void)dst_ip; (void)dst_port; (void)payload; (void)payload_len;
    socket_t *sk = (socket_t *)ctx;
    if (!sk) { skb_free(skb); return; }

    /* store source info in skb for recvfrom */
    skb->src_ip   = src_ip;
    skb->src_port = src_port;

    skb_queue_push(&sk->rx_queue, skb);
    waitq_wake_one(&sk->wq_rx);
}

static int udp_sock_bind(socket_t *sk, const struct sockaddr *addr,
                         socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    uint16_t port = ntohs(sin->sin_port);
    uint32_t ip   = sin->sin_addr.s_addr;  /* already NBO */

    int rc = udp_bind_port(port, ip, udp_sock_rx_callback, sk);
    if (rc < 0) return -1;

    sk->local_addr = *sin;
    sk->bound = 1;
    return 0;
}

static int udp_sock_connect(socket_t *sk, const struct sockaddr *addr,
                            socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    sk->remote_addr = *sin;
    sk->connected   = 1;

    /* auto-bind if not already bound */
    if (!sk->bound) {
        uint16_t eph = udp_alloc_ephemeral();
        if (eph == 0) return -1;
        int rc = udp_bind_port(eph, 0, udp_sock_rx_callback, sk);
        if (rc < 0) return -1;
        sk->local_addr.sin_family = AF_INET;
        sk->local_addr.sin_port   = htons(eph);
        sk->bound = 1;
    }
    return 0;
}

static ssize_t udp_sock_sendto(socket_t *sk, const void *buf, size_t len,
                               int flags, const struct sockaddr *dest_addr,
                               socklen_t addrlen)
{
    (void)flags;
    struct sockaddr_in dst;

    if (dest_addr && addrlen >= sizeof(struct sockaddr_in)) {
        memcpy(&dst, dest_addr, sizeof(dst));
    } else if (sk->connected) {
        dst = sk->remote_addr;
    } else {
        return -1;  /* EDESTADDRREQ */
    }

    /* auto-bind if needed */
    if (!sk->bound) {
        uint16_t eph = udp_alloc_ephemeral();
        if (eph == 0) return -1;
        int rc = udp_bind_port(eph, 0, udp_sock_rx_callback, sk);
        if (rc < 0) return -1;
        sk->local_addr.sin_family = AF_INET;
        sk->local_addr.sin_port   = htons(eph);
        sk->bound = 1;
    }

    uint32_t dst_ip = dst.sin_addr.s_addr;  /* already NBO */
    uint32_t nh;
    netdev_t *dev = ip_route(dst_ip, &nh);
    if (!dev) return -1;

    uint16_t src_port = ntohs(sk->local_addr.sin_port);
    uint16_t dst_port = ntohs(dst.sin_port);

    int rc = udp_tx(dev, dst_ip, src_port, dst_port, buf, len);
    return rc < 0 ? -1 : (ssize_t)len;
}

static ssize_t udp_sock_recvfrom(socket_t *sk, void *buf, size_t len,
                                 int flags, struct sockaddr *src_addr,
                                 socklen_t *addrlen)
{
    (void)flags;

    /* wait for a datagram */
    while (skb_queue_empty(&sk->rx_queue)) {
        if (socket_has_unmasked_signal_pending()) return -2; /* EINTR */
        if (sk->nonblock) return -1; /* EAGAIN */
        waitq_wait(&sk->wq_rx);
        if (socket_has_unmasked_signal_pending()) return -2; /* EINTR */
    }

    skbuff_t *skb = skb_queue_pop(&sk->rx_queue);
    if (!skb) return -1;

    /* skip the UDP header to get payload */
    size_t hdr_sz = sizeof(uint16_t) * 4;  /* 8 bytes UDP header */
    const uint8_t *payload = skb->data + hdr_sz;
    size_t payload_len = (skb->len > hdr_sz) ? (skb->len - hdr_sz) : 0;

    size_t copy = (len < payload_len) ? len : payload_len;
    memcpy(buf, payload, copy);

    /* fill source address */
    if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
        sin->sin_family      = AF_INET;
        sin->sin_port        = htons(skb->src_port);
        sin->sin_addr.s_addr = skb->src_ip;  /* both NBO */
        *addrlen = sizeof(struct sockaddr_in);
    }

    skb_free(skb);
    return (ssize_t)copy;
}

static int udp_sock_shutdown(socket_t *sk, int how) {
    (void)sk; (void)how;
    return 0;
}

static int udp_sock_close(socket_t *sk) {
    if (sk->bound) {
        uint16_t port = ntohs(sk->local_addr.sin_port);
        udp_unbind_port(port);
    }
    sk->proto_data = NULL;
    return 0;
}

static int udp_sock_poll(socket_t *sk, int events) {
    int ready = 0;
    if ((events & POLLIN) && !skb_queue_empty(&sk->rx_queue))
        ready |= POLLIN;
    if (events & POLLOUT)
        ready |= POLLOUT;  /* UDP is always writable */
    return ready;
}

static int udp_sock_setsockopt(socket_t *sk, int level, int optname,
                               const void *optval, socklen_t optlen) {
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int udp_sock_getsockopt(socket_t *sk, int level, int optname,
                               void *optval, socklen_t *optlen) {
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    return -1;
}

static socket_proto_ops_t udp_proto_ops = {
    .connect    = udp_sock_connect,
    .bind       = udp_sock_bind,
    .listen     = NULL,
    .accept     = NULL,
    .sendto     = udp_sock_sendto,
    .recvfrom   = udp_sock_recvfrom,
    .shutdown   = udp_sock_shutdown,
    .close      = udp_sock_close,
    .poll       = udp_sock_poll,
    .setsockopt = udp_sock_setsockopt,
    .getsockopt = udp_sock_getsockopt,
};

/* ══════════════════════════════════════════════════════════════════════════
 *  RAW ICMP protocol operations
 * ══════════════════════════════════════════════════════════════════════════ */

static int raw_icmp_sock_bind(socket_t *sk, const struct sockaddr *addr,
                              socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    memcpy(&sk->local_addr, addr, sizeof(struct sockaddr_in));
    sk->bound = 1;
    return 0;
}

static int raw_icmp_sock_connect(socket_t *sk, const struct sockaddr *addr,
                                 socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    memcpy(&sk->remote_addr, addr, sizeof(struct sockaddr_in));
    sk->connected = 1;
    return 0;
}

static ssize_t raw_icmp_sock_sendto(socket_t *sk, const void *buf, size_t len,
                                    int flags, const struct sockaddr *dest_addr,
                                    socklen_t addrlen)
{
    (void)flags;
    struct sockaddr_in dst;

    if (dest_addr && addrlen >= sizeof(struct sockaddr_in)) {
        memcpy(&dst, dest_addr, sizeof(dst));
    } else if (sk->connected) {
        dst = sk->remote_addr;
    } else {
        return -1;
    }

    uint32_t dst_ip = dst.sin_addr.s_addr;
    uint32_t nh;
    netdev_t *dev = ip_route(dst_ip, &nh);
    if (!dev) return -1;

    skbuff_t *skb = skb_alloc(len + 64);
    if (!skb) return -1;
    skb_reserve(skb, 64);
    memcpy(skb_put(skb, len), buf, len);
    skb->protocol = IPPROTO_ICMP;

    int rc = ip_tx(dev, skb, dev->ip_addr, dst_ip, IPPROTO_ICMP);
    return rc < 0 ? -1 : (ssize_t)len;
}

static ssize_t raw_icmp_sock_recvfrom(socket_t *sk, void *buf, size_t len,
                                      int flags, struct sockaddr *src_addr,
                                      socklen_t *addrlen)
{
    (void)flags;
    while (skb_queue_empty(&sk->rx_queue)) {
        if (socket_has_unmasked_signal_pending()) return -2; /* EINTR */
        if (sk->nonblock) return -1;
        waitq_wait(&sk->wq_rx);
        if (socket_has_unmasked_signal_pending()) return -2; /* EINTR */
    }

    skbuff_t *skb = skb_queue_pop(&sk->rx_queue);
    if (!skb) return -1;

    size_t copy = (len < skb->len) ? len : skb->len;
    memcpy(buf, skb->data, copy);

    if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = skb->src_ip;
        *addrlen = sizeof(struct sockaddr_in);
    }

    skb_free(skb);
    return (ssize_t)copy;
}

static int raw_icmp_sock_shutdown(socket_t *sk, int how) {
    (void)sk; (void)how;
    return 0;
}

static int raw_icmp_sock_close(socket_t *sk) {
    raw_icmp_unregister(sk);
    return 0;
}

static int raw_icmp_sock_poll(socket_t *sk, int events) {
    int ready = 0;
    if ((events & POLLIN) && !skb_queue_empty(&sk->rx_queue))
        ready |= POLLIN;
    if (events & POLLOUT)
        ready |= POLLOUT;
    return ready;
}

static int raw_icmp_sock_setsockopt(socket_t *sk, int level, int optname,
                                    const void *optval, socklen_t optlen) {
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int raw_icmp_sock_getsockopt(socket_t *sk, int level, int optname,
                                    void *optval, socklen_t *optlen) {
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    return -1;
}

static socket_proto_ops_t raw_icmp_proto_ops = {
    .connect    = raw_icmp_sock_connect,
    .bind       = raw_icmp_sock_bind,
    .listen     = NULL,
    .accept     = NULL,
    .sendto     = raw_icmp_sock_sendto,
    .recvfrom   = raw_icmp_sock_recvfrom,
    .shutdown   = raw_icmp_sock_shutdown,
    .close      = raw_icmp_sock_close,
    .poll       = raw_icmp_sock_poll,
    .setsockopt = raw_icmp_sock_setsockopt,
    .getsockopt = raw_icmp_sock_getsockopt,
};
