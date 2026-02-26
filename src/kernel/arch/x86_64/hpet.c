/* arch/x86_64/hpet.c — High Precision Event Timer driver
 *
 * Reads the HPET ACPI table, maps the HPET MMIO base, enables the main
 * counter, and exposes a microsecond-precision busy-wait for LAPIC
 * calibration and other timing needs.
 */
#include "hpet.h"
#include "acpi.h"
#include "lib/klog.h"
#include "mm/vmm.h"
#include <stdint.h>
#include <stdbool.h>

/* HPET ACPI table structure */
typedef struct {
    acpi_sdt_hdr_t hdr;
    uint32_t event_timer_block_id;
    /* Generic Address Structure for base address */
    uint8_t  addr_space_id;
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  reserved0;
    uint64_t base_address;
    uint8_t  hpet_number;
    uint16_t minimum_tick;
    uint8_t  page_protection;
} __attribute__((packed)) acpi_hpet_t;

/* HPET MMIO register offsets */
#define HPET_REG_GCAP_ID     0x000   /* General Capabilities and ID      */
#define HPET_REG_GEN_CONF    0x010   /* General Configuration            */
#define HPET_REG_GEN_INT_STS 0x020   /* General Interrupt Status         */
#define HPET_REG_MAIN_CNT    0x0F0   /* Main Counter Value               */

/* GCAP_ID fields */
#define HPET_GCAP_PERIOD_SHIFT   32  /* bits [63:32] = CLK_PERIOD in fs  */
#define HPET_GCAP_NUM_TIM_MASK   0x1F00
#define HPET_GCAP_NUM_TIM_SHIFT  8
#define HPET_GCAP_64BIT          (1 << 13)

/* GEN_CONF bits */
#define HPET_GCONF_ENABLE        (1 << 0)
#define HPET_GCONF_LEGACY_ROUTE  (1 << 1)

/* Module state */
static volatile uint64_t *g_hpet_base = NULL;
static uint64_t           g_hpet_period_fs = 0;  /* femtoseconds per tick */
static uint8_t            g_hpet_num_timers = 0;
static bool               g_hpet_available = false;

static inline uint64_t hpet_mmio_read64(uint32_t offset) {
    return g_hpet_base[offset / 8];
}

static inline void hpet_mmio_write64(uint32_t offset, uint64_t val) {
    g_hpet_base[offset / 8] = val;
}

bool hpet_init(void) {
    /* Look up HPET ACPI table */
    acpi_sdt_hdr_t *hdr = acpi_find_table("HPET", 0);
    if (!hdr) {
        KLOG_WARN("HPET: no ACPI HPET table found\n");
        return false;
    }

    acpi_hpet_t *htbl = (acpi_hpet_t *)hdr;
    uint64_t phys = htbl->base_address;

    if (phys == 0) {
        KLOG_WARN("HPET: base address is zero\n");
        return false;
    }

    KLOG_INFO("HPET: MMIO base phys=%p\n", (void *)phys);

    /* Map HPET MMIO page */
    g_hpet_base = (volatile uint64_t *)vmm_mmio_map((uintptr_t)phys, 4096);
    if (!g_hpet_base) {
        KLOG_ERR("HPET: failed to map MMIO region\n");
        return false;
    }

    /* Read capabilities */
    uint64_t gcap = hpet_mmio_read64(HPET_REG_GCAP_ID);
    g_hpet_period_fs = gcap >> 32;
    g_hpet_num_timers = (uint8_t)(((gcap & HPET_GCAP_NUM_TIM_MASK) >> HPET_GCAP_NUM_TIM_SHIFT) + 1);

    if (g_hpet_period_fs == 0 || g_hpet_period_fs > 100000000ULL) {
        KLOG_ERR("HPET: invalid counter period: %llu fs\n",
                 (unsigned long long)g_hpet_period_fs);
        return false;
    }

    /* Enable the main counter */
    uint64_t gconf = hpet_mmio_read64(HPET_REG_GEN_CONF);
    gconf |= HPET_GCONF_ENABLE;
    gconf &= ~HPET_GCONF_LEGACY_ROUTE;  /* don't remap IRQs 0/8 */
    hpet_mmio_write64(HPET_REG_GEN_CONF, gconf);

    g_hpet_available = true;

    /* Calculate frequency from period */
    uint64_t freq_hz = 1000000000000000ULL / g_hpet_period_fs;

    KLOG_INFO("HPET: period=%llu fs  freq=%llu Hz  timers=%u\n",
              (unsigned long long)g_hpet_period_fs,
              (unsigned long long)freq_hz,
              g_hpet_num_timers);

    return true;
}

uint64_t hpet_read(void) {
    if (!g_hpet_available) return 0;
    return hpet_mmio_read64(HPET_REG_MAIN_CNT);
}

uint64_t hpet_get_period_fs(void) {
    return g_hpet_period_fs;
}

uint8_t hpet_get_num_timers(void) {
    return g_hpet_num_timers;
}

bool hpet_is_available(void) {
    return g_hpet_available;
}

void hpet_sleep_us(uint64_t us) {
    if (!g_hpet_available) return;
    /* Convert microseconds to HPET ticks:
     * ticks = us * 1e9 / period_fs  (1 us = 1e9 fs) */
    uint64_t ticks = (us * 1000000000ULL) / g_hpet_period_fs;
    uint64_t start = hpet_read();
    while ((hpet_read() - start) < ticks)
        __asm__ volatile("pause");
}

void hpet_sleep_ms(uint64_t ms) {
    hpet_sleep_us(ms * 1000);
}
