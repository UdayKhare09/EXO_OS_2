#include "sched.h"
#include "task.h"
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

/* ── Per-CPU scheduler state ─────────────────────────────────────────────── */
typedef struct {
    task_t  *current;     /* task currently running                 */
    task_t  *queue_head;  /* head of run queue (circular singly-LL) */
    task_t  *queue_tail;  /* tail                                   */
    task_t  *idle;        /* dedicated idle task                    */
    uint32_t queue_len;
    uint32_t cpu_id;
} cpu_sched_t;

static cpu_sched_t cpu_scheds[MAX_CPUS];

/* Defined in context_switch.asm */
extern void task_switch_asm(uint64_t *old_rsp_ptr,
                            uint64_t  new_rsp,
                            uint64_t  new_cr3);

/* ── Run-queue helpers ────────────────────────────────────────────────────── */
static void rq_enqueue(cpu_sched_t *cs, task_t *t) {
    t->next = NULL;
    if (!cs->queue_head) {
        cs->queue_head = cs->queue_tail = t;
    } else {
        cs->queue_tail->next = t;
        cs->queue_tail = t;
    }
    cs->queue_len++;
}

/* Dequeue from front */
static task_t *rq_dequeue(cpu_sched_t *cs) {
    if (!cs->queue_head) return NULL;
    task_t *t = cs->queue_head;
    cs->queue_head = t->next;
    if (!cs->queue_head) cs->queue_tail = NULL;
    t->next = NULL;
    cs->queue_len--;
    return t;
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
    rq_enqueue(cs, t);
}

/* ── sched_tick: called from APIC timer ISR ──────────────────────────────── */
void sched_tick(void) {
    cpu_info_t  *ci = smp_self();
    if (!ci) return;
    uint32_t     cpu_id = ci->id;
    cpu_sched_t *cs     = &cpu_scheds[cpu_id];

    task_t *prev = cs->current;

    /* Re-enqueue previous task if still runnable */
    if (prev && prev != cs->idle && prev->state == TASK_RUNNING) {
        prev->state = TASK_RUNNABLE;
        rq_enqueue(cs, prev);
    }

    /* Pick next runnable task */
    task_t *next = NULL;
    uint32_t tried = 0;
    while (tried < cs->queue_len + 1) {
        task_t *t = rq_dequeue(cs);
        if (!t) break;
        if (t->state == TASK_RUNNABLE) { next = t; break; }
        if (t->state == TASK_DEAD)     { task_destroy(t); tried++; continue; }
        rq_enqueue(cs, t);   /* skip blocked tasks */
        tried++;
    }

    if (!next) next = cs->idle;   /* fallback to idle */

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
    cs->current->state = TASK_BLOCKED;
    sched_tick();
}

/* ── sched_unblock ───────────────────────────────────────────────────────── */
void sched_unblock(task_t *t) {
    t->state = TASK_RUNNABLE;
    rq_enqueue(&cpu_scheds[t->cpu_id], t);
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
