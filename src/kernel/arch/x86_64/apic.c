#include "apic.h"
#include "cpu.h"
#include "hpet.h"
#include "lib/klog.h"
#include "lib/panic.h"
#include "mm/vmm.h"
#include <stdint.h>

/* Virtual address of the mapped LAPIC MMIO page */
static volatile uint32_t *lapic_base = NULL;

/* Ticks-per-millisecond after calibration */
static uint32_t timer_ticks_per_ms = 0;

/* ── LAPIC register read/write ────────────────────────────────────────────── */
static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg >> 2];
}
static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg >> 2] = val;
}
static inline void lapic_wait_icr(void) {
    while (lapic_read(APIC_REG_ICR_LO) & APIC_ICR_PENDING) cpu_pause();
}

/* ── Disable legacy 8259 PIC ──────────────────────────────────────────────── */
void pic_disable(void) {
    /* Remap PIC IRQs to 0x20-0x2F (avoid conflicts with CPU exceptions) */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* Master offset = 0x20 */
    outb(0xA1, 0x28); io_wait();   /* Slave  offset = 0x28 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    /* Mask all IRQs on both PICs */
    outb(0x21, 0xFF); io_wait();
    outb(0xA1, 0xFF); io_wait();
    KLOG_INFO("APIC: legacy PIC disabled\n");
}

/* ── Enable LAPIC via Spurious Vector Register ────────────────────────────── */
void apic_enable(void) {
    lapic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VEC);
}

/* ── BSP init ─────────────────────────────────────────────────────────────── */
void apic_init(uintptr_t lapic_phys_base) {
    if (!cpu_has_apic()) {
        kpanic("APIC: LAPIC not supported by this CPU\n");
    }

    /* Map the LAPIC physical page into the kernel virtual address space */
    lapic_base = (volatile uint32_t *)vmm_mmio_map(lapic_phys_base, 0x1000);

    /* Enable via MSR: set bit 11 (APIC global enable) */
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    msr |= (1 << 11);
    wrmsr(MSR_APIC_BASE, msr);

    pic_disable();
    apic_enable();

    uint32_t ver = lapic_read(APIC_REG_VERSION);
    KLOG_INFO("APIC: BSP LAPIC id=%u ver=0x%x maxlvt=%u base=%p\n",
              apic_get_id(), ver & 0xFF, (ver >> 16) & 0xFF,
              (void *)lapic_phys_base);
}

/* ── AP init (called on each application processor) ──────────────────────── */
void apic_ap_init(void) {
    /* Enable via MSR */
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    msr |= (1 << 11);
    wrmsr(MSR_APIC_BASE, msr);
    apic_enable();
    KLOG_INFO("APIC: AP LAPIC id=%u enabled\n", apic_get_id());
}

/* ── EOI ──────────────────────────────────────────────────────────────────── */
void apic_send_eoi(void) {
    lapic_write(APIC_REG_EOI, 0);
}

/* ── Get current LAPIC ID ─────────────────────────────────────────────────── */
uint8_t apic_get_id(void) {
    return (uint8_t)(lapic_read(APIC_REG_ID) >> 24);
}

/* ── Send generic IPI ─────────────────────────────────────────────────────── */
void apic_send_ipi(uint8_t dest, uint32_t cmd_lo) {
    lapic_wait_icr();
    lapic_write(APIC_REG_ICR_HI, (uint32_t)dest << 24);
    lapic_write(APIC_REG_ICR_LO, cmd_lo);
    lapic_wait_icr();
}

/* ── INIT IPI (reset AP) ──────────────────────────────────────────────────── */
void apic_send_init(uint8_t dest) {
    apic_send_ipi(dest, APIC_IPI_INIT | APIC_ICR_LEVEL_ASSERT);
}

/* ── Startup IPI (SIPI): page = trampoline >> 12 ─────────────────────────── */
void apic_send_sipi(uint8_t dest, uint8_t page) {
    apic_send_ipi(dest, APIC_IPI_STARTUP | page);
}

/* ── PIT-based LAPIC timer calibration ───────────────────────────────────── */
#define PIT_CH2_PORT   0x42
#define PIT_CMD_PORT   0x43
#define PIT_CTRL_PORT  0x61

static void pit_sleep_ms(uint32_t ms) {
    /* Program PIT channel 2 for one-shot count */
    uint32_t count = (1193182 * ms) / 1000;
    if (count > 0xFFFF) count = 0xFFFF;

    uint8_t ctrl = inb(PIT_CTRL_PORT);
    outb(PIT_CTRL_PORT, (ctrl & 0xFD) | 0x01);  /* gate on */
    outb(PIT_CMD_PORT, 0xB2);                    /* ch2, lobyte/hibyte, mode1 */
    outb(PIT_CH2_PORT, (uint8_t)(count & 0xFF));
    outb(PIT_CH2_PORT, (uint8_t)(count >> 8));

    /* Restart */
    ctrl = inb(PIT_CTRL_PORT);
    outb(PIT_CTRL_PORT, ctrl & 0xFE);
    outb(PIT_CTRL_PORT, ctrl | 0x01);

    /* Wait until OUT goes high */
    while (!(inb(PIT_CTRL_PORT) & 0x20));
}

uint32_t apic_timer_calibrate(void) {
    /* mask timer interrupt during calibration */
    lapic_write(APIC_REG_TIMER_LVT,  APIC_TIMER_MASKED);
    lapic_write(APIC_REG_TIMER_DIV,  0x03);   /* divide by 16 */
    lapic_write(APIC_REG_TIMER_INIT, 0xFFFFFFFF);

    /* Use HPET for precision calibration if available, else fall back to PIT */
    if (hpet_is_available()) {
        hpet_sleep_ms(10);
        KLOG_INFO("APIC timer: calibrated via HPET\n");
    } else {
        pit_sleep_ms(10);
        KLOG_INFO("APIC timer: calibrated via PIT\n");
    }

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(APIC_REG_TIMER_CURR);
    timer_ticks_per_ms = elapsed / 10;

    lapic_write(APIC_REG_TIMER_LVT,  APIC_TIMER_MASKED);
    KLOG_INFO("APIC timer: %u ticks/ms\n", timer_ticks_per_ms);
    return timer_ticks_per_ms;
}

/* ── Start periodic timer for scheduler ──────────────────────────────────── */
void apic_timer_init(uint32_t ticks_per_ms) {
    timer_ticks_per_ms = ticks_per_ms;
    lapic_write(APIC_REG_TIMER_DIV,  0x03);           /* divide by 16 */
    lapic_write(APIC_REG_TIMER_LVT,  APIC_TIMER_PERIODIC | APIC_TIMER_VEC);
    lapic_write(APIC_REG_TIMER_INIT, ticks_per_ms);   /* 1 ms tick */
}

/* ── One-shot timer (used by SMP for AP wakeup delays) ───────────────────── */
void apic_timer_oneshot(uint32_t ticks) {
    lapic_write(APIC_REG_TIMER_DIV,  0x03);
    lapic_write(APIC_REG_TIMER_LVT,  APIC_TIMER_ONESHOT | APIC_TIMER_MASKED);
    lapic_write(APIC_REG_TIMER_INIT, ticks);
    while (lapic_read(APIC_REG_TIMER_CURR));
}
