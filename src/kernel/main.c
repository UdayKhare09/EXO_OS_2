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
#include "mm/pagecache.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "arch/x86_64/pci.h"
#include "gfx/fbcon.h"
#include "gfx/font.h"
#include "drivers/input/input.h"
#include "drivers/usb/usb_core.h"
#include "drivers/usb/hid.h"

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
#include "drivers/pty.h"
#include "drivers/devpts.h"
#include "fs/procfs.h"
#include "fs/sysfs.h"
#include "fs/fd.h"
#include "fs/elf.h"
#include "fs/gpt.h"
#include "syscall/syscall.h"

/* ── Networking ─────────────────────────────────────────────────────────── */
extern void net_init_task(void *arg);
extern void tty_set_fg_pgid(int pgid);

typedef struct {
    fbcon_t *con;
    char cwd[VFS_MOUNT_PATH_MAX];
} launcher_t;

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

static bool read_small_text_file(const char *path, char *buf, size_t buf_sz)
__attribute__((unused));
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

/* Suppress unused-function warnings: these may be used by future code */
__attribute__((unused))
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

static launcher_t *launcher_create_quiet(fbcon_t *con) {
    launcher_t *launcher = kmalloc(sizeof(*launcher));
    if (!launcher) return NULL;
    launcher->con = con;
    strncpy(launcher->cwd, "/home/root", VFS_MOUNT_PATH_MAX - 1);
    launcher->cwd[VFS_MOUNT_PATH_MAX - 1] = '\0';
    return launcher;
}

