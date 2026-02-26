Plan: EXO_OS Kernel Modernization
This is a large multi-phase effort. Each phase is independently compilable and testable. The work touches ~30 files and creates ~20 new ones.

TL;DR — Introduce a global shell command registration system, integrate ACPICA for complete ACPI (shutdown/reboot/power events), add HPET as a precision time source for LAPIC calibration, implement SMBus/I2C against the ICH9 controller already present on the PCI bus, build a VT-d IOMMU layer with per-device isolation domains, then wire all existing and new kernel subsystems into a suite of inspection commands (lspci, lsusb, lsblk, lsmem, lscpu, dmesg, uptime, poweroff, reboot, etc.).

Phase 1 — Shell Command Registration System
Why first: Every later phase adds commands. The monolithic if/else chain in run_line() cannot scale; it must be replaced before adding ~15 new commands.

Steps

Add to shell/shell.h:

typedef void (*shell_cmd_fn_t)(shell_t *sh, const char *args);
typedef struct { const char *name; const char *help; shell_cmd_fn_t fn; } shell_cmd_t;
void shell_register_cmd(const char *name, const char *help, shell_cmd_fn_t fn);
Add to shell/shell.c:

A global shell_cmd_t g_cmds[128] array + int g_cmd_count counter protected by a spinlock
shell_register_cmd() implementation — appends to the table, asserts no duplicate names
Replace the entire run_line() body with a binary search (sort table lexicographically after all registrations) → call cmd->fn(sh, args)
cmd_help() iterates g_cmds[] instead of a hardcoded string
Convert every existing command (cmd_ifconfig, cmd_ping, cmd_ls, etc.) to the new signature — wrap each with a static trampoline that unpacks sh vs fbcon as needed

Call shell_register_cmd() for all existing commands at the top of shell.c's init section or from shell_task() before the REPL loop starts

Each subsequent phase registers its commands from its own init function, before the shell task blocks waiting for input

Verification: All existing commands still work; help output matches the old list; a command registered from net_init_task() is visible in the shell by the time the user types it

Phase 2 — ACPICA Integration
Context: ACPICA (Intel/ACPICA project, BSD licensed) is the upstream AML interpreter used by Linux, FreeBSD, and UEFI. It requires implementing ~50 OS-layer callbacks (AcpiOs* functions) that bridge ACPICA's internal calls to our kernel primitives. This is the biggest single item.

New files/directories

src/kernel/3rdparty/acpica/ — vendored ACPICA source (from https://github.com/acpica/acpica). Only the source/components/ subdirectories are needed: debugger/ (optional, skip), disassembler/ (skip), events/, executer/, hardware/, namespace/, parser/, resources/, tables/, utilities/. Plus source/include/

src/kernel/arch/x86_64/acpi_osl.c — OS Services Layer. Implements every AcpiOs* function:

AcpiOsMapMemory(phys, len) → vmm_mmio_map(phys, len)
AcpiOsUnmapMemory(virt, len) → vmm_unmap_page() loop
AcpiOsAllocate(size) → kmalloc(size)
AcpiOsFree(ptr) → kfree(ptr)
AcpiOsReadMemory/WriteMemory — MMIO byte/word/dword
AcpiOsReadPort/WritePort — inb/inw/inl, outb/outw/outl
AcpiOsCreateMutex/AcquireMutex/ReleaseMutex → spinlock_t wrappers
AcpiOsCreateSemaphore/WaitSemaphore/SignalSemaphore → waitq_t wrappers
AcpiOsExecute(type, fn, ctx) → sched_spawn() wrapper
AcpiOsSleep(ms) → sched_sleep(ms)
AcpiOsStall(us) → busy-wait loop
AcpiOsGetTimer() → sched_get_ticks() * 10000 (100-ns units as ACPICA expects)
AcpiOsPrintf/VPrintf → klog_write() wrapper
AcpiOsInstallInterruptHandler → idt_register_handler()
AcpiOsReadPciConfiguration/WritePciConfiguration → pci_read32/write32()
AcpiOsGetRootPointer() → returns the RSDP physical address from Limine
src/kernel/arch/x86_64/acpi_osl.h — declares acpica_osl_init(uintptr_t rsdp_phys) which sets the RSDP for AcpiOsGetRootPointer()

acpi.c (modify) — after existing acpi_init():

Call AcpiInitializeSubsystem()
Call AcpiInitializeTables(NULL, 16, FALSE)
Call AcpiLoadTables()
Call AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION)
Call AcpiInitializeObjects(ACPI_FULL_INITIALIZATION)
Expose acpi_shutdown() → evaluate \_S5 via AcpiEvaluateObject, extract SLP_TYPa/b, write PM1a_CNTx via AcpiHwWritePm1Control() or direct FADT register access
Expose acpi_reboot() → AcpiReset() (wraps FADT reset register); fallback: keyboard controller 0x64 port 0xFE, fallback: lidt 0; int 0 triple fault
Expose acpi_find_table(sig, idx, header_out) → AcpiGetTable() wrapper
Expose acpi_list_tables() returning an array of 4-char signatures for the acpi-ls shell command
acpi.h (modify) — add:

