/* shell/cputest.c — CPU / SMP / scheduler / multithreading test suite
 *
 * Tests run sequentially, results logged to fbcon + serial.
 *
 *  T1  Task lifecycle        — spawn, run, self-terminate
 *  T2  SMP per-CPU coverage  — one task per CPU, each records cpu_id
 *  T3  Atomic shared counter — N tasks hammer __atomic_add; exact sum
 *  T4  Sleep accuracy        — sleep 50 ms, measure jiffy delta (±10 ms)
 *  T5  Wait-queue wake       — producer posts, blocked consumer wakes
 *  T6  Priority ordering     — high-pri task finishes before low-pri
 *  T7  Parallel sum          — N tasks sum array slices, merge
 *
 * DESIGN NOTES:
 *  • Worker tasks NEVER call KLOG_INFO — the serial spinlock (klog_lock)
 *    can stall a worker indefinitely if the BSP is flooding serial output.
 *  • Workers signal completion via __atomic_store_n (RELEASE) on a shared
 *    flag; the main task spins with sched_sleep(1) between polls (ACQUIRE).
 *  • All test-state globals are reset before each test.
 */

#include "cputest.h"
#include "gfx/fbcon.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/waitq.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── ANSI helpers ─────────────────────────────────────────────────────────── */
#define A_RESET   "\033[0m"
#define A_BOLD    "\033[1m"
#define A_GREEN   "\033[1;32m"
#define A_RED     "\033[1;31m"
#define A_YELLOW  "\033[1;33m"
#define A_CYAN    "\033[1;36m"
#define A_WHITE   "\033[1;37m"
#define A_MAGENTA "\033[1;35m"

/* ── Cross-CPU polling helper ────────────────────────────────────────────── */
/* Poll an atomic int until it equals `target`, or the timeout_ms expires.  */
static bool poll_until(volatile int *v, int target, uint32_t timeout_ms) {
    uint64_t deadline = sched_get_ticks() + timeout_ms;
    while (__atomic_load_n(v, __ATOMIC_ACQUIRE) != target) {
        if (sched_get_ticks() >= deadline) return false;
        sched_sleep(1);   /* yield so other tasks can run */
    }
    return true;
}

/* ── Test bookkeeping ─────────────────────────────────────────────────────── */
static fbcon_t *g_con;
static int      g_pass, g_fail;

static void test_hdr(int n, const char *name) {
    fbcon_printf_inst(g_con,
        "  " A_CYAN "[T%d]" A_RESET " " A_WHITE "%-40s" A_RESET " ... ",
        n, name);
    KLOG_INFO("cputest T%d: %s\n", n, name);
}

static void test_ok(int n, const char *detail) {
    fbcon_printf_inst(g_con, A_GREEN "PASS" A_RESET " %s\n", detail);
    KLOG_INFO("cputest T%d PASS: %s\n", n, detail);
    g_pass++;
}

static void test_ng(int n, const char *detail) {
    fbcon_printf_inst(g_con, A_RED "FAIL" A_RESET " %s\n", detail);
    KLOG_WARN("cputest T%d FAIL: %s\n", n, detail);
    g_fail++;
}

/* ════════════════════════════════════════════════════════════════════════════
 * T1 — Task Lifecycle
 * Spawn a worker; it immediately signals done then returns.
 * Verifies that task_create → schedule → run → exit works end-to-end.
 * ══════════════════════════════════════════════════════════════════════════*/
static volatile int t1_done;

static void t1_worker(void *arg) {
    (void)arg;
    /* Signal immediately — no sleep, no klog (avoids serial spinlock stall) */
    __atomic_store_n(&t1_done, 1, __ATOMIC_RELEASE);
}

static void run_t1(void) {
    test_hdr(1, "Task lifecycle (spawn → run → exit)");
    t1_done = 0;
    sched_spawn("t1", t1_worker, NULL);
    bool ok = poll_until(&t1_done, 1, 2000);
    if (ok) test_ok(1, "(task ran and signalled within 2000 ms)");
    else    test_ng(1, "(task did not signal within 2000 ms)");
}

/* ════════════════════════════════════════════════════════════════════════════
 * T2 — SMP Per-CPU Coverage
 * Pin exactly ONE task to each logical CPU via task_create + sched_add_task.
 * Each task records which cpu_id smp_self() reports; expect all CPUs seen.
 * ══════════════════════════════════════════════════════════════════════════*/
#define T2_MAX_CPUS  8
static volatile int t2_cpu_seen[T2_MAX_CPUS];
static volatile int t2_done;
static int          t2_ncpus;

