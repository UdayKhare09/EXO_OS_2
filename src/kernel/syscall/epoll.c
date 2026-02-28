/* syscall/epoll.c — epoll(7) implementation
 *
 * epoll_create1() → allocates an epoll instance backed by a file descriptor.
 * epoll_ctl()     → add/mod/del interest entries (fd + events + 64-bit data).
 * epoll_wait()    → poll all interest entries; block (with timeout) until ready.
 *
 * Internally uses the same fd->f_ops->poll() mechanism as sys_poll(2).
 */
#include "syscall/epoll.h"
#include "syscall.h"
#include "fs/fd.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "mm/kmalloc.h"
#include <stdint.h>
#include <stddef.h>

/* ── Interest-list entry ─────────────────────────────────────────────────── */
typedef struct epoll_entry {
    int      fd;
    uint32_t events;   /* interested events (EPOLLIN | EPOLLOUT | ...) */
    uint64_t data;     /* opaque epoll_data returned to user */
    int      oneshot;  /* EPOLLONESHOT: remove after first trigger */
    struct epoll_entry *next;
} epoll_entry_t;

/* ── Epoll instance ──────────────────────────────────────────────────────── */
typedef struct {
    epoll_entry_t *list;   /* interest list (singly-linked) */
    int            count;  /* number of entries */
} epoll_inst_t;

static void epoll_inst_free(epoll_inst_t *ep) {
    epoll_entry_t *e = ep->list;
    while (e) { epoll_entry_t *n = e->next; kfree(e); e = n; }
    kfree(ep);
}

/* ── file_ops for epoll file descriptors ─────────────────────────────────── */
static ssize_t epf_read(file_t *f, void *buf, size_t len) {
    (void)f; (void)buf; (void)len; return -EINVAL;
}
static ssize_t epf_write(file_t *f, const void *buf, size_t len) {
    (void)f; (void)buf; (void)len; return -EINVAL;
}
static int epf_close(file_t *f) {
    if (f && f->private_data) { epoll_inst_free((epoll_inst_t *)f->private_data); f->private_data=NULL; }
    return 0;
}
static int epf_poll(file_t *f, int events) {
    (void)f; (void)events; return 0;
}
static file_ops_t g_epoll_file_ops = {
    .read  = epf_read,
    .write = epf_write,
    .close = epf_close,
    .poll  = epf_poll,
    .ioctl = NULL,
};

/* ── epoll_create1(2) ────────────────────────────────────────────────────── */
int64_t sys_epoll_create1(int flags) {
    (void)flags;
    epoll_inst_t *ep = kmalloc(sizeof(epoll_inst_t));
    if (!ep) return -ENOMEM;
    ep->list  = NULL;
    ep->count = 0;

    file_t *f = file_alloc_generic(&g_epoll_file_ops, ep, 2 /* O_RDWR */);
    if (!f) { kfree(ep); return -ENOMEM; }

    task_t *t = sched_current();
    int fd = fd_alloc(t, f);
    if (fd < 0) { epoll_inst_free(ep); return -EMFILE; }
    return (int64_t)fd;
}

/* ── epoll_ctl(2) ────────────────────────────────────────────────────────── */
int64_t sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *event) {
    task_t *t = sched_current();
    file_t *ef = fd_get(t, epfd);
    if (!ef || ef->f_ops != &g_epoll_file_ops) return -EBADF;
    epoll_inst_t *ep = (epoll_inst_t *)ef->private_data;
    if (!ep) return -EBADF;

    /* Validate target fd */
    file_t *tf = fd_get(t, fd);
    if (!tf) return -EBADF;
    if (fd == epfd) return -EINVAL;

    if (op == EPOLL_CTL_ADD) {
        if (!event) return -EINVAL;
        /* Check duplicate */
        for (epoll_entry_t *e = ep->list; e; e = e->next)
            if (e->fd == fd) return -EEXIST;
        epoll_entry_t *ne = kmalloc(sizeof(epoll_entry_t));
        if (!ne) return -ENOMEM;
        ne->fd      = fd;
        ne->events  = event->events;
        ne->data    = event->data;
        ne->oneshot = (event->events & EPOLLONESHOT) ? 1 : 0;
        ne->next    = ep->list;
        ep->list    = ne;
        ep->count++;
        return 0;
    }
    if (op == EPOLL_CTL_MOD) {
        if (!event) return -EINVAL;
        for (epoll_entry_t *e = ep->list; e; e = e->next) {
            if (e->fd == fd) {
                e->events  = event->events;
                e->data    = event->data;
                e->oneshot = (event->events & EPOLLONESHOT) ? 1 : 0;
                return 0;
            }
        }
        return -ENOENT;
    }
    if (op == EPOLL_CTL_DEL) {
        epoll_entry_t **pp = &ep->list;
        while (*pp) {
            if ((*pp)->fd == fd) {
                epoll_entry_t *del = *pp;
                *pp = del->next;
                kfree(del);
                ep->count--;
                return 0;
            }
            pp = &(*pp)->next;
        }
        return -ENOENT;
    }
    return -EINVAL;
}

/* ── Internal: poll one fd using same logic as sys_poll ─────────────────── */
static int epoll_poll_fd(task_t *t, int fd, uint32_t events) {
    file_t *f = fd_get(t, fd);
    if (!f) return EPOLLERR | EPOLLHUP;
    if (f->f_ops && f->f_ops->poll)
        return f->f_ops->poll(f, (int)events) & (int)events;
    /* VFS-backed regular files are always ready for read+write */
    if (f->vnode) {
        int mask = 0;
        if (events & EPOLLIN)  mask |= EPOLLIN;
        if (events & EPOLLOUT) mask |= EPOLLOUT;
        return mask;
    }
    return 0;
}

/* ── epoll_wait(2) ───────────────────────────────────────────────────────── */
int64_t sys_epoll_wait(int epfd, epoll_event_t *events, int maxevents, int timeout) {
    if (!events || maxevents <= 0) return -EINVAL;
    task_t *t = sched_current();
    file_t *ef = fd_get(t, epfd);
    if (!ef || ef->f_ops != &g_epoll_file_ops) return -EBADF;
    epoll_inst_t *ep = (epoll_inst_t *)ef->private_data;
    if (!ep) return -EBADF;

    uint64_t deadline = 0;
    if (timeout > 0)
        deadline = sched_get_ticks() + (uint64_t)timeout;

    for (;;) {
        int nready = 0;
        epoll_entry_t *prev = NULL;
        for (epoll_entry_t *e = ep->list; e && nready < maxevents; prev = e, e = e->next) {
            int rev = epoll_poll_fd(t, e->fd, e->events);
            if (!rev) continue;
            events[nready].events = (uint32_t)rev;
            events[nready].data   = e->data;
            nready++;
            if (e->oneshot) {
                /* Remove from list */
                if (prev) prev->next = e->next; else ep->list = e->next;
                kfree(e);
                ep->count--;
                e = prev ? prev : ep->list;
                if (!e) break;
            }
        }
        if (nready > 0) return (int64_t)nready;
        if (timeout == 0) return 0;
        if (timeout > 0 && sched_get_ticks() >= deadline) return 0;
        sched_sleep(1);
    }
}

/* ── epoll_pwait(2) ──────────────────────────────────────────────────────── */
int64_t sys_epoll_pwait(int epfd, epoll_event_t *events, int maxevents, int timeout,
                        const uint64_t *sigmask, uint64_t sigsetsize) {
    (void)sigmask; (void)sigsetsize;
    return sys_epoll_wait(epfd, events, maxevents, timeout);
}
