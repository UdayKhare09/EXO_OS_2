#include "task.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "ipc/signal.h"
#include "ipc/ipc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "arch/x86_64/cpu.h"
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

static uint32_t next_tid = 1;

task_t *task_create(const char *name, task_entry_t entry, void *arg,
                    uint32_t cpu_id) {
    /* Allocate task_t structure in kernel heap (use PMM for now) */
    uintptr_t task_page = pmm_alloc_pages(1);
    if (!task_page) return NULL;
    task_t *t = (task_t *)vmm_phys_to_virt(task_page);
    memset(t, 0, sizeof(*t));

    /* Allocate kernel stack */
    uintptr_t stack_phys = pmm_alloc_pages(TASK_STACK_SIZE / PAGE_SIZE);
    if (!stack_phys) { pmm_free_pages(task_page, 1); return NULL; }
    t->stack_phys = stack_phys;
    uintptr_t stack_virt_top = vmm_phys_to_virt(stack_phys) + TASK_STACK_SIZE;

    /* FPU/SSE/AVX state buffer — must be 64-byte aligned for XSAVE/XRSTOR.
     * pmm_alloc_pages returns 4096-byte aligned addresses (>= 64 bytes). */
    uintptr_t fpu_phys = pmm_alloc_pages(1);
    if (!fpu_phys) {
        pmm_free_pages(stack_phys, TASK_STACK_SIZE / PAGE_SIZE);
        pmm_free_pages(task_page, 1);
        return NULL;
    }
    t->fpu_state = (uint8_t *)vmm_phys_to_virt(fpu_phys);
    memset(t->fpu_state, 0, PAGE_SIZE);
    /* Zeroed XSTATE_BV (offset 512) means XRSTOR uses initial FPU state:
     * FCW=0x037F, MXCSR=0x1F80, all ST/XMM/YMM regs zeroed. */

    /* Push initial frame onto the stack */
    stack_virt_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)stack_virt_top;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)task_trampoline;

    /* Push entry and arg above the frame so trampoline can pop them */
    /* We abuse rbx/rbp to pass entry and arg: */
    frame->rbx = (uint64_t)entry;   /* entry function */
    frame->r12 = (uint64_t)arg;     /* argument       */

    t->rsp    = stack_virt_top;
    t->cr3    = read_cr3();
    t->state  = TASK_RUNNABLE;
    t->tid    = next_tid++;
    t->cpu_id = cpu_id;
    t->next   = NULL;

    /* Priority scheduler fields */
    t->priority        = 4;   /* default: middle priority (0-7) */
    t->timeslice_ticks = 0;

    /* IPC + signal resources */
    t->sig_pending  = 0;
    t->sig_handlers = signal_table_alloc();
    t->mailbox      = ipc_mailbox_create(t);

    strncpy(t->name, name ? name : "unnamed", TASK_NAME_MAX - 1);
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    task_register(t);

    KLOG_DEBUG("task: created '%s' tid=%u cpu=%u rsp=%p\n",
               t->name, t->tid, t->cpu_id, (void *)t->rsp);
    return t;
}

void task_destroy(task_t *t) {
    if (!t) return;
    task_unregister(t);
    fd_close_all(t);
    signal_table_free(t->sig_handlers);  t->sig_handlers = NULL;
    ipc_mailbox_destroy(t->mailbox);     t->mailbox      = NULL;
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
