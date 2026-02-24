/* drivers/storage/blkdev.h — Block device abstraction layer
 *
 * All storage drivers register a blkdev_t into the global registry.
 * Higher layers (GPT parser, VFS filesystems) call blkdev_read/write
 * which route through the buffer cache before hitting the driver ops.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Block device ops vtable ─────────────────────────────────────────────── */
typedef struct blkdev blkdev_t;

typedef struct blkdev_ops {
    /* Read `count` 512-byte sectors starting at `lba` into `buf`.
     * Returns 0 on success, -1 on error. */
    int (*read_blocks)(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf);

    /* Write `count` 512-byte sectors from `buf` starting at `lba`.
     * Returns 0 on success, -1 on error. */
    int (*write_blocks)(blkdev_t *dev, uint64_t lba, uint32_t count,
                        const void *buf);

    /* Return total number of 512-byte sectors (0 if unknown). */
    uint64_t (*get_block_count)(blkdev_t *dev);

    /* Return sector size in bytes (almost always 512). */
    uint32_t (*get_block_size)(blkdev_t *dev);

    /* Flush any hardware write buffers. Returns 0 on success. */
    int (*flush)(blkdev_t *dev);
} blkdev_ops_t;

/* ── Block device descriptor ─────────────────────────────────────────────── */
struct blkdev {
    char          name[32];    /* e.g. "vda", "sda", "vda1", "sda2"           */
    uint64_t      dev_id;      /* unique ID assigned by blkdev_register()      */
    blkdev_ops_t *ops;         /* driver vtable                                */
    void         *priv;        /* driver-private data                          */

    /* Cached geometry (filled by blkdev_register from ops queries) */
    uint64_t      block_count; /* total sectors                                */
    uint32_t      block_size;  /* sector size (bytes)                          */

    /* Partition offset (0 for whole-disk devices) */
    uint64_t      part_offset; /* LBA offset for partition wrappers            */
};

/* ── Registry ────────────────────────────────────────────────────────────── */
#define BLKDEV_MAX 32

/* Register a block device; assigns dev_id. Returns 0 on success, -1 if full. */
int blkdev_register(blkdev_t *dev);

/* Look up by numeric ID (as assigned by blkdev_register). Returns NULL. */
blkdev_t *blkdev_get(uint64_t dev_id);

/* Look up by name (e.g. "vda", "sda1"). Returns NULL if not found. */
blkdev_t *blkdev_find_by_name(const char *name);

/* Return total number of registered devices. */
int blkdev_count(void);

/* Iterate: get the n-th registered device (0-based). Returns NULL if oob. */
blkdev_t *blkdev_get_nth(int n);

/* ── Convenience I/O (bypasses bcache — use bcache_get instead normally) ─── */

/* Direct read: `count` sectors starting at `lba` into `buf`.
 * Returns 0 on success, -1 on error. */
int blkdev_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf);

/* Direct write: `count` sectors from `buf` starting at `lba`.
 * Returns 0 on success, -1 on error. */
int blkdev_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                 const void *buf);

/* Flush device write buffers. Returns 0 on success, -1 on error. */
int blkdev_flush(blkdev_t *dev);

/* ── Partition wrapper ───────────────────────────────────────────────────── */
/* Allocate a partition blkdev that wraps `parent` with an LBA offset.
 * The resulting device is registered automatically.
 * `name` — device name (e.g. "vda1").
 * `lba_start` / `lba_end` — inclusive partition boundary from GPT/MBR.
 * Returns the new blkdev_t* or NULL on failure. */
blkdev_t *blkdev_partition_create(blkdev_t *parent, const char *name,
                                  uint64_t lba_start, uint64_t lba_end);
