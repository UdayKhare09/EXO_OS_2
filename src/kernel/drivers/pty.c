/* drivers/pty.c — Pseudo-Terminal master/slave implementation
 *
 * Follows the Linux pty driver model:
 *   /dev/ptmx                → open gives master fd; TIOCGPTN reads slave #
 *   /dev/pts/<N>             → slave end; mounted as devpts
 *
 * Line discipline (ldisc):
 *   - Canonical mode  (ICANON set): master writes accumulate until \n;
 *     slave read returns a whole line.
 *   - Raw mode (ICANON clear): every byte is passed through immediately.
 *   - ICRNL: \r → \n on slave input
 *   - OPOST|ONLCR: \n → \r\n on slave output (program→master)
 *   - ECHO: echo slave input back to master output
 *
 * SIGWINCH: TIOCSWINSZ on master delivers SIGWINCH to slave's fg pgrp.
 */
#include "drivers/pty.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "ipc/signal.h"
#include "fs/fd.h"
#include <stdint.h>
#include <stddef.h>

/* ioctl numbers (Linux ABI) */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCSCTTY   0x540E
#define TIOCNOTTY   0x5422
#define TIOCGPTN    0x80045430U  /* get PTY slave number (uint) */
#define TIOCSPTLCK  0x40045431U  /* lock/unlock PTY             */
#define TIOCPTYGNAME 0x40805453U /* macOS compat — ignore       */

#define OFLAG_OPOST 0x00000001U
#define OFLAG_ONLCR 0x00000004U
#define LFLAG_ECHO  0x00000008U
#define LFLAG_ICANON 0x00000002U
#define IFLAG_ICRNL 0x00000100U

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} pty_termios_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} pty_winsize_t;

/* ── Global PTY table ────────────────────────────────────────────────────── */
static pty_pair_t g_pty_table[PTY_MAX];

void pty_init(void) {
    memset(g_pty_table, 0, sizeof(g_pty_table));
    KLOG_INFO("[pty] initialized %d slots\n", PTY_MAX);
}

int pty_alloc(void) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_pty_table[i].open) {
            pty_pair_t *p = &g_pty_table[i];
            memset(p, 0, sizeof(*p));
            p->index  = i;
            p->open   = 1;
            /* Linux defaults: ECHO | ICANON | ICRNL | OPOST | ONLCR */
            p->c_iflag  = 0x00000500U; /* ICRNL | IXON */
            p->c_oflag  = 0x00000005U; /* OPOST | ONLCR */
            p->c_cflag  = 0x000000BFU;
            p->c_lflag  = 0x00008A3BU; /* ECHO | ICANON | ISIG | ... */
            static const uint8_t cc_def[19] = {
                3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0 };
            memcpy(p->c_cc, cc_def, 19);
            p->c_ispeed = 38400;
            p->c_ospeed = 38400;
            p->ws_row   = 24;
            p->ws_col   = 80;
            p->ws_xpixel = 0;
            p->ws_ypixel = 0;
            p->fg_pgid  = 1;
            task_t *cur = sched_current();
            p->owner_uid = cur ? cur->cred.fsuid : 0;
            p->owner_gid = 5; /* tty group */
            p->master_open = 1;
            p->slave_open  = 0;
            return i;
        }
    }
    return -1;
}

void pty_free(int index) {
    if (index < 0 || index >= PTY_MAX) return;
    g_pty_table[index].open = 0;
}

pty_pair_t *pty_get(int index) {
    if (index < 0 || index >= PTY_MAX) return NULL;
    if (!g_pty_table[index].open)     return NULL;
    return &g_pty_table[index];
}

/* ── Helper: send SIGWINCH to fg pgrp of a pty pair ─────────────────────── */
static void pty_signal_winch(pty_pair_t *p) {
    if (!p || p->fg_pgid <= 0) return;
    for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t) continue;
        if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE) continue;
        if ((int)t->pgid == p->fg_pgid)
            signal_send(t, 28); /* SIGWINCH */
    }
}