main.c — replace existing acpi_init(rsdp) call with new sequence that also invokes ACPICA full initialization

Build system (makefile) — add src/kernel/3rdparty/acpica/**/*.c to the source list; add -Isrc/kernel/3rdparty/acpica/source/include to CFLAGS; add -DACPI_USE_STANDARD_HEADERS=0 -DACPI_USE_LOCAL_CACHE to disable stdlib deps

Verification: poweroff and reboot commands work in QEMU; acpi-ls shows FACP, MADT, HPET, DMAR, etc.

Phase 3 — HPET (High Precision Event Timer)
New files

src/kernel/arch/x86_64/hpet.c / hpet.h
Steps

In hpet_init():

Call acpi_find_table("HPET") to get the HPET ACPI table
Extract BaseAddress.Address (64-bit MMIO address)
Call vmm_mmio_map(phys, 4096) to get a virtual pointer g_hpet_base
Read GCAP_ID register (offset 0x000) → extract COUNTER_CLK_PERIOD (femtoseconds per tick) and NUM_TIM_CAP (number of comparators)
Enable main counter: set bit 0 of GEN_CONF register (offset 0x010)
Store period in g_hpet_period_fs
Expose:

Modify arch/x86_64/apic.c — apic_timer_calibrate():

If hpet_init() succeeded earlier, use hpet_sleep_us(1000) for the 1 ms reference interval instead of whatever delay loop it currently uses
This makes LAPIC calibration accurate to HPET precision (~nanoseconds)
In main.c — call hpet_init() after ACPICA is up (before apic_timer_calibrate())

Verification: sysinfo APIC timer value is stable across reboots; hpet shell command prints counter value and period

Phase 4 — SMBus / I2C
Context: PCI device 8086:2930, class=0c/05 at 00:1f.3 is the ICH9 SMBus controller, already visible in the PCI enumeration log. I2C bus devices (e.g., DDR SPD EEPROMs at addresses 0x50–0x57) sit on this bus.

New files

src/kernel/drivers/bus/smbus.c / smbus.h
Steps

In smbus_init():

Call pci_find(0x8086, 0x2930) (already in scanned table)
Call pci_enable_device()
Read BAR4 for I/O base address (SMBBASE)
Enable SMBus via PCI command register and SMBUS host config register offset 0x40
Implement the ICH9 SMBus protocol registers (I/O port offsets from SMBBASE):

HSTS (+0x00) — Host Status
HCNT (+0x02) — Host Control
HCMD (+0x03) — Host Command
XMIT_SLWA (+0x04) — Transmit Slave Address
HDAT0/1 (+0x05/+0x06) — Data registers
Expose:

Expose a generic I2C interface header src/kernel/drivers/bus/i2c.h:

SMBus registers itself as an I2C bus named "smbus0"

Add smbus_probe_spd() in smbus.c — scans addresses 0x50–0x57 to detect DDR SPD EEPROMs, reads first 4 bytes (type, capacity, speed), fills a spd_info_t[] used by lsmem

Call smbus_init() from main.c during init (after PCI enumeration, before spawning tasks)

Verification: i2c-scan shell command shows detected devices; on QEMU -m 2G with i2c-smbus device, SPD slots appear in lsmem

Phase 6 — Monitoring Commands
All commands register via shell_register_cmd() from the relevant subsystem's init function. Two new source files hold the command implementations:

src/kernel/shell/cmd_sysfs.c — system info commands (lspci, lsusb, lsmem, lscpu, lsblk, lsnet, dmesg, uptime, acpi-ls, iommu)
src/kernel/shell/cmd_power.c — poweroff, reboot
Required API additions before commands can be written:

