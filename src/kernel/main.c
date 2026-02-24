/**
 * main.c — EXO_OS kernel entry point
 *
 * Boot sequence:
 *   1. Receive Limine 8 boot info
 *   2. Init serial logging
 *   3. Init GDT + IDT on BSP
 *   4. Init PMM from Limine memory map
 *   5. Init VMM (HHDM offset)
 *   6. Init ACPI / parse MADT
 *   7. Init APIC (disable PIC, enable LAPIC, calibrate timer)
 *   8. Init SMP (bring up AP cores)
 *   9. Enable interrupts and start scheduler
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Limine 8 protocol header — downloaded into build/ by the Makefile */
#include <limine.h>

#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "ipc/ipc.h"
#include "ipc/signal.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/smp.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "arch/x86_64/pci.h"
#include "gfx/virtio_gpu.h"
#include "gfx/compositor.h"
#include "gfx/wm.h"
#include "gfx/fbcon.h"

/* ── Limine protocol: start marker ──────────────────────────────────────── */
__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

/* ── Base revision: MUST live between start and end markers ──────────────── */
__attribute__((section(".limine_requests")))
LIMINE_BASE_REVISION(3)

/* ── Limine protocol: end marker ─────────────────────────────────────────── */
__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ── Limine requests ─────────────────────────────────────────────────────── */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_req = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kaddr_req = {
    .id       = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

/* ── Sleep test ──────────────────────────────────────────────────────────── */
static void task_sleeper(void *arg) {
    (void)arg;
    task_t *self = sched_current();
    KLOG_INFO("[sleep] task tid=%u starting\n", self->tid);

    for (int i = 0; i < 5; i++) {
        uint64_t t0 = sched_get_ticks();
        sched_sleep(200);   /* sleep 200 ms */
        uint64_t t1 = sched_get_ticks();
        KLOG_INFO("[sleep] woke up after ~%llu ms (tick %llu -> %llu)\n",
                  (unsigned long long)(t1 - t0),
                  (unsigned long long)t0,
                  (unsigned long long)t1);
    }
    KLOG_INFO("[sleep] task done\n");
}

/* ── Priority scheduler test ─────────────────────────────────────────────── */
static void task_high_pri(void *arg) {
    (void)arg;
    task_t *self = sched_current();
    sched_set_priority(self, 0);  /* highest priority */
    KLOG_INFO("[prio] high-pri task (tid=%u) priority=0\n", self->tid);

    for (int i = 0; i < 5; i++) {
        KLOG_INFO("[prio] high-pri round %d\n", i);
        for (volatile int d = 0; d < 500000; d++) cpu_pause();
    }
    KLOG_INFO("[prio] high-pri task done\n");
}

static void task_low_pri(void *arg) {
    (void)arg;
    task_t *self = sched_current();
    sched_set_priority(self, 7);  /* lowest priority */
    KLOG_INFO("[prio] low-pri task (tid=%u) priority=7\n", self->tid);

    for (int i = 0; i < 5; i++) {
        KLOG_INFO("[prio] low-pri round %d\n", i);
        for (volatile int d = 0; d < 500000; d++) cpu_pause();
    }
    KLOG_INFO("[prio] low-pri task done\n");
}

/* ── IPC + Signal test ───────────────────────────────────────────────────── */
static volatile uint32_t g_consumer_tid  = 0;
static volatile int      g_consumer_done = 0;

/* Signal handlers — run in task context (safe to call KLOG_INFO) */
static void on_sigusr1(int sig) {
    (void)sig;
    KLOG_INFO("[ipc] consumer: SIGUSR1 received!\n");
}
static void on_sigterm(int sig) {
    (void)sig;
    KLOG_INFO("[ipc] consumer: SIGTERM received, shutting down\n");
    g_consumer_done = 1;
}

static void task_consumer(void *arg) {
    (void)arg;
    task_t *self = sched_current();
    signal_set(self, SIGUSR1, on_sigusr1);
    signal_set(self, SIGTERM, on_sigterm);
    g_consumer_tid = self->tid;  /* publish TID so producer can find us */

    KLOG_INFO("[ipc] consumer tid=%u ready\n", self->tid);
    while (!g_consumer_done) {
        ipc_msg_t msg;
        int r = ipc_recv(&msg);   /* blocks; returns -1 on signal */
        if (r == 0) {
            KLOG_INFO("[ipc] consumer: msg type=%u data=%llu from tid=%u\n",
                      msg.type, msg.data[0], msg.from_tid);
        }
        /* if r == -1 signal_dispatch already ran handlers above */
    }
    KLOG_INFO("[ipc] consumer: exiting\n");
}

static void task_producer(void *arg) {
    (void)arg;
    task_t *self = sched_current();

    /* Spin until consumer has published its TID */
    while (!g_consumer_tid)
        for (volatile int d = 0; d < 100000; d++) cpu_pause();

    uint32_t ctid = g_consumer_tid;
    KLOG_INFO("[ipc] producer tid=%u -> consumer tid=%u\n", self->tid, ctid);

    /* Send 5 data messages */
    for (uint32_t i = 1; i <= 5; i++) {
        ipc_msg_t msg = { .from_tid = self->tid, .type = IPC_MSG_DATA,
                          .data = { i, 0, 0, 0 } };
        if (ipc_send(ctid, &msg) == 0)
            KLOG_INFO("[ipc] producer: sent message %u\n", i);
        for (volatile int d = 0; d < 3000000; d++) cpu_pause();
    }

    /* Send SIGUSR1 */
    KLOG_INFO("[ipc] producer: sending SIGUSR1\n");
    task_t *consumer = task_lookup(ctid);
    if (consumer) signal_send(consumer, SIGUSR1);

    for (volatile int d = 0; d < 5000000; d++) cpu_pause();

    /* Send SIGTERM to shut the consumer down */
    KLOG_INFO("[ipc] producer: sending SIGTERM\n");
    consumer = task_lookup(ctid);
    if (consumer) signal_send(consumer, SIGTERM);

    KLOG_INFO("[ipc] producer: done\n");
}

/* ── Kernel entry point ─────────────────────────────────────────────────── */
__attribute__((noreturn))
void kmain(void) {
    /* ── 1. Serial / logging ─────────────────────────────────────────────── */
    klog_init();
    KLOG_INFO("\n=== EXO_OS kernel starting ===\n");

    /* ── 2. Validate Limine responses ────────────────────────────────────── */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        KLOG_ERR("Limine base revision not supported\n");
        goto halt;
    }
    if (!memmap_req.response || !hhdm_req.response || !rsdp_req.response) {
        KLOG_ERR("Missing required Limine responses\n");
        goto halt;
    }

    KLOG_INFO("Limine: HHDM offset  = %p\n",
              (void *)hhdm_req.response->offset);
    KLOG_INFO("Limine: kernel virt  = %p  phys = %p\n",
              (void *)kaddr_req.response->virtual_base,
              (void *)kaddr_req.response->physical_base);

    /* ── 3. GDT + IDT on BSP ─────────────────────────────────────────────── */
    /* Temporary stack top; will be replaced per-CPU by smp_init */
    /* Linker-defined end of kernel */
    extern char kernel_end[];
    uintptr_t bsp_stack = (uintptr_t)kernel_end + 0x4000;

    gdt_init(0, bsp_stack);
    gdt_load_tss(0);
    idt_init();
    KLOG_INFO("GDT + IDT initialised\n");

    /* ── 4. PMM ───────────────────────────────────────────────────────────── */
    struct limine_memmap_response *mm_resp = memmap_req.response;
    pmm_memmap_entry_t entries[512];
    uint64_t entry_count = mm_resp->entry_count;
    if (entry_count > 512) entry_count = 512;
    for (uint64_t i = 0; i < entry_count; i++) {
        entries[i].base   = mm_resp->entries[i]->base;
        entries[i].length = mm_resp->entries[i]->length;
        entries[i].type   = mm_resp->entries[i]->type;
    }
    pmm_init(entries, entry_count, hhdm_req.response->offset);
    pmm_print_stats();

    /* ── 5. VMM ───────────────────────────────────────────────────────────── */
    /* Use the actual PML4 from CR3 (Limine's page tables), NOT the kernel
     * physical load address — those are completely different things. */
    vmm_init(hhdm_req.response->offset, read_cr3());

    /* ── 5a. Kernel heap (slab allocator) ────────────────────────────────── */
    kmalloc_init();

    /* ── 5b. PCI enumeration ─────────────────────────────────────────────── */
    static pci_device_t s_pci_buf[64];
    int pci_count = pci_enumerate(s_pci_buf, 64);
    KLOG_INFO("pci: found %d device(s)\n", pci_count);

    /* ── 5c. Graphics stack (VirtIO GPU only) ───────────────────────────── */
    {
        gfx_surface_t *screen = NULL;
        if (!virtio_gpu_init(&screen)) {
            KLOG_ERR("gfx: VirtIO GPU required but not found — halting\n");
            goto halt;
        }
        KLOG_INFO("gfx: VirtIO GPU %dx%d ready\n", screen->w, screen->h);
        compositor_init(screen, virtio_gpu_flush);
        compositor_set_bg(GFX_DESKTOP_BG);
        wm_init(screen->w, screen->h);
        fbcon_init();
        fbcon_takeover_klog();
    }

    /* ── 6. ACPI / MADT ──────────────────────────────────────────────────── */
    acpi_init((uintptr_t)rsdp_req.response->address);

    /* ── 7. APIC ─────────────────────────────────────────────────────────── */
    madt_info_t *madt = acpi_get_madt_info();
    apic_init(madt->lapic_base);

    /* Calibrate LAPIC timer using PIT */
    g_apic_ticks_per_ms = apic_timer_calibrate();

    /* Start periodic 1 ms timer on BSP */
    apic_timer_init(g_apic_ticks_per_ms);

    /* ── 8. Scheduler init on BSP (CPU 0) ────────────────────────────────── */
    sched_init(0);

    /* ── 9. SMP: bring up AP cores ───────────────────────────────────────── */
    smp_init();

    /* ── 10. Spawn all tasks (interrupts still OFF) ─────────────────────── */
    KLOG_INFO("Spawning priority test tasks...\n");
    sched_spawn("high-pri", task_high_pri, NULL);
    sched_spawn("low-pri",  task_low_pri,  NULL);
    sched_spawn("sleeper",  task_sleeper,  NULL);
    sched_spawn("consumer", task_consumer, NULL);
    sched_spawn("producer", task_producer, NULL);
    KLOG_INFO("Kernel init complete. Entering idle.\n");

    /* ── 11. Enable interrupts — scheduler takes over from here ─────────── */
    cpu_sti();

    /* BSP falls into idle — scheduler timer will preempt as needed */
    sched_idle_loop();

halt:
    cpu_cli();
    for (;;) cpu_halt();
}
