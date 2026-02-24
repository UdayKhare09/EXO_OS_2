#include "sched.h"
#include "task.h"
#include "ipc/signal.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/idt.h"
#include "mm/vmm.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include <stdint.h>
#include <stddef.h>

/* Exposed for AP cores to read after calibration */
uint32_t g_apic_ticks_per_ms = 0;

/* Global jiffy counter: incremented 1/ms by BSP timer ISR (~1 ms resolution) */
static volatile uint64_t g_jiffies = 0;

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
                            uint64_t  new_cr3);

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

    /* Advance jiffy counter on CPU 0 (shared millisecond clock) */
    if (cpu_id == 0)
        __atomic_add_fetch(&g_jiffies, 1, __ATOMIC_RELAXED);

    uint64_t now = __atomic_load_n(&g_jiffies, __ATOMIC_RELAXED);

    task_t *prev = cs->current;

    /* MLFQ: increment timeslice counter for running task */
    if (prev && prev != cs->idle && prev->state == TASK_RUNNING) {
        prev->timeslice_ticks++;
    }

    rq_lock(cs);
    /* Re-enqueue previous task if still runnable (with priority adjustment) */
    if (prev && prev != cs->idle && prev->state == TASK_RUNNING) {
        prev->state = TASK_RUNNABLE;

        /* MLFQ feedback: if task used full timeslice, drop priority */
        if (prev->timeslice_ticks >= TIMESLICE_TICKS) {
            if (prev->priority < NPRIO - 1) prev->priority++;
            prev->timeslice_ticks = 0;
        }

        rq_enqueue(cs, prev);
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
        rq_enqueue(cs, t);   /* skip blocked tasks */
        tried++;
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

    /* Reset timeslice counter when task starts running */
    if (next != cs->idle) {
        next->timeslice_ticks = 0;
    }

    next->state = TASK_RUNNING;
    cs->current = next;

    if (prev == next) return;     /* same task: no switch needed */

    /* Perform context switch */
    task_switch_asm(&prev->rsp, next->rsp, next->cr3);
}

/* ── sched_task_exit ─────────────────────────────────────────────────────── */
__attribute__((noreturn))
void sched_task_exit(void) {
    cpu_info_t  *ci = smp_self();
    cpu_sched_t *cs = &cpu_scheds[ci->id];
    cs->current->state = TASK_DEAD;
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

/* ── sched_pick_cpu: return CPU index with fewest queued tasks ────────────── */
uint32_t sched_pick_cpu(void) {
    uint32_t ncpus   = smp_cpu_count();
    uint32_t best    = 0;
    uint32_t min_len = cpu_scheds[0].queue_len;
    for (uint32_t i = 1; i < ncpus; i++) {
        if (cpu_scheds[i].queue_len < min_len) {
            min_len = cpu_scheds[i].queue_len;
            best    = i;
        }
    }
    return best;
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