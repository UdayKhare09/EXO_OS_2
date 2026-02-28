#include "sched.h"
#include "task.h"
#include "ipc/signal.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "mm/vmm.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "lib/timer.h"
#include <stdint.h>
#include <stddef.h>

/* Exposed for AP cores to read after calibration */
uint32_t g_apic_ticks_per_ms = 0;

/* Global jiffy counter: incremented 1/ms by BSP timer ISR (~1 ms resolution) */
static volatile uint64_t g_jiffies = 0;

static void sched_process_itimers(uint64_t now) {
    for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t) continue;
        if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE) continue;
        uint64_t deadline = __atomic_load_n(&t->itimer_real_deadline, __ATOMIC_RELAXED);
        if (!deadline || now < deadline) continue;

        signal_send(t, SIGALRM);

        uint64_t interval = __atomic_load_n(&t->itimer_real_interval, __ATOMIC_RELAXED);
        if (interval == 0) {
            __atomic_store_n(&t->itimer_real_deadline, 0, __ATOMIC_RELAXED);
        } else {
            uint64_t next = deadline;
            do { next += interval; } while (next <= now);
            __atomic_store_n(&t->itimer_real_deadline, next, __ATOMIC_RELAXED);
        }
    }
}

#define NPRIO  8    /* 0 = highest priority, 7 = lowest */
#define TIMESLICE_TICKS  10   /* ticks before priority drop */

/* ── Per-CPU scheduler state ─────────────────────────────────────────────── */
typedef struct {
    task_t  *current;         /* task currently running                     */
    task_t  *queue_head[NPRIO]; /* head of each priority queue              */
    task_t  *queue_tail[NPRIO]; /* tail of each priority queue              */
    task_t  *idle;            /* dedicated idle task                        */
    task_t  *sleep_head;      /* linked list of sleeping tasks (sleep_next) */
    uint32_t queue_len;       /* total tasks across all queues              */
    uint32_t cpu_id;
    volatile int rq_lock;     /* spinlock protecting all queues             */
} cpu_sched_t;

static cpu_sched_t cpu_scheds[MAX_CPUS];

/* Defined in context_switch.asm */
extern void task_switch_asm(uint64_t *old_rsp_ptr,
                            uint64_t  new_rsp,
                            uint64_t  new_cr3,
                            uint8_t  *old_fpu,
                            uint8_t  *new_fpu);

/* ── Run-queue lock helpers ──────────────────────────────────────────────── */

/* ISR context: interrupts already disabled; just spin on cross-CPU contention */
static inline void rq_lock(cpu_sched_t *cs) {
    while (__atomic_test_and_set(&cs->rq_lock, __ATOMIC_ACQUIRE));
}
static inline void rq_unlock(cpu_sched_t *cs) {
    __atomic_clear(&cs->rq_lock, __ATOMIC_RELEASE);
}

/* Task context: disable local interrupts first so the timer ISR on THIS CPU
 * cannot re-enter rq_lock while we hold it (same-CPU deadlock prevention). */
static inline uint64_t irq_save_cli(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory");
}

/* ── Run-queue helpers ────────────────────────────────────────────────────── */
static void rq_enqueue(cpu_sched_t *cs, task_t *t) {
    uint8_t pri = t->priority;
    if (pri >= NPRIO) pri = NPRIO - 1;

    t->next = NULL;
    if (!cs->queue_head[pri]) {
        cs->queue_head[pri] = cs->queue_tail[pri] = t;
    } else {
        cs->queue_tail[pri]->next = t;
        cs->queue_tail[pri]       = t;
    }
    cs->queue_len++;
}

/* Dequeue from highest priority non-empty queue */
static task_t *rq_dequeue(cpu_sched_t *cs) {
    for (int pri = 0; pri < NPRIO; pri++) {
        if (!cs->queue_head[pri]) continue;
        task_t *t = cs->queue_head[pri];
        cs->queue_head[pri] = t->next;
        if (!cs->queue_head[pri]) cs->queue_tail[pri] = NULL;
        t->next = NULL;
        cs->queue_len--;
        return t;
    }
    return NULL;
}

/* ── Idle task entry ──────────────────────────────────────────────────────── */
static void idle_entry(void *arg) {
    (void)arg;
    while (1) { cpu_sti(); cpu_halt(); }
}

