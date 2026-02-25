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
 *  13.  Spawn: shell task, USB stack task, storage-init task
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
#include "fs/gpt.h"
#include "syscall/syscall.h"

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

/* ── Shell task ─────────────────────────────────────────────────────────── */
/* Reads from the global input ring and feeds characters to the shell.
 *
 * CRITICAL: sched_sleep(1) is called on EVERY iteration — not just when
 * the ring is empty.  A busy-loop here starves the "usb-evt" task, which
 * is the only task that calls hid_transfer_done() → input_push_key().
 * Without yielding every ms, no further HID events reach the ring.        */
static void shell_task(void *arg) {
    (void)arg;

    fbcon_t *con = fbcon_get();
    if (!con) {
        KLOG_ERR("shell: no framebuffer console — bailing\n");
        return;
    }

    shell_t *sh = shell_create(con);
    if (!sh) {
        KLOG_ERR("shell: failed to allocate shell instance\n");
        return;
    }

    KLOG_INFO("shell: running (USB HID keyboard input)\n");

    uint64_t next_blink = sched_get_ticks() + 500;  /* first blink in 500 ms */

    for (;;) {
        /* Drain all pending key events from the HID ring */
        input_event_t ev;
        while (input_poll(&ev)) {
            if (ev.type  == INPUT_EV_KEY &&
                ev.state == INPUT_KEY_PRESS) {
                char c = input_keycode_to_ascii(ev.keycode, ev.modifiers);
                if (c) shell_on_char_inst(sh, c);
            }
        }

        /* Cursor blink (500 ms interval using real jiffy clock) */
        uint64_t now = sched_get_ticks();
        if (now >= next_blink) {
            fbcon_tick(con);
            next_blink = now + 500;
        }

        /* Yield every iteration — gives usb-evt time to push the next
         * keyboard HID report into the ring before we poll again.         */
        sched_sleep(1);
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
    syscall_init();

    /* ── 5c. Input event subsystem ────────────────────────────────────────── */
    input_init();

    /* ── 5d. PCI enumeration ──────────────────────────────────────────────── */
    static pci_device_t s_pci_buf[64];
    int pci_count = pci_enumerate(s_pci_buf, 64);
    KLOG_INFO("pci: found %d device(s)\n", pci_count);

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

    /* ── 8. APIC ──────────────────────────────────────────────────────────── */
    madt_info_t *madt = acpi_get_madt_info();
    apic_init(madt->lapic_base);
    g_apic_ticks_per_ms = apic_timer_calibrate();
    apic_timer_init(g_apic_ticks_per_ms);

    /* ── 9. Scheduler init on BSP ─────────────────────────────────────────── */
    sched_init(0);

    /* ── 10. SMP: bring up AP cores ───────────────────────────────────────── */
    smp_init();

    /* ── 11. Spawn tasks ──────────────────────────────────────────────────── */
    sched_spawn("shell",        shell_task,        NULL);
    sched_spawn("usb-init",     usb_init_task,     NULL);
    sched_spawn("storage-init", storage_init_task, NULL);

    KLOG_INFO("Kernel init complete. Entering idle.\n");

    /* ── 12. Enable interrupts — scheduler takes over ─────────────────────── */
    cpu_sti();
    sched_idle_loop();

halt:
    cpu_cli();
    for (;;) cpu_halt();
}
