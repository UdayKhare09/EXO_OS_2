/* net/unix.c — AF_UNIX (POSIX local) socket implementation
 *
 * Supports SOCK_STREAM and SOCK_DGRAM / SOCK_SEQPACKET over abstract-namespace
 * and filesystem-path addresses.  Backed by ring buffers for data transfer.
 *
 * Connection model (SOCK_STREAM):
 *   connect() → finds listener, enqueues a server-side peer, wakes accept()
 *   accept()  → dequeues pending peer, returns new fd
 *   send/recv  → write/read through peer's ring buffer
 */
#include "net/unix.h"
#include "net/socket.h"
#include "net/socket_defs.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include "sched/cred.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/waitq.h"
#include "lib/spinlock.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include <stdint.h>
#include <stddef.h>

/* ── Ring buffer ─────────────────────────────────────────────────────────── */
#define UNIX_BUFSIZE    (32 * 1024)

typedef struct {
    uint8_t  data[UNIX_BUFSIZE];
    uint32_t head, tail;       /* head = read ptr, tail = write ptr */
    spinlock_t lock;
} unix_ring_t;

static inline uint32_t ring_used(const unix_ring_t *r) {
    return (r->tail - r->head) & (UNIX_BUFSIZE - 1);
}
static inline uint32_t ring_free(const unix_ring_t *r) {
    return (UNIX_BUFSIZE - 1) - ring_used(r);
}
static inline int ring_empty(const unix_ring_t *r) { return r->head == r->tail; }

static uint32_t ring_write(unix_ring_t *r, const uint8_t *src, uint32_t len) {
    uint32_t avail = ring_free(r);
    if (len > avail) len = avail;
    for (uint32_t i = 0; i < len; i++)
        r->data[(r->tail + i) & (UNIX_BUFSIZE - 1)] = src[i];
    r->tail = (r->tail + len) & (UNIX_BUFSIZE - 1);
    return len;
}

static uint32_t ring_read(unix_ring_t *r, uint8_t *dst, uint32_t len) {
    uint32_t avail = ring_used(r);
    if (len > avail) len = avail;
    for (uint32_t i = 0; i < len; i++)
        dst[i] = r->data[(r->head + i) & (UNIX_BUFSIZE - 1)];
    r->head = (r->head + len) & (UNIX_BUFSIZE - 1);
    return len;
}

/* ── unix_sock state machine ─────────────────────────────────────────────── */
typedef enum {
    US_INITIAL = 0,
    US_BOUND,
    US_LISTENING,
    US_CONNECTED,
    US_CLOSING,
} unix_state_t;

#define UNIX_ACCEPT_MAX 16

typedef struct unix_sock {
    unix_state_t  state;
    int           type;        /* SOCK_STREAM / SOCK_DGRAM */

    /* Path (sun_path) */
    char          path[108];
    int           path_len;
    int           abstract;    /* 1 if path[0] == '\0' (abstract namespace) */

    /* Data ring (incoming data for this socket) */
    unix_ring_t   rx;

    /* Peer (for connected sockets) */
    struct unix_sock *peer;

    /* Wait queues */
    waitq_t       wq_rx;       /* wake when data arrives in rx */
    waitq_t       wq_accept;   /* wake when a connection arrives */

    /* Accept queue (server-side pending connections) */
    struct unix_sock *accept_q[UNIX_ACCEPT_MAX];
    int               aq_head, aq_tail;  /* circular */
    spinlock_t        aq_lock;

    /* Non-blocking flag (from socket flags) */
    int           nonblock;

    /* shutdown state */
    int           shut_rd, shut_wr;

    /* Peer credentials (filled at connect/socketpair time) */
    uint32_t      peer_pid, peer_uid, peer_gid;

    /* SCM_RIGHTS: pending file_t* queue (pushed by sender, popped by receiver) */
    file_t       *fd_queue[16];
    int           fd_qhead, fd_qtail;
    spinlock_t    fd_qlock;
} unix_sock_t;

/* ── Global bound-socket registry ───────────────────────────────────────── */
#define UNIX_BIND_MAX 64