/* ── Timer ISR handler (called on every CPU for vec APIC_TIMER_VEC) ───────── */
static void sched_timer_isr(cpu_regs_t *regs) {
    (void)regs;
    apic_send_eoi();
    /* Advance the shared ms clock ONLY from the hardware timer ISR.
     * sched_tick() is also called directly from sched_sleep / sched_task_exit;
     * if those calls also incremented g_jiffies the clock would run orders of
     * magnitude faster than 1 tick/ms, breaking all timing.                */
    cpu_info_t *ci = smp_self();
    if (ci && ci->id == 0) {
        uint64_t now = __atomic_add_fetch(&g_jiffies, 1, __ATOMIC_RELAXED);
        ktimer_tick(now);
        sched_process_itimers(now);
    }
    sched_tick();
}

/* ── sched_init ───────────────────────────────────────────────────────────── */
void sched_init(uint32_t cpu_id) {
    cpu_sched_t *cs = &cpu_scheds[cpu_id];
    memset(cs, 0, sizeof(*cs));
    cs->cpu_id = cpu_id;

    /* Create idle task for this CPU */
    cs->idle = task_create("idle", idle_entry, NULL, cpu_id);
    if (!cs->idle) kpanic("sched: cannot create idle task\n");
    cs->idle->state = TASK_RUNNABLE;

    /* Start with idle as current (no real task yet) */
    cs->current = cs->idle;

    /* Register timer ISR only once (all CPUs share the same IDT) */
    if (cpu_id == 0) {
        idt_register_handler(APIC_TIMER_VEC, sched_timer_isr);
    }

    KLOG_INFO("sched: CPU%u initialised\n", cpu_id);
}

/* ── sched_add_task ───────────────────────────────────────────────────────── */
void sched_add_task(task_t *t, uint32_t cpu_id) {
    cpu_sched_t *cs = &cpu_scheds[cpu_id];
    t->state = TASK_RUNNABLE;
    uint64_t f = irq_save_cli();
    rq_lock(cs);
    rq_enqueue(cs, t);
    rq_unlock(cs);
    irq_restore(f);
}

/* ── sched_tick: called from APIC timer ISR ──────────────────────────────── */
void sched_tick(void) {
    cpu_info_t  *ci = smp_self();
    if (!ci) return;
    uint32_t     cpu_id = ci->id;
    cpu_sched_t *cs     = &cpu_scheds[cpu_id];

    /* NOTE: g_jiffies is advanced ONLY by sched_timer_isr(), never here.
     * Direct callers (sched_sleep, sched_task_exit) must NOT modify it.   */
    uint64_t now = __atomic_load_n(&g_jiffies, __ATOMIC_RELAXED);

    task_t *prev = cs->current;

    /* MLFQ: increment timeslice for the running task (including idle so it
     * yields after 1 tick and gives queued workers a chance to run).       */
    if (prev && prev->state == TASK_RUNNING) {
        prev->timeslice_ticks++;
    }

    rq_lock(cs);

    /* Re-enqueue previous task if it has used its timeslice (or is idle).  */
    if (prev && prev->state == TASK_RUNNING) {
        /* Idle gets a 1-tick timeslice so it never starves queued workers.  */
        uint32_t max_ticks = (prev == cs->idle) ? 1 : TIMESLICE_TICKS;

        if (prev->timeslice_ticks >= max_ticks) {
            prev->state = TASK_RUNNABLE;
            prev->timeslice_ticks = 0;

            if (prev != cs->idle) {
                /* MLFQ feedback: used full timeslice → drop priority */
                if (prev->priority < NPRIO - 1) prev->priority++;
            }
            /* Idle goes to the lowest-priority queue so real tasks preempt it */
            if (prev == cs->idle)
                prev->priority = NPRIO - 1;

            rq_enqueue(cs, prev);
        }
    }

    /* Wake sleeping tasks whose deadline has passed */
    task_t **sp = &cs->sleep_head;
    while (*sp) {
        task_t *st = *sp;
        if (now >= st->sleep_deadline) {
            *sp = st->sleep_next;   /* unlink */
            st->sleep_next    = NULL;
            st->sleep_deadline = 0;
            st->state          = TASK_RUNNABLE;
            rq_enqueue(cs, st);
        } else {
            sp = &st->sleep_next;
        }
    }

    /* Pick next runnable task; collect dead tasks to free after unlock */
    task_t *next      = NULL;
    task_t *dead_list = NULL;
    uint32_t tried    = 0;
    while (tried < cs->queue_len + 1) {
        task_t *t = rq_dequeue(cs);
        if (!t) break;
        if (t->state == TASK_RUNNABLE) { next = t; break; }
        if (t->state == TASK_DEAD) {
            t->next   = dead_list;
            dead_list = t;
            tried++;  continue;
        }
        if (t->state == TASK_ZOMBIE) {
            /* Zombies stay in limbo until reaped by parent via wait4 */
            tried++;  continue;
        }
        rq_enqueue(cs, t);   /* skip blocked tasks */
        tried++;
    }

    /* If we are about to switch away from a task that is still running
     * (timeslice not yet expired), re-enqueue it so it is not lost.
     * Without this, the task stays TASK_RUNNING but is neither in any
     * queue nor cs->current — permanently dropped from scheduling.    */
    task_t *effective_next = next ? next : cs->idle;
    if (prev && prev->state == TASK_RUNNING && prev != effective_next) {
        prev->state = TASK_RUNNABLE;
        rq_enqueue(cs, prev);
    }

    rq_unlock(cs);

    /* Free dead tasks outside the lock (task_destroy acquires kmalloc_lock) */
    while (dead_list) {
        task_t *d = dead_list;
        dead_list = d->next;
        d->next = NULL;
        task_destroy(d);
    }

    if (!next) next = cs->idle;   /* fallback to idle */

    /* Fast-path SIGKILL: destroy the task before it runs */
    if (next != cs->idle &&
        (next->sig_pending & (1u << SIGKILL))) {
        task_destroy(next);
        next = cs->idle;
    }

    /* Reset timeslice counter when a task starts a fresh run */
    next->timeslice_ticks = 0;

    next->state = TASK_RUNNING;
    cs->current = next;

    if (prev == next) return;     /* same task: no switch needed */

    /* Update TSS RSP0 so ring 3 → ring 0 transitions land on
     * the incoming task's kernel stack top. */
    uintptr_t next_kstack_top = vmm_phys_to_virt(next->stack_phys) + TASK_STACK_SIZE;
    gdt_set_tss_rsp0(cpu_id, next_kstack_top);

    /* Also update cpu_info->kernel_stack_top (GS:16) so the SYSCALL
     * fast-path entry (syscall_entry in context_switch.asm) uses the
     * incoming task's kernel stack, not the shared per-CPU boot stack.
     * Without this, SYSCALL frames on the boot stack corrupt the idle
     * task's saved context that also lives on that boot stack. */
    cpu_info_t *ci_ctx = smp_self();
    if (ci_ctx) ci_ctx->kernel_stack_top = next_kstack_top;

    /* If switching to a user task, load its FS base (TLS pointer) */
    if (next->is_user && next->fs_base) {
        wrmsr(0xC0000100, next->fs_base);  /* MSR_FS_BASE */
    }

    /* Perform context switch */
    task_switch_asm(&prev->rsp, next->rsp, next->cr3,
                    prev->fpu_state, next->fpu_state);
}

