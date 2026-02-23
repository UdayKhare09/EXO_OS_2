#pragma once
#include <stdint.h>

#define IDT_ENTRIES  256

/* Interrupt/trap gate flags */
#define IDT_INT_GATE    0x8E   /* Present, DPL=0, interrupt gate */
#define IDT_TRAP_GATE   0x8F   /* Present, DPL=0, trap gate      */
#define IDT_USER_GATE   0xEE   /* Present, DPL=3, interrupt gate  */

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          /* bits [2:0] = IST index */
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

/* CPU register state pushed by ISR stubs */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9,  r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec;   /* interrupt vector number */
    uint64_t err;   /* error code (0 if none)  */
    /* pushed by CPU */
    uint64_t rip, cs, rflags, rsp, ss;
} cpu_regs_t;

typedef void (*isr_handler_t)(cpu_regs_t *regs);

void idt_init(void);
void idt_load(void);   /* reload IDTR only — for APs */
void idt_set_handler(uint8_t vec, void *handler, uint8_t flags, uint8_t ist);
void idt_register_handler(uint8_t vec, isr_handler_t fn);

/* ISR stubs defined in isr.asm */
extern void *isr_stub_table[IDT_ENTRIES];