/* ── Ldisc: write bytes from master to slave input queue ─────────────────── */
/* This is what a terminal emulator / SSH daemon does: feed keystrokes */
static void ldisc_master_to_slave(pty_pair_t *p, uint8_t c) {
    bool icrnl  = (p->c_iflag & IFLAG_ICRNL) != 0;
    bool echo   = (p->c_lflag & LFLAG_ECHO)  != 0;

    if (icrnl && c == '\r') c = '\n';

    /* echo back to master output (slave → master direction) */
    if (echo) {
        pty_ring_write(&p->s2m, c);
        if (c == '\n') pty_ring_write(&p->s2m, '\r');
    }

    pty_ring_write(&p->m2s, c);
}

/* ── Ldisc: write bytes from slave (program) to master output ─────────────── */
static void ldisc_slave_to_master(pty_pair_t *p, uint8_t c) {
    bool opost = (p->c_oflag & OFLAG_OPOST) != 0;
    bool onlcr = (p->c_oflag & OFLAG_ONLCR) != 0;

    if (opost && onlcr && c == '\n') {
        pty_ring_write(&p->s2m, '\r');
    }
    pty_ring_write(&p->s2m, c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Master file_ops
 * ═══════════════════════════════════════════════════════════════════════════ */

static ssize_t pty_master_read(file_t *f, void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return -EIO;

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;

    while (done < count) {
        uint8_t c;
        if (pty_ring_read(&p->s2m, &c) == 0) {
            dst[done++] = c;
        } else {
            if (done > 0) break;
            if (f->flags & O_NONBLOCK) return -EAGAIN;
            sched_sleep(1);
        }
    }
    return (ssize_t)done;
}

static ssize_t pty_master_write(file_t *f, const void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return -EIO;

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        ldisc_master_to_slave(p, src[i]);
    }
    return (ssize_t)count;
}

static int pty_master_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return -EIO;

    if (cmd == TIOCGPTN) {
        unsigned int *ptn = (unsigned int *)(uintptr_t)arg;
        if (!ptn) return -EINVAL;
        *ptn = (unsigned int)p->index;
        return 0;
    }
    if (cmd == TIOCSPTLCK) {
        /* lock/unlock: we don't implement locking, just succeed */
        return 0;
    }
    if (cmd == TIOCGWINSZ) {
        pty_winsize_t *ws = (pty_winsize_t *)(uintptr_t)arg;
        if (!ws) return -EINVAL;
        ws->ws_row    = p->ws_row;
        ws->ws_col    = p->ws_col;
        ws->ws_xpixel = p->ws_xpixel;
        ws->ws_ypixel = p->ws_ypixel;
        return 0;
    }
    if (cmd == TIOCSWINSZ) {
        const pty_winsize_t *ws = (const pty_winsize_t *)(uintptr_t)arg;
        if (!ws) return -EINVAL;
        p->ws_row    = ws->ws_row;
        p->ws_col    = ws->ws_col;
        p->ws_xpixel = ws->ws_xpixel;
        p->ws_ypixel = ws->ws_ypixel;
        pty_signal_winch(p);
        return 0;
    }
    if (cmd == TCGETS) {
        pty_termios_t *ts = (pty_termios_t *)(uintptr_t)arg;
        if (!ts) return -EINVAL;
        ts->c_iflag  = p->c_iflag;
        ts->c_oflag  = p->c_oflag;
        ts->c_cflag  = p->c_cflag;
        ts->c_lflag  = p->c_lflag;
        ts->c_line   = 0;
        memcpy(ts->c_cc, p->c_cc, 19);
        ts->c_ispeed = p->c_ispeed;
        ts->c_ospeed = p->c_ospeed;
        return 0;
    }
    if (cmd == TCSETS || cmd == TCSETSW || cmd == TCSETSF) {
        const pty_termios_t *ts = (const pty_termios_t *)(uintptr_t)arg;
        if (!ts) return -EINVAL;
        p->c_iflag  = ts->c_iflag;
        p->c_oflag  = ts->c_oflag;
        p->c_cflag  = ts->c_cflag;
        p->c_lflag  = ts->c_lflag;
        memcpy(p->c_cc, ts->c_cc, 19);
        p->c_ispeed = ts->c_ispeed;
        p->c_ospeed = ts->c_ospeed;
        return 0;
    }
    if (cmd == TIOCGPGRP) {
        int *pgid = (int *)(uintptr_t)arg;
        if (!pgid) return -EINVAL;
        *pgid = p->fg_pgid;
        return 0;
    }
    if (cmd == TIOCSPGRP) {
        const int *pgid = (const int *)(uintptr_t)arg;
        if (!pgid || *pgid <= 0) return -EINVAL;
        p->fg_pgid = *pgid;
        return 0;
    }
    if (cmd == TIOCSCTTY) {
        task_t *cur = sched_current();
        if (cur) {
            p->fg_pgid = (int)cur->pgid;
            /* Assign controlling terminal to every task in caller's session */
            for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
                task_t *t = task_get_from_table(i);
                if (!t || t->is_kthread) continue;
                if (t->sid == cur->sid)
                    t->ctty_pty = (void *)p;
            }
        }
        return 0;
    }
    if (cmd == TIOCNOTTY) {
        task_t *cur = sched_current();
        if (cur) {
            /* Detach only this task from its controlling terminal */
            cur->ctty_pty    = NULL;
            cur->ctty_is_raw = 0;
        }
        return 0;
    }
    return -EINVAL;
}

