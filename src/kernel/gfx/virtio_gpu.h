/* gfx/virtio_gpu.h — VirtIO GPU v1 driver: double-buffering, HW cursor, virgl 3D */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gfx/gfx.h"

/* ── Initialisation ──────────────────────────────────────────────────────── */

/* Bring up virtio-gpu.  On success *out is the back-buffer surface.
 * Creates front+back GPU resources for tear-free double-buffering.      */
bool virtio_gpu_init(gfx_surface_t **out);
bool virtio_gpu_available(void);

/* ── Frame presentation (double-buffered) ────────────────────────────────── */

/* Sync dirty rect CPU→GPU then flip front↔back atomically (no tearing). */
void virtio_gpu_flush(gfx_rect_t dirty);

/* ── Hardware cursor (GPU composites independently – zero CPU cost) ───────── */

/* Upload a 64×64 ARGB8888 cursor bitmap and set the hotspot.
 * Pass NULL to remove the cursor.  Runs entirely on the display engine. */
void virtio_gpu_cursor_set(const uint32_t *rgba64, uint32_t hot_x, uint32_t hot_y);

/* Reposition hardware cursor — no pixel transfer, instant. */
void virtio_gpu_cursor_move(int x, int y);

/* ── GPU resource management (for virgl-accelerated compositing) ─────────── */

/* Allocate a GPU-side 3D resource backed by CPU-writable shadow RAM.
 * Returns resource_id > 0 on success, 0 on failure. */
uint32_t virtio_gpu_resource_alloc(uint32_t w, uint32_t h);

/* Get the CPU surface for a resource (draw window pixels here). */
gfx_surface_t *virtio_gpu_resource_surface(uint32_t res_id);

/* Push a dirty rect from CPU shadow RAM → GPU resource. */
void virtio_gpu_resource_upload(uint32_t res_id, gfx_rect_t dirty);

/* Free a resource and its backing memory. */
void virtio_gpu_resource_free(uint32_t res_id);

/* ── virgl 3D ─────────────────────────────────────────────────────────────── */

/* True when virgl is supported and a 3D context was successfully created. */
bool virtio_gpu_virgl_available(void);

/* GPU-side solid fill of a resource region (no CPU touches the pixels). */
void virtio_gpu_virgl_fill(uint32_t res_id, gfx_rect_t r, gfx_color_t colour);

/* GPU-side blit/composite: copy src rect onto dst resource at (dx, dy).
 * Used by the compositor — replaces CPU memcpy + full-screen TRANSFER. */
void virtio_gpu_virgl_blit(uint32_t src_id, gfx_rect_t src_r,
                            uint32_t dst_id, int dx, int dy);

/* Submit a raw virgl command buffer (n_dwords uint32 words). */
void virtio_gpu_virgl_submit(const uint32_t *cmdbuf, uint32_t n_dwords);
