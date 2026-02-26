/* arch/x86_64/hpet.h — High Precision Event Timer
 *
 * Provides nanosecond-precision time source via the HPET main counter.
 * Discovered from the ACPI HPET table; requires acpi_init() first.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialise HPET from the ACPI table.  Returns true on success. */
bool hpet_init(void);

/* Read the 64-bit main counter value */
uint64_t hpet_read(void);

/* Return the counter period in femtoseconds (10^-15 s) */
uint64_t hpet_get_period_fs(void);

/* Return the number of comparators (timers) */
uint8_t hpet_get_num_timers(void);

/* Busy-wait using the HPET counter */
void hpet_sleep_us(uint64_t us);
void hpet_sleep_ms(uint64_t ms);

/* Returns true if HPET was successfully initialized */
bool hpet_is_available(void);
