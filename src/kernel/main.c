/**
 * main.c — EXO_OS kernel entry point
 *
 * Boot sequence:
 *   1.  Serial logging
 *   2.  GDT + IDT on BSP
 *   3.  PMM from Limine memory map
 *   4.  VMM (HHDM offset) + kernel heap
 *   5.  Filesystem stack (VFS, tmpfs, FAT32, ext2)
 *   6.  Input event subsystem
 *   7.  PCI enumeration
 *   8.  Limine GOP framebuffer → fbcon text console
 *   9.  ACPI / MADT
 *  10.  APIC (disable PIC, enable LAPIC, calibrate timer)
 *  11.  SMP (bring up AP cores)
 *  12.  Scheduler
 *  13.  Spawn: init task, USB stack task, storage-init task
 *  14.  Enable interrupts → idle loop
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Limine 8 protocol header */
#include <limine.h>

#include "lib/klog.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/hpet.h"
#include "arch/x86_64/rtc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "arch/x86_64/pci.h"
#include "gfx/fbcon.h"
#include "gfx/font.h"
#include "drivers/input/input.h"
#include "drivers/usb/usb_core.h"
#include "drivers/usb/hid.h"
#include "shell/shell.h"

/* ── Filesystem stack ───────────────────────────────────────────────────── */
#include "fs/bcache.h"
#include "fs/vfs.h"
#include "fs/fat32/fat32.h"
#include "fs/ext2/ext2.h"
#include "fs/tmpfs/tmpfs.h"
#include "drivers/storage/blkdev.h"
#include "drivers/storage/virtio_blk.h"
#include "drivers/storage/ahci.h"
#include "drivers/bus/smbus.h"
#include "drivers/devfs.h"
#include "fs/procfs.h"
#include "fs/gpt.h"
#include "syscall/syscall.h"

/* ── Networking ─────────────────────────────────────────────────────────── */
extern void net_init_task(void *arg);

/* ── Limine protocol: start marker ────────────────────────────────────── */
__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

/* ── Base revision ─────────────────────────────────────────────────────── */
__attribute__((section(".limine_requests")))
LIMINE_BASE_REVISION(3)

/* ── Limine protocol: end marker ───────────────────────────────────────── */
__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ── Limine requests ───────────────────────────────────────────────────── */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_req = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kaddr_req = {
    .id       = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

/* GOP framebuffer — replaces VirtIO GPU */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

/* ── Storage + filesystem initialisation task ───────────────────────────── */
static void storage_init_task(void *arg) {
    (void)arg;
    KLOG_INFO("storage-init: scanning for block devices\n");

    virtio_blk_init();
    ahci_init();

    gpt_partition_t parts[16];
    int found_parts = 0;
    int n = blkdev_count();
    for (int i = 0; i < n; i++) {
        blkdev_t *dev = blkdev_get_nth(i);
        if (!dev) continue;
        if (dev->part_offset) continue;

        KLOG_INFO("storage-init: scanning %s for GPT\n", dev->name);
        int np = gpt_scan(dev, parts + found_parts, 16 - found_parts);
        if (np <= 0) {
            KLOG_WARN("storage-init: no GPT on %s\n", dev->name);
            continue;
        }
        KLOG_INFO("storage-init: %d partition(s) found on %s\n", np, dev->name);
        found_parts += np;
    }

    /* Mount ext2 at / */
    bool root_mounted = false;
    for (int i = 0; i < found_parts; i++) {
        if (!parts[i].blkdev) continue;
        if (gpt_guid_equal(&parts[i].type_guid, &GPT_GUID_LINUX_DATA)) {
            if (vfs_mount("/", parts[i].blkdev, "ext2") == 0) {
                KLOG_INFO("storage-init: ext2 root at / from %s\n", parts[i].blkdev->name);
                root_mounted = true;
                break;
            }
        }
    }
    if (!root_mounted) {
        for (int i = 0; i < found_parts; i++) {
            if (!parts[i].blkdev) continue;
            if (vfs_mount("/", parts[i].blkdev, "ext2") == 0) {
                KLOG_INFO("storage-init: ext2 root at / from %s (fallback)\n", parts[i].blkdev->name);
                root_mounted = true;
                break;
            }
        }
    }
    if (!root_mounted)
        KLOG_WARN("storage-init: no ext2 root filesystem found!\n");

    /* Mount EFI partition (FAT32) at /boot */
    for (int i = 0; i < found_parts; i++) {
        if (!parts[i].blkdev) continue;
        if (gpt_guid_equal(&parts[i].type_guid, &GPT_GUID_EFI_SYSTEM)) {
            if (vfs_mount("/boot", parts[i].blkdev, "fat32") == 0) {
                KLOG_INFO("storage-init: fat32 EFI at /boot from %s\n", parts[i].blkdev->name);
                break;
            }
        }
    }
    KLOG_INFO("storage-init: done\n");
}

/* ── USB + HID init task ────────────────────────────────────────────────── */
static void usb_init_task(void *arg) {
    (void)arg;
    hid_init();
    if (!usb_init())
        KLOG_WARN("usb-init: USB stack unavailable\n");
}

static bool vfs_path_exists(const char *path) {
    int err = 0;
    vnode_t *vn = vfs_lookup(path, true, &err);
    if (!vn) return false;
    vfs_vnode_put(vn);
    return true;
}

static bool read_small_text_file(const char *path, char *buf, size_t buf_sz) {
    if (!buf || buf_sz < 2) return false;
    int err = 0;
    vnode_t *vn = vfs_lookup(path, true, &err);
    if (!vn || !vn->ops || !vn->ops->read) {
        if (vn) vfs_vnode_put(vn);
        return false;
    }
    size_t to_read = vn->size;
    if (to_read >= buf_sz) to_read = buf_sz - 1;
    ssize_t rd = vn->ops->read(vn, buf, to_read, 0);
    vfs_vnode_put(vn);
    if (rd < 0) return false;
    buf[(size_t)rd] = '\0';
    return true;
}

static bool parse_shell_from_environment(const char *text, char *out, size_t out_sz) {
    if (!text || !out || out_sz < 2) return false;
    const char *p = text;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        size_t len = (size_t)(p - line);
        while (*p == '\n' || *p == '\r') p++;

        if (len <= 6 || strncmp(line, "SHELL=", 6) != 0)
            continue;

        const char *val = line + 6;
        size_t vlen = len - 6;
        if (vlen >= 2 &&
            ((val[0] == '"' && val[vlen - 1] == '"') ||
             (val[0] == '\'' && val[vlen - 1] == '\''))) {
            val++;
            vlen -= 2;
        }
        if (!vlen || val[0] != '/')
            continue;
        if (vlen >= out_sz) vlen = out_sz - 1;
        memcpy(out, val, vlen);
        out[vlen] = '\0';
        return true;
    }
    return false;
}

