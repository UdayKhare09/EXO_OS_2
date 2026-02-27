#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_CPUS     256
#define CPU_STACK_SIZE  (4096 * 4)   /* 16 KiB per CPU kernel stack */

/* Per-CPU structure — stored in GS base */
typedef struct cpu_info {
    struct cpu_info *self;       /* offset  0: GS:0 = self for fast access    */
    uint32_t         id;         /* offset  8: sequential CPU index           */
    uint8_t          lapic_id;   /* offset 12: hardware LAPIC ID              */
    uint8_t          online;     /* offset 13: 1 = boot complete              */
    uint8_t          _pad[2];    /* offset 14: explicit padding               */
    uintptr_t        kernel_stack_top;  /* offset 16                          */
    uint8_t         *isr_xsave_buf;    /* offset 24: per-CPU 4 KiB XSAVE buf */
    uintptr_t        user_rsp_scratch; /* offset 32: scratch for SYSCALL entry */
} cpu_info_t;

/* Byte offset of isr_xsave_buf in cpu_info_t — referenced by isr.asm */
#define CPU_INFO_ISR_XSAVE_BUF_OFF  24

void smp_init(void);              /* BSP: discover and boot all APs           */
cpu_info_t *smp_self(void);       /* returns current CPU info via GS base     */
uint32_t    smp_cpu_count(void);  /* total number of CPUs brought online      */

/* Called by each AP after trampoline */
void smp_ap_entry(cpu_info_t *info);
