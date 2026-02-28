#include "task.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/pagecache.h"
#include "mm/kmalloc.h"
#include "ipc/signal.h"
#include "ipc/ipc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "fs/fd.h"   /* fd_close_all */
#include <stdint.h>
#include <stddef.h>

/* ── Task ID registry ────────────────────────────────────────────────────── */
static task_t *task_table[TASK_TABLE_SIZE];

task_t *task_lookup(uint32_t tid) {
    if (tid == 0 || tid >= TASK_TABLE_SIZE) return NULL;
    return __atomic_load_n(&task_table[tid], __ATOMIC_ACQUIRE);
}

static void task_register(task_t *t) {
    if (t->tid < TASK_TABLE_SIZE)
        __atomic_store_n(&task_table[t->tid], t, __ATOMIC_RELEASE);
}

static void task_unregister(task_t *t) {
    if (t->tid < TASK_TABLE_SIZE)
        __atomic_store_n(&task_table[t->tid], (task_t *)NULL, __ATOMIC_RELEASE);
}

/* Initial register frame pushed onto a new task's stack.
 * task_switch_asm will pop these when first switching to this task. */
typedef struct {
    uint64_t r15, r14, r13, r12, rbx, rbp;  /* callee-saved   */
    uint64_t rip;                            /* task entry addr */
} __attribute__((packed)) init_frame_t;

/* Wrapper that calls entry(arg) then marks task dead */
extern void task_trampoline(void);   /* defined in context_switch.asm */

/* User-mode entry trampoline: sets up iretq frame to jump to ring 3 */
extern void user_mode_trampoline(void);  /* defined in context_switch.asm */

/* User tasks get TIDs 1..USER_TID_MAX; kernel threads get TIDs
 * (TASK_TABLE_SIZE-1) counting down to KTHREAD_TID_MIN.  Both ranges
 * fit inside task_table[] so task_lookup() works for either. */
#define USER_TID_MAX    511u
#define KTHREAD_TID_MIN 512u
static uint32_t next_user_tid    = 1;
static uint32_t next_kthread_tid = TASK_TABLE_SIZE - 1;

/* PID 1 user process: set once, used for orphan reparenting */
task_t *g_init_task = NULL;

static void task_release_shared_mappings(task_t *t) {
    if (!t || !t->cr3) return;

    for (vma_t *v = t->vma_list; v; v = v->next) {
        if (!(v->flags & VMA_FILE) || !(v->flags & VMA_SHARED) || !v->file)
            continue;

        for (uint64_t a = v->start; a < v->end; a += PAGE_SIZE) {
            uint64_t *pte = vmm_get_pte(t->cr3, a);
            if (!pte || !(*pte & VMM_PRESENT))
                continue;

            uint64_t within = a - v->start;
            if (within >= v->file_size)
                continue;

            pagecache_release(v->file, v->file_offset + within);
        }
    }
}

/* Shared init for both kernel and user tasks.
 * is_kthread=1 => allocate from the high TID pool (hidden from /proc).
 * is_kthread=0 => allocate from the low user TID pool. */
static task_t *task_alloc_common(const char *name, uint32_t cpu_id, int is_kthread) {
    uintptr_t task_page = pmm_alloc_pages(1);
    if (!task_page) return NULL;
    task_t *t = (task_t *)vmm_phys_to_virt(task_page);
    memset(t, 0, sizeof(*t));

    /* Allocate kernel stack */
    uintptr_t stack_phys = pmm_alloc_pages(TASK_STACK_SIZE / PAGE_SIZE);
    if (!stack_phys) { pmm_free_pages(task_page, 1); return NULL; }
    t->stack_phys = stack_phys;

    /* FPU/SSE/AVX state buffer — must be 64-byte aligned for XSAVE/XRSTOR. */
    uintptr_t fpu_phys = pmm_alloc_pages(1);
    if (!fpu_phys) {
        pmm_free_pages(stack_phys, TASK_STACK_SIZE / PAGE_SIZE);
        pmm_free_pages(task_page, 1);
        return NULL;
    }
    t->fpu_state = (uint8_t *)vmm_phys_to_virt(fpu_phys);
    memset(t->fpu_state, 0, PAGE_SIZE);

    t->state  = TASK_RUNNABLE;
    if (is_kthread) {
        if (next_kthread_tid < KTHREAD_TID_MIN) next_kthread_tid = KTHREAD_TID_MIN;
        t->tid = next_kthread_tid--;
    } else {
        if (next_user_tid > USER_TID_MAX) next_user_tid = 1;
        t->tid = next_user_tid++;
    }
    t->pid    = t->tid;   /* default: PID == TID */
    t->cpu_id = cpu_id;
    t->next   = NULL;

    /* Process identity defaults */
    t->ppid   = 0;
    t->pgid   = t->pid;
    t->sid    = t->pid;
    t->uid    = 0;
    t->gid    = 0;
    t->umask  = 022;
    t->group_count = 1;
    t->groups[0] = t->gid;
    t->parent = NULL;
    t->children   = NULL;
    t->child_next = NULL;
    t->exit_status  = 0;
    t->is_user      = 0;
    t->is_kthread   = (uint8_t)(is_kthread ? 1 : 0);
    t->ctty_pty     = NULL;
    t->ctty_is_raw  = 0;

    /* Priority scheduler fields */
    t->priority        = 4;
    t->timeslice_ticks = 0;

    /* IPC + signal resources */
    t->sig_pending  = 0;
    t->sig_mask     = 0;
    t->sig_handlers = signal_table_alloc();
    t->sigactions   = NULL;   /* lazy-allocated on first rt_sigaction call */
    t->mailbox      = ipc_mailbox_create(t);

    /* Memory management */
    t->vma_list     = NULL;
    t->brk_base     = USER_HEAP_BASE;
    t->brk_current  = USER_HEAP_BASE;
    t->mmap_next    = USER_MMAP_BASE;
    t->owns_address_space = 0;
    t->fs_base      = 0;
    t->clear_child_tid = NULL;

    strncpy(t->name, name ? name : "unnamed", TASK_NAME_MAX - 1);
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->exe_path[0] = '\0';   /* set by execve; empty for kernel threads (linux: /proc/pid/exe unresolvable) */
    task_register(t);

    return t;
}

