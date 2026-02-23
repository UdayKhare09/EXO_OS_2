#include "gdt.h"
#include "lib/string.h"
#include "lib/klog.h"

static gdt_table_t gdt_tables[MAX_CPUS];
static tss_t       tss_entries[MAX_CPUS];

static void gdt_set_entry(gdt_entry_t *e, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    e->base_low  = (uint16_t)(base & 0xFFFF);
    e->base_mid  = (uint8_t)((base >> 16) & 0xFF);
    e->base_high = (uint8_t)((base >> 24) & 0xFF);
    e->limit_low = (uint16_t)(limit & 0xFFFF);
    e->gran      = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    e->access    = access;
}

static void gdt_set_tss_desc(tss_desc_t *d, uintptr_t base, uint32_t limit) {
    d->len        = (uint16_t)limit;
    d->base_low   = (uint16_t)(base & 0xFFFF);
    d->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    d->flags_lo   = 0x89;  /* Present, DPL=0, Type=0x9 (64-bit TSS available) */
    d->flags_hi   = 0x00;
    d->base_high  = (uint8_t)((base >> 24) & 0xFF);
    d->base_upper = (uint32_t)(base >> 32);
    d->reserved   = 0;
}

void gdt_init(uint32_t cpu_id, uintptr_t kernel_stack_top) {
    gdt_table_t *gdt = &gdt_tables[cpu_id];
    tss_t       *tss = &tss_entries[cpu_id];

    memset(gdt, 0, sizeof(*gdt));
    memset(tss, 0, sizeof(*tss));

    /* 0x00: null descriptor */
    gdt_set_entry(&gdt->entries[0], 0, 0, 0, 0);

    /* 0x08: kernel code (64-bit) */
    gdt_set_entry(&gdt->entries[1], 0, 0xFFFFF, 0x9A, 0xA0);

    /* 0x10: kernel data */
    gdt_set_entry(&gdt->entries[2], 0, 0xFFFFF, 0x92, 0xC0);

    /* 0x18: user data (DPL=3) */
    gdt_set_entry(&gdt->entries[3], 0, 0xFFFFF, 0xF2, 0xC0);

    /* 0x20: user code (64-bit, DPL=3) */
    gdt_set_entry(&gdt->entries[4], 0, 0xFFFFF, 0xFA, 0xA0);

    /* 0x28: TSS descriptor (16 bytes) */
    tss->rsp[0]    = kernel_stack_top;
    tss->iomap_base = sizeof(tss_t);
    gdt_set_tss_desc(&gdt->tss, (uintptr_t)tss, sizeof(*tss) - 1);

    gdtr_t gdtr = {
        .limit = sizeof(*gdt) - 1,
        .base  = (uint64_t)gdt,
    };

    __asm__ volatile(
        "lgdt %0\n\t"
        /* reload CS via a far return */
        "pushq %1\n\t"
        "lea  1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* reload data segments */
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : "m"(gdtr),
          "i"((uint64_t)GDT_KERN_CODE),
          "i"((uint16_t)GDT_KERN_DATA)
        : "rax", "memory"
    );

    KLOG_INFO("GDT[%u]: loaded at %p\n", cpu_id, (void *)gdt);
}

void gdt_load_tss(uint32_t cpu_id) {
    uint16_t sel = GDT_TSS_LOW;
    __asm__ volatile("ltr %0" : : "r"(sel));
    KLOG_INFO("TSS[%u]: loaded (sel=0x%x)\n", cpu_id, sel);
}

tss_t *gdt_get_tss(uint32_t cpu_id) {
    return &tss_entries[cpu_id];
}
