#include "smp.h"
#include "apic.h"
#include "acpi.h"
#include "gdt.h"
#include "idt.h"
#include "cpu.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include <stdint.h>

#define SMP_TRAMPOLINE_PHYS  0x8000UL
#define SMP_MAILBOX_PHYS     0x8F00UL   /* within same 4 KiB page as trampoline */

/* Laid out matching trampoline.asm mailbox offsets (all naturally aligned) */
typedef struct {
    uint64_t cr3_val;           /* +0x00 */
    uint64_t ap_stack_top;      /* +0x08 */
    uint64_t ap_entry_virt;     /* +0x10 */
    uint64_t cpu_info_virt;     /* +0x18 */
    uint32_t ap_ready_flag;     /* +0x20 */
} smp_mailbox_t;

/* Defined by objcopy when wrapping trampoline.bin into an ELF object.
 * objcopy names symbols:  _binary_<path>_start  _binary_<path>_end
 * The path uses underscores for / and . characters.             */
extern char _binary_build_obj_arch_x86_64_trampoline_bin_start[];
extern char _binary_build_obj_arch_x86_64_trampoline_bin_end[];
#define trampoline_start _binary_build_obj_arch_x86_64_trampoline_bin_start
#define trampoline_end   _binary_build_obj_arch_x86_64_trampoline_bin_end

static cpu_info_t cpus[MAX_CPUS];
static uint32_t   cpu_count = 0;

static uint8_t    bsp_lapic_id;

cpu_info_t *smp_self(void) {
    cpu_info_t *info;
    /* GS base holds pointer to cpu_info_t */
    __asm__ volatile("mov %%gs:0, %0" : "=r"(info));
    return info;
}

uint32_t smp_cpu_count(void) { return cpu_count; }

/* ── Set GS base for current CPU ─────────────────────────────────────────── */
static void smp_set_gs(cpu_info_t *info) {
    wrmsr(MSR_GS_BASE, (uint64_t)info);
    wrmsr(MSR_KERN_GS_BASE, (uint64_t)info);
}

/* ── AP C entry (called from trampoline after long-mode switch) ───────────── */
__attribute__((noreturn))
void smp_ap_entry(cpu_info_t *info) {
    /* Set up GDT + TSS on this AP */
    gdt_init(info->id, info->kernel_stack_top);
    gdt_load_tss(info->id);

    /* Reload IDT (shares BSP's static idt[] table — don't zero handlers!) */
    idt_load();

    /* Enable LAPIC */
    apic_ap_init();

    /* Store cpu_info_t in GS base for smp_self() */
    smp_set_gs(info);

    /* Init per-CPU scheduler */
    sched_init(info->id);

    info->online = 1;

    /* Signal BSP that we are ready */
    smp_mailbox_t *mb = (smp_mailbox_t *)vmm_phys_to_virt(SMP_MAILBOX_PHYS);
    __atomic_store_n(&mb->ap_ready_flag, 1, __ATOMIC_SEQ_CST);

    cpu_sti();

    /* Start periodic APIC timer on this AP */
    apic_timer_init(g_apic_ticks_per_ms);

    /* Drop into scheduler idle loop */
    sched_idle_loop();
}

