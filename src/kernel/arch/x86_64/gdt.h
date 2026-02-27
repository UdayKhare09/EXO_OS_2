#pragma once
#include <stdint.h>

#define GDT_NULL        0x00
#define GDT_KERN_CODE   0x08
#define GDT_KERN_DATA   0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS_LOW     0x28   /* TSS occupies two slots (16 bytes) */

#define MAX_CPUS        256

/* GDT entry (normal 8-byte descriptor) */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;          /* [7:4] flags, [3:0] limit_high */
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* TSS descriptor is a 16-byte system segment descriptor */
typedef struct {
    uint16_t len;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  flags_lo;
    uint8_t  flags_hi;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) tss_desc_t;

/* Task State Segment (64-bit) */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];        /* RSP0, RSP1, RSP2 */
    uint64_t reserved1;
    uint64_t ist[7];        /* IST1..IST7       */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

/* Per-CPU GDT table (null + 4 normal + TSS 16-byte descriptor = 5*8+16 = 56 bytes) */
typedef struct {
    gdt_entry_t entries[5];   /* null(0x00), k_code(0x08), k_data(0x10), u_data(0x18), u_code(0x20) */
    tss_desc_t  tss;          /* TSS at offset 0x28 = GDT_TSS_LOW */
} __attribute__((packed)) gdt_table_t;

typedef struct {
    uint16_t  limit;
    uint64_t  base;
} __attribute__((packed)) gdtr_t;

void gdt_init(uint32_t cpu_id, uintptr_t kernel_stack_top);
void gdt_load_tss(uint32_t cpu_id);
tss_t *gdt_get_tss(uint32_t cpu_id);

/* Update TSS RSP0 for the given CPU (called on every context switch
 * so ring 3 → ring 0 transitions use the correct kernel stack). */
void gdt_set_tss_rsp0(uint32_t cpu_id, uint64_t rsp0);
