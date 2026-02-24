#pragma once
#include "task.h"
#include <stdint.h>

/* Initialise scheduler for a specific CPU (call on BSP and each AP) */
void sched_init(uint32_t cpu_id);

/* Add a task to the run queue of the given CPU */
void sched_add_task(task_t *t, uint32_t cpu_id);

/* Called by APIC timer ISR — pick and switch to next task */
void sched_tick(void);

/* Mark current task dead and yield (called when task entry returns) */
__attribute__((noreturn)) void sched_task_exit(void);

/* Block current task (e.g., waiting for I/O) */
void sched_block(void);

/* Unblock a specific task */
void sched_unblock(task_t *t);

/* Idle loop for AP cores */
__attribute__((noreturn)) void sched_idle_loop(void);

/* Returns currently running task on this CPU */
task_t *sched_current(void);

/* Load-balanced task creation: picks the CPU with fewest queued tasks */
uint32_t sched_pick_cpu(void);
task_t  *sched_spawn(const char *name, task_entry_t entry, void *arg);

/* Set task priority (0=highest, 7=lowest); default is 4 */
void sched_set_priority(task_t *t, uint8_t priority);

/* Sleep current task for at least `ms` milliseconds */
void sched_sleep(uint32_t ms);

/* Return the global jiffy counter (1 jiffy = 1 APIC timer tick = ~1 ms) */
uint64_t sched_get_ticks(void);

/* Snapshot of a task for enumeration */
typedef struct {
    uint32_t     tid;
    uint32_t     cpu_id;
    uint8_t      priority;
    task_state_t state;
    char         name[TASK_NAME_MAX];
} sched_task_info_t;

/* Enumerate all active tasks into buf[]. Returns count written.
 * Safe to call from any context (reads atomically). */
int sched_snapshot_tasks(sched_task_info_t *buf, int max_count);

/* Global tick-per-ms for APIC (set by main after calibration) */
extern uint32_t g_apic_ticks_per_ms;
