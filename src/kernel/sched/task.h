#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mm/pmm.h"        /* for PAGE_SIZE   */
#include "ipc/signal.h"    /* for sig_handler_t */

/* Forward-declare file_t to avoid circular include (fs/fd.h includes sched/task.h) */
struct file;

#define TASK_STACK_SIZE   (PAGE_SIZE * 8)   /* 32 KiB default stack */
#define TASK_NAME_MAX     32
#define TASK_FD_TABLE_SIZE 256
#define TASK_CWD_MAX      512

typedef enum {
    TASK_RUNNABLE  = 0,
    TASK_RUNNING   = 1,
    TASK_BLOCKED   = 2,
    TASK_DEAD      = 3,
    TASK_SLEEPING  = 4,   /* blocked until sleep_deadline tick */
    TASK_ZOMBIE    = 5,   /* exited but parent hasn't waited  */
} task_state_t;

typedef void (*task_entry_t)(void *arg);

struct ipc_mailbox;   /* defined in ipc/ipc.c */

/* ── Virtual Memory Area (VMA) — tracks user-space mappings ──────────────── */
#define VMA_READ    (1 << 0)
#define VMA_WRITE   (1 << 1)
#define VMA_EXEC    (1 << 2)
#define VMA_USER    (1 << 3)
#define VMA_ANON    (1 << 4)   /* anonymous (not file-backed) */
#define VMA_STACK   (1 << 5)   /* stack region                */
#define VMA_HEAP    (1 << 6)   /* heap (brk) region           */

typedef struct vma {
    uint64_t    start;      /* page-aligned start address */
    uint64_t    end;        /* page-aligned end address (exclusive) */
    uint32_t    flags;      /* VMA_READ | VMA_WRITE | ... */
    struct vma *next;       /* sorted linked list */
} vma_t;

void vma_insert(struct task *t, vma_t *v);

typedef struct task {
    uint64_t      rsp;              /* saved stack pointer (top of regs frame)  */
    uint64_t      cr3;              /* page table root (physical addr of PML4)  */
    uint8_t      *fpu_state;        /* 4 KiB page-aligned XSAVE buffer          */
    task_state_t  state;
    uint32_t      tid;              /* task ID (also serves as PID for processes) */
    uint32_t      cpu_id;           /* CPU this task is pinned/running on       */
    char          name[TASK_NAME_MAX];
    struct task  *next;             /* run-queue linked list                    */
    uintptr_t     stack_phys;       /* physical base of kernel stack            */

    /* ── Process identity ────────────────────────────────────────────────── */
    uint32_t      pid;              /* process ID (== tid for main thread)      */
    uint32_t      ppid;             /* parent process ID                        */
    uint32_t      pgid;             /* process group ID                         */
    uint32_t      uid, gid;         /* user/group (always 0 = root for now)     */
    struct task  *parent;           /* parent task pointer                      */
    struct task  *children;         /* first child (linked via child_next)      */
    struct task  *child_next;       /* next sibling (in parent's children list) */
    int           exit_status;      /* set by sys_exit(), read by wait()        */
    uint8_t       is_user;          /* 1 = user-mode process, 0 = kernel task   */

    /* ── Scheduling (MLFQ) ──────────────────────────────────────────────── */
    uint8_t       priority;         /* 0=highest, 7=lowest                       */
    uint32_t      timeslice_ticks;  /* ticks used in current timeslice           */

    /* ── Sleep ──────────────────────────────────────────────────────────── */
    uint64_t      sleep_deadline;   /* wake when g_jiffies >= this value         */
    struct task  *sleep_next;       /* sleep-list linked list                    */

    /* ── IPC + Signals ──────────────────────────────────────────────────── */
    volatile uint32_t    sig_pending;   /* bitmask: bit N set = signal N pending */
    uint32_t             sig_mask;      /* signal mask (blocked signals)          */
    sig_handler_t       *sig_handlers;  /* [NSIGS] handler table; NULL = all DFL  */
    kernel_sigaction_t  *sigactions;    /* [NSIGS] rt_sigaction table             */
    struct ipc_mailbox  *mailbox;       /* receive message queue                   */

    /* ── Memory management ───────────────────────────────────────────────── */
    vma_t        *vma_list;         /* sorted list of user-space VMAs            */
    uint64_t      brk_base;         /* start of heap (set by ELF loader)         */
    uint64_t      brk_current;      /* current brk position                      */
    uint64_t      mmap_next;        /* next free address for mmap                */
    uint8_t       owns_address_space; /* 1 if task owns/destroys its CR3         */

    /* ── Thread support ──────────────────────────────────────────────────── */
    uint64_t      fs_base;          /* FS base (TLS pointer for user-space)      */
    uint64_t     *clear_child_tid;  /* set_tid_address: write 0 + futex on exit  */
    uint64_t      robust_list_head; /* set_robust_list head pointer (user VA)    */
    uint64_t      robust_list_len;  /* ABI size for robust list head             */

    /* ── Filesystem ──────────────────────────────────────────────────────── */
    struct file         *fd_table[TASK_FD_TABLE_SIZE];   /* open files        */
    char                 cwd[TASK_CWD_MAX];              /* current working dir */
} task_t;

/* Create a new kernel task; returns NULL on error */
task_t *task_create(const char *name, task_entry_t entry, void *arg,
                    uint32_t cpu_id);

/* Create a user-mode process with its own address space.
 * The process starts at user_entry with user_stack_top as RSP.
 * Returns the new task (added to scheduler), or NULL on error. */
task_t *task_create_user(const char *name, uintptr_t pml4_phys,
                         uintptr_t user_entry, uintptr_t user_stack_top,
                         uint32_t cpu_id);

/* Free a task (must be DEAD) */
void task_destroy(task_t *t);

/* Look up a task by TID (returns NULL if not found or dead) */
task_t *task_lookup(uint32_t tid);

/* Total number of task slots available */
#define TASK_TABLE_SIZE 1024

/* Access task table for enumeration (read-only) */
task_t *task_get_from_table(uint32_t index);
