#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_CPUS     256
#define CPU_STACK_SIZE  (4096 * 4)   /* 16 KiB per CPU kernel stack */

/* Per-CPU structure — stored in GS base */
typedef struct cpu_info {
    struct cpu_info *self;       /* pointer to self (for GS-relative access)  */
    uint32_t         id;         /* sequential CPU index (0 = BSP)             */
    uint8_t          lapic_id;   /* hardware LAPIC ID                          */
    uint8_t          online;     /* 1 = boot complete                          */
    uintptr_t        kernel_stack_top;
} cpu_info_t;

void smp_init(void);              /* BSP: discover and boot all APs           */
cpu_info_t *smp_self(void);       /* returns current CPU info via GS base     */
uint32_t    smp_cpu_count(void);  /* total number of CPUs brought online      */

/* Called by each AP after trampoline */
void smp_ap_entry(cpu_info_t *info);