API gap	Location	Addition
USB device enumeration	drivers/usb/usb_core.h	int usb_device_count(void) + usb_device_t *usb_get_device_nth(int n) — return struct with slot_id, VID, PID, class, speed, port, attached driver name
klog ring buffer	lib/klog.c / klog.h	Add a 64 KiB ring buffer alongside serial output; expose klog_read(char *buf, size_t max) → returns bytes copied
PCI vendor/class decode	New file: arch/x86_64/pci_ids.h	Lookup tables for common vendor names + class/subclass strings (100–200 entries, static arrays)
Commands:

Command	Data source	Output
lspci [-v]	pci_get_devices() + pci_ids.h	00:04.0 Virtio network device [1af4:1000]; -v adds BARs, IRQ, capabilities
lsusb	usb_device_count() + usb_get_device_nth()	Bus 1 Device 1: ID 0627:0001 class=03 HID (hid-boot)
lsblk	blkdev_get_nth() loop	Tree: vda  256M → ├─vda1 64M EFI / └─vda2 191M Linux
lsnet	netdev_get_nth() loop	Each netdev_t: name, MAC, IP, mask, GW, link state, RX/TX byte counters
lsmem	pmm_get_total/free_pages() + SPD from smbus_probe_spd()	Memory regions table; total/free/used in MB; DIMM info if SMBus available
lscpu	CPUID (extend existing sysinfo data)	Vendor, brand, physical/logical cores, cache topology (L1/L2/L3 via CPUID leaf 4), feature flags (SSE/AVX/etc), LAPIC ticks/ms
dmesg [-n N]	klog_read()	Ring buffer dump; -n N shows last N lines
uptime	sched_get_ticks() → convert to d:hh:mm:ss	up 0:00:03:12
acpi-ls	acpi_list_tables()	Table of ACPI signatures: FACP MADT HPET DMAR SSDT…
iommu	iommu_is_enabled(), DRHD list	Unit list, domain count per unit, device→domain mapping
hpet	hpet_read(), hpet_get_period_fs()	Counter value, period, estimated frequency
i2c-scan	smbus_read_byte() probe 0x00–0x7F	Grid of detected I2C addresses
poweroff	acpi_shutdown()	Graceful ACPI S5
reboot	acpi_reboot()	FADT reset register → KBC fallback
Existing commands (ping, tcpconn, ifconfig, etc.) are kept; ifconfig → optionally kept as alias of lsnet.

Phase Ordering & File Summary
Strict dependency order: Phase 1 → Phase 2 → Phase 3 → Phase 4 → Phase 5 → Phase 6

New files created (~20):

src/kernel/3rdparty/acpica/ (vendored, ~80 .c files)
src/kernel/arch/x86_64/acpi_osl.c / acpi_osl.h
src/kernel/arch/x86_64/hpet.c / hpet.h
src/kernel/arch/x86_64/iommu.c / iommu.h
src/kernel/arch/x86_64/pci_ids.h
src/kernel/drivers/bus/smbus.c / smbus.h
src/kernel/drivers/bus/i2c.h
src/kernel/shell/cmd_sysfs.c
src/kernel/shell/cmd_power.c
Modified files (~12):

acpi.c + acpi.h — ACPICA init + new APIs
apic.c — HPET-based calibration
virtio_net.c — IOMMU domain attachment
virtio_blk.c — IOMMU domain attachment
xhci.c — IOMMU domain attachment
usb_core.c + usb_core.h — enumeration API
klog.c + klog.h — ring buffer
shell.c + shell.h — registration system
main.c — init sequence additions
makefile — ACPICA source glob + include path
Verification (end-to-end)

After all phases:

exo:/> lspci
00:00.0  Host bridge           Intel 82G33/G31/P35/P31 Express [8086:29c0]
00:01.0  VGA compatible        QEMU VGA [1234:1111]
00:02.0  SCSI storage ctrl     Virtio block device [1af4:1001]
...

exo:/> lsusb
Bus 1 Device 1: ID 0627:0001  HID (hid-boot) keyboard  port=5
Bus 1 Device 2: ID 0627:0001  HID (hid-boot) mouse     port=6

exo:/> lsmem
Total:  2047 MiB   Free: 1991 MiB   Used: 56 MiB
DIMM0: 2048 MiB DDR4  (via SMBus SPD if available)

exo:/> poweroff       (→ QEMU exits)
exo:/> reboot         (→ QEMU restarts)



Start implementation, scrap iommu and add rtc(or a modern take)