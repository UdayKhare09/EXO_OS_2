/* arch/x86_64/ioapic.c — I/O APIC programming
 *
 * Routes external interrupts (PCI legacy, ISA) through the I/O APIC
 * redirection table to LAPIC vectors.
 *
 * Register access is via indirect I/O:
 *   IOREGSEL (offset 0x00) – write register index
 *   IOWIN    (offset 0x10) – read/write data at selected register
 */
#include "ioapic.h"
#include "mm/vmm.h"
#include "lib/klog.h"

/* ── IOAPIC registers (indices into IOREGSEL) ───────────────────────────── */
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDTBL   0x10   /* each entry = 2 × 32-bit regs */

/* Redirection entry low-word bits */
#define IOAPIC_MASKED       (1u << 16)

/* ── State ───────────────────────────────────────────────────────────────── */
static volatile uint32_t *g_ioapic_base = NULL;
static uint32_t           g_gsi_base    = 0;
static uint32_t           g_max_irqs    = 0;

/* ── Register access ─────────────────────────────────────────────────────── */
static inline uint32_t ioapic_read(uint32_t reg) {
    g_ioapic_base[0]          = reg;          /* IOREGSEL at offset 0x00 */
    return g_ioapic_base[0x10 / 4];           /* IOWIN    at offset 0x10 */
}

static inline void ioapic_write(uint32_t reg, uint32_t val) {
    g_ioapic_base[0]          = reg;
    g_ioapic_base[0x10 / 4]   = val;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void ioapic_init(uintptr_t base_phys, uint32_t gsi_base) {
    g_ioapic_base = (volatile uint32_t *)vmm_mmio_map(base_phys, 0x1000);
    g_gsi_base    = gsi_base;

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    g_max_irqs   = ((ver >> 16) & 0xFF) + 1;

    KLOG_INFO("IOAPIC: mapped %p  %u IRQs  GSI base %u\n",
              (void *)g_ioapic_base, g_max_irqs, g_gsi_base);

    /* Mask every entry to start clean */
    for (uint32_t i = 0; i < g_max_irqs; i++) {
        uint32_t reg_lo = IOAPIC_REG_REDTBL + i * 2;
        uint32_t lo = ioapic_read(reg_lo);
        ioapic_write(reg_lo, lo | IOAPIC_MASKED);
    }
}

int ioapic_is_init(void) {
    return g_ioapic_base != NULL;
}

void ioapic_set_irq(uint8_t gsi, uint8_t vector,
                     uint8_t dest_lapic, uint32_t flags) {
    if (!g_ioapic_base) return;
    if (gsi < g_gsi_base) return;

    uint32_t idx = gsi - g_gsi_base;
    if (idx >= g_max_irqs) return;

    uint32_t reg_lo = IOAPIC_REG_REDTBL + idx * 2;
    uint32_t reg_hi = reg_lo + 1;

    /* Low 32 bits: vector + delivery mode / polarity / trigger from flags.
     * Default flags = 0 → edge-triggered, active-high, fixed delivery.
     * NOT masked (bit 16 clear) so the IRQ is enabled immediately. */
    uint32_t lo = (uint32_t)vector | flags;

    /* High 32 bits: destination LAPIC ID in bits [31:24] */
    uint32_t hi = (uint32_t)dest_lapic << 24;

    ioapic_write(reg_hi, hi);
    ioapic_write(reg_lo, lo);

    KLOG_INFO("IOAPIC: GSI %u → vector 0x%02x  dest LAPIC %u\n",
              gsi, vector, dest_lapic);
}

void ioapic_mask_irq(uint8_t gsi) {
    if (!g_ioapic_base) return;
    if (gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base;
    if (idx >= g_max_irqs) return;

    uint32_t reg = IOAPIC_REG_REDTBL + idx * 2;
    uint32_t lo = ioapic_read(reg);
    ioapic_write(reg, lo | IOAPIC_MASKED);
}

void ioapic_unmask_irq(uint8_t gsi) {
    if (!g_ioapic_base) return;
    if (gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base;
    if (idx >= g_max_irqs) return;

    uint32_t reg = IOAPIC_REG_REDTBL + idx * 2;
    uint32_t lo = ioapic_read(reg);
    ioapic_write(reg, lo & ~IOAPIC_MASKED);
}
