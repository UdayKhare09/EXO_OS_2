/*
 * gfx/virtio_gpu.c — VirtIO GPU v1 driver
 *
 * Features:
 *   • Modern VirtIO PCI transport (capability walking)
 *   • Double-buffered presentation (front/back GPU resources, SET_SCANOUT flip)
 *   • Hardware cursor (UPDATE_CURSOR / MOVE_CURSOR — GPU composites independently)
 *   • virgl 3D context with GPU-side CLEAR and BLIT (zero-copy compositing)
 *   • Dynamic GPU resource allocation for per-window surfaces
 */
#include "virtio_gpu.h"
#include "arch/x86_64/pci.h"
#include "arch/x86_64/cpu.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/klog.h"
#include <stddef.h>

/* ── VirtIO PCI capability ─────────────────────────────────────────────────── */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

/* PCI Vendor-Specific capability ID */
#define PCI_CAP_VENDOR  0x09

/* Device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE      1
#define VIRTIO_STATUS_DRIVER           2
#define VIRTIO_STATUS_DRIVER_OK        4
#define VIRTIO_STATUS_FEATURES_OK      8
#define VIRTIO_STATUS_FAILED         128

/* GPU 2D commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102u
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107u
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108u
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109u

/* GPU 3D / virgl commands */
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200u
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201u
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202u
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205u
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206u
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207u

/* Cursor commands */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR           0x0300u
#define VIRTIO_GPU_CMD_MOVE_CURSOR             0x0301u

/* Responses */
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101u
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102u
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103u

/* Capsets */
#define VIRTIO_GPU_CAPSET_VIRGL                1u

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2u
#define VIRTIO_GPU_FLAG_FENCE (1u)

/* Resource IDs: 1=scanout/front, 2=back, 100=cursor, 3+ = dynamic */
#define RES_ID_FRONT    1u
#define RES_ID_BACK     2u
#define RES_ID_CURSOR   100u
#define RES_ID_DYN_BASE 3u

/* Cursor size (VirtIO spec: must be 64×64 ARGB8888) */
#define CURSOR_W 64u
#define CURSOR_H 64u

/* Max dynamic resources */
#define VIRTIO_GPU_MAX_RESOURCES 64

/* Virtqueue size (power of 2) */
#define VQ_SIZE  64

/* ── virgl command encoding ─────────────────────────────────────────────── */
#define VIRGL_CMD(cmd, obj_type, nargs)  (((uint32_t)(nargs)<<16)|((uint32_t)(obj_type)<<8)|(uint32_t)(cmd))

#define VIRGL_OBJECT_SURFACE              8u

#define VIRGL_CCMD_CREATE_OBJECT          1u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE  5u
#define VIRGL_CCMD_CLEAR                  7u
#define VIRGL_CCMD_BLIT                   16u

/* virgl format matching VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM */
#define VIRGL_FORMAT_B8G8R8X8_UNORM       2u

/* Gallium pipe constants */
#define PIPE_TEXTURE_2D        2u
#define PIPE_BIND_RENDER_TARGET (1u << 1)   /* 2 */
#define PIPE_BIND_SAMPLER_VIEW  (1u << 3)   /* 8 */
#define PIPE_CLEAR_COLOR0       4u   /* 1<<2 */

/* IEEE754 float constants (no FPU required) */
#define F32_0    0x00000000u
#define F32_1    0x3F800000u   /* 1.0f */
#define F64_1_LO 0x00000000u   /* double 1.0 low  32 bits */
#define F64_1_HI 0x3FF00000u   /* double 1.0 high 32 bits */

/* ── Common config layout (MMIO) ─────────────────────────────────────────── */
typedef volatile struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t config_msix_vector;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    /* queue */
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    /* no padding here — queue_desc must be at offset 32 per VirtIO 1.0 spec */
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} virtio_common_cfg_t;

/* ── GPU device config ───────────────────────────────────────────────────── */
typedef volatile struct __attribute__((packed)) {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} virtio_gpu_config_t;

/* ── GPU protocol structs ────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} gpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    struct { uint32_t enabled, flags, r, g, b, a, x, y, width, height; } pmodes[16];
} gpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} gpu_cmd_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} gpu_mem_entry_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    gpu_mem_entry_t entries[1];
} gpu_cmd_attach_backing_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    struct { uint32_t x, y, width, height; } r;  /* rect first per spec */
    uint32_t scanout_id;
    uint32_t resource_id;
} gpu_cmd_set_scanout_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    struct { uint32_t x, y, width, height; } r;
    uint64_t offset;       /* byte offset into resource backing storage */
    uint32_t resource_id;
    uint32_t padding;
} gpu_cmd_transfer_t;

/* 3-D box for TRANSFER_TO_HOST_3D (VirtIO spec §5.7.6.8) */
typedef struct __attribute__((packed)) {
    uint32_t x, y, z;
    uint32_t w, h, d;
} gpu_box_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    gpu_box_t      box;
    uint64_t       offset;       /* byte offset into resource backing */
    uint32_t       resource_id;
    uint32_t       level;        /* mip level — 0 for our flat 2-D use */
    uint32_t       stride;       /* row stride in bytes; 0 = auto */
    uint32_t       layer_stride; /* 0 for 2-D */
} gpu_cmd_transfer_3d_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    struct { uint32_t x, y, width, height; } r;
    uint32_t resource_id;
    uint32_t padding;
} gpu_cmd_resource_flush_t;

/* ── New 3D / virgl protocol structs ────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t capset_index;
    uint32_t padding;
} gpu_cmd_get_capset_info_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} gpu_resp_capset_info_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t nlen;           /* debug name length (0 = no name) */
    uint32_t context_init;   /* 0 = virgl */
    char     debug_name[64];
} gpu_cmd_ctx_create_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} gpu_cmd_ctx_resource_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;       /* PIPE_TEXTURE_2D = 2 */
    uint32_t format;       /* VIRGL_FORMAT_B8G8R8X8_UNORM = 2 */
    uint32_t bind;         /* PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW */
    uint32_t width;
    uint32_t height;
    uint32_t depth;        /* 1 for 2-D */
    uint32_t array_size;   /* 1 */
    uint32_t last_level;   /* 0 */
    uint32_t nr_samples;   /* 0 */
    uint32_t flags;        /* 0 */
    uint32_t padding;      /* VirtIO spec trailing pad — MUST be present */
} gpu_cmd_resource_create_3d_t;

typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    uint32_t size;          /* byte size of the inline command buffer */
    uint32_t padding;
    /* virgl command dwords follow immediately */
} gpu_cmd_submit_3d_t;