/* ── sched_task_exit ─────────────────────────────────────────────────────── */
__attribute__((noreturn))
void sched_task_exit(void) {
    cpu_info_t  *ci = smp_self();
    cpu_sched_t *cs = &cpu_scheds[ci->id];
    task_t *cur = cs->current;

    /* Reparent any children of this task to init (PID 1).
     * This prevents dangling ->parent pointers when child exits later. */
    if (g_init_task && cur != g_init_task) {
        task_t *child = cur->children;
        while (child) {
            task_t *next_child = child->child_next;
            child->parent     = g_init_task;
            child->ppid       = g_init_task->pid;
            /* Link into init's children list */
            child->child_next = g_init_task->children;
            g_init_task->children = child;
            child = next_child;
        }
        cur->children = NULL;
        /* If we reparented any zombies, wake init so it can reap them */
        signal_send(g_init_task, SIGCHLD);
    }

    /* Threads in the same thread-group (pid == parent->pid) are reaped
     * immediately; only real child processes become zombies for wait4(). */
    if (cur->parent) {
        if (cur->pid == cur->parent->pid)
            cur->state = TASK_DEAD;
        else {
            cur->state = TASK_ZOMBIE;
            signal_send(cur->parent, SIGCHLD);
        }
    } else {
        cur->state = TASK_DEAD;
    }

    /* Trigger immediate reschedule by forcing a tick */
    sched_tick();
    /* unreachable */
    for (;;) cpu_halt();
}

/* ── sched_block ─────────────────────────────────────────────────────────── */
void sched_block(void) {
    cpu_info_t  *ci = smp_self();
    cpu_sched_t *cs = &cpu_scheds[ci->id];
    task_t *cur = cs->current;

    /* MLFQ: voluntary block (I/O wait) → boost priority (reward) */
    if (cur && cur != cs->idle && cur->priority > 0) {
        cur->priority--;
    }

    cur->state = TASK_BLOCKED;
    sched_tick();
}