static struct {
    char         path[108];
    int          path_len;
    unix_sock_t *sock;
} g_unix_bound[UNIX_BIND_MAX];

static spinlock_t g_unix_bind_lock;

static int unix_register(unix_sock_t *us) {
    spinlock_acquire(&g_unix_bind_lock);
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        if (!g_unix_bound[i].sock) {
            g_unix_bound[i].sock = us;
            memcpy(g_unix_bound[i].path, us->path, us->path_len);
            g_unix_bound[i].path_len = us->path_len;
            spinlock_release(&g_unix_bind_lock);
            return 0;
        }
    }
    spinlock_release(&g_unix_bind_lock);
    return -ENOMEM;
}

static void unix_unregister(unix_sock_t *us) {
    spinlock_acquire(&g_unix_bind_lock);
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        if (g_unix_bound[i].sock == us) {
            g_unix_bound[i].sock = NULL;
            g_unix_bound[i].path_len = 0;
        }
    }
    spinlock_release(&g_unix_bind_lock);
}

static unix_sock_t *unix_find_listener(const char *path, int path_len) {
    spinlock_acquire(&g_unix_bind_lock);
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        unix_sock_t *us = g_unix_bound[i].sock;
        if (!us) continue;
        if (us->state != US_LISTENING) continue;
        if (g_unix_bound[i].path_len == path_len &&
            memcmp(g_unix_bound[i].path, path, path_len) == 0) {
            spinlock_release(&g_unix_bind_lock);
            return us;
        }
    }
    spinlock_release(&g_unix_bind_lock);
    return NULL;
}

static int unix_task_in_group(const task_t *t, uint32_t gid) {
    if (!t) return 0;
    if (t->cred.gid == gid || t->cred.egid == gid || t->cred.fsgid == gid) return 1;
    uint32_t n = t->cred.group_count;
    if (n > TASK_MAX_GROUPS) n = TASK_MAX_GROUPS;
    for (uint32_t i = 0; i < n; i++)
        if (t->cred.groups[i] == gid) return 1;
    return 0;
}

static int unix_check_path_write_perm(const char *path, int path_len) {
    if (!path || path_len <= 0) return -EINVAL;
    if (path[0] == '\0') return 0; /* abstract namespace */

    char kpath[109];
    int n = path_len;
    if (n > 108) n = 108;
    memcpy(kpath, path, (size_t)n);
    while (n > 0 && kpath[n - 1] == '\0') n--;
    kpath[n] = '\0';
    if (!kpath[0]) return -EINVAL;

    int err = 0;
    vnode_t *v = vfs_lookup(kpath, true, &err);
    if (!v) return err ? err : -ENOENT;

    task_t *cur = sched_current();
    if (!cur) {
        vfs_vnode_put(v);
        return -ESRCH;
    }

    if (capable(&cur->cred, CAP_DAC_OVERRIDE)) {
        vfs_vnode_put(v);
        return 0;
    }

    uint32_t perm;
    if (cur->cred.fsuid == v->uid) perm = (v->mode >> 6) & 7;
    else if (unix_task_in_group(cur, v->gid)) perm = (v->mode >> 3) & 7;
    else perm = v->mode & 7;

    vfs_vnode_put(v);
    return (perm & 2) ? 0 : -EACCES;
}

/* ── Proto ops ───────────────────────────────────────────────────────────── */
static int unix_bind(socket_t *sk, const struct sockaddr *addr, socklen_t addrlen) {
    if (!addr || addrlen < 3) return -EINVAL;
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (us->state != US_INITIAL) return -EINVAL;

    struct sockaddr_un { uint16_t family; char path[108]; };
    const struct sockaddr_un *sun = (const struct sockaddr_un *)addr;
    int path_len = (int)addrlen - 2;  /* subtract sa_family */
    if (path_len <= 0 || path_len > 107) return -EINVAL;

    memcpy(us->path, sun->path, path_len);
    us->path_len = path_len;
    us->abstract = (sun->path[0] == '\0') ? 1 : 0;
    us->state    = US_BOUND;

    int r = unix_register(us);
    if (r < 0) { us->state = US_INITIAL; return r; }
    return 0;
}

