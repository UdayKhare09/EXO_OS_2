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
    TASK_SLEEPING  = 4,   /* blocked until sleep_deadline tick */
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

    /* ── Scheduling (MLFQ) ──────────────────────────────────────────────── */
    uint8_t       priority;         /* 0=highest, 7=lowest                       */
    uint32_t      timeslice_ticks;  /* ticks used in current timeslice           */

    /* ── Sleep ──────────────────────────────────────────────────────────── */
    uint64_t      sleep_deadline;   /* wake when g_jiffies >= this value         */
    struct task  *sleep_next;       /* sleep-list linked list                    */

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

/* Total number of task slots available */
#define TASK_TABLE_SIZE 1024

/* Access task table for enumeration (read-only) */
task_t *task_get_from_table(uint32_t index);