static void launcher_exec_path(launcher_t *launcher, const char *args) {
    fbcon_t *con = launcher->con;
    if (!args || !*args) {
        fbcon_puts_inst(con, "  usage: exec <path>\n");
        return;
    }

    while (*args == ' ') args++;
    if (!*args) {
        fbcon_puts_inst(con, "  usage: exec <path>\n");
        return;
    }

    char fullpath[VFS_MOUNT_PATH_MAX];
    if (args[0] == '/') {
        strncpy(fullpath, args, VFS_MOUNT_PATH_MAX - 1);
        fullpath[VFS_MOUNT_PATH_MAX - 1] = '\0';
    } else {
        path_join(launcher->cwd, args, fullpath);
    }

    int vfs_err = 0;
    vnode_t *vn = vfs_lookup(fullpath, true, &vfs_err);
    if (!vn) {
        fbcon_printf_inst(con, "  exec: '%s': file not found\n", fullpath);
        return;
    }

    uint64_t file_size = vn->size;
    if (file_size == 0 || file_size > (64ULL * 1024 * 1024)) {
        fbcon_printf_inst(con, "  exec: '%s': invalid file size (%llu)\n", fullpath, file_size);
        vfs_vnode_put(vn);
        return;
    }

    void *buf = kmalloc(file_size);
    if (!buf) {
        fbcon_puts_inst(con, "  exec: out of memory\n");
        vfs_vnode_put(vn);
        return;
    }

    ssize_t rd = vn->ops->read(vn, buf, file_size, 0);
    vfs_vnode_put(vn);
    if (rd < 0 || (uint64_t)rd != file_size) {
        fbcon_printf_inst(con, "  exec: read error (%ld)\n", rd);
        kfree(buf);
        return;
    }

    uintptr_t pml4 = vmm_create_address_space();
    if (!pml4) {
        fbcon_puts_inst(con, "  exec: failed to create address space\n");
        kfree(buf);
        return;
    }

    elf_info_t info;
    int r = elf_load(buf, file_size, pml4, 0, &info);
    kfree(buf);
    if (r < 0) {
        fbcon_printf_inst(con, "  exec: ELF load failed (%d)\n", r);
        vmm_destroy_address_space(pml4);
        return;
    }

    uintptr_t user_stack_top = USER_STACK_TOP;
    uintptr_t stack_pages = 8;
    for (uintptr_t i = 0; i < stack_pages; i++) {
        uintptr_t pg = pmm_alloc_pages(1);
        if (pg) {
            memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
            vmm_map_page_in(pml4,
                            user_stack_top - (stack_pages - i) * PAGE_SIZE,
                            pg, VMM_USER_RW);
        }
    }

    uintptr_t sp = user_stack_top;

    #define EXEC_PUSH(val) do {                                          \
        sp -= 8;                                                          \
        uint64_t *_pte = vmm_get_pte(pml4, sp);                          \
        if (_pte) {                                                       \
            uintptr_t _ph = (*_pte) & 0x000FFFFFFFFFF000ULL;             \
            uintptr_t _of = sp & (PAGE_SIZE - 1);                        \
            *(uint64_t *)(vmm_phys_to_virt(_ph) + _of) = (uint64_t)(val);\
        }                                                                 \
    } while(0)

    #define EXEC_PUSH_STR(str) ({ \
        size_t _len = strlen(str) + 1; \
        sp -= _len; \
        sp &= ~0x7ULL; \
        uintptr_t _addr = sp; \
        uint64_t *_pte2 = vmm_get_pte(pml4, sp); \
        if (_pte2) { \
            uintptr_t _ph2 = (*_pte2) & 0x000FFFFFFFFFF000ULL; \
            uintptr_t _of2 = sp & (PAGE_SIZE - 1); \
            memcpy((void *)(vmm_phys_to_virt(_ph2) + _of2), (str), _len); \
        } \
        _addr; \
    })

    const char *argv0_str = fullpath;
    /* For /sbin/init: plain "init" (not a login shell).
     * For /bin/sh launched by init itself: init.c sets argv0="-sh";
     *   if the kernel is launching the shell directly as a fallback,
     *   treat it as a login shell. */
    if (strcmp(fullpath, "/sbin/init") == 0)
        argv0_str = "init";
    else if (strcmp(fullpath, "/bin/sh") == 0)
        argv0_str = "-sh";
    uintptr_t argv0_addr = EXEC_PUSH_STR(argv0_str);

    char env_pwd[VFS_MOUNT_PATH_MAX + 5];
    const char *cwd_env = (launcher && launcher->cwd[0]) ? launcher->cwd : "/";
    size_t cwd_len = strlen(cwd_env);
    if (cwd_len > VFS_MOUNT_PATH_MAX - 1)
        cwd_len = VFS_MOUNT_PATH_MAX - 1;
    memcpy(env_pwd, "PWD=", 4);
    memcpy(env_pwd + 4, cwd_env, cwd_len);
    env_pwd[4 + cwd_len] = '\0';

    const char *env_list[] = {
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "HOME=/home/root",
        "TERM=linux",
        "SHELL=/bin/sh",
        "USER=root",
        "LOGNAME=root",
        "HOSTNAME=exo",
        "TMPDIR=/tmp",
        "PS1=root@exo:\\w# ",
        "SSL_CERT_FILE=/etc/ssl/cert.pem",
        "SSL_CERT_DIR=/etc/ssl/certs",
        env_pwd,
    };
    uintptr_t env_addrs[sizeof(env_list) / sizeof(env_list[0])] = {0};
    int env_count = 0;
    for (size_t i = 0; i < sizeof(env_list) / sizeof(env_list[0]); i++)
        env_addrs[env_count++] = EXEC_PUSH_STR(env_list[i]);

    /* AT_RANDOM: 16 bytes of random data (we fill with a simple pseudo-random) */
    sp      -= 16;
    sp      &= ~0xFULL;
    uint64_t random_addr = sp;
    {
        uint64_t *rp_virt = NULL;
        uint64_t *pte2 = vmm_get_pte(pml4, sp);
        if (pte2) {
            uintptr_t ph2 = (*pte2) & 0x000FFFFFFFFFF000ULL;
            uintptr_t of2 = sp & (PAGE_SIZE - 1);
            rp_virt = (uint64_t *)(vmm_phys_to_virt(ph2) + of2);
            /* Fill 16 bytes with deterministic pseudo-random (fine for boot) */
            rp_virt[0] = 0xDEADBEEFCAFEBABEULL ^ (uint64_t)(uintptr_t)fullpath;
            rp_virt[1] = 0xFEEDFACEFACEFEEDULL ^ (uint64_t)sp;
        }
    }

    /* AT_EXECFN: the executable path string */
    uintptr_t execfn_addr = EXEC_PUSH_STR(fullpath);

    sp &= ~0xFULL;

    /* Auxiliary vector (AT_NULL terminator first, pushed in reverse) */
    EXEC_PUSH(0);   EXEC_PUSH(0);    /* AT_NULL */

    EXEC_PUSH(execfn_addr);          /* AT_EXECFN value */
    EXEC_PUSH(31);                   /* AT_EXECFN = 31  */

    EXEC_PUSH(random_addr);          /* AT_RANDOM value */
    EXEC_PUSH(25);                   /* AT_RANDOM = 25  */

    EXEC_PUSH(0);    EXEC_PUSH(14);  /* AT_EGID  = 14 */
    EXEC_PUSH(0);    EXEC_PUSH(13);  /* AT_GID   = 13 */
    EXEC_PUSH(0);    EXEC_PUSH(12);  /* AT_EUID  = 12 */
    EXEC_PUSH(0);    EXEC_PUSH(11);  /* AT_UID   = 11 */
    EXEC_PUSH(0);    EXEC_PUSH(23);  /* AT_SECURE = 23 */
    EXEC_PUSH(info.entry);           /* AT_ENTRY value */
    EXEC_PUSH(9);                    /* AT_ENTRY = 9   */
    EXEC_PUSH(PAGE_SIZE);            /* AT_PAGESZ value */
    EXEC_PUSH(6);                    /* AT_PAGESZ = 6   */
    EXEC_PUSH(info.phdr_size);       /* AT_PHENT value */
    EXEC_PUSH(4);                    /* AT_PHENT = 4   */
    EXEC_PUSH(info.phdr_count);      /* AT_PHNUM value */
    EXEC_PUSH(5);                    /* AT_PHNUM = 5   */
    EXEC_PUSH(info.phdr_vaddr);      /* AT_PHDR value  */
    EXEC_PUSH(3);                    /* AT_PHDR = 3    */

    EXEC_PUSH(0);
    for (int i = env_count - 1; i >= 0; i--)
        EXEC_PUSH(env_addrs[i]);

    EXEC_PUSH(0);
    EXEC_PUSH(argv0_addr);
    EXEC_PUSH(1);

    #undef EXEC_PUSH_STR
    #undef EXEC_PUSH

    uint32_t cpu = sched_pick_cpu();
    task_t *t = task_create_user(fullpath, pml4, info.entry, sp, cpu);
    if (!t) {
        fbcon_puts_inst(con, "  exec: failed to create task\n");
        vmm_destroy_address_space(pml4);
        return;
    }

    t->brk_base    = info.brk_start;
    t->brk_current = info.brk_start;
    t->mmap_next   = USER_MMAP_BASE;
    const char *initial_cwd = (launcher && launcher->cwd[0]) ? launcher->cwd : "/";
    strncpy(t->cwd, initial_cwd, TASK_CWD_MAX - 1);
    t->cwd[TASK_CWD_MAX - 1] = '\0';

    vma_t *stack_vma = kmalloc(sizeof(vma_t));
    if (stack_vma) {
        stack_vma->start = user_stack_top - stack_pages * PAGE_SIZE;
        stack_vma->end   = user_stack_top;
        stack_vma->flags = VMA_READ | VMA_WRITE | VMA_USER | VMA_STACK;
        stack_vma->file = NULL;
        stack_vma->file_offset = 0;
        stack_vma->file_size = 0;
        stack_vma->mmap_flags = 0;
        stack_vma->next  = NULL;
        vma_insert(t, stack_vma);
    }

    int vfs_err2 = 0;
    vnode_t *tty = vfs_lookup("/dev/tty", true, &vfs_err2);
    if (tty) {
        fd_setup_stdio(t, tty);
        vfs_vnode_put(tty);
    } else {
        fbcon_puts_inst(con, "  exec: warning: could not open /dev/tty for stdio\n");
    }

    tty_set_fg_pgid((int)t->pgid);

    /* PID 1 has no parent in the process tree.  All other processes spawned
     * by the kernel launcher get the current kernel thread as parent (so
     * that SIGCHLD + zombie reaping work for intra-kernel launches). */
    if (t->pid == 1) {
        t->parent = NULL;
        t->ppid   = 0;
    } else {
        t->parent = sched_current();
    }
    uint32_t child_tid = t->tid;

    sched_add_task(t, cpu);

    /* For PID 1 the kernel launcher does NOT wait — init manages its own
     * children.  For any other process the kernel launcher blocks until the
     * child exits so it can clean up the zombie. */
    if (t->pid != 1) {
        task_t *ct;
        for (;;) {
            ct = task_lookup(child_tid);
            if (!ct || ct->state == TASK_ZOMBIE || ct->state == TASK_DEAD)
                break;
            sched_sleep(10);
        }
        ct = task_lookup(child_tid);
        if (ct && ct->state == TASK_ZOMBIE) {
            ct->parent = NULL;
            task_destroy(ct);
        }
    }
}

