#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── APIC register offsets (xAPIC, MMIO) ──────────────────────────────────── */
#define APIC_REG_ID           0x020
#define APIC_REG_VERSION      0x030
#define APIC_REG_TPR          0x080   /* Task Priority Register       */
#define APIC_REG_EOI          0x0B0   /* End of Interrupt             */
#define APIC_REG_SVR          0x0F0   /* Spurious Interrupt Vector    */
#define APIC_REG_ICR_LO       0x300   /* Interrupt Command Reg low    */
#define APIC_REG_ICR_HI       0x310   /* Interrupt Command Reg high   */
#define APIC_REG_TIMER_LVT    0x320   /* Timer LVT entry              */
#define APIC_REG_LINT0        0x350
#define APIC_REG_LINT1        0x360
#define APIC_REG_ERROR        0x370
#define APIC_REG_TIMER_INIT   0x380   /* Timer initial count          */
#define APIC_REG_TIMER_CURR   0x390   /* Timer current count          */
#define APIC_REG_TIMER_DIV    0x3E0   /* Timer divide config          */

/* SVR flags */
#define APIC_SVR_ENABLE       (1 << 8)
#define APIC_SPURIOUS_VEC     0xFF

/* Timer LVT modes */
#define APIC_TIMER_ONESHOT    (0 << 17)
#define APIC_TIMER_PERIODIC   (1 << 17)
#define APIC_TIMER_MASKED     (1 << 16)
#define APIC_TIMER_VEC        0x20     /* IRQ vector for APIC timer    */

/* IPI delivery modes */
#define APIC_IPI_INIT         0x00500
#define APIC_IPI_STARTUP      0x00600
#define APIC_IPI_FIXED        0x00000

/* ICR flags */
#define APIC_ICR_LEVEL_ASSERT (1 << 14)
#define APIC_ICR_PENDING      (1 << 12)

void  apic_init(uintptr_t lapic_phys_base);  /* call once on BSP       */
void  apic_ap_init(void);                    /* call on each AP        */
void  apic_enable(void);
void  apic_send_eoi(void);
void  apic_send_ipi(uint8_t dest_lapic_id, uint32_t cmd_lo);
void  apic_send_init(uint8_t dest);
void  apic_send_sipi(uint8_t dest, uint8_t page);
void  apic_timer_init(uint32_t ticks_per_ms);
void  apic_timer_oneshot(uint32_t ticks);
uint8_t apic_get_id(void);

/* Calibrate LAPIC timer.  Returns ticks per millisecond. */
uint32_t apic_timer_calibrate(void);

/* Disable legacy 8259 PIC */
void pic_disable(void);
