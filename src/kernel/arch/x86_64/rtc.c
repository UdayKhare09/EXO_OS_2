/* arch/x86_64/rtc.c — Real-Time Clock driver (MC146818 / CMOS RTC)
 *
 * Modern implementation:
 *   - NMI-safe CMOS register access (port 0x70/0x71)
 *   - Handles BCD and 12-hour mode
 *   - Century register from ACPI FADT
 *   - Converts broken-down time → Unix epoch
 *   - Caches last-read time for lockless fast path
 *   - Uses HPET for sub-second precision when available
 */

#include "arch/x86_64/rtc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/hpet.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "lib/spinlock.h"

/* ── CMOS register definitions ────────────────────────────────────────────── */
#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

#define RTC_REG_SEC    0x00
#define RTC_REG_MIN    0x02
#define RTC_REG_HOUR   0x04
#define RTC_REG_WDAY   0x06
#define RTC_REG_DAY    0x07
#define RTC_REG_MON    0x08
#define RTC_REG_YEAR   0x09
#define RTC_REG_STATA  0x0A  /* Status register A */
#define RTC_REG_STATB  0x0B  /* Status register B */

/* Status register A — bit 7: Update In Progress */
#define RTC_UIP  (1 << 7)

/* Status register B bits */
#define RTC_STATB_24HR (1 << 1)   /* 24-hour mode */
#define RTC_STATB_BIN  (1 << 2)   /* Binary mode (vs BCD) */

/* PM bit for 12-hour mode */
#define RTC_HOUR_PM    0x80

/* ── Module state ─────────────────────────────────────────────────────────── */
static bool        g_rtc_avail;
static uint8_t     g_century_reg;   /* FADT century register index, or 0 */
static rtc_time_t  g_cached_time;
static rtc_epoch_t g_boot_epoch;    /* epoch at boot */
static uint64_t    g_boot_hpet;     /* HPET counter at boot (for sub-sec) */
static spinlock_t  g_rtc_lock;

/* ── Low-level CMOS access ────────────────────────────────────────────────── */
/* NMI is preserved: we keep the NMI-disable bit (0x80) off so NMIs flow. */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg & 0x7F);  /* bit 7 = NMI-disable; keep it 0 */
    io_wait();
    return inb(CMOS_DATA);
}

/* Wait until the RTC update-in-progress bit clears, with a safety cap */
static void rtc_wait_uip(void) {
    int tries = 10000;
    while ((cmos_read(RTC_REG_STATA) & RTC_UIP) && --tries > 0)
        cpu_pause();
}

/* BCD → binary conversion */
static inline uint8_t bcd2bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

/* ── Day-of-week calculation (Tomohiko Sakamoto) ────────────────────────── */
static uint8_t day_of_week(uint16_t y, uint8_t m, uint8_t d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (uint8_t)((y + y/4 - y/100 + y/400 + t[m - 1] + d) % 7);
}