/* ── Init task: launch /sbin/init (PID 1) ────────────────────────────────── */
static void init_task(void *arg) {
    (void)arg;

    /* Preferred init binary; fall back to /bin/sh if not present */
    static const char *const candidates[] = {
        "/sbin/init",
        "/bin/sh",
        NULL,
    };

    const char *init_path = NULL;

    /* Wait for the filesystem to be ready */
    for (;;) {
        for (int i = 0; candidates[i]; i++) {
            if (vfs_path_exists(candidates[i])) {
                init_path = candidates[i];
                break;
            }
        }
        if (init_path) break;
        sched_sleep(10);
    }

    KLOG_INFO("init: launching '%s'\n", init_path);

    /* Small grace period for USB/HID to come up */
    sched_sleep(200);

    fbcon_t *con = fbcon_get();
    launcher_t *launcher = launcher_create_quiet(con);
    if (!launcher) {
        KLOG_ERR("init: failed to create launcher context\n");
        return;
    }

    /* Launch /sbin/init as PID 1. launcher_exec_path deliberately returns
     * immediately for PID 1 (init supervises its own children). */
    launcher_exec_path(launcher, init_path);

    /* Init task has nothing else to do once PID 1 is running. */
    for (;;) sched_sleep(10000);
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
    pagecache_init();

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
    vfs_mkdir("/sys",  0555);
    vfs_mkdir("/run",  0755);
    vfs_mkdir("/etc",  0755);
    vfs_mkdir("/home", 0755);
    vfs_mkdir("/home/root", 0755);
    vfs_mkdir("/var",  0755);
    vfs_mkdir("/var/run", 0755);
    vfs_mkdir("/var/log", 0755);
    vfs_mkdir("/lib",  0755);
    vfs_mkdir("/mnt",  0755);
    if (vfs_mount("/run", NULL, "tmpfs") < 0)
        KLOG_WARN("vfs: failed to mount tmpfs on /run\n");
    /* Standard /run subdirectories populated by init/systemd on real Linux */
    vfs_mkdir("/run/lock",     01777);
    vfs_mkdir("/run/shm",      01777);
    vfs_mkdir("/run/user",     0755);
    vfs_mkdir("/run/udev",     0755);
    vfs_mkdir("/run/dbus",     0755);
    vfs_mkdir("/run/network",  0755);
    vfs_mkdir("/run/sshd",     0750);
    if (vfs_mount("/tmp", NULL, "tmpfs") < 0)
        KLOG_WARN("vfs: failed to mount tmpfs on /tmp\n");
    devfs_init();
    /* PTY subsystem: must come after devfs (needs /dev mounted) */
    pty_init();
    vfs_mkdir("/dev/pts", 0755);
    devpts_init();
    /* /dev/shm: shared-memory tmpfs (used by mmap MAP_SHARED) */
    vfs_mkdir("/dev/shm", 01777);
    vfs_mount("/dev/shm", NULL, "tmpfs");
    procfs_init();
    sysfs_init();
    syscall_init();
    extern void unix_socket_init(void);
    unix_socket_init();

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



    sched_spawn("kinit",        init_task,         NULL);
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
