/* drivers/pty.h — Pseudo-Terminal (PTY) master/slave pair
 *
 * Linux model: /dev/ptmx opens a PTY master; grantpt/unlockpt/ptsname give
 * you the slave path /dev/pts/<N>.  The master and slave are connected
 * bidirectionally: writes to master appear as reads on slave and vice-versa.
 *
 * In-kernel dataflow (matches Linux ldisc TTYDISC):
 *   user-space process      PTY slave           PTY master       SSH/terminal
 *   write(slave, "hello")   → output queue  →  master reads  →  display
 *   master writes keystroke → input queue   →  slave reads   →  process stdin
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "fs/fd.h"

#define PTY_MAX        64      /* max simultaneously open PTY pairs */
#define PTY_BUF_SIZE   4096    /* ring-buffer size per direction    */

typedef struct pty_ring {
    uint8_t  buf[PTY_BUF_SIZE];
    uint32_t head;   /* next read position  */
    uint32_t tail;   /* next write position */
} pty_ring_t;

typedef struct pty_pair {
    int        index;           /* PTY number (0‥PTY_MAX-1)         */
    int        open;            /* 1 = pair is active                */
    pty_ring_t m2s;             /* master→slave (keyboard input)     */
    pty_ring_t s2m;             /* slave→master (program output)     */

    /* TTY line-discipline state (per-pair, like Linux struct tty_struct) */
    uint32_t   c_iflag;
    uint32_t   c_oflag;
    uint32_t   c_cflag;
    uint32_t   c_lflag;
    uint8_t    c_cc[19];        /* NCCS = 19                         */
    uint32_t   c_ispeed;
    uint32_t   c_ospeed;

    uint16_t   ws_row;
    uint16_t   ws_col;
    uint16_t   ws_xpixel;
    uint16_t   ws_ypixel;

    int        fg_pgid;         /* foreground process group          */
    uint32_t   owner_uid;       /* owner for /dev/pts/<N>            */
    uint32_t   owner_gid;       /* group for /dev/pts/<N>            */

    int        master_open;     /* ref count masters (0 or 1)        */
    int        slave_open;      /* ref count slaves  (0+)            */
} pty_pair_t;

/* Initialise the PTY subsystem (called from devpts_init) */
void pty_init(void);

/* Allocate a new PTY pair; returns index ≥ 0 or -1 on failure */
int pty_alloc(void);

/* Release a PTY pair (called when both ends are closed) */
void pty_free(int index);

/* Get a PTY pair (or NULL) */
pty_pair_t *pty_get(int index);

/* Ring-buffer helpers */
static inline int pty_ring_empty(const pty_ring_t *r) {
    return r->head == r->tail;
}

static inline int pty_ring_full(const pty_ring_t *r) {
    return ((r->tail + 1) % PTY_BUF_SIZE) == r->head;
}

static inline int pty_ring_write(pty_ring_t *r, uint8_t c) {
    if (pty_ring_full(r)) return -1;
    r->buf[r->tail] = c;
    r->tail = (r->tail + 1) % PTY_BUF_SIZE;
    return 0;
}

static inline int pty_ring_read(pty_ring_t *r, uint8_t *c) {
    if (pty_ring_empty(r)) return -1;
    *c = r->buf[r->head];
    r->head = (r->head + 1) % PTY_BUF_SIZE;
    return 0;
}

/* file_ops for the master side (installed when /dev/ptmx is opened) */
extern file_ops_t g_pty_master_fops;

/* file_ops for the slave side (installed when /dev/pts/N is opened) */
extern file_ops_t g_pty_slave_fops;

/* lflag bits (subset; must match values in pty.c) */
#define LFLAG_ISIG   0x00000001U  /* deliver SIGINT/SIGQUIT on ctrl chars */

/* Return the c_lflag of the active foreground PTY.
 * Falls back to LFLAG_ISIG when no PTY has an fg process group. */
uint32_t pty_fg_lflag(void);
