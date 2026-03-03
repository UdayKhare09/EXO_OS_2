#pragma once
#include <stdint.h>

struct task;  /* forward declaration */

/* ── Wait Queue ───────────────────────────────────────────────────────────
 * A wait queue is a list of tasks sleeping on a condition.
 * When the condition becomes true, waitq_wake_*() moves tasks back to RUNNABLE.
 *
 * Typical pattern:
 *   while (!condition) waitq_wait(&wq);     // in task context
 *   ...
 *   condition = true;
 *   waitq_wake_all(&wq);                    // can be called from any context
 */

typedef struct waitq {
    struct task *head;
    struct task *tail;
    volatile int lock;
} waitq_t;

/* Initialize a wait queue (call once before use) */
void waitq_init(waitq_t *wq);

/* Block current task on this wait queue.
 * The task state becomes BLOCKED and scheduler picks another task.
 * When woken by waitq_wake_*, the task becomes RUNNABLE. */
void waitq_wait(waitq_t *wq);

/* Like waitq_wait() but returns after at most timeout_ms milliseconds.
 * Returns 0 if woken by waitq_wake_*, 1 if the timeout expired first.
 * Uses a ktimer internally — safe to call from task context only. */
int waitq_wait_timeout(waitq_t *wq, uint64_t timeout_ms);

/* Wake one task from the wait queue (FIFO order).
 * Safe to call from any CPU or ISR context. */
void waitq_wake_one(waitq_t *wq);

/* Wake all tasks in the wait queue.
 * Safe to call from any CPU or ISR context. */
void waitq_wake_all(waitq_t *wq);
