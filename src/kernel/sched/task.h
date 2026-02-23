#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mm/pmm.h"        /* for PAGE_SIZE   */
#include "ipc/signal.h"    /* for sig_handler_t */

#define TASK_STACK_SIZE   (PAGE_SIZE * 4)   /* 16 KiB default stack */
#define TASK_NAME_MAX     32

typedef enum {
    TASK_RUNNABLE  = 0,
    TASK_RUNNING   = 1,
    TASK_BLOCKED   = 2,
    TASK_DEAD      = 3,
} task_state_t;

typedef void (*task_entry_t)(void *arg);

struct ipc_mailbox;   /* defined in ipc/ipc.c */

typedef struct task {
    uint64_t      rsp;              /* saved stack pointer (top of regs frame)  */
    uint64_t      cr3;              /* page table root (kernel addr space share) */
    task_state_t  state;
    uint32_t      tid;              /* task ID                                  */
    uint32_t      cpu_id;           /* CPU this task is pinned/running on       */
    char          name[TASK_NAME_MAX];
    struct task  *next;             /* run-queue linked list                    */
    uintptr_t     stack_phys;       /* physical base of kernel stack            */

    /* ── IPC + Signals ──────────────────────────────────────────────────── */
    volatile uint32_t    sig_pending;   /* bitmask: bit N set = signal N pending */
    sig_handler_t       *sig_handlers;  /* [NSIGS] handler table; NULL = all DFL  */
    struct ipc_mailbox  *mailbox;       /* receive message queue                   */
} task_t;

/* Create a new task; returns NULL on error */
task_t *task_create(const char *name, task_entry_t entry, void *arg,
                    uint32_t cpu_id);

/* Free a task (must be DEAD) */
void task_destroy(task_t *t);

/* Look up a task by TID (returns NULL if not found or dead) */
task_t *task_lookup(uint32_t tid);