static void t2_worker(void *arg) {
    (void)arg;
    cpu_info_t *ci = smp_self();
    if (ci && ci->id < T2_MAX_CPUS)
        __atomic_store_n((volatile int *)&t2_cpu_seen[ci->id], 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t2_done, 1, __ATOMIC_RELEASE);
}

static void run_t2(void) {
    t2_ncpus = (int)smp_cpu_count();
    if (t2_ncpus > T2_MAX_CPUS) t2_ncpus = T2_MAX_CPUS;

    test_hdr(2, "SMP coverage (one task pinned per CPU)");
    t2_done = 0;
    for (int i = 0; i < t2_ncpus; i++) t2_cpu_seen[i] = 0;

    /* Pin one task explicitly to each CPU — no reliance on load balancer  */
    for (int i = 0; i < t2_ncpus; i++) {
        task_t *t = task_create("t2", t2_worker, NULL, (uint32_t)i);
        if (t) sched_add_task(t, (uint32_t)i);
    }

    poll_until(&t2_done, t2_ncpus, 4000);

    int seen = 0;
    for (int i = 0; i < t2_ncpus; i++) seen += t2_cpu_seen[i];

    char det[64];
    ksnprintf(det, sizeof(det), "(%d/%d CPUs executed a task)", seen, t2_ncpus);
    if (seen == t2_ncpus) test_ok(2, det);
    else                   test_ng(2, det);
}

/* ════════════════════════════════════════════════════════════════════════════
 * T3 — Atomic Shared Counter
 * N_TASKS tasks, each adding ITERS to a shared counter atomically.
 * Expected final value: N_TASKS × ITERS.
 * Using 4 tasks (one per CPU) with 1000 iters each = 4000 expected.
 * ══════════════════════════════════════════════════════════════════════════*/
#define T3_NTASKS   4
#define T3_ITERS    1000
static volatile uint64_t t3_counter;
static volatile int      t3_done;

static void t3_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < T3_ITERS; i++)
        __atomic_add_fetch(&t3_counter, 1ULL, __ATOMIC_RELAXED);
    __atomic_add_fetch(&t3_done, 1, __ATOMIC_RELEASE);
}

static void run_t3(void) {
    test_hdr(3, "Atomic counter (4 tasks × 1000 incr = 4000)");
    t3_counter = 0; t3_done = 0;

    for (int i = 0; i < T3_NTASKS; i++)
        sched_spawn("t3", t3_worker, NULL);

    poll_until(&t3_done, T3_NTASKS, 5000);

    uint64_t expected = (uint64_t)T3_NTASKS * T3_ITERS;
    uint64_t got      = __atomic_load_n(&t3_counter, __ATOMIC_SEQ_CST);

    char det[64];
    ksnprintf(det, sizeof(det), "(got=%lu expected=%lu)", got, expected);
    if (got == expected) test_ok(3, det);
    else                 test_ng(3, det);
}

/* ════════════════════════════════════════════════════════════════════════════
 * T4 — Sleep Accuracy
 * Shell task (CPU0) sleeps 50 ms; measure jiffy delta.
 * Pass if |delta − 50| ≤ 10 ms.
 * ══════════════════════════════════════════════════════════════════════════*/
static void run_t4(void) {
    test_hdr(4, "Sleep accuracy (50 ms ± 10 ms)");
    uint64_t t0 = sched_get_ticks();
    sched_sleep(50);
    uint64_t delta = sched_get_ticks() - t0;

    char det[64];
    ksnprintf(det, sizeof(det), "(slept %lu ms, target 50 ms)", delta);
    int64_t err = (int64_t)delta - 50;
    if (err < 0) err = -err;
    if (err <= 10) test_ok(4, det);
    else           test_ng(4, det);
}

/* ════════════════════════════════════════════════════════════════════════════
 * T5 — Tick Count Monotonicity + Cross-Task Visibility
 * Spawn two tasks that each read sched_get_ticks(), store the result.
 * After both complete, verify:
 *   • Both stored a non-zero tick
 *   • The main task's ticks are >= both workers' ticks
 * This verifies cross-CPU tick visibility without needing waitq.
 * ══════════════════════════════════════════════════════════════════════════*/
static volatile uint64_t t5_tick[2];
static volatile int      t5_done;

static void t5_worker(void *arg) {
    int idx = (int)(uintptr_t)arg;
    /* Brief busy-wait to ensure non-zero tick */
    for (volatile int i = 0; i < 100000; i++) cpu_pause();
    uint64_t t = sched_get_ticks();
    __atomic_store_n((volatile uint64_t *)&t5_tick[idx], t, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t5_done, 1, __ATOMIC_RELEASE);
}

