/* ipc/pipe.c — Pipe implementation for EXO_OS
 *
 * Pipes are unidirectional byte streams with a kernel-side ring buffer.
 * Each pipe creates two file descriptors: fd[0] for reading, fd[1] for writing.
 * Integrates with the fd layer via file_ops_t.
 */
#include "ipc/signal.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "sched/waitq.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include <stdint.h>
#include <stddef.h>

#define PIPE_BUF_SIZE  4096   /* size of the kernel ring buffer */

typedef struct pipe {
    uint8_t    buf[PIPE_BUF_SIZE];
    uint32_t   read_pos;     /* next byte to read  */
    uint32_t   write_pos;    /* next byte to write */
    uint32_t   count;        /* bytes in buffer    */
    uint32_t   readers;      /* number of open read ends  */
    uint32_t   writers;      /* number of open write ends */
    waitq_t    read_wq;      /* readers wait here         */
    waitq_t    write_wq;     /* writers wait here          */
    volatile int lock;
} pipe_t;

static void pipe_lock(pipe_t *p)   { while (__atomic_test_and_set(&p->lock, __ATOMIC_ACQUIRE)) __asm__("pause"); }
static void pipe_unlock(pipe_t *p) { __atomic_clear(&p->lock, __ATOMIC_RELEASE); }

/* ── file_ops for read end ──────────────────────────────────────────────── */

static ssize_t pipe_read_op(file_t *f, void *buf, size_t count) {
    pipe_t *p = (pipe_t *)f->private_data;
    if (!p) return -EIO;

    uint8_t *dst = (uint8_t *)buf;
    size_t total = 0;

    while (total == 0) {
        pipe_lock(p);
        if (p->count > 0) {
            size_t avail = p->count;
            if (avail > count) avail = count;
            for (size_t i = 0; i < avail; i++) {
                dst[total++] = p->buf[p->read_pos];
                p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
                p->count--;
            }
            pipe_unlock(p);
            waitq_wake_one(&p->write_wq);  /* wake blocked writers */
            break;
        }
        /* Buffer empty */
        if (p->writers == 0) {
            pipe_unlock(p);
            return 0; /* EOF — no more writers */
        }
        pipe_unlock(p);
        /* Block until data arrives */
        waitq_wait(&p->read_wq);
    }

    return (ssize_t)total;
}

static ssize_t pipe_write_op(file_t *f, const void *buf, size_t count) {
    pipe_t *p = (pipe_t *)f->private_data;
    if (!p) return -EIO;

    /* If no readers, raise SIGPIPE */
    pipe_lock(p);
    if (p->readers == 0) {
        pipe_unlock(p);
        task_t *cur = sched_current();
        if (cur) signal_send(cur, SIGPIPE);
        return -EPIPE;
    }
    pipe_unlock(p);

    const uint8_t *src = (const uint8_t *)buf;
    size_t total = 0;

    while (total < count) {
        pipe_lock(p);
        if (p->readers == 0) {
            pipe_unlock(p);
            if (total > 0) return (ssize_t)total;
            return -EPIPE;
        }
        while (total < count && p->count < PIPE_BUF_SIZE) {
            p->buf[p->write_pos] = src[total++];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        pipe_unlock(p);
        waitq_wake_one(&p->read_wq);  /* wake blocked readers */
        if (total < count) {
            waitq_wait(&p->write_wq);  /* wait for space */
        }
    }

    return (ssize_t)total;
}

/* ── file_ops close for read end ─────────────────────────────────────────── */
static int pipe_close_read(file_t *f) {
    pipe_t *p = (pipe_t *)f->private_data;
    if (!p) return 0;
    pipe_lock(p);
    p->readers--;
    bool should_free = (p->readers == 0 && p->writers == 0);
    pipe_unlock(p);
    waitq_wake_all(&p->write_wq);  /* wake writers so they get EPIPE */
    if (should_free)
        kfree(p);
    return 0;
}

/* ── file_ops close for write end ────────────────────────────────────────── */
static int pipe_close_write(file_t *f) {
    pipe_t *p = (pipe_t *)f->private_data;
    if (!p) return 0;
    pipe_lock(p);
    p->writers--;
    bool should_free = (p->readers == 0 && p->writers == 0);
    pipe_unlock(p);
    waitq_wake_all(&p->read_wq);   /* wake readers so they get EOF */
    if (should_free)
        kfree(p);
    return 0;
}

static file_ops_t pipe_read_ops = {
    .read  = pipe_read_op,
    .write = NULL,           /* can't write to read end */
    .close = pipe_close_read,
    .poll  = NULL,
    .ioctl = NULL,
};

static file_ops_t pipe_write_ops = {
    .read  = NULL,           /* can't read from write end */
    .write = pipe_write_op,
    .close = pipe_close_write,
    .poll  = NULL,
    .ioctl = NULL,
};

/* ── sys_pipe2 ──────────────────────────────────────────────────────────── */
int64_t sys_pipe2(int pipefd[2], int flags) {
    if (!pipefd) return -EFAULT;

    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    pipe_t *p = kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(*p));
    p->readers = 1;
    p->writers = 1;
    waitq_init(&p->read_wq);
    waitq_init(&p->write_wq);

    /* Create file objects for read and write ends */
    file_t *rf = file_alloc_generic(&pipe_read_ops, p, O_RDONLY);
    if (!rf) { kfree(p); return -ENOMEM; }

    file_t *wf = file_alloc_generic(&pipe_write_ops, p, O_WRONLY);
    if (!wf) { file_put(rf); kfree(p); return -ENOMEM; }

    int rfd = fd_alloc(cur, rf);
    if (rfd < 0) { file_put(rf); file_put(wf); kfree(p); return -EMFILE; }

    int wfd = fd_alloc(cur, wf);
    if (wfd < 0) { fd_close(cur, rfd); file_put(wf); kfree(p); return -EMFILE; }

    if (flags & 0x80000) {  /* O_CLOEXEC */
        rf->fd_flags |= FD_CLOEXEC;
        wf->fd_flags |= FD_CLOEXEC;
    }

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    KLOG_DEBUG("pipe2: created pipe rd=%d wr=%d\n", rfd, wfd);
    return 0;
}
