/* drivers/fb.c — /dev/fb0 Linux fbdev character device.
 *
 * Provides the minimal fbdev interface required for SDL2 direct-render,
 * Linux framebuffer console utilities, and any program that uses
 * FBIOGET_FSCREENINFO / FBIOGET_VSCREENINFO / mmap(fd, MAP_SHARED).
 *
 * No write-combining or hardware cursor is attempted; we just expose
 * the Limine UEFI GOP framebuffer that fbcon already uses.
 */
#include "fb.h"
#include "gfx/fbcon.h"    /* fbcon_get_fb_info() */
#include "fs/fd.h"
#include "fs/vfs.h"       /* EINVAL, ENODEV, etc. */
#include "mm/vmm.h"       /* vmm_map_page_in, VMM_* */
#include "mm/pmm.h"       /* PAGE_SIZE */
#include "sched/sched.h"  /* sched_current() */
#include "sched/task.h"   /* task_t, cr3 */
#include "net/socket_defs.h" /* POLLIN, POLLOUT */
#include "lib/string.h"   /* memset, memcpy, strncpy */
#include <stdint.h>
#include <stddef.h>

/* ── Pixel format constants (XRGB8888 — the Limine default on x86-64) ─── */
/* Each pixel is stored as 0x00RRGGBB in memory (32 bpp, no alpha used).   */
#define FB_BPP 32
/* Bit-field layout for XRGB8888:
 *   transp: offset=24, length=0  (no alpha)
 *   red:    offset=16, length=8
 *   green:  offset= 8, length=8
 *   blue:   offset= 0, length=8                                            */

/* ── Helpers to populate the fixed/variable info structs ─────────────────── */

static void fill_fix(fb_fix_screeninfo_t *fix) {
    uint64_t phys;  uint32_t w, h, pitch;
    fbcon_get_fb_info(&phys, &w, &h, &pitch);

    memset(fix, 0, sizeof(*fix));
    /* Use strncpy-equivalent via memcpy — id is 16 bytes */
    const char *name = "EXO FB";
    size_t nlen = 0;
    while (name[nlen] && nlen < 15) nlen++;
    for (size_t i = 0; i < nlen; i++) fix->id[i] = name[i];

    fix->smem_start  = phys;
    fix->smem_len    = pitch * h;
    fix->type        = FB_TYPE_PACKED_PIXELS;
    fix->type_aux    = 0;
    fix->visual      = FB_VISUAL_TRUECOLOR;
    fix->xpanstep    = 0;
    fix->ypanstep    = 0;
    fix->ywrapstep   = 0;
    fix->line_length = pitch;
    fix->mmio_start  = 0;
    fix->mmio_len    = 0;
    fix->accel       = FB_ACCEL_NONE;
}

static void fill_var(fb_var_screeninfo_t *var) {
    uint64_t phys;  uint32_t w, h, pitch;
    fbcon_get_fb_info(&phys, &w, &h, &pitch);
    (void)phys; (void)pitch;

    memset(var, 0, sizeof(*var));
    var->xres         = w;
    var->yres         = h;
    var->xres_virtual = w;
    var->yres_virtual = h;
    var->xoffset      = 0;
    var->yoffset      = 0;

    var->bits_per_pixel = FB_BPP;
    var->grayscale      = 0;

    /* XRGB8888: blue at bit 0, green at 8, red at 16, no alpha */
    var->red.offset = 16; var->red.length = 8; var->red.msb_right = 0;
    var->green.offset = 8; var->green.length = 8; var->green.msb_right = 0;
    var->blue.offset = 0; var->blue.length = 8; var->blue.msb_right = 0;
    var->transp.offset = 0; var->transp.length = 0; var->transp.msb_right = 0;

    /* No timing information (Limine abstracts the hardware fully). */
    var->pixclock = 0;
    var->vmode    = 0;  /* FB_VMODE_NONINTERLACED */
}

/* ══════════════════════════════════════════════════════════════════════════
 * file_ops callbacks
 * ══════════════════════════════════════════════════════════════════════════ */

static ssize_t fb_read(file_t *f, void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
    /* Direct framebuffer reads should go through mmap.  -EIO matches Linux. */
    return -EIO;
}

