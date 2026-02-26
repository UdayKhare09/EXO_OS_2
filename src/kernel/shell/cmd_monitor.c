/* shell/cmd_monitor.c — System monitoring & power shell commands
 *
 * New commands:
 *   lspci     — list PCI devices
 *   lsusb     — list USB devices
 *   lsblk     — list block devices
 *   lsnet     — list network interfaces
 *   lsmem     — memory statistics
 *   lscpu     — CPU / APIC info
 *   dmesg     — kernel log ring buffer
 *   uptime    — time since boot
 *   date      — current date/time (RTC)
 *   acpi-ls   — list ACPI tables
 *   hpet      — HPET timer info
 *   i2cscan   — scan I2C bus
 *   poweroff  — ACPI S5 shutdown
 *   reboot    — ACPI / KBC reset
 *
 * Each command follows the shell_cmd_fn_t signature:
 *   void cmd_xxx(shell_t *sh, const char *args);
 *
 * Registration happens in cmd_monitor_register() called from
 * shell_register_builtins().
 */

#include "shell.h"
#include "gfx/fbcon.h"
#include "lib/string.h"
#include "lib/klog.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/hpet.h"
#include "arch/x86_64/rtc.h"
#include "arch/x86_64/pci.h"
#include "arch/x86_64/smp.h"
#include "drivers/usb/usb_core.h"
#include "drivers/storage/blkdev.h"
#include "drivers/net/netdev.h"
#include "drivers/bus/smbus.h"
#include "drivers/bus/i2c.h"
#include "net/netutil.h"

/* ── ANSI helpers ─────────────────────────────────────────────────────────── */
#define A_RESET  "\033[0m"
#define A_BOLD   "\033[1m"
#define A_GREEN  "\033[1;32m"
#define A_CYAN   "\033[1;36m"
#define A_RED    "\033[1;31m"
#define A_YELLOW "\033[1;33m"
#define A_WHITE  "\033[1;37m"

