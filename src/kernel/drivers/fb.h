/* drivers/fb.h — Linux fbdev ABI types and ioctl commands for /dev/fb0.
 *
 * Only the subset needed for SDL2/ncurses/directfb-style framebuffer access
 * is implemented.  Fields match the Linux kernel ABI exactly so that
 * user-space built against glibc <linux/fb.h> works without changes.
 */
#pragma once
#include <stdint.h>
#include "fs/fd.h"   /* file_ops_t */

/* ── ioctl commands ──────────────────────────────────────────────────────── */
/* Linux fbdev ioctl numbers (from <linux/fb.h>) */
#define FBIOGET_VSCREENINFO  0x4600   /* get variable screen info */
#define FBIOPUT_VSCREENINFO  0x4601   /* set variable screen info */
#define FBIOGET_FSCREENINFO  0x4602   /* get fixed   screen info */
#define FBIOGETCMAP          0x4604   /* get colormap (stub)     */
#define FBIOPUTCMAP          0x4605   /* set colormap (stub)     */
#define FBIOBLANK            0x4611   /* blank / unblank display */
#define FBIOGET_CON2FBMAP    0x460F   /* console → fb mapping    */
#define FBIOPUT_CON2FBMAP    0x4610

/* ── fb_fix_screeninfo ─────────────────────────────────────────────────── */
/* Constant (hardware-determined) screen parameters */
#define FB_TYPE_PACKED_PIXELS    0   /* Packed Pixels                        */
#define FB_VISUAL_TRUECOLOR      2   /* True color (fixed cmap)              */
#define FB_ACCEL_NONE          255   /* No hardware accelerator              */

typedef struct fb_fix_screeninfo {
    char     id[16];           /* identification string, e.g. "EXO FB"      */
    uint64_t smem_start;       /* start of frame buffer mem (physical addr) */
    uint32_t smem_len;         /* length of frame buffer mem in bytes       */
    uint32_t type;             /* see FB_TYPE_*                             */
    uint32_t type_aux;         /* interleave for interleaved Planes         */
    uint32_t visual;           /* see FB_VISUAL_*                           */
    uint16_t xpanstep;         /* zero if no hardware panning               */
    uint16_t ypanstep;         /* zero if no hardware panning               */
    uint16_t ywrapstep;        /* zero if no hardware ywrap                 */
    uint32_t line_length;      /* length of a line in bytes                 */
    uint64_t mmio_start;       /* start of Memory Mapped I/O (physical)     */
    uint32_t mmio_len;         /* length of Memory Mapped I/O               */
    uint32_t accel;            /* type of acceleration available            */
    uint16_t capabilities;     /* see FB_CAP_*                              */
    uint16_t reserved[2];
} __attribute__((packed)) fb_fix_screeninfo_t;

/* ── fb_bitfield ───────────────────────────────────────────────────────── */
typedef struct fb_bitfield {
    uint32_t offset;           /* beginning of bitfield (from LSB) */
    uint32_t length;           /* length of bitfield               */
    uint32_t msb_right;        /* ≠ 0 if MSB is right              */
} fb_bitfield_t;

/* ── fb_var_screeninfo ─────────────────────────────────────────────────── */
/* User-changeable screen parameters */
typedef struct fb_var_screeninfo {
    uint32_t xres;             /* visible resolution (horizontal)  */
    uint32_t yres;             /* visible resolution (vertical)    */
    uint32_t xres_virtual;     /* virtual resolution               */
    uint32_t yres_virtual;
    uint32_t xoffset;          /* offset from virtual to visible   */
    uint32_t yoffset;

    uint32_t bits_per_pixel;
    uint32_t grayscale;        /* 0=colour, 1=grayscale            */

    fb_bitfield_t red;
    fb_bitfield_t green;
    fb_bitfield_t blue;
    fb_bitfield_t transp;      /* transparency                     */

    uint32_t nonstd;           /* != 0 Non standard pixel format   */
    uint32_t activate;
    uint32_t height;           /* height of picture in mm          */
    uint32_t width;            /* width of picture in mm           */

    uint32_t accel_flags;      /* (OBSOLETE) see fb_info.flags     */

    /* Timing: All values in picoseconds except pixclock (in ps/dotclock) */
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
} fb_var_screeninfo_t;

/* ── Public API ────────────────────────────────────────────────────────── */
extern file_ops_t g_fb_fops;