static void run_t5(void) {
    test_hdr(5, "Cross-CPU tick visibility (2 workers)");
    t5_tick[0] = 0; t5_tick[1] = 0; t5_done = 0;

    sched_spawn("t5a", t5_worker, (void *)(uintptr_t)0);
    sched_spawn("t5b", t5_worker, (void *)(uintptr_t)1);

    bool ok = poll_until(&t5_done, 2, 2000);
    uint64_t now = sched_get_ticks();
    uint64_t a = __atomic_load_n(&t5_tick[0], __ATOMIC_ACQUIRE);
    uint64_t b = __atomic_load_n(&t5_tick[1], __ATOMIC_ACQUIRE);

    char det[80];
    ksnprintf(det, sizeof(det),
              "(a=%lu b=%lu now=%lu)", a, b, now);

    if (ok && a > 0 && b > 0 && now >= a && now >= b)
        test_ok(5, det);
    else
        test_ng(5, det);
}

/* ════════════════════════════════════════════════════════════════════════════
 * T6 — Wait-Queue Wake (producer/consumer)
 * Consumer calls waitq_wait (BLOCKS).  Producer wakes after 30 ms.
 * Consumer records its wakeup tick.  Verify consumer was unblocked.
 * ══════════════════════════════════════════════════════════════════════════*/
static waitq_t        t6_wq;
static volatile int   t6_consumer_done;
static volatile int   t6_producer_done;

static void t6_consumer(void *arg) {
    (void)arg;
    waitq_wait(&t6_wq);
    __atomic_store_n(&t6_consumer_done, 1, __ATOMIC_RELEASE);
}

static void t6_producer(void *arg) {
    (void)arg;
    sched_sleep(30);
    waitq_wake_one(&t6_wq);
    __atomic_store_n(&t6_producer_done, 1, __ATOMIC_RELEASE);
}

static void run_t6(void) {
    test_hdr(6, "Wait-queue: producer wakes blocked consumer");
    waitq_init(&t6_wq);
    t6_consumer_done = 0;
    t6_producer_done = 0;

    sched_spawn("t6c", t6_consumer, NULL);
    sched_spawn("t6p", t6_producer, NULL);

    bool ok = poll_until(&t6_consumer_done, 1, 2000);
    if (ok) test_ok(6, "(consumer woken via waitq_wake_one)");
    else    test_ng(6, "(consumer not woken within 2000 ms)");
}

/* ════════════════════════════════════════════════════════════════════════════
 * T7 — MLFQ Priority Ordering
 * Spawn HIGH‑pri (0) and LOW‑pri (7) tasks simultaneously.
 * Both just signal a per-task done flag after a small busy loop.
 * Record finish sequence (order of done flags set).
 * High-pri should finish first (or at same time) because the scheduler
 * prefers lower priority numbers.
 * ══════════════════════════════════════════════════════════════════════════*/
static volatile uint64_t t7_done_order[2];  /* [0]=hi done tick, [1]=lo done tick */
static volatile int      t7_count;

static void t7_hi_worker(void *arg) {
    (void)arg;
    /* Small busy-work */
    for (volatile int i = 0; i < 500000; i++) cpu_pause();
    uint64_t tick = sched_get_ticks();
    __atomic_store_n((volatile uint64_t *)&t7_done_order[0], tick, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t7_count, 1, __ATOMIC_RELEASE);
}

static void t7_lo_worker(void *arg) {
    (void)arg;
    for (volatile int i = 0; i < 500000; i++) cpu_pause();
    uint64_t tick = sched_get_ticks();
    __atomic_store_n((volatile uint64_t *)&t7_done_order[1], tick, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t7_count, 1, __ATOMIC_RELEASE);
}

static void run_t7(void) {
    test_hdr(7, "MLFQ priority (hi-pri finishes first)");
    t7_count = 0;
    t7_done_order[0] = 0;
    t7_done_order[1] = 0;

    /* Spawn low-pri first so the scheduler has a live low-pri task when
     * the high-pri task is added — forcing a priority decision. */
    task_t *tlo = sched_spawn("t7lo", t7_lo_worker, NULL);
    task_t *thi = sched_spawn("t7hi", t7_hi_worker, NULL);
    if (tlo) sched_set_priority(tlo, 7);   /* lowest  = 7 */
    if (thi) sched_set_priority(thi, 0);   /* highest = 0 */

    bool ok = poll_until(&t7_count, 2, 3000);
    uint64_t hi = __atomic_load_n(&t7_done_order[0], __ATOMIC_ACQUIRE);
    uint64_t lo = __atomic_load_n(&t7_done_order[1], __ATOMIC_ACQUIRE);

    char det[80];
    ksnprintf(det, sizeof(det),
              "(hi_tick=%lu lo_tick=%lu diff=%ld ms)",
              hi, lo, (int64_t)lo - (int64_t)hi);

    if (!ok)              test_ng(7, "(tasks did not complete within 3000 ms)");
    else if (hi <= lo)    test_ok(7, det);
    else                  test_ng(7, det);
}