/* ── PCI class names (subset) ─────────────────────────────────────────────── */
static const char *pci_class_name(uint8_t class, uint8_t subclass) {
    switch (class) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x04: return "RAID controller";
        case 0x05: return "ATA controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "Storage controller";
        }
    case 0x02:
        switch (subclass) {
        case 0x00: return "Ethernet controller";
        case 0x80: return "Network controller";
        default:   return "Network controller";
        }
    case 0x03:
        switch (subclass) {
        case 0x00: return "VGA controller";
        case 0x01: return "XGA controller";
        default:   return "Display controller";
        }
    case 0x04: return "Multimedia device";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-PCI bridge";
        case 0x80: return "Bridge device";
        default:   return "Bridge device";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    default:   return "Unknown";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* lspci — List PCI devices                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_lspci(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    pci_device_t *devs;
    int count = pci_get_devices(&devs);

    fbcon_printf_inst(c, A_BOLD "  PCI devices (%d):" A_RESET "\n", count);
    for (int i = 0; i < count; i++) {
        pci_device_t *d = &devs[i];
        const char *cls = pci_class_name(d->class, d->subclass);
        fbcon_printf_inst(c, "  %02x:%02x.%x  %04x:%04x  [%02x%02x] %s\n",
                          d->bus, d->dev, d->fn,
                          d->vendor_id, d->device_id,
                          d->class, d->subclass,
                          cls);
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* lsusb — List USB devices                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_lsusb(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    int count = usb_device_count();

    fbcon_printf_inst(c, A_BOLD "  USB devices (%d):" A_RESET "\n", count);
    for (int i = 0; i < count; i++) {
        const usb_device_t *d = usb_get_device(i);
        if (!d) continue;
        const char *sp;
        switch (d->speed) {
        case 1:  sp = "Full"; break;
        case 2:  sp = "Low";  break;
        case 3:  sp = "High"; break;
        case 4:  sp = "Super"; break;
        default: sp = "?";    break;
        }
        fbcon_printf_inst(c,
            "  Bus %u Dev %u: ID %04x:%04x  class %02x/%02x/%02x  %s-speed\n",
            0, d->addr & 0xFF,
            d->dev_desc.idVendor, d->dev_desc.idProduct,
            d->iface_class, d->iface_subclass, d->iface_protocol,
            sp);
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* lsblk — List block devices                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_lsblk(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    int count = blkdev_count();

    fbcon_printf_inst(c, A_BOLD "  Block devices (%d):" A_RESET "\n", count);
    fbcon_printf_inst(c, "  %-10s %10s  %10s  %s\n",
                      "NAME", "SECTORS", "SIZE", "TYPE");
    for (int i = 0; i < count; i++) {
        blkdev_t *d = blkdev_get_nth(i);
        if (!d) continue;
        uint64_t bytes = d->block_count * (uint64_t)d->block_size;
        const char *unit;
        uint64_t display;
        if (bytes >= (1ULL << 30)) {
            display = bytes >> 30; unit = "GiB";
        } else if (bytes >= (1ULL << 20)) {
            display = bytes >> 20; unit = "MiB";
        } else if (bytes >= (1ULL << 10)) {
            display = bytes >> 10; unit = "KiB";
        } else {
            display = bytes; unit = "B";
        }
        const char *type = d->part_offset ? "part" : "disk";
        fbcon_printf_inst(c, "  %-10s %10llu  %7llu %s  %s\n",
                          d->name,
                          (unsigned long long)d->block_count,
                          (unsigned long long)display, unit,
                          type);
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* lsnet — List network interfaces                                           */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_lsnet(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    int count = netdev_count();

    fbcon_printf_inst(c, A_BOLD "  Network interfaces (%d):" A_RESET "\n", count);
    for (int i = 0; i < count; i++) {
        netdev_t *d = netdev_get_nth(i);
        if (!d) continue;

        fbcon_printf_inst(c, "  %s: link=%s  mtu=%u\n",
                          d->name,
                          d->link ? A_GREEN "UP" A_RESET : A_RED "DOWN" A_RESET,
                          d->mtu);

        fbcon_printf_inst(c, "    HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n",
                          d->mac[0], d->mac[1], d->mac[2],
                          d->mac[3], d->mac[4], d->mac[5]);

        if (d->ip_addr) {
            uint8_t *ip = (uint8_t *)&d->ip_addr;
            uint8_t *nm = (uint8_t *)&d->netmask;
            uint8_t *gw = (uint8_t *)&d->gateway;
            fbcon_printf_inst(c, "    inet %u.%u.%u.%u  mask %u.%u.%u.%u  gw %u.%u.%u.%u\n",
                              ip[0], ip[1], ip[2], ip[3],
                              nm[0], nm[1], nm[2], nm[3],
                              gw[0], gw[1], gw[2], gw[3]);
        }
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* lsmem — Memory statistics                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_lsmem(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    uint64_t total = pmm_get_total_pages();
    uint64_t free  = pmm_get_free_pages();
    uint64_t used  = total - free;

    uint64_t total_mb = (total * PAGE_SIZE) >> 20;
    uint64_t free_mb  = (free  * PAGE_SIZE) >> 20;
    uint64_t used_mb  = (used  * PAGE_SIZE) >> 20;

    fbcon_printf_inst(c, A_BOLD "  Memory:" A_RESET "\n");
    fbcon_printf_inst(c, "  Total: %llu MiB (%llu pages)\n",
                      (unsigned long long)total_mb, (unsigned long long)total);
    fbcon_printf_inst(c, "  Used:  %llu MiB (%llu pages)\n",
                      (unsigned long long)used_mb,  (unsigned long long)used);
    fbcon_printf_inst(c, "  Free:  %llu MiB (%llu pages)\n",
                      (unsigned long long)free_mb,  (unsigned long long)free);
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* lscpu — CPU information                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_lscpu(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;

    /* Brand string from CPUID leaves 0x80000002..4 */
    char brand[49];
    uint32_t *b = (uint32_t *)brand;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        uint32_t eax, ebx, ecx, edx;
        cpuid(leaf, &eax, &ebx, &ecx, &edx);
        *b++ = eax; *b++ = ebx; *b++ = ecx; *b++ = edx;
    }
    brand[48] = '\0';

    /* Trim leading spaces */
    const char *bp = brand;
    while (*bp == ' ') bp++;

    /* Feature flags */
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    madt_info_t *madt = acpi_get_madt_info();

    fbcon_printf_inst(c, A_BOLD "  CPU:" A_RESET "\n");
    fbcon_printf_inst(c, "  Model:   %s\n", bp);
    fbcon_printf_inst(c, "  Cores:   %u (from MADT)\n", madt->cpu_count);
    fbcon_printf_inst(c, "  BSP ID:  %u\n", cpu_lapic_id());
    fbcon_printf_inst(c, "  Features:");

    if (edx & (1 << 0))  fbcon_puts_inst(c, " fpu");
    if (edx & (1 << 4))  fbcon_puts_inst(c, " tsc");
    if (edx & (1 << 9))  fbcon_puts_inst(c, " apic");
    if (edx & (1 << 25)) fbcon_puts_inst(c, " sse");
    if (edx & (1 << 26)) fbcon_puts_inst(c, " sse2");
    if (ecx & (1 << 0))  fbcon_puts_inst(c, " sse3");
    if (ecx & (1 << 9))  fbcon_puts_inst(c, " ssse3");
    if (ecx & (1 << 19)) fbcon_puts_inst(c, " sse4.1");
    if (ecx & (1 << 20)) fbcon_puts_inst(c, " sse4.2");
    if (ecx & (1 << 28)) fbcon_puts_inst(c, " avx");
    if (ecx & (1 << 25)) fbcon_puts_inst(c, " aes");

    /* Extended features (leaf 7) */
    uint32_t eax7, ebx7, ecx7, edx7;
    cpuid(7, &eax7, &ebx7, &ecx7, &edx7);
    if (ebx7 & (1 << 5))  fbcon_puts_inst(c, " avx2");

    fbcon_putchar_inst(c, '\n');
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* dmesg — Kernel log ring buffer                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_dmesg(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;

    uint32_t total, buf_size;
    const char *ring = klog_get_ring(&total, &buf_size);

    if (total == 0) {
        fbcon_puts_inst(c, "  (empty log)\n\n");
        return;
    }

    /* Determine the range to print */
    uint32_t start, len;
    if (total <= buf_size) {
        start = 0;
        len   = total;
    } else {
        /* Buffer has wrapped; oldest data starts at total % buf_size */
        start = total % buf_size;
        len   = buf_size;
    }

    /* Print character by character from ring */
    for (uint32_t i = 0; i < len; i++) {
        char ch = ring[(start + i) % buf_size];
        if (ch)
            fbcon_putchar_inst(c, ch);
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* uptime — Time since boot                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_uptime(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    uint64_t ticks = sched_get_ticks();
    uint64_t sec   = ticks / 1000;
    uint64_t ms    = ticks % 1000;

    uint64_t hours = sec / 3600;
    uint64_t mins  = (sec % 3600) / 60;
    uint64_t secs  = sec % 60;

    fbcon_printf_inst(c, "  up %llu:%02llu:%02llu.%03llu\n\n",
                      (unsigned long long)hours,
                      (unsigned long long)mins,
                      (unsigned long long)secs,
                      (unsigned long long)ms);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* date — Current date and time from RTC                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_date(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;

    if (!rtc_is_available()) {
        fbcon_puts_inst(c, "  RTC not available\n\n");
        return;
    }

    static const char *wday_name[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *mon_name[] = {
        "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    rtc_time_t t;
    rtc_read_time(&t);

    fbcon_printf_inst(c, "  %s %s %2u %02u:%02u:%02u UTC %u\n\n",
                      wday_name[t.weekday % 7],
                      (t.month >= 1 && t.month <= 12) ? mon_name[t.month] : "???",
                      t.day, t.hour, t.minute, t.second, t.year);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* acpi-ls — List ACPI tables                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_acpi_ls(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;

    acpi_table_entry_t tables[ACPI_MAX_TABLES];
    int count = acpi_list_tables(tables, ACPI_MAX_TABLES);

    fbcon_printf_inst(c, A_BOLD "  ACPI tables (%d):" A_RESET "\n", count);
    fbcon_printf_inst(c, "  %-6s %8s  %s\n", "SIG", "LENGTH", "REV");
    for (int i = 0; i < count; i++) {
        fbcon_printf_inst(c, "  %-6s %8u  %u\n",
                          tables[i].sig, tables[i].length, tables[i].revision);
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* hpet — HPET timer information                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_hpet(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;

    if (!hpet_is_available()) {
        fbcon_puts_inst(c, "  HPET not available\n\n");
        return;
    }

    uint64_t period_fs = hpet_get_period_fs();
    uint64_t freq_mhz  = 1000000000000000ULL / period_fs / 1000000;
    uint64_t counter   = hpet_read();

    fbcon_printf_inst(c, A_BOLD "  HPET:" A_RESET "\n");
    fbcon_printf_inst(c, "  Period:   %llu fs/tick\n", (unsigned long long)period_fs);
    fbcon_printf_inst(c, "  Freq:     ~%llu MHz\n", (unsigned long long)freq_mhz);
    fbcon_printf_inst(c, "  Timers:   %u\n", hpet_get_num_timers());
    fbcon_printf_inst(c, "  Counter:  0x%llx\n\n", (unsigned long long)counter);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* i2cscan — Scan I2C bus for responding devices                             */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_i2cscan(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;

    int nbus = i2c_bus_count();
    if (nbus == 0) {
        fbcon_puts_inst(c, "  No I2C buses registered\n\n");
        return;
    }

    for (int b = 0; b < nbus; b++) {
        i2c_bus_t *bus = i2c_get_bus(b);
        if (!bus) continue;

        fbcon_printf_inst(c, A_BOLD "  I2C bus %d: %s" A_RESET "\n", b, bus->name);
        fbcon_puts_inst(c, "       0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
        for (int row = 0; row < 8; row++) {
            fbcon_printf_inst(c, "  %02x:", row * 16);
            for (int col = 0; col < 16; col++) {
                uint8_t addr = (uint8_t)(row * 16 + col);
                if (addr < 0x03 || addr > 0x77) {
                    fbcon_puts_inst(c, "   ");
                } else if (bus->ops && bus->ops->probe && bus->ops->probe(bus, addr)) {
                    fbcon_printf_inst(c, " %02x", addr);
                } else {
                    fbcon_puts_inst(c, " --");
                }
            }
            fbcon_putchar_inst(c, '\n');
        }
    }
    fbcon_putchar_inst(c, '\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* poweroff — ACPI S5 shutdown                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_poweroff(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    fbcon_puts_inst(c, "  Powering off...\n");
    acpi_shutdown();
    /* If we get here, shutdown failed */
    fbcon_puts_inst(c, A_RED "  ACPI shutdown failed!" A_RESET "\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* reboot — ACPI / KBC / triple-fault reset                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void cmd_reboot(shell_t *sh, const char *args) {
    (void)args;
    fbcon_t *c = sh->con;
    fbcon_puts_inst(c, "  Rebooting...\n");
    acpi_reboot();
    /* If we get here, reboot failed */
    fbcon_puts_inst(c, A_RED "  Reboot failed!" A_RESET "\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Registration                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */
void cmd_monitor_register(void) {
    shell_register_cmd("lspci",    "list PCI devices",              cmd_lspci);
    shell_register_cmd("lsusb",    "list USB devices",              cmd_lsusb);
    shell_register_cmd("lsblk",    "list block devices",            cmd_lsblk);
    shell_register_cmd("lsnet",    "list network interfaces",       cmd_lsnet);
    shell_register_cmd("lsmem",    "memory statistics",             cmd_lsmem);
    shell_register_cmd("lscpu",    "CPU / APIC information",        cmd_lscpu);
    shell_register_cmd("dmesg",    "kernel log ring buffer",        cmd_dmesg);
    shell_register_cmd("uptime",   "time since boot",               cmd_uptime);
    shell_register_cmd("date",     "current date/time (UTC)",       cmd_date);
    shell_register_cmd("acpi-ls",  "list ACPI tables",              cmd_acpi_ls);
    shell_register_cmd("hpet",     "HPET timer information",        cmd_hpet);
    shell_register_cmd("i2cscan",  "scan I2C bus",                  cmd_i2cscan);
    shell_register_cmd("poweroff", "ACPI S5 shutdown",              cmd_poweroff);
    shell_register_cmd("reboot",   "ACPI / KBC reset",              cmd_reboot);
}
