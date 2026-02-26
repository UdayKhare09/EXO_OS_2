/* arch/x86_64/rtc.h — Real-Time Clock driver
 *
 * Modern RTC implementation for x86_64:
 *   - Reads date/time from CMOS RTC (MC146818-compatible, ports 0x70/0x71)
 *   - Uses ACPI FADT century register when available
 *   - Optionally augments with HPET for sub-second precision
 *   - Maintains a monotonic wall-clock via periodic tick updates
 *
 * All times are in UTC.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Broken-down time structure (UTC) */
typedef struct {
    uint8_t  second;    /* 0-59 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  day;       /* 1-31 */
    uint8_t  month;     /* 1-12 */
    uint16_t year;      /* full year, e.g. 2026 */
    uint8_t  weekday;   /* 0=Sunday, 1=Monday, ... 6=Saturday */
} rtc_time_t;

/* Unix-epoch timestamp (seconds since 1970-01-01 00:00:00 UTC) */
typedef int64_t rtc_epoch_t;

/* Initialise the RTC driver.  Reads and caches the current time. */
void rtc_init(void);

/* Read the current CMOS RTC time into *t.  This queries hardware directly. */
void rtc_read_time(rtc_time_t *t);

/* Get the current time as a Unix epoch timestamp. */
rtc_epoch_t rtc_get_epoch(void);

/* Get a cached copy of the last-read time (fast, no I/O). */
void rtc_get_time(rtc_time_t *t);

/* Format current time as "YYYY-MM-DD HH:MM:SS" into buf (at least 20 bytes). */
void rtc_format_time(char *buf, size_t bufsz);

/* Returns true if the RTC has been successfully initialised. */
bool rtc_is_available(void);