/* ════════════════════════════════════════════════════════════════════════════
 * T8 — Parallel Array Summation (multi-task compute)
 * Divide a 512-element uint32 array among 4 workers; merge and compare
 * against serial reference.  Verifies parallel compute correctness.
 * ══════════════════════════════════════════════════════════════════════════*/
#define T8_LEN      512
#define T8_WORKERS  4

static uint32_t         t8_data[T8_LEN];
static volatile uint64_t t8_partial[T8_WORKERS];
static volatile int      t8_done;

typedef struct { int id, start, end; } t8_arg_t;
static t8_arg_t t8_args[T8_WORKERS];   /* static — no dangling ptr risk */

static void t8_worker(void *arg) {
    t8_arg_t *a = (t8_arg_t *)arg;
    uint64_t sum = 0;
    for (int i = a->start; i < a->end; i++)
        sum += t8_data[i];
    __atomic_store_n((volatile uint64_t *)&t8_partial[a->id], sum, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t8_done, 1, __ATOMIC_RELEASE);
}

static void run_t8(void) {
    test_hdr(8, "Parallel sum (4 tasks × 128 elements)");

    /* data[i] = i & 0xFF  → serial sum = 2 × (0+1+…+127) × 2 = 32640 */
    for (int i = 0; i < T8_LEN; i++) t8_data[i] = (uint32_t)(i & 0xFF);
    uint64_t serial = 0;
    for (int i = 0; i < T8_LEN; i++) serial += t8_data[i];

    t8_done = 0;
    int slice = T8_LEN / T8_WORKERS;
    for (int i = 0; i < T8_WORKERS; i++) {
        t8_partial[i]  = 0;
        t8_args[i].id    = i;
        t8_args[i].start = i * slice;
        t8_args[i].end   = (i == T8_WORKERS - 1) ? T8_LEN : (i + 1) * slice;
        sched_spawn("t8", t8_worker, &t8_args[i]);
    }

    poll_until(&t8_done, T8_WORKERS, 4000);

    uint64_t parallel = 0;
    for (int i = 0; i < T8_WORKERS; i++)
        parallel += __atomic_load_n((volatile uint64_t *)&t8_partial[i], __ATOMIC_ACQUIRE);

    char det[80];
    ksnprintf(det, sizeof(det),
              "(parallel=%lu serial=%lu)", parallel, serial);
    if (parallel == serial) test_ok(8, det);
    else                    test_ng(8, det);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Public entry point
 * ══════════════════════════════════════════════════════════════════════════*/
void cputest_run(fbcon_t *con) {
    g_con  = con;
    g_pass = 0;
    g_fail = 0;

    uint32_t ncpus = smp_cpu_count();

    fbcon_printf_inst(con,
        "\n" A_MAGENTA
        "  ╔════════════════════════════════════════╗\n"
        "  ║" A_WHITE "  EXO_OS CPU / SMP / Sched Test Suite" A_MAGENTA " ║\n"
        "  ╚════════════════════════════════════════╝\n" A_RESET
        "\n"
        "  " A_WHITE "CPUs:" A_RESET " " A_GREEN "%u" A_RESET
        "  " A_WHITE "Sched:" A_RESET " " A_CYAN "MLFQ-8" A_RESET
        "  " A_WHITE "Timer:" A_RESET " " A_CYAN "~1 ms/tick" A_RESET
        "  " A_WHITE "Ticks now:" A_RESET " " A_CYAN "%lu" A_RESET "\n\n",
        ncpus, sched_get_ticks());

    KLOG_INFO("cputest: suite starting (%u CPUs, tick=%lu)\n",
              ncpus, sched_get_ticks());

    run_t1();
    run_t2();
    run_t3();
    run_t4();
    run_t5();
    run_t6();
    run_t7();
    run_t8();

    /* ── Summary ─────────────────────────────────────────────────────────── */
    fbcon_printf_inst(con,
        "\n  " A_WHITE "Results:" A_RESET
        "  " A_GREEN "%d passed" A_RESET
        "  " A_RED   "%d failed" A_RESET
        "  —  %s\n\n",
        g_pass, g_fail,
        (g_fail == 0)
            ? A_GREEN "ALL TESTS PASSED ✓" A_RESET
            : A_RED   "SOME TESTS FAILED ✗" A_RESET);

    KLOG_INFO("cputest: done — %d pass, %d fail\n", g_pass, g_fail);
}