static int pty_master_close(file_t *f) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return 0;
    p->master_open = 0;
    /* if slave is also closed, free the pair */
    if (p->slave_open == 0)
        pty_free(p->index);
    return 0;
}

static int pty_master_poll(file_t *f, int events) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return 0;
    int ready = 0;
    if ((events & 1) && !pty_ring_empty(&p->s2m)) ready |= 1; /* POLLIN */
    if ((events & 4) && !pty_ring_full(&p->m2s))  ready |= 4; /* POLLOUT */
    return ready;
}

file_ops_t g_pty_master_fops = {
    .read  = pty_master_read,
    .write = pty_master_write,
    .close = pty_master_close,
    .poll  = pty_master_poll,
    .ioctl = pty_master_ioctl,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Slave file_ops
 * ═══════════════════════════════════════════════════════════════════════════ */

static ssize_t pty_slave_read(file_t *f, void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return -EIO;

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    bool canonical = (p->c_lflag & LFLAG_ICANON) != 0;

    while (done < count) {
        uint8_t c;
        if (pty_ring_read(&p->m2s, &c) == 0) {
            dst[done++] = c;
            if (canonical && c == '\n') break;
        } else {
            if (done > 0 && !canonical) break;
            if (f->flags & O_NONBLOCK) {
                if (done > 0) break;
                return -EAGAIN;
            }
            sched_sleep(1);
        }
    }
    return (ssize_t)done;
}

static ssize_t pty_slave_write(file_t *f, const void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return -EIO;

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        ldisc_slave_to_master(p, src[i]);
    }
    return (ssize_t)count;
}

static int pty_slave_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    /* slave ioctl mirrors master — same termios/winsize state */
    return pty_master_ioctl(f, cmd, arg);
}

static int pty_slave_close(file_t *f) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return 0;
    if (p->slave_open > 0) p->slave_open--;
    /* send SIGHUP to fg pgrp when slave closes (like Linux does) */
    if (p->slave_open == 0) {
        pty_signal_winch(p); /* reuse helper for pgrp iteration */
        if (p->fg_pgid > 0) {
            for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
                task_t *t = task_get_from_table(i);
                if (!t) continue;
                if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE) continue;
                if ((int)t->pgid == p->fg_pgid)
                    signal_send(t, SIGHUP);
            }
        }
    }
    if (p->master_open == 0)
        pty_free(p->index);
    return 0;
}

static int pty_slave_poll(file_t *f, int events) {
    pty_pair_t *p = (pty_pair_t *)f->private_data;
    if (!p) return 0;
    int ready = 0;
    if ((events & 1) && !pty_ring_empty(&p->m2s)) ready |= 1;
    if ((events & 4) && !pty_ring_full(&p->s2m))  ready |= 4;
    return ready;
}

file_ops_t g_pty_slave_fops = {
    .read  = pty_slave_read,
    .write = pty_slave_write,
    .close = pty_slave_close,
    .poll  = pty_slave_poll,
    .ioctl = pty_slave_ioctl,
};