static ssize_t fb_write(file_t *f, const void *buf, size_t count) {
    /* Accept pixel writes at current offset (write-through copy). */
    uint64_t phys; uint32_t w, h, pitch;
    fbcon_get_fb_info(&phys, &w, &h, &pitch);
    (void)w;

    uint64_t fb_total = (uint64_t)pitch * h;
    if (!phys || f->offset >= fb_total) return -EIO;

    /* Map HHDM virtual address of the framebuffer and memcpy into it.
     * The Limine HHDM mapping is always present in the kernel half.        */
    extern uintptr_t vmm_phys_to_virt(uintptr_t phys);
    uint8_t *virt_fb = (uint8_t *)vmm_phys_to_virt((uintptr_t)phys);
    if (!virt_fb) return -EIO;

    uint64_t avail = fb_total - f->offset;
    if ((uint64_t)count > avail) count = (size_t)avail;
    for (size_t i = 0; i < count; i++)
        virt_fb[f->offset + i] = ((const uint8_t *)buf)[i];
    f->offset += count;
    return (ssize_t)count;
}

static int fb_poll(file_t *f, int events) {
    (void)f;
    int ready = 0;
    if (events & (POLLOUT | POLLWRNORM)) ready |= POLLOUT | POLLWRNORM;
    /* Always ready for write; reads go through mmap. */
    return ready;
}

static int fb_ioctl(file_t *f, unsigned long cmd, unsigned long arg) {
    (void)f;
    void *uptr = (void *)(uintptr_t)arg;

    switch (cmd) {
    case FBIOGET_FSCREENINFO: {
        if (!uptr) return -EINVAL;
        fb_fix_screeninfo_t fix;
        fill_fix(&fix);
        /* Copy to user — kernel memory is identity-mapped so this is a
         * direct memcpy of the kernel-side struct into user-provided ptr.  */
        memcpy(uptr, &fix, sizeof(fix));
        return 0;
    }
    case FBIOGET_VSCREENINFO: {
        if (!uptr) return -EINVAL;
        fb_var_screeninfo_t var;
        fill_var(&var);
        memcpy(uptr, &var, sizeof(var));
        return 0;
    }
    case FBIOPUT_VSCREENINFO: {
        /* We accept but ignore attempts to change the screen mode because
         * Limine gives us a fixed GOP framebuffer.  Return success so that
         * applications that call FBIOPUT after FBIOGET don't fail.         */
        return 0;
    }
    case FBIOBLANK:
        /* Blank / unblank: accept and ignore. */
        return 0;
    case FBIOGET_CON2FBMAP:
        if (!uptr) return -EINVAL;
        /* Both virtual console 0 and 1 map to fb0. */
        *(uint32_t *)(uintptr_t)arg = 0;
        return 0;
    default:
        return -EINVAL;
    }
}

/* ── /dev/fb0 mmap ────────────────────────────────────────────────────────
 * Maps `len` bytes starting at `offset` bytes into the framebuffer physical
 * memory at virtual address `addr` in the current task's address space.
 *
 * addr must be page-aligned (guaranteed by sys_mmap before calling here).
 * Returns addr on success, negative errno on failure.                      */
static int64_t fb_mmap(file_t *f, uint64_t addr, uint64_t len,
                        int prot, int flags, int64_t offset) {
    (void)f; (void)prot; (void)flags;

    uint64_t phys; uint32_t w, h, pitch;
    fbcon_get_fb_info(&phys, &w, &h, &pitch);
    (void)w;
    if (!phys) return -ENODEV;

    uint64_t fb_total = (uint64_t)pitch * h;
    if ((uint64_t)offset >= fb_total) return -EINVAL;
    if (len == 0) return -EINVAL;

    /* Clamp requested length to what remains in the framebuffer. */
    if ((uint64_t)offset + len > fb_total)
        len = (size_t)(fb_total - (uint64_t)offset);

    task_t  *cur  = sched_current();
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

    /* offset is already page-aligned by the caller (sys_mmap rounds down).
     * Map each physical page of the framebuffer into the user address space. */
    uint64_t phys_off = (uint64_t)offset & ~(PAGE_SIZE - 1);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t pa = phys + phys_off + i * PAGE_SIZE;
        uint64_t va = addr + i * PAGE_SIZE;
        /* Write-through + Write-combining not possible without PAT support.
         * Use present+writable+user — good enough for Limine UEFI GOP.     */
        vmm_map_page_in(cur->cr3, va, pa, VMM_PRESENT | VMM_WRITE | VMM_USER);
    }
    return (int64_t)addr;
}

static int fb_close(file_t *f) {
    (void)f;
    return 0;
}

/* ── Exported file_ops ───────────────────────────────────────────────────── */
file_ops_t g_fb_fops = {
    .read  = fb_read,
    .write = fb_write,
    .close = fb_close,
    .poll  = fb_poll,
    .ioctl = fb_ioctl,
    .mmap  = fb_mmap,
};