/* ── sched_unblock ───────────────────────────────────────────────────────── */
void sched_unblock(task_t *t) {
    if (!t) return;
    /* Atomically transition BLOCKED → RUNNABLE.
     * If the task was not BLOCKED (already runnable, running, or dead),
     * do nothing — it is already in a queue or doesn't need waking. */
    task_state_t old = TASK_BLOCKED;
    if (!__atomic_compare_exchange_n(&t->state, &old, TASK_RUNNABLE,
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
        return;

    cpu_sched_t *cs = &cpu_scheds[t->cpu_id];
    uint64_t f = irq_save_cli();
    rq_lock(cs);
    rq_enqueue(cs, t);
    rq_unlock(cs);
    irq_restore(f);
}

/* ── sched_current ───────────────────────────────────────────────────────── */
task_t *sched_current(void) {
    cpu_info_t *ci = smp_self();
    if (!ci) return NULL;
    return cpu_scheds[ci->id].current;
}

/* ── sched_idle_loop: never returns; used by AP cores ────────────────────── */
__attribute__((noreturn))
void sched_idle_loop(void) {
    cpu_info_t *ci = smp_self();
    KLOG_INFO("sched: CPU%u entering idle loop\n", ci->id);
    cpu_sti();
    for (;;) cpu_halt();
}

/* ── sched_pick_cpu: return CPU with fewest queued tasks ─────────────────── */
/* Round-robin tiebreaker ensures tasks spread across all CPUs when equal    */
static uint32_t g_rr_cpu = 0;   /* round-robin counter (relaxed, approx OK) */

uint32_t sched_pick_cpu(void) {
    uint32_t ncpus = smp_cpu_count();
    if (ncpus == 1) return 0;

    /* Find the minimum queue length across all CPUs */
    uint32_t min_len = (uint32_t)-1;
    for (uint32_t i = 0; i < ncpus; i++) {
        if (cpu_scheds[i].queue_len < min_len)
            min_len = cpu_scheds[i].queue_len;
    }

    /* Among CPUs at min_len, pick the next one in round-robin order so that
     * tasks are distributed evenly even when all CPUs are equally idle.    */
    uint32_t start = __atomic_add_fetch(&g_rr_cpu, 1, __ATOMIC_RELAXED) % ncpus;
    for (uint32_t j = 0; j < ncpus; j++) {
        uint32_t i = (start + j) % ncpus;
        if (cpu_scheds[i].queue_len == min_len)
            return i;
    }
    return 0;   /* unreachable */
}

/* ── sched_spawn: create a task on the least-loaded CPU ──────────────────── */
task_t *sched_spawn(const char *name, task_entry_t entry, void *arg) {
    uint32_t cpu = sched_pick_cpu();
    task_t  *t   = task_create(name, entry, arg, cpu);
    if (t) sched_add_task(t, cpu);
    return t;
}
/* ── sched_set_priority ───────────────────────────────────────────────── */
void sched_set_priority(task_t *t, uint8_t priority) {
    if (!t) return;
    if (priority >= NPRIO) priority = NPRIO - 1;
    t->priority = priority;
    t->timeslice_ticks = 0;
}

/* ── sched_get_ticks ──────────────────────────────────────────────────── */
uint64_t sched_get_ticks(void) {
    return __atomic_load_n(&g_jiffies, __ATOMIC_RELAXED);
}

/* ── sched_sleep ───────────────────────────────────────────────────────── */
void sched_sleep(uint32_t ms) {
    if (ms == 0) return;

    cpu_info_t  *ci = smp_self();
    cpu_sched_t *cs = &cpu_scheds[ci->id];
    task_t      *cur = cs->current;

    uint64_t deadline = __atomic_load_n(&g_jiffies, __ATOMIC_RELAXED) + ms;

    uint64_t f = irq_save_cli();
    rq_lock(cs);

    cur->sleep_deadline = deadline;
    cur->state          = TASK_SLEEPING;

    /* Prepend to per-CPU sleep list */
    cur->sleep_next  = cs->sleep_head;
    cs->sleep_head   = cur;

    rq_unlock(cs);
    irq_restore(f);

    /* Yield CPU; sched_tick() will re-enqueue us after deadline */
    sched_tick();
}

/* ── Task snapshot for task manager ──────────────────────────────────────── */
int sched_snapshot_tasks(sched_task_info_t *buf, int max_count) {
    int n = 0;
    for (uint32_t i = 0; i < TASK_TABLE_SIZE && n < max_count; i++) {
        task_t *t = task_get_from_table(i);
        if (!t || t->state == TASK_DEAD) continue;
        buf[n].tid      = t->tid;
        buf[n].cpu_id   = t->cpu_id;
        buf[n].priority = t->priority;
        buf[n].state    = t->state;
        strncpy(buf[n].name, t->name, TASK_NAME_MAX - 1);
        buf[n].name[TASK_NAME_MAX - 1] = '\0';
        n++;
    }
    return n;
}