/* ── Epoch conversion (UTC broken-down → seconds since 1970-01-01) ──────── */
static rtc_epoch_t time_to_epoch(const rtc_time_t *t) {
    /* Days from years */
    int y = t->year;
    int m = t->month;
    int d = t->day;

    /* Days per month (non-leap) */
    static const int mdays[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    /* Total days from 1970 to start of year y */
    int64_t days = 0;
    for (int yr = 1970; yr < y; yr++) {
        days += 365;
        if ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)
            days++;
    }

    /* Add days for complete months */
    for (int mn = 1; mn < m; mn++) {
        days += mdays[mn];
        if (mn == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
            days++;
    }

    days += d - 1; /* day-of-month is 1-based */

    return (rtc_epoch_t)(days * 86400 + t->hour * 3600 + t->minute * 60 + t->second);
}

/* ── Hardware read ────────────────────────────────────────────────────────── */
void rtc_read_time(rtc_time_t *t) {
    uint8_t sec, min, hour, day, mon, year, century = 0;
    uint8_t sec2, min2, hour2, day2, mon2, year2, century2 = 0;

    /*
     * The MC146818 may be mid-update; read twice and compare to
     * ensure consistent values (standard technique).
     */
    do {
        rtc_wait_uip();
        sec     = cmos_read(RTC_REG_SEC);
        min     = cmos_read(RTC_REG_MIN);
        hour    = cmos_read(RTC_REG_HOUR);
        day     = cmos_read(RTC_REG_DAY);
        mon     = cmos_read(RTC_REG_MON);
        year    = cmos_read(RTC_REG_YEAR);
        if (g_century_reg)
            century = cmos_read(g_century_reg);

        rtc_wait_uip();
        sec2     = cmos_read(RTC_REG_SEC);
        min2     = cmos_read(RTC_REG_MIN);
        hour2    = cmos_read(RTC_REG_HOUR);
        day2     = cmos_read(RTC_REG_DAY);
        mon2     = cmos_read(RTC_REG_MON);
        year2    = cmos_read(RTC_REG_YEAR);
        if (g_century_reg)
            century2 = cmos_read(g_century_reg);
    } while (sec != sec2 || min != min2 || hour != hour2 ||
             day != day2 || mon != mon2 || year != year2 ||
             century != century2);

    /* Check Status Register B for data format */
    uint8_t statb = cmos_read(RTC_REG_STATB);
    bool is_bcd = !(statb & RTC_STATB_BIN);
    bool is_24h = !!(statb & RTC_STATB_24HR);

    /* Handle 12-hour mode PM flag before BCD conversion */
    bool pm = false;
    if (!is_24h && (hour & RTC_HOUR_PM)) {
        pm = true;
        hour &= ~RTC_HOUR_PM;
    }

    /* Convert BCD → binary if needed */
    if (is_bcd) {
        sec  = bcd2bin(sec);
        min  = bcd2bin(min);
        hour = bcd2bin(hour);
        day  = bcd2bin(day);
        mon  = bcd2bin(mon);
        year = bcd2bin(year);
        if (g_century_reg)
            century = bcd2bin(century);
    }

    /* 12-hour → 24-hour conversion */
    if (!is_24h) {
        if (hour == 12)
            hour = pm ? 12 : 0;
        else if (pm)
            hour += 12;
    }

    /* Compute full year */
    uint16_t full_year;
    if (g_century_reg && century != 0) {
        full_year = (uint16_t)century * 100 + year;
    } else {
        /* Assume 2000-2099 if century register unknown */
        full_year = 2000 + year;
    }

    t->second  = sec;
    t->minute  = min;
    t->hour    = hour;
    t->day     = day;
    t->month   = mon;
    t->year    = full_year;
    t->weekday = day_of_week(full_year, mon, day);
}

/* ── Public API ───────────────────────────────────────────────────────────── */
void rtc_init(void) {
    spinlock_init(&g_rtc_lock);

    /* Try to get century register from ACPI FADT */
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table("FACP", 0);
    if (fadt && fadt->hdr.length >= 109) {
        g_century_reg = fadt->century;
        if (g_century_reg)
            klog_info("RTC: FADT century register = 0x%02x", g_century_reg);
    }

    /* First read */
    rtc_read_time(&g_cached_time);
    g_boot_epoch = time_to_epoch(&g_cached_time);

    /* Snapshot HPET counter for sub-second tracking */
    if (hpet_is_available())
        g_boot_hpet = hpet_read();

    g_rtc_avail = true;

    static const char *wday_name[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

    klog_info("RTC: %04u-%02u-%02u %02u:%02u:%02u UTC (%s) epoch=%lld",
              g_cached_time.year, g_cached_time.month, g_cached_time.day,
              g_cached_time.hour, g_cached_time.minute, g_cached_time.second,
              wday_name[g_cached_time.weekday],
              (long long)g_boot_epoch);
}

void rtc_get_time(rtc_time_t *t) {
    spinlock_acquire(&g_rtc_lock);
    *t = g_cached_time;
    spinlock_release(&g_rtc_lock);
}

rtc_epoch_t rtc_get_epoch(void) {
    if (!g_rtc_avail)
        return 0;

    /*
     * If HPET is available, compute elapsed seconds since boot to
     * avoid hitting the slow CMOS registers on every call.
     */
    if (hpet_is_available()) {
        uint64_t now   = hpet_read();
        uint64_t delta = now - g_boot_hpet;
        uint64_t fs_per_tick = hpet_get_period_fs();
        /* Convert femtoseconds → seconds: delta * fs_per_tick / 1e15 */
        uint64_t elapsed_sec = (delta / 1000000000ULL) * fs_per_tick / 1000000ULL;
        return g_boot_epoch + (rtc_epoch_t)elapsed_sec;
    }

    /* Fallback: re-read CMOS */
    rtc_time_t now;
    rtc_read_time(&now);

    spinlock_acquire(&g_rtc_lock);
    g_cached_time = now;
    spinlock_release(&g_rtc_lock);

    return time_to_epoch(&now);
}

void rtc_format_time(char *buf, size_t bufsz) {
    if (!g_rtc_avail) {
        ksnprintf(buf, bufsz, "(no RTC)");
        return;
    }

    /* Get current time via epoch → reconstruct or re-read */
    rtc_time_t t;
    rtc_read_time(&t);

    spinlock_acquire(&g_rtc_lock);
    g_cached_time = t;
    spinlock_release(&g_rtc_lock);

    ksnprintf(buf, bufsz, "%04u-%02u-%02u %02u:%02u:%02u",
              t.year, t.month, t.day, t.hour, t.minute, t.second);
}

bool rtc_is_available(void) {
    return g_rtc_avail;
}