task_t *task_create(const char *name, task_entry_t entry, void *arg,
                    uint32_t cpu_id) {
    task_t *t = task_alloc_common(name, cpu_id, /*is_kthread=*/1);
    if (!t) return NULL;

    uintptr_t stack_virt_top = vmm_phys_to_virt(t->stack_phys) + TASK_STACK_SIZE;

    /* Push initial frame onto the stack */
    stack_virt_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)stack_virt_top;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)task_trampoline;
    frame->rbx = (uint64_t)entry;   /* entry function */
    frame->r12 = (uint64_t)arg;     /* argument       */

    t->rsp    = stack_virt_top;
    t->cr3    = read_cr3();
    t->is_user = 0;
    t->owns_address_space = 0;

    KLOG_DEBUG("task: created kernel '%s' tid=%u cpu=%u rsp=%p\n",
               t->name, t->tid, t->cpu_id, (void *)t->rsp);
    return t;
}

task_t *task_create_user(const char *name, uintptr_t pml4_phys,
                         uintptr_t user_entry, uintptr_t user_stack_top,
                         uint32_t cpu_id) {
    task_t *t = task_alloc_common(name, cpu_id, /*is_kthread=*/0);
    if (!t) return NULL;

    uintptr_t stack_virt_top = vmm_phys_to_virt(t->stack_phys) + TASK_STACK_SIZE;

    /* Push initial frame: trampoline will use rbx=user_entry, r12=user_stack */
    stack_virt_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)stack_virt_top;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)user_mode_trampoline;
    frame->rbx = user_entry;        /* user RIP */
    frame->r12 = user_stack_top;    /* user RSP */

    t->rsp     = stack_virt_top;
    t->cr3     = pml4_phys;
    t->is_user    = 1;
    t->is_kthread = 0;
    t->owns_address_space = 1;

    /* First user process ever created is init (PID 1). */
    if (t->pid == 1 && g_init_task == NULL)
        g_init_task = t;

    KLOG_DEBUG("task: created user '%s' tid=%u cpu=%u entry=%p ustack=%p\n",
               t->name, t->tid, t->cpu_id,
               (void *)user_entry, (void *)user_stack_top);
    return t;
}

void task_destroy(task_t *t) {
    if (!t) return;
    task_unregister(t);
    fd_close_all(t);
    signal_table_free(t->sig_handlers);  t->sig_handlers = NULL;
    sigaction_table_free(t->sigactions); t->sigactions   = NULL;
    ipc_mailbox_destroy(t->mailbox);     t->mailbox      = NULL;

    if (t->env_block) { kfree(t->env_block); t->env_block = NULL; }

    /* Free VMAs */
    vma_t *v = t->vma_list;
    while (v) {
        vma_t *next = v->next;
        if (v->file)
            vfs_vnode_put(v->file);
        kfree(v);
        v = next;
    }
    t->vma_list = NULL;

    /* Destroy user address space (if process had its own) */
    if (t->is_user && t->owns_address_space && t->cr3 != vmm_get_kernel_pml4()) {
        task_release_shared_mappings(t);
        vmm_destroy_address_space(t->cr3);
    }

    if (t->fpu_state) {
        pmm_free_pages(vmm_virt_to_phys((uintptr_t)t->fpu_state), 1);
        t->fpu_state = NULL;
    }
    pmm_free_pages(t->stack_phys, TASK_STACK_SIZE / PAGE_SIZE);
    uintptr_t task_phys = vmm_virt_to_phys((uintptr_t)t);
    pmm_free_pages(task_phys, 1);
}

task_t *task_get_from_table(uint32_t index) {
    if (index >= TASK_TABLE_SIZE) return NULL;
    return __atomic_load_n(&task_table[index], __ATOMIC_ACQUIRE);
}
