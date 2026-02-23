#pragma once
#include <stdint.h>

/* ── Message types ───────────────────────────────────────────────────────── */
#define IPC_MSG_DATA    0   /* generic data payload                           */
#define IPC_MSG_PING    1   /* ping — expect a PONG reply                     */
#define IPC_MSG_PONG    2   /* pong reply                                     */
#define IPC_MSG_EXIT    3   /* request receiver to exit                       */

/* ── Message struct ──────────────────────────────────────────────────────── */
#define IPC_MSG_DATA_WORDS  4   /* 4 × uint64_t = 32 bytes of payload         */

typedef struct {
    uint32_t from_tid;                  /* TID of the sender                  */
    uint32_t type;                      /* IPC_MSG_* constant                 */
    uint64_t data[IPC_MSG_DATA_WORDS];  /* payload                            */
} ipc_msg_t;

/* ── Mailbox capacity ────────────────────────────────────────────────────── */
#define IPC_QUEUE_CAP  16

/* Opaque mailbox type; defined in ipc.c */
struct ipc_mailbox;
struct task;

/* ── Lifecycle (called from task.c) ─────────────────────────────────────── */
struct ipc_mailbox *ipc_mailbox_create(struct task *owner);
void                ipc_mailbox_destroy(struct ipc_mailbox *mb);

/* ── Messaging ───────────────────────────────────────────────────────────── */

/* Send `msg` to the task with TID `dest_tid`.
 * Wakes the receiver if it is blocked in ipc_recv.
 * Returns  0: message enqueued.
 * Returns -1: task not found, mailbox full, or task is dead. */
int ipc_send(uint32_t dest_tid, const ipc_msg_t *msg);

/* Receive one message into `out`.
 * Blocks the current task if the mailbox is empty.
 * Returns  0: message received in *out.
 * Returns -1: interrupted by a signal (caller should re-check exit condition). */
int ipc_recv(ipc_msg_t *out);

/* Non-blocking receive.
 * Returns  0: message received in *out.
 * Returns -1: mailbox was empty. */
int ipc_try_recv(ipc_msg_t *out);