/* ── Boot a single AP ─────────────────────────────────────────────────────── */
static void smp_boot_ap(uint32_t cpu_idx, uint8_t lapic_id) {
    cpu_info_t *info = &cpus[cpu_idx];

    /* Allocate kernel stack */
    uintptr_t stack_phys = pmm_alloc_pages(CPU_STACK_SIZE / PAGE_SIZE);
    if (!stack_phys) kpanic("SMP: cannot allocate AP stack\n");
    info->kernel_stack_top = vmm_phys_to_virt(stack_phys) + CPU_STACK_SIZE;

    info->self      = info;
    info->id        = cpu_idx;
    info->lapic_id  = lapic_id;
    info->online    = 0;

    /* Fill mailbox */
    smp_mailbox_t *mb = (smp_mailbox_t *)vmm_phys_to_virt(SMP_MAILBOX_PHYS);
    mb->cr3_val       = read_cr3();
    mb->ap_stack_top  = info->kernel_stack_top;
    mb->ap_entry_virt = (uint64_t)smp_ap_entry;
    mb->cpu_info_virt = (uint64_t)info;
    __atomic_store_n(&mb->ap_ready_flag, 0, __ATOMIC_SEQ_CST);

    cpu_mfence();

    KLOG_INFO("SMP: booting AP lapic_id=%u (cpu%u)\n", lapic_id, cpu_idx);

    /* INIT IPI — assert */
    apic_send_init(lapic_id);
    /* Wait ~10 ms */
    for (volatile uint64_t d = 0; d < 3000000ULL; d++) cpu_pause();

    /* INIT IPI — de-assert (level=0, trigger=level, delivery=INIT) */
    apic_send_ipi(lapic_id, APIC_IPI_INIT | (1 << 15));  /* trigger mode = level, level = 0 (de-assert) */
    for (volatile uint64_t d = 0; d < 300000ULL; d++) cpu_pause();

    /* SIPI × 2  (200 µs between) */
    for (int s = 0; s < 2; s++) {
        apic_send_sipi(lapic_id, SMP_TRAMPOLINE_PHYS >> 12);
        for (volatile uint64_t d = 0; d < 300000ULL; d++) cpu_pause();
    }

    /* Wait for AP to signal ready (timeout ~200 ms) */
    for (uint64_t t = 0; t < 200000000ULL; t++) {
        if (__atomic_load_n(&mb->ap_ready_flag, __ATOMIC_SEQ_CST)) {
            KLOG_INFO("SMP: AP%u online\n", cpu_idx);
            return;
        }
        cpu_pause();
    }
    KLOG_WARN("SMP: AP%u timeout — may not have started\n", cpu_idx);
}

/* ── Identity-map low memory so APs can reach trampoline + GDT in 64-bit ── */
static void smp_identity_map_trampoline(void) {
    /* Map first 2 MiB identity (phys == virt) so trampoline code, tmp GDT,
     * and mailbox are all reachable after paging is enabled on AP. */
    for (uintptr_t pa = 0; pa < 0x200000; pa += PAGE_SIZE) {
        vmm_map_page(pa, pa, VMM_KERNEL_RW);
    }
}

/* ── Copy trampoline to low memory ────────────────────────────────────────── */
static void smp_install_trampoline(void) {
    size_t len = (size_t)(trampoline_end - trampoline_start);
    void *dst  = (void *)vmm_phys_to_virt(SMP_TRAMPOLINE_PHYS);
    memcpy(dst, trampoline_start, len);
    smp_identity_map_trampoline();
    KLOG_INFO("SMP: trampoline (%zu bytes) at phys=0x%x\n",
              len, SMP_TRAMPOLINE_PHYS);
}

/* ── Main SMP init called from BSP ───────────────────────────────────────── */
void smp_init(void) {
    madt_info_t *madt  = acpi_get_madt_info();
    bsp_lapic_id        = apic_get_id();

    /* Register BSP as CPU 0 */
    uint32_t bsp_idx = cpu_count++;
    cpus[bsp_idx].self      = &cpus[bsp_idx];
    cpus[bsp_idx].id        = bsp_idx;
    cpus[bsp_idx].lapic_id  = bsp_lapic_id;
    cpus[bsp_idx].online    = 1;
    smp_set_gs(&cpus[bsp_idx]);

    smp_install_trampoline();

    /* Boot each AP that is NOT the BSP */
    for (uint32_t i = 0; i < madt->cpu_count; i++) {
        uint8_t lid = madt->lapic_ids[i];
        if (lid == bsp_lapic_id) continue;
        uint32_t idx = cpu_count++;
        smp_boot_ap(idx, lid);
    }

    KLOG_INFO("SMP: %u CPU(s) online\n", cpu_count);
}