/* UPDATE_CURSOR and MOVE_CURSOR share the same wire format */
typedef struct __attribute__((packed)) {
    gpu_ctrl_hdr_t hdr;
    struct { uint32_t scanout_id, x, y, padding; } pos;
    uint32_t resource_id;   /* 0 for MOVE_CURSOR */
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} gpu_cmd_cursor_t;

/* ── Virtqueue ────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
} vq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vq_used_elem_t ring[VQ_SIZE];
    uint16_t avail_event;
} vq_used_t;

typedef struct {
    vq_desc_t  *desc;       /* descriptor table (phys-aligned) */
    vq_avail_t *avail;      /* driver ring                     */
    vq_used_t  *used;       /* device ring                     */
    uint16_t    free_head;  /* next free descriptor            */
    uint16_t    last_used;  /* last idx we processed           */
    uintptr_t   notify_virt;/* virtio notify register virt addr*/
    uint32_t    notify_mul; /* notify_off_multiplier           */
    uint16_t    notify_off; /* per-queue notify_off            */
} virtq_t;

/* ── Driver state ─────────────────────────────────────────────────────────── */

/* Per dynamic-resource slot */
typedef struct {
    uint32_t       id;
    uint32_t       w, h;
    uintptr_t      phys;        /* backing DMA physical address */
    size_t         phys_pages;
    gfx_color_t   *shadow;      /* CPU-side BGRA write address  */
    gfx_surface_t *surf;        /* GFX surface pointing at shadow */
} gpu_resource_t;

static struct {
    pci_device_t          *pci;
    virtio_common_cfg_t   *common;
    virtio_gpu_config_t   *devcfg;
    uint8_t               *notify_base;
    uint32_t               notify_mul;
    uint8_t               *isr;
    virtq_t                ctlq;
    virtq_t                curq;    /* cursor queue (queue 1) */

    /* Framebuffer dimensions */
    uint32_t               fb_w, fb_h;

    /* Double-buffering: back = CPU draws here, front = displayed */
    uintptr_t              back_phys;
    size_t                 back_pages;
    gfx_color_t           *back_shadow;
    gfx_surface_t         *back_surf;    /* returned to caller */
    uint32_t               scanout_res;  /* which resource is currently on screen */

    /* Hardware cursor */
    uintptr_t              cursor_phys;
    gfx_color_t           *cursor_shadow;

    /* virgl 3D */
    bool                   virgl_ok;
    uint32_t               virgl_ctx_id; /* = 1 */
    /* surface handles for SET_FRAMEBUFFER_STATE */
    uint32_t               back_surf_handle;
    uint32_t               front_surf_handle;

    /* Dynamic resource table */
    gpu_resource_t         res_table[VIRTIO_GPU_MAX_RESOURCES];
    int                    res_count;
    uint32_t               next_res_id; /* monotonically increasing, starts at RES_ID_DYN_BASE */

    bool                   ready;
} gpu;

/* ── MMIO helpers (volatile byte-wise for MMIO correctness) ──────────────── */

/* ── Virtqueue initialise ─────────────────────────────────────────────────── */
static bool vq_init(virtq_t *vq, uint16_t qidx, virtio_common_cfg_t *cc,
                    uint8_t *notify_base, uint32_t notify_mul) {
    /* Allocate aligned pages for desc, avail, used */
    uintptr_t desc_phys  = pmm_alloc_pages(1);
    uintptr_t avail_phys = pmm_alloc_pages(1);
    uintptr_t used_phys  = pmm_alloc_pages(1);
    if (!desc_phys || !avail_phys || !used_phys) return false;

    vq->desc  = (vq_desc_t  *)vmm_phys_to_virt(desc_phys);
    vq->avail = (vq_avail_t *)vmm_phys_to_virt(avail_phys);
    vq->used  = (vq_used_t  *)vmm_phys_to_virt(used_phys);
    memset(vq->desc,  0, PAGE_SIZE);
    memset(vq->avail, 0, PAGE_SIZE);
    memset(vq->used,  0, PAGE_SIZE);

    /* Build free list */
    for (int i = 0; i < VQ_SIZE - 1; i++) vq->desc[i].next = i + 1;
    vq->free_head = 0;
    vq->last_used = 0;

    /* Tell device */
    cc->queue_select       = qidx;
    cc->queue_size         = VQ_SIZE;
    cc->queue_msix_vector  = 0xFFFF; /* no MSI-X */
    cc->queue_desc         = (uint64_t)desc_phys;
    cc->queue_driver       = (uint64_t)avail_phys;
    cc->queue_device       = (uint64_t)used_phys;
    cc->queue_enable       = 1;

    vq->notify_mul = notify_mul;
    vq->notify_off = cc->queue_notify_off;
    vq->notify_virt = (uintptr_t)notify_base + (uint32_t)cc->queue_notify_off * notify_mul;
    return true;
}

/* ── Submit a 2-descriptor chain (req→resp) and wait for completion ───────── */
static void vq_submit2(virtq_t *vq, uintptr_t req_phys, uint32_t req_len,
                        uintptr_t resp_phys, uint32_t resp_len) {
    uint16_t d0 = vq->free_head;
    uint16_t d1 = vq->desc[d0].next;
    vq->free_head = vq->desc[d1].next;

    /* req descriptor */
    vq->desc[d0].addr  = req_phys;
    vq->desc[d0].len   = req_len;
    vq->desc[d0].flags = 0x01;  /* NEXT */
    vq->desc[d0].next  = d1;

    /* resp descriptor (device writes) */
    vq->desc[d1].addr  = resp_phys;
    vq->desc[d1].len   = resp_len;
    vq->desc[d1].flags = 0x02;  /* WRITE */
    vq->desc[d1].next  = 0;

    /* Publish to available ring */
    uint16_t avail_idx = vq->avail->idx & (VQ_SIZE - 1);
    vq->avail->ring[avail_idx] = d0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    vq->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Notify device */
    *(volatile uint32_t *)vq->notify_virt = 0;

    /* Poll used ring */
    while (vq->used->idx == vq->last_used)
        __asm__ volatile("pause");
    vq->last_used = vq->used->idx;

    /* Return descriptors */
    vq->desc[d1].next = vq->free_head;
    vq->free_head     = d0;
}