static int unix_listen(socket_t *sk, int backlog) {
    (void)backlog;
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (us->state != US_BOUND) return -EINVAL;
    us->state = US_LISTENING;
    return 0;
}

static int unix_connect(socket_t *sk, const struct sockaddr *addr, socklen_t addrlen) {
    if (!addr || addrlen < 3) return -EINVAL;
    unix_sock_t *uc = (unix_sock_t *)sk->proto_data;
    if (uc->state == US_CONNECTED) return -EISCONN;

    struct sockaddr_un { uint16_t family; char path[108]; };
    const struct sockaddr_un *sun = (const struct sockaddr_un *)addr;
    int path_len = (int)addrlen - 2;
    if (path_len <= 0 || path_len > 107) return -EINVAL;

    int pchk = unix_check_path_write_perm(sun->path, path_len);
    if (pchk < 0) return pchk;

    unix_sock_t *listener = unix_find_listener(sun->path, path_len);
    if (!listener) return -ECONNREFUSED;

    /* Check accept queue room */
    spinlock_acquire(&listener->aq_lock);
    int next = (listener->aq_tail + 1) % UNIX_ACCEPT_MAX;
    if (next == listener->aq_head) {
        spinlock_release(&listener->aq_lock);
        return -ECONNREFUSED; /* backlog full */
    }

    /* Create the server-side half of the connection */
    unix_sock_t *us = kmalloc(sizeof(unix_sock_t));
    if (!us) { spinlock_release(&listener->aq_lock); return -ENOMEM; }
    memset(us, 0, sizeof(*us));
    us->type  = uc->type;
    us->state = US_CONNECTED;
    waitq_init(&us->wq_rx);
    waitq_init(&us->wq_accept);
    spinlock_init(&us->aq_lock);
    spinlock_init(&us->rx.lock);

    /* Link the pair */
    uc->peer  = us;
    us->peer  = uc;
    uc->state = US_CONNECTED;

    /* Record peer credentials at connect time */
    task_t *cur = sched_current();
    if (cur) {
        uc->peer_pid = cur->pid; uc->peer_uid = cur->cred.euid; uc->peer_gid = cur->cred.egid;
        us->peer_pid = cur->pid; us->peer_uid = cur->cred.euid; us->peer_gid = cur->cred.egid;
    }

    /* Enqueue server half on listener's accept queue */
    listener->accept_q[listener->aq_tail] = us;
    listener->aq_tail = next;
    spinlock_release(&listener->aq_lock);

    waitq_wake_one(&listener->wq_accept);
    return 0;
}

static int unix_accept(socket_t *sk, struct sockaddr *addr, socklen_t *addrlen) {
    unix_sock_t *listener = (unix_sock_t *)sk->proto_data;
    if (listener->state != US_LISTENING) return -EINVAL;

    /* Wait for a connection */
    while (1) {
        spinlock_acquire(&listener->aq_lock);
        if (listener->aq_head != listener->aq_tail) break;
        spinlock_release(&listener->aq_lock);
        if (listener->nonblock || sk->nonblock) return -EAGAIN;
        waitq_wait(&listener->wq_accept);
    }
    unix_sock_t *us = listener->accept_q[listener->aq_head];
    listener->aq_head = (listener->aq_head + 1) % UNIX_ACCEPT_MAX;
    spinlock_release(&listener->aq_lock);

    /* Optionally return the peer address */
    if (addr && addrlen && us->peer && us->peer->path_len > 0) {
        struct sockaddr_un { uint16_t family; char path[108]; } sun;
        sun.family = 1; /* AF_UNIX */
        memcpy(sun.path, us->peer->path, us->peer->path_len);
        int olen = (int)*addrlen;
        int copy = us->peer->path_len + 2;
        if (copy > olen) copy = olen;
        memcpy(addr, &sun, copy);
        *addrlen = (socklen_t)(us->peer->path_len + 2);
    }

    /* Wrap server-side unix_sock in a new socket_t + file_t */
    socket_t *new_sk = kmalloc(sizeof(socket_t));
    if (!new_sk) { us->state = US_CLOSING; return -ENOMEM; }
    memset(new_sk, 0, sizeof(socket_t));
    new_sk->domain     = AF_UNIX;
    new_sk->type       = sk->type;
    new_sk->proto_data = us;
    /* Re-use the same proto_ops (they live in static storage, no issue) */
    extern socket_proto_ops_t g_unix_proto_ops;
    new_sk->proto_ops  = &g_unix_proto_ops;
    waitq_init(&new_sk->wq_rx);
    waitq_init(&new_sk->wq_tx);
    skb_queue_init(&new_sk->rx_queue);

    extern file_ops_t g_socket_file_ops;
    file_t *nf = file_alloc_generic(&g_socket_file_ops, new_sk, 2 /* O_RDWR */);
    if (!nf) { kfree(new_sk); us->state = US_CLOSING; return -ENOMEM; }
    task_t *t = sched_current();
    int nfd = fd_alloc(t, nf);
    if (nfd < 0) { kfree(new_sk); us->state = US_CLOSING; return -EMFILE; }
    return nfd;
}

