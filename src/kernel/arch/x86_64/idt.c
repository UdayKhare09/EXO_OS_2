#include "idt.h"
#include "gdt.h"
#include "cpu.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/vmm.h"
#include "ipc/signal.h"

static idt_entry_t  idt[IDT_ENTRIES];
static idtr_t       idtr;
static isr_handler_t isr_handlers[IDT_ENTRIES];

void idt_set_handler(uint8_t vec, void *handler, uint8_t flags, uint8_t ist) {
    uintptr_t addr = (uintptr_t)handler;
    idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector    = GDT_KERN_CODE;
    idt[vec].ist         = ist & 0x07;
    idt[vec].flags       = flags;
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)(addr >> 32);
    idt[vec].reserved    = 0;
}

void idt_register_handler(uint8_t vec, isr_handler_t fn) {
    isr_handlers[vec] = fn;
}

/* ── Page fault handler (#PF, vector 14) ──────────────────────────────────── */
static void page_fault_handler(cpu_regs_t *regs) {
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    task_t *cur = sched_current();
    uintptr_t pml4 = cur ? cur->cr3 : vmm_get_kernel_pml4();

    /* Try COW / demand-page resolution */
    if (vmm_handle_page_fault(pml4, fault_addr, regs->err)) {
        return;  /* handled — resume execution */
    }

    /* Unresolvable fault in user-mode (CS RPL=3) → SIGSEGV */
    if ((regs->cs & 3) == 3 && cur) {
        KLOG_WARN("PF: SIGSEGV tid=%u addr=%p err=0x%x rip=%p\n",
                  cur->tid, (void *)fault_addr,
                  (uint32_t)regs->err, (void *)regs->rip);
        signal_send(cur, SIGSEGV);
        /* If SIGKILL, scheduler will kill task. For now, mark dead. */
        cur->state = TASK_DEAD;
        sched_tick();
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Kernel-mode page fault — fatal */
    KLOG_WARN("#PF in kernel: addr=%p err=0x%x rip=%p\n",
              (void *)fault_addr, (uint32_t)regs->err, (void *)regs->rip);
    if (cur && cur->tid > 0) {
        cur->state = TASK_DEAD;
        sched_tick();
        for (;;) __asm__ volatile("cli; hlt");
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* Called from isr.asm's common_isr_handler label */
void isr_dispatch(cpu_regs_t *regs) {
    uint8_t vec = (uint8_t)regs->vec;
    if (isr_handlers[vec]) {
        isr_handlers[vec](regs);
        return;
    }

    /* ── Unhandled exception in vectors 0-31 ─────────────────────────────── */
    if (vec < 32) {
        KLOG_WARN("Unhandled exception vec=0x%x err=0x%x rip=%p\n",
                  vec, (uint32_t)regs->err, (void *)regs->rip);

        /* Fatal CPU exception: kill the current task (if not idle/kernel init)
         * to prevent an infinite fault loop where iretq returns to the same
         * faulting instruction. */
        task_t *cur = sched_current();
        if (cur && cur->tid > 0) {
            KLOG_WARN("  killing task '%s' tid=%u\n", cur->name, cur->tid);
            cur->state = TASK_DEAD;
            sched_tick();
            /* unreachable for the killed task */
            for (;;) __asm__ volatile("cli; hlt");
        }
        /* If no current task (very early boot), halt the CPU */
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Unhandled IRQ/vector above 31: just warn (these are non-fatal) */
    KLOG_WARN("Unhandled interrupt vec=0x%x err=0x%x rip=%p\n",
              vec, (uint32_t)regs->err, (void *)regs->rip);
}

/* ── #UD — Invalid Opcode (vector 6) ─────────────────────────────────────── */
static void invalid_opcode_handler(cpu_regs_t *regs) {
    task_t *cur = sched_current();
    if ((regs->cs & 3) == 3 && cur) {
        KLOG_WARN("#UD SIGILL tid=%u '%s' rip=%p\n",
                  cur->tid, cur->name, (void *)regs->rip);
        signal_send(cur, SIGILL);
        cur->state = TASK_DEAD;
        sched_tick();
        for (;;) __asm__ volatile("cli; hlt");
    }
    KLOG_WARN("#UD in kernel rip=%p\n", (void *)regs->rip);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── #MF — x87 Floating-Point Exception (vector 16) ─────────────────────── */
static void x87_fp_handler(cpu_regs_t *regs) {
    task_t *cur = sched_current();
    /* Clear x87 exception flags so the handler can return without re-faulting */
    __asm__ volatile("fnclex");
    if ((regs->cs & 3) == 3 && cur) {
        KLOG_WARN("#MF SIGFPE (x87) tid=%u '%s' rip=%p\n",
                  cur->tid, cur->name, (void *)regs->rip);
        signal_send(cur, SIGFPE);
        cur->state = TASK_DEAD;
        sched_tick();
        for (;;) __asm__ volatile("cli; hlt");
    }
    KLOG_WARN("#MF in kernel rip=%p\n", (void *)regs->rip);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── #XF — SIMD Floating-Point Exception (vector 19) ────────────────────── */
static void simd_fp_handler(cpu_regs_t *regs) {
    task_t *cur = sched_current();
    /* Clear MXCSR exception-status flags so the faulting instruction can be
     * retried (or, more commonly, so the signal handler can return cleanly).
     * MXCSR is at byte offset 24 in the legacy FXSAVE area, which is the
     * first 512 bytes of the XSAVE buffer. */
    if (cur && cur->fpu_state) {
        uint32_t *mxcsr = (uint32_t *)(cur->fpu_state + 24);
        *mxcsr &= ~0x3Fu;   /* clear IE|DE|ZE|OE|UE|PE status bits */
    }
    if ((regs->cs & 3) == 3 && cur) {
        KLOG_WARN("#XF SIGFPE (SSE) tid=%u '%s' rip=%p\n",
                  cur->tid, cur->name, (void *)regs->rip);
        signal_send(cur, SIGFPE);
        cur->state = TASK_DEAD;
        sched_tick();
        for (;;) __asm__ volatile("cli; hlt");
    }
    KLOG_WARN("#XF in kernel rip=%p\n", (void *)regs->rip);
    for (;;) __asm__ volatile("cli; hlt");
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    memset(isr_handlers, 0, sizeof(isr_handlers));

    /* Install all stubs (defined in isr.asm) */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_handler((uint8_t)i, isr_stub_table[i], IDT_INT_GATE, 0);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    /* Register page fault handler (#PF = vector 14) */
    idt_register_handler(14, page_fault_handler);

    /* Register SIMD FP exception handler (#XF = vector 19) */
    idt_register_handler(19, simd_fp_handler);

    /* Register x87 FP exception handler (#MF = vector 16) */
    idt_register_handler(16, x87_fp_handler);

    /* Register invalid opcode handler (#UD = vector 6) */
    idt_register_handler(6, invalid_opcode_handler);

    __asm__ volatile("lidt %0" : : "m"(idtr));
    KLOG_INFO("IDT: loaded %d entries at %p\n", IDT_ENTRIES, (void *)&idt);
}

/* Just reload the IDTR — for APs that share the BSP's IDT table */
void idt_load(void) {
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