static void pick_user_shell_path(char *out, size_t out_sz) {
    if (!out || out_sz < 2) return;
    strncpy(out, "/bin/sh", out_sz - 1);
    out[out_sz - 1] = '\0';

    char envbuf[512];
    char configured[VFS_MOUNT_PATH_MAX];
    if (read_small_text_file("/etc/environment", envbuf, sizeof(envbuf)) &&
        parse_shell_from_environment(envbuf, configured, sizeof(configured))) {
        strncpy(out, configured, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

/* ── Init task: launch configured user-space shell ──────────────────────── */
static void init_task(void *arg) {
    (void)arg;

    char shell_path[VFS_MOUNT_PATH_MAX];
    pick_user_shell_path(shell_path, sizeof(shell_path));
    KLOG_INFO("init: waiting for shell '%s'...\n", shell_path);

    while (!vfs_path_exists(shell_path)) {
        sched_sleep(10);
        pick_user_shell_path(shell_path, sizeof(shell_path));
    }

    /* Small grace period for USB/HID to come up */
    sched_sleep(200);

    fbcon_t *con = fbcon_get();
    shell_t *launcher = shell_create_quiet(con);
    if (!launcher) {
        KLOG_ERR("init: failed to create launcher shell context\n");
        return;
    }

    extern void cmd_exec_path(shell_t *sh, const char *path);
    for (;;) {
        if (con)
            fbcon_printf_inst(con, "\n  EXO_OS — launching %s\n\n", shell_path);
        cmd_exec_path(launcher, shell_path);
        KLOG_WARN("init: %s exited; restarting in 1s\n", shell_path);
        sched_sleep(1000);
        pick_user_shell_path(shell_path, sizeof(shell_path));
    }
}

/* ── Kernel entry point ─────────────────────────────────────────────────── */
__attribute__((noreturn))
void kmain(void) {
    /* ── 1. Serial / logging ──────────────────────────────────────────────── */
    klog_init();
    KLOG_INFO("\n=== EXO_OS kernel starting ===\n");

    /* ── 2. Validate Limine responses ─────────────────────────────────────── */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        KLOG_ERR("Limine base revision not supported\n");
        goto halt;
    }
    if (!memmap_req.response || !hhdm_req.response || !rsdp_req.response) {
        KLOG_ERR("Missing required Limine responses\n");
        goto halt;
    }

    KLOG_INFO("Limine: HHDM offset  = %p\n",  (void *)hhdm_req.response->offset);
    KLOG_INFO("Limine: kernel virt  = %p  phys = %p\n",
              (void *)kaddr_req.response->virtual_base,
              (void *)kaddr_req.response->physical_base);

    /* ── 3. GDT + IDT on BSP ──────────────────────────────────────────────── */
    extern char kernel_end[];
    uintptr_t bsp_stack = (uintptr_t)kernel_end + 0x4000;
    gdt_init(0, bsp_stack);
    gdt_load_tss(0);
    idt_init();
    KLOG_INFO("GDT + IDT initialised\n");

    /* ── 4. PMM ───────────────────────────────────────────────────────────── */
    struct limine_memmap_response *mm_resp = memmap_req.response;
    pmm_memmap_entry_t entries[512];
    uint64_t entry_count = mm_resp->entry_count;
    if (entry_count > 512) entry_count = 512;
    for (uint64_t i = 0; i < entry_count; i++) {
        entries[i].base   = mm_resp->entries[i]->base;
        entries[i].length = mm_resp->entries[i]->length;
        entries[i].type   = mm_resp->entries[i]->type;
    }
    pmm_init(entries, entry_count, hhdm_req.response->offset);
    pmm_print_stats();

    /* ── 5. VMM ───────────────────────────────────────────────────────────── */
    vmm_init(hhdm_req.response->offset, read_cr3());

    /* ── 5a. Kernel heap ──────────────────────────────────────────────────── */
    kmalloc_init();

    /* ── 5b. Filesystem stack ─────────────────────────────────────────────── */
    bcache_init();
    vfs_init();
    tmpfs_register();
    fat32_register();
    ext2_register();
    if (vfs_mount("/", NULL, "tmpfs") < 0)
        KLOG_WARN("vfs: failed to mount tmpfs as root\n");
    else
        KLOG_INFO("vfs: tmpfs mounted as root\n");
    vfs_mkdir("/dev",  0755);
    vfs_mkdir("/tmp",  01777);
    vfs_mkdir("/boot", 0755);
    vfs_mkdir("/proc", 0555);
    devfs_init();
    procfs_init();
    syscall_init();

    /* ── 5c. Input event subsystem ────────────────────────────────────────── */
    input_init();

    /* ── 5d. PCI enumeration ──────────────────────────────────────────────── */
    static pci_device_t s_pci_buf[64];
    int pci_count = pci_enumerate(s_pci_buf, 64);
    KLOG_INFO("pci: found %d device(s)\n", pci_count);

    /* ── 5e. SMBus / I2C (depends on PCI scan) ───────────────────────────── */
    smbus_init();

    /* ── 6. GOP framebuffer console ───────────────────────────────────────── */
    if (!fb_req.response || fb_req.response->framebuffer_count == 0) {
        KLOG_ERR("gfx: no Limine framebuffer — halting\n");
        goto halt;
    }
    {
        struct limine_framebuffer *lfb = fb_req.response->framebuffers[0];
        KLOG_INFO("gfx: GOP framebuffer %lux%lu bpp=%u pitch=%lu\n",
                  lfb->width, lfb->height, lfb->bpp, lfb->pitch);

        fbcon_fb_t fbdesc = {
            .fb    = (uint32_t *)lfb->address,
            .width  = (uint32_t)lfb->width,
            .height = (uint32_t)lfb->height,
            .pitch  = (uint32_t)lfb->pitch,
        };
        fbcon_init(&fbdesc);
        KLOG_INFO("gfx: fbcon initialised (%ux%u px)\n",
                  fbdesc.width, fbdesc.height);
    }

    /* ── 7. ACPI / MADT ───────────────────────────────────────────────────── */
    acpi_init((uintptr_t)rsdp_req.response->address);

    /* ── 7b. HPET — precision timer (depends on ACPI tables) ─────────────── */
    hpet_init();

    /* ── 7c. RTC — wall-clock time (uses ACPI FADT + HPET if available) ──── */
    rtc_init();

    /* ── 8. APIC ──────────────────────────────────────────────────────────── */
    madt_info_t *madt = acpi_get_madt_info();
    apic_init(madt->lapic_base);
    g_apic_ticks_per_ms = apic_timer_calibrate();
    apic_timer_init(g_apic_ticks_per_ms);

    /* ── 8b. I/O APIC — route external interrupts ────────────────────────── */
    if (madt->ioapic_found) {
        ioapic_init((uintptr_t)madt->ioapic_addr, madt->ioapic_gsi_base);
    } else {
        KLOG_WARN("IOAPIC: not found in MADT — external IRQs unavailable\n");
    }

    /* ── 9. Scheduler init on BSP ─────────────────────────────────────────── */
    sched_init(0);

    /* ── 10. SMP: bring up AP cores ───────────────────────────────────────── */
    smp_init();

    /* ── 11. Spawn tasks ──────────────────────────────────────────────────── */
    /* Register all built-in shell commands before spawning the shell */
    extern void shell_register_builtins(void);
    shell_register_builtins();
    shell_sort_commands();

    sched_spawn("init",         init_task,         NULL);
    sched_spawn("usb-init",     usb_init_task,     NULL);
    sched_spawn("storage-init", storage_init_task, NULL);
    sched_spawn("net-init",     net_init_task,     NULL);

    KLOG_INFO("Kernel init complete. Entering idle.\n");

    /* ── 12. Enable interrupts — scheduler takes over ─────────────────────── */
    cpu_sti();
    sched_idle_loop();

halt:
    cpu_cli();
    for (;;) cpu_halt();
}