static ssize_t unix_sendto(socket_t *sk, const void *buf, size_t len, int flags,
                           const struct sockaddr *dest, socklen_t addrlen) {
    (void)flags; (void)dest; (void)addrlen;
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (us->state != US_CONNECTED || !us->peer) return -ENOTCONN;
    if (us->shut_wr) return -EPIPE;
    unix_sock_t *peer = us->peer;
    if (peer->state == US_CLOSING || peer->shut_rd) return -EPIPE;

    uint32_t written = 0;
    while (written < len) {
        spinlock_acquire(&peer->rx.lock);
        uint32_t n = ring_write(&peer->rx, (const uint8_t *)buf + written,
                                (uint32_t)(len - written));
        spinlock_release(&peer->rx.lock);
        if (n) {
            written += n;
            waitq_wake_one(&peer->wq_rx);
        } else {
            if (sk->nonblock) break;
            waitq_wait(&peer->wq_rx);  /* wait for drain */
        }
    }
    return (ssize_t)written ? (ssize_t)written : -EAGAIN;
}

static ssize_t unix_recvfrom(socket_t *sk, void *buf, size_t len, int flags,
                              struct sockaddr *src, socklen_t *addrlen) {
    (void)flags; (void)src; (void)addrlen;
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (us->state != US_CONNECTED && us->state != US_LISTENING) return -ENOTCONN;
    if (us->shut_rd) return 0;

    while (1) {
        spinlock_acquire(&us->rx.lock);
        uint32_t n = ring_read(&us->rx, (uint8_t *)buf, (uint32_t)len);
        spinlock_release(&us->rx.lock);
        if (n) return (ssize_t)n;
        /* If peer disconnected and buf empty, return EOF */
        if (!us->peer || us->peer->state == US_CLOSING) return 0;
        if (sk->nonblock || us->nonblock) return -EAGAIN;
        waitq_wait(&us->wq_rx);
    }
}

static int unix_shutdown(socket_t *sk, int how) {
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (how == 0 || how == 2) us->shut_rd = 1;
    if (how == 1 || how == 2) us->shut_wr = 1;
    if (us->peer) waitq_wake_all(&us->peer->wq_rx);
    return 0;
}

static int unix_close(socket_t *sk) {
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (!us) return 0;
    us->state = US_CLOSING;
    /* Disconnect peer */
    if (us->peer) {
        us->peer->peer = NULL;
        waitq_wake_all(&us->peer->wq_rx);
        us->peer = NULL;
    }
    unix_unregister(us);
    waitq_wake_all(&us->wq_rx);
    waitq_wake_all(&us->wq_accept);
    kfree(us);
    sk->proto_data = NULL;
    return 0;
}

