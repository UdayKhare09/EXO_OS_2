/* arch/x86_64/ioapic.h — I/O APIC driver for external interrupt routing
 *
 * The I/O APIC routes external device interrupts (PCI, ISA) to LAPIC
 * vectors on specific CPUs.  Without programming the IOAPIC, no external
 * interrupts (including PCI device IRQs) reach the processor.
 */
#pragma once
#include <stdint.h>

/* Initialise the I/O APIC at the given physical MMIO base.
 * `gsi_base` is the Global System Interrupt base for this IOAPIC
 * (reported by ACPI MADT).  Masks all IRQs initially. */
void ioapic_init(uintptr_t base_phys, uint32_t gsi_base);

/* Program an IOAPIC redirection entry:
 *   gsi         – Global System Interrupt number (= IRQ line typically)
 *   vector      – IDT vector to deliver
 *   dest_lapic  – destination LAPIC ID (usually BSP = 0)
 *   flags       – low-32 redirection flags (polarity, trigger, etc.)
 *                 Pass 0 for default (edge-triggered, active-high, fixed). */
void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t dest_lapic, uint32_t flags);

/* Redirection entry flags for ioapic_set_irq(flags) */
#define IOAPIC_FLAG_LEVEL_TRIGGERED  (1u << 15)
#define IOAPIC_FLAG_ACTIVE_LOW       (1u << 13)
/* Convenience: PCI interrupts are level-triggered, active-low */
#define IOAPIC_FLAG_PCI  (IOAPIC_FLAG_LEVEL_TRIGGERED | IOAPIC_FLAG_ACTIVE_LOW)

/* Mask / unmask a single GSI line */
void ioapic_mask_irq(uint8_t gsi);
void ioapic_unmask_irq(uint8_t gsi);

/* Returns true if the IOAPIC has been initialised */
int ioapic_is_init(void);
