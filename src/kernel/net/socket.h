/* net/socket.h — BSD socket abstraction over fd layer
 *
 * Each socket_t owns a file_t (via file_alloc_generic) with file_ops_t
 * pointing to socket_read/write/close/poll/ioctl.  The socket_t lives
 * in file->private_data.
 *
 * For TCP sockets, socket_t.proto_data points to a tcp_tcb_t.
 * For UDP sockets, it points to a udp_sock_t (internal to socket.c).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net/socket_defs.h"
#include "net/skbuff.h"
#include "sched/waitq.h"
#include "fs/fd.h"

/* ── Protocol operations (TCP / UDP) ─────────────────────────────────────── */
struct socket;
typedef struct socket_proto_ops {
    int      (*connect)(struct socket *sk, const struct sockaddr *addr,
                        socklen_t addrlen);
    int      (*bind)(struct socket *sk, const struct sockaddr *addr,
                     socklen_t addrlen);
    int      (*listen)(struct socket *sk, int backlog);
    int      (*accept)(struct socket *sk, struct sockaddr *addr,
                       socklen_t *addrlen);
    ssize_t  (*sendto)(struct socket *sk, const void *buf, size_t len,
                       int flags, const struct sockaddr *dest_addr,
                       socklen_t addrlen);
    ssize_t  (*recvfrom)(struct socket *sk, void *buf, size_t len,
                         int flags, struct sockaddr *src_addr,
                         socklen_t *addrlen);
    int      (*shutdown)(struct socket *sk, int how);
    int      (*close)(struct socket *sk);
    int      (*poll)(struct socket *sk, int events);
    int      (*setsockopt)(struct socket *sk, int level, int optname,
                           const void *optval, socklen_t optlen);
    int      (*getsockopt)(struct socket *sk, int level, int optname,
                           void *optval, socklen_t *optlen);
} socket_proto_ops_t;

/* ── Socket object ───────────────────────────────────────────────────────── */
#define SOCK_RXQUEUE_MAX 128

typedef struct socket {
    int                 domain;     /* AF_INET */
    int                 type;       /* SOCK_STREAM / SOCK_DGRAM */
    int                 protocol;   /* IPPROTO_TCP / IPPROTO_UDP */

    socket_proto_ops_t *proto_ops;  /* TCP or UDP ops */
    void               *proto_data; /* tcp_tcb_t* or udp_sock internal */

    /* local + remote address */
    struct sockaddr_in  local_addr;
    struct sockaddr_in  remote_addr;
    int                 bound;
    int                 connected;

    /* non-blocking flag */
    int                 nonblock;

    /* RX packet queue (for UDP — datagrams queued here) */
    skb_queue_t         rx_queue;

    /* wait queues (some overlap with TCP TCB waitqs) */
    waitq_t             wq_rx;
    waitq_t             wq_tx;

    /* error */
    int                 so_error;
} socket_t;

/* ── Create/destroy ──────────────────────────────────────────────────────── */
/* Returns fd on success, negative on error */
int  socket_create(int domain, int type, int protocol);

/* Lookup socket_t from fd (current task) — returns NULL if fd is not a socket */
socket_t *socket_from_fd(int fd);

/* ── file_ops vtable for socket fds ──────────────────────────────────────── */
extern file_ops_t g_socket_file_ops;

/* ── init ────────────────────────────────────────────────────────────────── */
void socket_init(void);