static int unix_poll(socket_t *sk, int events) {
    unix_sock_t *us = (unix_sock_t *)sk->proto_data;
    if (!us) return POLLERR;
    int mask = 0;
    if (events & POLLIN) {
        if (us->state == US_LISTENING) {
            /* readable if accept queue non-empty */
            spinlock_acquire(&us->aq_lock);
            if (us->aq_head != us->aq_tail) mask |= POLLIN;
            spinlock_release(&us->aq_lock);
        } else {
            spinlock_acquire(&us->rx.lock);
            if (!ring_empty(&us->rx)) mask |= POLLIN;
            spinlock_release(&us->rx.lock);
            /* EOF */
            if (!us->peer || us->peer->state == US_CLOSING) mask |= POLLIN | POLLHUP;
        }
    }
    if (events & POLLOUT) {
        if (us->state == US_CONNECTED && us->peer && !us->shut_wr)
            mask |= POLLOUT;
    }
    return mask;
}

static int unix_setsockopt(socket_t *sk, int level, int optname,
                           const void *optval, socklen_t optlen) {
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

/* ── SCM_RIGHTS: push/pop file_t references ────────────────────────────── */
void unix_push_files(void *dest_v, file_t **files, int count) {
    unix_sock_t *dest = (unix_sock_t *)dest_v;
    spinlock_acquire(&dest->fd_qlock);
    for (int i = 0; i < count; i++) {
        int next = (dest->fd_qtail + 1) & 15;
        if (next == dest->fd_qhead) break; /* full — drop */
        files[i]->refcount++;              /* hold a reference */
        dest->fd_queue[dest->fd_qtail] = files[i];
        dest->fd_qtail = next;
    }
    spinlock_release(&dest->fd_qlock);
}

int unix_pop_files(void *src_v, task_t *receiver, int *out_fds, int maxcount) {
    unix_sock_t *src = (unix_sock_t *)src_v;
    int n = 0;
    spinlock_acquire(&src->fd_qlock);
    while (n < maxcount && src->fd_qhead != src->fd_qtail) {
        file_t *f = src->fd_queue[src->fd_qhead];
        src->fd_qhead = (src->fd_qhead + 1) & 15;
        spinlock_release(&src->fd_qlock);
        int newfd = fd_alloc(receiver, f); /* f's refcount already held by push */
        out_fds[n++] = (newfd >= 0) ? newfd : -1;
        spinlock_acquire(&src->fd_qlock);
    }
    spinlock_release(&src->fd_qlock);
    return n;
}

/* ── SO_PEERCRED via getsockopt ─────────────────────────────────────────── */
static int unix_getsockopt(socket_t *sk, int level, int optname,
                           void *optval, socklen_t *optlen) {
    (void)level;
    if (optname == 17 /* SO_PEERCRED */ && optval && optlen && *optlen >= 12) {
        unix_sock_t *us = (unix_sock_t *)sk->proto_data;
        uint32_t *cred = (uint32_t *)optval;
        cred[0] = us ? us->peer_pid : 0;
        cred[1] = us ? us->peer_uid : 0;
        cred[2] = us ? us->peer_gid : 0;
        *optlen = 12;
        return 0;
    }
    return 0;
}
/* ── Public proto_ops table ──────────────────────────────────────────────── */
socket_proto_ops_t g_unix_proto_ops = {
    .connect    = unix_connect,
    .bind       = unix_bind,
    .listen     = unix_listen,
    .accept     = unix_accept,
    .sendto     = unix_sendto,
    .recvfrom   = unix_recvfrom,
    .shutdown   = unix_shutdown,
    .close      = unix_close,
    .poll       = unix_poll,
    .setsockopt = unix_setsockopt,
    .getsockopt = unix_getsockopt,
};

/* ── Public entry points ─────────────────────────────────────────────────── */
void *unix_sock_from_socket(socket_t *sk) {
    return sk ? (void *)sk->proto_data : NULL;
}
void *unix_sock_get_peer(void *us_v) {
    unix_sock_t *us = (unix_sock_t *)us_v;
    return us ? (void *)us->peer : NULL;
}
void unix_socket_init(void) {
    spinlock_init(&g_unix_bind_lock);
    memset(g_unix_bound, 0, sizeof(g_unix_bound));
    KLOG_INFO("unix: AF_UNIX sockets initialised\n");
}

int unix_socket_create(socket_t *sk, int type) {
    (void)type;
    unix_sock_t *us = kmalloc(sizeof(unix_sock_t));
    if (!us) return -ENOMEM;
    memset(us, 0, sizeof(*us));
    us->type = type;
    us->state = US_INITIAL;
    waitq_init(&us->wq_rx);
    waitq_init(&us->wq_accept);
    spinlock_init(&us->aq_lock);
    spinlock_init(&us->rx.lock);
    sk->proto_data = us;
    sk->proto_ops  = &g_unix_proto_ops;
    return 0;
}

/* ── unix_socketpair: create a pre-connected pair ────────────────────────── */
int unix_socketpair(int type, int sv[2]) {
    if (!sv) return -EFAULT;
    int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

    unix_sock_t *ua = kmalloc(sizeof(unix_sock_t));
    unix_sock_t *ub = kmalloc(sizeof(unix_sock_t));
    if (!ua || !ub) { kfree(ua); kfree(ub); return -ENOMEM; }
    memset(ua, 0, sizeof(*ua));
    memset(ub, 0, sizeof(*ub));

    ua->type = base_type; ua->state = US_CONNECTED;
    waitq_init(&ua->wq_rx); waitq_init(&ua->wq_accept);
    spinlock_init(&ua->aq_lock); spinlock_init(&ua->rx.lock);
    spinlock_init(&ua->fd_qlock);

    ub->type = base_type; ub->state = US_CONNECTED;
    waitq_init(&ub->wq_rx); waitq_init(&ub->wq_accept);
    spinlock_init(&ub->aq_lock); spinlock_init(&ub->rx.lock);
    spinlock_init(&ub->fd_qlock);

    ua->peer = ub; ub->peer = ua;

    /* Record credentials for the pair */
    task_t *ct = sched_current();
    if (ct) {
        ua->peer_pid = ub->peer_pid = ct->pid;
        ua->peer_uid = ub->peer_uid = ct->cred.euid;
        ua->peer_gid = ub->peer_gid = ct->cred.egid;
    }

    socket_t *ska = kmalloc(sizeof(socket_t));
    socket_t *skb = kmalloc(sizeof(socket_t));
    if (!ska || !skb) {
        kfree(ska); kfree(skb); kfree(ua); kfree(ub);
        return -ENOMEM;
    }
    memset(ska, 0, sizeof(*ska)); memset(skb, 0, sizeof(*skb));
    ska->domain = AF_UNIX; ska->type = base_type;
    ska->proto_data = ua;  ska->proto_ops = &g_unix_proto_ops;
    ska->nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;
    waitq_init(&ska->wq_rx); waitq_init(&ska->wq_tx);
    skb_queue_init(&ska->rx_queue);

    skb->domain = AF_UNIX; skb->type = base_type;
    skb->proto_data = ub;  skb->proto_ops = &g_unix_proto_ops;
    skb->nonblock = ska->nonblock;
    waitq_init(&skb->wq_rx); waitq_init(&skb->wq_tx);
    skb_queue_init(&skb->rx_queue);

    extern file_ops_t g_socket_file_ops;
    file_t *fa = file_alloc_generic(&g_socket_file_ops, ska, 2 /* O_RDWR */);
    file_t *fb = file_alloc_generic(&g_socket_file_ops, skb, 2 /* O_RDWR */);
    if (!fa || !fb) {
        kfree(ska); kfree(skb); kfree(ua); kfree(ub);
        return -ENOMEM;
    }

    task_t *t = sched_current();
    int fda = fd_alloc(t, fa);
    if (fda < 0) { kfree(ska); kfree(skb); kfree(ua); kfree(ub); return -EMFILE; }
    int fdb = fd_alloc(t, fb);
    if (fdb < 0) {
        fd_close(t, fda);
        kfree(skb); kfree(ub);
        return -EMFILE;
    }
    if (type & SOCK_CLOEXEC) {
        t->fd_flags[fda] |= FD_CLOEXEC;
        t->fd_flags[fdb] |= FD_CLOEXEC;
    }
    sv[0] = fda; sv[1] = fdb;
    return 0;
}
