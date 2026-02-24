#include "idt.h"
#include "gdt.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "sched/sched.h"
#include "sched/task.h"

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

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    memset(isr_handlers, 0, sizeof(isr_handlers));

    /* Install all stubs (defined in isr.asm) */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_handler((uint8_t)i, isr_stub_table[i], IDT_INT_GATE, 0);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    __asm__ volatile("lidt %0" : : "m"(idtr));
    KLOG_INFO("IDT: loaded %d entries at %p\n", IDT_ENTRIES, (void *)&idt);
}

/* Just reload the IDTR — for APs that share the BSP's IDT table */
void idt_load(void) {
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
