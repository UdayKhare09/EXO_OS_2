/**
 * main.c — EXO_OS kernel entry point
 *
 * Boot sequence:
 *   1.  Serial logging
 *   2.  GDT + IDT on BSP
 *   3.  PMM from Limine memory map
 *   4.  VMM (HHDM offset) + kernel heap
 *   5.  PCI enumeration
 *   6.  Graphics stack (VirtIO GPU)
 *   7.  ACPI / MADT
 *   8.  APIC (disable PIC, enable LAPIC, calibrate timer)
 *   9.  SMP (bring up AP cores)
 *   10. Scheduler, USB stack (via task)
 *   11. Enable interrupts → idle loop
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
#include "gfx/desktop.h"
#include "drivers/input/input.h"
#include "drivers/usb/usb_core.h"
#include "drivers/usb/hid.h"

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

/* ── USB + HID init task ─────────────────────────────────────────────────── */
/* Runs after cpu_sti() so that xHCI control transfers can use sched_sleep  */
static void usb_init_task(void *arg) {
    (void)arg;
    /* Register HID boot-protocol driver before enumeration starts           */
    hid_init();
    /* Discover xHCI controller, enumerate ports, bind class drivers.        */
    /* usb_init() also spawns the persistent "usb-evt" task internally.      */
    if (!usb_init())
        KLOG_WARN("usb-init: USB stack unavailable\n");
    /* Task exits cleanly — usb-evt task continues in the background         */
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
    /* ── 5b. Input event subsystem (lock-free SPSC rings, safe pre-IRQ) ────── */
    input_init();
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
        /* Desktop task will create all windows (terminals, sysinfo, taskmgr) */
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

    /* ── 10. Spawn desktop (cursor + shell + input loop) ─────────────────── */
    sched_spawn("desktop", desktop_task, NULL);

    /* ── 11. Spawn USB stack (as task — needs sched_sleep for xHCI cmds) ── */
    sched_spawn("usb-init", usb_init_task, NULL);
    KLOG_INFO("Kernel init complete. Entering idle.\n");

    /* ── 11. Enable interrupts — scheduler takes over from here ─────────── */
    cpu_sti();

    /* BSP falls into idle — scheduler timer will preempt as needed */
    sched_idle_loop();

halt:
    cpu_cli();
    for (;;) cpu_halt();
}