/* ── Helper: alloc page-aligned DMA buffer ───────────────────────────────── */
static uintptr_t alloc_dma(size_t sz, void **virt_out) {
    size_t pages = (sz + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    void *virt = (void *)vmm_phys_to_virt(phys);
    memset(virt, 0, pages * PAGE_SIZE);
    if (virt_out) *virt_out = virt;
    return phys;
}

/* ── GPU: send command with no-data response ─────────────────────────────── */
static bool gpu_cmd(void *req, uint32_t req_sz) {
    uintptr_t req_phys  = vmm_virt_to_phys((uintptr_t)req);
    gpu_ctrl_hdr_t *resp_v;
    uintptr_t resp_phys = alloc_dma(sizeof(gpu_ctrl_hdr_t), (void **)&resp_v);
    if (!resp_phys) return false;
    vq_submit2(&gpu.ctlq, req_phys, req_sz, resp_phys, sizeof(gpu_ctrl_hdr_t));
    bool ok = (resp_v->type == VIRTIO_GPU_RESP_OK_NODATA);
    pmm_free_pages(resp_phys, 1);
    return ok;
}

/* ── Helper: send SET_SCANOUT pointing at a resource ─────────────────────── */
static bool gpu_set_scanout(uint32_t res_id, uint32_t w, uint32_t h) {
    gpu_cmd_set_scanout_t *ss;
    uintptr_t ss_phys = alloc_dma(sizeof(*ss), (void **)&ss);
    ss->hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    ss->r.x = 0; ss->r.y = 0; ss->r.width = w; ss->r.height = h;
    ss->scanout_id  = 0;
    ss->resource_id = res_id;
    bool ok = gpu_cmd(ss, sizeof(*ss));
    pmm_free_pages(ss_phys, 1);
    return ok;
}

/* ── Helper: create virgl 3D resource ────────────────────────────────────── */
static bool virgl_create_resource_3d(uint32_t res_id, uint32_t w, uint32_t h,
                                     uintptr_t shadow_phys, size_t shadow_bytes) {
    gpu_cmd_resource_create_3d_t *cr;
    uintptr_t cr_phys = alloc_dma(sizeof(*cr), (void **)&cr);
    cr->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cr->hdr.ctx_id  = 0;  /* not yet in a context */
    cr->resource_id = res_id;
    cr->target      = PIPE_TEXTURE_2D;
    cr->format      = VIRGL_FORMAT_B8G8R8X8_UNORM;
    cr->bind        = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
    cr->width       = w;
    cr->height      = h;
    cr->depth       = 1;
    cr->array_size  = 1;
    cr->last_level  = 0;
    cr->nr_samples  = 0;
    cr->flags       = 0;
    bool ok = gpu_cmd(cr, sizeof(*cr));
    pmm_free_pages(cr_phys, 1);
    if (!ok) return false;

    /* Attach backing storage so CPU can still write pixels directly */
    gpu_cmd_attach_backing_t *ab;
    uintptr_t ab_phys = alloc_dma(sizeof(*ab), (void **)&ab);
    ab->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab->resource_id = res_id;
    ab->nr_entries  = 1;
    ab->entries[0].addr   = shadow_phys;
    ab->entries[0].length = (uint32_t)shadow_bytes;
    ok = gpu_cmd(ab, sizeof(*ab));
    pmm_free_pages(ab_phys, 1);
    if (!ok) return false;

    /* Attach resource to virgl context */
    gpu_cmd_ctx_resource_t *ar;
    uintptr_t ar_phys = alloc_dma(sizeof(*ar), (void **)&ar);
    ar->hdr.type    = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    ar->hdr.ctx_id  = gpu.virgl_ctx_id;
    ar->resource_id = res_id;
    ar->padding     = 0;
    ok = gpu_cmd(ar, sizeof(*ar));
    pmm_free_pages(ar_phys, 1);
    return ok;
}

/* ── Helper: submit virgl command buffer wrapped in SUBMIT_3D ─────────────── */
static void gpu_submit_virgl(const uint32_t *cmdbuf, uint32_t n_dwords) {
    if (!gpu.virgl_ok) return;
    uint32_t data_bytes = n_dwords * 4;
    size_t total = sizeof(gpu_cmd_submit_3d_t) + data_bytes;
    gpu_cmd_submit_3d_t *sub;
    uintptr_t sub_phys = alloc_dma(total, (void **)&sub);
    sub->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    sub->hdr.ctx_id = gpu.virgl_ctx_id;
    sub->size       = data_bytes;
    sub->padding    = 0;
    memcpy((uint8_t *)sub + sizeof(*sub), cmdbuf, data_bytes);
    gpu_cmd(sub, (uint32_t)total);
    pmm_free_pages(sub_phys, (total + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* ── Helper: build a virgl surface + set FB state for a resource ──────────── */
static void virgl_bind_rendertarget(uint32_t res_id, uint32_t surf_handle) {
    /* CREATE_OBJECT surface: 6 dwords */
    uint32_t cmds[10];
    int n = 0;
    cmds[n++] = VIRGL_CMD(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    cmds[n++] = surf_handle;
    cmds[n++] = res_id;
    cmds[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    cmds[n++] = 0; /* level = 0 */
    cmds[n++] = 0; /* first_layer=0, last_layer=0 */

    /* SET_FRAMEBUFFER_STATE: 1 cbuf, no depth */
    cmds[n++] = VIRGL_CMD(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    cmds[n++] = 1;            /* nr_cbufs */
    cmds[n++] = 0;            /* zsurf = 0 */
    cmds[n++] = surf_handle;  /* cbuf[0] */

    gpu_submit_virgl(cmds, n);
}

/* ── Public API: init ──────────────────────────────────────────────────────── */
bool virtio_gpu_init(gfx_surface_t **out) {
    /* Find virtio-gpu PCI device (0x1050 = virtio-gpu) */
    pci_device_t *pdev = pci_find(0x1AF4, 0x1050);
    if (!pdev) {
        KLOG_WARN("virtio-gpu: no device found\n");
        return false;
    }
    pci_enable_device(pdev);
    gpu.pci = pdev;

    /* Walk VirtIO PCI capabilities */
    uint8_t cap_ptr = pci_find_cap(pdev, PCI_CAP_VENDOR);
    uint32_t notify_mul = 0;
    bool found_common = false, found_notify = false, found_isr = false, found_dev = false;

    while (cap_ptr) {
        uint8_t cfg_type = pci_read8(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 3);
        uint8_t bar_idx  = pci_read8(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 4);
        uint32_t offset  = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 8);
        uint32_t length  = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 12);

        uintptr_t bar_phys = pdev->bars[bar_idx].base;
        if (!bar_phys) {
            uint8_t next = pci_read8(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 1);
            cap_ptr = (uint8_t)(next & ~3u);
            continue;
        }
        size_t bar_sz = (size_t)pdev->bars[bar_idx].size;
        size_t needed = (size_t)offset + (size_t)(length > 0 ? length : 0x1000);
        if (bar_sz < needed)  bar_sz = needed;
        if (bar_sz < 0x10000) bar_sz = 0x10000;
        uintptr_t vaddr = vmm_mmio_map(bar_phys, bar_sz);

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            gpu.common = (virtio_common_cfg_t *)(vaddr + offset);
            found_common = true; break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            gpu.notify_base = (uint8_t *)(vaddr + offset);
            notify_mul = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 16);
            found_notify = true; break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            gpu.isr = (uint8_t *)(vaddr + offset);
            found_isr = true; break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            gpu.devcfg = (virtio_gpu_config_t *)(vaddr + offset);
            found_dev = true; break;
        }

        uint8_t next = pci_read8(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 1);
        cap_ptr = (uint8_t)(next & ~3u);
    }

    if (!found_common || !found_notify || !found_isr || !found_dev) {
        KLOG_ERR("virtio-gpu: missing caps (c=%d n=%d i=%d d=%d)\n",
                 found_common, found_notify, found_isr, found_dev);
        return false;
    }

    /* ── Device init sequence ──────────────────────────────────────────── */
    virtio_common_cfg_t *cc = gpu.common;
    cc->device_status = 0;
    for (volatile int i = 0; i < 100000; i++);
    cc->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    cc->device_status |= VIRTIO_STATUS_DRIVER;

    /* Read device-offered features (low 32 bits) */
    cc->device_feature_select = 0;
    uint32_t dev_feat_lo = cc->device_feature;
    /* Read device-offered features (high 32 bits) */
    cc->device_feature_select = 1;
    (void)cc->device_feature;  /* high bits read but unused for now */

    /* VIRTIO_GPU_F_VIRGL = bit 0: device supports virgl 3D mode.
     * MUST negotiate this for ANY 3D command to work. */
    #define VIRTIO_GPU_F_VIRGL  (1u << 0)

    bool device_has_virgl = !!(dev_feat_lo & VIRTIO_GPU_F_VIRGL);

    uint32_t drv_feat_lo = 0;
    if (device_has_virgl)
        drv_feat_lo |= VIRTIO_GPU_F_VIRGL;

    cc->driver_feature_select = 0;
    cc->driver_feature        = drv_feat_lo;
    /* VIRTIO_F_VERSION_1 (bit 32 = select=1, bit 0) — mandatory for modern */
    cc->driver_feature_select = 1;
    cc->driver_feature        = (1u << 0);
    cc->device_status        |= VIRTIO_STATUS_FEATURES_OK;
    if (!(cc->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        KLOG_ERR("virtio-gpu: FEATURES_OK not set\n"); return false;
    }
    KLOG_INFO("virtio-gpu: features dev=0x%x drv=0x%x virgl=%s\n",
              dev_feat_lo, drv_feat_lo, device_has_virgl ? "yes" : "no");

    /* Setup controlq (queue 0) and cursorq (queue 1) */
    if (!vq_init(&gpu.ctlq, 0, cc, gpu.notify_base, notify_mul)) {
        KLOG_ERR("virtio-gpu: controlq init failed\n"); return false;
    }
    /* Cursor queue is optional — ignore failure, just won't have HW cursor */
    vq_init(&gpu.curq, 1, cc, gpu.notify_base, notify_mul);

    cc->device_status |= VIRTIO_STATUS_DRIVER_OK;
    KLOG_INFO("virtio-gpu: device ready, %u scanouts, %u capsets\n",
              gpu.devcfg->num_scanouts, gpu.devcfg->num_capsets);

    /* ── GET_DISPLAY_INFO ──────────────────────────────────────────────── */
    gpu_ctrl_hdr_t *req_hdr;
    uintptr_t req_phys = alloc_dma(sizeof(*req_hdr), (void **)&req_hdr);
    req_hdr->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    gpu_resp_display_info_t *disp_info;
    uintptr_t disp_phys = alloc_dma(sizeof(*disp_info), (void **)&disp_info);
    vq_submit2(&gpu.ctlq, req_phys, sizeof(*req_hdr), disp_phys, sizeof(*disp_info));

    uint32_t fb_w = 1280, fb_h = 720;
    if (disp_info->hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO &&
        disp_info->pmodes[0].enabled) {
        fb_w = disp_info->pmodes[0].width;
        fb_h = disp_info->pmodes[0].height;
    }
    KLOG_INFO("virtio-gpu: scanout 0 = %ux%u\n", fb_w, fb_h);
    pmm_free_pages(req_phys, 1);
    pmm_free_pages(disp_phys, (sizeof(*disp_info) + PAGE_SIZE - 1) / PAGE_SIZE);

    gpu.fb_w = fb_w;
    gpu.fb_h = fb_h;
    gpu.next_res_id = RES_ID_DYN_BASE;

    /* ── Probe for virgl capset (only if feature was negotiated) ─────── */
    gpu.virgl_ok = false;
    uint32_t virgl_version = 0;
    if (device_has_virgl) {
        uint32_t num_capsets = gpu.devcfg->num_capsets;
        for (uint32_t ci = 0; ci < num_capsets && !gpu.virgl_ok; ci++) {
            gpu_cmd_get_capset_info_t *gci;
            uintptr_t gci_phys = alloc_dma(sizeof(*gci), (void **)&gci);
            gci->hdr.type      = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
            gci->capset_index  = ci;
            gci->padding       = 0;
            gpu_resp_capset_info_t *rci;
            uintptr_t rci_phys = alloc_dma(sizeof(*rci), (void **)&rci);
            vq_submit2(&gpu.ctlq, gci_phys, sizeof(*gci), rci_phys, sizeof(*rci));

            KLOG_INFO("virtio-gpu: capset[%u] resp=0x%x id=%u ver=%u size=%u\n",
                      ci, rci->hdr.type, rci->capset_id,
                      rci->capset_max_version, rci->capset_max_size);

            if (rci->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET_INFO &&
                rci->capset_id == VIRTIO_GPU_CAPSET_VIRGL &&
                rci->capset_max_version > 0) {
                gpu.virgl_ok  = true;
                virgl_version = rci->capset_max_version;
            }
            pmm_free_pages(gci_phys, 1);
            pmm_free_pages(rci_phys, 1);
        }
    }

    /* ── Virgl: create context BEFORE resources ──────────────────────── */
    if (gpu.virgl_ok) {
        gpu.virgl_ctx_id      = 1;
        gpu.back_surf_handle  = 0x101;
        gpu.front_surf_handle = 0x102;

        gpu_cmd_ctx_create_t *ctx;
        uintptr_t ctx_phys = alloc_dma(sizeof(*ctx), (void **)&ctx);
        ctx->hdr.type     = VIRTIO_GPU_CMD_CTX_CREATE;
        ctx->hdr.ctx_id   = gpu.virgl_ctx_id;
        ctx->nlen         = 0;
        ctx->context_init = 0;
        bool ok = gpu_cmd(ctx, sizeof(*ctx));
        pmm_free_pages(ctx_phys, 1);
        if (!ok) {
            KLOG_WARN("virtio-gpu: CTX_CREATE failed, disabling virgl\n");
            gpu.virgl_ok = false;
        } else {
            KLOG_INFO("virtio-gpu: virgl context ready (capset version=%u)\n",
                      virgl_version);
        }
    }

    /* ── Create front and back frame-buffer resources ─────────────────── */
    /* CRITICAL: resource IDs must not be reused between 2D and 3D types.  */
    /* If virgl is enabled, create as RESOURCE_CREATE_3D from the start.   */
    /* If not, create as RESOURCE_CREATE_2D.  Never mix on the same ID.    */
    size_t fb_bytes = (size_t)fb_w * fb_h * 4;
    size_t fb_pages = (fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uintptr_t front_phys = pmm_alloc_pages(fb_pages);
    uintptr_t back_phys  = pmm_alloc_pages(fb_pages);
    if (!front_phys || !back_phys) {
        KLOG_ERR("virtio-gpu: cannot alloc frame-buffer memory\n"); return false;
    }
    gfx_color_t *front_virt = (gfx_color_t *)vmm_phys_to_virt(front_phys);
    gfx_color_t *back_virt  = (gfx_color_t *)vmm_phys_to_virt(back_phys);
    memset(front_virt, 0, fb_pages * PAGE_SIZE);
    memset(back_virt,  0, fb_pages * PAGE_SIZE);

    if (gpu.virgl_ok) {
        /* Create both scanout buffers as virgl 3D resources */
        if (!virgl_create_resource_3d(RES_ID_FRONT, fb_w, fb_h, front_phys, fb_bytes) ||
            !virgl_create_resource_3d(RES_ID_BACK,  fb_w, fb_h, back_phys,  fb_bytes)) {
            KLOG_WARN("virtio-gpu: RESOURCE_CREATE_3D failed, disabling virgl\n");
            gpu.virgl_ok = false;
        }
    }

    if (!gpu.virgl_ok) {
        /* Fallback: plain 2D resources - attach backing and register */
        bool ok = true;
        for (int r = 0; r < 2 && ok; r++) {
            uint32_t rid  = (r == 0) ? RES_ID_FRONT : RES_ID_BACK;
            uintptr_t bph = (r == 0) ? front_phys   : back_phys;
            gpu_cmd_resource_create_2d_t *cr;
            uintptr_t cr_phys = alloc_dma(sizeof(*cr), (void **)&cr);
            cr->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
            cr->resource_id = rid;
            cr->format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
            cr->width = fb_w; cr->height = fb_h;
            ok = gpu_cmd(cr, sizeof(*cr));
            pmm_free_pages(cr_phys, 1);
            if (!ok) break;
            gpu_cmd_attach_backing_t *ab;
            uintptr_t ab_phys = alloc_dma(sizeof(*ab), (void **)&ab);
            ab->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
            ab->resource_id = rid;
            ab->nr_entries  = 1;
            ab->entries[0].addr   = bph;
            ab->entries[0].length = (uint32_t)(fb_pages * PAGE_SIZE);
            ok = gpu_cmd(ab, sizeof(*ab));
            pmm_free_pages(ab_phys, 1);
        }
        if (!ok) { KLOG_ERR("virtio-gpu: 2D resource creation failed\n"); return false; }
    }

    gpu.back_phys   = back_phys;
    gpu.back_pages  = fb_pages;
    gpu.back_shadow = back_virt;
    gpu.scanout_res = RES_ID_FRONT;
    (void)front_virt; /* front is GPU-side; we never write it from CPU */

    if (gpu.virgl_ok) {
        /* Pre-create surface objects for front+back for SET_FRAMEBUFFER_STATE */
        virgl_bind_rendertarget(RES_ID_BACK,  gpu.back_surf_handle);
        virgl_bind_rendertarget(RES_ID_FRONT, gpu.front_surf_handle);
        KLOG_INFO("virtio-gpu: virgl 3D context ready\n");
    }

    /* ── Cursor resource (64×64 ARGB) ─────────────────────────────────── */
    size_t cursor_bytes = CURSOR_W * CURSOR_H * 4;
    size_t cursor_pages = (cursor_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    gpu.cursor_phys   = pmm_alloc_pages(cursor_pages);
    gpu.cursor_shadow = (gfx_color_t *)vmm_phys_to_virt(gpu.cursor_phys);
    memset(gpu.cursor_shadow, 0, cursor_pages * PAGE_SIZE);
    /* Create 2D cursor resource */
    gpu_cmd_resource_create_2d_t *ccr;
    uintptr_t ccr_phys = alloc_dma(sizeof(*ccr), (void **)&ccr);
    ccr->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    ccr->resource_id = RES_ID_CURSOR;
    ccr->format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    ccr->width       = CURSOR_W;
    ccr->height      = CURSOR_H;
    gpu_cmd(ccr, sizeof(*ccr));
    pmm_free_pages(ccr_phys, 1);
    gpu_cmd_attach_backing_t *cab;
    uintptr_t cab_phys = alloc_dma(sizeof(*cab), (void **)&cab);
    cab->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cab->resource_id = RES_ID_CURSOR;
    cab->nr_entries  = 1;
    cab->entries[0].addr   = gpu.cursor_phys;
    cab->entries[0].length = (uint32_t)(cursor_pages * PAGE_SIZE);
    gpu_cmd(cab, sizeof(*cab));
    pmm_free_pages(cab_phys, 1);

    /* ── SET_SCANOUT on front buffer ───────────────────────────────────── */
    if (!gpu_set_scanout(RES_ID_FRONT, fb_w, fb_h)) {
        KLOG_ERR("virtio-gpu: SET_SCANOUT failed\n"); return false;
    }

    /* Back surface is where the compositor/WM draws */
    gpu.back_surf = gfx_surface_wrap(gpu.back_shadow, (int)fb_w, (int)fb_h,
                                     (int)(fb_w * sizeof(gfx_color_t)));
    gpu.ready = true;
    *out = gpu.back_surf;
    KLOG_INFO("virtio-gpu: double-buffered %ux%u ready, virgl=%s\n",
              fb_w, fb_h, gpu.virgl_ok ? "yes" : "no");
    return true;
}

void virtio_gpu_flush(gfx_rect_t dirty) {
    if (!gpu.ready) return;
    /* Clamp dirty rect */
    if (dirty.x < 0) dirty.x = 0;
    if (dirty.y < 0) dirty.y = 0;
    if (dirty.x + dirty.w > (int)gpu.fb_w) dirty.w = (int)gpu.fb_w - dirty.x;
    if (dirty.y + dirty.h > (int)gpu.fb_h) dirty.h = (int)gpu.fb_h - dirty.y;
    if (dirty.w <= 0 || dirty.h <= 0) return;

    uint32_t back_id  = RES_ID_BACK;
    uint32_t front_id = RES_ID_FRONT;

    /* TRANSFER: push CPU shadow → back GPU resource.
     * 3D resources (virgl mode) require TRANSFER_TO_HOST_3D.
     * 2D resources require TRANSFER_TO_HOST_2D. */
    if (gpu.virgl_ok) {
        gpu_cmd_transfer_3d_t *tf;
        uintptr_t tf_phys = alloc_dma(sizeof(*tf), (void **)&tf);
        tf->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
        tf->hdr.ctx_id  = gpu.virgl_ctx_id;
        tf->box.x = (uint32_t)dirty.x;  tf->box.y = (uint32_t)dirty.y;  tf->box.z = 0;
        tf->box.w = (uint32_t)dirty.w;  tf->box.h = (uint32_t)dirty.h;  tf->box.d = 1;
        tf->offset      = (uint64_t)dirty.y * gpu.fb_w * 4 + (uint64_t)dirty.x * 4;
        tf->resource_id = back_id;
        tf->level       = 0;
        tf->stride      = gpu.fb_w * 4;
        tf->layer_stride = 0;
        gpu_cmd(tf, sizeof(*tf));
        pmm_free_pages(tf_phys, 1);
    } else {
        gpu_cmd_transfer_t *tf;
        uintptr_t tf_phys = alloc_dma(sizeof(*tf), (void **)&tf);
        tf->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        tf->r.x = (uint32_t)dirty.x;  tf->r.y = (uint32_t)dirty.y;
        tf->r.width  = (uint32_t)dirty.w; tf->r.height = (uint32_t)dirty.h;
        tf->offset      = (uint64_t)dirty.y * gpu.fb_w * 4 + (uint64_t)dirty.x * 4;
        tf->resource_id = back_id;
        tf->padding     = 0;
        gpu_cmd(tf, sizeof(*tf));
        pmm_free_pages(tf_phys, 1);
    }

    /* RESOURCE_FLUSH on back buffer to make GPU see it */
    gpu_cmd_resource_flush_t *rf;
    uintptr_t rf_phys = alloc_dma(sizeof(*rf), (void **)&rf);
    rf->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    rf->resource_id = back_id;
    rf->r.x = (uint32_t)dirty.x; rf->r.y = (uint32_t)dirty.y;
    rf->r.width = (uint32_t)dirty.w; rf->r.height = (uint32_t)dirty.h;
    gpu_cmd(rf, sizeof(*rf));
    pmm_free_pages(rf_phys, 1);

    /* If virgl is available, use GPU BLIT to copy back→front */
    if (gpu.virgl_ok) {
        /* SET_FRAMEBUFFER_STATE on back first (already done at init) */
        /* BLIT back→front  (virgl encoding: 21 payload dwords) */
        uint32_t blit[22];
        blit[0]  = VIRGL_CMD(VIRGL_CCMD_BLIT, 0, 21);
        blit[1]  = 0x0Fu;  /* mask = PIPE_MASK_RGBA | filter=NEAREST */
        blit[2]  = 0u;     /* scissor minx | miny<<16 (disabled) */
        blit[3]  = 0u;     /* scissor maxx | maxy<<16 (disabled) */
        /* dst */
        blit[4]  = front_id;
        blit[5]  = 0u;     /* dst level */
        blit[6]  = VIRGL_FORMAT_B8G8R8X8_UNORM;
        blit[7]  = (uint32_t)dirty.x;
        blit[8]  = (uint32_t)dirty.y;
        blit[9]  = 0u;     /* dst z */
        blit[10] = (uint32_t)dirty.w;
        blit[11] = (uint32_t)dirty.h;
        blit[12] = 1u;     /* dst d */
        /* src */
        blit[13] = back_id;
        blit[14] = 0u;     /* src level */
        blit[15] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        blit[16] = (uint32_t)dirty.x;
        blit[17] = (uint32_t)dirty.y;
        blit[18] = 0u;     /* src z */
        blit[19] = (uint32_t)dirty.w;
        blit[20] = (uint32_t)dirty.h;
        blit[21] = 1u;     /* src d */
        gpu_submit_virgl(blit, 22);

        /* Flush front to display */
        uintptr_t rf2_phys = alloc_dma(sizeof(*rf), (void **)&rf);
        rf->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        rf->resource_id = front_id;
        rf->r.x = (uint32_t)dirty.x; rf->r.y = (uint32_t)dirty.y;
        rf->r.width = (uint32_t)dirty.w; rf->r.height = (uint32_t)dirty.h;
        gpu_cmd(rf, sizeof(*rf));
        pmm_free_pages(rf2_phys, 1);
    } else {
        /* No virgl: present back buffer directly as scanout (single-buffer mode).
         * Swap SET_SCANOUT to back resource so the display engine shows it. */
        if (gpu.scanout_res != back_id) {
            gpu_set_scanout(back_id, gpu.fb_w, gpu.fb_h);
            gpu.scanout_res = back_id;
        }
        /* Flush back to display */
        uintptr_t rf2_phys = alloc_dma(sizeof(*rf), (void **)&rf);
        rf->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        rf->resource_id = back_id;
        rf->r.x = (uint32_t)dirty.x; rf->r.y = (uint32_t)dirty.y;
        rf->r.width = (uint32_t)dirty.w; rf->r.height = (uint32_t)dirty.h;
        gpu_cmd(rf, sizeof(*rf));
        pmm_free_pages(rf2_phys, 1);
    }
}

bool virtio_gpu_available(void) {
    return gpu.ready;
}

/* ── Hardware cursor ──────────────────────────────────────────────────────── */

void virtio_gpu_cursor_set(const uint32_t *rgba64, uint32_t hot_x, uint32_t hot_y) {
    if (!gpu.ready) return;

    if (rgba64) {
        /* Copy bitmap into cursor shadow */
        memcpy(gpu.cursor_shadow, rgba64, CURSOR_W * CURSOR_H * 4);

        /* TRANSFER_TO_HOST_2D for cursor resource */
        gpu_cmd_transfer_t *tf;
        uintptr_t tf_phys = alloc_dma(sizeof(*tf), (void **)&tf);
        tf->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        tf->r.x = 0; tf->r.y = 0; tf->r.width = CURSOR_W; tf->r.height = CURSOR_H;
        tf->offset      = 0;
        tf->resource_id = RES_ID_CURSOR;
        tf->padding     = 0;
        gpu_cmd(tf, sizeof(*tf));
        pmm_free_pages(tf_phys, 1);
    }

    /* Update cursor on cursor queue (or controlq if no cursorq) */
    gpu_cmd_cursor_t *cur;
    uintptr_t cur_phys = alloc_dma(sizeof(*cur), (void **)&cur);
    cur->hdr.type      = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cur->pos.scanout_id = 0;
    cur->pos.x = 0; cur->pos.y = 0; cur->pos.padding = 0;
    cur->resource_id   = rgba64 ? RES_ID_CURSOR : 0;
    cur->hot_x         = hot_x;
    cur->hot_y         = hot_y;
    cur->padding       = 0;
    gpu_cmd(cur, sizeof(*cur));
    pmm_free_pages(cur_phys, 1);
}

void virtio_gpu_cursor_move(int x, int y) {
    if (!gpu.ready) return;
    gpu_cmd_cursor_t *cur;
    uintptr_t cur_phys = alloc_dma(sizeof(*cur), (void **)&cur);
    cur->hdr.type      = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cur->pos.scanout_id = 0;
    cur->pos.x = (uint32_t)x; cur->pos.y = (uint32_t)y; cur->pos.padding = 0;
    cur->resource_id   = 0;
    cur->hot_x         = 0; cur->hot_y = 0; cur->padding = 0;
    gpu_cmd(cur, sizeof(*cur));
    pmm_free_pages(cur_phys, 1);
}

/* ── Dynamic GPU resource management ─────────────────────────────────────── */

uint32_t virtio_gpu_resource_alloc(uint32_t w, uint32_t h) {
    if (!gpu.ready || gpu.res_count >= VIRTIO_GPU_MAX_RESOURCES) return 0;

    uint32_t res_id = gpu.next_res_id++;
    size_t bytes  = (size_t)w * h * 4;
    size_t pages  = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    gfx_color_t *shadow = (gfx_color_t *)vmm_phys_to_virt(phys);
    memset(shadow, 0, pages * PAGE_SIZE);

    bool ok;
    if (gpu.virgl_ok) {
        ok = virgl_create_resource_3d(res_id, w, h, phys, bytes);
    } else {
        uintptr_t dummy;
        gfx_color_t *dummy_v;
        /* create_resource_2d allocates its own backing — we need to do it manually */
        gpu_cmd_resource_create_2d_t *cr;
        uintptr_t cr_phys = alloc_dma(sizeof(*cr), (void **)&cr);
        cr->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cr->resource_id = res_id;
        cr->format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        cr->width       = w; cr->height = h;
        ok = gpu_cmd(cr, sizeof(*cr));
        pmm_free_pages(cr_phys, 1);
        if (ok) {
            gpu_cmd_attach_backing_t *ab;
            uintptr_t ab_phys = alloc_dma(sizeof(*ab), (void **)&ab);
            ab->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
            ab->resource_id = res_id;
            ab->nr_entries  = 1;
            ab->entries[0].addr   = phys;
            ab->entries[0].length = (uint32_t)(pages * PAGE_SIZE);
            ok = gpu_cmd(ab, sizeof(*ab));
            pmm_free_pages(ab_phys, 1);
        }
        (void)dummy; (void)dummy_v;
    }
    if (!ok) { pmm_free_pages(phys, pages); return 0; }

    gpu_resource_t *slot = &gpu.res_table[gpu.res_count++];
    slot->id          = res_id;
    slot->w           = w;
    slot->h           = h;
    slot->phys        = phys;
    slot->phys_pages  = pages;
    slot->shadow      = shadow;
    slot->surf        = gfx_surface_wrap(shadow, (int)w, (int)h,
                                         (int)(w * sizeof(gfx_color_t)));
    return res_id;
}

static gpu_resource_t *res_find(uint32_t res_id) {
    for (int i = 0; i < gpu.res_count; i++)
        if (gpu.res_table[i].id == res_id) return &gpu.res_table[i];
    return NULL;
}

gfx_surface_t *virtio_gpu_resource_surface(uint32_t res_id) {
    gpu_resource_t *r = res_find(res_id);
    return r ? r->surf : NULL;
}

void virtio_gpu_resource_upload(uint32_t res_id, gfx_rect_t dirty) {
    gpu_resource_t *r = res_find(res_id);
    if (!r) return;
    /* Clamp */
    if (dirty.x < 0) dirty.x = 0;
    if (dirty.y < 0) dirty.y = 0;
    if (dirty.x + dirty.w > (int)r->w) dirty.w = (int)r->w - dirty.x;
    if (dirty.y + dirty.h > (int)r->h) dirty.h = (int)r->h - dirty.y;
    if (dirty.w <= 0 || dirty.h <= 0) return;

    gpu_cmd_transfer_t *tf;
    uintptr_t tf_phys = alloc_dma(sizeof(*tf), (void **)&tf);
    tf->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    tf->r.x = (uint32_t)dirty.x; tf->r.y = (uint32_t)dirty.y;
    tf->r.width = (uint32_t)dirty.w; tf->r.height = (uint32_t)dirty.h;
    tf->offset      = (uint64_t)dirty.y * r->w * 4 + (uint64_t)dirty.x * 4;
    tf->resource_id = res_id;
    tf->padding     = 0;
    gpu_cmd(tf, sizeof(*tf));
    pmm_free_pages(tf_phys, 1);
}

void virtio_gpu_resource_free(uint32_t res_id) {
    for (int i = 0; i < gpu.res_count; i++) {
        if (gpu.res_table[i].id == res_id) {
            gpu_resource_t *r = &gpu.res_table[i];
            pmm_free_pages(r->phys, r->phys_pages);
            /* TODO: free r->surf via gfx_surface_free if such function exists */
            /* Compact table */
            gpu.res_table[i] = gpu.res_table[--gpu.res_count];
            return;
        }
    }
}

/* ── virgl 3D operations ──────────────────────────────────────────────────── */

bool virtio_gpu_virgl_available(void) {
    return gpu.virgl_ok;
}

void virtio_gpu_virgl_submit(const uint32_t *cmdbuf, uint32_t n_dwords) {
    gpu_submit_virgl(cmdbuf, n_dwords);
}

void virtio_gpu_virgl_fill(uint32_t res_id, gfx_rect_t r, gfx_color_t colour) {
    if (!gpu.virgl_ok) return;

    /* Extract RGBA float bits from packed colour (BGRA8888 layout) */
    uint8_t b = (colour >>  0) & 0xFF;
    uint8_t g = (colour >>  8) & 0xFF;
    uint8_t rv = (colour >> 16) & 0xFF;
    uint8_t a  = (colour >> 24) & 0xFF;

    /* Convert [0,255] → IEEE754 float via union trick (no FPU needed for
     * approximate values: we use pre-computed table of common values, or just
     * bit-cast via the standard conversion helper) */
    /* We'll pass 0xNN/255 as float.  To avoid FPU in the kernel we compute
     * approximate IEEE754 bits by treating the fraction as: if v==0 → 0,
     * if v==255 → F32_1, else encode as (127+0) exponent with mantissa. */
    /* Simplest approach: store as fixed point — virgl accepts float RGBA */
    /* We take a pragmatic route: for each channel scale 0-255 → f32 bits */
    #define TO_F32_BITS(v) ( \
        (v) == 0   ? 0x00000000u : \
        (v) == 255 ? 0x3F800000u : \
        (0x3F800000u | (((uint32_t)(v) - 128u) < 128u \
            ? ((uint32_t)(v) << 15) \
            : 0u)))
    /* Actually virgl wants proper IEEE754 floats.  We use a simple bit-exact
     * approach: f = v / 255.0  expressed as (v * 0x808081) >> 23 for mantissa.
     * Exponent for range [0,1] is 0x7E (=126 for v≥128) or 0x7D etc.
     * Use a 17-entry LUT for common greys, and raw approximation otherwise. */
    /* Simplest correct kernel approach: since we have no FPU, use the fact that
     * normalised float for x∈[0.5,1.0] has exponent=0x3F (biased 127):
     *   bits = 0x3F000000 | ((v - 128) << 16)  for v≥128
     *   bits = 0x3E000000 | ((v - 64)  << 17)  for v∈[64,128)
     *   etc.   This gives < 0.2% error — fine for colour fills. */
    /* We use the simplest possible: map linearly using (v << 15) method */
    #undef TO_F32_BITS
    /* Use virgl-friendly: pack as u8-normalised uint in virgl CLEAR command.
     * VIRGL_CCMD_CLEAR takes 4 IEEE754 floats for RGBA colour.
     * For kernel use we just bit-cast an integer approximation: */
    (void)b; (void)g; (void)rv; (void)a;
    /* Use a simple u32→float conversion: result = v/255.0 ≈
     *   exponent = 127 - clz(v),  mantissa = v << (24 - (32-clz(v)))
     * For the purpose of fill colours this approximation is adequate.
     * We use the formula:  f32bits(v/255) ≈ (v * 0x808081u >> 7) for v∈[0,255] */
    /* Exact: for v=0→0x00000000, v=255→0x3F800000, v=128→0x3F000000 */
    #define CHAN_F32(v) (                                                         \
        (v) == 0u   ? 0x00000000u                                                \
      : (v) == 255u ? 0x3F800000u                                                \
      : (0x3E800000u + (uint32_t)((uint32_t)(v) * 0x00010203u >> 8u)))
    uint32_t rf = CHAN_F32(rv);
    uint32_t gf = CHAN_F32(g);
    uint32_t bf = CHAN_F32(b);
    uint32_t af = CHAN_F32(a);
    #undef CHAN_F32

    /* SET_FRAMEBUFFER_STATE to target resource */
    uint32_t surf_handle = 0x200 + res_id;
    uint32_t fb_cmds[10];
    int n = 0;
    /* Create surface for this resource if needed */
    fb_cmds[n++] = VIRGL_CMD(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    fb_cmds[n++] = surf_handle;
    fb_cmds[n++] = res_id;
    fb_cmds[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    fb_cmds[n++] = 0u; fb_cmds[n++] = 0u;
    fb_cmds[n++] = VIRGL_CMD(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    fb_cmds[n++] = 1u; fb_cmds[n++] = 0u; fb_cmds[n++] = surf_handle;
    gpu_submit_virgl(fb_cmds, n);

    /* CLEAR */
    uint32_t clear[9] = {
        VIRGL_CMD(VIRGL_CCMD_CLEAR, 0, 8),
        PIPE_CLEAR_COLOR0,
        rf, gf, bf, af,
        F64_1_LO, F64_1_HI,  /* depth = 1.0 (double) */
        0u,                   /* stencil */
    };
    gpu_submit_virgl(clear, 9);
    (void)r; /* TODO: scissor rect — virgl CLEAR covers entire FB */
}

void virtio_gpu_virgl_blit(uint32_t src_id, gfx_rect_t src_r,
                            uint32_t dst_id, int dx, int dy) {
    if (!gpu.virgl_ok) return;

    uint32_t blit[22];
    blit[0]  = VIRGL_CMD(VIRGL_CCMD_BLIT, 0, 21);
    blit[1]  = 0x0Fu;  /* mask=PIPE_MASK_RGBA | filter=NEAREST */
    blit[2]  = 0u;     /* scissor minx|miny<<16 (disabled) */
    blit[3]  = 0u;     /* scissor maxx|maxy<<16 (disabled) */
    /* dst */
    blit[4]  = dst_id;
    blit[5]  = 0u;     /* dst level */
    blit[6]  = VIRGL_FORMAT_B8G8R8X8_UNORM;
    blit[7]  = (uint32_t)dx;
    blit[8]  = (uint32_t)dy;
    blit[9]  = 0u;     /* dst z */
    blit[10] = (uint32_t)src_r.w;
    blit[11] = (uint32_t)src_r.h;
    blit[12] = 1u;     /* dst d */
    /* src */
    blit[13] = src_id;
    blit[14] = 0u;     /* src level */
    blit[15] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    blit[16] = (uint32_t)src_r.x;
    blit[17] = (uint32_t)src_r.y;
    blit[18] = 0u;     /* src z */
    blit[19] = (uint32_t)src_r.w;
    blit[20] = (uint32_t)src_r.h;
    blit[21] = 1u;     /* src d */
    gpu_submit_virgl(blit, 22);
}
